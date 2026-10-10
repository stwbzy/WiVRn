/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "body_motion_classifier.h"

#include <cassert>
#include <cmath>

namespace
{
using namespace wivrn;

constexpr auto flags = static_cast<xrt_space_relation_flags>(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
                                                             XRT_SPACE_RELATION_POSITION_VALID_BIT |
                                                             XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
                                                             XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

body_motion_sample sample(int64_t timestamp, float head_x, float hand_x = 0.0f, xrt_quat orientation = {0, 0, 0, 1})
{
	body_motion_sample result{};
	result.timestamp_ns = timestamp;
	result.head = {.relation = {.relation_flags = flags, .pose = {.orientation = orientation, .position = {head_x, 1.6f, 0.0f}}},
	               .valid = true};
	result.left_hand = {.relation = {.relation_flags = flags, .pose = {.orientation = {0, 0, 0, 1}, .position = {hand_x, 1.2f, 0.2f}}},
	                    .valid = true};
	result.right_hand = result.left_hand;
	for (size_t i = 0; i < body_pose_role_count; ++i)
	{
		result.relations[i] = {.relation_flags = flags,
		                       .pose = {.orientation = {0, 0, 0, 1}, .position = {head_x, 1.0f, 0.0f}}};
		result.relation_valid[i] = true;
	}
	return result;
}
} // namespace

int main()
{
	body_motion_classifier classifier;
	assert(classifier.update(sample(0, 0.0f)) == body_motion_state::unknown);
	assert(classifier.update(sample(100'000'000, 0.01f)) == body_motion_state::chill);

	classifier.reset();
	assert(classifier.update(sample(0, 0.0f)) == body_motion_state::unknown);
	assert(classifier.update(sample(100'000'000, 0.20f)) == body_motion_state::sprint);

	// A single quiet frame cannot switch an active sprint profile. The new
	// candidate must remain stable for the classifier dwell time.
	assert(classifier.update(sample(200'000'000, 0.21f)) == body_motion_state::sprint);
	assert(classifier.update(sample(500'000'000, 0.22f)) == body_motion_state::sprint);
	assert(classifier.update(sample(650'000'000, 0.23f)) == body_motion_state::chill);

	classifier.reset();
	const float half_turn = std::sin(1.2f / 2.0f);
	const float cosine = std::cos(1.2f / 2.0f);
	assert(classifier.update(sample(0, 0.0f, 0.0f, {0, 0, 0, 1})) == body_motion_state::unknown);
	assert(classifier.update(sample(100'000'000, 0.01f, 0.05f, {0, half_turn, 0, cosine})) == body_motion_state::flow);

	// A long gap (for example after a reconnect) must not be classified as a
	// burst of movement.
	classifier.reset();
	assert(classifier.update(sample(0, 0.0f)) == body_motion_state::unknown);
	assert(classifier.update(sample(1'000'000'000, 1.0f)) == body_motion_state::unknown);
}
