#pragma once

#include "vr/roblox_vr_device_bridge_internal.h"
#include <iosfwd>
#include <string>

namespace wivrn::roblox {
// Derived from the installed ELF, not transplanted from another build.
// Static candidate only; registration is confined to the optional build overlay.
inline constexpr const char* installed_sha256 =
    "a0464843bc408a37d05cd8a6cb20090f295b7299e73dcbe4a06d23d46ad7314e";
inline constexpr mocktail::vr::internal::VrBuildContract installed_contract = {
    "3b211f25d8658e88e28c86a4ff1e02ff96092741",
    {0x6c7c138, 0x37cb87e, 0x37cbc60, 0x37cbc50, 0x37cbc82,
     0x7361fb0, 0x188},
    0x6b6fb20, 0x4d34510, 0x263af92, 0x3a0, 0x735fde8, 0x1d65140,
};
std::string VerifyInstalledStaticPayload(const std::string& path, std::ostream* output = nullptr);
}
