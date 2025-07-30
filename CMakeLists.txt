cmake_minimum_required(VERSION 3.16)

include(ExternalProject)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build compute_badgevms/ as compute_badgevms.bin
set(EXTRA_COMPONENT_DIRS compute_badgevms)
set(COMPONENTS compute_badgevms)
project(compute_badgevms)

# Build compute_badgevms_sdk_libs/
add_subdirectory(compute_badgevms_sdk_libs)

# Build compute_badgevms_sdk_apps/
# Outputs are copied to the compute_storage fatfs
add_subdirectory(compute_badgevms_sdk_apps)

# Build compute_storage/ as storage.bin
add_subdirectory(compute_storage)

# Build and run compute_badgevms_host_tests/
ExternalProject_Add(compute_badgevms_host_tests
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/compute_badgevms_host_tests
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/compute_badgevms_host_tests
    BUILD_ALWAYS true
    INSTALL_COMMAND ""
    TEST_AFTER_INSTALL true
)

# Generate sdk_dist/
add_custom_target(sdk
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/sdk_staging ${CMAKE_SOURCE_DIR}/sdk_dist
    DEPENDS build_sdk ${CMAKE_SOURCE_DIR}/compute_badgevms_sdk_include/ ${CMAKE_SOURCE_DIR}/compute_badgevms/include
    COMMENT "Creating SDK distribution in sdk_dist/"
)
