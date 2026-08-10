if(WIVRN_BUILD_DISSECTOR)
	add_subdirectory(wireshark)
endif()

if (WIVRN_BUILD_WIVRNCTL)
	add_subdirectory(wivrnctl)
endif()

if (WIVRN_BUILD_TEST AND NOT ANDROID)
	if (WIVRN_USE_SYSTEM_OPENXR)
		find_package(OpenXR 1.1.58 REQUIRED)
	endif()

	add_executable(wivrn-fsr-openxr-harness fsr_openxr_harness.cpp)
	target_compile_definitions(wivrn-fsr-openxr-harness PRIVATE XR_USE_GRAPHICS_API_VULKAN)
	target_link_libraries(wivrn-fsr-openxr-harness PRIVATE OpenXR::openxr_loader Vulkan::Vulkan)
	set_target_properties(wivrn-fsr-openxr-harness PROPERTIES OUTPUT_NAME wivrn-fsr-openxr-harness)
endif()
