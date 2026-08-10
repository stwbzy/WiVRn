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
#ifndef WIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION
#define WIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION 0
#endif

bool fsr_upscaling_requested();
bool fsr_frame_generation_enabled();
float fsr_rcas_sharpness_stops();
std::array<uint32_t, 2> fsr_output_dimensions(const from_headset::headset_info_packet &);
std::array<uint32_t, 2> fsr_stream_dimensions(const from_headset::headset_info_packet &);
} // namespace wivrn
