function(wivrn_replace_once variable anchor replacement)
  string(FIND "${${variable}}" "${anchor}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Pinned tuning anchor missing: ${anchor}")
  endif()
  string(REPLACE "${anchor}" "" removed "${${variable}}")
  string(LENGTH "${${variable}}" before)
  string(LENGTH "${removed}" after)
  string(LENGTH "${anchor}" size)
  math(EXPR count "(${before} - ${after}) / ${size}")
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "Pinned tuning anchor is ambiguous: ${anchor}")
  endif()
  string(REPLACE "${anchor}" "${replacement}" result "${${variable}}")
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()

function(wivrn_generated_source target original source)
  get_target_property(sources ${target} SOURCES)
  list(REMOVE_ITEM sources "${original}")
  get_filename_component(name "${original}" NAME)
  set(path "${CMAKE_BINARY_DIR}/wivrn-overlay/${name}")
  file(WRITE "${path}" "${source}")
  list(APPEND sources "${path}")
  set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
endfunction()

function(wivrn_tuning_overlay)
  add_library(wivrn_launcher_tuning STATIC
    "${WIVRN_LAUNCHER_OVERLAY_DIR}/home_view.cpp"
    "${WIVRN_LAUNCHER_OVERLAY_DIR}/render_tuning.cpp")
  target_include_directories(wivrn_launcher_tuning PUBLIC
    "${WIVRN_LAUNCHER_OVERLAY_DIR}" "${CMAKE_SOURCE_DIR}/include")
  mocktail_apply_compile_options(wivrn_launcher_tuning)
  foreach(target IN ITEMS mocktail_vr mocktail_runtime mocktail_experience_runtime)
    target_link_libraries(${target} PRIVATE wivrn_launcher_tuning)
  endforeach()

  file(READ "${CMAKE_SOURCE_DIR}/src/runtime/roblox_experience_composition.cc" source)
  wivrn_replace_once(source
    "return status.ok() ? DrainExternalLaunchRequests() : status;"
    "if (status.ok()) wivrn::launcher::SetHomeActive(true);
  return status.ok() ? DrainExternalLaunchRequests() : status;")
  wivrn_replace_once(source
    "  if (presence_observer_.valid()) {"
    "  wivrn::launcher::SetHomeActive(phase == RobloxExperiencePresencePhase::kBrowsing);
  if (presence_observer_.valid()) {")
  wivrn_replace_once(source
    "Status RobloxExperienceComposition::Shutdown() {"
    "Status RobloxExperienceComposition::Shutdown() {
  wivrn::launcher::SetHomeActive(false);")
  wivrn_generated_source(mocktail_experience_runtime
    src/runtime/roblox_experience_composition.cc "#include \"home_view.h\"\n${source}")

  file(READ "${CMAKE_SOURCE_DIR}/src/runtime/performance_policy.cc" source)
  wivrn_replace_once(source
    "    overrides[\"FFlagDebugDisableDRS\"] = \"True\";"
    "    overrides[\"FFlagDebugDisableDRS\"] = \"True\";
    const char* preset = std::getenv(\"WIVRN_ROBLOX_VR_QUALITY\");
    const auto tuning = wivrn::launcher::Tuning(preset ? preset : \"legacy\");
    if (!tuning.valid) {
      if (error) *error = \"Unknown VR quality preset\";
      return false;
    }
    if (!tuning.legacy) {
      overrides[\"FIntDebugForceMSAASamples\"] = std::to_string(tuning.msaa);
      overrides[\"FIntRenderTextureMipBias\"] = \"0\";
      overrides[\"FFlagRenderEnableLowEndLOD\"] = tuning.low_lod ? \"True\" : \"False\";
      overrides[\"FIntRenderTextureTotalBudgetMB\"] = std::to_string(tuning.texture_budget_mb);
    }")
  wivrn_generated_source(mocktail_runtime src/runtime/performance_policy.cc
    "#include \"render_tuning.h\"\n${source}")

  file(READ "${CMAKE_SOURCE_DIR}/src/vr/openxr_backend.cc" source)
  set(anchor "    xr_eye_width_[0] = config_views[0].recommendedImageRectWidth;")
  wivrn_replace_once(source "${anchor}"
    "    const char* preset = std::getenv(\"WIVRN_ROBLOX_VR_QUALITY\");
    const auto tuning = wivrn::launcher::Tuning(preset ? preset : \"legacy\");
    Require(tuning.valid, \"Unknown VR quality preset\");
${anchor}")
  wivrn_replace_once(source
    "    xr_eye_height_[1] = config_views[1].recommendedImageRectHeight;"
    "    xr_eye_height_[1] = config_views[1].recommendedImageRectHeight;
    const auto max_width = std::min(config_views[0].maxImageRectWidth,
                                    config_views[1].maxImageRectWidth);
    const auto max_height = std::min(config_views[0].maxImageRectHeight,
                                     config_views[1].maxImageRectHeight);
    const double scale = std::min({tuning.scale,
        static_cast<double>(max_width) / std::max(xr_eye_width_[0], xr_eye_width_[1]),
        static_cast<double>(max_height) / std::max(xr_eye_height_[0], xr_eye_height_[1])});
    if (!tuning.legacy) {
      for (int eye = 0; eye < 2; ++eye) {
        xr_eye_width_[eye] = wivrn::launcher::ScaledExtent(xr_eye_width_[eye], max_width, scale);
        xr_eye_height_[eye] = wivrn::launcher::ScaledExtent(xr_eye_height_[eye], max_height, scale);
      }
    }
    Log(\"  [vr-quality] preset=%s scale=%.2f msaa=%d eye=%ux%u\\n\",
        preset ? preset : \"legacy\", scale, tuning.msaa,
        std::max(xr_eye_width_[0], xr_eye_width_[1]), std::max(xr_eye_height_[0], xr_eye_height_[1]));")
  wivrn_generated_source(mocktail_vr src/vr/openxr_backend.cc
    "#include \"render_tuning.h\"\n${source}")
  set_source_files_properties("${CMAKE_BINARY_DIR}/wivrn-overlay/openxr_backend.cc"
    PROPERTIES COMPILE_DEFINITIONS "XR_USE_GRAPHICS_API_OPENGL_ES;XR_USE_PLATFORM_EGL"
    COMPILE_OPTIONS "-fexceptions")

  find_package(GTest REQUIRED)
  add_executable(wivrn-launcher-vr-tests "${WIVRN_LAUNCHER_OVERLAY_DIR}/tuning_test.cpp")
  target_link_libraries(wivrn-launcher-vr-tests PRIVATE wivrn_launcher_tuning GTest::gtest_main)
  add_test(NAME launcher-vr-tuning COMMAND wivrn-launcher-vr-tests)
endfunction()

function(wivrn_bridge_tuning source_variable)
  set(source "${${source_variable}}")
  wivrn_replace_once(source
    "void RobloxVrDeviceBridge::OnPointerFrameCall(void* result, void* service) {"
    "void RobloxVrDeviceBridge::OnPointerFrameCall(void* result, void* service) {
  // The guest draws its visible ray from UserCFrame. Keep its hit-test on that
  // same ray instead of silently substituting a different OpenXR aim pose.
  const char* mode = std::getenv(\"WIVRN_ROBLOX_POINTER\");
  if (mode && std::strcmp(mode, \"native\") == 0) return;")
  wivrn_replace_once(source
    "      const auto pose = backend->PublishedHeadPose();"
    "      static wivrn::launcher::HomeView home_view;
      const auto before = home_view.recenter_count();
      const auto raw = backend->PublishedHeadPose();
      const auto pose = home_view.Update(raw, wivrn::launcher::HomeActive(),
          wivrn::launcher::HomeGeneration(), mocktail::vr::perf::NowNs());
      if (home_view.recenter_count() != before)
        Log(\"  [vr-home] recentered hold_ms=1500 frame=%llu\\n\",
            static_cast<unsigned long long>(pose.frame));")
  set(${source_variable} "#include \"home_view.h\"\n#include \"mocktail/vr/vr_perf.h\"\n${source}" PARENT_SCOPE)
endfunction()
