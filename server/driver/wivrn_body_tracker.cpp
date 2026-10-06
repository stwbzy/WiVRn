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

#include "wivrn_body_tracker.h"
#include "driver/pose_list.h"
#include "driver/wivrn_generic_tracker.h"
#include "util/u_logging.h"
#include "utils/method.h"
#include "utils/overloaded.h"
#include "wivrn_packets.h"
#include "wivrn_session.h"
#include "xrt_cast.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"
#include <algorithm>
#include <cmath>
#include <magic_enum.hpp>
#include <unordered_map>

static_assert(std::to_underlying(XR_BODY_JOINT_COUNT_FB) == std::to_underlying(XRT_BODY_JOINT_COUNT_FB));
static_assert(std::to_underlying(XR_FULL_BODY_JOINT_COUNT_META) == std::to_underlying(XRT_FULL_BODY_JOINT_COUNT_META));
static_assert(XR_BODY_JOINT_COUNT_BD == XRT_BODY_JOINT_COUNT_BD);

static xrt_space_relation to_relation(const wivrn::from_headset::meta_body::pose & pose)
{
	return {
	        .relation_flags = from_pose_flags(pose.flags),
	        .pose = xrt_cast(XrPosef{
	                .orientation = pose.orientation,
	                .position = pose.position,
	        }),
	};
}
static xrt_space_relation to_relation(const wivrn::from_headset::bd_body::pose & pose)
{
	return {
	        .relation_flags = from_pose_flags(pose.flags),
	        .pose = xrt_cast(XrPosef{
	                .orientation = pose.orientation,
	                .position = pose.position,
	        }),
	};
}
static xrt_space_relation to_relation(const wivrn::from_headset::meta_body::pose & base, const wivrn::from_headset::meta_body::packed_pose & pose)
{
	return {
	        .relation_flags = from_pose_flags(pose.flags),
	        .pose = xrt_cast(XrPosef{
	                .orientation = pose.orientation,
	                .position = {
	                        .x = base.position.x + float(pose.position.x / 10'000.f),
	                        .y = base.position.y + float(pose.position.y / 10'000.f),
	                        .z = base.position.z + float(pose.position.z / 10'000.f),
	                },
	        }),
	};
}

static inline bool is_joint_set_active(wivrn::from_headset::body_type body_type, const xrt_body_joint_set & set)
{
	switch (body_type)
	{
		case wivrn::from_headset::body_type::fb:
		case wivrn::from_headset::body_type::meta:
			return set.base_body_joint_set_meta.is_active;
		case wivrn::from_headset::body_type::bd:
			return set.body_joint_set_bd.is_active;
		default:
			assert(false);
			__builtin_unreachable();
	}
}

static bool relation_is_valid(const xrt_space_relation & relation)
{
	constexpr auto required = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
	return (relation.relation_flags & required) == required;
}

static bool same_supervisor_settings(const wivrn::from_headset::body_tracking_settings & a,
                                     const wivrn::from_headset::body_tracking_settings & b)
{
	return a.profile == b.profile && a.fidelity == b.fidelity &&
	       a.calibration_mode == b.calibration_mode && a.supervisor == b.supervisor &&
	       a.supervisor_path == b.supervisor_path &&
	       a.smoothing_position_permille == b.smoothing_position_permille &&
	       a.smoothing_rotation_permille == b.smoothing_rotation_permille &&
	       a.confidence_threshold_permille == b.confidence_threshold_permille &&
	       a.dropout_hold_ms == b.dropout_hold_ms && a.recovery_samples == b.recovery_samples &&
	       a.prediction_ms == b.prediction_ms && a.body_height_mm == b.body_height_mm &&
	       a.floor_assist == b.floor_assist;
}

static wivrn::fbt_supervisor_config supervisor_config(const wivrn::from_headset::body_tracking_settings & settings,
                                                      wivrn::body_motion_state motion_state = wivrn::body_motion_state::unknown)
{
	wivrn::fbt_supervisor_config result{};
	switch (settings.profile)
	{
		case wivrn::body_tracking_profile::stable:
			result.minimum_confidence = 0.60f;
			result.hold_duration_ns = 180'000'000;
			result.hard_loss_duration_ns = 700'000'000;
			result.recovery_samples = 5;
			result.max_linear_speed_m_s = 6.0f;
			result.max_angular_speed_rad_s = 15.0f;
			result.smoothing_alpha = 0.22f;
			break;
		case wivrn::body_tracking_profile::balanced:
			break;
		case wivrn::body_tracking_profile::responsive:
			result.minimum_confidence = 0.35f;
			result.hold_duration_ns = 60'000'000;
			result.hard_loss_duration_ns = 350'000'000;
			result.recovery_samples = 2;
			result.max_linear_speed_m_s = 12.0f;
			result.max_angular_speed_rad_s = 30.0f;
			result.smoothing_alpha = 0.65f;
			break;
		case wivrn::body_tracking_profile::custom: {
			result.minimum_confidence = std::clamp(settings.confidence_threshold_permille / 1000.0f, 0.0f, 1.0f);
			result.hold_duration_ns = int64_t(settings.dropout_hold_ms) * 1'000'000;
			result.hard_loss_duration_ns = std::max<int64_t>(500'000'000, result.hold_duration_ns * 5);
			result.recovery_samples = std::max<uint32_t>(1, settings.recovery_samples);
			const float smoothing = (settings.smoothing_position_permille + settings.smoothing_rotation_permille) / 2000.0f;
			result.smoothing_alpha = std::clamp(1.0f - smoothing, 0.08f, 0.98f);
			break;
		}
	}

	const auto motion_profile = settings.motion_profile == wivrn::body_motion_profile::automatic
	                                    ? motion_state
	                            : settings.motion_profile == wivrn::body_motion_profile::sprint
	                                    ? wivrn::body_motion_state::sprint
	                            : settings.motion_profile == wivrn::body_motion_profile::chill
	                                    ? wivrn::body_motion_state::chill
	                            : settings.motion_profile == wivrn::body_motion_profile::flow
	                                    ? wivrn::body_motion_state::flow
	                                    : wivrn::body_motion_state::unknown;
	// `current` and an unclassified Auto stream intentionally leave the existing
	// technical Balanced/Stable/Responsive policy untouched.
	switch (motion_profile)
	{
		case wivrn::body_motion_state::sprint:
			result.hold_duration_ns = 60'000'000;
			result.hard_loss_duration_ns = 350'000'000;
			result.recovery_samples = 2;
			result.max_linear_speed_m_s = 15.0f;
			result.max_angular_speed_rad_s = 35.0f;
			result.smoothing_alpha = 0.72f;
			break;
		case wivrn::body_motion_state::chill:
			result.hold_duration_ns = 220'000'000;
			result.hard_loss_duration_ns = 900'000'000;
			result.recovery_samples = 5;
			result.max_linear_speed_m_s = 5.0f;
			result.max_angular_speed_rad_s = 12.0f;
			result.smoothing_alpha = 0.18f;
			break;
		case wivrn::body_motion_state::flow:
			result.hold_duration_ns = 100'000'000;
			result.hard_loss_duration_ns = 500'000'000;
			result.recovery_samples = 2;
			result.max_linear_speed_m_s = 12.0f;
			result.max_angular_speed_rad_s = 40.0f;
			result.smoothing_alpha = 0.55f;
			break;
		case wivrn::body_motion_state::unknown:
			break;
	}

	// The raw path still validates, holds and recovers, but does not alter an
	// accepted relation. The filtered path applies the adaptive one-pole filter.
	result.smoothing_enabled = settings.supervisor_path == wivrn::body_tracking_supervisor_path::filtered;
	return result;
}

static wivrn::body_motion_state configured_motion_state(const wivrn::from_headset::body_tracking_settings & settings,
                                                        wivrn::body_motion_state detected)
{
	switch (settings.motion_profile)
	{
		case wivrn::body_motion_profile::sprint:
			return wivrn::body_motion_state::sprint;
		case wivrn::body_motion_profile::chill:
			return wivrn::body_motion_state::chill;
		case wivrn::body_motion_profile::flow:
			return wivrn::body_motion_state::flow;
		case wivrn::body_motion_profile::automatic:
			return detected;
		case wivrn::body_motion_profile::current:
			return wivrn::body_motion_state::unknown;
	}
	return wivrn::body_motion_state::unknown;
}

static float micro_motion_threshold(const wivrn::from_headset::body_tracking_settings & settings,
                                    wivrn::body_motion_state motion_state)
{
	float threshold_mm = settings.micro_motion_threshold_mm;
	if (settings.motion_profile == wivrn::body_motion_profile::sprint ||
	    motion_state == wivrn::body_motion_state::sprint)
		threshold_mm *= 0.35f;
	else if (settings.motion_profile == wivrn::body_motion_profile::flow ||
	         motion_state == wivrn::body_motion_state::flow)
		threshold_mm *= 0.65f;
	return std::clamp(threshold_mm / 1000.0f, 0.0f, 0.05f);
}

static float relation_speed(const xrt_space_relation & relation, xrt_space_relation_flags flag)
{
	if ((relation.relation_flags & flag) == 0)
		return 0.0f;
	const auto & velocity = relation.linear_velocity;
	const float value = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
	return std::isfinite(value) ? value : 0.0f;
}

static float angular_speed(const xrt_space_relation & relation)
{
	if ((relation.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) == 0)
		return 0.0f;
	const auto & velocity = relation.angular_velocity;
	const float value = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
	return std::isfinite(value) ? value : 0.0f;
}

static bool body_is_moving(const wivrn::body_pose_assist_input & input, const std::vector<xrt_space_relation> & relations)
{
	// Do not quantize a slow walk: velocity flags from the HMD/hand anchors are
	// a better macro-motion signal than the per-frame displacement of a virtual
	// leg, which can remain below the dead zone at normal refresh rates.
	if (relation_speed(input.head.relation, XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) > 0.10f ||
	    angular_speed(input.head.relation) > 0.80f ||
	    relation_speed(input.left_hand.relation, XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) > 0.16f ||
	    relation_speed(input.right_hand.relation, XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) > 0.16f ||
	    angular_speed(input.left_hand.relation) > 1.20f || angular_speed(input.right_hand.relation) > 1.20f)
		return true;

	for (const auto & relation: relations)
		if (relation_speed(relation, XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) > 0.20f || angular_speed(relation) > 1.50f)
			return true;
	return false;
}

static wivrn::body_tracking_supervisor_state protocol_state(wivrn::fbt_supervisor_state state)
{
	switch (state)
	{
		case wivrn::fbt_supervisor_state::warming_up:
			return wivrn::body_tracking_supervisor_state::warming_up;
		case wivrn::fbt_supervisor_state::tracking:
			return wivrn::body_tracking_supervisor_state::tracking;
		case wivrn::fbt_supervisor_state::holding:
			return wivrn::body_tracking_supervisor_state::holding;
		case wivrn::fbt_supervisor_state::lost:
			return wivrn::body_tracking_supervisor_state::lost;
		case wivrn::fbt_supervisor_state::recovering:
			return wivrn::body_tracking_supervisor_state::recovering;
	}
	return wivrn::body_tracking_supervisor_state::lost;
}

static wivrn::body_tracking_supervisor_reason protocol_reason(wivrn::fbt_supervisor_reason reason)
{
	switch (reason)
	{
		case wivrn::fbt_supervisor_reason::none:
		case wivrn::fbt_supervisor_reason::accepted:
		case wivrn::fbt_supervisor_reason::warming_up:
		case wivrn::fbt_supervisor_reason::recovery_pending:
		case wivrn::fbt_supervisor_reason::recovered:
			return wivrn::body_tracking_supervisor_reason::none;
		case wivrn::fbt_supervisor_reason::reordered_timestamp:
			return wivrn::body_tracking_supervisor_reason::reordered;
		case wivrn::fbt_supervisor_reason::low_confidence:
		case wivrn::fbt_supervisor_reason::invalid_confidence:
			return wivrn::body_tracking_supervisor_reason::low_confidence;
		case wivrn::fbt_supervisor_reason::excessive_linear_velocity:
			return wivrn::body_tracking_supervisor_reason::excessive_velocity;
		case wivrn::fbt_supervisor_reason::excessive_angular_velocity:
			return wivrn::body_tracking_supervisor_reason::excessive_angular_velocity;
		case wivrn::fbt_supervisor_reason::held_dropout:
		case wivrn::fbt_supervisor_reason::hold_expired:
		case wivrn::fbt_supervisor_reason::hard_loss:
			return wivrn::body_tracking_supervisor_reason::dropout;
		case wivrn::fbt_supervisor_reason::reset:
			return wivrn::body_tracking_supervisor_reason::reset;
		case wivrn::fbt_supervisor_reason::invalid_timestamp:
		case wivrn::fbt_supervisor_reason::invalid_flags:
		case wivrn::fbt_supervisor_reason::non_finite:
		case wivrn::fbt_supervisor_reason::invalid_quaternion:
		case wivrn::fbt_supervisor_reason::count:
			return wivrn::body_tracking_supervisor_reason::invalid_pose;
	}
	return wivrn::body_tracking_supervisor_reason::internal_error;
}

template <typename T>
static void interpolate_joints(const T & a, const T & b, T & out, float t)
{
	for (auto [a, b, out]: std::views::zip(a.joint_locations, b.joint_locations, out.joint_locations))
		out.relation = wivrn::pose_list::interpolate(a.relation, b.relation, t);
}

template <typename T>
static void extrapolate_joints(const T & a, const T & b, T & out, int64_t ta, int64_t tb, int64_t t)
{
	for (auto [a, b, out]: std::views::zip(a.joint_locations, b.joint_locations, out.joint_locations))
		out.relation = wivrn::pose_list::extrapolate(a.relation, b.relation, ta, tb, t);
}

namespace wivrn
{
xrt_body_joint_set body_joints_list::interpolate(const xrt_body_joint_set & a, const xrt_body_joint_set & b, float t)
{
	if (not is_joint_set_active(type, a))
	{
		// in case neither is valid, both will be zeroed,
		// so return the later one for timestamp's sake
		return b;
	}
	else if (not is_joint_set_active(type, b))
	{
		return a;
	}

	xrt_body_joint_set result = b;
	switch (type)
	{
		case wivrn::from_headset::body_type::fb:
			interpolate_joints(a.body_joint_set_fb, b.body_joint_set_fb, result.body_joint_set_fb, t);
			break;
		case wivrn::from_headset::body_type::meta:
			interpolate_joints(a.full_body_joint_set_meta, b.full_body_joint_set_meta, result.full_body_joint_set_meta, t);
			break;
		case wivrn::from_headset::body_type::bd:
			interpolate_joints(a.body_joint_set_bd, b.body_joint_set_bd, result.body_joint_set_bd, t);
			break;
		default:
			assert(false);
			__builtin_unreachable();
	}
	return result;
}

xrt_body_joint_set body_joints_list::extrapolate(const xrt_body_joint_set & a, const xrt_body_joint_set & b, int64_t ta, int64_t tb, int64_t t)
{
	if (not is_joint_set_active(type, a))
	{
		// in case neither is valid, both will be zeroed,
		// so return the later one for timestamp's sake
		return b;
	}
	else if (not is_joint_set_active(type, b))
	{
		return a;
	}

	xrt_body_joint_set result = b;
	switch (type)
	{
		case wivrn::from_headset::body_type::fb:
			extrapolate_joints(a.body_joint_set_fb, b.body_joint_set_fb, result.body_joint_set_fb, ta, tb, t);
			break;
		case wivrn::from_headset::body_type::meta:
			extrapolate_joints(a.full_body_joint_set_meta, b.full_body_joint_set_meta, result.full_body_joint_set_meta, ta, tb, t);
			break;
		case wivrn::from_headset::body_type::bd:
			extrapolate_joints(a.body_joint_set_bd, b.body_joint_set_bd, result.body_joint_set_bd, ta, tb, t);
			break;
		default:
			assert(false);
			__builtin_unreachable();
	}
	return result;
}

void body_joints_list::update_tracking(const wivrn::from_headset::meta_body & tracking, const clock_offset & offset)
{
	assert(type == from_headset::body_type::fb or type == from_headset::body_type::meta);

	xrt_body_joint_set s{
	        .base_body_joint_set_meta = {
	                .sample_time_ns = tracking.timestamp,
	                .confidence = tracking.confidence,
	                .is_active = false,
	        },
	};
	const bool calibration_blocks_output =
	        tracking.calibration_state == body_tracking_calibration_state::invalid ||
	        tracking.calibration_state == body_tracking_calibration_state::calibrating;
	bool any_valid = false;
	std::vector<xrt_space_relation> safe_relations(parent.joint_count());
	auto publish_joint = [&](uint32_t joint, xrt_space_relation pose) {
		// META explicitly defines calibrating/invalid poses as unsafe. Do not let
		// either the supervisor hold path or the unsupervised path publish them.
		if (calibration_blocks_output)
			pose = xrt_space_relation XRT_SPACE_RELATION_ZERO;
		else
			pose = parent.supervise_joint(joint, tracking.timestamp, pose, tracking.confidence);
		s.full_body_joint_set_meta.joint_locations[joint].relation = pose;
		safe_relations[joint] = pose;
		any_valid = any_valid || relation_is_valid(pose);
	};
	std::visit(utils::overloaded{
	                   [&](std::monostate) {
		                   for (uint32_t joint = 0; joint < parent.joint_count(); ++joint)
			                   publish_joint(joint, {});
	                   },
	                   [&](auto & joints) {
		                   publish_joint(XRT_BODY_JOINT_ROOT_FB, to_relation(joints.root));
		                   for (size_t joint = XRT_BODY_JOINT_HIPS_FB; joint < std::size(joints.joints) + 1; joint++)
		                   {
			                   publish_joint(joint, to_relation(joints.root, joints.joints[joint - 1]));
		                   }
	                   }},
	           tracking.joints);
	s.base_body_joint_set_meta.is_active = any_valid;
	parent.body_active = any_valid;
	parent.update_virtual_trackers(safe_relations,
	                               tracking.production_timestamp,
	                               tracking.timestamp,
	                               tracking.confidence,
	                               offset,
	                               calibration_blocks_output);

	add_sample(tracking.production_timestamp, tracking.timestamp, s, offset);
}
void body_joints_list::update_tracking(const wivrn::from_headset::bd_body & tracking, const clock_offset & offset)
{
	assert(type == from_headset::body_type::bd);

	xrt_body_joint_set s{
	        .body_joint_set_bd = {
	                .sample_time_ns = tracking.timestamp,
	                .is_active = false,
	                .all_joint_poses_tracked = tracking.all_tracked,
	        },
	};
	bool any_valid = false;
	std::vector<xrt_space_relation> safe_relations(parent.joint_count());
	for (size_t joint = XRT_BODY_JOINT_PELVIS_BD; joint < XRT_BODY_JOINT_COUNT_BD; joint++)
	{
		const auto pose = parent.supervise_joint(joint, tracking.timestamp, to_relation(tracking.joints[joint]), 1.0f);
		s.body_joint_set_bd.joint_locations[joint].relation = pose;
		safe_relations[joint] = pose;
		any_valid = any_valid || relation_is_valid(pose);
	}
	s.body_joint_set_bd.is_active = any_valid;
	parent.body_active = any_valid;
	parent.update_virtual_trackers(safe_relations,
	                               tracking.production_timestamp,
	                               tracking.timestamp,
	                               1.0f,
	                               offset);

	add_sample(tracking.production_timestamp, tracking.timestamp, s, offset);
}

uint32_t wivrn_body_tracker::joint_count() const
{
	switch (cnx.get_info().body_tracking)
	{
		case from_headset::body_type::fb:
			return XRT_BODY_JOINT_COUNT_FB;
		case from_headset::body_type::meta:
			return XRT_FULL_BODY_JOINT_COUNT_META;
		case from_headset::body_type::bd:
			return XRT_BODY_JOINT_COUNT_BD;
		default:
			return 0;
	}
}

std::array<std::optional<uint32_t>, body_pose_role_count> wivrn_body_tracker::pose_role_joints() const
{
	std::array<std::optional<uint32_t>, body_pose_role_count> result{};
	const auto set = [&](body_pose_role role, uint32_t joint) { result[static_cast<size_t>(role)] = joint; };
	switch (cnx.get_info().body_tracking)
	{
		case from_headset::body_type::meta:
			set(body_pose_role::chest, XRT_FULL_BODY_JOINT_CHEST_META);
			set(body_pose_role::hip, XRT_FULL_BODY_JOINT_HIPS_META);
			set(body_pose_role::left_knee, XRT_FULL_BODY_JOINT_LEFT_LOWER_LEG_META);
			set(body_pose_role::right_knee, XRT_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META);
			set(body_pose_role::left_foot, XRT_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META);
			set(body_pose_role::right_foot, XRT_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META);
			break;
		case from_headset::body_type::bd:
			set(body_pose_role::chest, XRT_BODY_JOINT_SPINE3_BD);
			set(body_pose_role::hip, XRT_BODY_JOINT_PELVIS_BD);
			set(body_pose_role::left_knee, XRT_BODY_JOINT_LEFT_KNEE_BD);
			set(body_pose_role::right_knee, XRT_BODY_JOINT_RIGHT_KNEE_BD);
			set(body_pose_role::left_foot, XRT_BODY_JOINT_LEFT_FOOT_BD);
			set(body_pose_role::right_foot, XRT_BODY_JOINT_RIGHT_FOOT_BD);
			break;
		case from_headset::body_type::fb:
			set(body_pose_role::chest, XRT_BODY_JOINT_CHEST_FB);
			set(body_pose_role::hip, XRT_BODY_JOINT_HIPS_FB);
			break;
		default:
			break;
	}
	return result;
}

void wivrn_body_tracker::update_virtual_trackers(const std::vector<xrt_space_relation> & relations,
                                                 XrTime production_timestamp,
                                                 int64_t timestamp_ns,
                                                 float provider_confidence,
                                                 const clock_offset & offset,
                                                 bool calibration_blocks_output)
{
	if (calibration_blocks_output)
	{
		for (auto & [_, tracker]: virtual_trackers)
			tracker.reset_tracking();
		pose_assist.reset();
		motion_classifier.reset();
		micro_motion_relations.clear();
		micro_motion_initialized = false;
		return;
	}

	body_pose_assist_input input{};
	input.timestamp_ns = timestamp_ns;
	input.provider_confidence = provider_confidence;
	input.body_height_mm = body_settings.body_height_mm.value_or(1700);
	const auto roles = pose_role_joints();
	for (size_t i = 0; i < body_pose_role_count; ++i)
	{
		if (roles[i] && *roles[i] < relations.size())
		{
			input.provider[i] = relations[*roles[i]];
			input.provider_valid[i] = relation_is_valid(input.provider[i]);
		}
	}
	const auto set_ankle = [&](size_t side, uint32_t joint) {
		if (joint < relations.size())
			input.ankles[side] = {.relation = relations[joint], .valid = relation_is_valid(relations[joint])};
	};
	switch (cnx.get_info().body_tracking)
	{
		case from_headset::body_type::meta:
			set_ankle(0, XRT_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_META);
			set_ankle(1, XRT_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_META);
			break;
		case from_headset::body_type::bd:
			set_ankle(0, XRT_BODY_JOINT_LEFT_ANKLE_BD);
			set_ankle(1, XRT_BODY_JOINT_RIGHT_ANKLE_BD);
			break;
		default:
			break;
	}
	if (auto anchors = cnx.get_body_anchors(timestamp_ns))
	{
		input.head = {.relation = anchors->head, .valid = anchors->head_valid};
		input.left_hand = {.relation = anchors->left_hand, .valid = anchors->left_hand_valid};
		input.right_hand = {.relation = anchors->right_hand, .valid = anchors->right_hand_valid};
	}

	body_motion_sample motion_input{};
	motion_input.timestamp_ns = timestamp_ns;
	motion_input.relations = input.provider;
	motion_input.relation_valid = input.provider_valid;
	motion_input.head = input.head;
	motion_input.left_hand = input.left_hand;
	motion_input.right_hand = input.right_hand;
	if (body_settings.motion_profile == body_motion_profile::automatic)
	{
		const auto detected = motion_classifier.update(motion_input);
		if (detected != body_motion_state::unknown && detected != active_motion_state)
		{
			active_motion_state = detected;
			apply_motion_policy();
		}
	}
	else
	{
		active_motion_state = configured_motion_state(body_settings, body_motion_state::unknown);
	}

	const auto assisted = pose_assist.update(input);
	if (assisted.active)
	{
		// The lying-pose layer owns its own bounded trajectory; do not apply a
		// standing micro-deadzone to synthesized tracker relations.
		micro_motion_initialized = false;
	}
	else if (micro_motion_relations.size() != relations.size())
	{
		micro_motion_relations.assign(relations.size(), XRT_SPACE_RELATION_ZERO);
		micro_motion_initialized = false;
	}
	const float micro_threshold = micro_motion_threshold(body_settings, active_motion_state);
	const bool micro_filter_active = micro_threshold > 0.0f && !body_is_moving(input, relations);
	for (auto & [joint, tracker]: virtual_trackers)
	{
		xrt_space_relation pose = joint < relations.size() ? relations[joint] : xrt_space_relation XRT_SPACE_RELATION_ZERO;
		const auto role = std::find_if(roles.begin(), roles.end(), [joint](const auto & value) { return value && *value == joint; });
		if (assisted.active && role != roles.end())
			pose = assisted.relations[std::distance(roles.begin(), role)];
		if (!assisted.active && micro_threshold > 0.0f && relation_is_valid(pose) &&
		    joint < micro_motion_relations.size())
		{
			if (!micro_filter_active)
			{
				// Keep the cache aligned while moving so enabling the dead zone later
				// never snaps back to an old standing pose.
				micro_motion_relations[joint] = pose;
			}
			else if (micro_motion_initialized && relation_is_valid(micro_motion_relations[joint]))
			{
				const auto & previous = micro_motion_relations[joint].pose.position;
				const auto & current = pose.pose.position;
				const float dx = current.x - previous.x;
				const float dy = current.y - previous.y;
				const float dz = current.z - previous.z;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) <= micro_threshold)
					pose = micro_motion_relations[joint];
				else
					micro_motion_relations[joint] = pose;
			}
			else
				micro_motion_relations[joint] = pose;
		}

		// Quest frequently reports VALID without TRACKED. Virtual tracker
		// consumers expect both for a usable supervised pose.
		std::underlying_type_t<xrt_space_relation_flags> flags = pose.relation_flags;
		if (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)
			flags |= XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
		if (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT)
			flags |= XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
		pose.relation_flags = static_cast<xrt_space_relation_flags>(flags);
		tracker.update_tracking(production_timestamp, timestamp_ns, pose, offset);
	}
	if (!assisted.active && micro_threshold > 0.0f)
		micro_motion_initialized = micro_filter_active;
}

bool wivrn_body_tracker::is_foot_joint(uint32_t joint) const
{
	switch (cnx.get_info().body_tracking)
	{
		case from_headset::body_type::meta:
			return joint == XRT_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META ||
			       joint == XRT_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META;
		case from_headset::body_type::bd:
			return joint == XRT_BODY_JOINT_LEFT_FOOT_BD || joint == XRT_BODY_JOINT_RIGHT_FOOT_BD;
		case from_headset::body_type::fb:
		default:
			return false;
	}
}

xrt_space_relation wivrn_body_tracker::supervise_joint(uint32_t joint,
                                                       int64_t timestamp_ns,
                                                       const xrt_space_relation & relation,
                                                       float confidence)
{
	if (body_settings.supervisor == body_tracking_supervisor::off || joint >= joint_supervisors.size())
		return relation;
	auto output = joint_supervisors[joint].update(timestamp_ns, relation, confidence).relation;
	if (!relation_is_valid(output))
		return output;

	if (body_settings.prediction_ms > 0 &&
	    (output.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0)
	{
		const float seconds = body_settings.prediction_ms / 1000.0f;
		output.pose.position.x += output.linear_velocity.x * seconds;
		output.pose.position.y += output.linear_velocity.y * seconds;
		output.pose.position.z += output.linear_velocity.z * seconds;
	}

	// The body is located in WiVRn's floor-relative world space. Floor assist
	// only prevents a tracked foot from sinking below that plane; it never pins
	// a lifted foot or fabricates a missing relation.
	if (body_settings.floor_assist && is_foot_joint(joint) && output.pose.position.y < 0.0f)
		output.pose.position.y = 0.0f;
	return output;
}

void wivrn_body_tracker::configure_supervisors(const from_headset::body_tracking_settings & settings)
{
	body_settings = settings;
	pose_assist.configure(settings.pose_assist);
	joint_supervisors.clear();
	joint_supervisors.reserve(joint_count());
	active_motion_state = body_motion_state::unknown;
	motion_classifier.reset();
	micro_motion_relations.clear();
	micro_motion_initialized = false;
	const auto config = supervisor_config(settings, active_motion_state);
	for (uint32_t joint = 0; joint < joint_count(); ++joint)
		joint_supervisors.emplace_back(config);
	supervisor_configured = true;
	reset_tracking();
}

void wivrn_body_tracker::apply_motion_policy()
{
	const auto config = supervisor_config(body_settings, active_motion_state);
	for (auto & supervisor: joint_supervisors)
		supervisor.reconfigure(config);
}

wivrn_body_tracker::wivrn_body_tracker(xrt_device * hmd, wivrn::wivrn_session & cnx, std::function<void(xrt_device &)> device_add_callback) :
        xrt_device{
                .name = XRT_DEVICE_FB_BODY_TRACKING, // FIXME: replace with something more appropriate, when there is one
                .device_type = XRT_DEVICE_TYPE_BODY_TRACKER,
                .tracking_origin = hmd->tracking_origin,
                .supported = {
                        .body_tracking = true,
                },
                .update_inputs = method_pointer<&wivrn_body_tracker::update_inputs>,
                .get_body_skeleton = method_pointer<&wivrn_body_tracker::get_body_skeleton>,
                .get_body_joints = method_pointer<&wivrn_body_tracker::get_body_joints>,
                .destroy = [](xrt_device *) {},
        },
        joints_list(cnx.get_info().body_tracking, *this),
        cnx(cnx)
{
	static const std::unordered_map<from_headset::body_part_mask, std::string> body_part_pretty_names{
	        {from_headset::body_part_mask::chest, "Chest"},
	        {from_headset::body_part_mask::left_elbow, "Left Elbow"},
	        {from_headset::body_part_mask::right_elbow, "Right Elbow"},
	        {from_headset::body_part_mask::hip, "Hip"},
	        {from_headset::body_part_mask::left_knee, "Left Knee"},
	        {from_headset::body_part_mask::right_knee, "Right Knee"},
	        {from_headset::body_part_mask::left_foot, "Left Foot"},
	        {from_headset::body_part_mask::right_foot, "Right Foot"},
	};

	switch (cnx.get_info().body_tracking)
	{
		case from_headset::body_type::meta:
			strcpy(str, "WiVRn Meta body tracker");
			strcpy(serial, "WiVRn Meta body tracker");
			inputs_array.push_back({
			        .active = true,
			        .name = XRT_INPUT_FB_BODY_TRACKING,
			});
			inputs_array.push_back({
			        .active = true,
			        .name = XRT_INPUT_META_FULL_BODY_TRACKING,
			});

			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_CHEST_META, from_headset::body_part_mask::chest);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_LEFT_ARM_LOWER_META, from_headset::body_part_mask::left_elbow);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_RIGHT_ARM_LOWER_META, from_headset::body_part_mask::right_elbow);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_HIPS_META, from_headset::body_part_mask::hip);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_LEFT_LOWER_LEG_META, from_headset::body_part_mask::left_knee);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META, from_headset::body_part_mask::right_knee);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META, from_headset::body_part_mask::left_foot);
			joint_body_parts.emplace(XRT_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META, from_headset::body_part_mask::right_foot);
			break;
		case from_headset::body_type::fb:
			strcpy(str, "WiVRn FB body tracker");
			strcpy(serial, "WiVRn FB body tracker");
			inputs_array.push_back({
			        .active = true,
			        .name = XRT_INPUT_FB_BODY_TRACKING,
			});

			joint_body_parts.emplace(XRT_BODY_JOINT_CHEST_FB, from_headset::body_part_mask::chest);
			joint_body_parts.emplace(XRT_BODY_JOINT_LEFT_ARM_LOWER_FB, from_headset::body_part_mask::left_elbow);
			joint_body_parts.emplace(XRT_BODY_JOINT_RIGHT_ARM_LOWER_FB, from_headset::body_part_mask::right_elbow);
			joint_body_parts.emplace(XRT_BODY_JOINT_HIPS_FB, from_headset::body_part_mask::hip);
			break;
		case from_headset::body_type::bd:
			strcpy(str, "WiVRn BD body tracker");
			strcpy(serial, "WiVRn BD body tracker");
			inputs_array.push_back({
			        .active = true,
			        .name = XRT_INPUT_BD_BODY_TRACKING,
			});

			joint_body_parts.emplace(XRT_BODY_JOINT_SPINE3_BD, from_headset::body_part_mask::chest);
			joint_body_parts.emplace(XRT_BODY_JOINT_LEFT_ELBOW_BD, from_headset::body_part_mask::left_elbow);
			joint_body_parts.emplace(XRT_BODY_JOINT_RIGHT_ELBOW_BD, from_headset::body_part_mask::right_elbow);
			joint_body_parts.emplace(XRT_BODY_JOINT_PELVIS_BD, from_headset::body_part_mask::hip);
			joint_body_parts.emplace(XRT_BODY_JOINT_LEFT_KNEE_BD, from_headset::body_part_mask::left_knee);
			joint_body_parts.emplace(XRT_BODY_JOINT_RIGHT_KNEE_BD, from_headset::body_part_mask::right_knee);
			joint_body_parts.emplace(XRT_BODY_JOINT_LEFT_FOOT_BD, from_headset::body_part_mask::left_foot);
			joint_body_parts.emplace(XRT_BODY_JOINT_RIGHT_FOOT_BD, from_headset::body_part_mask::right_foot);
			break;
		default:
			assert(false);
			__builtin_unreachable();
	}

	inputs = inputs_array.data();
	input_count = inputs_array.size();

	for (auto & [joint, body_part]: joint_body_parts)
	{
		auto & name = body_part_pretty_names.at(body_part);
		auto [it, success] = virtual_trackers.emplace(std::piecewise_construct,
		                                              std::forward_as_tuple(joint),
		                                              std::forward_as_tuple(name, hmd, cnx));
		assert(success);
		device_add_callback(it->second);
	}
	const auto & pose_catalog = body_pose_catalog_status();
	U_LOG_I("Body pose catalog loaded: poses=%zu rejected=%zu",
	        pose_catalog.loaded,
	        pose_catalog.rejected);
	for (const auto & issue: pose_catalog.issues)
		U_LOG_W("Body pose catalog: %s", issue.c_str());
	(*this)(*cnx.get_settings());
}

