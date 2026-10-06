#include "payload_gate.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: wivrn-roblox-preflight [--experimental] LIBROBLOX_ELF COMPATIBILITY_JSON\n"
                     "Read-only exact-build check. Does not load or patch Roblox.\n"
                     "Exit: 0=contract available, 2=refused, 1=usage/error.\n";
        return 0;
    }
    const bool experimental = argc == 4 && std::string_view(argv[1]) == "--experimental";
    if (argc != 3 && !experimental) {
        std::cerr << "Use --help for usage.\n";
        return 1;
    }
    try {
        const int index = experimental ? 2 : 1;
        const auto result = wivrn::roblox::CheckPayload(argv[index], argv[index + 1], experimental);
        std::cout << "build_id=" << result.build_id << '\n';
        if (!result) {
            std::cout << "contract_available=0 reason=" << result.error << '\n';
            return 2;
        }
        std::cout << "contract_available=1 version=" << result.profile->version_name
                  << " integration_required=1\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
