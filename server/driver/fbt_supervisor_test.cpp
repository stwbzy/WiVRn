/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fbt_supervisor.h"

#include <cmath>
#include <stdexcept>

namespace
{
using wivrn::fbt_supervisor;
using wivrn::fbt_supervisor_reason;
using wivrn::fbt_supervisor_state;

void check(bool condition, const char * message)
{
	if (!condition)
		throw std::runtime_error(message);
}

xrt_space_relation pose(float x = 0.0f)
{
	return {
	        .relation_flags = static_cast<xrt_space_relation_flags>(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                                  XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                                  XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                                  XRT_SPACE_RELATION_POSITION_TRACKED_BIT),
	        .pose = {
	                .orientation = {0.0f, 0.0f, 0.0f, 1.0f},
	                .position = {x, 0.0f, 0.0f},
	        },
	        .linear_velocity = {0.0f, 0.0f, 0.0f},
	        .angular_velocity = {0.0f, 0.0f, 0.0f},
	};
}

xrt_space_relation relation_with_linear_spike()
{
	auto result = pose();
	result.relation_flags = static_cast<xrt_space_relation_flags>(static_cast<uint32_t>(result.relation_flags) |
	                                                               XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
	result.linear_velocity = {9.0f, 0.0f, 0.0f};
	return result;
}

xrt_space_relation relation_with_angular_spike()
{
	auto result = pose();
	result.relation_flags = static_cast<xrt_space_relation_flags>(static_cast<uint32_t>(result.relation_flags) |
	                                                               XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	result.angular_velocity = {0.0f, 0.0f, 21.0f};
	return result;
}
} // namespace

int main()
{
	fbt_supervisor supervisor;

	// Confidence and the initial relation are checked before entering tracking.
	auto result = supervisor.update(0, pose(), 0.49f);
	check(result.state == fbt_supervisor_state::warming_up, "low confidence must warm up");
	check(result.reason == fbt_supervisor_reason::low_confidence, "low confidence reason missing");
	result = supervisor.update(1'000'000, pose(), 1.0f);
	check(result.state == fbt_supervisor_state::tracking && result.output_valid, "first good pose must track");

	// A velocity spike is rejected and briefly held rather than published.
	result = supervisor.update(10'000'000, relation_with_linear_spike(), 1.0f);
	check(!result.input_valid && result.reason == fbt_supervisor_reason::excessive_linear_velocity, "linear spike accepted");
	check(result.state == fbt_supervisor_state::holding && result.held && result.output_valid, "spike did not enter hold");
	result = supervisor.update(15'000'000, relation_with_angular_spike(), 1.0f);
	check(result.reason == fbt_supervisor_reason::excessive_angular_velocity && result.output_valid,
	      "angular spike was not rejected");

	// Providers such as META omit velocity fields. A positional teleport must
	// still be rejected using the delta between samples.
	result = supervisor.update(16'000'000, pose(5.0f), 1.0f);
	check(result.reason == fbt_supervisor_reason::excessive_linear_velocity && result.output_valid,
	      "implicit linear spike was not rejected");

	// NaN data is rejected without poisoning the cached relation.
	auto nan_pose = pose(1.0f);
	nan_pose.pose.position.x = std::nanf("");
	result = supervisor.update(20'000'000, nan_pose, 1.0f);
	check(result.reason == fbt_supervisor_reason::non_finite && result.output_valid, "NaN was not rejected safely");
	check(std::isfinite(result.relation.pose.position.x), "held relation became non-finite");

	check(supervisor.tick(100'000'000).output_valid, "hold ended too early");
	result = supervisor.tick(120'000'000);
	check(result.state == fbt_supervisor_state::holding && !result.output_valid &&
	              result.reason == fbt_supervisor_reason::hold_expired,
	      "hold expiry was not reported");
	result = supervisor.tick(510'000'000);
	check(result.state == fbt_supervisor_state::lost && !result.output_valid &&
	              result.reason == fbt_supervisor_reason::hard_loss,
	      "hard loss was not reported");

	// Recovery is gated by three consecutive valid samples.
	result = supervisor.update(520'000'000, pose(2.0f), 1.0f);
	check(result.state == fbt_supervisor_state::recovering && !result.output_valid &&
	              result.reason == fbt_supervisor_reason::recovery_pending,
	      "recovery started too early");
	result = supervisor.update(530'000'000, pose(2.1f), 1.0f);
	check(result.state == fbt_supervisor_state::recovering && !result.output_valid, "recovery was not gated");
	result = supervisor.update(540'000'000, pose(2.2f), 1.0f);
	check(result.state == fbt_supervisor_state::tracking && result.output_valid &&
	              result.reason == fbt_supervisor_reason::recovered,
	      "recovery did not complete");

	// Equal and older packet timestamps cannot alter state or pose.
	const auto before_reorder = result.relation.pose.position.x;
	result = supervisor.update(530'000'000, pose(99.0f), 1.0f);
	check(result.reordered && result.reason == fbt_supervisor_reason::reordered_timestamp,
	      "reordered packet was not ignored");
	check(std::abs(result.relation.pose.position.x - before_reorder) < 0.0001f, "reordered packet changed pose");

	// Reset clears lifecycle and cached output; it also leaves an auditable reset metric.
	supervisor.reset();
	check(supervisor.current_state() == fbt_supervisor_state::warming_up && !supervisor.has_valid_pose(),
	      "reset retained tracking state");
	check(supervisor.metrics().resets == 1 && supervisor.metrics().accepted_samples == 0, "reset metrics incorrect");

	// META supplies locations without velocities. The supervisor derives a
	// bounded linear velocity for optional short-horizon prediction.
	fbt_supervisor derived_velocity;
	derived_velocity.update(0, pose(0.0f), 1.0f);
	result = derived_velocity.update(100'000'000, pose(0.1f), 1.0f);
	check((result.relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0,
	      "derived linear velocity flag missing");
	check(std::abs(result.relation.linear_velocity.x - 1.0f) < 0.001f,
	      "derived linear velocity is incorrect");
}
