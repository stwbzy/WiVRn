#include "payload_gate.h"

#include "compat/elf_build_id.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"
#include "installed_contract.h"

#include <tuple>

namespace wivrn::roblox {
namespace {
auto Fields(const mocktail::compat::VrDebugDeviceBridgeProfile& p) {
    return std::make_tuple(p.vtable_rva, p.constructor_rva, p.state_getter_rva,
                          p.eye_getter_rva, p.eye_initializer_rva,
                          p.emulator_flag_storage_rva,
                          p.device_create_framebuffer_vtable_offset);
}
}

PayloadCheck CheckBuild(const std::string& build_id, const std::string& manifest, bool allow_experimental) {
    PayloadCheck check;
    check.build_id = build_id;
    auto lookup = mocktail::compat::FindBuildProfile(manifest, build_id);
    if (!lookup) {
        check.error = lookup.error;
        return check;
    }
    if (!lookup.profile) {
        check.error = "Unknown Roblox Build ID; no hooks will be installed";
        return check;
    }
    const auto& profile = *lookup.profile;
    if (profile.elf_build_id == installed_contract.build_id && !allow_experimental) {
        check.error = "Local static candidate requires explicit experimental opt-in";
        return check;
    }
    if (profile.status != mocktail::compat::BuildStatus::kSupported &&
        !(allow_experimental && profile.status == mocktail::compat::BuildStatus::kExperimental)) {
        check.error = "Experimental build requires explicit opt-in; unverified/legacy builds refused";
        return check;
    }
    const auto* verified = mocktail::vr::FindRobloxVrDeviceProfile(profile.elf_build_id);
    if (!profile.allow_host_abi_bridges || !profile.vr_debug_device_bridge || !verified) {
        check.error = "This Build ID has no verified native VR bridge contract";
        return check;
    }
    if (Fields(*profile.vr_debug_device_bridge) != Fields(*verified)) {
        check.error = "VR offsets do not match the compiled exact-build contract";
        return check;
    }
    check.profile = profile;
    return check;
}

PayloadCheck CheckPayload(const std::string& elf_path, const std::string& manifest, bool allow_experimental) {
    auto id = mocktail::compat::ReadElfBuildId(elf_path);
    if (!id) return {{}, id.error, std::nullopt};
    auto check = CheckBuild(id.build_id, manifest, allow_experimental);
    if (check && id.build_id == installed_contract.build_id) {
        check.error = VerifyInstalledStaticPayload(elf_path);
        if (!check.error.empty()) check.profile.reset();
    }
    return check;
}
} // namespace wivrn::roblox
