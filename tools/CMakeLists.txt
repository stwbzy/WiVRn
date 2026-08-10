if(WIVRN_BUILD_DISSECTOR)
	add_subdirectory(wireshark)
endif()

if (WIVRN_BUILD_WIVRNCTL)
	add_subdirectory(wivrnctl)
endif()

if (WIVRN_BUILD_TEST AND NOT ANDROID)
	add_executable(fsr-vulkan-smoke fsr_vulkan_smoke.cpp)
	target_compile_features(fsr-vulkan-smoke PRIVATE cxx_std_20)
	target_link_libraries(fsr-vulkan-smoke PRIVATE Vulkan::Vulkan)
	set_target_properties(fsr-vulkan-smoke PROPERTIES OUTPUT_NAME wivrn-fsr-vulkan-smoke)
endif()
