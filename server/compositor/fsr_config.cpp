/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"
#include "spatial_upscaling.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace wivrn
{
bool fsr_upscaling_enabled(const from_headset::headset_info_packet & info)
{
	if (info.spatial_upscaling != from_headset::spatial_upscaling_mode::fsr1_pc)
		return false;
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
	return server_render_extent(info);
}

std::array<uint32_t, 2> fsr_stream_dimensions(const from_headset::headset_info_packet & info)
{
	return transport_extent(info);
}

std::array<uint16_t, 2> fsr_encoder_dimensions(const from_headset::headset_info_packet & info,
                                               uint32_t alignment)
{
	const auto stream = fsr_stream_dimensions(info);
	const auto aligned = align_video_extent(stream, alignment);
	return {uint16_t(aligned[0]), uint16_t(aligned[1])};
}
} // namespace wivrn
