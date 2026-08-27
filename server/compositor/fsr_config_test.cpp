/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_config.h"

#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace
{
void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("FSR configuration test check failed");
}
} // namespace

int main()
{
	setenv("WIVRN_FSR_UPSCALING", "1", 1);
	setenv("WIVRN_FSR_SHARPNESS", "0.4", 1);

	wivrn::from_headset::headset_info_packet info{};
	info.native_eye_width = 2000;
	info.native_eye_height = 2200;
	info.target_eye_width = 2000;
	info.target_eye_height = 2200;
	info.render_eye_width = 1600;
	info.render_eye_height = 1760;
	info.stream_eye_width = 960;
	info.stream_eye_height = 1056;

	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1200, 1320}));
	check(std::abs(wivrn::fsr_rcas_sharpness_stops() - 0.4f) < 0.001f);

	// One-to-one input stays on the normal path.
	info.render_eye_width = info.native_eye_width;
	info.render_eye_height = info.native_eye_height;
	info.stream_eye_width = 1000;
	info.stream_eye_height = 1100;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{2000, 2200}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{1000, 1100}));

	// More than four times the input area is outside FSR 1's supported range.
	info.render_eye_width = 900;
	info.render_eye_height = 900;
	info.stream_eye_width = 720;
	info.stream_eye_height = 720;
	check((wivrn::fsr_output_dimensions(info) == std::array<uint32_t, 2>{900, 900}));
	check((wivrn::fsr_stream_dimensions(info) == std::array<uint32_t, 2>{720, 720}));

	// The 10% gate prevents an expensive 100 -> 101% reconstruction pass.
	info = {};
	info.upscaling = wivrn::upscaling_mode::sgsr1;
	info.meta_compositor_available = true;
	info.render_eye_width = 2000;
	info.render_eye_height = 2000;
	info.target_eye_width = 2020;
	info.target_eye_height = 2020;
	auto selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::meta_compositor);
	check(selection.fallback_reason == wivrn::runtime_fallback_reason::upscale_below_threshold);

	// 100/100 and 80/80 likewise avoid a reconstruction pass.
	info.target_eye_width = 2000;
	info.target_eye_height = 2000;
	selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::meta_compositor);
	info.render_eye_width = 1600;
	info.render_eye_height = 1600;
	info.target_eye_width = 1600;
	info.target_eye_height = 1600;
	selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::meta_compositor);

	// 80 -> 100% is large enough for the explicitly selected local path.
	info.target_eye_width = 2000;
	info.target_eye_height = 2000;
	selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::sgsr1);

	// FSR beyond its 4x area bound takes the safe compositor route.
	info.upscaling = wivrn::upscaling_mode::fsr1;
	info.render_eye_width = 900;
	info.render_eye_height = 900;
	info.target_eye_width = 2000;
	info.target_eye_height = 2200;
	selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::meta_compositor);
	check(selection.fallback_reason == wivrn::runtime_fallback_reason::unsupported);

	// Unknown runtimes cannot use Meta's filter extension.
	info.upscaling = wivrn::upscaling_mode::meta_compositor;
	info.meta_compositor_available = false;
	selection = wivrn::select_upscaling(info);
	check(selection.effective == wivrn::upscaling_mode::bilinear);
	check(selection.fallback_reason == wivrn::runtime_fallback_reason::unsupported);
}
