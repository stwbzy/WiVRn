/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstdint>

namespace wivrn
{

// This policy is intentionally independent of OpenXR, Vulkan and Android.
// Values which are fractions are normalized to [0, 1], with larger values
// meaning more motion, uncertainty, load or thermal pressure as documented on
// each field below.
enum class frame_synthesis_policy_mode : uint8_t
{
	off,
	automatic,
	always,
};

enum class frame_synthesis_action : uint8_t
{
	full_synthesis,
	conservative_synthesis,
	pose_only,
};

enum class frame_synthesis_reason : uint8_t
{
	none,
	mode_off,
	auto_waiting,
	insufficient_history,
	scene_cut,
	pose_discontinuity,
	low_confidence,
	extreme_motion,
	thermal,
	deadline,
	stereo_mismatch,
	invalid_metric,
	session_disabled,
};

enum class frame_synthesis_classification_kind : uint8_t
{
	stable,
	global_fast_motion,
	low_confidence,
	scene_cut,
	thermal,
	deadline,
	invalid,
};

// Metrics for one eye and one real-frame interval.
//
// pose_delta_degrees is deliberately expressed in degrees because the cut
// threshold is specified in degrees.  All other *_fraction fields are
// normalized.  A value of -1 means that an optional alias is not supplied;
// the policy then uses the corresponding primary field.
struct frame_synthesis_metrics
{
	float pose_delta_degrees = 0.0f;
	float pose_degrees = -1.0f; // Optional spelling used by telemetry adapters.
	bool generation_continuous = true;
	bool fov_continuous = true;

	float correlation = 1.0f;         // normalized luma correlation, high is good
	float histogram_distance = 0.0f;  // normalized distance, low is good
	float consistency_bad_fraction = 0.0f;
	float consistency_error_px = 0.0f; // flow-resolution forward/backward error
	float hole_fraction = 0.0f;
	float holes = -1.0f; // Optional alias for hole_fraction.
	float flow_p95_fraction = 0.0f; // P95 / image dimension for this interval
	float flow_p95 = -1.0f;         // Optional alias for flow_p95_fraction.

	// Thermal severity is normalized as 0=cool and 1=critical.  The primary
	// field is thermal_severity; thermal is accepted for simple adapters.
	float thermal_severity = 0.0f;
	float thermal = -1.0f;

	// deadline_utilization is application time / application period.  It is
	// used by Automatic's 90/70% hysteresis.  pipeline_utilization is the
	// complete synthesis pipeline time / display period and is used by the
	// 55%-P95 watchdog.  deadline is a compatibility alias for adapters which
	// expose only one normalized deadline metric; when present it is used for
	// both purposes.
	float deadline_utilization = 0.0f;
	float pipeline_utilization = -1.0f;
	float application_utilization = -1.0f;
	float deadline = -1.0f;
	bool deadline_missed = false;

	// A caller can invalidate a sample without manufacturing sentinel metric
	// values.  history_valid=false asks the policy to return pose-only and reset
	// temporal state for that sample.
	bool history_valid = true;
	bool valid = true;
};

// A stereo sample.  The policy never returns full synthesis for one eye and
// conservative/pose-only for the other: the least safe eye controls both.
struct frame_synthesis_stereo_metrics
{
	frame_synthesis_metrics left{};
	frame_synthesis_metrics right{};
	bool stereo_valid = true;
	bool generation_continuous = true;
	bool fov_continuous = true;
	bool history_valid = true;
};

struct frame_synthesis_classification
{
	frame_synthesis_classification_kind kind = frame_synthesis_classification_kind::stable;
	frame_synthesis_reason reason = frame_synthesis_reason::none;
	float confidence = 1.0f;
};

// The primary action is one of the three synthesis modes.  reset_history and
// disable_session are orthogonal safety outputs and may accompany pose_only.
// The boolean fields are intentionally explicit so callers do not need to
// infer safety side effects from the primary enum.
struct frame_synthesis_decision
{
	frame_synthesis_action action = frame_synthesis_action::pose_only;
	frame_synthesis_reason reason = frame_synthesis_reason::none;
	bool full_synthesis = false;
	bool conservative_synthesis = false;
	bool pose_only = true;
	bool reset_history = false;
	bool disable_session = false;

