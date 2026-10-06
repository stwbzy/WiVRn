#include "installed_contract.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: wivrn-roblox-contract-verify LIBROBLOX_ELF\n"
                     "Read-only candidate verification. Never activates hooks.\n";
        return 1;
    }
    auto error = wivrn::roblox::VerifyInstalledStaticPayload(argv[1], &std::cout);
    if (!error.empty()) { std::cerr << error << '\n'; return 2; }
    std::cout << "static_abi_verified=1 runtime_hooks_installed=0 live_roblox_verified=0\n";
    return 0;
}
