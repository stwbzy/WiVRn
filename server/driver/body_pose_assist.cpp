/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "body_pose_assist.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace wivrn
{
namespace
{
constexpr float epsilon = 1.0e-5f;
constexpr float pi = 3.14159265358979323846f;
constexpr uint32_t valid_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
                                 XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;

xrt_vec3 add(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

xrt_vec3 sub(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

xrt_vec3 cross(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {
	        a.y * b.z - a.z * b.y,
	        a.z * b.x - a.x * b.z,
	        a.x * b.y - a.y * b.x,
	};
}

xrt_vec3 mul(const xrt_vec3 & a, float factor)
{
	return {a.x * factor, a.y * factor, a.z * factor};
}

xrt_vec3 negate(const xrt_vec3 & value)
{
	return {-value.x, -value.y, -value.z};
}

xrt_vec3 horizontal(const xrt_vec3 & value)
{
	return {value.x, 0.0f, value.z};
}

float dot(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const xrt_vec3 & value)
{
	return std::sqrt(dot(value, value));
}

xrt_vec3 normalize(const xrt_vec3 & value, xrt_vec3 fallback = {0.0f, 0.0f, 1.0f})
{
	const float norm = length(value);
	return norm > epsilon ? mul(value, 1.0f / norm) : fallback;
}

xrt_quat conjugate(const xrt_quat & value)
{
	return {-value.x, -value.y, -value.z, value.w};
}

xrt_quat multiply(const xrt_quat & a, const xrt_quat & b)
{
	return {
	        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

xrt_quat axis_angle(const xrt_vec3 & axis, float angle)
{
	const float half_angle = angle * 0.5f;
	const float sine = std::sin(half_angle);
	return {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half_angle)};
}

xrt_quat lying_orientation(const xrt_vec3 & body_axis, body_pose_kind kind)
{
	// Give synthesized joints a stable body-relative frame. Copying the HMD
	// orientation made all four manual poses equivalent and could roll every
	// virtual tracker onto the same side.
	const float yaw = std::atan2(-body_axis.x, -body_axis.z);
	const auto heading = axis_angle({0.0f, 1.0f, 0.0f}, yaw);
	float local_rotation = 0.0f;
	xrt_vec3 local_axis{1.0f, 0.0f, 0.0f};
	switch (kind)
	{
		case body_pose_kind::back:
			local_rotation = pi * 0.5f;
			break;
		case body_pose_kind::front:
			local_rotation = -pi * 0.5f;
			break;
		case body_pose_kind::left_side:
			local_axis = {0.0f, 0.0f, 1.0f};
			local_rotation = pi * 0.5f;
			break;
		case body_pose_kind::right_side:
			local_axis = {0.0f, 0.0f, 1.0f};
			local_rotation = -pi * 0.5f;
			break;
		case body_pose_kind::upright:
			break;
	}
	return multiply(heading, axis_angle(local_axis, local_rotation));
}

float support_surface_y(const body_pose_assist_settings & settings, float head_y)
{
	if (settings.surface != body_surface_mode::bed)
		return 0.0f;

	// A stale or mistyped bed value must not place the synthesized lower body
	// above its head anchor. Keep enough clearance for a side-lying pelvis.
	const float requested = settings.bed_height_mm / 1000.0f;
	const float maximum = std::max(0.0f, head_y - 0.24f);
	return std::clamp(requested, 0.0f, maximum);
}

float coherent_surface_y(const body_pose_assist_settings & settings, body_pose_kind kind, float head_y)
{
	float surface_y = support_surface_y(settings, head_y);
	const bool side_pose = kind == body_pose_kind::left_side || kind == body_pose_kind::right_side;
	const float nominal_clearance = kind == body_pose_kind::front ? 0.20f : side_pose ? 0.25f
	                                                                                  : 0.23f;
	const float clearance = head_y - surface_y;
	// Tracking origins and a configured bed height can disagree. Never stretch
	// the torso vertically to bridge that error: keep the template coherent and
	// let it float relative to the requested plane instead of breaking the avatar.
	if (clearance < 0.12f || clearance > 0.52f)
		surface_y = head_y - nominal_clearance;
	return surface_y;
}

xrt_vec3 solve_knee(const xrt_vec3 & hip,
                    const xrt_vec3 & foot,
                    const xrt_vec3 & bend_hint,
                    float thigh_length,
                    float shin_length)
{
	const auto hip_to_foot = sub(foot, hip);
	const float distance = std::clamp(length(hip_to_foot), 0.05f, thigh_length + shin_length - 0.001f);
	const auto direction = normalize(hip_to_foot);
	const float along = (thigh_length * thigh_length - shin_length * shin_length + distance * distance) /
	                    (2.0f * distance);
	const float away = std::sqrt(std::max(0.0f, thigh_length * thigh_length - along * along));
	const auto projected_hint = sub(bend_hint, mul(direction, dot(bend_hint, direction)));
	auto bend = normalize(projected_hint, cross(direction, {1.0f, 0.0f, 0.0f}));
	if (length(bend) <= epsilon)
		bend = {0.0f, 1.0f, 0.0f};
	return add(add(hip, mul(direction, along)), mul(bend, away));
}

xrt_vec3 constrain_endpoint(const xrt_vec3 & root, const xrt_vec3 & endpoint, float minimum, float maximum)
{
	const auto delta = sub(endpoint, root);
	const float distance = length(delta);
	const auto direction = normalize(delta, {0.0f, 0.0f, 1.0f});
	return add(root, mul(direction, std::clamp(distance, minimum, maximum)));
}

xrt_vec3 rotate(const xrt_quat & orientation, const xrt_vec3 & value)
{
	const xrt_quat vector{value.x, value.y, value.z, 0.0f};
	const auto result = multiply(multiply(orientation, vector), conjugate(orientation));
	return {result.x, result.y, result.z};
}

xrt_quat nlerp(const xrt_quat & from, const xrt_quat & to, float alpha)
{
	xrt_quat target = to;
	const float quaternion_dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
	if (quaternion_dot < 0.0f)
		target = {-to.x, -to.y, -to.z, -to.w};
	xrt_quat result{
	        from.x + (target.x - from.x) * alpha,
	        from.y + (target.y - from.y) * alpha,
	        from.z + (target.z - from.z) * alpha,
	        from.w + (target.w - from.w) * alpha,
	};
	const float norm = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
	if (norm > epsilon)
	{
		result.x /= norm;
		result.y /= norm;
		result.z /= norm;
		result.w /= norm;
	}
	return result;
}

xrt_quat rotation_between(const xrt_vec3 & from, const xrt_vec3 & to)
{
	const auto source = normalize(from, {0.0f, -1.0f, 0.0f});
	const auto target = normalize(to, source);
	const float cosine = std::clamp(dot(source, target), -1.0f, 1.0f);
	if (cosine > 1.0f - epsilon)
		return {0.0f, 0.0f, 0.0f, 1.0f};
	if (cosine < -1.0f + epsilon)
	{
		auto axis = normalize(cross(source, {1.0f, 0.0f, 0.0f}), {0.0f, 0.0f, 1.0f});
		if (length(axis) <= epsilon)
			axis = normalize(cross(source, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, 1.0f});
		return axis_angle(axis, pi);
	}

	const auto axis = cross(source, target);
	xrt_quat result{axis.x, axis.y, axis.z, 1.0f + cosine};
	const float norm = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
	return norm > epsilon ? xrt_quat{result.x / norm, result.y / norm, result.z / norm, result.w / norm}
	                      : xrt_quat{0.0f, 0.0f, 0.0f, 1.0f};
}

xrt_quat align_local_axis(const xrt_quat & base, const xrt_vec3 & local_axis, const xrt_vec3 & world_axis)
{
	return multiply(rotation_between(rotate(base, local_axis), world_axis), base);
}

bool valid(const xrt_space_relation & relation)
{
	if ((static_cast<uint32_t>(relation.relation_flags) &
	     (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT)) !=
	    (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT))
		return false;
	const auto & p = relation.pose.position;
	const auto & q = relation.pose.orientation;
	const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(q.x) &&
	       std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) && norm > epsilon;
}

xrt_space_relation make_relation(const xrt_vec3 & position, const xrt_quat & orientation)
{
	return {
	        .relation_flags = static_cast<xrt_space_relation_flags>(valid_flags),
	        .pose = {
	                .orientation = orientation,
	                .position = position,
	        },
	};
}

xrt_space_relation blend_relation(const xrt_space_relation & from, const xrt_space_relation & to, float alpha)
{
	if (!valid(from))
		return to;
	if (!valid(to))
		return from;
	xrt_space_relation result = to;
	result.pose.position = add(mul(from.pose.position, 1.0f - alpha), mul(to.pose.position, alpha));
	result.pose.orientation = nlerp(from.pose.orientation, to.pose.orientation, alpha);
	return result;
}

body_pose_state state_for(body_pose_kind kind)
{
	switch (kind)
	{
		case body_pose_kind::upright:
			return body_pose_state::upright;
		case body_pose_kind::back:
			return body_pose_state::back;
		case body_pose_kind::front:
			return body_pose_state::front;
		case body_pose_kind::left_side:
			return body_pose_state::left_side;
		case body_pose_kind::right_side:
			return body_pose_state::right_side;
	}
	return body_pose_state::uncertain;
}

bool is_lying(body_pose_kind kind)
{
	return kind != body_pose_kind::upright;
}

size_t profile_index(body_pose_kind kind)
{
	switch (kind)
	{
		case body_pose_kind::back:
			return 0;
		case body_pose_kind::front:
			return 1;
		case body_pose_kind::left_side:
			return 2;
		case body_pose_kind::right_side:
			return 3;
		case body_pose_kind::upright:
			break;
	}
	return 0;
}
} // namespace

body_pose_assist::body_pose_assist() = default;

void body_pose_assist::set_skeleton_profile(const body_pose_skeleton_profile & profile)
{
	if (!profile.valid)
		return;
	body_pose_skeleton_profile sanitized = profile;
	sanitized.torso_length = std::clamp(sanitized.torso_length, 0.34f, 0.62f);
	sanitized.pelvis_half_width = std::clamp(sanitized.pelvis_half_width, 0.065f, 0.14f);
	for (size_t side = 0; side < 2; ++side)
	{
		sanitized.thigh_length[side] = std::clamp(sanitized.thigh_length[side], 0.28f, 0.58f);
		sanitized.shin_length[side] = std::clamp(sanitized.shin_length[side], 0.28f, 0.58f);
		sanitized.ankle_to_foot[side] = std::clamp(sanitized.ankle_to_foot[side], 0.035f, 0.16f);
	}
	skeleton_profile_ = sanitized;
}

void body_pose_assist::configure(const body_pose_assist_settings & settings)
{
	const bool asset_changed = settings_.asset != settings.asset;
	const bool preserve_active_pose = settings_.mode != body_pose_assist_mode::off &&
	                                  settings.mode != body_pose_assist_mode::off && last_timestamp_ns_ != 0;
	settings_ = settings;
	settings_.bed_height_mm = std::min<uint16_t>(settings_.bed_height_mm, 1500);
	settings_.transition_ms = std::clamp<uint16_t>(settings_.transition_ms, 50, 2000);
	settings_.lower_body_influence_permille = std::min<uint16_t>(settings_.lower_body_influence_permille, 1000);
	settings_.anchor_influence_permille = std::min<uint16_t>(settings_.anchor_influence_permille, 1000);
	for (auto & profile: settings_.profiles)
	{
		profile.hip_height_mm = std::min<uint16_t>(profile.hip_height_mm, 500);
		profile.knee_bend_deg = std::min<uint16_t>(profile.knee_bend_deg, 150);
		profile.leg_spread_mm = std::min<uint16_t>(profile.leg_spread_mm, 600);
		profile.foot_spread_mm = std::min<uint16_t>(profile.foot_spread_mm, 500);
	}
	if (!preserve_active_pose)
		reset();
	else
	{
		// Sliders and Auto/Manual changes arrive as live settings packets. Preserve
		// tracker ownership and let the normal spatial blend move to the new target.
		candidate_kind_ = target_kind_;
		candidate_since_ns_ = last_timestamp_ns_;
		if (asset_changed)
		{
			influence_ = 0.0f;
			solved_ = {};
			contacts_ = {};
		}
	}
}

void body_pose_assist::reset()
{
	output_ = {};
	output_.state = settings_.mode == body_pose_assist_mode::off ? body_pose_state::disabled : body_pose_state::upright;
	state_ = body_pose_state::upright;
	target_kind_ = body_pose_kind::upright;
	synthesis_kind_ = body_pose_kind::upright;
	candidate_kind_ = body_pose_kind::upright;
	candidate_since_ns_ = 0;
	last_timestamp_ns_ = 0;
	last_valid_head_ns_ = 0;
	influence_ = 0.0f;
	effective_surface_y_ = 0.0f;
	surface_timestamp_ns_ = 0;
	body_axis_ = {0.0f, 0.0f, 1.0f};
	have_body_axis_ = false;
	have_effective_surface_ = false;
	solved_ = {};
	contacts_ = {};
}

body_pose_assist_output body_pose_assist::passthrough(const body_pose_assist_input & input)
{
	body_pose_assist_output result{};
	result.state = settings_.mode == body_pose_assist_mode::off ? body_pose_state::disabled : body_pose_state::upright;
	result.active = false;
	result.posture_confidence = (input.head.valid ? 0.5f : 0.0f) +
	                            (input.left_hand.valid ? 0.25f : 0.0f) +
	                            (input.right_hand.valid ? 0.25f : 0.0f);
	result.fallback_count = output_.fallback_count;
	result.anchor_mask = input.head.valid ? 1u : 0u;
	if (input.left_hand.valid)
		result.anchor_mask |= 1u << 1;
	if (input.right_hand.valid)
		result.anchor_mask |= 1u << 2;
	for (size_t i = 0; i < body_pose_role_count; ++i)
	{
		result.relations[i] = input.provider[i];
		result.confidence[i] = input.provider_valid[i] ? std::clamp(input.provider_confidence, 0.0f, 1.0f) : 0.0f;
	}
	return result;
}

body_pose_kind body_pose_assist::classify(const body_pose_assist_input & input, float & confidence) const
{
	confidence = 0.0f;
	if (!input.head.valid || !valid(input.head.relation))
		return body_pose_kind::upright;

	const float surface_y = support_surface_y(settings_, input.head.relation.pose.position.y);
	const float head_height = input.head.relation.pose.position.y - surface_y;
	const float upright_head_height = std::clamp(input.body_height_mm / 1000.0f * 0.65f, 0.90f, 1.35f);
	const float height_score = std::clamp(1.0f - head_height / upright_head_height, 0.0f, 1.0f);

	float horizontal_torso = 0.0f;
	if (input.provider_valid[static_cast<size_t>(body_pose_role::hip)] &&
	    valid(input.provider[static_cast<size_t>(body_pose_role::hip)]))
	{
		const auto delta = sub(input.head.relation.pose.position,
		                       input.provider[static_cast<size_t>(body_pose_role::hip)].pose.position);
		const float torso_length = length(delta);
		if (torso_length > 0.05f)
			horizontal_torso = std::sqrt(delta.x * delta.x + delta.z * delta.z) / torso_length;
	}
	else if (input.provider_valid[static_cast<size_t>(body_pose_role::chest)] &&
	         valid(input.provider[static_cast<size_t>(body_pose_role::chest)]))
	{
		const auto delta = sub(input.head.relation.pose.position,
		                       input.provider[static_cast<size_t>(body_pose_role::chest)].pose.position);
		const float torso_length = length(delta);
		if (torso_length > 0.05f)
			horizontal_torso = std::sqrt(delta.x * delta.x + delta.z * delta.z) / torso_length;
	}

	const float lying_score = std::clamp(0.65f * height_score + 0.35f * horizontal_torso, 0.0f, 1.0f);
	// Once a lying pose owns the virtual trackers, require substantially stronger
	// evidence before releasing it. This prevents borderline head/hip samples
	// from alternating ownership with the raw FBT provider every frame.
	const float lying_threshold = is_lying(target_kind_) || influence_ > 0.5f ? 0.40f : 0.60f;
	if (lying_score < lying_threshold)
	{
		confidence = std::clamp(1.0f - lying_score, 0.0f, 1.0f);
		return body_pose_kind::upright;
	}

	const auto up = normalize(rotate(input.head.relation.pose.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 1.0f, 0.0f});
	if (up.y > 0.45f)
		return confidence = lying_score, body_pose_kind::back;
	if (up.y < -0.45f)
		return confidence = lying_score, body_pose_kind::front;
	confidence = lying_score;
	return up.x >= 0.0f ? body_pose_kind::left_side : body_pose_kind::right_side;
}

body_pose_assist_output body_pose_assist::synthesize(const body_pose_assist_input & input,
                                                     body_pose_kind kind,
                                                     float transition,
                                                     float posture_confidence)
{
	auto result = output_;
	result.active = true;
	result.posture_confidence = posture_confidence;
	result.state = state_;
	result.anchor_mask = input.head.valid ? 1u : 0u;
	if (input.left_hand.valid)
		result.anchor_mask |= 1u << 1;
	if (input.right_hand.valid)
		result.anchor_mask |= 1u << 2;
	if (!input.head.valid || !valid(input.head.relation) || !is_lying(kind))
		return passthrough(input);

	const auto head_position = input.head.relation.pose.position;
	const xrt_vec3 world_up{0.0f, 1.0f, 0.0f};
	const auto head_forward = horizontal(rotate(input.head.relation.pose.orientation, {0.0f, 0.0f, -1.0f}));
	const auto head_axis = normalize(negate(head_forward), have_body_axis_ ? body_axis_ : xrt_vec3{0.0f, 0.0f, 1.0f});

	xrt_vec3 provider_axis{};
	bool have_provider_axis = false;
	if (input.provider_valid[static_cast<size_t>(body_pose_role::hip)] &&
	    valid(input.provider[static_cast<size_t>(body_pose_role::hip)]))
	{
		provider_axis = horizontal(sub(input.provider[static_cast<size_t>(body_pose_role::hip)].pose.position,
		                               head_position));
		const float provider_distance = length(provider_axis);
		have_provider_axis = provider_distance >= 0.18f && provider_distance <= 1.20f;
		if (have_provider_axis)
			provider_axis = normalize(provider_axis);
	}

	xrt_vec3 hand_axis{};
	bool have_hand_axis = false;
	if (input.left_hand.valid && input.right_hand.valid && valid(input.left_hand.relation) &&
	    valid(input.right_hand.relation))
	{
		const auto hand_span = horizontal(sub(input.right_hand.relation.pose.position,
		                                      input.left_hand.relation.pose.position));
		const float hand_distance = length(hand_span);
		if (hand_distance >= 0.18f && hand_distance <= 1.50f)
		{
			hand_axis = normalize(cross(normalize(hand_span), world_up));
			const auto sign_reference = have_body_axis_ ? body_axis_ : have_provider_axis ? provider_axis
			                                                                              : head_axis;
			if (dot(hand_axis, sign_reference) < 0.0f)
				hand_axis = negate(hand_axis);
			have_hand_axis = true;
		}
	}

	xrt_vec3 axis = have_hand_axis ? hand_axis : have_provider_axis ? provider_axis
	                                                                : head_axis;
	if (have_hand_axis && have_provider_axis && dot(hand_axis, provider_axis) > 0.25f)
		axis = normalize(add(mul(hand_axis, 0.80f), mul(provider_axis, 0.20f)), hand_axis);
	if (have_body_axis_ && dot(axis, body_axis_) < 0.0f)
		axis = negate(axis);
	axis = normalize(axis, have_body_axis_ ? body_axis_ : xrt_vec3{0.0f, 0.0f, 1.0f});
	if (!have_body_axis_)
		body_axis_ = axis;
	else
		body_axis_ = normalize(add(mul(body_axis_, 0.82f), mul(axis, 0.18f)), body_axis_);
	have_body_axis_ = true;
	axis = body_axis_;

	const xrt_vec3 lateral{-axis.z, 0.0f, axis.x};
	const auto & profile = settings_.profiles[profile_index(kind)];
	const float target_surface_y = coherent_surface_y(settings_, kind, head_position.y);
	if (!have_effective_surface_)
	{
		effective_surface_y_ = target_surface_y;
		have_effective_surface_ = true;
	}
	else
	{
		const float elapsed_seconds = surface_timestamp_ns_ == 0
		                                      ? 0.0f
		                                      : std::clamp(float(input.timestamp_ns - surface_timestamp_ns_) / 1.0e9f,
		                                                   0.0f,
		                                                   0.10f);
		const float maximum_step = std::clamp(elapsed_seconds * 0.45f, 0.001f, 0.025f);
		const float delta = std::clamp(target_surface_y - effective_surface_y_, -maximum_step, maximum_step);
		effective_surface_y_ += delta;
	}
	surface_timestamp_ns_ = input.timestamp_ns;
	const float surface_y = effective_surface_y_;

	const float body_height = std::clamp(input.body_height_mm / 1000.0f, 1.20f, 2.30f);
	if (const auto * asset = find_body_pose_asset(settings_.asset, kind))
	{
		body_pose_asset_solve_input solve_input{
		        .head = head_position,
		        .body_forward = axis,
		        .surface_y = surface_y,
		        .body_height_m = body_height,
		        .skeleton = skeleton_profile_,
		        .target_influence = transition * transition * (3.0f - 2.0f * transition),
		};
		const auto set_source = [&](body_pose_role role, xrt_vec3 & position, bool & position_valid) {
			const size_t index = static_cast<size_t>(role);
			if (!input.provider_valid[index] || !valid(input.provider[index]))
				return;
			position = input.provider[index].pose.position;
			position_valid = true;
			solve_input.source_orientations[index] = input.provider[index].pose.orientation;
			solve_input.source_orientations_valid[index] = true;
		};
		set_source(body_pose_role::chest, solve_input.source_chest, solve_input.source_chest_valid);
		set_source(body_pose_role::hip, solve_input.source_hip, solve_input.source_hip_valid);
		for (size_t side = 0; side < 2; ++side)
		{
			const auto knee_role = side == 0 ? body_pose_role::left_knee : body_pose_role::right_knee;
			const auto foot_role = side == 0 ? body_pose_role::left_foot : body_pose_role::right_foot;
			set_source(knee_role, solve_input.source_knees[side], solve_input.source_knees_valid[side]);
			if (input.ankles[side].valid && valid(input.ankles[side].relation))
			{
				solve_input.source_ankles[side] = input.ankles[side].relation.pose.position;
				solve_input.source_ankles_valid[side] = true;
				const size_t foot_index = static_cast<size_t>(foot_role);
				if (input.provider_valid[foot_index] && valid(input.provider[foot_index]))
				{
					solve_input.source_orientations[foot_index] = input.provider[foot_index].pose.orientation;
					solve_input.source_orientations_valid[foot_index] = true;
				}
			}
			else
			{
				set_source(foot_role, solve_input.source_ankles[side], solve_input.source_ankles_valid[side]);
			}
		}
		const auto asset_solution = solve_body_pose_asset(*asset, solve_input);
		if (asset_solution.valid)
		{
			solved_ = {
			        .chest = asset_solution.chest,
			        .hip = asset_solution.hip,
			        .hip_joints = asset_solution.hip_joints,
			        .knees = asset_solution.knees,
			        .ankles = asset_solution.ankles,
			        .feet = asset_solution.feet,
			        .kind = kind,
			        .valid = true,
			};
			contacts_ = {};
			result.relations = {
			        make_relation(asset_solution.chest, asset_solution.chest_orientation),
			        make_relation(asset_solution.hip, asset_solution.hip_orientation),
			        make_relation(asset_solution.knees[0], asset_solution.knee_orientation[0]),
			        make_relation(asset_solution.knees[1], asset_solution.knee_orientation[1]),
			        make_relation(asset_solution.feet[0], asset_solution.foot_orientation[0]),
			        make_relation(asset_solution.feet[1], asset_solution.foot_orientation[1]),
			};
			const float global_quality = std::clamp(input.provider_confidence, 0.0f, 1.0f);
			const float anchor_quality = settings_.anchor_influence_permille / 1000.0f;
			for (size_t i = 0; i < body_pose_role_count; ++i)
			{
				const bool provider_pose_valid = input.provider_valid[i] && valid(input.provider[i]);
				result.confidence[i] = std::clamp(posture_confidence * anchor_quality +
				                                          (provider_pose_valid ? global_quality * 0.15f : 0.0f),
				                                  0.0f,
				                                  1.0f);
				if (!provider_pose_valid && transition > 0.5f)
					++result.fallback_count;
			}
			return result;
		}
	}

	const float fallback_torso_length = std::clamp(body_height * 0.27f, 0.38f, 0.52f);
	const float fallback_thigh_length = std::clamp(body_height * 0.245f, 0.32f, 0.52f);
	const float fallback_shin_length = std::clamp(body_height * 0.246f, 0.32f, 0.52f);
	const float torso_length = skeleton_profile_.valid ? skeleton_profile_.torso_length : fallback_torso_length;
	const std::array<float, 2> thigh_lengths = skeleton_profile_.valid
	                                                   ? skeleton_profile_.thigh_length
	                                                   : std::array<float, 2>{fallback_thigh_length,
	                                                                          fallback_thigh_length};
	const std::array<float, 2> shin_lengths = skeleton_profile_.valid
	                                                  ? skeleton_profile_.shin_length
	                                                  : std::array<float, 2>{fallback_shin_length,
	                                                                         fallback_shin_length};
	const std::array<float, 2> foot_lengths = skeleton_profile_.valid
	                                                  ? skeleton_profile_.ankle_to_foot
	                                                  : std::array<float, 2>{0.08f, 0.08f};
	const float thigh_length = (thigh_lengths[0] + thigh_lengths[1]) * 0.5f;
	const float shin_length = (shin_lengths[0] + shin_lengths[1]) * 0.5f;
	const bool side_pose = kind == body_pose_kind::left_side || kind == body_pose_kind::right_side;
	const float requested_hip_height = profile.hip_height_mm / 1000.0f;
	const float minimum_hip_height = side_pose ? 0.14f : 0.08f;
	const float maximum_hip_height = side_pose ? 0.20f : 0.17f;
	const float anchor_limit = std::max(minimum_hip_height, head_position.y - surface_y - 0.05f);
	const float hip_height = std::min(std::clamp(requested_hip_height, minimum_hip_height, maximum_hip_height), anchor_limit);
	const float requested_hip_y = surface_y + hip_height;
	const float hip_y = std::clamp(requested_hip_y,
	                               head_position.y - torso_length * 0.86f,
	                               head_position.y + torso_length * 0.20f);
	const float vertical_torso = head_position.y - hip_y;
	const float axial_torso = std::sqrt(std::max(0.18f * 0.18f,
	                                             torso_length * torso_length -
	                                                     vertical_torso * vertical_torso));
	const auto hip_position = xrt_vec3{
	        head_position.x + axis.x * axial_torso,
	        hip_y,
	        head_position.z + axis.z * axial_torso,
	};
	const float chest_fraction = 0.48f;
	const float knee_spread = profile.leg_spread_mm / 2000.0f;
	const float foot_spread = profile.foot_spread_mm / 2000.0f;
	const float pelvis_half_width = skeleton_profile_.valid
	                                        ? skeleton_profile_.pelvis_half_width
	                                        : std::clamp(body_height * 0.052f, 0.075f, 0.11f);

	xrt_vec3 left_knee{};
	xrt_vec3 right_knee{};
	xrt_vec3 left_foot{};
	xrt_vec3 right_foot{};
	const auto bent_leg = [&](const xrt_vec3 & hip_joint,
	                          float foot_y,
	                          float foot_lateral,
	                          float flex_degrees,
	                          const xrt_vec3 & bend_hint) {
		const float flex = std::clamp(flex_degrees, 5.0f, 120.0f) * pi / 180.0f;
		const float chord_squared = thigh_length * thigh_length + shin_length * shin_length +
		                            2.0f * thigh_length * shin_length * std::cos(flex);
		const float hip_lateral = dot(sub(hip_joint, hip_position), lateral);
		const float lateral_delta = foot_lateral - hip_lateral;
		const float vertical_delta = foot_y - hip_joint.y;
		const float axial_distance = std::sqrt(std::max(0.04f,
		                                                chord_squared - lateral_delta * lateral_delta -
		                                                        vertical_delta * vertical_delta));
		auto foot = add(add(hip_position, mul(axis, axial_distance)), mul(lateral, foot_lateral));
		foot.y = foot_y;
		return std::pair{solve_knee(hip_joint, foot, bend_hint, thigh_length, shin_length), foot};
	};

	if (kind == body_pose_kind::front)
	{
		// Prone: thighs and knees stay near the support while the lower legs
		// rise. This avoids the impossible raised-knee supine shape on the front.
		const float flex = std::clamp(float(profile.knee_bend_deg) * 0.35f, 5.0f, 55.0f) * pi / 180.0f;
		const auto make_prone_leg = [&](float side) {
			const auto hip_joint = add(hip_position, mul(lateral, pelvis_half_width * side));
			auto desired_knee = add(add(hip_position, mul(axis, thigh_length)), mul(lateral, knee_spread * side));
			desired_knee.y = surface_y + 0.06f;
			auto knee = add(hip_joint, mul(normalize(sub(desired_knee, hip_joint), axis), thigh_length));
			const auto shin_hint = add(add(mul(axis, std::cos(flex)), mul(world_up, std::sin(flex))),
			                           mul(lateral, (foot_spread - knee_spread) * side));
			auto foot = add(knee, mul(normalize(shin_hint, axis), shin_length));
			return std::pair{knee, foot};
		};
		std::tie(left_knee, left_foot) = make_prone_leg(1.0f);
		std::tie(right_knee, right_foot) = make_prone_leg(-1.0f);
	}
	else if (side_pose)
	{
		const bool left_is_lower = kind == body_pose_kind::left_side;
		const float half_stack = std::clamp(pelvis_half_width * 0.90f, 0.065f, 0.10f);
		const float lower_y = std::max(surface_y + 0.055f, hip_position.y - half_stack);
		const float upper_y = std::max(lower_y + 0.13f, hip_position.y + half_stack);
		const float anterior_sign = left_is_lower ? 1.0f : -1.0f;
		const auto bend_hint = normalize(add(mul(lateral, anterior_sign), mul(world_up, 0.08f)));
		const float lower_foot_lateral = anterior_sign * foot_spread * 0.30f;
		const float upper_foot_lateral = anterior_sign * (foot_spread * 0.30f + 0.02f);
		const auto lower_hip = xrt_vec3{hip_position.x, lower_y, hip_position.z};
		const auto upper_hip = xrt_vec3{hip_position.x, upper_y, hip_position.z};
		if (left_is_lower)
		{
			std::tie(left_knee, left_foot) = bent_leg(lower_hip,
			                                          std::max(surface_y + 0.045f, lower_y - 0.01f),
			                                          lower_foot_lateral,
			                                          profile.knee_bend_deg,
			                                          bend_hint);
			std::tie(right_knee, right_foot) = bent_leg(upper_hip,
			                                            upper_y - 0.01f,
			                                            upper_foot_lateral,
			                                            profile.knee_bend_deg,
			                                            bend_hint);
		}
		else
		{
			std::tie(right_knee, right_foot) = bent_leg(lower_hip,
			                                            std::max(surface_y + 0.045f, lower_y - 0.01f),
			                                            lower_foot_lateral,
			                                            profile.knee_bend_deg,
			                                            bend_hint);
			std::tie(left_knee, left_foot) = bent_leg(upper_hip,
			                                          upper_y - 0.01f,
			                                          upper_foot_lateral,
			                                          profile.knee_bend_deg,
			                                          bend_hint);
		}
	}
	else
	{
		const auto left_hint = normalize(add(world_up, mul(lateral, knee_spread * 3.0f)));
		const auto right_hint = normalize(add(world_up, mul(lateral, -knee_spread * 3.0f)));
		const auto left_hip = add(hip_position, mul(lateral, pelvis_half_width));
		const auto right_hip = add(hip_position, mul(lateral, -pelvis_half_width));
		std::tie(left_knee, left_foot) = bent_leg(left_hip,
		                                          surface_y + 0.045f,
		                                          foot_spread,
		                                          profile.knee_bend_deg,
		                                          left_hint);
		std::tie(right_knee, right_foot) = bent_leg(right_hip,
		                                            surface_y + 0.045f,
		                                            -foot_spread,
		                                            profile.knee_bend_deg,
		                                            right_hint);
	}

	// The provider is an observation of the skeleton, not six independent final
	// trackers. Fuse pelvis and ankle targets first, then project both legs back
	// onto fixed-length chains. This is the key invariant that prevents the raw
	// FBT estimate and the lying template from fighting each other.
	const float global_quality = std::clamp(input.provider_confidence, 0.0f, 1.0f);
	const float lower_influence = settings_.lower_body_influence_permille / 1000.0f;
	const auto bounded_position = [](const xrt_vec3 & target, const xrt_vec3 & observation, float limit) {
		const auto delta = sub(observation, target);
		const float distance = length(delta);
		return distance > limit ? add(target, mul(delta, limit / distance)) : observation;
	};
	const auto fuse_position = [&](const xrt_vec3 & target,
	                               const body_pose_anchor & observation,
	                               float weight,
	                               float limit) {
		if (!observation.valid || !valid(observation.relation))
			return target;
		const auto bounded = bounded_position(target, observation.relation.pose.position, limit);
		return add(mul(target, 1.0f - weight), mul(bounded, weight));
	};

	body_pose_anchor provider_hip{};
	const size_t hip_role = static_cast<size_t>(body_pose_role::hip);
	if (input.provider_valid[hip_role])
		provider_hip = {.relation = input.provider[hip_role], .valid = true};
	const float torso_weight = std::max(lower_influence, 0.25f) * global_quality;
	auto solved_hip = fuse_position(hip_position, provider_hip, torso_weight, 0.045f);
	auto solved_chest = add(head_position, mul(sub(solved_hip, head_position), chest_fraction));

	std::array<xrt_vec3, 2> target_ankles{left_foot, right_foot};
	for (size_t side = 0; side < 2; ++side)
	{
		body_pose_anchor ankle_observation = input.ankles[side];
		const size_t foot_role = static_cast<size_t>(side == 0 ? body_pose_role::left_foot
		                                                       : body_pose_role::right_foot);
		if ((!ankle_observation.valid || !valid(ankle_observation.relation)) && input.provider_valid[foot_role] &&
		    valid(input.provider[foot_role]))
		{
			ankle_observation = {.relation = input.provider[foot_role], .valid = true};
			ankle_observation.relation.pose.position =
			        sub(ankle_observation.relation.pose.position, mul(axis, foot_lengths[side]));
		}
		const float entry_weight = solved_.valid ? 0.0f : (1.0f - transition) * 0.35f;
		const float observation_weight = std::max(lower_influence * global_quality, entry_weight);
		target_ankles[side] = fuse_position(target_ankles[side], ankle_observation, observation_weight, 0.10f);

		if (!contacts_[side].active || contacts_[side].kind != kind)
		{
			contacts_[side] = {.ankle = target_ankles[side], .kind = kind, .active = true};
		}
		else
		{
			const float drift = length(sub(target_ankles[side], contacts_[side].ankle));
			if (drift <= 0.18f)
				target_ankles[side] = contacts_[side].ankle;
			else
				contacts_[side].ankle = target_ankles[side];
		}
	}

	if (solved_.valid)
	{
		// A posture switch is blended in joint space as well. The leg chains are
		// reprojected immediately below, so even the transition itself is a valid
		// body rather than a cross-fade between unrelated tracker transforms.
		const float smoothing = solved_.kind == kind ? 0.22f : 0.12f;
		solved_hip = add(mul(solved_.hip, 1.0f - smoothing), mul(solved_hip, smoothing));
		solved_chest = add(mul(solved_.chest, 1.0f - smoothing), mul(solved_chest, smoothing));
		for (size_t side = 0; side < 2; ++side)
			target_ankles[side] = add(mul(solved_.ankles[side], 1.0f - smoothing),
			                          mul(target_ankles[side], smoothing));
	}
	solved_hip.y = std::clamp(solved_hip.y, surface_y + 0.04f, head_position.y + 0.005f);
	solved_chest = add(head_position, mul(sub(solved_hip, head_position), chest_fraction));
	for (auto & ankle: target_ankles)
		ankle.y = std::clamp(ankle.y, surface_y + 0.025f, head_position.y + 0.005f);

	std::array<xrt_vec3, 2> hip_joints{
	        add(solved_hip, mul(lateral, pelvis_half_width)),
	        add(solved_hip, mul(lateral, -pelvis_half_width)),
	};
	if (side_pose)
	{
		const bool left_is_lower = kind == body_pose_kind::left_side;
		const float half_stack = std::clamp(pelvis_half_width * 0.90f, 0.065f, 0.10f);
		const float lower_y = std::max(surface_y + 0.055f, solved_hip.y - half_stack);
		const float upper_y = std::max(lower_y + 0.13f, solved_hip.y + half_stack);
		hip_joints[0] = {solved_hip.x, left_is_lower ? lower_y : upper_y, solved_hip.z};
		hip_joints[1] = {solved_hip.x, left_is_lower ? upper_y : lower_y, solved_hip.z};
	}

	const std::array<xrt_vec3, 2> canonical_knees{left_knee, right_knee};
	std::array<xrt_vec3, 2> solved_knees{};
	std::array<xrt_vec3, 2> solved_feet{};
	for (size_t side = 0; side < 2; ++side)
	{
		const float maximum_reach = thigh_lengths[side] + shin_lengths[side] - 0.004f;
		const float minimum_reach = std::abs(thigh_lengths[side] - shin_lengths[side]) + 0.04f;
		target_ankles[side] = constrain_endpoint(hip_joints[side],
		                                         target_ankles[side],
		                                         minimum_reach,
		                                         maximum_reach);
		const auto bend_hint = sub(canonical_knees[side], hip_joints[side]);
		solved_knees[side] = solve_knee(hip_joints[side],
		                                target_ankles[side],
		                                bend_hint,
		                                thigh_lengths[side],
		                                shin_lengths[side]);
		if (solved_knees[side].y > head_position.y + 0.015f ||
		    solved_knees[side].y < surface_y - 0.025f)
		{
			// Search the nearest valid pole direction instead of clipping the final
			// tracker, which would silently change a bone length.
			const bool above = solved_knees[side].y > head_position.y + 0.015f;
			const xrt_vec3 corrective_pole = above ? xrt_vec3{0.0f, -1.0f, 0.0f}
			                                       : xrt_vec3{0.0f, 1.0f, 0.0f};
			auto best = solved_knees[side];
			float best_error = above ? best.y - (head_position.y + 0.015f)
			                         : (surface_y - 0.025f) - best.y;
			for (int step = 1; step <= 32; ++step)
			{
				const float amount = float(step) / 32.0f;
				const auto pole = add(mul(bend_hint, 1.0f - amount), mul(corrective_pole, amount));
				const auto candidate = solve_knee(hip_joints[side],
				                                  target_ankles[side],
				                                  pole,
				                                  thigh_lengths[side],
				                                  shin_lengths[side]);
				const float error = candidate.y > head_position.y + 0.015f
				                            ? candidate.y - (head_position.y + 0.015f)
				                    : candidate.y < surface_y - 0.025f ? (surface_y - 0.025f) - candidate.y
				                                                       : 0.0f;
				if (error < best_error)
				{
					best = candidate;
					best_error = error;
				}
				if (error == 0.0f)
					break;
			}
			solved_knees[side] = best;
		}
		solved_feet[side] = add(target_ankles[side], mul(axis, foot_lengths[side]));
	}

	solved_ = {
	        .chest = solved_chest,
	        .hip = solved_hip,
	        .hip_joints = hip_joints,
	        .knees = solved_knees,
	        .ankles = target_ankles,
	        .feet = solved_feet,
	        .kind = kind,
	        .valid = true,
	};

	const auto torso_orientation = lying_orientation(axis, kind);
	const auto heading_orientation = lying_orientation(axis, body_pose_kind::upright);
	const auto left_leg_orientation = align_local_axis(torso_orientation,
	                                                   {0.0f, -1.0f, 0.0f},
	                                                   sub(target_ankles[0], solved_knees[0]));
	const auto right_leg_orientation = align_local_axis(torso_orientation,
	                                                    {0.0f, -1.0f, 0.0f},
	                                                    sub(target_ankles[1], solved_knees[1]));
	const auto left_foot_orientation = align_local_axis(side_pose ? torso_orientation : heading_orientation,
	                                                    {0.0f, 0.0f, -1.0f},
	                                                    sub(solved_feet[0], target_ankles[0]));
	const auto right_foot_orientation = align_local_axis(side_pose ? torso_orientation : heading_orientation,
	                                                     {0.0f, 0.0f, -1.0f},
	                                                     sub(solved_feet[1], target_ankles[1]));

	result.relations = {
	        make_relation(solved_chest, torso_orientation),
	        make_relation(solved_hip, torso_orientation),
	        make_relation(solved_knees[0], left_leg_orientation),
	        make_relation(solved_knees[1], right_leg_orientation),
	        make_relation(solved_feet[0], left_foot_orientation),
	        make_relation(solved_feet[1], right_foot_orientation),
	};
	const float anchor_quality = settings_.anchor_influence_permille / 1000.0f;
	for (size_t i = 0; i < body_pose_role_count; ++i)
	{
		const bool provider_pose_valid = input.provider_valid[i] && valid(input.provider[i]);
		result.confidence[i] = std::clamp(posture_confidence * anchor_quality +
		                                          (provider_pose_valid ? global_quality * 0.15f : 0.0f),
		                                  0.0f,
		                                  1.0f);
		if (!provider_pose_valid && transition > 0.5f)
			++result.fallback_count;
	}
	return result;
}

body_pose_assist_output body_pose_assist::update(const body_pose_assist_input & input)
{
	if (settings_.mode == body_pose_assist_mode::off)
	{
		last_timestamp_ns_ = std::max(last_timestamp_ns_, input.timestamp_ns);
		output_ = passthrough(input);
		return output_;
	}
	if (last_timestamp_ns_ != 0 && input.timestamp_ns <= last_timestamp_ns_)
	{
		// UDP can duplicate or reorder body samples. Returning passthrough here
		// relinquishes tracker ownership for a single frame and causes a visible
		// snap. Keep the last committed output until a newer sample arrives.
		return output_;
	}

	const int64_t previous_timestamp_ns = last_timestamp_ns_;
	last_timestamp_ns_ = input.timestamp_ns;
	const int64_t elapsed_ns = previous_timestamp_ns == 0 ? 0 : input.timestamp_ns - previous_timestamp_ns;
	const int64_t transition_ns = static_cast<int64_t>(settings_.transition_ms) * 1'000'000;
	const bool head_valid = input.head.valid && valid(input.head.relation);
	if (head_valid)
		last_valid_head_ns_ = input.timestamp_ns;

	float posture_confidence = 0.0f;
	body_pose_kind detected = settings_.mode == body_pose_assist_mode::manual
	                                  ? settings_.manual_pose
	                                  : classify(input, posture_confidence);
	if (settings_.mode == body_pose_assist_mode::manual)
		posture_confidence = (input.head.valid ? 0.5f : 0.0f) + (input.left_hand.valid ? 0.25f : 0.0f) +
		                     (input.right_hand.valid ? 0.25f : 0.0f);
	else if (!head_valid && output_.active && last_valid_head_ns_ != 0 &&
	         input.timestamp_ns - last_valid_head_ns_ <= std::max<int64_t>(transition_ns * 2, 500'000'000))
	{
		// A brief anchor dropout is not posture evidence. Preserve ownership while
		// Quest reacquires the head pose instead of starting an upright transition.
		detected = target_kind_;
		posture_confidence = output_.posture_confidence;
	}

	if (detected != candidate_kind_)
	{
		candidate_kind_ = detected;
		candidate_since_ns_ = input.timestamp_ns;
	}

	// Returning to the committed pose cancels a transient candidate immediately.
	// Manual mode is already an explicit decision and does not need classification
	// debounce; its spatial blend still uses transition_ms below.
	if (detected == target_kind_)
	{
		candidate_kind_ = target_kind_;
		candidate_since_ns_ = input.timestamp_ns;
	}
	else if (settings_.mode == body_pose_assist_mode::manual ||
	         input.timestamp_ns - candidate_since_ns_ >= transition_ns)
	{
		target_kind_ = candidate_kind_;
		if (is_lying(target_kind_))
			synthesis_kind_ = target_kind_;
	}

	const float desired_influence = is_lying(target_kind_) ? 1.0f : 0.0f;
	const float influence_step = transition_ns == 0
	                                     ? 1.0f
	                                     : std::clamp(float(elapsed_ns) / float(transition_ns), 0.0f, 1.0f);
	if (influence_ < desired_influence)
		influence_ = std::min(desired_influence, influence_ + influence_step);
	else if (influence_ > desired_influence)
		influence_ = std::max(desired_influence, influence_ - influence_step);

	const bool entry_pending = !is_lying(target_kind_) && is_lying(candidate_kind_) && candidate_kind_ != target_kind_;
	if (influence_ > 0.001f && influence_ < 0.999f)
		state_ = body_pose_state::transitioning;
	else if (entry_pending)
		state_ = body_pose_state::transitioning;
	else
		state_ = state_for(target_kind_);

	if (influence_ <= 0.001f || !is_lying(synthesis_kind_))
	{
		output_ = passthrough(input);
		output_.state = state_;
		output_.posture_confidence = posture_confidence;
		return output_;
	}

	if (head_valid)
	{
		output_ = synthesize(input, synthesis_kind_, influence_, posture_confidence);
	}
	else
	{
		// Keep the last assisted relation during a short head dropout. If automatic
		// mode eventually commits to upright, influence_ blends it back to whatever
		// provider relations remain valid rather than switching ownership abruptly.
		auto held = output_;
		held.anchor_mask = (input.left_hand.valid ? 1u << 1 : 0u) |
		                   (input.right_hand.valid ? 1u << 2 : 0u);
		held.posture_confidence = posture_confidence;
		for (size_t i = 0; i < body_pose_role_count; ++i)
		{
			if (input.provider_valid[i] && valid(input.provider[i]) && valid(output_.relations[i]))
				held.relations[i] = blend_relation(input.provider[i], output_.relations[i], influence_);
			held.confidence[i] *= influence_;
		}
		output_ = held;
	}
	output_.state = state_;
	output_.active = influence_ > 0.01f;
	return output_;
}

} // namespace wivrn
