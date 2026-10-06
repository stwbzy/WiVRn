#include "mocktail/vr/openxr_backend.h"
#include <iostream>

int main() {
    // Exercise the real backend, not the independent preview renderer.
    mocktail::vr::OpenXrBackend backend;
    const auto status = backend.Arm();
    if (!status) {
        std::cerr << "arm_failed=" << status.message() << '\n';
        return 1;
    }
    std::cout << "backend_armed=" << backend.armed()
              << " runtime=" << backend.runtime_name()
              << " submitted_frames=" << backend.submitted_frames() << '\n';
    backend.Disarm();
    std::cout << "backend_armed_after_disarm=" << backend.armed() << '\n';
    return backend.armed() ? 1 : 0;
}
