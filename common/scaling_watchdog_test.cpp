/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scaling_watchdog.h"

#include <stdexcept>

namespace
{
void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("scaling watchdog test check failed");
}
} // namespace

int main()
{
	constexpr float period = 1.0f / 90.0f;
	wivrn::scaling_watchdog gpu_watchdog(wivrn::upscaling_mode::sgsr1, true);
	std::optional<wivrn::scaling_watchdog::fallback> transition;
	for (uint32_t i = 0; i < wivrn::scaling_watchdog::window_frames; ++i)
		transition = gpu_watchdog.sample(period * 0.60f, period, false);
	check(transition.has_value());
	check(transition->to == wivrn::upscaling_mode::meta_compositor);
	check(transition->reason == wivrn::runtime_fallback_reason::client_gpu_budget);

	// A single bad missed-frame window is tolerated.  Three consecutive
	// windows above one percent trigger the session-only fallback.
	wivrn::scaling_watchdog missed_watchdog(wivrn::upscaling_mode::meta_compositor, true);
	for (int window = 0; window < 3; ++window)
	{
		transition.reset();
		for (uint32_t i = 0; i < wivrn::scaling_watchdog::window_frames; ++i)
			transition = missed_watchdog.sample(period * 0.1f, period, i < 2);
		if (window < 2)
			check(!transition);
	}
	check(transition.has_value());
	check(transition->to == wivrn::upscaling_mode::bilinear);
	check(transition->reason == wivrn::runtime_fallback_reason::client_missed_intervals);

	// Without Meta compositor support, expensive paths fall directly to the
	// fused bilinear pass.
	wivrn::scaling_watchdog no_meta(wivrn::upscaling_mode::fsr1, false);
	for (uint32_t i = 0; i < wivrn::scaling_watchdog::window_frames; ++i)
		transition = no_meta.sample(period * 0.60f, period, false);
	check(transition.has_value());
	check(transition->to == wivrn::upscaling_mode::bilinear);
	return 0;
}
