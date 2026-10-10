/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "body_pose_assist.h"

#include <cassert>
#include <cmath>

namespace
{
using namespace wivrn;

constexpr auto pose_flags = static_cast<xrt_space_relation_flags>(
        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

xrt_space_relation relation(float x, float y, float z, xrt_quat orientation = {0, 0, 0, 1})
{
	return {
	        .relation_flags = pose_flags,
	        .pose = {
	                .orientation = orientation,
	                .position = {x, y, z},
	        },
	};
}

body_pose_assist_input lying_input()
{
	body_pose_assist_input input{};
	input.timestamp_ns = 1'000'000'000;
	input.provider_confidence = 0.9f;
	input.head = {.relation = relation(0.0f, 0.40f, 0.0f), .valid = true};
	input.left_hand = {.relation = relation(-0.35f, 0.35f, 0.10f), .valid = true};
	input.right_hand = {.relation = relation(0.35f, 0.35f, 0.10f), .valid = true};
	input.provider[static_cast<size_t>(body_pose_role::chest)] = relation(0.0f, 0.18f, 0.35f);
	input.provider_valid[static_cast<size_t>(body_pose_role::chest)] = true;
	input.provider[static_cast<size_t>(body_pose_role::hip)] = relation(0.0f, 0.14f, 0.75f);
	input.provider_valid[static_cast<size_t>(body_pose_role::hip)] = true;
	return input;
}

body_pose_assist_input upright_input()
{
	auto input = lying_input();
	input.head.relation = relation(0.0f, 1.65f, 0.0f);
	input.provider[static_cast<size_t>(body_pose_role::chest)] = relation(0.0f, 1.25f, 0.0f);
	input.provider[static_cast<size_t>(body_pose_role::hip)] = relation(0.0f, 0.95f, 0.0f);
	return input;
}

body_pose_assist_output manual_pose(body_pose_kind kind,
                                    body_pose_assist_input input = lying_input(),
                                    uint16_t lower_body_influence_permille = 0,
                                    body_pose_asset_selection asset = body_pose_asset_selection::procedural)
{
	body_pose_assist assistant;
	body_pose_assist_settings settings{};
	settings.mode = body_pose_assist_mode::manual;
	settings.manual_pose = kind;
	settings.transition_ms = 100;
	settings.lower_body_influence_permille = lower_body_influence_permille;
	settings.asset = asset;
	assistant.configure(settings);
	assistant.update(input);
	input.timestamp_ns += 150'000'000;
	return assistant.update(input);
}

float orientation_distance(const xrt_quat & a, const xrt_quat & b)
{
	return 1.0f - std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

float position_distance(const xrt_vec3 & a, const xrt_vec3 & b)
{
	const float x = a.x - b.x;
	const float y = a.y - b.y;
	const float z = a.z - b.z;
	return std::sqrt(x * x + y * y + z * z);
}

float dot(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

xrt_vec3 direction(const xrt_vec3 & from, const xrt_vec3 & to)
{
	xrt_vec3 result{to.x - from.x, to.y - from.y, to.z - from.z};
	const float distance = std::sqrt(dot(result, result));
	assert(distance > 0.001f);
	return {result.x / distance, result.y / distance, result.z / distance};
}

xrt_vec3 rotate(const xrt_quat & q, const xrt_vec3 & value)
{
	const xrt_vec3 qv{q.x, q.y, q.z};
	const xrt_vec3 first_cross{
	        qv.y * value.z - qv.z * value.y,
	        qv.z * value.x - qv.x * value.z,
	        qv.x * value.y - qv.y * value.x,
	};
	const xrt_vec3 second_cross{
	        qv.y * first_cross.z - qv.z * first_cross.y,
	        qv.z * first_cross.x - qv.x * first_cross.z,
	        qv.x * first_cross.y - qv.y * first_cross.x,
	};
	return {
	        value.x + 2.0f * (q.w * first_cross.x + second_cross.x),
	        value.y + 2.0f * (q.w * first_cross.y + second_cross.y),
	        value.z + 2.0f * (q.w * first_cross.z + second_cross.z),
	};
}

void assert_leg_chain(const body_pose_assist_output & pose, body_pose_role knee_role, body_pose_role foot_role)
{
	const auto & knee = pose.relations[static_cast<size_t>(knee_role)].pose;
	const auto & foot = pose.relations[static_cast<size_t>(foot_role)].pose;
	const float shin_length = position_distance(knee.position, foot.position);
	assert(shin_length >= 0.31f && shin_length <= 0.53f);
	const auto tracker_down = direction({0.0f, 0.0f, 0.0f}, rotate(knee.orientation, {0.0f, -1.0f, 0.0f}));
	const auto shin_direction = direction(knee.position, foot.position);
	assert(dot(tracker_down, shin_direction) > 0.98f);
}
} // namespace

int main()
{
	body_pose_assist assistant;
	body_pose_assist_settings settings{};
	settings.mode = body_pose_assist_mode::automatic;
	settings.transition_ms = 100;
	assistant.configure(settings);

	auto input = lying_input();
	auto output = assistant.update(input);
	assert(output.state == body_pose_state::transitioning);
	assert(!output.active);

	input.timestamp_ns += 150'000'000;
	output = assistant.update(input);
	assert(output.state == body_pose_state::back);
	assert(output.active);
	assert(output.anchor_mask == 7);
	assert(output.relations[static_cast<size_t>(body_pose_role::left_knee)].relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT);
	assert(output.fallback_count > 0);

	// A single contradictory automatic sample must not release tracker ownership.
	auto upright = upright_input();
	upright.timestamp_ns = input.timestamp_ns + 20'000'000;
	output = assistant.update(upright);
	assert(output.active);
	assert(output.state == body_pose_state::back);
	input.timestamp_ns = upright.timestamp_ns + 20'000'000;
	output = assistant.update(input);
	assert(output.active);
	assert(output.state == body_pose_state::back);
	for (int i = 0; i < 200; ++i)
	{
		auto noisy = i % 2 == 0 ? upright_input() : lying_input();
		noisy.timestamp_ns = input.timestamp_ns + 10'000'000;
		input.timestamp_ns = noisy.timestamp_ns;
		output = assistant.update(noisy);
		assert(output.active);
		assert(output.state == body_pose_state::back);
	}

	settings.mode = body_pose_assist_mode::manual;
	settings.manual_pose = body_pose_kind::front;
	assistant.configure(settings);
	input.timestamp_ns += 20'000'000;
	assistant.update(input);
	input.timestamp_ns += 150'000'000;
	output = assistant.update(input);
	assert(output.state == body_pose_state::front);
	assert(output.active);

	// Live changes must retain ownership. Previously each slider step rebuilt all
	// supervisors and reset every virtual tracker.
	settings.bed_height_mm = 1500;
	settings.surface = body_surface_mode::bed;
	assistant.configure(settings);
	input.timestamp_ns += 20'000'000;
	output = assistant.update(input);
	assert(output.active);
	for (const auto & pose: output.relations)
	{
		assert(std::isfinite(pose.pose.position.y));
		assert(pose.pose.position.y <= input.head.relation.pose.position.y + 0.02f);
	}

	settings.manual_pose = body_pose_kind::left_side;
	assistant.configure(settings);
	input.timestamp_ns += 20'000'000;
	output = assistant.update(input);
	assert(output.active);
	assert(output.state == body_pose_state::left_side);

	settings.bed_height_mm = 0;
	assistant.configure(settings);
	float previous_hip_y = output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position.y;
	for (int i = 0; i < 30; ++i)
	{
		input.timestamp_ns += 10'000'000;
		output = assistant.update(input);
		const float hip_y = output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position.y;
		assert(std::abs(hip_y - previous_hip_y) < 0.05f);
		previous_hip_y = hip_y;
	}

	// Duplicate/reordered timestamps and a brief head dropout retain the committed
	// manual pose instead of publishing a one-frame raw FBT relation.
	const auto committed_hip = output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position;
	auto reordered = input;
	reordered.timestamp_ns -= 1;
	reordered.provider[static_cast<size_t>(body_pose_role::hip)].pose.position.z += 5.0f;
	output = assistant.update(reordered);
	assert(output.active);
	assert(output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position.z == committed_hip.z);

	input.timestamp_ns += 20'000'000;
	input.head.valid = false;
	output = assistant.update(input);
	assert(output.active);
	assert(output.state == body_pose_state::left_side);
	assert(output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position.z == committed_hip.z);
	input.head.valid = true;

	// The four fixed choices need distinct canonical orientation and mirrored
	// side-leg geometry even when every profile has the same numeric defaults.
	const auto back = manual_pose(body_pose_kind::back);
	const auto front = manual_pose(body_pose_kind::front);
	const auto left = manual_pose(body_pose_kind::left_side);
	const auto right = manual_pose(body_pose_kind::right_side);
	const auto hip = static_cast<size_t>(body_pose_role::hip);
	const auto left_knee = static_cast<size_t>(body_pose_role::left_knee);
	const auto right_knee = static_cast<size_t>(body_pose_role::right_knee);
	const auto left_foot = static_cast<size_t>(body_pose_role::left_foot);
	const auto right_foot = static_cast<size_t>(body_pose_role::right_foot);
	assert(orientation_distance(back.relations[hip].pose.orientation, front.relations[hip].pose.orientation) > 0.1f);
	assert(orientation_distance(left.relations[hip].pose.orientation, right.relations[hip].pose.orientation) > 0.1f);
	assert(back.relations[left_knee].pose.position.y > back.relations[left_foot].pose.position.y + 0.10f);
	assert(front.relations[left_foot].pose.position.y > front.relations[left_knee].pose.position.y + 0.10f);
	assert(position_distance(back.relations[left_knee].pose.position, back.relations[left_foot].pose.position) > 0.30f);
	assert(position_distance(back.relations[left_knee].pose.position, back.relations[left_foot].pose.position) < 0.55f);
	assert(left.relations[left_knee].pose.position.y < left.relations[right_knee].pose.position.y);
	assert(right.relations[left_knee].pose.position.y > right.relations[right_knee].pose.position.y);

	// Supine/prone legs stay on their own side instead of crossing through the
	// pelvis. Side poses stack the legs vertically and bend both knees toward the
	// same anatomical front, which is the failure visible in the VRChat captures.
	assert(back.relations[left_knee].pose.position.x < back.relations[right_knee].pose.position.x);
	assert(back.relations[left_foot].pose.position.x < back.relations[right_foot].pose.position.x);
	assert(front.relations[left_knee].pose.position.x < front.relations[right_knee].pose.position.x);
	assert(front.relations[left_foot].pose.position.x < front.relations[right_foot].pose.position.x);
	assert(std::abs(left.relations[left_knee].pose.position.x - left.relations[right_knee].pose.position.x) < 0.05f);
	assert(std::abs(right.relations[left_knee].pose.position.x - right.relations[right_knee].pose.position.x) < 0.05f);
	assert(left.relations[left_knee].pose.position.x < -0.15f);
	assert(right.relations[left_knee].pose.position.x > 0.15f);
	for (const auto * pose: {&back, &front, &left, &right})
	{
		assert_leg_chain(*pose, body_pose_role::left_knee, body_pose_role::left_foot);
		assert_leg_chain(*pose, body_pose_role::right_knee, body_pose_role::right_foot);
		for (const auto & tracker: pose->relations)
		{
			assert(std::isfinite(tracker.pose.position.x));
			assert(std::isfinite(tracker.pose.position.y));
			assert(std::isfinite(tracker.pose.position.z));
			assert(position_distance(tracker.pose.position, lying_input().head.relation.pose.position) < 1.55f);
		}
	}

	// The curated Unity asset is accepted only for its authored back category.
	// It preserves axial torso orientation and exact measured leg lengths while
	// remaining the sole source of lower-body targets.
	const auto & catalog = body_pose_catalog_status();
	assert(catalog.loaded >= 1);
	const auto * animation_25 = find_body_pose_asset(body_pose_asset_selection::unity_animation_25,
	                                                 body_pose_kind::back);
	assert(animation_25);
	assert(find_body_pose_asset(body_pose_asset_selection::unity_animation_25, body_pose_kind::front) == nullptr);
	body_pose_skeleton_profile asset_profile{};
	asset_profile.valid = true;
	asset_profile.torso_length = 0.49f;
	asset_profile.pelvis_half_width = 0.095f;
	asset_profile.thigh_length = {0.36f, 0.38f};
	asset_profile.shin_length = {0.34f, 0.35f};
	asset_profile.ankle_to_foot = {0.07f, 0.075f};
	const auto asset_solution = solve_body_pose_asset(
	        *animation_25,
	        {
	                .head = lying_input().head.relation.pose.position,
	                .body_forward = {0.0f, 0.0f, 1.0f},
	                .surface_y = 0.0f,
	                .body_height_m = 1.70f,
	                .skeleton = asset_profile,
	        });
	assert(asset_solution.valid);
	assert(rotate(asset_solution.chest_orientation, {0.0f, 0.0f, 1.0f}).y > 0.85f);
	for (size_t side = 0; side < 2; ++side)
	{
		assert(std::abs(position_distance(asset_solution.hip_joints[side], asset_solution.knees[side]) -
		                asset_profile.thigh_length[side]) < 0.002f);
		assert(std::abs(position_distance(asset_solution.knees[side], asset_solution.ankles[side]) -
		                asset_profile.shin_length[side]) < 0.002f);
		assert(asset_solution.ankles[side].y >= 0.0f);
		assert(asset_solution.feet[side].y >= 0.0f);
	}
	body_pose_asset_solve_input transition_input{
	        .head = lying_input().head.relation.pose.position,
	        .body_forward = {0.0f, 0.0f, 1.0f},
	        .surface_y = 0.0f,
	        .body_height_m = 1.70f,
	        .skeleton = asset_profile,
	        .target_influence = 0.35f,
	        .source_chest = {0.0f, 0.20f, 0.30f},
	        .source_hip = {0.0f, 0.15f, 0.70f},
	        .source_knees = {xrt_vec3{-0.15f, 0.25f, 0.85f}, xrt_vec3{0.15f, 0.25f, 0.85f}},
	        .source_ankles = {xrt_vec3{-0.15f, 0.04f, 1.10f}, xrt_vec3{0.15f, 0.04f, 1.10f}},
	        .source_chest_valid = true,
	        .source_hip_valid = true,
	        .source_knees_valid = {true, true},
	        .source_ankles_valid = {true, true},
	};
	const auto transition_solution = solve_body_pose_asset(*animation_25, transition_input);
	assert(transition_solution.valid);
	for (size_t side = 0; side < 2; ++side)
	{
		assert(std::abs(position_distance(transition_solution.hip_joints[side], transition_solution.knees[side]) -
		                asset_profile.thigh_length[side]) < 0.002f);
		assert(std::abs(position_distance(transition_solution.knees[side], transition_solution.ankles[side]) -
		                asset_profile.shin_length[side]) < 0.002f);
		assert(transition_solution.ankles[side].y >= 0.0f);
		assert(transition_solution.feet[side].y >= 0.0f);
	}

	const auto asset_back = manual_pose(body_pose_kind::back,
	                                    lying_input(),
	                                    1000,
	                                    body_pose_asset_selection::unity_animation_25);
	assert(asset_back.active);
	assert(asset_back.state == body_pose_state::back);
	assert(rotate(asset_back.relations[hip].pose.orientation, {0.0f, 0.0f, 1.0f}).y > 0.85f);
	assert(position_distance(asset_back.relations[left_knee].pose.position,
	                         back.relations[left_knee].pose.position) > 0.05f);
	// A manual pose selected while the tracking origin reports a standing-height
	// HMD may float, but it must remain a human-sized horizontal chain rather than
	// stretching the hip to the physical floor.
	const auto high_head_back = manual_pose(body_pose_kind::back, upright_input());
	assert(position_distance(high_head_back.relations[hip].pose.position,
	                         upright_input().head.relation.pose.position) < 0.58f);
	for (const auto & tracker: high_head_back.relations)
		assert(tracker.pose.position.y <= upright_input().head.relation.pose.position.y + 0.02f);

	// Even 100% provider influence cannot drag a synthesized leg meters away or
	// invert left/right topology.
	auto outlier_input = lying_input();
	for (size_t i = left_knee; i <= right_foot; ++i)
	{
		outlier_input.provider[i] = relation(i % 2 == 0 ? -5.0f : 5.0f, 4.0f, -3.0f);
		outlier_input.provider_valid[i] = true;
	}
	const auto bounded = manual_pose(body_pose_kind::back, outlier_input, 1000);
	for (size_t i = left_knee; i <= right_foot; ++i)
		assert(position_distance(bounded.relations[i].pose.position, back.relations[i].pose.position) < 0.14f);
	assert(bounded.relations[left_knee].pose.position.x < bounded.relations[right_knee].pose.position.x);
	const auto asset_outliers = manual_pose(body_pose_kind::back,
	                                        outlier_input,
	                                        1000,
	                                        body_pose_asset_selection::unity_animation_25);
	for (size_t role = left_knee; role <= right_foot; ++role)
		assert(position_distance(asset_outliers.relations[role].pose.position,
		                         asset_back.relations[role].pose.position) < 0.002f);

	// Replay a long stationary lying sequence with millimetric HMD noise and
	// alternating provider outliers. Contact locks must keep the feet planted,
	// while the projected chains preserve the calibrated Meta proportions.
	body_pose_assist replay_assistant;
	body_pose_assist_settings replay_settings{};
	replay_settings.mode = body_pose_assist_mode::manual;
	replay_settings.manual_pose = body_pose_kind::back;
	replay_settings.transition_ms = 100;
	replay_settings.lower_body_influence_permille = 1000;
	replay_assistant.configure(replay_settings);
	body_pose_skeleton_profile replay_profile{};
	replay_profile.valid = true;
	replay_profile.torso_length = 0.49f;
	replay_profile.pelvis_half_width = 0.095f;
	replay_profile.thigh_length = {0.36f, 0.38f};
	replay_profile.shin_length = {0.34f, 0.35f};
	replay_profile.ankle_to_foot = {0.07f, 0.075f};
	replay_assistant.set_skeleton_profile(replay_profile);
	auto replay_input = lying_input();
	replay_assistant.update(replay_input);
	replay_input.timestamp_ns += 150'000'000;
	auto replay = replay_assistant.update(replay_input);
	const auto planted_left = replay.relations[left_foot].pose.position;
	const auto planted_right = replay.relations[right_foot].pose.position;
	for (int frame = 0; frame < 1000; ++frame)
	{
		replay_input.timestamp_ns += 10'000'000;
		const float jitter = float((frame % 7) - 3) * 0.001f;
		replay_input.head.relation.pose.position.x = jitter;
		replay_input.head.relation.pose.position.y = 0.40f - jitter * 0.5f;
		for (size_t side = 0; side < 2; ++side)
		{
			replay_input.ankles[side] = {
			        .relation = relation(side == 0 ? -4.0f : 4.0f, frame % 2 == 0 ? 3.0f : -2.0f, -5.0f),
			        .valid = true,
			};
		}
		replay = replay_assistant.update(replay_input);
		assert(replay.active);
		for (const auto & tracker: replay.relations)
		{
			assert(std::isfinite(tracker.pose.position.x));
			assert(std::isfinite(tracker.pose.position.y));
			assert(std::isfinite(tracker.pose.position.z));
		}
	}
	assert(position_distance(replay.relations[left_foot].pose.position, planted_left) < 0.02f);
	assert(position_distance(replay.relations[right_foot].pose.position, planted_right) < 0.02f);
	const xrt_vec3 replay_axis{0.0f, 0.0f, 1.0f};
	const xrt_vec3 replay_lateral{-1.0f, 0.0f, 0.0f};
	const auto replay_hip = replay.relations[hip].pose.position;
	for (size_t side = 0; side < 2; ++side)
	{
		const auto hip_joint = xrt_vec3{
		        replay_hip.x + replay_lateral.x * replay_profile.pelvis_half_width * (side == 0 ? 1.0f : -1.0f),
		        replay_hip.y,
		        replay_hip.z,
		};
		const auto knee_position = replay.relations[side == 0 ? left_knee : right_knee].pose.position;
		const auto foot_position = replay.relations[side == 0 ? left_foot : right_foot].pose.position;
		const auto ankle_position = xrt_vec3{
		        foot_position.x - replay_axis.x * replay_profile.ankle_to_foot[side],
		        foot_position.y - replay_axis.y * replay_profile.ankle_to_foot[side],
		        foot_position.z - replay_axis.z * replay_profile.ankle_to_foot[side],
		};
		assert(std::abs(position_distance(hip_joint, knee_position) - replay_profile.thigh_length[side]) < 0.002f);
		assert(std::abs(position_distance(knee_position, ankle_position) - replay_profile.shin_length[side]) < 0.002f);
	}

	// Rapid surface and manual-pose changes must remain finite and retain one
	// coherent owner. This reproduces the former 0 -> 1500 mm crash sequence.
	for (int frame = 0; frame < 120; ++frame)
	{
		replay_settings.surface = body_surface_mode::bed;
		replay_settings.bed_height_mm = frame % 2 == 0 ? 0 : 1500;
		replay_settings.manual_pose = static_cast<body_pose_kind>(1 + (frame / 10) % 4);
		replay_assistant.configure(replay_settings);
		replay_input.timestamp_ns += 10'000'000;
		replay = replay_assistant.update(replay_input);
		assert(replay.active);
		for (const auto & tracker: replay.relations)
			assert(std::isfinite(tracker.pose.position.x) && std::isfinite(tracker.pose.position.y) &&
			       std::isfinite(tracker.pose.position.z));
	}

	settings.mode = body_pose_assist_mode::off;
	assistant.configure(settings);
	input.timestamp_ns += 150'000'000;
	output = assistant.update(input);
	assert(output.state == body_pose_state::disabled);
	assert(!output.active);
	assert(output.relations[static_cast<size_t>(body_pose_role::hip)].pose.position.z ==
	       input.provider[static_cast<size_t>(body_pose_role::hip)].pose.position.z);
}
