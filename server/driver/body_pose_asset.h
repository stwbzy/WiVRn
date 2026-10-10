/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wivrn
{

struct body_pose_skeleton_profile
{
	bool valid = false;
	float torso_length = 0.46f;
	float pelvis_half_width = 0.09f;
	std::array<float, 2> thigh_length{0.42f, 0.42f};
	std::array<float, 2> shin_length{0.42f, 0.42f};
	std::array<float, 2> ankle_to_foot{0.08f, 0.08f};
};

struct body_pose_asset_root
{
	xrt_vec3 head{};
	xrt_vec3 hip{};
	xrt_vec3 chest{};
	xrt_vec3 chest_forward{};
	xrt_vec3 hip_forward{};
	float head_surface_blend = 0.65f;
};

struct body_pose_asset_leg
{
	xrt_vec3 ankle{};
	xrt_vec3 foot_direction{0.0f, 0.0f, 1.0f};
	xrt_vec3 bend_pole{};
	bool contact_enabled = false;
};

struct body_pose_asset
{
	std::string uuid;
	std::string name;
	body_pose_kind category = body_pose_kind::upright;
	float reference_height_m = 1.7f;
	body_pose_asset_root root{};
	std::array<body_pose_asset_leg, 2> legs{};
};

struct body_pose_asset_solution
{
	xrt_vec3 chest{};
	xrt_vec3 hip{};
	std::array<xrt_vec3, 2> hip_joints{};
	std::array<xrt_vec3, 2> knees{};
	std::array<xrt_vec3, 2> ankles{};
	std::array<xrt_vec3, 2> feet{};
	xrt_quat chest_orientation{0.0f, 0.0f, 0.0f, 1.0f};
	xrt_quat hip_orientation{0.0f, 0.0f, 0.0f, 1.0f};
	std::array<xrt_quat, 2> knee_orientation{};
	std::array<xrt_quat, 2> foot_orientation{};
	bool valid = false;
};

struct body_pose_asset_solve_input
{
	xrt_vec3 head{};
	xrt_vec3 body_forward{0.0f, 0.0f, 1.0f};
	float surface_y = 0.0f;
	float body_height_m = 1.7f;
	body_pose_skeleton_profile skeleton{};
	float target_influence = 1.0f;
	xrt_vec3 source_chest{};
	xrt_vec3 source_hip{};
	std::array<xrt_vec3, 2> source_knees{};
	std::array<xrt_vec3, 2> source_ankles{};
	bool source_chest_valid = false;
	bool source_hip_valid = false;
	std::array<bool, 2> source_knees_valid{};
	std::array<bool, 2> source_ankles_valid{};
	// chest, hip, left/right knee, left/right foot
	std::array<xrt_quat, 6> source_orientations{};
	std::array<bool, 6> source_orientations_valid{};
};

struct body_pose_catalog_metrics
{
	size_t loaded = 0;
	size_t rejected = 0;
	std::vector<std::string> issues;
};

const body_pose_asset * find_body_pose_asset(body_pose_asset_selection selection, body_pose_kind category);
const body_pose_catalog_metrics & body_pose_catalog_status();
body_pose_asset_solution solve_body_pose_asset(const body_pose_asset & asset,
                                               const body_pose_asset_solve_input & input);

} // namespace wivrn
