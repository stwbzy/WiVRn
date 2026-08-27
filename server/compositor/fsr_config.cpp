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
bool legacy_fsr_upscaling_requested()
{
	static const bool enabled = parse_bool(std::getenv("WIVRN_FSR_UPSCALING"));
	return enabled;
}

bool meaningful_upscale(uint32_t input_width,
                        uint32_t input_height,
                        uint32_t output_width,
                        uint32_t output_height)
{
	if (input_width == 0 || input_height == 0 || output_width <= input_width || output_height <= input_height)
		return false;
	// Expensive reconstruction below ten percent per dimension is visually
	// indistinguishable from the fused path on the target devices.
	return uint64_t(output_width) * 10 >= uint64_t(input_width) * 11 ||
	       uint64_t(output_height) * 10 >= uint64_t(input_height) * 11;
}

upscaling_selection select_upscaling(const from_headset::headset_info_packet & info)
{
	upscaling_selection result{
	        .requested = info.upscaling,
	        .effective = info.upscaling,
	};
	if (result.requested == upscaling_mode::native && legacy_fsr_upscaling_requested())
	{
		result.requested = upscaling_mode::fsr1;
		result.effective = upscaling_mode::fsr1;
	}

	const uint32_t target_width = info.target_eye_width ? info.target_eye_width : info.native_eye_width;
	const uint32_t target_height = info.target_eye_height ? info.target_eye_height : info.native_eye_height;
	if (result.effective == upscaling_mode::native)
		return result;

	if (!meaningful_upscale(info.render_eye_width, info.render_eye_height, target_width, target_height))
	{
		result.effective = info.meta_compositor_available ? upscaling_mode::meta_compositor
		                                                  : upscaling_mode::bilinear;
		result.fallback_reason = runtime_fallback_reason::upscale_below_threshold;
		return result;
	}

	if (result.effective == upscaling_mode::meta_compositor && !info.meta_compositor_available)
	{
		result.effective = upscaling_mode::bilinear;
		result.fallback_reason = runtime_fallback_reason::unsupported;
	}
	if (result.effective == upscaling_mode::fsr1)
	{
		const uint64_t input_area = uint64_t(info.render_eye_width) * info.render_eye_height;
		const uint64_t output_area = uint64_t(target_width) * target_height;
		if (input_area == 0 || target_width < info.render_eye_width || target_height < info.render_eye_height ||
		    output_area > input_area * 4)
		{
			result.effective = info.meta_compositor_available ? upscaling_mode::meta_compositor
			                                                  : upscaling_mode::bilinear;
			result.fallback_reason = runtime_fallback_reason::unsupported;
		}
	}
	// NIS was not present in this fork.  Preserve the configuration value while
	// using the safe fused sampler until a portable NIS shader is built in.
	if (result.effective == upscaling_mode::nis)
	{
		result.effective = info.meta_compositor_available ? upscaling_mode::meta_compositor
		                                                  : upscaling_mode::bilinear;
		result.fallback_reason = runtime_fallback_reason::unsupported;
	}
	return result;
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
	const auto selection = select_upscaling(info);
	const uint32_t target_width = info.target_eye_width ? info.target_eye_width : info.native_eye_width;
	const uint32_t target_height = info.target_eye_height ? info.target_eye_height : info.native_eye_height;
	if (selection.effective != upscaling_mode::fsr1 || target_width < info.render_eye_width ||
	    target_height < info.render_eye_height || target_width == 0 || target_height == 0)
		return input;

	const uint64_t input_area = uint64_t(input[0]) * input[1];
	const uint64_t output_area = uint64_t(target_width) * target_height;
	// AMD documents EASU for a 1x through 4x output/input area range.
	if (input_area == 0 || output_area > input_area * 4)
		return input;

	return {target_width, target_height};
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
