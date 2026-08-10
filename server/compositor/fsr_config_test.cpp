/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("FSR configuration test check failed");
}
} // namespace

int main()
{
	wivrn::from_headset::headset_info_packet info{};
	check(!info.fsr_upscaling); // Protocol/config default is OFF.
	info.native_eye_width = 2000;
	info.native_eye_height = 2200;
	info.render_eye_width = 1600;
	info.render_eye_height = 1760;
	info.stream_eye_width = 960;
	info.stream_eye_height = 1056;

	// OFF preserves the stable render/stream dimensions exactly.
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{1600, 1760}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{960, 1056}));
	check(!wivrn::fsr_upscaling_enabled(info));

	info.fsr_upscaling = true;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1200, 1320}));
	check((wivrn::fsr_encoder_dimensions(info) == std::array<uint16_t, 2>{1216, 1344}));
	check(wivrn::fsr_upscaling_enabled(info));
	check(std::abs(wivrn::sanitize_fsr_sharpness(0.2f) - 0.2f) < 0.001f);
	check(wivrn::sanitize_fsr_sharpness(-1.0f) == 0.0f);
	check(wivrn::sanitize_fsr_sharpness(9.0f) == 2.0f);
	check(wivrn::sanitize_fsr_sharpness(std::numeric_limits<float>::quiet_NaN()) == 0.2f);

	// One-to-one input stays on the normal path.
	info.render_eye_width = info.native_eye_width;
	info.render_eye_height = info.native_eye_height;
	info.stream_eye_width = 1000;
	info.stream_eye_height = 1100;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1000, 1100}));
	check(!wivrn::fsr_upscaling_enabled(info));

	// More than four times the input area is outside FSR 1's supported range.
	info.render_eye_width = 900;
	info.render_eye_height = 900;
	info.stream_eye_width = 720;
	info.stream_eye_height = 720;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{900, 900}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{720, 720}));

	// Zero and inverted dimensions also fall back without division by zero.
	info.render_eye_width = 0;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{0, 900}));
	info.render_eye_width = 2100;
	info.render_eye_height = 2200;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2100, 2200}));

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
}
