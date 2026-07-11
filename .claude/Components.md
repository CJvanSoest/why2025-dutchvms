# Components

What lives where. See [Data-Flows.md](Data-Flows.md) for how these actually
talk to each other at runtime.

## `badgevms/` — the P4 kernel

### Kernel core

| File | Owns |
|---|---|
| `task.c` / `task.h` | PID allocation, ELF-loading task creation (`run_task`/`run_task_path`), the process table (`process_table_lock`), the `hades`/`zeus`/`cerberos` FreeRTOS tasks that handle process death, resource cleanup and crash isolation. |
| `device.c` / `include/badgevms/device.h` | The device registry — a name→device_t hash table (`device_table_lock`) that drivers register into and apps look up by logical name. |
| `memory.c` | PSRAM/heap bring-up, per-task memory accounting. |
| `application.c` | App manifest lifecycle: `application_create/destroy/list/get`, backed by `<uid>.json` files written as **siblings** of the app's install directory in `applications_base_dir`, not inside it — `application_destroy()` must delete both (see Pitfalls.md). |
| `logical_names.c` | VMS-style logical name resolution (`DEVICE:[dir.subdir]filename` paths, e.g. `SEARCH` → `FLASH0:[SUBDIR], FLASH0:[SUBDIR.ANOTHER]`). |
| `compositor/compositor.c` | Window/framebuffer management, double-buffering, `window_present()`. `compositor/pixel_functions.c` and `compositor/window_decorations.c` are its helpers. |
| `deploy_protocol.c` | The UART app-deploy protocol listener (PUT/GET/LIST/DELETE/PING over a CRC-16/X.25-framed binary protocol on UART0). Client: `why2025-apps/tools/badge_deploy.py`. All paths go through `vms_to_posix()` — validate segments there, not downstream. |
| `ota.c` / `include/badgevms/ota.h` | Generic OTA partition write + rollback-safety API (`ota_session_open/write/commit/abort`, `validate_ota_partition`/`invalidate_ota_partition`). Writes only `ota_0`/`ota_1`, never the `storage` FAT partition — see `docs/design/SD-and-OTA-Updates.md`. |
| `why2025_firmware.c` | This fork's `app_main()` entry point and boot sequence — see Data-Flows.md "Cold start". |
| `wrapped_funcs.c` / `why_io.h` | libc wrapper layer (`why_malloc`, `why_fopen`, etc.) plus `why_die()`, the kernel-fatal-error helper (see Guidelines.md's hard rules). |
| `init.c` | Config/app-manifest parsing at boot, `validate_ota_partition()` call site. |

### `badgevms/drivers/`

| File | Owns |
|---|---|
| `wifi.c` | WiFi station bring-up, SNTP clock sync on connect, kicks off `flash_slave_c6_if_needed()`. |
| `tca8418.c` | Keyboard driver (TCA8418 I2C keypad controller) — scancode table, event decode. Read this file's own history in Pitfalls.md before touching the buffer-fill loop. |
| `ble_gatt_server.c` | BLE GATT server used by apps' companion-pairing flows (e.g. cj_meshcore's phone companion). |
| `st7703.c` | Display panel driver. |
| `lora_proto_client.c` | P4-side client for the C6's LoRa protocol server — the other half of the wire boundary described in Data-Flows.md. Exposed to apps via `include/badgevms/lora.h`. |
| `badgevms_i2c_bus.c` | Generic I2C bus/device VFS layer **only** — the LED-matrix, status-LED and board-bringup code that used to live here was split out (see below); don't let new unrelated peripheral code creep back in. |
| `board_bringup.c` / `.h` | Vibrator default-off + PCA9698 LED-matrix-add-on presence detection on I2C2; starts the other two on success. |
| `led_matrix_pca9698.c` / `led_matrix_internal.h` | The 12×20 PCA9698 LED-matrix driver (bit-bang + HW-i2c refresh backends). App-facing surface: `include/badgevms/led_matrix.h` + `led_matrix_bridge.c`. |
| `status_led_ws2812.c` / `status_led_internal.h` | The 4× RGBW status LEDs (WS2812/RMT). App-facing surface: `include/badgevms/status_led.h` + `status_led_bridge.c`. |
| `esp-serial-flasher/slave_c6_flasher.c` | Flashes the C6 over UART1 from SD-staged binaries (`flash_slave_c6_if_needed()`) — the proven "install from SD, MD5-gated" pattern other update mechanisms should mirror. |
| `badgelink/` | An alternate, experimental UART app-deploy transport (Tanmatsu's own protocol) — off by default (`CJ_BADGEVMS_ENABLE_BADGELINK` Kconfig option), covered by its own non-blocking CI job so it doesn't silently bit-rot against `deploy_protocol.c`. |

## `connectivity_esp_hosted/slave/` — the C6 co-processor firmware

A fork of Espressif's esp-hosted slave. Most of `main/` tracks upstream and
should be left alone; the first-party addition is:

| File | Owns |
|---|---|
| `main/tanmatsu/lora/lora_protocol_server.c` | The LoRa protocol server: decodes P4 requests (`lora_protocol_handle_packet()`), drives the SX126x over SPI, and now (post async-TX rework) queues `PACKET_TX` to a dedicated `lora_tx_task` instead of blocking the shared esp-hosted Rx thread — see Data-Flows.md. `radio_op_mutex` guards each full logical radio operation. |
| `main/tanmatsu/lora/lora_protocol.h` | The wire-format structs shared *by convention* with `badgevms/drivers/lora_proto_client.c` — no common header, kept in sync by hand. |
| `main/tanmatsu/infrared/` | IR protocol server (separate peripheral, same pattern as LoRa: a protocol server exposed to the P4 over esp-hosted custom events). |
| `main/slave_control.c` | The esp-hosted custom-RPC dispatch this fork's LoRa/IR servers hook into (`handle_custom_rpc_request()`) — its own comment documents the "runs on the Rx thread, do not block" contract that the LoRa async-TX rework exists to respect. |

## Where the real apps live (not this repo)

[why2025-apps](https://github.com/CJvanSoest/why2025-apps): `apps/cj_*`
(MeshCore, storage browser, WiFi analyzer, sensors, LED-matrix control app,
the launcher, ...) plus `tools/badge_deploy.py`. Built independently against
`sdk_dist/` (produced here by `idf.py sdk`), deployed as PIE ELFs onto the
SD card — never linked into this repo's own build.
