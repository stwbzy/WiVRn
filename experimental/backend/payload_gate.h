#pragma once

#include "compat/build_profile.h"

#include <optional>
#include <string>

namespace wivrn::roblox {

struct PayloadCheck {
    std::string build_id;
    std::string error;
    std::optional<mocktail::compat::BuildProfile> profile;
    explicit operator bool() const { return error.empty() && profile.has_value(); }
};

// Read-only preflight. Never loads guest code, arms XR or changes memory.
PayloadCheck CheckPayload(const std::string& elf_path, const std::string& manifest,
                          bool allow_experimental = false);
PayloadCheck CheckBuild(const std::string& build_id, const std::string& manifest,
                        bool allow_experimental = false);

} // namespace wivrn::roblox
