/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frame_synthesis_policy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wivrn
{
namespace
{
constexpr float unset = -1.0f;

bool finite(float value)
{
	return std::isfinite(value);
}

bool normalized(float value)
{
	return finite(value) && value >= 0.0f && value <= 1.0f;
}

// Optional aliases use a negative sentinel.  NaN is not a valid alias and is
// deliberately ignored when the primary metric is available; a NaN primary
// metric is rejected by validate_metrics below.
float alias_or(float primary, float alias)
{
	return finite(alias) && alias >= 0.0f ? alias : primary;
}

float pose_delta(const frame_synthesis_metrics & metrics)
{
	return alias_or(metrics.pose_delta_degrees, metrics.pose_degrees);
}

float holes(const frame_synthesis_metrics & metrics)
{
	return alias_or(metrics.hole_fraction, metrics.holes);
}

float flow_p95(const frame_synthesis_metrics & metrics)
{
	return alias_or(metrics.flow_p95_fraction, metrics.flow_p95);
}

float thermal(const frame_synthesis_metrics & metrics)
{
	return alias_or(metrics.thermal_severity, metrics.thermal);
}

// A separately reported pipeline value is preferred.  The one-field
// `deadline` adapter is intentionally accepted as both pipeline and
// application utilization.  `deadline_utilization` itself remains the Auto
// application-load signal and does not accidentally trip the 55% pipeline
// watchdog when no pipeline measurement was supplied.
float pipeline_utilization(const frame_synthesis_metrics & metrics)
{
	if (metrics.pipeline_utilization >= 0.0f)
		return metrics.pipeline_utilization;
	if (metrics.deadline >= 0.0f)
		return metrics.deadline;
	return unset;
}

float application_utilization(const frame_synthesis_metrics & metrics)
{
	if (metrics.application_utilization >= 0.0f)
		return metrics.application_utilization;
	if (metrics.deadline >= 0.0f)
		return metrics.deadline;
	return metrics.deadline_utilization;
}

bool validate_metrics(const frame_synthesis_metrics & metrics)
{
	if (!metrics.valid)
		return false;

	const float pose = pose_delta(metrics);
	const float hole = holes(metrics);
	const float flow = flow_p95(metrics);
	const float hot = thermal(metrics);
	const float pipeline = pipeline_utilization(metrics);
	const float application = application_utilization(metrics);
	if (!finite(pose) || pose < 0.0f)
		return false;
	if (!normalized(metrics.correlation) || !normalized(metrics.histogram_distance))
		return false;
	if (!normalized(metrics.consistency_bad_fraction) || !finite(metrics.consistency_error_px) ||
	    metrics.consistency_error_px < 0.0f)
		return false;
	if (!normalized(hole) || !normalized(flow) || !normalized(hot))
		return false;
	// Deadline values are allowed above 1.0: that is an over-budget sample,
	// not malformed input.  Negative values are reserved for optional fields.
	if (pipeline != unset && (!finite(pipeline) || pipeline < 0.0f))
		return false;
	if (!finite(application) || application < 0.0f)
		return false;
	return true;
}

frame_synthesis_reason cut_reason(const frame_synthesis_metrics & metrics)
{
	if (!metrics.generation_continuous || !metrics.fov_continuous)
		return frame_synthesis_reason::scene_cut;
	if (pose_delta(metrics) > frame_synthesis_policy_thresholds::scene_cut_pose_degrees)
		return frame_synthesis_reason::scene_cut;
	if (metrics.correlation < frame_synthesis_policy_thresholds::scene_cut_correlation &&
	    metrics.histogram_distance > frame_synthesis_policy_thresholds::scene_cut_histogram_distance)
		return frame_synthesis_reason::scene_cut;
	return frame_synthesis_reason::none;
}

frame_synthesis_decision make_output_decision(frame_synthesis_action action,
                                              frame_synthesis_reason reason,
                                              bool reset_history = false,
                                              bool disable_session = false)
{
	frame_synthesis_decision result;
	result.action = action;
	result.reason = reason;
	result.full_synthesis = action == frame_synthesis_action::full_synthesis;
	result.conservative_synthesis = action == frame_synthesis_action::conservative_synthesis;
	result.pose_only = action == frame_synthesis_action::pose_only;
	result.reset_history = reset_history;
	result.disable_session = disable_session;
	return result;
}

int severity(frame_synthesis_classification_kind kind)
{
	switch (kind)
	{
		case frame_synthesis_classification_kind::stable:
			return 0;
		case frame_synthesis_classification_kind::global_fast_motion:
			return 1;
		case frame_synthesis_classification_kind::low_confidence:
			return 2;
		case frame_synthesis_classification_kind::thermal:
		case frame_synthesis_classification_kind::deadline:
			return 3;
		case frame_synthesis_classification_kind::scene_cut:
			return 4;
		case frame_synthesis_classification_kind::invalid:
			return 5;
	}
	return 5;
}

frame_synthesis_classification combine(const frame_synthesis_classification & left,
                                       const frame_synthesis_classification & right)
{
	const auto & worst = severity(left.kind) >= severity(right.kind) ? left : right;
	frame_synthesis_classification result = worst;
	result.confidence = std::min(left.confidence, right.confidence);
	return result;
}

float p95(const std::array<float, frame_synthesis_policy_thresholds::window_samples> & values)
{
	std::vector<float> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	const size_t rank = (sorted.size() * 95u - 1u) / 100u;
	return sorted[rank];
}

} // namespace

frame_synthesis_classification classify_frame_synthesis(const frame_synthesis_metrics & metrics)
{
	if (!validate_metrics(metrics))
		return {frame_synthesis_classification_kind::invalid, frame_synthesis_reason::invalid_metric, 0.0f};

	if (const auto reason = cut_reason(metrics); reason != frame_synthesis_reason::none)
		return {frame_synthesis_classification_kind::scene_cut, reason, 0.0f};

	const float hot = thermal(metrics);
	if (hot >= frame_synthesis_policy_thresholds::thermal_severe_fraction)
		return {frame_synthesis_classification_kind::thermal, frame_synthesis_reason::thermal, 0.0f};

	const float pipeline = pipeline_utilization(metrics);
	if (metrics.deadline_missed ||
	    (pipeline != unset && pipeline > frame_synthesis_policy_thresholds::pipeline_deadline_p95_fraction))
		return {frame_synthesis_classification_kind::deadline, frame_synthesis_reason::deadline, 0.0f};

	const float hole = holes(metrics);
	if (metrics.consistency_bad_fraction > frame_synthesis_policy_thresholds::low_confidence_bad_fraction ||
	    metrics.consistency_error_px > frame_synthesis_policy_thresholds::low_confidence_error_pixels ||
    hole > frame_synthesis_policy_thresholds::low_confidence_hole_fraction ||
    metrics.correlation < frame_synthesis_policy_thresholds::scene_cut_correlation ||
    metrics.histogram_distance > frame_synthesis_policy_thresholds::scene_cut_histogram_distance)
	{
		const float confidence = std::min({1.0f - metrics.consistency_bad_fraction, 1.0f - hole, metrics.correlation});
		return {frame_synthesis_classification_kind::low_confidence,
		        frame_synthesis_reason::low_confidence,
		        std::max(0.0f, confidence)};
	}

	const float flow = flow_p95(metrics);
	if (flow > frame_synthesis_policy_thresholds::extreme_flow_fraction)
	{
		const float confidence = std::max(0.0f, 1.0f - flow);
		return {frame_synthesis_classification_kind::global_fast_motion,
		        frame_synthesis_reason::extreme_motion,
		        confidence};
	}

	const float confidence = std::min({metrics.correlation, 1.0f - hole, 1.0f - metrics.consistency_bad_fraction});
	return {frame_synthesis_classification_kind::stable, frame_synthesis_reason::none, confidence};
}

frame_synthesis_policy::frame_synthesis_policy(frame_synthesis_policy_mode mode) : mode_(mode)
{
	automatic_active_ = mode_ == frame_synthesis_policy_mode::always;
}

void frame_synthesis_policy::set_mode(frame_synthesis_policy_mode mode)
{
	mode_ = mode;
	automatic_active_ = mode == frame_synthesis_policy_mode::always;
	auto_enter_windows_ = 0;
	auto_exit_windows_ = 0;
}

void frame_synthesis_policy::reset()
{
	automatic_active_ = mode_ == frame_synthesis_policy_mode::always;
	session_disabled_ = false;
	sample_count_ = 0;
	missed_intervals_ = 0;
	bad_windows_ = 0;
	auto_enter_windows_ = 0;
	auto_exit_windows_ = 0;
	safety_reason_ = frame_synthesis_reason::none;
	pipeline_samples_.fill(0.0f);
	application_samples_.fill(0.0f);
	thermal_bad_.fill(false);
	deadline_bad_.fill(false);
}

frame_synthesis_decision frame_synthesis_policy::update(const frame_synthesis_metrics & metrics)
{
	frame_synthesis_stereo_metrics stereo;
	stereo.left = metrics;
	stereo.right = metrics;
	return update(stereo);
}

frame_synthesis_decision frame_synthesis_policy::update(const frame_synthesis_stereo_metrics & metrics)
{
	const size_t index = sample_count_;
	const auto record = [](const frame_synthesis_metrics & eye) {
		const float pipeline = pipeline_utilization(eye);
		const float application = application_utilization(eye);
		return std::array<float, 4>{pipeline == unset ? 0.0f : std::max(0.0f, pipeline),
		                           std::max(0.0f, application),
		                           thermal(eye) >= frame_synthesis_policy_thresholds::thermal_severe_fraction ? 1.0f : 0.0f,
		                           (eye.deadline_missed ||
		                            (pipeline != unset && pipeline > frame_synthesis_policy_thresholds::pipeline_deadline_p95_fraction))
		                                   ? 1.0f
		                                   : 0.0f};
	};

	const auto left = record(metrics.left);
	const auto right = record(metrics.right);
	pipeline_samples_[index] = std::max(left[0], right[0]);
	application_samples_[index] = std::max(left[1], right[1]);
	thermal_bad_[index] = left[2] != 0.0f || right[2] != 0.0f;
	deadline_bad_[index] = left[3] != 0.0f || right[3] != 0.0f;
	if (metrics.left.deadline_missed || metrics.right.deadline_missed)
		++missed_intervals_;
	++sample_count_;

	if (sample_count_ == thresholds::window_samples)
		record_window();

	return make_decision(metrics);
}

void frame_synthesis_policy::record_window()
{
	const float pipeline_p95 = p95(pipeline_samples_);
	const float application_p95 = p95(application_samples_);
	const float missed_fraction = static_cast<float>(missed_intervals_) /
	                              static_cast<float>(thresholds::window_samples);
	const bool thermal_bad = std::any_of(thermal_bad_.begin(), thermal_bad_.end(), [](bool bad) { return bad; });
	const bool deadline_bad = pipeline_p95 > thresholds::pipeline_deadline_p95_fraction ||
	                          missed_fraction > thresholds::missed_interval_fraction;

	if (thermal_bad || deadline_bad)
	{
		++bad_windows_;
		safety_reason_ = thermal_bad ? frame_synthesis_reason::thermal : frame_synthesis_reason::deadline;
	}
	else
	{
		bad_windows_ = 0;
		safety_reason_ = frame_synthesis_reason::none;
	}
	if (bad_windows_ >= thresholds::required_bad_windows)
		session_disabled_ = true;

	if (mode_ == frame_synthesis_policy_mode::automatic && !session_disabled_)
	{
		if (!automatic_active_)
		{
			if (application_p95 > thresholds::automatic_enter_fraction)
				++auto_enter_windows_;
			else
				auto_enter_windows_ = 0;
			auto_exit_windows_ = 0;
			if (auto_enter_windows_ >= thresholds::required_bad_windows)
			{
				automatic_active_ = true;
				auto_enter_windows_ = 0;
			}
		}
		else
		{
			if (application_p95 < thresholds::automatic_exit_fraction)
				++auto_exit_windows_;
			else
				auto_exit_windows_ = 0;
			auto_enter_windows_ = 0;
			if (auto_exit_windows_ >= thresholds::required_bad_windows)
			{
				automatic_active_ = false;
				auto_exit_windows_ = 0;
			}
		}
	}

	sample_count_ = 0;
	missed_intervals_ = 0;
	pipeline_samples_.fill(0.0f);
	application_samples_.fill(0.0f);
	thermal_bad_.fill(false);
	deadline_bad_.fill(false);
}

frame_synthesis_decision frame_synthesis_policy::make_decision(const frame_synthesis_stereo_metrics & metrics)
{
	frame_synthesis_decision result;
	result.samples_in_window = sample_count_;
	result.consecutive_bad_windows = bad_windows_;
	result.automatic_active = automatic_active_;

	if (session_disabled_)
	{
		result = make_output_decision(frame_synthesis_action::pose_only,
		                      safety_reason_ == frame_synthesis_reason::none ? frame_synthesis_reason::session_disabled : safety_reason_,
		                      true,
		                      true);
	}
	else if (!metrics.stereo_valid)
	{
		result = make_output_decision(frame_synthesis_action::pose_only,
		                      frame_synthesis_reason::stereo_mismatch,
		                      true);
	}
	else if (!metrics.history_valid || !metrics.left.history_valid || !metrics.right.history_valid)
	{
		result = make_output_decision(frame_synthesis_action::pose_only,
		                      frame_synthesis_reason::insufficient_history,
		                      true);
	}
	else if (!metrics.generation_continuous || !metrics.fov_continuous)
	{
		result = make_output_decision(frame_synthesis_action::pose_only,
		                      frame_synthesis_reason::scene_cut,
		                      true);
	}
	else
	{
		const auto left = classify_frame_synthesis(metrics.left);
		const auto right = classify_frame_synthesis(metrics.right);
		const auto classification = combine(left, right);
		const bool unsafe = classification.kind == frame_synthesis_classification_kind::invalid ||
		                    classification.kind == frame_synthesis_classification_kind::scene_cut ||
		                    classification.kind == frame_synthesis_classification_kind::thermal ||
		                    classification.kind == frame_synthesis_classification_kind::deadline ||
		                    classification.kind == frame_synthesis_classification_kind::low_confidence;

		if (classification.kind == frame_synthesis_classification_kind::invalid)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::invalid_metric, true);
		else if (classification.kind == frame_synthesis_classification_kind::scene_cut)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::scene_cut, true);
		else if (classification.kind == frame_synthesis_classification_kind::thermal)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::thermal);
		else if (classification.kind == frame_synthesis_classification_kind::deadline)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::deadline);
		else if (classification.kind == frame_synthesis_classification_kind::low_confidence)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::low_confidence);
		else if (mode_ == frame_synthesis_policy_mode::off)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::mode_off);
		else if (mode_ == frame_synthesis_policy_mode::automatic && !automatic_active_)
			result = make_output_decision(frame_synthesis_action::pose_only, frame_synthesis_reason::auto_waiting);
		else if (classification.kind == frame_synthesis_classification_kind::global_fast_motion)
			result = make_output_decision(frame_synthesis_action::conservative_synthesis,
		                      frame_synthesis_reason::extreme_motion);
		else if (!unsafe)
			result = make_output_decision(frame_synthesis_action::full_synthesis, frame_synthesis_reason::none);
	}

	result.samples_in_window = sample_count_;
	result.consecutive_bad_windows = bad_windows_;
	result.automatic_active = automatic_active_;
	return result;
}

} // namespace wivrn
