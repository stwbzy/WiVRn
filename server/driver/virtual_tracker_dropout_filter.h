/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

namespace wivrn
{
class virtual_tracker_dropout_filter
{
public:
	static constexpr int64_t grace_period_ns = 100'000'000;

	enum class action
	{
		accept_valid,
		suppress_invalid,
		publish_invalid,
		maintain_invalid,
		recover,
		wait_for_valid,
		ignore_reordered,
	};

	struct result
	{
		action next_action;
		int64_t invalid_duration_ns = 0;
	};

	result update(int64_t produced_timestamp, bool valid)
	{
		if (has_timestamp && produced_timestamp <= last_produced_timestamp)
			return {.next_action = action::ignore_reordered};

		has_timestamp = true;
		last_produced_timestamp = produced_timestamp;

		if (valid)
		{
			have_valid_pose = true;
			if (invalid_since)
			{
				const int64_t duration = produced_timestamp - invalid_since;
				const bool was_published = invalid_published;
				invalid_since = 0;
				invalid_published = false;
				return {
				        .next_action = was_published ? action::recover : action::accept_valid,
				        .invalid_duration_ns = duration,
				};
			}
			return {.next_action = action::accept_valid};
		}

		if (!have_valid_pose)
			return {.next_action = action::wait_for_valid};

		if (!invalid_since)
			invalid_since = produced_timestamp;

		const int64_t duration = produced_timestamp - invalid_since;
		if (invalid_published)
		{
			return {
			        .next_action = action::maintain_invalid,
			        .invalid_duration_ns = duration,
			};
		}

		if (duration >= grace_period_ns)
		{
			invalid_published = true;
			return {
			        .next_action = action::publish_invalid,
			        .invalid_duration_ns = duration,
			};
		}

		return {
		        .next_action = action::suppress_invalid,
		        .invalid_duration_ns = duration,
		};
	}

	void reset()
	{
		has_timestamp = false;
		have_valid_pose = false;
		invalid_published = false;
		last_produced_timestamp = 0;
		invalid_since = 0;
	}

private:
	bool has_timestamp = false;
	bool have_valid_pose = false;
	bool invalid_published = false;
	int64_t last_produced_timestamp = 0;
	int64_t invalid_since = 0;
};
} // namespace wivrn
