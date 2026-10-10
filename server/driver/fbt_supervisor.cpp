/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fbt_supervisor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wivrn
{
namespace
{
constexpr uint32_t required_relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
constexpr uint32_t allowed_relation_flags = XRT_SPACE_RELATION_BITMASK_ALL;

constexpr float epsilon = 1.0e-6f;

bool finite(float value)
{
	return std::isfinite(value);
}

bool finite(const xrt_vec3 & value)
{
	return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const xrt_quat & value)
{
	return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

float dot(const xrt_quat & a, const xrt_quat & b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

float norm(const xrt_quat & value)
{
	return std::sqrt(dot(value, value));
}

xrt_quat scale(const xrt_quat & value, float factor)
{
	return {value.x * factor, value.y * factor, value.z * factor, value.w * factor};
}

xrt_quat add(const xrt_quat & a, const xrt_quat & b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

xrt_quat nlerp(const xrt_quat & from, const xrt_quat & to, float alpha)
{
	// q and -q encode the same orientation.  Taking the shortest arc avoids
	// a full-turn interpolation when a producer flips quaternion sign.
	xrt_quat target = to;
	if (dot(from, target) < 0.0f)
		target = scale(target, -1.0f);
	xrt_quat result = add(scale(from, 1.0f - alpha), scale(target, alpha));
	const float length = norm(result);
	return length > epsilon ? scale(result, 1.0f / length) : from;
}

xrt_vec3 blend(const xrt_vec3 & from, const xrt_vec3 & to, float alpha)
{
	return {
	        from.x + (to.x - from.x) * alpha,
	        from.y + (to.y - from.y) * alpha,
	        from.z + (to.z - from.z) * alpha,
	};
}

xrt_vec3 subtract(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float length(const xrt_vec3 & value)
{
	return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

int64_t elapsed(int64_t timestamp_ns, int64_t start_ns)
{
	if (timestamp_ns <= start_ns)
		return 0;
	const auto value = timestamp_ns - start_ns;
	return value < 0 ? std::numeric_limits<int64_t>::max() : value;
}

float quaternion_angle(const xrt_quat & a, const xrt_quat & b)
{
	const float a_norm = norm(a);
	const float b_norm = norm(b);
	if (a_norm <= epsilon || b_norm <= epsilon)
		return 0.0f;
	const float cosine = std::clamp(std::abs(dot(a, b) / (a_norm * b_norm)), 0.0f, 1.0f);
	return 2.0f * std::acos(cosine);
}

float adaptive_alpha(const fbt_supervisor_config & config,
                     const xrt_space_relation & previous,
                     int64_t previous_timestamp_ns,
                     const xrt_space_relation & current,
                     float confidence,
                     int64_t timestamp_ns)
{
	if (!config.smoothing_enabled)
		return 1.0f;

	const float min_alpha = std::clamp(config.smoothing_min_alpha, 0.0f, 1.0f);
	const float max_alpha = std::max(min_alpha, std::clamp(config.smoothing_max_alpha, 0.0f, 1.0f));
	const float base_alpha = std::clamp(config.smoothing_alpha, min_alpha, max_alpha);
	float alpha = base_alpha * (0.45f + 0.55f * std::clamp(confidence, 0.0f, 1.0f));

	float motion = 0.0f;
	if ((static_cast<uint32_t>(current.relation_flags) & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0u)
	{
		if (config.max_linear_speed_m_s > epsilon)
			motion = std::max(motion, length(current.linear_velocity) / config.max_linear_speed_m_s);
	}
	if ((static_cast<uint32_t>(current.relation_flags) & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0u)
	{
		if (config.max_angular_speed_rad_s > epsilon)
		{
			const float angular_speed = length(current.angular_velocity);
			motion = std::max(motion, angular_speed / config.max_angular_speed_rad_s);
		}
	}

	const int64_t dt_ns = elapsed(timestamp_ns, previous_timestamp_ns);
	if (dt_ns > 0)
	{
		const float dt = static_cast<float>(dt_ns) * 1.0e-9f;
		if (config.max_linear_speed_m_s > epsilon)
			motion = std::max(motion, length(subtract(current.pose.position, previous.pose.position)) / dt / config.max_linear_speed_m_s);
		if (config.max_angular_speed_rad_s > epsilon)
			motion = std::max(motion, quaternion_angle(previous.pose.orientation, current.pose.orientation) / dt / config.max_angular_speed_rad_s);
	}

	motion = std::clamp(motion, 0.0f, 1.0f);
	alpha += motion * std::clamp(config.smoothing_motion_gain, 0.0f, 1.0f) * (1.0f - alpha);
	return std::clamp(alpha, min_alpha, max_alpha);
}

void blend_relation(const xrt_space_relation & previous, xrt_space_relation & current, float alpha)
{
	current.pose.position = blend(previous.pose.position, current.pose.position, alpha);
	current.pose.orientation = nlerp(previous.pose.orientation, current.pose.orientation, alpha);

	const uint32_t previous_flags = static_cast<uint32_t>(previous.relation_flags);
	const uint32_t current_flags = static_cast<uint32_t>(current.relation_flags);
	if ((previous_flags & current_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0u)
		current.linear_velocity = blend(previous.linear_velocity, current.linear_velocity, alpha);
	if ((previous_flags & current_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0u)
		current.angular_velocity = blend(previous.angular_velocity, current.angular_velocity, alpha);
}
} // namespace

fbt_supervisor::fbt_supervisor(fbt_supervisor_config config) : config_(config)
{
	reconfigure(config, false);
}

void fbt_supervisor::reconfigure(fbt_supervisor_config config, bool preserve_state)
{
	config_ = config;
	config_.minimum_confidence = std::clamp(config_.minimum_confidence, 0.0f, 1.0f);
	config_.hold_duration_ns = std::max<int64_t>(0, config_.hold_duration_ns);
	config_.hard_loss_duration_ns = std::max<int64_t>(0, config_.hard_loss_duration_ns);
	config_.recovery_samples = std::max<uint32_t>(1, config_.recovery_samples);
	config_.max_linear_speed_m_s = std::max(config_.max_linear_speed_m_s, epsilon);
	config_.max_angular_speed_rad_s = std::max(config_.max_angular_speed_rad_s, epsilon);
	config_.quaternion_norm_tolerance = std::clamp(config_.quaternion_norm_tolerance, 0.001f, 1.0f);
	config_.smoothing_min_alpha = std::clamp(config_.smoothing_min_alpha, 0.0f, 1.0f);
	config_.smoothing_max_alpha = std::clamp(config_.smoothing_max_alpha, config_.smoothing_min_alpha, 1.0f);
	config_.smoothing_alpha = std::clamp(config_.smoothing_alpha, config_.smoothing_min_alpha, config_.smoothing_max_alpha);
	config_.smoothing_motion_gain = std::clamp(config_.smoothing_motion_gain, 0.0f, 1.0f);
	if (!preserve_state)
		clear_lifecycle();
}

fbt_supervisor::validation fbt_supervisor::validate(const xrt_space_relation & relation, float confidence) const
{
	const uint32_t flags = static_cast<uint32_t>(relation.relation_flags);
	if ((flags & required_relation_flags) != required_relation_flags || (flags & ~allowed_relation_flags) != 0u ||
	    ((flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) != 0u &&
	     (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0u) ||
	    ((flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0u && (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) == 0u))
		return {false, fbt_supervisor_reason::invalid_flags};

	if (!finite(relation.pose.orientation) || !finite(relation.pose.position) ||
	    ((flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0u && !finite(relation.linear_velocity)) ||
	    ((flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0u && !finite(relation.angular_velocity)) || !finite(confidence))
		return {false, fbt_supervisor_reason::non_finite};

	const float quaternion_norm = norm(relation.pose.orientation);
	if (!finite(quaternion_norm) || std::abs(quaternion_norm - 1.0f) > config_.quaternion_norm_tolerance)
		return {false, fbt_supervisor_reason::invalid_quaternion};

	if (confidence < 0.0f || confidence > 1.0f)
		return {false, fbt_supervisor_reason::invalid_confidence};
	if (confidence < config_.minimum_confidence)
		return {false, fbt_supervisor_reason::low_confidence};

	if ((flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0u &&
	    length(relation.linear_velocity) > config_.max_linear_speed_m_s)
		return {false, fbt_supervisor_reason::excessive_linear_velocity};
	if ((flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0u &&
	    length(relation.angular_velocity) > config_.max_angular_speed_rad_s)
		return {false, fbt_supervisor_reason::excessive_angular_velocity};

	return {true, fbt_supervisor_reason::accepted};
}

void fbt_supervisor::record_reason(fbt_supervisor_reason reason)
{
	metrics_.last_reason = reason;
	const auto index = static_cast<size_t>(reason);
	if (index < metrics_.reason_counts.size())
		metrics_.reason_counts[index]++;
}

void fbt_supervisor::clear_lifecycle()
{
	state_ = fbt_supervisor_state::warming_up;
	have_timestamp_ = false;
	last_timestamp_ns_ = 0;
	evaluation_timestamp_ns_ = 0;
	have_valid_pose_ = false;
	last_valid_timestamp_ns_ = 0;
	dropout_started_ns_ = 0;
	dropout_active_ = false;
	hold_expired_ = false;
	recovery_valid_samples_ = 0;
	have_recovery_pose_ = false;
	filtered_ = XRT_SPACE_RELATION_ZERO;
	recovery_filtered_ = XRT_SPACE_RELATION_ZERO;
	recovery_timestamp_ns_ = 0;
	filtered_confidence_ = 0.0f;
	filtered_timestamp_ns_ = 0;
}

void fbt_supervisor::reset()
{
	clear_lifecycle();
	metrics_ = {};
	metrics_.resets = 1;
	metrics_.last_input_reason = fbt_supervisor_reason::reset;
	record_reason(fbt_supervisor_reason::reset);
}

void fbt_supervisor::normalize_relation(xrt_space_relation & relation) const
{
	const float quaternion_norm = norm(relation.pose.orientation);
	if (quaternion_norm > epsilon)
		relation.pose.orientation = scale(relation.pose.orientation, 1.0f / quaternion_norm);
}

float fbt_supervisor::adaptive_alpha(const xrt_space_relation & relation, float confidence, int64_t timestamp_ns) const
{
	return ::wivrn::adaptive_alpha(config_, filtered_, filtered_timestamp_ns_, relation, confidence, timestamp_ns);
}

void fbt_supervisor::smooth_into(const xrt_space_relation & relation, float confidence, int64_t timestamp_ns)
{
	xrt_space_relation current = relation;
	normalize_relation(current);
	if (!have_valid_pose_)
	{
		filtered_ = current;
		filtered_timestamp_ns_ = timestamp_ns;
		filtered_confidence_ = confidence;
		have_valid_pose_ = true;
		return;
	}

	// META body locations omit velocity. Derive a finite-difference linear
	// velocity from accepted poses so bounded server-side prediction can still
	// be used without asking the game for motion vectors.
	const int64_t dt_ns = elapsed(timestamp_ns, filtered_timestamp_ns_);
	if ((current.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) == 0 && dt_ns > 0)
	{
		const float inverse_dt = 1.0e9f / static_cast<float>(dt_ns);
		const auto delta = subtract(current.pose.position, filtered_.pose.position);
		current.linear_velocity = {delta.x * inverse_dt, delta.y * inverse_dt, delta.z * inverse_dt};
		current.relation_flags = static_cast<xrt_space_relation_flags>(
		        current.relation_flags | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
	}

	const float alpha = adaptive_alpha(current, confidence, timestamp_ns);
	blend_relation(filtered_, current, alpha);
	filtered_ = current;
	filtered_timestamp_ns_ = timestamp_ns;
	filtered_confidence_ = confidence;
	metrics_.smoothing_updates++;
}

fbt_supervisor_result fbt_supervisor::make_result(fbt_supervisor_reason reason,
                                                  bool input_valid,
                                                  bool accepted,
                                                  float confidence) const
{
	fbt_supervisor_result result{};
	result.state = state_;
	result.reason = reason;
	result.input_valid = input_valid;
	result.accepted = accepted;
	result.confidence = confidence;
	result.consecutive_recovery_samples = recovery_valid_samples_;

	if (state_ == fbt_supervisor_state::holding || state_ == fbt_supervisor_state::lost ||
	    state_ == fbt_supervisor_state::recovering)
		result.age_ns = elapsed(evaluation_timestamp_ns_, dropout_started_ns_);

	const bool held = state_ == fbt_supervisor_state::holding && have_valid_pose_ &&
	                  result.age_ns <= config_.hold_duration_ns;
	const bool tracking = state_ == fbt_supervisor_state::tracking && have_valid_pose_;
	if (held || tracking)
	{
		result.relation = filtered_;
		result.output_valid = true;
		result.held = held;
	}
	result.output = result.relation;
	return result;
}

fbt_supervisor_result fbt_supervisor::invalid_sample(int64_t timestamp_ns,
                                                     fbt_supervisor_reason input_reason,
                                                     float confidence)
{
	metrics_.rejected_samples++;
	metrics_.last_input_reason = input_reason;

	if (!have_valid_pose_)
	{
		state_ = fbt_supervisor_state::warming_up;
		recovery_valid_samples_ = 0;
		have_recovery_pose_ = false;
		record_reason(input_reason);
		return make_result(input_reason, false, false, confidence);
	}

	if (state_ == fbt_supervisor_state::tracking)
	{
		state_ = fbt_supervisor_state::holding;
		dropout_started_ns_ = timestamp_ns;
		dropout_active_ = true;
		hold_expired_ = false;
	}
	else if (state_ == fbt_supervisor_state::recovering)
	{
		state_ = fbt_supervisor_state::lost;
		recovery_valid_samples_ = 0;
		have_recovery_pose_ = false;
		dropout_started_ns_ = timestamp_ns;
		dropout_active_ = true;
		hold_expired_ = true;
	}
	else if (!dropout_active_)
	{
		dropout_started_ns_ = timestamp_ns;
		dropout_active_ = true;
	}

	const int64_t age_ns = elapsed(timestamp_ns, dropout_started_ns_);
	if (age_ns >= config_.hard_loss_duration_ns)
	{
		if (state_ != fbt_supervisor_state::lost)
		{
			state_ = fbt_supervisor_state::lost;
			metrics_.hard_losses++;
		}
		recovery_valid_samples_ = 0;
		have_recovery_pose_ = false;
		record_reason(fbt_supervisor_reason::hard_loss);
		return make_result(fbt_supervisor_reason::hard_loss, false, false, confidence);
	}

	if (age_ns > config_.hold_duration_ns)
	{
		if (!hold_expired_)
		{
			hold_expired_ = true;
			metrics_.hold_expirations++;
		}
		record_reason(fbt_supervisor_reason::hold_expired);
		return make_result(fbt_supervisor_reason::hold_expired, false, false, confidence);
	}

	metrics_.held_samples++;
	record_reason(input_reason);
	return make_result(input_reason, false, false, confidence);
}

fbt_supervisor_result fbt_supervisor::valid_sample(int64_t timestamp_ns,
                                                   const xrt_space_relation & relation,
                                                   float confidence)
{
	metrics_.last_input_reason = fbt_supervisor_reason::accepted;

	if (state_ == fbt_supervisor_state::lost)
	{
		state_ = fbt_supervisor_state::recovering;
		recovery_valid_samples_ = 1;
		recovery_filtered_ = relation;
		normalize_relation(recovery_filtered_);
		recovery_timestamp_ns_ = timestamp_ns;
		have_recovery_pose_ = true;
		metrics_.recovery_attempts++;
		metrics_.recovery_samples++;
	}
	else if (state_ == fbt_supervisor_state::recovering)
	{
		const float alpha = ::wivrn::adaptive_alpha(config_,
		                                            recovery_filtered_,
		                                            recovery_timestamp_ns_,
		                                            relation,
		                                            confidence,
		                                            timestamp_ns);
		xrt_space_relation current = relation;
		normalize_relation(current);
		blend_relation(recovery_filtered_, current, alpha);
		recovery_filtered_ = current;
		recovery_timestamp_ns_ = timestamp_ns;
		recovery_valid_samples_++;
		metrics_.recovery_samples++;
	}
	else
	{
		smooth_into(relation, confidence, timestamp_ns);
	}

	if (state_ == fbt_supervisor_state::recovering)
	{
		if (recovery_valid_samples_ < config_.recovery_samples)
		{
			metrics_.accepted_samples++;
			record_reason(fbt_supervisor_reason::recovery_pending);
			return make_result(fbt_supervisor_reason::recovery_pending, true, true, confidence);
		}

		filtered_ = recovery_filtered_;
		filtered_timestamp_ns_ = timestamp_ns;
		filtered_confidence_ = confidence;
		have_valid_pose_ = true;
		last_valid_timestamp_ns_ = timestamp_ns;
		recovery_valid_samples_ = 0;
		have_recovery_pose_ = false;
		state_ = fbt_supervisor_state::tracking;
		dropout_started_ns_ = 0;
		dropout_active_ = false;
		hold_expired_ = false;
		metrics_.accepted_samples++;
		metrics_.recoveries++;
		record_reason(fbt_supervisor_reason::recovered);
		return make_result(fbt_supervisor_reason::recovered, true, true, confidence);
	}

	const bool had_dropout = state_ == fbt_supervisor_state::holding;
	state_ = fbt_supervisor_state::tracking;
	last_valid_timestamp_ns_ = timestamp_ns;
	dropout_started_ns_ = 0;
	dropout_active_ = false;
	hold_expired_ = false;
	metrics_.accepted_samples++;
	record_reason(had_dropout ? fbt_supervisor_reason::recovered : fbt_supervisor_reason::accepted);
	return make_result(had_dropout ? fbt_supervisor_reason::recovered : fbt_supervisor_reason::accepted,
	                   true,
	                   true,
	                   confidence);
}

fbt_supervisor_result fbt_supervisor::update(int64_t timestamp_ns,
                                             const xrt_space_relation & relation,
                                             float confidence)
{
	metrics_.samples++;

	if (timestamp_ns < 0)
	{
		metrics_.rejected_samples++;
		metrics_.last_input_reason = fbt_supervisor_reason::invalid_timestamp;
		record_reason(fbt_supervisor_reason::invalid_timestamp);
		return make_result(fbt_supervisor_reason::invalid_timestamp, false, false, confidence);
	}
	if (have_timestamp_ && timestamp_ns <= last_timestamp_ns_)
	{
		metrics_.reordered_samples++;
		metrics_.last_input_reason = fbt_supervisor_reason::reordered_timestamp;
		record_reason(fbt_supervisor_reason::reordered_timestamp);
		auto result = make_result(fbt_supervisor_reason::reordered_timestamp, false, false, confidence);
		result.reordered = true;
		return result;
	}

	have_timestamp_ = true;
	last_timestamp_ns_ = timestamp_ns;
	evaluation_timestamp_ns_ = timestamp_ns;
	const validation checked = validate(relation, confidence);
	if (!checked.valid)
		return invalid_sample(timestamp_ns, checked.reason, confidence);

	// META body packets do not carry velocities. Derive conservative apparent
	// speeds from consecutive poses as well, otherwise a one-frame teleport can
	// pass validation simply because the velocity-valid flags are absent. Once
	// tracking is fully lost we deliberately allow recovery at a new origin.
	if (have_valid_pose_ && (state_ == fbt_supervisor_state::tracking || state_ == fbt_supervisor_state::holding))
	{
		const int64_t dt_ns = elapsed(timestamp_ns, filtered_timestamp_ns_);
		if (dt_ns > 0)
		{
			const float dt = static_cast<float>(dt_ns) * 1.0e-9f;
			if (length(subtract(relation.pose.position, filtered_.pose.position)) / dt > config_.max_linear_speed_m_s)
				return invalid_sample(timestamp_ns, fbt_supervisor_reason::excessive_linear_velocity, confidence);
			if (quaternion_angle(filtered_.pose.orientation, relation.pose.orientation) / dt > config_.max_angular_speed_rad_s)
				return invalid_sample(timestamp_ns, fbt_supervisor_reason::excessive_angular_velocity, confidence);
		}
	}
	return valid_sample(timestamp_ns, relation, confidence);
}

fbt_supervisor_result fbt_supervisor::timeout(int64_t timestamp_ns)
{
	if (!have_valid_pose_ || state_ == fbt_supervisor_state::warming_up || state_ == fbt_supervisor_state::tracking)
		return make_result(fbt_supervisor_reason::none, false, false, filtered_confidence_);

	if (state_ == fbt_supervisor_state::recovering)
	{
		record_reason(fbt_supervisor_reason::recovery_pending);
		return make_result(fbt_supervisor_reason::recovery_pending, false, false, filtered_confidence_);
	}

	const int64_t age_ns = elapsed(timestamp_ns, dropout_started_ns_);
	if (state_ != fbt_supervisor_state::lost && age_ns >= config_.hard_loss_duration_ns)
	{
		state_ = fbt_supervisor_state::lost;
		metrics_.hard_losses++;
		recovery_valid_samples_ = 0;
		have_recovery_pose_ = false;
		record_reason(fbt_supervisor_reason::hard_loss);
		return make_result(fbt_supervisor_reason::hard_loss, false, false, filtered_confidence_);
	}
	if (state_ == fbt_supervisor_state::lost)
	{
		record_reason(fbt_supervisor_reason::hard_loss);
		return make_result(fbt_supervisor_reason::hard_loss, false, false, filtered_confidence_);
	}
	if (age_ns > config_.hold_duration_ns)
	{
		if (!hold_expired_)
		{
			hold_expired_ = true;
			metrics_.hold_expirations++;
		}
		record_reason(fbt_supervisor_reason::hold_expired);
		return make_result(fbt_supervisor_reason::hold_expired, false, false, filtered_confidence_);
	}

	metrics_.held_samples++;
	record_reason(fbt_supervisor_reason::held_dropout);
	return make_result(fbt_supervisor_reason::held_dropout, false, false, filtered_confidence_);
}

fbt_supervisor_result fbt_supervisor::tick(int64_t timestamp_ns)
{
	metrics_.timeout_polls++;
	if (timestamp_ns < 0)
	{
		metrics_.last_input_reason = fbt_supervisor_reason::invalid_timestamp;
		record_reason(fbt_supervisor_reason::invalid_timestamp);
		return make_result(fbt_supervisor_reason::invalid_timestamp, false, false, filtered_confidence_);
	}
	if (timestamp_ns > evaluation_timestamp_ns_)
		evaluation_timestamp_ns_ = timestamp_ns;
	return timeout(timestamp_ns);
}

} // namespace wivrn
