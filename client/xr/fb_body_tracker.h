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

#pragma once

#include "wivrn_packets.h"

#include "utils/handle.h"
#include <chrono>
#include <optional>
#include <string_view>
#include <openxr/openxr.h>

namespace xr
{
class instance;
class session;

class fb_body_tracker : public utils::handle<XrBodyTrackerFB>
{
	PFN_xrSuggestBodyTrackingCalibrationOverrideMETA xrSuggestBodyTrackingCalibrationOverrideMETA{};
	PFN_xrResetBodyTrackingCalibrationMETA xrResetBodyTrackingCalibrationMETA{};
	PFN_xrRequestBodyTrackingFidelityMETA xrRequestBodyTrackingFidelityMETA{};
	PFN_xrLocateBodyJointsFB xrLocateBodyJointsFB{};
	PFN_xrGetBodySkeletonFB xrGetBodySkeletonFB{};

	// An extension can be advertised by a runtime while the underlying body
	// tracking device does not implement it.  Keep these flags separate from
	// the instance extension list so an unsupported optional chain/function is
	// disabled after the first XR_ERROR_FEATURE_UNSUPPORTED response.
	bool calibration_extension_enabled_ = false;
	bool fidelity_extension_enabled_ = false;
	std::optional<XrBodyTrackingCalibrationStateMETA> effective_calibration_status_;
	std::optional<XrBodyTrackingFidelityMETA> effective_fidelity_;
	std::chrono::steady_clock::time_point next_error_log_{};

	XrBodyJointSetFB joint_set{};
	uint32_t last_sent_skeleton_generation = static_cast<uint32_t>(-1);
	uint32_t skeleton_generation = 0;

public:
	using packet_type = wivrn::from_headset::meta_body;

	fb_body_tracker(instance & inst, session & s, bool full_body);

	packet_type locate_spaces(XrTime time, XrSpace reference);
	bool should_send_skeleton();
	wivrn::from_headset::meta_body_skeleton get_skeleton() noexcept(false);
	void mark_skeleton_sent() noexcept
	{
		last_sent_skeleton_generation = skeleton_generation;
	}

	// These wrappers deliberately return the OpenXR result instead of throwing:
	// callers may use them from a control/UI path and unsupported optional
	// capabilities must not tear down the tracking thread.
	XrResult request_fidelity(XrBodyTrackingFidelityMETA fidelity) noexcept;
	XrResult suggest_calibration_override(float body_height) noexcept;
	XrResult reset_calibration() noexcept;

	std::optional<XrBodyTrackingCalibrationStateMETA> effective_calibration_status() const noexcept
	{
		return effective_calibration_status_;
	}
	std::optional<XrBodyTrackingFidelityMETA> effective_fidelity() const noexcept
	{
		return effective_fidelity_;
	}

	// Long names mirror the OpenXR entry points and make this API convenient
	// for callers that do not want to depend on the shorter aliases above.
	XrResult request_body_tracking_fidelity(XrBodyTrackingFidelityMETA fidelity) noexcept
	{
		return request_fidelity(fidelity);
	}
	XrResult suggest_body_tracking_calibration_override(float body_height) noexcept
	{
		return suggest_calibration_override(body_height);
	}
	XrResult reset_body_tracking_calibration() noexcept
	{
		return reset_calibration();
	}

private:
	void log_error(std::string_view operation, XrResult result) noexcept;
};
} // namespace xr
