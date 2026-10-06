/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "body_pose_asset.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>

namespace wivrn
{

enum class body_pose_role : uint8_t
{
	chest,
	hip,
	left_knee,
	right_knee,
	left_foot,
	right_foot,
	count,
};

constexpr size_t body_pose_role_count = static_cast<size_t>(body_pose_role::count);

struct body_pose_anchor
{
	xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	bool valid = false;
};

struct body_pose_assist_input
{
	int64_t timestamp_ns = 0;
	float provider_confidence = 0.0f;
	uint16_t body_height_mm = 1700;
	std::array<xrt_space_relation, body_pose_role_count> provider{};
	std::array<bool, body_pose_role_count> provider_valid{};
	body_pose_anchor head;
	body_pose_anchor left_hand;
	body_pose_anchor right_hand;
	std::array<body_pose_anchor, 2> ankles;
};

struct body_pose_assist_output
{
	std::array<xrt_space_relation, body_pose_role_count> relations{};
	std::array<float, body_pose_role_count> confidence{};
	body_pose_state state = body_pose_state::disabled;
	float posture_confidence = 0.0f;
	uint16_t anchor_mask = 0;
	uint64_t fallback_count = 0;
	bool active = false;
};

/**
 * Whole-body pose assistance used by virtual trackers.
 *
 * This is intentionally provider-neutral.  The body tracker maps Meta/FB/BD
 * joint ids to these roles and keeps the OpenXR body output independent from
 * this experimental layer.
 */
class body_pose_assist
{
public:
	body_pose_assist();

	void configure(const body_pose_assist_settings & settings);
	void set_skeleton_profile(const body_pose_skeleton_profile & profile);
	body_pose_assist_output update(const body_pose_assist_input & input);
	void reset();

	const body_pose_assist_output & output() const
	{
		return output_;
	}

private:
	struct solved_body
	{
		xrt_vec3 chest{};
		xrt_vec3 hip{};
		std::array<xrt_vec3, 2> hip_joints{};
		std::array<xrt_vec3, 2> knees{};
		std::array<xrt_vec3, 2> ankles{};
		std::array<xrt_vec3, 2> feet{};
		body_pose_kind kind = body_pose_kind::upright;
		bool valid = false;
	};

	struct contact_lock
	{
		xrt_vec3 ankle{};
		body_pose_kind kind = body_pose_kind::upright;
		bool active = false;
	};

	body_pose_kind classify(const body_pose_assist_input & input, float & confidence) const;
	body_pose_assist_output passthrough(const body_pose_assist_input & input);
	body_pose_assist_output synthesize(const body_pose_assist_input & input,
	                                   body_pose_kind kind,
	                                   float transition,
	                                   float posture_confidence);

	body_pose_assist_settings settings_{};
	body_pose_skeleton_profile skeleton_profile_{};
	body_pose_assist_output output_{};
	solved_body solved_{};
	std::array<contact_lock, 2> contacts_{};
	body_pose_state state_ = body_pose_state::upright;
	body_pose_kind target_kind_ = body_pose_kind::upright;
	body_pose_kind synthesis_kind_ = body_pose_kind::upright;
	body_pose_kind candidate_kind_ = body_pose_kind::upright;
	int64_t candidate_since_ns_ = 0;
	int64_t last_timestamp_ns_ = 0;
	int64_t last_valid_head_ns_ = 0;
	float influence_ = 0.0f;
	float effective_surface_y_ = 0.0f;
	int64_t surface_timestamp_ns_ = 0;
	xrt_vec3 body_axis_{0.0f, 0.0f, 1.0f};
	bool have_body_axis_ = false;
	bool have_effective_surface_ = false;
};

} // namespace wivrn
