/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"

#include <array>
#include <cstdint>

namespace wivrn
{
struct upscaling_selection
{
	upscaling_mode requested = upscaling_mode::native;
	upscaling_mode effective = upscaling_mode::native;
	runtime_fallback_reason fallback_reason = runtime_fallback_reason::none;
};

bool legacy_fsr_upscaling_requested();
float fsr_rcas_sharpness_stops();
bool meaningful_upscale(uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height);
upscaling_selection select_upscaling(const from_headset::headset_info_packet &);
std::array<uint32_t, 2> fsr_output_dimensions(const from_headset::headset_info_packet &);
std::array<uint32_t, 2> fsr_stream_dimensions(const from_headset::headset_info_packet &);
} // namespace wivrn
