/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Sapphire <imsapphire0@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "fb_body_tracker.h"
#include "spdlog/spdlog.h"
#include "utils/overloaded.h"
#include "wivrn_packets.h"
#include "xr/instance.h"
#include "xr/session.h"
#include "xr/to_string.h"
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <variant>
#include <openxr/openxr.h>

xr::fb_body_tracker::fb_body_tracker(instance & inst, session & s, bool full_body) :
        handle(inst.get_proc<PFN_xrDestroyBodyTrackerFB>("xrDestroyBodyTrackerFB")),
        xrSuggestBodyTrackingCalibrationOverrideMETA(inst.has_extension(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME)
                                                             ? inst.get_proc<PFN_xrSuggestBodyTrackingCalibrationOverrideMETA>(
                                                                       "xrSuggestBodyTrackingCalibrationOverrideMETA")
                                                             : nullptr),
        xrResetBodyTrackingCalibrationMETA(inst.has_extension(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME)
                                                   ? inst.get_proc<PFN_xrResetBodyTrackingCalibrationMETA>("xrResetBodyTrackingCalibrationMETA")
                                                   : nullptr),
        xrRequestBodyTrackingFidelityMETA(inst.has_extension(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)
                                                  ? inst.get_proc<PFN_xrRequestBodyTrackingFidelityMETA>("xrRequestBodyTrackingFidelityMETA")
                                                  : nullptr),
        xrLocateBodyJointsFB(inst.get_proc<PFN_xrLocateBodyJointsFB>("xrLocateBodyJointsFB")),
        xrGetBodySkeletonFB(inst.get_proc<PFN_xrGetBodySkeletonFB>("xrGetBodySkeletonFB")),
        calibration_extension_enabled_(inst.has_extension(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME)),
        fidelity_extension_enabled_(inst.has_extension(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)),
        joint_set(full_body ? XR_BODY_JOINT_SET_FULL_BODY_META : XR_BODY_JOINT_SET_DEFAULT_FB)
{
	auto xrCreateBodyTrackerFB = inst.get_proc<PFN_xrCreateBodyTrackerFB>("xrCreateBodyTrackerFB");

	XrBodyTrackerCreateInfoFB create_info{
	        .type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB,
	        .bodyJointSet = joint_set,
	};

	CHECK_XR(xrCreateBodyTrackerFB(s, &create_info, &id));

	// Enable IOBT.  High fidelity is the historical default; a runtime that
	// advertises the extension but does not implement it is handled as an
	// optional capability by request_fidelity().
	if (fidelity_extension_enabled_)
		request_fidelity(XR_BODY_TRACKING_FIDELITY_HIGH_META);
}

