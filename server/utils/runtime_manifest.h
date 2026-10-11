/*
 * WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace wivrn
{

// Discovery failures must not prevent the server from starting or stopping.
std::vector<std::filesystem::path> find_runtime_manifests(
        const std::filesystem::path & directory,
        std::ostream & diagnostics);

} // namespace wivrn
