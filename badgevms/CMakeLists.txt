idf_component_register(
    SRCS
     ${CMAKE_CURRENT_BINARY_DIR}/generated_symbols.c
     "application.c"
     "badgelink_transport_uart.c"
     "buddy_alloc.c"
     "compositor/compositor.c"
     "compositor/pixel_functions.c"
     "compositor/window_decorations.c"
     "crypto.c"
     "curl.c"
     "deploy_protocol.c"
     "device.c"
     "display_bridge.c"
     "drivers/badgelink/badgelink.c"
     "drivers/badgelink/badgelink.pb.c"
     "drivers/badgelink/badgelink_appfs.c"
     "drivers/badgelink/badgelink_fs.c"
     "drivers/badgelink/badgelink_nvs.c"
     "drivers/badgelink/badgelink_setusbmode.c"
     "drivers/badgelink/badgelink_startapp.c"
     "drivers/badgelink/cobs.c"
     "drivers/badgelink/nanopb/pb_common.c"
     "drivers/badgelink/nanopb/pb_decode.c"
     "drivers/badgelink/nanopb/pb_encode.c"
     "drivers/badgevms_i2c_bus.c"
     "drivers/ble_gatt_server.c"
     "drivers/board_bringup.c"
     "drivers/bosch_bmi270.c"
     "drivers/bosch_bme690.c"
     "drivers/display_backlight_client.c"
     "drivers/esp-serial-flasher/slave_c6_flasher.c"
     "drivers/esp-serial-flasher/why2025_firmware.c"
     "drivers/fatfs.c"
     "drivers/led_matrix_pca9698.c"
     "drivers/lora_proto_client.c"
     "drivers/socket.c"
     "drivers/st7703.c"
     "drivers/status_led_ws2812.c"
     "drivers/tca8418.c"
     "drivers/tty.c"
     "drivers/wifi.c"
     "init.c"
     "led_matrix_bridge.c"
     "logical_names.c"
     "meminfo_bridge.c"
     "memory.c"
     "nvs_bridge.c"
     "memory_heap_caps.c"
     "notify.c"
     "ota.c"
     "pathfuncs.c"
     "status_led_bridge.c"
     "task.c"
     "thirdparty/cJSON.c"
     "thirdparty/dlmalloc.c"
     "thirdparty/tomlc17.c"
     "user_event.c"
     "why2025_firmware.c"
     "wrapped_funcs.c"
     "wrapped_fs.c"
    INCLUDE_DIRS
     ${CMAKE_CURRENT_SOURCE_DIR}/
     ${CMAKE_CURRENT_SOURCE_DIR}/include
     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/badgelink
     ${CMAKE_CURRENT_SOURCE_DIR}/drivers/badgelink/nanopb
    PRIV_REQUIRES
     "app_update"
     "bmi270"
     "bme690"
     "bt"
     "elf_loader"
     "esp-tls"
     "esp_driver_ppa"
     "esp_http_client"
     "esp_hw_support"
     "esp_lcd"
     "esp_lcd_st7703"
     "esp_mm"
     "esp_driver_ledc"
     "esp_driver_rmt"
     "esp_psram"
     "esp_tca8418"
     "esp_timer"
     "esp_wifi"
     "esp_wifi_remote"
     "fatfs"
     "freertos"
     "hal"
     "lwip"
     "mbedtls"
     "nvs_flash"
     "sd_card"
     "spi_flash"
     "vfs"
     "why_stdio"
     EMBED_TXTFILES
     "../flash_storage/skel/init.toml"
)

target_link_libraries(${COMPONENT_LIB} PRIVATE
    m
    g

    -Wl,--wrap=esp_panic_handler
    -Wl,--wrap=xt_unhandled_exception

    # heap_caps
    -Wl,--wrap=heap_caps_malloc_base
    -Wl,--wrap=heap_caps_realloc_base
    -Wl,--wrap=heap_caps_calloc_base
    -Wl,--wrap=heap_caps_malloc
    -Wl,--wrap=heap_caps_malloc_default
    -Wl,--wrap=heap_caps_realloc_default
    -Wl,--wrap=heap_caps_free
    -Wl,--wrap=heap_caps_realloc
    -Wl,--wrap=heap_caps_calloc
    -Wl,--wrap=heap_caps_aligned_alloc
    -Wl,--wrap=heap_caps_aligned_free
    -Wl,--wrap=heap_caps_aligned_calloc
    #-Wl,--wrap=heap_caps_get_total_size
    #-Wl,--wrap=heap_caps_get_free_size
    #-Wl,--wrap=heap_caps_get_minimum_free_size
    #-Wl,--wrap=heap_caps_get_largest_free_block
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    USE_DL_PREFIX
    HAVE_MORECORE=1
    HAVE_MMAP=0
    HAVE_MREMAP=0
    MORECORE=why_sbrk
    MORECORE_CONTIGUOUS=1
    NO_MALLINFO
    malloc_getpagesize=0x10000
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-empty-body
    -Wno-stringop-overflow
    -Wno-char-subscripts # For toml

    # Emit a .su sidecar file per translation unit (file:line:col:function
    # <TAB> stack-frame-bytes <TAB> {static,dynamic,bounded}) so CI can catch
    # oversized stack frames in kernel code -- see scripts/check_stack_usage.py.
    -fstack-usage
)

#
# Generate generated_symbols.c
#

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated_symbols.c
    COMMAND ${PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/generate_symbols.py
            ${CMAKE_CURRENT_SOURCE_DIR}/symbols.yml
            ${CMAKE_CURRENT_BINARY_DIR}/generated_symbols.c
    DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/symbols.yml
     ${CMAKE_CURRENT_SOURCE_DIR}/generate_symbols.py
    COMMENT "Generating symbol table from symbols.yml"
    VERBATIM
)
add_custom_target(generated_symbols
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated_symbols.c
)

add_dependencies(${COMPONENT_LIB} generated_symbols)
