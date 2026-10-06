#include "payload_gate.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace {
constexpr const char* verified_id = "ade08266c67aee88ec9c1d00902150e1684dad3a";
class GateFixture : public testing::Test {
protected:
    std::string path = std::string(MOCKTAIL_TEST_BINARY_DIRECTORY) + "/gate-fixture.json";
    nlohmann::json manifest;
    void SetUp() override {
        std::ifstream input(BACKEND_PROFILE_MANIFEST);
        ASSERT_TRUE(input.good());
        input >> manifest;
    }
    void TearDown() override { std::filesystem::remove(path); }
    nlohmann::json& Profile() {
        for (auto& profile : manifest["profiles"])
            if (profile["elf_build_id"] == verified_id) return profile;
        throw std::runtime_error("Verified fixture missing");
    }
    void Save() { std::ofstream(path) << manifest.dump(); }
};
}

TEST(PayloadGate, VerifiedBuildAcceptedWithoutArmingRuntime) {
    auto result = wivrn::roblox::CheckBuild(verified_id, BACKEND_PROFILE_MANIFEST);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.profile->elf_build_id, verified_id);
}

TEST(PayloadGate, UnknownBuildRefused) {
    auto result = wivrn::roblox::CheckBuild(
        "3b211f25d8658e88e28c86a4ff1e02ff96092741", BACKEND_PROFILE_MANIFEST);
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.profile);
}

TEST_F(GateFixture, AlteredOffsetsRefused) {
    Profile()["vr_debug_device_bridge"]["vtable_rva"] = "0x10";
    Save();
    auto result = wivrn::roblox::CheckBuild(verified_id, path);
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.profile);
}

TEST_F(GateFixture, HostBridgePermissionRequired) {
    Profile()["allow_host_abi_bridges"] = false;
    Save();
    EXPECT_FALSE(wivrn::roblox::CheckBuild(verified_id, path));
}

TEST_F(GateFixture, MissingVrProfileRefused) {
    Profile().erase("vr_debug_device_bridge");
    Save();
    EXPECT_FALSE(wivrn::roblox::CheckBuild(verified_id, path));
}

TEST(PayloadGate, MissingElfRefused) {
    auto result = wivrn::roblox::CheckPayload("/nonexistent/libroblox.so", "/nonexistent/profile");
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.error.empty());
}

TEST(PayloadGate, InvalidIdRefused) {
    auto result = wivrn::roblox::CheckBuild("not-an-id", "/nonexistent/profile");
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.profile.has_value());
}

TEST(PayloadGate, LocalCandidateAlwaysRequiresExplicitOptIn) {
    EXPECT_FALSE(wivrn::roblox::CheckBuild(
        "3b211f25d8658e88e28c86a4ff1e02ff96092741", BACKEND_CANDIDATE_MANIFEST));
}

TEST(PayloadGate, LocalCandidateRequiresOptionalCompilation) {
    auto result = wivrn::roblox::CheckBuild(
        "3b211f25d8658e88e28c86a4ff1e02ff96092741", BACKEND_CANDIDATE_MANIFEST, true);
    EXPECT_EQ(static_cast<bool>(result), static_cast<bool>(BACKEND_INSTALLED_CONTRACT));
}

TEST(PayloadGate, OptionalContractCannotInstallWithoutExactBuildConsent) {
    auto lookup = mocktail::compat::FindBuildProfile(BACKEND_CANDIDATE_MANIFEST,
        "3b211f25d8658e88e28c86a4ff1e02ff96092741");
    ASSERT_TRUE(lookup);
    ASSERT_TRUE(lookup.profile);
    unsetenv("WIVRN_ROBLOX_EXPERIMENTAL_BUILD");
    mocktail::vr::RobloxVrDeviceBridge bridge;
    EXPECT_FALSE(bridge.Install(*lookup.profile));
}

TEST(PayloadGate, CandidateCannotBypassOptInByClaimingSupportedStatus) {
    nlohmann::json manifest;
    std::ifstream(BACKEND_CANDIDATE_MANIFEST) >> manifest;
    manifest["profiles"][0]["status"] = "supported";
    const std::string path = std::string(MOCKTAIL_TEST_BINARY_DIRECTORY) + "/candidate-status-fixture.json";
    std::ofstream(path) << manifest.dump();
    auto result = wivrn::roblox::CheckBuild(
        "3b211f25d8658e88e28c86a4ff1e02ff96092741", path);
    std::filesystem::remove(path);
    EXPECT_FALSE(result);
}
