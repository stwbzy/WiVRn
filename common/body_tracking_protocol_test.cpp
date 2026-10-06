/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
using namespace wivrn;

template <typename T>
T roundtrip(const T & value)
{
	serialization_packet serialized;
	serialized.serialize(value);

	std::vector<uint8_t> bytes;
	for (const auto span: static_cast<std::vector<std::span<uint8_t>> &>(serialized))
		bytes.insert(bytes.end(), span.begin(), span.end());

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()], std::default_delete<uint8_t[]>());
	std::copy(bytes.begin(), bytes.end(), memory.get());
	deserialization_packet input(memory, std::span<uint8_t>(memory.get(), bytes.size()));
	return input.deserialize<T>();
}

void test_settings()
{
	from_headset::settings_changed input{};
	input.body_tracking = {
	        .profile = body_tracking_profile::custom,
	        .motion_profile = body_motion_profile::flow,
	        .fidelity = body_tracking_fidelity::high,
	        .calibration_mode = body_tracking_calibration_mode::manual,
	        .supervisor = body_tracking_supervisor::server,
	        .supervisor_path = body_tracking_supervisor_path::raw,
	        .smoothing_position_permille = 450,
	        .smoothing_rotation_permille = 350,
	        .confidence_threshold_permille = 700,
	        .dropout_hold_ms = 250,
	        .recovery_samples = 5,
	        .prediction_ms = 8,
	        .body_height_mm = 1780,
	        .floor_assist = true,
	        .micro_motion_threshold_mm = 9,
	};
	input.body_tracking.pose_assist.mode = body_pose_assist_mode::manual;
	input.body_tracking.pose_assist.manual_pose = body_pose_kind::left_side;
	input.body_tracking.pose_assist.surface = body_surface_mode::bed;
	input.body_tracking.pose_assist.asset = body_pose_asset_selection::unity_animation_25;
	input.body_tracking.pose_assist.bed_height_mm = 520;
	input.body_tracking.pose_assist.transition_ms = 700;
	input.body_tracking.pose_assist.lower_body_influence_permille = 180;
	input.body_tracking.pose_assist.anchor_influence_permille = 920;
	input.body_tracking.pose_assist.profiles[2].knee_bend_deg = 42;

	const auto output = roundtrip(input);
	assert(output.body_tracking.profile == body_tracking_profile::custom);
	assert(output.body_tracking.motion_profile == body_motion_profile::flow);
	assert(output.body_tracking.fidelity == body_tracking_fidelity::high);
	assert(output.body_tracking.calibration_mode == body_tracking_calibration_mode::manual);
	assert(output.body_tracking.supervisor_path == body_tracking_supervisor_path::raw);
	assert(output.body_tracking.body_height_mm == 1780);
	assert(output.body_tracking.floor_assist);
	assert(output.body_tracking.micro_motion_threshold_mm == 9);
	assert(output.body_tracking.pose_assist.mode == body_pose_assist_mode::manual);
	assert(output.body_tracking.pose_assist.manual_pose == body_pose_kind::left_side);
	assert(output.body_tracking.pose_assist.surface == body_surface_mode::bed);
	assert(output.body_tracking.pose_assist.asset == body_pose_asset_selection::unity_animation_25);
	assert(output.body_tracking.pose_assist.bed_height_mm == 520);
	assert(output.body_tracking.pose_assist.transition_ms == 700);
	assert(output.body_tracking.pose_assist.lower_body_influence_permille == 180);
	assert(output.body_tracking.pose_assist.anchor_influence_permille == 920);
	assert(output.body_tracking.pose_assist.profiles[2].knee_bend_deg == 42);
}

void test_reset_and_epoch()
{
	from_headset::body_tracking_reset_request request{
	        .request_id = 42,
	        .reset = body_tracking_reset::pose_alignment,
	        .body_epoch = 17,
	        .connection_generation = 9,
	};
	const auto request_copy = roundtrip(request);
	assert(request_copy.request_id == 42);
	assert(request_copy.body_epoch == 17);
	assert(request_copy.connection_generation == 9);
	assert(request_copy.reset == body_tracking_reset::pose_alignment);

	to_headset::body_tracking_reset_ack ack{
	        .request_id = request.request_id,
	        .reset = request.reset,
	        .accepted = true,
	        .body_epoch = 18,
	        .connection_generation = request.connection_generation,
	};
	const auto ack_copy = roundtrip(ack);
	assert(ack_copy.request_id == 42);
	assert(ack_copy.accepted);
	assert(ack_copy.body_epoch == 18);
}

void test_status_defaults()
{
	const auto status = roundtrip(to_headset::body_tracking_status{});
	assert(status.profile == body_tracking_profile::balanced);
	assert(status.calibration_state == body_tracking_calibration_state::unknown);
	assert(status.supervisor == body_tracking_supervisor::off);
	assert(status.supervisor_state == body_tracking_supervisor_state::disabled);
	assert(status.supervisor_reason == body_tracking_supervisor_reason::none);
	assert(status.accepted == 0);
	assert(status.rejected == 0);
	assert(status.held == 0);
	assert(status.body_epoch == 0);
	assert(status.connection_generation == 0);
	assert(status.pose_state == body_pose_state::disabled);
	assert(status.pose_confidence_permille == 0);
	assert(status.pose_anchor_mask == 0);
	assert(status.pose_fallbacks == 0);
}

void test_effective_body_metadata()
{
	from_headset::meta_body input{};
	input.body_epoch = 4;
	input.connection_generation = 11;
	input.calibration_state = body_tracking_calibration_state::valid;
	input.fidelity = body_tracking_fidelity::high;

	const auto output = roundtrip(input);
	assert(output.body_epoch == 4);
	assert(output.connection_generation == 11);
	assert(output.calibration_state == body_tracking_calibration_state::valid);
	assert(output.fidelity == body_tracking_fidelity::high);
}
} // namespace

int main()
{
	test_settings();
	test_reset_and_epoch();
	test_status_defaults();
	test_effective_body_metadata();
}
