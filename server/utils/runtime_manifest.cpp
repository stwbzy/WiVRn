/*
 * WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime_manifest.h"
#include <algorithm>
#include <ostream>

namespace wivrn
{

std::vector<std::filesystem::path> find_runtime_manifests(
        const std::filesystem::path & directory,
        std::ostream & diagnostics)
{
	std::vector<std::filesystem::path> result;
	std::error_code ec;
	std::filesystem::directory_iterator it(directory, ec);
	if (ec)
	{
		diagnostics << "Cannot search for WiVRn OpenXR manifests in " << directory
		            << ": " << ec.message()
		            << ". Automatic OpenXR runtime activation is unavailable; "
		               "check CMAKE_INSTALL_PREFIX and reinstall the server.\n";
		return result;
	}

	while (it != std::filesystem::directory_iterator{})
	{
		const auto & file = *it;
		const auto filename = file.path().filename();
		if (filename.native().starts_with("openxr_wivrn") && filename.extension() == ".json")
		{
			if (file.is_regular_file(ec))
				result.push_back(file.path());
			else if (ec)
				diagnostics << "Cannot read OpenXR manifest " << file.path() << ": " << ec.message() << '\n';
		}

		it.increment(ec);
		if (ec)
		{
			diagnostics << "Cannot finish searching for OpenXR manifests in " << directory
			            << ": " << ec.message() << '\n';
			break;
		}
	}

	if (result.empty())
		diagnostics << "No WiVRn OpenXR manifest found in " << directory
		            << "; check CMAKE_INSTALL_PREFIX and reinstall the server.\n";
	std::ranges::sort(result);
	return result;
}

} // namespace wivrn
