# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include(ExternalProject)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS compute_badgevms)
project(why2025_firmware)

add_subdirectory(compute_badgevms_sdk_libs)

add_subdirectory(compute_badgevms_sdk_apps)

ExternalProject_Add(compute_badgevms_host_tests
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/compute_badgevms_host_tests
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/compute_badgevms_host_tests
    BUILD_COMMAND ninja
    INSTALL_COMMAND ninja test
    CONFIGURE_HANDLED_BY_BUILD false
    BUILD_ALWAYS 1
)

set(FATFS_STAGING_DIR ${CMAKE_BINARY_DIR}/compute_storage_staging)
set(FATFS_STATIC_DIR ${CMAKE_SOURCE_DIR}/compute_storage)
file(MAKE_DIRECTORY ${FATFS_STAGING_DIR})

function(prepare_fatfs_image)
    if(EXISTS ${FATFS_STATIC_DIR})
        file(COPY ${FATFS_STATIC_DIR}/ DESTINATION ${FATFS_STAGING_DIR})
    endif()

    get_property(ALL_APP_ELFS GLOBAL PROPERTY APP_ELFS)

    set(FATFS_STAGED_FILES "")
    set(APP_ELF_TARGETS "")

    foreach(APP_ELF ${ALL_APP_ELFS})
        get_filename_component(ELF_NAME ${APP_ELF} NAME)

        set(STAGED_FILE ${FATFS_STAGING_DIR}/${ELF_NAME})

        add_custom_command(
            OUTPUT ${STAGED_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy ${APP_ELF} ${STAGED_FILE}
            COMMAND ${CMAKE_STRIP} ${STAGED_FILE}
            DEPENDS ${APP_ELF}
            COMMENT "Copying ${ELF_NAME} to fatfs staging"
        )

        list(APPEND FATFS_STAGED_FILES ${STAGED_FILE})

        get_filename_component(ELF_NAME_WE ${APP_ELF} NAME_WE)
        list(APPEND APP_ELF_TARGETS ${ELF_NAME_WE}_elf)
    endforeach()

    add_custom_target(prepare_fatfs_staging ALL
        DEPENDS ${FATFS_STAGED_FILES}
    )

    # Ensure the staging target depends on all app ELF targets
    if(APP_ELF_TARGETS)
        add_dependencies(prepare_fatfs_staging ${APP_ELF_TARGETS})
    endif()
endfunction()

prepare_fatfs_image()

fatfs_create_spiflash_image(storage ${FATFS_STAGING_DIR} FLASH_IN_PROJECT PRESERVE_TIME)
add_dependencies(fatfs_storage_bin prepare_fatfs_staging)

add_custom_target(sdk
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/sdk_staging ${CMAKE_SOURCE_DIR}/sdk_dist
    DEPENDS build_sdk ${CMAKE_SOURCE_DIR}/compute_badgevms_sdk_include/ ${CMAKE_SOURCE_DIR}/compute_badgevms/include
    COMMENT "Creating SDK distribution in sdk_dist/"
)
