/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "wivrn_packets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace wivrn
{
class scaling_watchdog
{
public:
	struct fallback
	{
		upscaling_mode from;
		upscaling_mode to;
		runtime_fallback_reason reason;
		float p95_seconds;
		float missed_percent;
	};

	static constexpr uint32_t window_frames = 180;

	scaling_watchdog(upscaling_mode effective, bool meta_supported) :
	        effective_(effective), meta_supported_(meta_supported)
	{
		gpu_seconds_.reserve(window_frames);
	}

	upscaling_mode effective() const
	{
		return effective_;
	}
	runtime_fallback_reason fallback_reason() const
	{
		return fallback_reason_;
	}

	std::optional<fallback> sample(float gpu_seconds, float display_period_seconds, bool missed_interval)
	{
		if (std::isfinite(gpu_seconds) && gpu_seconds >= 0)
			gpu_seconds_.push_back(gpu_seconds);
		else
			gpu_seconds_.push_back(0);
		missed_intervals_ += missed_interval;
		if (gpu_seconds_.size() < window_frames)
			return std::nullopt;

		auto sorted = gpu_seconds_;
		std::ranges::sort(sorted);
		const float p95 = sorted[(sorted.size() * 95 - 1) / 100];
		const float missed = 100.0f * float(missed_intervals_) / float(window_frames);
		const bool gpu_over_budget = display_period_seconds > 0 && p95 > display_period_seconds * 0.55f;
		if (missed > 1.0f)
			++consecutive_missed_windows_;
		else
			consecutive_missed_windows_ = 0;
		gpu_seconds_.clear();
		missed_intervals_ = 0;

		const auto reason = gpu_over_budget
		                            ? runtime_fallback_reason::client_gpu_budget
		                            : (consecutive_missed_windows_ >= 3
		                                       ? runtime_fallback_reason::client_missed_intervals
		                                       : runtime_fallback_reason::none);
		if (reason == runtime_fallback_reason::none)
			return std::nullopt;

		const auto previous = effective_;
		effective_ = next_fallback(effective_);
		if (effective_ == previous)
			return std::nullopt;
		fallback_reason_ = reason;
		consecutive_missed_windows_ = 0;
		return fallback{previous, effective_, reason, p95, missed};
	}

private:
	upscaling_mode next_fallback(upscaling_mode mode) const
	{
		switch (mode)
		{
			case upscaling_mode::sgsr1:
			case upscaling_mode::fsr1:
			case upscaling_mode::nis:
				return meta_supported_ ? upscaling_mode::meta_compositor : upscaling_mode::bilinear;
			case upscaling_mode::meta_compositor:
				return upscaling_mode::bilinear;
			case upscaling_mode::native:
			case upscaling_mode::bilinear:
				return mode;
		}
		return upscaling_mode::bilinear;
	}

	upscaling_mode effective_;
	bool meta_supported_;
	runtime_fallback_reason fallback_reason_ = runtime_fallback_reason::none;
	std::vector<float> gpu_seconds_;
	uint32_t missed_intervals_ = 0;
	uint32_t consecutive_missed_windows_ = 0;
};
} // namespace wivrn
