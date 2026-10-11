/*
 * WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime_manifest.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;

struct temporary_directory
{
	fs::path path;

	temporary_directory()
	{
		auto pattern = (fs::temp_directory_path() / "wivrn-runtime-manifest-XXXXXX").string();
		auto created = mkdtemp(pattern.data());
		if (!created)
			throw std::runtime_error("Cannot create runtime manifest test directory");
		path = created;
	}

	~temporary_directory()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

int main()
{
	temporary_directory fixture;
	const auto directory = fixture.path / "openxr";
	std::ostringstream diagnostics;

	// Missing installation directory: no exception, actionable diagnostic.
	assert(wivrn::find_runtime_manifests(directory, diagnostics).empty());
	assert(diagnostics.str().find(directory.string()) != std::string::npos);
	assert(diagnostics.str().find("CMAKE_INSTALL_PREFIX") != std::string::npos);

	// An ordinary file where a directory was expected must also be harmless.
	std::ofstream(directory).put('\n');
	diagnostics.str("");
	assert(wivrn::find_runtime_manifests(directory, diagnostics).empty());
	assert(!diagnostics.str().empty());
	fs::remove(directory);
	fs::create_directory(directory);

	// Empty installations must not silently claim successful activation.
	diagnostics.str("");
	assert(wivrn::find_runtime_manifests(directory, diagnostics).empty());
	assert(diagnostics.str().find("No WiVRn OpenXR manifest") != std::string::npos);

	// Keep the generic and ABI manifests; reject backups, unrelated files,
	// directories named like manifests, and broken symlinks.
	std::ofstream(directory / "openxr_wivrn.json").put('\n');
	std::ofstream(directory / "openxr_wivrn.x86_64.json").put('\n');
	std::ofstream(directory / "openxr_wivrn.json.wivrn-backup").put('\n');
	std::ofstream(directory / "another_runtime.json").put('\n');
	fs::create_directory(directory / "openxr_wivrn.directory.json");
	diagnostics.str("");
	const std::vector<fs::path> expected{
	        directory / "openxr_wivrn.json",
	        directory / "openxr_wivrn.x86_64.json"};
	assert(wivrn::find_runtime_manifests(directory, diagnostics) == expected);
	assert(diagnostics.str().empty());

	fs::create_symlink("missing.json", directory / "openxr_wivrn.broken.json");
	diagnostics.str("");
	assert(wivrn::find_runtime_manifests(directory, diagnostics) == expected);
	assert(diagnostics.str().find("openxr_wivrn.broken.json") != std::string::npos);
	fs::remove(directory / "openxr_wivrn.broken.json");

	// Valid symlinked manifests (e.g. distribution packages) remain supported.
	fs::create_symlink("openxr_wivrn.json", directory / "openxr_wivrn.aarch64.json");
	const std::vector<fs::path> expected_with_symlink{
	        directory / "openxr_wivrn.aarch64.json",
	        directory / "openxr_wivrn.json",
	        directory / "openxr_wivrn.x86_64.json"};
	diagnostics.str("");
	assert(wivrn::find_runtime_manifests(directory, diagnostics) == expected_with_symlink);
	assert(diagnostics.str().empty());

	// Root bypasses permissions; exercise this check when running unprivileged.
	if (geteuid() != 0)
	{
		fs::permissions(directory, fs::perms::none);
		diagnostics.str("");
		auto manifests = wivrn::find_runtime_manifests(directory, diagnostics);
		fs::permissions(directory, fs::perms::owner_all);
		assert(manifests.empty());
		assert(!diagnostics.str().empty());
	}
}
