/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace wivrn
{
using spatial_extent = std::array<uint32_t, 2>;

bool is_headset_upscaling(from_headset::spatial_upscaling_mode mode);
bool is_spatial_upscaling(from_headset::spatial_upscaling_mode mode);
std::string_view spatial_upscaling_name(from_headset::spatial_upscaling_mode mode);

// All currently exposed spatial algorithms support at most 2x per axis. An
// equal or smaller target intentionally selects the normal rendering path.
bool valid_upscaling_extent(from_headset::spatial_upscaling_mode mode,
                            spatial_extent input,
                            spatial_extent target);

spatial_extent reconstruction_extent(const from_headset::headset_info_packet & info);
spatial_extent server_render_extent(const from_headset::headset_info_packet & info);
spatial_extent transport_extent(const from_headset::headset_info_packet & info);

spatial_extent align_video_extent(spatial_extent extent, uint32_t alignment = 64);

// Scale the base budget by real encoded area relative to the native reference
// produced with the default foveation. The user budget is always a hard cap.
uint32_t automatic_bitrate(uint32_t base_bitrate_bps,
                           spatial_extent encoded_extent,
                           spatial_extent reference_extent,
                           uint32_t minimum_bitrate_bps = 5'000'000);

uint32_t selected_bitrate(const from_headset::settings_changed & settings);
} // namespace wivrn
