#include "render_tuning.h"

#include <algorithm>
#include <cmath>

namespace wivrn::launcher
{
std::uint32_t ScaledExtent(std::uint32_t recommended, std::uint32_t maximum, double scale)
{
	if (!recommended || !maximum || !std::isfinite(scale) || scale < .75 || scale > 1.75)
		return 0;
	const double pixels = std::ceil(recommended * scale / 4.0) * 4.0;
	return static_cast<std::uint32_t>(std::min(pixels, static_cast<double>(maximum)));
}
} // namespace wivrn::launcher
