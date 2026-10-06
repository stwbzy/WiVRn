#include "installed_contract.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(InstalledContract, ExactPayloadPassesReadOnly) {
    EXPECT_TRUE(wivrn::roblox::VerifyInstalledStaticPayload(BACKEND_TEST_ELF).empty());
}

TEST(InstalledContract, ChangedBytesWithSameBuildIdAreRejected) {
    const auto path = std::filesystem::path(MOCKTAIL_TEST_BINARY_DIRECTORY) / "changed-payload-fixture.so";
    std::filesystem::copy_file(BACKEND_TEST_ELF, path, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.good());
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    const auto error = wivrn::roblox::VerifyInstalledStaticPayload(path.string());
    std::filesystem::remove(path);
    EXPECT_NE(error.find("payload_hash"), std::string::npos) << error;
}

TEST(InstalledContract, MissingPayloadFailsClosed) {
    EXPECT_FALSE(wivrn::roblox::VerifyInstalledStaticPayload("/nonexistent/libroblox.so").empty());
}
