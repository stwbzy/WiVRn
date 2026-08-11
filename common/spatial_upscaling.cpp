/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spatial_upscaling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace wivrn
{
bool is_headset_upscaling(from_headset::spatial_upscaling_mode mode)
{
	using enum from_headset::spatial_upscaling_mode;
	return mode == sgsr1_headset || mode == fsr1_headset || mode == nis_headset;
}

bool is_spatial_upscaling(from_headset::spatial_upscaling_mode mode)
{
	return mode != from_headset::spatial_upscaling_mode::off;
}

std::string_view spatial_upscaling_name(from_headset::spatial_upscaling_mode mode)
{
	using enum from_headset::spatial_upscaling_mode;
	switch (mode)
	{
		case off:
			return "Off";
		case fsr1_pc:
			return "PC - FSR1";
		case sgsr1_headset:
			return "Headset - SGSR1";
		case fsr1_headset:
			return "Headset - FSR1";
		case nis_headset:
			return "Headset - NIS";
	}
	return "Unknown";
}

bool valid_upscaling_extent(from_headset::spatial_upscaling_mode mode,
                            spatial_extent input,
                            spatial_extent target)
{
	if (!is_spatial_upscaling(mode) || input[0] == 0 || input[1] == 0 ||
	    target[0] <= input[0] || target[1] <= input[1])
		return false;

	return uint64_t(target[0]) <= uint64_t(input[0]) * 2 &&
	       uint64_t(target[1]) <= uint64_t(input[1]) * 2;
}

spatial_extent reconstruction_extent(const from_headset::headset_info_packet & info)
{
	const spatial_extent input{info.render_eye_width, info.render_eye_height};
	const spatial_extent target{info.target_eye_width, info.target_eye_height};
	return valid_upscaling_extent(info.spatial_upscaling, input, target) ? target : input;
}

spatial_extent server_render_extent(const from_headset::headset_info_packet & info)
{
	if (info.spatial_upscaling == from_headset::spatial_upscaling_mode::fsr1_pc)
		return reconstruction_extent(info);
	return {info.render_eye_width, info.render_eye_height};
}

spatial_extent transport_extent(const from_headset::headset_info_packet & info)
{
	const spatial_extent stream{info.stream_eye_width, info.stream_eye_height};
	const auto output = server_render_extent(info);
	if (output == spatial_extent{info.render_eye_width, info.render_eye_height} ||
	    info.render_eye_width == 0 || info.render_eye_height == 0)
		return stream;

	return {
	        uint32_t(std::lround(double(output[0]) * info.stream_eye_width / info.render_eye_width)),
	        uint32_t(std::lround(double(output[1]) * info.stream_eye_height / info.render_eye_height)),
	};
}

spatial_extent align_video_extent(spatial_extent extent, uint32_t alignment)
{
	if (alignment == 0)
		throw std::invalid_argument("video alignment must be non-zero");
	for (auto & value: extent)
	{
		const uint64_t aligned = (uint64_t(value) + alignment - 1) / alignment * alignment;
		if (aligned == 0 || aligned > std::numeric_limits<uint16_t>::max())
			throw std::runtime_error("requested video size exceeds the protocol limit");
		value = uint32_t(aligned);
	}
	return extent;
}

uint32_t automatic_bitrate(uint32_t base_bitrate_bps,
                           spatial_extent encoded_extent,
                           spatial_extent reference_extent,
                           uint32_t minimum_bitrate_bps)
{
	if (base_bitrate_bps == 0 || encoded_extent[0] == 0 || encoded_extent[1] == 0 ||
	    reference_extent[0] == 0 || reference_extent[1] == 0)
		return base_bitrate_bps;

	const long double encoded_area = static_cast<long double>(encoded_extent[0]) * encoded_extent[1];
	const long double reference_area = static_cast<long double>(reference_extent[0]) * reference_extent[1];
	const auto scaled = static_cast<uint64_t>(std::llround(base_bitrate_bps * encoded_area / reference_area));
	const uint32_t floor = std::min(base_bitrate_bps, minimum_bitrate_bps);
	return std::clamp<uint64_t>(scaled, floor, base_bitrate_bps);
}

uint32_t selected_bitrate(const from_headset::settings_changed & settings)
{
	return settings.bitrate == from_headset::bitrate_mode::automatic
	               ? std::min(settings.effective_bitrate_bps, settings.bitrate_bps)
	               : settings.bitrate_bps;
}
} // namespace wivrn
