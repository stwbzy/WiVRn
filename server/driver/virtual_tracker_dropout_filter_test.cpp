/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "virtual_tracker_dropout_filter.h"

#include <stdexcept>

namespace
{
using filter = wivrn::virtual_tracker_dropout_filter;
using action = filter::action;

void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("virtual tracker dropout filter test check failed");
}

filter::result update(filter & value, int64_t milliseconds, bool valid)
{
	return value.update(milliseconds * 1'000'000, valid);
}
} // namespace

int main()
{
	filter value;

	// A reset or newly enabled tracker stays untracked until its first valid pose.
	check(update(value, 1, false).next_action == action::wait_for_valid);
	check(update(value, 2, true).next_action == action::accept_valid);

	// Invalid samples below 100 ms are suppressed.
	for (int64_t timestamp = 13; timestamp < 113; timestamp += 11)
		check(update(value, timestamp, false).next_action == action::suppress_invalid);
	auto transient_recovery = update(value, 112, true);
	check(transient_recovery.next_action == action::ignore_reordered);
	transient_recovery = update(value, 113, true);
	check(transient_recovery.next_action == action::accept_valid);
	check(transient_recovery.invalid_duration_ns == 100'000'000);

	// The first invalid sample at or beyond the grace period publishes loss once.
	check(update(value, 124, false).next_action == action::suppress_invalid);
	check(update(value, 223, false).next_action == action::suppress_invalid);
	auto lost = update(value, 224, false);
	check(lost.next_action == action::publish_invalid);
	check(lost.invalid_duration_ns == filter::grace_period_ns);
	check(update(value, 235, false).next_action == action::maintain_invalid);

	// Recovery is immediate, and duplicates/reordered packets cannot disturb it.
	auto recovered = update(value, 246, true);
	check(recovered.next_action == action::recover);
	check(recovered.invalid_duration_ns == 122'000'000);
	check(update(value, 246, false).next_action == action::ignore_reordered);
	check(update(value, 245, false).next_action == action::ignore_reordered);

	value.reset();
	check(update(value, 300, false).next_action == action::wait_for_valid);
	check(update(value, 301, true).next_action == action::accept_valid);
}
