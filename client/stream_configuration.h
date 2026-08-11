/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"

#include <openxr/openxr.h>

class configuration;

namespace wivrn
{
void configure_stream_dimensions(from_headset::headset_info_packet & info,
                                 const configuration & config,
                                 const XrViewConfigurationView & view);

from_headset::settings_changed make_stream_settings(
        const configuration & config,
        const from_headset::headset_info_packet & info);
} // namespace wivrn
