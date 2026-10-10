/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "body_motion_classifier.h"

#include <algorithm>
#include <cmath>

namespace wivrn
{
namespace
{
constexpr int64_t classification_hold_ns = 400'000'000;

float distance(const xrt_vec3 & a, const xrt_vec3 & b)
{
	const float x = a.x - b.x;
	const float y = a.y - b.y;
	const float z = a.z - b.z;
	const float result = std::sqrt(x * x + y * y + z * z);
	return std::isfinite(result) ? result : 0.0f;
}

float quaternion_angle(const xrt_quat & a, const xrt_quat & b)
{
	const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	if (!std::isfinite(dot))
		return 0.0f;
	return 2.0f * std::acos(std::clamp(dot, 0.0f, 1.0f));
}

float speed(const body_pose_anchor & current, const body_pose_anchor & previous, float dt)
{
	if (!current.valid || !previous.valid)
		return 0.0f;
	return distance(current.relation.pose.position, previous.relation.pose.position) / dt;
}

} // namespace

body_motion_state body_motion_classifier::classify(float body_speed,
                                                   float hand_speed,
                                                   float head_vertical_speed,
                                                   float head_angular_speed) const
{
	// Sprint is identified by translational/cadence energy. Flow catches broad
	// gestures and spins without mistaking a quiet seated user for a runner.
	if (body_speed >= 1.10f || (head_vertical_speed >= 0.45f && body_speed >= 0.55f))
		return body_motion_state::sprint;
	if (head_angular_speed >= 2.0f || hand_speed >= 1.15f || body_speed >= 0.65f)
		return body_motion_state::flow;
	return body_motion_state::chill;
}

body_motion_state body_motion_classifier::update(const body_motion_sample & sample)
{
	if (!have_previous_ || sample.timestamp_ns <= previous_.timestamp_ns)
	{
		previous_ = sample;
		have_previous_ = true;
		return state_;
	}
	// A resumed stream can have a large timestamp gap. Do not turn that gap
	// into a synthetic sprint/flow burst based on one stale sample.
	if (sample.timestamp_ns - previous_.timestamp_ns > 500'000'000)
	{
		previous_ = sample;
		candidate_ = state_;
		candidate_since_ns_ = sample.timestamp_ns;
		return state_;
	}

	const float dt = std::clamp(float(sample.timestamp_ns - previous_.timestamp_ns) / 1.0e9f, 0.001f, 0.20f);
	float body_speed = 0.0f;
	for (size_t i = 0; i < body_pose_role_count; ++i)
		if (sample.relation_valid[i] && previous_.relation_valid[i])
			body_speed = std::max(body_speed,
			                      distance(sample.relations[i].pose.position, previous_.relations[i].pose.position) / dt);
	const float head_speed = speed(sample.head, previous_.head, dt);
	const float left_hand_speed = speed(sample.left_hand, previous_.left_hand, dt);
	const float right_hand_speed = speed(sample.right_hand, previous_.right_hand, dt);
	const float hand_speed = std::max(left_hand_speed, right_hand_speed);
	const float head_vertical_speed = sample.head.valid && previous_.head.valid
	                                          ? std::abs(sample.head.relation.pose.position.y - previous_.head.relation.pose.position.y) / dt
	                                          : 0.0f;
	const float head_angular_speed = sample.head.valid && previous_.head.valid
	                                         ? quaternion_angle(sample.head.relation.pose.orientation,
	                                                            previous_.head.relation.pose.orientation) /
	                                                   dt
	                                         : 0.0f;

	const auto detected = classify(std::max(body_speed, head_speed), hand_speed, head_vertical_speed, head_angular_speed);
	const float evidence = detected == body_motion_state::sprint
	                               ? std::clamp(std::max(body_speed / 1.10f, head_vertical_speed / 0.45f), 0.0f, 1.0f)
	                       : detected == body_motion_state::flow
	                               ? std::clamp(std::max({body_speed / 0.65f, head_angular_speed / 2.0f, hand_speed / 1.15f}), 0.0f, 1.0f)
	                               : std::clamp(1.0f - std::max(body_speed / 0.65f, hand_speed / 1.15f), 0.0f, 1.0f);
	confidence_ = 0.75f * confidence_ + 0.25f * evidence;
	if (detected != candidate_)
	{
		candidate_ = detected;
		candidate_since_ns_ = sample.timestamp_ns;
	}
	if (state_ == body_motion_state::unknown ||
	    (detected != state_ && sample.timestamp_ns - candidate_since_ns_ >= classification_hold_ns))
		state_ = candidate_;

	previous_ = sample;
	return state_;
}

void body_motion_classifier::reset()
{
	previous_ = {};
	state_ = body_motion_state::unknown;
	candidate_ = body_motion_state::unknown;
	candidate_since_ns_ = 0;
	confidence_ = 0.0f;
	have_previous_ = false;
}

} // namespace wivrn
