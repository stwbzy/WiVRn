/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"
#include "spatial_upscaling.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("spatial upscaling configuration test check failed");
}
} // namespace

int main()
{
	using enum wivrn::from_headset::spatial_upscaling_mode;
	wivrn::from_headset::headset_info_packet info{};
	check(info.spatial_upscaling == off);
	info.native_eye_width = 2000;
	info.native_eye_height = 2200;
	info.target_eye_width = 2000;
	info.target_eye_height = 2200;
	info.render_eye_width = 1600;
	info.render_eye_height = 1760;
	info.stream_eye_width = 960;
	info.stream_eye_height = 1056;

	// Off and all headset modes keep the server stream at game resolution.
	for (auto mode: {off, sgsr1_headset, fsr1_headset, nis_headset})
	{
		info.spatial_upscaling = mode;
		check((wivrn::fsr_output_dimensions(info) == wivrn::spatial_extent{1600, 1760}));
		check((wivrn::fsr_stream_dimensions(info) == wivrn::spatial_extent{960, 1056}));
		check(!wivrn::fsr_upscaling_enabled(info));
	}

	// PC FSR reconstructs before foveation, so stream dimensions scale with it.
	info.spatial_upscaling = fsr1_pc;
	check((wivrn::fsr_output_dimensions(info) == wivrn::spatial_extent{2000, 2200}));
	check((wivrn::fsr_stream_dimensions(info) == wivrn::spatial_extent{1200, 1320}));
	check((wivrn::fsr_encoder_dimensions(info) == std::array<uint16_t, 2>{1216, 1344}));
	check(wivrn::fsr_upscaling_enabled(info));
	check(std::abs(wivrn::sanitize_fsr_sharpness(0.2f) - 0.2f) < 0.001f);
	check(wivrn::sanitize_fsr_sharpness(-1.0f) == 0.0f);
	check(wivrn::sanitize_fsr_sharpness(9.0f) == 2.0f);
	check(wivrn::sanitize_fsr_sharpness(std::numeric_limits<float>::quiet_NaN()) == 0.2f);

	// Equal targets and downscales always select the existing path.
	info.target_eye_width = info.render_eye_width;
	info.target_eye_height = info.render_eye_height;
	check(!wivrn::fsr_upscaling_enabled(info));
	info.target_eye_width = 1200;
	info.target_eye_height = 1300;
	check(!wivrn::fsr_upscaling_enabled(info));

	// More than 2x on either axis is outside all selected algorithms' contract.
	info.target_eye_width = 3201;
	info.target_eye_height = 2200;
	check(!wivrn::fsr_upscaling_enabled(info));
	check(!wivrn::valid_upscaling_extent(sgsr1_headset, {1600, 1760}, {3201, 2200}));

	// Encoder alignment and invalid arguments.
	check((wivrn::align_video_extent({1, 65}, 64) == wivrn::spatial_extent{64, 128}));
	bool rejected_alignment = false;
	try
	{
		(void)wivrn::fsr_encoder_dimensions(info, 0);
	}
	catch (const std::invalid_argument &)
	{
		rejected_alignment = true;
	}
	check(rejected_alignment);

	// Automatic bitrate follows actual encoded area and respects both limits.
	check(wivrn::automatic_bitrate(50'000'000, {1000, 1000}, {2000, 2000}) == 12'500'000);
	check(wivrn::automatic_bitrate(50'000'000, {100, 100}, {2000, 2000}) == 5'000'000);
	check(wivrn::automatic_bitrate(50'000'000, {3000, 3000}, {2000, 2000}) == 50'000'000);
	check(wivrn::automatic_bitrate(4'000'000, {100, 100}, {2000, 2000}) == 4'000'000);
	wivrn::from_headset::settings_changed settings{
	        .bitrate_bps = 50'000'000,
	        .effective_bitrate_bps = 12'500'000,
	        .bitrate = wivrn::from_headset::bitrate_mode::manual,
	};
	check(wivrn::selected_bitrate(settings) == 50'000'000);
	settings.bitrate = wivrn::from_headset::bitrate_mode::automatic;
	check(wivrn::selected_bitrate(settings) == 12'500'000);
	settings.effective_bitrate_bps = 60'000'000;
	check(wivrn::selected_bitrate(settings) == 50'000'000); // Budget is a hard cap.
	check(wivrn::from_headset::settings_changed{}.bitrate ==
	      wivrn::from_headset::bitrate_mode::automatic);
}
