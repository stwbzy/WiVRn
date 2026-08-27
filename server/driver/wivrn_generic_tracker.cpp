/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 * Copyright (C) 2026  Sapphire <imsapphire0@gmail.com>
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

#include "wivrn_generic_tracker.h"
#include "util/u_logging.h"
#include "utils/method.h"
#include "wivrn_session.h"

#include "math/m_api.h"
#include "math/m_eigen_interop.hpp"
#include "math/m_space.h"
#include "math/m_vec3.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <format>

using namespace xrt::auxiliary::math;

namespace wivrn
{

xrt_space_relation tracker_pose_list::interpolate(const xrt_space_relation & a, const xrt_space_relation & b, float t)
{
	xrt_space_relation result;
	xrt_space_relation_flags flags = xrt_space_relation_flags(a.relation_flags & b.relation_flags);
	m_space_relation_interpolate(const_cast<xrt_space_relation *>(&a), const_cast<xrt_space_relation *>(&b), t, flags, &result);
	return result;
}

xrt_space_relation tracker_pose_list::extrapolate(const xrt_space_relation & a, const xrt_space_relation & b, int64_t ta, int64_t tb, int64_t t)
{
	float h = (tb - ta) / 1.e9;

	xrt_space_relation res = t < ta ? a : b;

	xrt_vec3 lin_vel = res.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT ? res.linear_velocity : (b.pose.position - a.pose.position) / h;

	float dt = (t - tb) / 1.e9;

	float dt2_over_2 = dt * dt / 2;
	res.pose.position = res.pose.position + lin_vel * dt;

	if (res.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)
	{
		xrt_vec3 dtheta = res.angular_velocity * dt;
		xrt_quat dq;
		math_quat_exp(&dtheta, &dq);

		map_quat(res.pose.orientation) = map_quat(res.pose.orientation) * map_quat(dq);
	}

	return res;
}

static bool is_usable_tracker_pose(const xrt_space_relation & pose)
{
	constexpr auto required_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
	if ((pose.relation_flags & required_flags) != required_flags)
		return false;

	return std::isfinite(pose.pose.orientation.x) &&
	       std::isfinite(pose.pose.orientation.y) &&
	       std::isfinite(pose.pose.orientation.z) &&
	       std::isfinite(pose.pose.orientation.w) &&
	       std::isfinite(pose.pose.position.x) &&
	       std::isfinite(pose.pose.position.y) &&
	       std::isfinite(pose.pose.position.z);
}

virtual_tracker_dropout_filter::result
tracker_pose_list::update_tracking(XrTime produced_timestamp, XrTime timestamp, const xrt_space_relation & pose, const clock_offset & offset)
{
	std::lock_guard lock(update_mutex);
	auto result = dropout_filter.update(produced_timestamp, is_usable_tracker_pose(pose));

	switch (result.next_action)
	{
		case virtual_tracker_dropout_filter::action::accept_valid:
			add_sample(produced_timestamp, timestamp, pose, offset);
			break;
		case virtual_tracker_dropout_filter::action::publish_invalid:
			replace_sample(produced_timestamp, timestamp, {}, offset);
			break;
		case virtual_tracker_dropout_filter::action::recover:
			// Do not interpolate recovery against the deliberately invalid sample.
			replace_sample(produced_timestamp, timestamp, pose, offset);
			break;
		case virtual_tracker_dropout_filter::action::suppress_invalid:
		case virtual_tracker_dropout_filter::action::maintain_invalid:
		case virtual_tracker_dropout_filter::action::wait_for_valid:
		case virtual_tracker_dropout_filter::action::ignore_reordered:
			break;
	}
	return result;
}

void tracker_pose_list::reset_tracking()
{
	std::lock_guard lock(update_mutex);
	dropout_filter.reset();
	history<tracker_pose_list, xrt_space_relation>::reset();
}

wivrn_generic_tracker::wivrn_generic_tracker(std::string name, xrt_device * hmd, wivrn_session & cnx) :
        xrt_device{
                .name = XRT_DEVICE_VIVE_TRACKER,
                .device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER,
                .hmd = nullptr,
                .tracking_origin = hmd->tracking_origin,
                .supported = {
                        .orientation_tracking = true,
                        .position_tracking = true,
                },
                .update_inputs = method_pointer<&wivrn_generic_tracker::update_inputs>,
                .get_tracked_pose = method_pointer<&wivrn_generic_tracker::get_tracked_pose>,
                .destroy = [](xrt_device *) {},
        },
        cnx(cnx)
{
	auto unique_name = std::format("WiVRn generic tracker ({})", name);
	strlcpy(str, unique_name.c_str(), std::size(str));
	{
		std::string serial{};
		serial.reserve(name.size());
		std::ranges::transform(name, std::back_inserter(serial), [](auto c) { return tolower(c); });
		std::ranges::replace(serial, ' ', '-');
		auto unique_serial = std::format("wivrn-{}", serial);
		strlcpy(this->serial, unique_serial.c_str(), std::size(this->serial));
	}

	pose_input.name = XRT_INPUT_GENERIC_TRACKER_POSE;
	pose_input.active = true;

	inputs = &pose_input;
	input_count = 1;
}

wivrn_generic_tracker::~wivrn_generic_tracker()
{
	if (invalid_samples_suppressed || transient_dropouts || sustained_dropouts || recoveries || reordered_samples)
	{
		U_LOG_I("Virtual tracker %s summary: suppressed_invalid_samples=%" PRIu64
		        ", transient_dropouts=%" PRIu64 ", sustained_dropouts=%" PRIu64
		        ", recoveries=%" PRIu64 ", reordered_samples=%" PRIu64
		        ", maximum_invalid_gap=%" PRIi64 "ms",
		        serial,
		        invalid_samples_suppressed,
		        transient_dropouts,
		        sustained_dropouts,
		        recoveries,
		        reordered_samples,
		        maximum_invalid_duration / 1'000'000);
	}
}

xrt_result_t wivrn_generic_tracker::update_inputs()
{
	return XRT_SUCCESS;
}

xrt_result_t wivrn_generic_tracker::get_tracked_pose(xrt_input_name name, int64_t at_timestamp_ns, xrt_space_relation * res)
{
	XrTime production_timestamp;

	if (!enabled.load(std::memory_order_acquire))
	{
		*res = {};
		return XRT_SUCCESS;
	}

	if (name == XRT_INPUT_GENERIC_TRACKER_POSE)
	{
		std::tie(production_timestamp, *res) = poses.get_at(at_timestamp_ns);

		cnx.add_tracking_request(device_id::BODY, at_timestamp_ns, production_timestamp);
		return XRT_SUCCESS;
	}

	U_LOG_XDEV_UNSUPPORTED_INPUT(this, u_log_get_global_level(), name);
	return XRT_ERROR_INPUT_UNSUPPORTED;
}

void wivrn_generic_tracker::update_tracking(XrTime produced_timestamp, XrTime timestamp, const xrt_space_relation & pose, const clock_offset & offset)
{
	auto result = poses.update_tracking(produced_timestamp, timestamp, pose, offset);
	maximum_invalid_duration = std::max(maximum_invalid_duration, XrDuration(result.invalid_duration_ns));

	switch (result.next_action)
	{
		case virtual_tracker_dropout_filter::action::accept_valid:
			if (result.invalid_duration_ns)
			{
				transient_dropouts++;
				U_LOG_D("Virtual tracker %s suppressed a %" PRIi64 "ms tracking dropout",
				        serial,
				        result.invalid_duration_ns / 1'000'000);
			}
			break;
		case virtual_tracker_dropout_filter::action::suppress_invalid:
			invalid_samples_suppressed++;
			break;
		case virtual_tracker_dropout_filter::action::publish_invalid:
			sustained_dropouts++;
			U_LOG_W("Virtual tracker %s lost tracking for %" PRIi64 "ms; publishing untracked state",
			        serial,
			        result.invalid_duration_ns / 1'000'000);
			break;
		case virtual_tracker_dropout_filter::action::recover:
			recoveries++;
			U_LOG_I("Virtual tracker %s recovered after %" PRIi64 "ms",
			        serial,
			        result.invalid_duration_ns / 1'000'000);
			break;
		case virtual_tracker_dropout_filter::action::ignore_reordered:
			reordered_samples++;
			break;
		case virtual_tracker_dropout_filter::action::maintain_invalid:
		case virtual_tracker_dropout_filter::action::wait_for_valid:
			break;
	}
}

void wivrn_generic_tracker::set_enabled(bool enabled)
{
	if (this->enabled.load(std::memory_order_acquire) == enabled)
		return;

	// Keep stale poses inaccessible while the history and grace state are reset.
	this->enabled.store(false, std::memory_order_release);
	poses.reset_tracking();
	this->enabled.store(enabled, std::memory_order_release);
	U_LOG_I("Virtual tracker %s %s", serial, enabled ? "enabled" : "disabled");
}

void wivrn_generic_tracker::reset_tracking()
{
	poses.reset_tracking();
}
} // namespace wivrn
