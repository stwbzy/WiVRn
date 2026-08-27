/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <array>
#include <cstdint>

namespace wivrn
{

/**
 * The lifecycle of one body articulation.  A supervisor is deliberately
 * instantiated per joint: losing one joint must not make an otherwise useful
 * body sample unusable.
 */
enum class fbt_supervisor_state : uint8_t
{
	warming_up,
	tracking,
	holding,
	lost,
	recovering,
};

/**
 * Why a sample or a state transition was reported by the supervisor.
 *
 * Reason values describe the input and the safety decision, rather than an
 * implementation detail.  They are suitable for counters and telemetry.
 */
enum class fbt_supervisor_reason : uint8_t
{
	none,
	accepted,
	warming_up,
	reordered_timestamp,
	invalid_timestamp,
	invalid_flags,
	non_finite,
	invalid_quaternion,
	low_confidence,
	invalid_confidence,
	excessive_linear_velocity,
	excessive_angular_velocity,
	held_dropout,
	hold_expired,
	hard_loss,
	recovery_pending,
	recovered,
	reset,

	count,

	// Friendly aliases used by callers that classify a bad input rather than
	// a state transition.
	invalid_relation = invalid_flags,
	non_finite_relation = non_finite,
	linear_velocity_spike = excessive_linear_velocity,
	angular_velocity_spike = excessive_angular_velocity,
};

/** Conservative defaults are intentionally kept in one public contract. */
struct fbt_supervisor_config
{
	// A joint with less confidence than this is treated as unavailable.
	float minimum_confidence = 0.5f;

	// Keep the last good relation valid for this long.  After this interval a
	// relation returned by update/tick has no valid flags, while the lifecycle
	// remains holding until hard_loss_duration_ns.
	int64_t hold_duration_ns = 100'000'000;

	// A dropout becomes a hard loss at this age.  This is intentionally longer
	// than hold_duration_ns to avoid brief network gaps causing a reset.
	int64_t hard_loss_duration_ns = 500'000'000;

	// Number of consecutive valid samples needed after a hard loss.
	uint32_t recovery_samples = 3;

	float max_linear_speed_m_s = 8.0f;
	float max_angular_speed_rad_s = 20.0f;

	// The accepted quaternion norm must be within this distance of one.  The
	// output is normalized, so small numerical drift never reaches consumers.
	float quaternion_norm_tolerance = 0.1f;

	// Exponential smoothing uses only the previous output and the current
	// sample.  Motion raises alpha (less lag); low confidence lowers alpha.
	bool smoothing_enabled = true;
	float smoothing_alpha = 0.35f;
	float smoothing_min_alpha = 0.12f;
	float smoothing_max_alpha = 0.90f;
	float smoothing_motion_gain = 0.75f;
};

/** Counters are per supervisor and reset by reset(). */
struct fbt_supervisor_metrics
{
	uint64_t samples = 0;
	uint64_t accepted_samples = 0;
	uint64_t rejected_samples = 0;
	uint64_t reordered_samples = 0;
	uint64_t held_samples = 0;
	uint64_t hold_expirations = 0;
	uint64_t hard_losses = 0;
	uint64_t recovery_attempts = 0;
	uint64_t recovery_samples = 0;
	uint64_t recoveries = 0;
	uint64_t smoothing_updates = 0;
	uint64_t resets = 0;
	uint64_t timeout_polls = 0;

	// Index with static_cast<size_t>(fbt_supervisor_reason).  Aliases map to
	// the same bucket.  The final count value itself is not a valid index.
	std::array<uint64_t, static_cast<size_t>(fbt_supervisor_reason::count)> reason_counts{};
	fbt_supervisor_reason last_reason = fbt_supervisor_reason::none;
	fbt_supervisor_reason last_input_reason = fbt_supervisor_reason::none;
};

/** Result of processing one packet or advancing the timeout clock. */
struct fbt_supervisor_result
{
	fbt_supervisor_state state = fbt_supervisor_state::warming_up;
	fbt_supervisor_reason reason = fbt_supervisor_reason::none;

	// `relation` is the relation safe for publication. `output` is a named
	// alias kept for adapters that use output terminology; both are identical.
	xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	xrt_space_relation output = XRT_SPACE_RELATION_ZERO;

	bool input_valid = false;
	bool accepted = false;
	bool output_valid = false;
	bool held = false;
	bool reordered = false;
	uint32_t consecutive_recovery_samples = 0;
	int64_t age_ns = 0;
	float confidence = 0.0f;

