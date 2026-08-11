/*
 * WiVRn VR streaming
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "application.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
	auto root = std::filesystem::temp_directory_path() / "wivrn-application-test";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "data/applications");
	std::filesystem::create_directories(root / "system/applications");

	{
		std::ofstream desktop(root / "system/applications/wayvr.desktop");
		desktop << "[Desktop Entry]\n"
		           "Type=Application\n"
		           "Name=WayVR\n"
		           "Exec=wayvr\n"
		           "Categories=Utility;X-WiVRn-VR;\n"
		           "X-XR-Plugin=true\n"
		           "X-XR-Plugin-Exec=wayvr --openxr\n";
	}

	setenv("HOME", root.c_str(), true);
	setenv("XDG_DATA_HOME", (root / "data").c_str(), true);
	setenv("XDG_DATA_DIRS", (root / "system").c_str(), true);

	auto apps = wivrn::list_applications();
	auto wayvr = apps.find("wayvr");
	if (wayvr == apps.end())
	{
		std::cerr << "WayVR desktop entry was not detected as a VR application\n";
		return EXIT_FAILURE;
	}
	if (wayvr->second.name.at("") != "WayVR")
	{
		std::cerr << "WayVR desktop entry has the wrong name\n";
		return EXIT_FAILURE;
	}
	if (wayvr->second.exec != "wayvr --openxr")
	{
		std::cerr << "Expected X-XR-Plugin-Exec, got: " << wayvr->second.exec << '\n';
		return EXIT_FAILURE;
	}

	std::filesystem::remove_all(root);
	return EXIT_SUCCESS;
}
