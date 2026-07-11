# Data flows

How this actually runs, with real function names. Read this before chasing a
symptom to the wrong file or the wrong side of the P4/C6 boundary.

## Cold start (P4)

`why2025_firmware.c`'s `app_main()`, roughly in order:

1. `memory_init()` — PSRAM/heap bring-up. If this fails, nothing past here is
   safe; boot doesn't continue.
2. `task_init()` — process table, PID allocator, the `hades`/`zeus` kernel
   tasks. On failure, `invalidate_ota_partition()` is called (this build is
   treated as bad; the bootloader rolls back to the previous OTA partition
   next boot).
3. `notify_system_init()` — the cross-app notification/badge system apps use
   for unread-message indicators etc.
4. `device_init()` — the device registry. Same `invalidate_ota_partition()`
   pattern on failure.
5. `logical_names_system_init()` — VMS-style path resolution.
6. Driver bring-up — `badgevms_i2c_bus_create()` for the main I2C bus
   (which on `port == 0` also kicks off `board_bringup_start()`: vibrator
   default-off, then PCA9698 LED-matrix-add-on detection → the matrix
   refresh task + the WS2812 status-LED task if found), keyboard, display,
   WiFi (`wifi_create()`, which also calls `flash_slave_c6_if_needed()` —
   the C6 gets reflashed here, at every boot, whenever the SD-staged
   binaries' MD5 differs from what's already on the chip).
7. `deploy_protocol_init()` (or `badgelink_transport_uart_init()` if
   `CJ_BADGEVMS_ENABLE_BADGELINK` is on) — starts the UART app-deploy
   listener. Non-fatal if it fails.
8. `init.c`'s config/manifest parsing, `validate_ota_partition()` (marks
   this boot's partition valid, cancelling the ESP-IDF rollback timer).
9. The launcher app is loaded via `run_task_path()` like any other app —
   nothing in the kernel hardcodes "launcher is special" beyond it being the
   configured default app.

## The P4↔C6 LoRa RPC

Every LoRa operation crosses the P4/C6 boundary as a custom esp-hosted event
(`TANMATSU_EVENT_LORA`), carrying an 8-byte `lora_protocol_header_t`
(`sequence_number` + `type`) plus a type-specific payload.

**P4 side** (`badgevms/drivers/lora_proto_client.c`, exposed to apps via
`include/badgevms/lora.h`): `request_reply()` builds a request, sends it, and
blocks (with a timeout) for the matching ACK/NACK or reply. `PACKET_TX`'s
payload is a `lora_protocol_lora_packet_t` (`uint8_t length; uint8_t
data[];`) — a length-prefixed sub-structure, not the raw LoRa bytes
themselves.

**C6 side** (`lora_protocol_server.c`): `lora_protocol_packet_callback()` is
invoked directly from `slave_control.c`'s custom-RPC dispatch, **on the
shared esp-hosted Rx thread**, which has a documented no-blocking-calls
contract. `lora_protocol_handle_packet()` switches on request type:

- `GET_MODE`/`SET_MODE`/`GET_CONFIG`/`GET_STATUS` reply synchronously — fast,
  no radio TX/RX wait involved.
- `SET_CONFIG` calls `apply_config()`, which takes `radio_op_mutex` for the
  duration of its multi-step SX126x SPI sequence, then replies synchronously
  (still fast — no TX_DONE wait).
- `PACKET_TX` does **not** transmit inline. It unwraps the
  `lora_protocol_lora_packet_t`, builds a `lora_tx_request_t`, and
  `xQueueSend()`s it (non-blocking) to `tx_request_queue`. A dedicated
  `lora_tx_task` picks it up, takes `radio_op_mutex`, runs the actual
  multi-step transmit sequence (which blocks up to 1000ms waiting for the
  TX_DONE IRQ), and sends the ACK/NACK back to the P4 only once that
  completes — asynchronously, off the Rx thread.

`lora_task` (a separate FreeRTOS task, IRQ-driven via `sx126x_irq_wait()`)
owns receiving: on `SX126X_COMMAND_STATUS_DATA_AVAILABLE` it calls
`read_data()` (which packages the received bytes + signal-quality stats into
a `PACKET_RX` event sent unsolicited to the P4) and re-arms RX mode — also
under `radio_op_mutex`, so it can't interleave with a transmit or a config
apply mid-sequence.

This is why a wire-format bug here is silent rather than a build or runtime
error: nothing type-checks a `lora_protocol_lora_packet_t` against what the
other side expects. The C6 firmware once radiated the length-prefix byte
itself as the first byte of the actual over-the-air payload, shifting every
subsequent byte by one — every packet this firmware ever sent had that bug,
and it produced no error anywhere, just adverts and messages that real
MeshCore nodes silently rejected. See Pitfalls.md.

## App install and launch

1. **Install**: `why2025-apps/tools/badge_deploy.py put local.elf
   "SD0:[BADGEVMS.APPS.name]name.elf"` streams the file over UART0 through
   `deploy_protocol.c`'s CRC-framed PUT command. `vms_to_posix()` turns the
   VMS-style path into a POSIX path on the SD card (rejecting `.`/`..`
   segments). A manifest (`<uid>.json`) is written as a **sibling** of the
   app's install directory, not inside it.
2. **Enumerate**: `application_list()` scans `applications_base_dir` for
   `*.json` manifests, snapshotting the directory listing before opening
   each file (a retry-loop guards against an SD read race that otherwise
   intermittently returns empty content for a valid file — see Pitfalls.md).
3. **Launch**: the launcher (or any app) calls `run_task_path()` (`task.c`),
   which allocates a PID, loads the ELF, and creates a FreeRTOS task/process
   for it. Process death is handled asynchronously by the `hades` kernel
   task, which is signalled via `hades_queue` and cleans up the process
   table entry, thread resources, and any orphaned child processes.
4. **Uninstall**: `application_destroy()` must remove **both** the install
   directory (`rm_rf`) and the sibling manifest — deleting only the
   directory leaves a ghost, permanently-unreusable entry (fixed once
   already, see Pitfalls.md; don't reintroduce the asymmetry).
