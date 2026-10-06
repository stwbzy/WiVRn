#include <array>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>

#include "update/compatibility_catalog.h"
#include "update/payload_integrity.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"

int main(int argc, char ** argv)
{
	if (argc != 6)
	{
		std::cerr << "Usage: wivrn-launcher-canary STORE ID PROFILE_DIR RUNTIME STATE\n";
		return 2;
	}
	const std::filesystem::path store_root = argv[1];
	const std::string id = argv[2];
	const std::filesystem::path profiles = argv[3];
	const std::filesystem::path runtime = argv[4];
	const std::filesystem::path state = argv[5];
	if (!std::regex_match(id, std::regex("[1-9][0-9]*-[0-9a-f]{40}")) ||
	    !store_root.is_absolute() || !profiles.is_absolute() ||
	    !runtime.is_absolute() || !state.is_absolute())
	{
		std::cerr << "Invalid absolute paths or payload ID\n";
		return 2;
	}
	const auto payload = store_root / "payloads" / id;
	const auto verified = mocktail::update::VerifyPreparedPayload(payload);
	if (!verified || verified.payload_id != id)
	{
		std::cerr << "Payload verification failed: " << verified.error << '\n';
		return 2;
	}
	mocktail::update::CanaryOptions options;
	options.runtime_binary = runtime;
	options.payload_directory = payload;
	if (std::filesystem::is_regular_file(profiles))
	{
		const auto catalog = mocktail::update::LoadCompatibilityCatalog(profiles);
		if (!catalog)
		{
			std::cerr << catalog.error << '\n';
			return 2;
		}
		const auto exact = mocktail::update::FindSupportedProfile(
		        catalog.profiles, verified.metadata.version_name, verified.metadata.version_code);
		if (!exact || exact->elf_build_id != verified.metadata.build_id)
		{
			std::cerr << "Payload is not an exact supported build; HostAbi derivation required\n";
			return 2;
		}
		options.compatibility_manifest = profiles;
	}
	else
	{
		options.compatibility_manifest = profiles / "compatibility.json";
		options.host_abi_profile = profiles / "host_abi_profile.json";
	}
	options.cache_root = state / "cache";
	options.state_root = state / "state";
	std::array<std::filesystem::path, 2> logs;
	for (std::size_t index = 0; index < logs.size(); ++index)
	{
		std::cout << "[isolated-launcher] Desktop canary " << index + 1 << "/2; VR disabled\n"
		          << std::flush;
		const auto result = mocktail::update::RunReadinessCanary(options);
		std::cout << "[isolated-launcher] log=" << result.log_path << '\n';
		if (!result)
		{
			std::cerr << "[isolated-launcher] " << result.error
			          << "; active payload unchanged\n";
			return 1;
		}
		logs[index] = result.log_path;
	}
	mocktail::update::PayloadStore store(store_root, options.compatibility_manifest, runtime);
	const auto activated = options.probation_candidate()
	                               ? store.PromoteProbation(id, options.host_abi_profile, options.compatibility_manifest, logs)
	                               : store.Promote(id);
	if (!activated)
	{
		std::cerr << "[isolated-launcher] Approval failed: " << activated.error << '\n';
		return 1;
	}
	std::cout << "[isolated-launcher] Desktop ABI approved: " << activated.payload_id
	          << "; headset stereo acceptance still required\n";
}