xrt_result_t wivrn_body_tracker::update_inputs()
{
	return XRT_SUCCESS;
}
xrt_result_t wivrn_body_tracker::get_body_skeleton(xrt_input_name body_skeleton_type, xrt_body_skeleton * out_value)
{
	if (!std::ranges::contains(inputs_array, body_skeleton_type, &xrt_input::name))
	{
		U_LOG_XDEV_UNSUPPORTED_INPUT(this, u_log_get_global_level(), body_skeleton_type);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	auto s = this->meta_skeleton.lock();
	*out_value = *s;
	return XRT_SUCCESS;
}
xrt_result_t wivrn_body_tracker::get_body_joints(xrt_input_name body_tracking_type, int64_t at_timestamp_ns, xrt_body_joint_set * out_value)
{
	if (!std::ranges::contains(inputs_array, body_tracking_type, &xrt_input::name))
	{
		U_LOG_XDEV_UNSUPPORTED_INPUT(this, u_log_get_global_level(), body_tracking_type);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	XrTime production_timestamp;
	std::tie(production_timestamp, *out_value) = joints_list.get_at(at_timestamp_ns);
	if (is_joint_set_active(cnx.get_info().body_tracking, *out_value))
	{
		out_value->body_pose = xrt_space_relation{
		        .relation_flags = xrt_space_relation_flags(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
		                                                   XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT),
		        .pose = XRT_POSE_IDENTITY,
		};
	}
	else
	{
		out_value->body_pose = {};
	}

	if (body_tracking_type == XRT_INPUT_FB_BODY_TRACKING or body_tracking_type == XRT_INPUT_META_FULL_BODY_TRACKING)
		out_value->base_body_joint_set_meta.skeleton_changed_count = meta_skeleton_generation;

	cnx.add_tracking_request(device_id::BODY, at_timestamp_ns, production_timestamp);
	return XRT_SUCCESS;
}

void wivrn_body_tracker::update_tracking(const from_headset::meta_body & tracking, const clock_offset & offset)
{
	calibration_state = tracking.calibration_state;
	effective_fidelity = tracking.fidelity;
	joints_list.update_tracking(tracking, offset);
}
void wivrn_body_tracker::update_tracking(const from_headset::bd_body & tracking, const clock_offset & offset)
{
	joints_list.update_tracking(tracking, offset);
}
void wivrn_body_tracker::update_skeleton(const from_headset::meta_body_skeleton & skeleton)
{
	assert(std::ranges::contains(inputs_array, XRT_INPUT_FB_BODY_TRACKING, &xrt_input::name));

	auto s = meta_skeleton.lock();
	std::visit(utils::overloaded{
	                   [&s](auto & skeleton) {
		                   for (size_t i = 0; i < skeleton.joints.size(); i++)
		                   {
			                   static_assert(offsetof(xrt_full_body_skeleton_meta, joints) == offsetof(xrt_body_skeleton_fb, joints));
			                   static_assert(sizeof(xrt_full_body_skeleton_meta::joints[0]) == sizeof(xrt_body_skeleton_fb::joints[0]));
			                   const auto & joint = skeleton.joints[i];
			                   s->full_body_skeleton_meta.joints[i] = xrt_body_skeleton_joint_fb{
			                           .pose = xrt_cast(joint.pose),
			                           .joint = joint.joint,
			                           .parent_joint = joint.parentJoint,
			                   };
		                   }
	                   }},
	           skeleton.skeleton);
	if (const auto * full = std::get_if<from_headset::meta_body_skeleton::meta_skeleton>(&skeleton.skeleton))
	{
		const auto joint_offset_length = [&](XrFullBodyJointMETA joint) {
			const auto & p = full->joints[std::to_underlying(joint)].pose.position;
			return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		};
		body_pose_skeleton_profile profile{};
		profile.valid = true;
		profile.torso_length = joint_offset_length(XR_FULL_BODY_JOINT_SPINE_LOWER_META) +
		                       joint_offset_length(XR_FULL_BODY_JOINT_SPINE_MIDDLE_META) +
		                       joint_offset_length(XR_FULL_BODY_JOINT_SPINE_UPPER_META) +
		                       joint_offset_length(XR_FULL_BODY_JOINT_CHEST_META) +
		                       joint_offset_length(XR_FULL_BODY_JOINT_NECK_META) +
		                       joint_offset_length(XR_FULL_BODY_JOINT_HEAD_META);
		profile.pelvis_half_width =
		        (joint_offset_length(XR_FULL_BODY_JOINT_LEFT_UPPER_LEG_META) +
		         joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_UPPER_LEG_META)) *
		        0.5f;
		profile.thigh_length = {
		        joint_offset_length(XR_FULL_BODY_JOINT_LEFT_LOWER_LEG_META),
		        joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META),
		};
		profile.shin_length = {
		        joint_offset_length(XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_TWIST_META) +
		                joint_offset_length(XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_META),
		        joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_TWIST_META) +
		                joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_META),
		};
		profile.ankle_to_foot = {
		        joint_offset_length(XR_FULL_BODY_JOINT_LEFT_FOOT_SUBTALAR_META) +
		                joint_offset_length(XR_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META),
		        joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_FOOT_SUBTALAR_META) +
		                joint_offset_length(XR_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META),
		};
		pose_assist.set_skeleton_profile(profile);
	}
	meta_skeleton_generation++;
}

