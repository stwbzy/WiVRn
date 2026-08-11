/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "stream_configuration.h"

#include "application.h"
#include "configuration.h"
#include "spatial_upscaling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
uint16_t scaled_dimension(uint32_t recommended, float scale, uint32_t maximum)
{
	if (!std::isfinite(scale) || scale <= 0)
		scale = 1;
	const auto value = uint64_t(std::llround(recommended * double(scale)));
	return uint16_t(std::clamp<uint64_t>(value, 1, std::min<uint64_t>(maximum, std::numeric_limits<uint16_t>::max())));
}
} // namespace

namespace wivrn
{
void configure_stream_dimensions(from_headset::headset_info_packet & info,
                                 const configuration & config,
                                 const XrViewConfigurationView & view)
{
	info.native_eye_width = uint16_t(std::min<uint32_t>(view.recommendedImageRectWidth, UINT16_MAX));
	info.native_eye_height = uint16_t(std::min<uint32_t>(view.recommendedImageRectHeight, UINT16_MAX));
	info.render_eye_width = scaled_dimension(view.recommendedImageRectWidth,
	                                         config.resolution_scale,
	                                         view.maxImageRectWidth);
	info.render_eye_height = scaled_dimension(view.recommendedImageRectHeight,
	                                          config.resolution_scale,
	                                          view.maxImageRectHeight);
	const float target_limit = config.extended_config ? 1.5f : 1.0f;
	info.target_eye_width = scaled_dimension(view.recommendedImageRectWidth,
	                                         std::min(config.target_resolution_scale, target_limit),
	                                         view.maxImageRectWidth);
	info.target_eye_height = scaled_dimension(view.recommendedImageRectHeight,
	                                          std::min(config.target_resolution_scale, target_limit),
	                                          view.maxImageRectHeight);

	const float stream_scale = std::clamp(config.get_stream_scale(), 0.01f, 1.0f);
	info.stream_eye_width = scaled_dimension(info.render_eye_width, stream_scale, UINT16_MAX);
	info.stream_eye_height = scaled_dimension(info.render_eye_height, stream_scale, UINT16_MAX);
	info.spatial_upscaling = config.spatial_upscaling;

	// Client reconstruction is initially enabled only on Qualcomm/Adreno. A
	// stale or manually copied setting on another GPU safely migrates to Off.
	if (is_headset_upscaling(info.spatial_upscaling) &&
	    application::get_physical_device_properties().vendorID != 0x5143)
		info.spatial_upscaling = from_headset::spatial_upscaling_mode::off;
	if (is_spatial_upscaling(info.spatial_upscaling) &&
	    !valid_upscaling_extent(info.spatial_upscaling,
	                            {info.render_eye_width, info.render_eye_height},
	                            {info.target_eye_width, info.target_eye_height}))
		info.spatial_upscaling = from_headset::spatial_upscaling_mode::off;
}

from_headset::settings_changed make_stream_settings(
        const configuration & config,
        const from_headset::headset_info_packet & info)
{
	spatial_extent encoded = align_video_extent(transport_extent(info));
	const float reference_scale = std::clamp(config.get_default_stream_scale(), 0.01f, 1.0f);
	spatial_extent reference = align_video_extent({
	        uint32_t(std::lround(info.native_eye_width * reference_scale)),
	        uint32_t(std::lround(info.native_eye_height * reference_scale)),
	});
	const uint32_t effective = config.bitrate == from_headset::bitrate_mode::automatic
	                                   ? automatic_bitrate(config.bitrate_bps, encoded, reference)
	                                   : config.bitrate_bps;
	return {
	        .preferred_refresh_rate = config.preferred_refresh_rate,
	        .minimum_refresh_rate = config.minimum_refresh_rate.value_or(0),
	        .fps_divider = config.fps_divider,
	        .bitrate_bps = config.bitrate_bps,
	        .effective_bitrate_bps = effective,
	        .bitrate = config.bitrate,
	        .mirror_gamepad = config.forward_gamepad,
	        .enabled_body_parts = config.body_part_mask,
	};
}
} // namespace wivrn
