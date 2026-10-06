/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "body_pose_assist.h"

#include <array>
#include <cstdint>

namespace wivrn
{

enum class body_motion_state : uint8_t
{
	unknown,
	chill,
	sprint,
	flow,
};

struct body_motion_sample
{
	int64_t timestamp_ns = 0;
	std::array<xrt_space_relation, body_pose_role_count> relations{};
	std::array<bool, body_pose_role_count> relation_valid{};
	body_pose_anchor head;
	body_pose_anchor left_hand;
	body_pose_anchor right_hand;
};

/** Classifies movement intent with hysteresis so profiles cannot flap per frame. */
class body_motion_classifier
{
public:
	body_motion_state update(const body_motion_sample & sample);
	void reset();

	body_motion_state state() const
	{
		return state_;
	}

	float confidence() const
	{
		return confidence_;
	}

private:
	body_motion_state classify(float body_speed, float hand_speed, float head_vertical_speed, float head_angular_speed) const;

	body_motion_sample previous_{};
	body_motion_state state_ = body_motion_state::unknown;
	body_motion_state candidate_ = body_motion_state::unknown;
	int64_t candidate_since_ns_ = 0;
	float confidence_ = 0.0f;
	bool have_previous_ = false;
};

} // namespace wivrn
