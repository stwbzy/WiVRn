#pragma once

#include <cstdint>

#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/vr_pose_math.h"

namespace wivrn::launcher
{

// Only the authoritative host lifecycle can grant access to home controls.
void SetHomeActive(bool active);
bool HomeActive();
std::uint64_t HomeGeneration();

class HomeView
{
public:
	static constexpr std::uint64_t kHoldNs = 1500000000ULL;
	mocktail::vr::ScriptedPoseSample Update(
	        const mocktail::vr::ScriptedPoseSample & raw,
	        bool home,
	        std::uint64_t generation,
	        std::uint64_t now_ns);
	bool recentered() const
	{
		return recentered_;
	}
	std::uint64_t recenter_count() const
	{
		return recenter_count_;
	}

private:
	mocktail::vr::pose::Pose origin_{};
	std::uint64_t generation_ = 0;
	std::uint64_t held_since_ = 0;
	std::uint64_t recenter_count_ = 0;
	bool latched_ = false;
	bool recentered_ = false;
};

} // namespace wivrn::launcher
