/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"

#include <cstdint>

namespace wivrn::frame_synthesis_negotiation
{

constexpr bool known_backend(frame_synthesis_backend backend) noexcept
{
	switch (backend)
	{
		case frame_synthesis_backend::auto_select:
		case frame_synthesis_backend::headset_color:
		case frame_synthesis_backend::server_experimental:
			return true;
		default:
			return false;
	}
}

constexpr frame_synthesis_backend normalize_backend(frame_synthesis_backend backend) noexcept
{
	return known_backend(backend) ? backend : frame_synthesis_backend::auto_select;
}

/**
 * Negotiate a backend without consulting mode.  `server_experimental` is
 * deliberately opt-in; Auto never falls back to server synthesis merely
 * because the server advertises it.  Unknown wire values are safe Auto.
 */
constexpr frame_synthesis_backend negotiate_backend(
	        frame_synthesis_backend requested,
	        uint8_t headset_capabilities,
	        uint8_t server_capabilities) noexcept
{
	if (!known_backend(requested))
		return frame_synthesis_backend::auto_select;

	const bool headset_color = has_frame_synthesis_capability(
	        headset_capabilities,
	        frame_synthesis_capability::headset_color);
	const bool server_experimental = has_frame_synthesis_capability(
	        server_capabilities,
	        frame_synthesis_capability::server_experimental);

	switch (requested)
	{
		case frame_synthesis_backend::auto_select:
		case frame_synthesis_backend::headset_color:
			return headset_color ? frame_synthesis_backend::headset_color
		                     : frame_synthesis_backend::auto_select;
		case frame_synthesis_backend::server_experimental:
			return server_experimental ? frame_synthesis_backend::server_experimental
		                           : frame_synthesis_backend::auto_select;
		default:
			return frame_synthesis_backend::auto_select;
	}
}

/** Apply the requested mode at the call site: Off always disables synthesis. */
constexpr frame_synthesis_backend effective_backend(
	        frame_synthesis_mode mode,
	        frame_synthesis_backend requested,
	        uint8_t headset_capabilities,
	        uint8_t server_capabilities) noexcept
{
	return mode == frame_synthesis_mode::off
	               ? frame_synthesis_backend::auto_select
	               : negotiate_backend(requested, headset_capabilities, server_capabilities);
}

} // namespace wivrn::frame_synthesis_negotiation