	bool publishable() const
	{
		return output_valid;
	}
};

/**
 * Validates and supervises one xrt_space_relation at a time.
 *
 * This class has no dependency on a tracker, clock or queue and is therefore
 * usable from both wivrn_body_tracker and wivrn_generic_tracker.  Timestamps
 * are sample timestamps in nanoseconds; `tick` advances timeout state without
 * consuming a packet (and therefore does not affect packet reordering).
 */
class fbt_supervisor
{
public:
	using state = fbt_supervisor_state;
	using reason = fbt_supervisor_reason;
	using config = fbt_supervisor_config;
	using result = fbt_supervisor_result;
	using metrics_type = fbt_supervisor_metrics;

	explicit fbt_supervisor(fbt_supervisor_config config = {});

	// The canonical integration entry point.
	fbt_supervisor_result update(int64_t timestamp_ns, const xrt_space_relation & relation, float confidence = 1.0f);

	// Argument-order aliases make adapters with an existing relation-first API
	// straightforward to migrate without a wrapper object.
	fbt_supervisor_result update(const xrt_space_relation & relation, int64_t timestamp_ns, float confidence = 1.0f)
	{
		return update(timestamp_ns, relation, confidence);
	}
	fbt_supervisor_result update(int64_t timestamp_ns, float confidence, const xrt_space_relation & relation)
	{
		return update(timestamp_ns, relation, confidence);
	}
	fbt_supervisor_result submit(int64_t timestamp_ns, const xrt_space_relation & relation, float confidence = 1.0f)
	{
		return update(timestamp_ns, relation, confidence);
	}

	// Advance dropout/hold state when no packet arrived.  A poll does not
	// change the packet timestamp watermark.
	fbt_supervisor_result tick(int64_t timestamp_ns);
	fbt_supervisor_result poll(int64_t timestamp_ns)
	{
		return tick(timestamp_ns);
	}

	void reset();
	void reset_tracking()
	{
		reset();
	}

	const fbt_supervisor_config & configuration() const
	{
		return config_;
	}
	const fbt_supervisor_metrics & metrics() const
	{
		return metrics_;
	}
	fbt_supervisor_state current_state() const
	{
		return state_;
	}
	bool has_valid_pose() const
	{
		return have_valid_pose_;
	}

private:
	struct validation
	{
		bool valid = false;
		fbt_supervisor_reason reason = fbt_supervisor_reason::none;
	};

	validation validate(const xrt_space_relation & relation, float confidence) const;
	fbt_supervisor_result invalid_sample(int64_t timestamp_ns, fbt_supervisor_reason input_reason, float confidence);
	fbt_supervisor_result valid_sample(int64_t timestamp_ns, const xrt_space_relation & relation, float confidence);
	fbt_supervisor_result timeout(int64_t timestamp_ns);
	fbt_supervisor_result make_result(fbt_supervisor_reason reason, bool input_valid, bool accepted, float confidence) const;
	void record_reason(fbt_supervisor_reason reason);
	void clear_lifecycle();
	void normalize_relation(xrt_space_relation & relation) const;
	void smooth_into(const xrt_space_relation & relation, float confidence, int64_t timestamp_ns);
	float adaptive_alpha(const xrt_space_relation & relation, float confidence, int64_t timestamp_ns) const;

	fbt_supervisor_config config_;
	fbt_supervisor_state state_ = fbt_supervisor_state::warming_up;
	fbt_supervisor_metrics metrics_{};

	bool have_timestamp_ = false;
	int64_t last_timestamp_ns_ = 0;
	// Last timestamp used to evaluate lifecycle timeouts.  This is separate
	// from last_timestamp_ns_: tick() must not move the packet reorder fence.
	int64_t evaluation_timestamp_ns_ = 0;
	bool have_valid_pose_ = false;
	int64_t last_valid_timestamp_ns_ = 0;
	int64_t dropout_started_ns_ = 0;
	bool dropout_active_ = false;
	bool hold_expired_ = false;
	uint32_t recovery_valid_samples_ = 0;

	xrt_space_relation filtered_ = XRT_SPACE_RELATION_ZERO;
	xrt_space_relation recovery_filtered_ = XRT_SPACE_RELATION_ZERO;
	bool have_recovery_pose_ = false;
	int64_t recovery_timestamp_ns_ = 0;
	float filtered_confidence_ = 0.0f;
	int64_t filtered_timestamp_ns_ = 0;
};

// Names used by body-tracking adapters in downstream code.  Do not alias the
// class to `body_tracking_supervisor`: that name is the wire-level mode enum.
using fbt_joint_supervisor = fbt_supervisor;
using body_tracking_supervisor_config = fbt_supervisor_config;
using body_tracking_supervisor_result = fbt_supervisor_result;

} // namespace wivrn
