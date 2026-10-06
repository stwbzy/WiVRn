#include <filesystem>
#include <iostream>
#include <vector>

#include "update/android_manifest.h"
#include "update/apk_bundle.h"
#include "update/payload_store.h"
#include "update/zip_archive.h"

int main(int argc, char ** argv)
{
	if (argc != 7)
	{
		std::cerr << "Usage: wivrn-launcher-import STORE WORK TRUST COMPAT BASE SPLIT\n";
		return 2;
	}
	const auto encoded = mocktail::update::ReadZipEntry(argv[5], "AndroidManifest.xml", 16U * 1024U * 1024U);
	if (!encoded)
	{
		std::cerr << encoded.error << '\n';
		return 1;
	}
	const auto manifest = mocktail::update::ParseAndroidManifest(encoded.bytes);
	if (!manifest || manifest.package_name != "com.roblox.client")
	{
		std::cerr << "Invalid Roblox package: " << manifest.error << '\n';
		return 1;
	}
	mocktail::update::ExpectedPayloadIdentity identity;
	identity.version_name = manifest.version_name;
	identity.version_code = manifest.version_code;
	const auto prepared = mocktail::update::PreparePayloadFromArchives(
	        {argv[5], argv[6]}, identity, argv[3], argv[2], "isolated-local-apk-import");
	if (!prepared)
	{
		std::cerr << prepared.error << '\n';
		return 1;
	}
	mocktail::update::PayloadStore store(argv[1], argv[4]);
	const auto staged = store.Stage(prepared.directory);
	if (!staged)
	{
		std::cerr << staged.error << '\n';
		return 1;
	}
	// Staging never activates a build, even when its APK signature is trusted.
	std::cout << staged.payload_id << '\n';
}