void wivrn_body_tracker::reset_tracking()
{
	joints_list.reset();
	pose_assist.reset();
	motion_classifier.reset();
	active_motion_state = body_motion_state::unknown;
	micro_motion_relations.clear();
	micro_motion_initialized = false;
	for (auto & supervisor: joint_supervisors)
		supervisor.reset();
	for (auto & [_, tracker]: virtual_trackers)
		tracker.reset_tracking();
	body_active = false;
}

to_headset::body_tracking_status wivrn_body_tracker::status(uint64_t body_epoch, uint64_t connection_generation) const
{
	to_headset::body_tracking_status result{
	        .profile = body_settings.profile,
	        .fidelity = effective_fidelity,
	        .calibration_state = calibration_state,
	        .supervisor = body_settings.supervisor,
	        .supervisor_state = body_settings.supervisor == body_tracking_supervisor::off
	                                    ? body_tracking_supervisor_state::disabled
	                                    : body_tracking_supervisor_state::tracking,
	        .supervisor_reason = body_tracking_supervisor_reason::none,
	        .active = body_active,
	        .body_epoch = body_epoch,
	        .connection_generation = connection_generation,
	};
	const auto & pose = pose_assist.output();
	result.pose_state = pose.state;
	result.pose_confidence_permille = static_cast<uint16_t>(std::clamp(pose.posture_confidence * 1000.0f, 0.0f, 1000.0f));
	result.pose_anchor_mask = pose.anchor_mask;
	result.pose_fallbacks = pose.fallback_count;

	auto severity = [](body_tracking_supervisor_state state) {
		switch (state)
		{
			case body_tracking_supervisor_state::disabled:
				return 0;
			case body_tracking_supervisor_state::tracking:
				return 1;
			case body_tracking_supervisor_state::warming_up:
				return 2;
			case body_tracking_supervisor_state::recovering:
				return 3;
			case body_tracking_supervisor_state::holding:
				return 4;
			case body_tracking_supervisor_state::lost:
				return 5;
		}
		return 5;
	};

	if (body_settings.supervisor != body_tracking_supervisor::off)
	{
		for (const auto & supervisor: joint_supervisors)
		{
			const auto & metrics = supervisor.metrics();
			result.accepted += metrics.accepted_samples;
			result.rejected += metrics.rejected_samples;
			result.held += metrics.held_samples;
			const auto state = protocol_state(supervisor.current_state());
			if (severity(state) > severity(result.supervisor_state))
			{
				result.supervisor_state = state;
				result.supervisor_reason = protocol_reason(metrics.last_reason);
			}
			else if (result.supervisor_reason == body_tracking_supervisor_reason::none)
			{
				result.supervisor_reason = protocol_reason(metrics.last_reason);
			}
		}
	}
	if (calibration_state == body_tracking_calibration_state::invalid ||
	    calibration_state == body_tracking_calibration_state::calibrating)
		result.supervisor_reason = body_tracking_supervisor_reason::calibration;
	return result;
}