xr::fb_body_tracker::packet_type xr::fb_body_tracker::locate_spaces(XrTime time, XrSpace reference)
{
	xr::fb_body_tracker::packet_type ret{};

	assert(xrLocateBodyJointsFB);

	XrBodyJointsLocateInfoFB locate_info{
	        .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB,
	        .next = nullptr,
	        .baseSpace = reference,
	        .time = time,
	};

	std::array<XrBodyJointLocationFB, XR_FULL_BODY_JOINT_COUNT_META> joints{};
	XrBodyJointLocationsFB joint_locations{
	        .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB,
	        .next = nullptr,
	        .jointCount = (uint32_t)(joint_set == XR_BODY_JOINT_SET_FULL_BODY_META
	                                         ? std::to_underlying(XR_FULL_BODY_JOINT_COUNT_META)
	                                         : std::to_underlying(XR_BODY_JOINT_COUNT_FB)),
	        .jointLocations = joints.data(),
	};

	XrBodyTrackingCalibrationStatusMETA calibration_status{
	        .type = XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META,
	        .next = nullptr,
	        .status = XR_BODY_TRACKING_CALIBRATION_STATE_INVALID_META,
	};
	XrBodyTrackingFidelityStatusMETA fidelity_status{
	        .type = XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META,
	        .next = nullptr,
	        .fidelity = XR_BODY_TRACKING_FIDELITY_HIGH_META,
	};

	// Both META status structures extend XrBodyJointLocationsFB.  Build the
	// chain in reverse order so each optional structure is linked only while
	// its extension is enabled.
	void * extension_chain = nullptr;
	if (fidelity_extension_enabled_)
	{
		fidelity_status.next = extension_chain;
		extension_chain = &fidelity_status;
	}
	if (calibration_extension_enabled_)
	{
		calibration_status.next = extension_chain;
		extension_chain = &calibration_status;
	}
	joint_locations.next = extension_chain;

	auto locate = [&] {
		return xrLocateBodyJointsFB(id, &locate_info, &joint_locations);
	};
	XrResult res = locate();
	if (res == XR_ERROR_FEATURE_UNSUPPORTED and extension_chain)
	{
		// Some runtimes expose the extension at instance level but do not
		// implement it for the active body-tracking device.  Retry this frame
		// without optional output structures and stop asking on later frames.
		calibration_extension_enabled_ = false;
		fidelity_extension_enabled_ = false;
		effective_calibration_status_.reset();
		effective_fidelity_.reset();
		joint_locations.next = nullptr;
		res = locate();
	}
	if (!XR_SUCCEEDED(res))
	{
		log_error("xrLocateBodyJointsFB", res);
		return ret;
	}

	if (calibration_extension_enabled_)
	{
		switch (calibration_status.status)
		{
			case XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META:
			case XR_BODY_TRACKING_CALIBRATION_STATE_CALIBRATING_META:
			case XR_BODY_TRACKING_CALIBRATION_STATE_INVALID_META:
				effective_calibration_status_ = calibration_status.status;
				break;
			default:
				log_error("xrLocateBodyJointsFB returned an invalid body calibration status", XR_ERROR_VALIDATION_FAILURE);
				calibration_extension_enabled_ = false;
				effective_calibration_status_.reset();
				break;
		}
	}
	if (fidelity_extension_enabled_)
	{
		switch (fidelity_status.fidelity)
		{
			case XR_BODY_TRACKING_FIDELITY_LOW_META:
			case XR_BODY_TRACKING_FIDELITY_HIGH_META:
				effective_fidelity_ = fidelity_status.fidelity;
				break;
			default:
				log_error("xrLocateBodyJointsFB returned an invalid body tracking fidelity", XR_ERROR_VALIDATION_FAILURE);
				fidelity_extension_enabled_ = false;
				effective_fidelity_.reset();
				break;
		}
	}

	if (!joint_locations.isActive)
	{
		return ret;
	}

	skeleton_generation = joint_locations.skeletonChangedCount;
	ret.confidence = joint_locations.confidence;
	const auto & root = joints[XR_BODY_JOINT_ROOT_FB];

	if (joint_set == XR_BODY_JOINT_SET_FULL_BODY_META)
		ret.joints.emplace<wivrn::from_headset::meta_body::meta_joints>();
	else
		ret.joints.emplace<wivrn::from_headset::meta_body::fb_joints>();

	std::visit(utils::overloaded{
	                   [](std::monostate) {},
	                   [&root, &joints](auto & o) {
		                   o.root = {
		                           .position = root.pose.position,
		                           .orientation = pack(root.pose.orientation),
		                           .flags = wivrn::from_headset::to_pose_flags(root.locationFlags),
		                   };
		                   for (size_t joint = XR_BODY_JOINT_HIPS_FB; joint < std::size(o.joints) + 1; joint++)
		                   {
			                   const auto & loc = joints[joint];
			                   o.joints[joint - 1] = {
			                           .position = {
			                                   .x = int16_t((loc.pose.position.x - root.pose.position.x) * 10'000.f),
			                                   .y = int16_t((loc.pose.position.y - root.pose.position.y) * 10'000.f),
			                                   .z = int16_t((loc.pose.position.z - root.pose.position.z) * 10'000.f)},
			                           .orientation = pack(loc.pose.orientation),
			                           .flags = wivrn::from_headset::to_pose_flags(loc.locationFlags),
			                   };
		                   }
	                   },
	           },
	           ret.joints);

	return ret;
}

XrResult xr::fb_body_tracker::request_fidelity(XrBodyTrackingFidelityMETA requested) noexcept
{
	if (!id)
		return XR_ERROR_HANDLE_INVALID;
	if (requested != XR_BODY_TRACKING_FIDELITY_LOW_META and requested != XR_BODY_TRACKING_FIDELITY_HIGH_META)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!fidelity_extension_enabled_ or !xrRequestBodyTrackingFidelityMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;

	const XrResult result = xrRequestBodyTrackingFidelityMETA(id, requested);
	if (!XR_SUCCEEDED(result))
	{
		log_error("xrRequestBodyTrackingFidelityMETA", result);
		if (result == XR_ERROR_FEATURE_UNSUPPORTED)
		{
			fidelity_extension_enabled_ = false;
			effective_fidelity_.reset();
		}
	}
	return result;
}

XrResult xr::fb_body_tracker::suggest_calibration_override(float body_height) noexcept
{
	if (!id)
		return XR_ERROR_HANDLE_INVALID;
	if (!std::isfinite(body_height) or body_height <= 0.f)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!calibration_extension_enabled_ or !xrSuggestBodyTrackingCalibrationOverrideMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;

	XrBodyTrackingCalibrationInfoMETA info{
	        .type = XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META,
	        .next = nullptr,
	        .bodyHeight = body_height,
	};
	const XrResult result = xrSuggestBodyTrackingCalibrationOverrideMETA(id, &info);
	if (!XR_SUCCEEDED(result))
	{
		log_error("xrSuggestBodyTrackingCalibrationOverrideMETA", result);
		if (result == XR_ERROR_FEATURE_UNSUPPORTED)
		{
			calibration_extension_enabled_ = false;
			effective_calibration_status_.reset();
		}
	}
	return result;
}

XrResult xr::fb_body_tracker::reset_calibration() noexcept
{
	if (!id)
		return XR_ERROR_HANDLE_INVALID;
	if (!calibration_extension_enabled_ or !xrResetBodyTrackingCalibrationMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;

	const XrResult result = xrResetBodyTrackingCalibrationMETA(id);
	if (!XR_SUCCEEDED(result))
	{
		log_error("xrResetBodyTrackingCalibrationMETA", result);
		if (result == XR_ERROR_FEATURE_UNSUPPORTED)
		{
			calibration_extension_enabled_ = false;
			effective_calibration_status_.reset();
		}
	}
	return result;
}

void xr::fb_body_tracker::log_error(std::string_view operation, XrResult result) noexcept
{
	const auto now = std::chrono::steady_clock::now();
	if (now < next_error_log_)
		return;

	next_error_log_ = now + std::chrono::seconds(1);
	spdlog::warn("{} returned {}", operation, xr::to_string(result));
}

bool xr::fb_body_tracker::should_send_skeleton()
{
	return last_sent_skeleton_generation != skeleton_generation;
}
wivrn::from_headset::meta_body_skeleton xr::fb_body_tracker::get_skeleton()
{
	wivrn::from_headset::meta_body_skeleton ret{};
	if (joint_set == XR_BODY_JOINT_SET_FULL_BODY_META)
		ret.skeleton.emplace<wivrn::from_headset::meta_body_skeleton::meta_skeleton>();
	else
		ret.skeleton.emplace<wivrn::from_headset::meta_body_skeleton::fb_skeleton>();

	XrBodySkeletonFB skeleton{
	        .type = XR_TYPE_BODY_SKELETON_FB,
	};
	std::visit(utils::overloaded{
	                   [&skeleton](auto & s) {
		                   skeleton.joints = s.joints.data();
		                   skeleton.jointCount = s.joints.size();
	                   }},
	           ret.skeleton);
	if (auto res = xrGetBodySkeletonFB(id, &skeleton); !XR_SUCCEEDED(res))
	{
		throw std::runtime_error(std::format("xrGetBodySkeletonFB returned {}", xr::to_string(res)));
	}

	return ret;
}
