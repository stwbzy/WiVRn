/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace wivrn
{
bool fsr_upscaling_enabled(const from_headset::headset_info_packet & info)
{
	return fsr_output_dimensions(info) !=
	       std::array<uint32_t, 2>{info.render_eye_width, info.render_eye_height};
}

float sanitize_fsr_sharpness(float sharpness)
{
	return std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 2.0f) : 0.2f;
}

float fsr_rcas_sharpness_stops()
{
	static const float stops = [] {
		const char * value = std::getenv("WIVRN_FSR_SHARPNESS");
		if (value == nullptr || *value == '\0')
			return 0.2f;
		char * end = nullptr;
		const float parsed = std::strtof(value, &end);
		if (end == value || *end != '\0')
			return 0.2f;
		return sanitize_fsr_sharpness(parsed);
	}();
	return stops;
}

std::array<uint32_t, 2> fsr_output_dimensions(const from_headset::headset_info_packet & info)
{
	const std::array input{uint32_t(info.render_eye_width), uint32_t(info.render_eye_height)};
	if (!info.fsr_upscaling || info.native_eye_width < info.render_eye_width ||
	    info.native_eye_height < info.render_eye_height || info.native_eye_width == 0 ||
	    info.native_eye_height == 0 || info.render_eye_width == 0 || info.render_eye_height == 0 ||
	    (info.native_eye_width == info.render_eye_width && info.native_eye_height == info.render_eye_height))
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

std::array<uint16_t, 2> fsr_encoder_dimensions(const from_headset::headset_info_packet & info,
                                               uint32_t alignment)
{
	if (alignment == 0)
		throw std::invalid_argument("video alignment must be non-zero");
	const auto stream = fsr_stream_dimensions(info);
	const auto align = [alignment](uint32_t value) -> uint16_t {
		const uint64_t aligned = (uint64_t(value) + alignment - 1) / alignment * alignment;
		if (aligned == 0 || aligned > std::numeric_limits<uint16_t>::max())
			throw std::runtime_error("requested video size exceeds the protocol limit");
		return uint16_t(aligned);
	};
	return {align(stream[0]), align(stream[1])};
}
} // namespace wivrn