void wivrn_body_tracker::operator()(const from_headset::settings_changed & settings)
{
	if (!supervisor_configured || !same_supervisor_settings(body_settings, settings.body_tracking))
	{
		U_LOG_I("Body tracking policy updated: profile=%s supervisor=%s path=%s pose_assist=%s",
		        std::string(magic_enum::enum_name(settings.body_tracking.profile)).c_str(),
		        std::string(magic_enum::enum_name(settings.body_tracking.supervisor)).c_str(),
		        std::string(magic_enum::enum_name(settings.body_tracking.supervisor_path)).c_str(),
		        std::string(magic_enum::enum_name(settings.body_tracking.pose_assist.mode)).c_str());
		configure_supervisors(settings.body_tracking);
	}
	else
	{
		if (body_settings.motion_profile != settings.body_tracking.motion_profile)
		{
			U_LOG_I("Body motion profile updated live: profile=%s",
			        std::string(magic_enum::enum_name(settings.body_tracking.motion_profile)).c_str());
			body_settings.motion_profile = settings.body_tracking.motion_profile;
			active_motion_state = configured_motion_state(body_settings, body_motion_state::unknown);
			motion_classifier.reset();
			micro_motion_relations.clear();
			micro_motion_initialized = false;
			apply_motion_policy();
		}
		if (body_settings.micro_motion_threshold_mm != settings.body_tracking.micro_motion_threshold_mm)
		{
			body_settings.micro_motion_threshold_mm = settings.body_tracking.micro_motion_threshold_mm;
			micro_motion_relations.clear();
			micro_motion_initialized = false;
		}
		if (body_settings.pose_assist != settings.body_tracking.pose_assist)
		{
			U_LOG_I("Lying pose policy updated live: mode=%s pose=%s asset=%s bed=%umm",
			        std::string(magic_enum::enum_name(settings.body_tracking.pose_assist.mode)).c_str(),
			        std::string(magic_enum::enum_name(settings.body_tracking.pose_assist.manual_pose)).c_str(),
			        std::string(magic_enum::enum_name(settings.body_tracking.pose_assist.asset)).c_str(),
			        static_cast<unsigned>(settings.body_tracking.pose_assist.bed_height_mm));
			body_settings.pose_assist = settings.body_tracking.pose_assist;
			pose_assist.configure(settings.body_tracking.pose_assist);
		}
	}
	U_LOG_I("Virtual body tracker mask updated: 0x%02x", static_cast<unsigned>(settings.enabled_body_parts));
	for (auto & [joint, tracker]: virtual_trackers)
	{
		const auto body_part = joint_body_parts.at(joint);
		tracker.set_enabled(settings.enabled_body_parts & std::to_underlying(body_part));
	}
}
} // namespace wivrn
