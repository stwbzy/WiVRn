/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace
{
bool parse_bool(const char * value)
{
	if (value == nullptr || *value == '\0')
		return false;
	return value != std::string_view{"0"} && value != std::string_view{"false"} &&
	       value != std::string_view{"FALSE"} && value != std::string_view{"off"} &&
	       value != std::string_view{"OFF"} && value != std::string_view{"no"} &&
	       value != std::string_view{"NO"};
}
} // namespace

namespace wivrn
{
bool fsr_upscaling_requested()
{
	static const bool enabled = parse_bool(std::getenv("WIVRN_FSR_UPSCALING"));
	return enabled;
}

bool fsr_frame_generation_enabled()
{
	// Keep the unstable synthesis path opt-in at build time. Runtime frame
	// generation variables are intentionally ignored by the default build so a
	// stale launch command cannot switch the compositor into the black-screen
	// path while the implementation is being repaired.
	return WIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION != 0;
}

float fsr_rcas_sharpness_stops()
{
	static const float stops = [] {
		const char * value = std::getenv("WIVRN_FSR_SHARPNESS");
		if (value == nullptr || *value == '\0')
			return 0.2f;
		char * end = nullptr;
		const float parsed = std::strtof(value, &end);
		if (end == value || *end != '\0' || !std::isfinite(parsed))
			return 0.2f;
		return std::clamp(parsed, 0.0f, 2.0f);
	}();
	return stops;
}

std::array<uint32_t, 2> fsr_output_dimensions(const from_headset::headset_info_packet & info)
{
	const std::array input{uint32_t(info.render_eye_width), uint32_t(info.render_eye_height)};
	if (!fsr_upscaling_requested() || info.native_eye_width < info.render_eye_width ||
	    info.native_eye_height < info.render_eye_height || info.native_eye_width == 0 ||
	    info.native_eye_height == 0)
		return input;

	const uint64_t input_area = uint64_t(input[0]) * input[1];
	const uint64_t output_area = uint64_t(info.native_eye_width) * info.native_eye_height;
	// AMD documents EASU for a 1x through 4x output/input area range.
	if (input_area == 0 || output_area > input_area * 4)
		return input;

	return {info.native_eye_width, info.native_eye_height};
}

std::array<uint32_t, 2> fsr_stream_dimensions(const from_headset::headset_info_packet & info)
{
	const auto output = fsr_output_dimensions(info);
	if (output[0] == info.render_eye_width && output[1] == info.render_eye_height)
		return {info.stream_eye_width, info.stream_eye_height};

	const auto scaled = [](uint32_t output_size, uint32_t stream_size, uint32_t render_size) {
		return uint32_t(std::lround(double(output_size) * double(stream_size) / double(render_size)));
	};
	return {
	        scaled(output[0], info.stream_eye_width, info.render_eye_width),
	        scaled(output[1], info.stream_eye_height, info.render_eye_height),
	};
}
} // namespace wivrn
