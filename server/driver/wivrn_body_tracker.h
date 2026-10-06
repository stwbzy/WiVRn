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

#pragma once

#include "body_pose_assist.h"
#include "driver/wivrn_generic_tracker.h"
#include "utils/thread_safe.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "body_motion_classifier.h"
#include "fbt_supervisor.h"
#include "history.h"
#include "pose_list.h"
#include "wivrn_packets.h"

#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wivrn
{
class wivrn_session;
class wivrn_body_tracker;
struct clock_offset;

class body_joints_list : public history<body_joints_list, xrt_body_joint_set>
{
	wivrn::from_headset::body_type type;
	wivrn_body_tracker & parent;

public:
	body_joints_list(wivrn::from_headset::body_type type, wivrn_body_tracker & parent) : type(type), parent(parent) {}
	xrt_body_joint_set interpolate(const xrt_body_joint_set & a, const xrt_body_joint_set & b, float t);
	xrt_body_joint_set extrapolate(const xrt_body_joint_set & a, const xrt_body_joint_set & b, int64_t ta, int64_t tb, int64_t t);

	void update_tracking(const wivrn::from_headset::meta_body & tracking, const clock_offset & offset);
	void update_tracking(const wivrn::from_headset::bd_body & tracking, const clock_offset & offset);
};

class wivrn_body_tracker : public xrt_device
{
	friend body_joints_list;

	body_joints_list joints_list;
	std::vector<xrt_input> inputs_array;

	wivrn_session & cnx;

	thread_safe<xrt_body_skeleton> meta_skeleton;
	std::atomic_uint32_t meta_skeleton_generation;
	std::vector<fbt_supervisor> joint_supervisors;
	from_headset::body_tracking_settings body_settings{};
	from_headset::body_tracking_calibration_state calibration_state = from_headset::body_tracking_calibration_state::unknown;
	from_headset::body_tracking_fidelity effective_fidelity = from_headset::body_tracking_fidelity::automatic;
	bool supervisor_configured = false;
	bool body_active = false;
	body_pose_assist pose_assist;
	body_motion_classifier motion_classifier;
	body_motion_state active_motion_state = body_motion_state::unknown;
	std::vector<xrt_space_relation> micro_motion_relations;
	bool micro_motion_initialized = false;

	uint32_t joint_count() const;
	bool is_foot_joint(uint32_t joint) const;
	xrt_space_relation supervise_joint(uint32_t joint, int64_t timestamp_ns, const xrt_space_relation & relation, float confidence);
	void configure_supervisors(const from_headset::body_tracking_settings & settings);
	std::array<std::optional<uint32_t>, body_pose_role_count> pose_role_joints() const;
	void update_virtual_trackers(const std::vector<xrt_space_relation> & relations,
	                             XrTime production_timestamp,
	                             int64_t timestamp_ns,
	                             float provider_confidence,
	                             const clock_offset & offset,
	                             bool calibration_blocks_output = false);
	void apply_motion_policy();

protected:
	// key is joint
	std::unordered_map<uint32_t, from_headset::body_part_mask> joint_body_parts;
	// key is joint
	std::unordered_map<uint32_t, wivrn_generic_tracker> virtual_trackers;

public:
	using base_t = xrt_device;
	wivrn_body_tracker(xrt_device * hmd, wivrn_session & cnx, std::function<void(xrt_device &)> device_add_callback);

	xrt_result_t update_inputs();
	xrt_result_t get_body_skeleton(xrt_input_name body_tracking_type, xrt_body_skeleton * out_value);
	xrt_result_t get_body_joints(xrt_input_name body_tracking_type, int64_t at_timestamp_ns, xrt_body_joint_set * out_value);

	void update_tracking(const from_headset::meta_body &, const clock_offset &);
	void update_tracking(const from_headset::bd_body &, const clock_offset &);
	void update_skeleton(const from_headset::meta_body_skeleton &);
	void reset_tracking();
	to_headset::body_tracking_status status(uint64_t body_epoch, uint64_t connection_generation) const;

	void operator()(const from_headset::settings_changed & settings);
};

} // namespace wivrn
