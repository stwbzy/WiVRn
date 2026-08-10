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
bool fsr_upscaling_enabled(const from_headset::headset_info_packet &);
float sanitize_fsr_sharpness(float);
float fsr_rcas_sharpness_stops();
std::array<uint32_t, 2> fsr_output_dimensions(const from_headset::headset_info_packet &);
std::array<uint32_t, 2> fsr_stream_dimensions(const from_headset::headset_info_packet &);
std::array<uint16_t, 2> fsr_encoder_dimensions(const from_headset::headset_info_packet &, uint32_t alignment = 64);
} // namespace wivrn