	// Diagnostic state at the instant this decision was emitted.
	uint32_t samples_in_window = 0;
	uint32_t consecutive_bad_windows = 0;
	bool automatic_active = false;

	bool synthesizes() const
	{
		return full_synthesis || conservative_synthesis;
	}
};

// The thresholds are public both to keep the contract auditable and to make
// calibration/tests independent of any GPU or headset implementation.
struct frame_synthesis_policy_thresholds
{
	static constexpr uint32_t window_samples = 180;
	static constexpr uint32_t required_bad_windows = 3;

	static constexpr float scene_cut_pose_degrees = 35.0f;
	static constexpr float scene_cut_correlation = 0.35f;
	static constexpr float scene_cut_histogram_distance = 0.30f;
	static constexpr float low_confidence_bad_fraction = 0.20f;
	static constexpr float low_confidence_error_pixels = 1.50f;
	static constexpr float low_confidence_hole_fraction = 0.10f;
	static constexpr float extreme_flow_fraction = 0.10f;

	static constexpr float thermal_severe_fraction = 0.90f;
	static constexpr float pipeline_deadline_p95_fraction = 0.55f;
	static constexpr float missed_interval_fraction = 0.01f;
	static constexpr float automatic_enter_fraction = 0.90f;
	static constexpr float automatic_exit_fraction = 0.70f;
};

// Stateless, deterministic classification for one eye.
frame_synthesis_classification classify_frame_synthesis(const frame_synthesis_metrics & metrics);

class frame_synthesis_policy
{
public:
	using thresholds = frame_synthesis_policy_thresholds;

	explicit frame_synthesis_policy(frame_synthesis_policy_mode mode = frame_synthesis_policy_mode::automatic);

	void set_mode(frame_synthesis_policy_mode mode);
	frame_synthesis_policy_mode mode() const
	{
		return mode_;
	}

	// reset() represents a new connection/session.  It clears sticky safety
	// state and the 180-sample windows without changing the requested mode.
	void reset();

	frame_synthesis_decision update(const frame_synthesis_metrics & metrics);
	frame_synthesis_decision update(const frame_synthesis_stereo_metrics & metrics);

	// Names useful to callers which treat this as a classifier/policy pair.
	frame_synthesis_decision sample(const frame_synthesis_metrics & metrics)
	{
		return update(metrics);
	}
	frame_synthesis_decision sample(const frame_synthesis_stereo_metrics & metrics)
	{
		return update(metrics);
	}

	bool automatic_active() const
	{
		return automatic_active_;
	}
	bool session_disabled() const
	{
		return session_disabled_;
	}
	uint32_t samples_in_window() const
	{
		return sample_count_;
	}
	uint32_t consecutive_bad_windows() const
	{
		return bad_windows_;
	}

private:
	frame_synthesis_decision make_decision(const frame_synthesis_stereo_metrics & metrics);
	void record_window();

	frame_synthesis_policy_mode mode_;
	bool automatic_active_ = false;
	bool session_disabled_ = false;
	uint32_t sample_count_ = 0;
	uint32_t missed_intervals_ = 0;
	uint32_t bad_windows_ = 0;
	uint32_t auto_enter_windows_ = 0;
	uint32_t auto_exit_windows_ = 0;
	frame_synthesis_reason safety_reason_ = frame_synthesis_reason::none;
	std::array<float, thresholds::window_samples> pipeline_samples_{};
	std::array<float, thresholds::window_samples> application_samples_{};
	std::array<bool, thresholds::window_samples> thermal_bad_{};
	std::array<bool, thresholds::window_samples> deadline_bad_{};
};

} // namespace wivrn
