# The expected prefix is captured after all subdirectories have been configured.
# This catches a late KDEInstallDirs override after wivrn_config.h was generated.
file(STRINGS "${WIVRN_CONFIG_FILE}" compiled_prefix
    REGEX "^#define WIVRN_INSTALL_PREFIX ")
if(NOT compiled_prefix STREQUAL "#define WIVRN_INSTALL_PREFIX \"${EXPECTED_PREFIX}\"")
    message(FATAL_ERROR
        "Compiled installation prefix does not match cmake --install: "
        "${compiled_prefix}; expected ${EXPECTED_PREFIX}")
endif()
