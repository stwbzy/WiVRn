/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"

#include <cassert>
#include <cmath>
#include <cstdlib>

int main()
{
	setenv("WIVRN_FSR_UPSCALING", "1", 1);
	setenv("WIVRN_FSR_SHARPNESS", "0.4", 1);

	wivrn::from_headset::headset_info_packet info{};
	info.native_eye_width = 2000;
	info.native_eye_height = 2200;
	info.render_eye_width = 1600;
	info.render_eye_height = 1760;
	info.stream_eye_width = 960;
	info.stream_eye_height = 1056;

	assert((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	assert((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1200, 1320}));
	assert(std::abs(wivrn::fsr_rcas_sharpness_stops() - 0.4f) < 0.001f);

	// One-to-one input stays on the normal path.
	info.render_eye_width = info.native_eye_width;
	info.render_eye_height = info.native_eye_height;
	info.stream_eye_width = 1000;
	info.stream_eye_height = 1100;
	assert((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	assert((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1000, 1100}));

	// More than four times the input area is outside FSR 1's supported range.
	info.render_eye_width = 900;
	info.render_eye_height = 900;
	info.stream_eye_width = 720;
	info.stream_eye_height = 720;
	assert((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{900, 900}));
	assert((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{720, 720}));
}
