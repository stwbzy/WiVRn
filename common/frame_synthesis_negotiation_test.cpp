/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frame_synthesis_negotiation.h"

#include <stdexcept>

namespace
{
void check(bool condition, const char * message)
{
	if (!condition)
		throw std::runtime_error(message);
}

using namespace wivrn;
using namespace wivrn::frame_synthesis_negotiation;

constexpr uint8_t headset = static_cast<uint8_t>(frame_synthesis_capability::headset_color);
constexpr uint8_t server = static_cast<uint8_t>(frame_synthesis_capability::server_experimental);
} // namespace

int main()
{
	using namespace wivrn;

	check(negotiate_backend(frame_synthesis_backend::auto_select, headset, server) ==
	              frame_synthesis_backend::headset_color,
	      "Auto with headset capability must select headset_color");
	check(negotiate_backend(frame_synthesis_backend::auto_select, 0, server) ==
	              frame_synthesis_backend::auto_select,
	      "Auto without headset capability must not select server synthesis");
	check(negotiate_backend(frame_synthesis_backend::headset_color, 0, server) ==
	              frame_synthesis_backend::auto_select,
	      "Explicit headset_color without support must fall back to Auto");
	check(negotiate_backend(frame_synthesis_backend::server_experimental, headset, 0) ==
	              frame_synthesis_backend::auto_select,
	      "Server synthesis must require server capability");
	check(negotiate_backend(frame_synthesis_backend::server_experimental, 0, server) ==
	              frame_synthesis_backend::server_experimental,
	      "Explicit supported server synthesis must be selected");

	constexpr auto unknown = static_cast<frame_synthesis_backend>(0xff);
	check(normalize_backend(unknown) == frame_synthesis_backend::auto_select,
	      "Unknown backend enum must normalize to Auto");
	check(negotiate_backend(unknown, headset, server) == frame_synthesis_backend::auto_select,
	      "Unknown backend enum must not enable a backend");
	check(effective_backend(frame_synthesis_mode::off, frame_synthesis_backend::server_experimental, headset, server) ==
	              frame_synthesis_backend::auto_select,
	      "Off mode must be applied by the caller as Auto");

	return 0;
}
