/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frame_synthesis_policy.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool condition, const char * message)
{
	if (!condition)
		throw std::runtime_error(message);
}

wivrn::frame_synthesis_metrics good_metrics()
{
	wivrn::frame_synthesis_metrics metrics;
	metrics.application_utilization = 0.50f;
	return metrics;
}

void check_full(const wivrn::frame_synthesis_decision & decision)
{
	check(decision.action == wivrn::frame_synthesis_action::full_synthesis, "expected full synthesis action");
	check(decision.full_synthesis && !decision.conservative_synthesis && !decision.pose_only,
	      "full synthesis flags disagree");
}
} // namespace

int main()
{
	using namespace wivrn;

	// The stable path is full synthesis in Always mode.
	frame_synthesis_policy always(frame_synthesis_policy_mode::always);
	check_full(always.update(good_metrics()));

	// Fast motion is shared conservatively by both eyes.
	frame_synthesis_metrics fast = good_metrics();
	fast.flow_p95_fraction = frame_synthesis_policy_thresholds::extreme_flow_fraction + 0.01f;
	auto decision = always.update(fast);
	check(decision.action == frame_synthesis_action::conservative_synthesis,
	      "extreme flow should select conservative synthesis");
	check(decision.conservative_synthesis && !decision.full_synthesis && !decision.pose_only,
	      "conservative synthesis flags disagree");
	check(decision.reason == frame_synthesis_reason::extreme_motion, "missing extreme-motion reason");

	// A cut from either eye resets shared history and cannot create a stereo
	// candidate.  This also checks the strict threshold direction.
	frame_synthesis_stereo_metrics cut;
	cut.left = good_metrics();
	cut.right = good_metrics();
	cut.right.pose_delta_degrees = frame_synthesis_policy_thresholds::scene_cut_pose_degrees + 0.01f;
	decision = always.update(cut);
	check(decision.pose_only && decision.reset_history, "stereo cut must reset history");
	check(decision.reason == frame_synthesis_reason::scene_cut, "missing scene-cut reason");

	// Correlation and histogram are conjunctive for the cut detector.
	frame_synthesis_metrics histogram_cut = good_metrics();
	histogram_cut.correlation = frame_synthesis_policy_thresholds::scene_cut_correlation - 0.01f;
	histogram_cut.histogram_distance = frame_synthesis_policy_thresholds::scene_cut_histogram_distance + 0.01f;
	decision = always.update(histogram_cut);
	check(decision.pose_only && decision.reset_history, "correlation/histogram cut must reset history");

	// Inconsistency and holes reject extrapolation without invalidating a
	// usable real-frame history.
	frame_synthesis_metrics uncertain = good_metrics();
	uncertain.consistency_bad_fraction = frame_synthesis_policy_thresholds::low_confidence_bad_fraction + 0.01f;
	decision = always.update(uncertain);
	check(decision.pose_only && !decision.reset_history, "low confidence must be pose-only");
	check(decision.reason == frame_synthesis_reason::low_confidence, "missing low-confidence reason");
	uncertain = good_metrics();
	uncertain.hole_fraction = frame_synthesis_policy_thresholds::low_confidence_hole_fraction + 0.01f;
	decision = always.update(uncertain);
	check(decision.pose_only, "holes must reject synthesis");

	// If the right eye is unsafe, the left eye cannot continue alone.
	frame_synthesis_stereo_metrics stereo;
	stereo.left = good_metrics();
	stereo.right = good_metrics();
	stereo.right.hole_fraction = 0.20f;
	decision = always.update(stereo);
	check(decision.pose_only && !decision.full_synthesis, "unsafe right eye must gate stereo");

	// Explicitly invalid history and metrics are deterministic safety paths.
	frame_synthesis_metrics no_history = good_metrics();
	no_history.history_valid = false;
	decision = always.update(no_history);
	check(decision.pose_only && decision.reset_history, "missing history must request reset");
	check(decision.reason == frame_synthesis_reason::insufficient_history, "missing history reason");
	frame_synthesis_metrics invalid = good_metrics();
	invalid.correlation = std::numeric_limits<float>::quiet_NaN();
	decision = always.update(invalid);
	check(decision.pose_only && decision.reason == frame_synthesis_reason::invalid_metric,
	      "invalid metric must reject synthesis");

	// Automatic mode enters only after three complete 180-sample windows above
	// 90% application utilization, then exits after three below 70% windows.
	frame_synthesis_policy automatic(frame_synthesis_policy_mode::automatic);
	frame_synthesis_metrics overloaded = good_metrics();
	overloaded.application_utilization = frame_synthesis_policy_thresholds::automatic_enter_fraction + 0.01f;
	for (uint32_t window = 0; window < frame_synthesis_policy_thresholds::required_bad_windows; ++window)
	{
		for (uint32_t sample = 0; sample < frame_synthesis_policy_thresholds::window_samples; ++sample)
			decision = automatic.update(overloaded);
		if (window + 1 < frame_synthesis_policy_thresholds::required_bad_windows)
			check(!decision.full_synthesis, "Automatic entered before three windows");
	}
	check_full(decision);
	check(automatic.automatic_active(), "Automatic did not enter after three windows");

	frame_synthesis_metrics margin = good_metrics();
	margin.application_utilization = 0.80f;
	for (uint32_t sample = 0; sample < frame_synthesis_policy_thresholds::window_samples * 2; ++sample)
	{
		decision = automatic.update(margin);
		check_full(decision);
	}
	frame_synthesis_metrics underloaded = good_metrics();
	underloaded.application_utilization = frame_synthesis_policy_thresholds::automatic_exit_fraction - 0.01f;
	for (uint32_t window = 0; window < frame_synthesis_policy_thresholds::required_bad_windows; ++window)
	{
		for (uint32_t sample = 0; sample < frame_synthesis_policy_thresholds::window_samples; ++sample)
			decision = automatic.update(underloaded);
		if (window + 1 < frame_synthesis_policy_thresholds::required_bad_windows)
			check_full(decision);
	}
	check(decision.pose_only && decision.reason == frame_synthesis_reason::auto_waiting,
	      "Automatic did not leave after three low-load windows");
	check(!automatic.automatic_active(), "Automatic remained active below exit threshold");

	// A severe thermal sample is immediately pose-only; three bad windows make
	// the session fallback sticky until reset/reconnect.
	frame_synthesis_policy thermal_policy(frame_synthesis_policy_mode::always);
	frame_synthesis_metrics hot = good_metrics();
	hot.thermal_severity = frame_synthesis_policy_thresholds::thermal_severe_fraction + 0.01f;
	decision = thermal_policy.update(hot);
	check(decision.pose_only && !decision.disable_session, "thermal must first be pose-only");
	check(decision.reason == frame_synthesis_reason::thermal, "missing thermal reason");
	for (uint32_t sample = 1;
	     sample < frame_synthesis_policy_thresholds::required_bad_windows * frame_synthesis_policy_thresholds::window_samples;
	     ++sample)
		decision = thermal_policy.update(hot);
	check(decision.disable_session && decision.reset_history && thermal_policy.session_disabled(),
	      "three thermal windows must disable the session");
	thermal_policy.reset();
	check(!thermal_policy.session_disabled(), "reset must clear session fallback");
	check_full(thermal_policy.update(good_metrics()));

	// Pipeline deadline uses the 55% P95 threshold and the same three-window
	// sticky fallback.  Auto application utilization remains independent.
	frame_synthesis_policy deadline_policy(frame_synthesis_policy_mode::always);
	frame_synthesis_metrics late = good_metrics();
	late.pipeline_utilization = frame_synthesis_policy_thresholds::pipeline_deadline_p95_fraction + 0.01f;
	for (uint32_t window = 0; window < frame_synthesis_policy_thresholds::required_bad_windows; ++window)
	{
		for (uint32_t sample = 0; sample < frame_synthesis_policy_thresholds::window_samples; ++sample)
			decision = deadline_policy.update(late);
	}
	check(decision.disable_session && decision.reason == frame_synthesis_reason::deadline,
	      "three deadline windows must disable the session");

	// Off is explicit and does not synthesize even when metrics are perfect.
	frame_synthesis_policy off(frame_synthesis_policy_mode::off);
	decision = off.update(good_metrics());
	check(decision.pose_only && decision.reason == frame_synthesis_reason::mode_off,
	      "Off mode must remain pose-only");

	return 0;
}
