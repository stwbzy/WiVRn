/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "history.h"

#include <cmath>
#include <stdexcept>

namespace
{
struct scalar_sample
{
	float value = 0;
};

class scalar_history : public wivrn::history<scalar_history, scalar_sample>
{
public:
	scalar_sample interpolate(const scalar_sample & a, const scalar_sample & b, float t)
	{
		return {.value = std::lerp(a.value, b.value, t)};
	}

	void add(XrTime produced, XrTime timestamp, float value)
	{
		add_sample_server_time(produced, timestamp, {.value = value}, true);
	}
};

void check(bool condition)
{
	if (!condition)
		throw std::runtime_error("tracking history test check failed");
}

void check_near(float actual, float expected)
{
	check(std::abs(actual - expected) < 0.0001f);
}
} // namespace

int main()
{
	scalar_history history;
	history.add(10, 100'000'000, 0);
	history.add(20, 200'000'000, 100);

	check_near(history.get_at(125'000'000).second.value, 25);
	check_near(history.get_at(150'000'000).second.value, 50);
	check_near(history.get_at(175'000'000).second.value, 75);

	// A newer sample at effectively the same timestamp replaces the old one.
	history.add(30, 200'500'000, 120);
	check_near(history.get_at(200'500'000).second.value, 120);

	// A packet produced before an accepted packet cannot alter the history.
	history.add(25, 300'000'000, 300);
	check_near(history.get_at(300'000'000).second.value, 120);

	history.reset();
	check(history.get_at(150'000'000).first == 0);
}
