#pragma once

#include <cstdint>
#include <string_view>

namespace wivrn::launcher
{

struct RenderTuning
{
	double scale;
	int msaa;
	int texture_budget_mb;
	bool low_lod;
	bool legacy;
	bool valid = true;
};

inline RenderTuning Tuning(std::string_view profile)
{
	if (profile == "balanced")
		return {1.25, 2, 512, false, false};
	if (profile == "quality")
		return {1.5, 4, 1024, false, false};
	if (profile == "performance")
		return {1.0, 1, 512, true, false};
	if (profile == "legacy")
		return {1.0, 1, 128, true, true};
	return {1.0, 1, 128, true, true, false};
}

std::uint32_t ScaledExtent(std::uint32_t recommended, std::uint32_t maximum, double scale);
} // namespace wivrn::launcher
