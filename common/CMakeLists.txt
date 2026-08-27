if (WIVRN_USE_SYSTEM_BOOST)
    add_library(Boost::pfr ALIAS Boost::headers)
else()
    set(BOOST_INCLUDE_LIBRARIES locale url pfr iostreams)
    FetchContent_MakeAvailable(boost)
endif()

configure_file(wivrn_config.h.in wivrn_config.h)

add_library(wivrn-common-base STATIC EXCLUDE_FROM_ALL
    frame_synthesis_policy.cpp
    utils/strings.cpp
    utils/xdg_base_directory.cpp
    ${CMAKE_BINARY_DIR}/version.cpp
)
add_dependencies(wivrn-common-base wivrn-version)

add_library(wivrn-common STATIC EXCLUDE_FROM_ALL
    crypto.cpp
    smp.cpp
    secrets.cpp
    wivrn_sockets.cpp
    utils/strings.cpp
    vk/allocation.cpp
    vk/error_category.cpp
    vk/vk_allocator.cpp
    vk/vk_mem_alloc.cpp
)

if (WIVRN_BUILD_TEST)
	add_executable(frame-synthesis-policy-test frame_synthesis_policy_test.cpp)
	target_compile_features(frame-synthesis-policy-test PRIVATE cxx_std_20)
	target_link_libraries(frame-synthesis-policy-test PRIVATE wivrn-common-base)
	add_test(NAME frame-synthesis-policy COMMAND frame-synthesis-policy-test)

	add_executable(frame-synthesis-negotiation-test frame_synthesis_negotiation_test.cpp)
	target_compile_features(frame-synthesis-negotiation-test PRIVATE cxx_std_20)
	target_link_libraries(frame-synthesis-negotiation-test PRIVATE wivrn-common)
	add_test(NAME frame-synthesis-negotiation COMMAND frame-synthesis-negotiation-test)

	add_executable(body-tracking-protocol-test body_tracking_protocol_test.cpp)
	target_compile_features(body-tracking-protocol-test PRIVATE cxx_std_23)
	target_link_libraries(body-tracking-protocol-test PRIVATE wivrn-common)
	add_test(NAME body-tracking-protocol COMMAND body-tracking-protocol-test)
endif()

if (WIVRN_BUILD_DASHBOARD OR WIVRN_BUILD_SERVER)
    add_library(wivrn-common-server STATIC EXCLUDE_FROM_ALL
        application.cpp
        utils/flatpak.cpp
        utils/load_icon.cpp
        utils/ini.cpp
        utils/steam_info.cpp
        utils/vdf.cpp
        utils/xdg_icon_lookup.cpp
    )
    target_link_libraries(wivrn-common-server
        PRIVATE nlohmann_json::nlohmann_json PNG::PNG PkgConfig::librsvg-2.0 LibArchive::LibArchive Boost::iostreams
        PUBLIC wivrn-common-base)

    if (WIVRN_BUILD_TEST)
        add_executable(connection-lifecycle-test connection_lifecycle_test.cpp)
        target_compile_features(connection-lifecycle-test PRIVATE cxx_std_20)
        add_test(NAME connection-lifecycle COMMAND connection-lifecycle-test)

		add_executable(scaling-watchdog-test scaling_watchdog_test.cpp)
		target_compile_features(scaling-watchdog-test PRIVATE cxx_std_20)
		target_link_libraries(scaling-watchdog-test PRIVATE wivrn-common)
		add_test(NAME scaling-watchdog COMMAND scaling-watchdog-test)

        add_executable(list-apps
            test_application.cpp
        )

        target_link_libraries(list-apps wivrn-common-server)

        add_executable(vdf utils/vdf.cpp)
        target_compile_definitions(vdf PRIVATE WIVRN_BUILD_TEST)
        target_link_libraries(vdf Boost::iostreams)
    endif()
endif()

if(ANDROID)
    # The Vulkan headers in Android do not include the C++ headers, download them
    # Minimum version is 1.3.256
    set(VULKAN_VERSION "1.3.268.0")

    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz)
        if (EXISTS ${CMAKE_SOURCE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz)
            file(CREATE_LINK ${CMAKE_SOURCE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz SYMBOLIC)
        else()
            file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-Headers/archive/vulkan-sdk-${VULKAN_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz
                EXPECTED_HASH SHA256=94993cbe2b1a604c0d5d9ea37a767e1aba4d771d2bfd4ddceefd66243095164f)
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz DESTINATION ${CMAKE_BINARY_DIR})

    target_include_directories(wivrn-common SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/Vulkan-Headers-vulkan-sdk-${VULKAN_VERSION}/include)
    target_compile_definitions(wivrn-common PUBLIC VMA_VULKAN_VERSION=1001000)
else()
    target_link_libraries(wivrn-common PUBLIC Vulkan::Headers)
endif()

target_include_directories(wivrn-common-base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wivrn-common PUBLIC Boost::pfr OpenSSL::Crypto wivrn-external wivrn-common-base)
target_compile_definitions(wivrn-common PUBLIC VULKAN_HPP_NO_STRUCT_CONSTRUCTORS)
