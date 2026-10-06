cmake_minimum_required(VERSION 3.22)
project(WiVRnRobloxBackend VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOCKTAIL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../mocktail-vr" CACHE PATH "Pinned upstream sources")
set(JSON_INCLUDE_DIR "" CACHE PATH "Directory containing nlohmann/json.hpp")
set(EXPECTED_UPSTREAM_REVISION "9a1ccbb5cebea7bb578bfb198d2a3246734e6a40")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${MOCKTAIL_SOURCE}" OUTPUT_VARIABLE revision
  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT revision STREQUAL EXPECTED_UPSTREAM_REVISION)
  message(FATAL_ERROR "Unexpected Mocktail revision: ${revision}. Review before changing pin.")
endif()
find_path(JSON_HEADERS nlohmann/json.hpp HINTS "${JSON_INCLUDE_DIR}" REQUIRED)
find_package(OpenXR REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SDL3 3.4 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPLACEBO REQUIRED IMPORTED_TARGET libplacebo)
find_library(ELF_LIBRARY elf REQUIRED)

# Compile the real native XR/device bridge without the Android launcher/UI.
set(vr "${MOCKTAIL_SOURCE}/src/vr")
option(WIVRN_ENABLE_INSTALLED_VR_CONTRACT "Compile the local static-only 3056 candidate (not live verified)" OFF)
set(vr_bridge_source "${vr}/roblox_vr_device_bridge.cc")
if(WIVRN_ENABLE_INSTALLED_VR_CONTRACT)
  file(READ "${vr_bridge_source}" bridge_source)
  set(anchor "static constexpr VrBuildContract kContracts[] = {")
  string(FIND "${bridge_source}" "${anchor}" anchor_position)
  if(anchor_position EQUAL -1)
    message(FATAL_ERROR "Pinned upstream contract table anchor missing")
  endif()
  string(REPLACE "${anchor}" "${anchor}\n      wivrn::roblox::installed_contract," bridge_source "${bridge_source}")
  set(install_anchor "Status RobloxVrDeviceBridge::Install(const compat::BuildProfile &profile) {")
  string(FIND "${bridge_source}" "${install_anchor}" install_position)
  if(install_position EQUAL -1)
    message(FATAL_ERROR "Pinned upstream Install anchor missing")
  endif()
  string(REPLACE "${install_anchor}" "${install_anchor}
  if (profile.elf_build_id == wivrn::roblox::installed_contract.build_id) {
    const char* consent = std::getenv(\"WIVRN_ROBLOX_EXPERIMENTAL_BUILD\");
    if (!consent || profile.elf_build_id != consent)
      return FailedPrecondition(\"Local static candidate requires exact experimental Build ID opt-in\");
  }" bridge_source "${bridge_source}")
  set(vr_bridge_source "${CMAKE_CURRENT_BINARY_DIR}/generated/roblox_vr_device_bridge.cc")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(WRITE "${vr_bridge_source}" "#include \"installed_contract.h\"\n${bridge_source}")
endif()
add_library(wivrn_roblox_backend SHARED
  ${vr}/diagnostics.cc ${vr}/openxr_backend_mode.cc ${vr}/vr_perf.cc
  ${vr}/vr_mirror.cc ${vr}/xr_controller.cc ${vr_bridge_source}
  ${vr}/openxr_backend.cc ${vr}/gles_transport.cc ${vr}/xr_actions.cc
  ${vr}/openxr_probe.cc ${vr}/openxr_preview.cc
  ${MOCKTAIL_SOURCE}/src/compat/build_profile.cc
  ${MOCKTAIL_SOURCE}/src/compat/elf_build_id.cc
  payload_gate.cpp contract_verify.cpp)
target_include_directories(wivrn_roblox_backend PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${MOCKTAIL_SOURCE}/include"
  PRIVATE "${MOCKTAIL_SOURCE}/src" "${JSON_HEADERS}")
target_compile_definitions(wivrn_roblox_backend PRIVATE XR_USE_GRAPHICS_API_VULKAN)
set_source_files_properties(${vr}/openxr_backend.cc PROPERTIES
  COMPILE_DEFINITIONS "XR_USE_GRAPHICS_API_OPENGL_ES;XR_USE_PLATFORM_EGL")
target_compile_options(wivrn_roblox_backend PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(wivrn_roblox_backend PRIVATE
  OpenXR::openxr_loader Vulkan::Vulkan Threads::Threads ${ELF_LIBRARY} ${CMAKE_DL_LIBS} rt OpenSSL::Crypto)
target_link_options(wivrn_roblox_backend PRIVATE -Wl,-z,defs)
set_target_properties(wivrn_roblox_backend PROPERTIES OUTPUT_NAME wivrn-roblox-backend)

add_executable(wivrn-roblox-preflight preflight.cpp)
target_link_libraries(wivrn-roblox-preflight PRIVATE wivrn_roblox_backend)
add_executable(wivrn-roblox-xr-probe "${vr}/probe_main.cc")
target_link_libraries(wivrn-roblox-xr-probe PRIVATE wivrn_roblox_backend)
add_executable(wivrn-roblox-backend-smoke backend_smoke.cpp)
target_link_libraries(wivrn-roblox-backend-smoke PRIVATE wivrn_roblox_backend)
add_executable(wivrn-roblox-contract-verify contract_verify_main.cpp)
target_include_directories(wivrn-roblox-contract-verify PRIVATE "${MOCKTAIL_SOURCE}/src")
target_link_libraries(wivrn-roblox-contract-verify PRIVATE wivrn_roblox_backend OpenSSL::Crypto)

# Real Android Vulkan WSI/renderer adapter, still explicitly loaded by its host.
set(graphics "${MOCKTAIL_SOURCE}/src/graphics")
add_library(wivrn_roblox_host_adapter SHARED
  ${graphics}/bionic_vulkan_loader_adapter.cc
  ${graphics}/vulkan_text_overlay_compositor.cc
  ${graphics}/sdl_vulkan_wsi.cc ${graphics}/android_vulkan_wsi_adapter.cc
  ${graphics}/present_mode_policy.cc)
target_include_directories(wivrn_roblox_host_adapter PRIVATE "${MOCKTAIL_SOURCE}/include")
target_link_libraries(wivrn_roblox_host_adapter PRIVATE SDL3::SDL3
  PkgConfig::LIBPLACEBO ${CMAKE_DL_LIBS} Threads::Threads)
# Bind the adapter's dispatch table to its wrappers, not the host loader exports.
target_link_options(wivrn_roblox_host_adapter PRIVATE -Wl,-z,defs -Wl,-Bsymbolic-functions)
set_target_properties(wivrn_roblox_host_adapter PROPERTIES OUTPUT_NAME wivrn-roblox-host-adapter)
add_executable(wivrn-roblox-host-smoke host_adapter_smoke.cpp)
target_link_libraries(wivrn-roblox-host-smoke PRIVATE wivrn_roblox_backend SDL3::SDL3 ${CMAKE_DL_LIBS})
target_link_options(wivrn-roblox-host-smoke PRIVATE -Wl,--export-dynamic)

include(CTest)
set(WIVRN_TEST_PAYLOAD "" CACHE FILEPATH "Optional local ELF for read-only candidate verification tests")
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  add_executable(backend-tests payload_gate_test.cpp
    ${MOCKTAIL_SOURCE}/tests/vr/openxr_backend_test.cc
    ${MOCKTAIL_SOURCE}/tests/vr/roblox_vr_device_bridge_test.cc
    ${MOCKTAIL_SOURCE}/tests/vr/xr_controller_test.cc
    ${MOCKTAIL_SOURCE}/tests/vr/vr_pose_math_test.cc)
  target_include_directories(backend-tests PRIVATE "${MOCKTAIL_SOURCE}/src" "${JSON_HEADERS}")
  target_compile_definitions(backend-tests PRIVATE
    MOCKTAIL_TEST_BINARY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
    BACKEND_INSTALLED_CONTRACT=$<BOOL:${WIVRN_ENABLE_INSTALLED_VR_CONTRACT}>
    BACKEND_CANDIDATE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/installed-candidate.json"
    BACKEND_PROFILE_MANIFEST="${MOCKTAIL_SOURCE}/config/roblox_compatibility.json")
  target_link_libraries(backend-tests PRIVATE wivrn_roblox_backend GTest::gtest_main)
  add_test(NAME backend-tests COMMAND backend-tests)
  # Perf enablement is cached on first use; preserve upstream's process isolation.
  add_executable(backend-mirror-tests ${MOCKTAIL_SOURCE}/tests/vr/vr_mirror_test.cc)
  target_link_libraries(backend-mirror-tests PRIVATE wivrn_roblox_backend GTest::gtest_main)
  add_test(NAME backend-mirror-tests COMMAND backend-mirror-tests)
  add_executable(backend-queue-tests ${MOCKTAIL_SOURCE}/tests/vr/queue_submission_test.cc)
  target_link_libraries(backend-queue-tests PRIVATE wivrn_roblox_host_adapter wivrn_roblox_backend GTest::gtest_main)
  add_test(NAME backend-queue-tests COMMAND backend-queue-tests)
  if(WIVRN_TEST_PAYLOAD)
    add_executable(backend-contract-tests contract_verification_test.cpp)
    target_include_directories(backend-contract-tests PRIVATE "${MOCKTAIL_SOURCE}/src")
    target_compile_definitions(backend-contract-tests PRIVATE
      BACKEND_TEST_ELF="${WIVRN_TEST_PAYLOAD}"
      MOCKTAIL_TEST_BINARY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(backend-contract-tests PRIVATE wivrn_roblox_backend GTest::gtest_main)
    add_test(NAME backend-contract-tests COMMAND backend-contract-tests)
  endif()
endif()
