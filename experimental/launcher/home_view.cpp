#include "home_view.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace wivrn::launcher
{
namespace
{
std::atomic<bool> home_active{false};
std::atomic<std::uint64_t> home_generation{0};

using namespace mocktail::vr::pose;

bool ValidHead(const mocktail::vr::ScriptedPoseSample & sample)
{
	Quat normalized;
	return sample.valid && std::isfinite(sample.position[0]) &&
	       std::isfinite(sample.position[1]) && std::isfinite(sample.position[2]) &&
	       QuatNormalize({sample.orientation[0], sample.orientation[1], sample.orientation[2], sample.orientation[3]}, &normalized);
}

void Transform(const Pose & inverse, float * position, float * orientation)
{
	const Pose result = PoseMultiply(
	        inverse, {{position[0], position[1], position[2]}, {orientation[0], orientation[1], orientation[2], orientation[3]}});
	position[0] = result.position.x;
	position[1] = result.position.y;
	position[2] = result.position.z;
	orientation[0] = result.orientation.x;
	orientation[1] = result.orientation.y;
	orientation[2] = result.orientation.z;
	orientation[3] = result.orientation.w;
}
} // namespace

void SetHomeActive(bool active)
{
	const char * vr = std::getenv("MOCKTAIL_VR_ENABLED");
	active = active && vr && std::strcmp(vr, "1") == 0;
	if (home_active.exchange(active, std::memory_order_acq_rel) != active)
	{
		home_generation.fetch_add(1, std::memory_order_acq_rel);
		std::fprintf(stderr, "  [vr-home] phase=%s recenter=%s\n", active ? "home" : "experience", active ? "enabled" : "blocked");
	}
}

bool HomeActive()
{
	return home_active.load(std::memory_order_acquire);
}
std::uint64_t HomeGeneration()
{
	return home_generation.load(std::memory_order_acquire);
}

mocktail::vr::ScriptedPoseSample HomeView::Update(
        const mocktail::vr::ScriptedPoseSample & raw,
        bool home,
        std::uint64_t generation,
        std::uint64_t now_ns)
{
	if (!home || generation != generation_)
	{
		generation_ = generation;
		recentered_ = false;
		latched_ = false;
		held_since_ = 0;
	}
	if (!home)
		return raw;
	const bool held = ValidHead(raw) && raw.controllers_connected &&
	                  raw.hands[0].grip_valid && raw.controller_channels[27] > .5f;
	if (!held)
	{
		held_since_ = 0;
		latched_ = false;
	}
	else if (!latched_)
	{
		if (held_since_ == 0 || now_ns < held_since_)
			held_since_ = now_ns;
		if (now_ns - held_since_ >= kHoldNs)
		{
			Quat orientation;
			(void)QuatNormalize({raw.orientation[0], raw.orientation[1], raw.orientation[2], raw.orientation[3]}, &orientation);
			const auto forward = QuatRotate(orientation, {0, 0, -1});
			// Keep gravity upright: only recenter yaw, never bake head tilt into the world.
			const float yaw = std::atan2(-forward.x, -forward.z);
			origin_ = {{raw.position[0], raw.position[1], raw.position[2]},
			           {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}};
			latched_ = true;
			recentered_ = true;
			++recenter_count_;
		}
	}
	if (!recentered_ || !raw.valid)
		return raw;
	auto result = raw;
	const auto inverse = PoseInverse(origin_);
	Transform(inverse, result.position, result.orientation);
	if (result.floor_valid)
		Transform(inverse, result.floor_position, result.floor_orientation);
	for (auto & hand: result.hands)
	{
		if (hand.grip_valid)
			Transform(inverse, hand.grip_position, hand.grip_orientation);
		if (hand.aim_valid)
			Transform(inverse, hand.aim_position, hand.aim_orientation);
	}
	return result;
}
} // namespace wivrn::launcher
