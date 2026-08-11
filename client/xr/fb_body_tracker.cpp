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
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <variant>
#include <openxr/openxr.h>

xr::fb_body_tracker::fb_body_tracker(instance & inst, session & s, bool full_body) :
        handle(inst.get_proc<PFN_xrDestroyBodyTrackerFB>("xrDestroyBodyTrackerFB")),
        xrRequestBodyTrackingFidelityMETA(inst.has_extension(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)
                                                  ? inst.get_proc<PFN_xrRequestBodyTrackingFidelityMETA>("xrRequestBodyTrackingFidelityMETA")
                                                  : nullptr),
        xrLocateBodyJointsFB(inst.get_proc<PFN_xrLocateBodyJointsFB>("xrLocateBodyJointsFB")),
        xrGetBodySkeletonFB(inst.get_proc<PFN_xrGetBodySkeletonFB>("xrGetBodySkeletonFB")),
        joint_set(full_body ? XR_BODY_JOINT_SET_FULL_BODY_META : XR_BODY_JOINT_SET_DEFAULT_FB)
{
	auto xrCreateBodyTrackerFB = inst.get_proc<PFN_xrCreateBodyTrackerFB>("xrCreateBodyTrackerFB");

	XrBodyTrackerCreateInfoFB create_info{
	        .type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB,
	        .bodyJointSet = joint_set,
	};

	CHECK_XR(xrCreateBodyTrackerFB(s, &create_info, &id));

	// Enable IOBT.
	if (xrRequestBodyTrackingFidelityMETA)
		xrRequestBodyTrackingFidelityMETA(id, XR_BODY_TRACKING_FIDELITY_HIGH_META);
}

xr::fb_body_tracker::~fb_body_tracker()
{
	if (active_state_known && !last_active_state)
		maximum_inactive_duration = std::max(
		        maximum_inactive_duration,
		        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - inactive_since));

	if (locate_failures || inactive_samples || inactive_periods)
	{
		spdlog::info(
		        "Meta body tracking summary: locate_failures={}, inactive_samples={}, inactive_periods={}, maximum_inactive={}ms",
		        locate_failures,
		        inactive_samples,
		        inactive_periods,
		        std::chrono::duration_cast<std::chrono::milliseconds>(maximum_inactive_duration).count());
	}
}

void xr::fb_body_tracker::update_active_state(bool active)
{
	const auto now = std::chrono::steady_clock::now();
	if (!active_state_known)
	{
		active_state_known = true;
		last_active_state = active;
		if (!active)
		{
			inactive_periods++;
			inactive_since = now;
			spdlog::warn("Meta body tracking started inactive");
		}
		return;
	}

	if (last_active_state == active)
		return;

	last_active_state = active;
	if (!active)
	{
		inactive_periods++;
		inactive_since = now;
		spdlog::warn("Meta body tracking became inactive");
	}
	else
	{
		const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - inactive_since);
		maximum_inactive_duration = std::max(maximum_inactive_duration, duration);
		spdlog::info(
		        "Meta body tracking recovered after {}ms",
		        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
	}
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

	if (auto res = xrLocateBodyJointsFB(id, &locate_info, &joint_locations); !XR_SUCCEEDED(res))
	{
		locate_failures++;
		const auto now = std::chrono::steady_clock::now();
		if (now >= next_failure_log)
		{
			spdlog::warn("xrLocateBodyJointsFB returned {} ({} failures total)", xr::to_string(res), locate_failures);
			next_failure_log = now + std::chrono::seconds(5);
		}
		return ret;
	}

	if (!joint_locations.isActive)
	{
		inactive_samples++;
		update_active_state(false);
		return ret;
	}
	update_active_state(true);

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

	last_sent_skeleton_generation = skeleton_generation;
	return ret;
}
