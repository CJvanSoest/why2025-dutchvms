# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/). The exact
release format, entry style and the steps for cutting a release are fixed in
[Releases.md](guides/Releases.md); follow it rather than inventing per-release wording.

Sections per release:
- **Added** — new features
- **Changed** — changes in existing functionality
- **Deprecated** — soon-to-be removed features
- **Removed** — removed features
- **Fixed** — bug fixes
- **Security** — vulnerabilities and mitigations

History before this file existed (everything up to and including `v1.1.0`) was
not retroactively reconstructed in this format — see `git log` for that range.
Entries from here on are the source of truth going forward.

## [Unreleased]

## [1.5.3] - 2026-08-17

### Fixed
- **v1.5.2's SDIO fix reduced but didn't eliminate the crash** (PR #87) — the shrink-back-down fix
  cut how often the internal/DMA-capable SRAM pool ran dry, but a genuine allocation failure in
  `sdio_rx_get_buffer()` still hit a bare `assert()` and took the whole badge down. Hardware-
  reproduced twice more after v1.5.2 shipped: once mid-download of a large P4 OTA image, once
  during a freshly-booted image's own C6 MD5-check/reset traffic right after a successful OTA. The
  SDIO slave still expects the host to drain the announced transfer length off the bus regardless
  of whether a destination buffer exists, so skipping the read isn't safe — on an allocation
  failure, the driver now drains the transfer into a small static (BSS, not heap, so it's always
  available) scratch buffer and drops the data, instead of crashing. Also fixes a related bug the
  v1.5.2 patch left in place: on allocation failure, the buffer's recorded size was still updated
  to the failed length despite the pointer being NULL, which would have handed out a NULL pointer
  as "already big enough" on the very next call.

## [1.5.2] - 2026-08-17

### Fixed
- **A large WiFi download over the P4↔C6 SDIO link could crash the whole badge** (PR #86) — the
  SDIO RX double-buffer only ever grew to fit the largest transfer it had seen and never shrank
  back down, so a one-off large burst (an OTA firmware update, or a standalone C6 radio bundle
  sync, both routing multi-MB WiFi downloads through this exact path) permanently pinned its peak
  size in the small internal-SRAM/DMA-capable memory pool the whole system shares. Once that pool
  ran out, the next allocation in `sdio_rx_get_buffer()` hit its own `assert()` and took down the
  entire badge (not just the OTA app — this runs in kernel context, so BadgeVMS's own per-app
  crash isolation never got a chance to catch it). Hardware-reproduced during a v1.5.1 OTA update
  and again re-syncing the C6 bundle standalone. Fixed by shrinking the buffer back down after a
  sustained streak of much-smaller reads, so a one-off burst no longer pins its peak size forever.

## [1.5.1] - 2026-08-17

### Added
- **`ota_restart()` app-facing API** (PR #85) — apps can now reboot into a partition they just set
  as the boot target via `ota_session_commit()`. Previously nothing in the OTA path could restart
  the badge; `cj_launcher`'s "Update Firmware" screen always required a manual restart to actually
  boot a newly installed image, even on a clean, error-free update.

### Fixed
- **OTA download progress had no visible total** — `curl.c`'s `HTTP_EVENT_ON_DATA` handler only
  read the response's Content-Length at `HTTP_EVENT_ON_FINISH`, i.e. after the whole transfer had
  already completed, so a caller streaming a download (like `cj_launcher`'s firmware update) could
  never show "downloaded / total" progress — only a running byte count with no end in sight. Fixed
  by refreshing `content_length` on every `HTTP_EVENT_ON_DATA` call instead: headers are already
  fully parsed by the time the first data chunk arrives, so the real total is available immediately.

## [1.5.0] - 2026-08-16

### Added
- **BadgeLink over native USB, app-controlled** (PR #84) — a new
  `badgevms/drivers/usb_device.c` brings up TinyUSB device mode + the WHY2025 carrier's GPIO2 USB
  mux (discovered 2026-08-16 via Senna-chan's `tanmatsu-launcher` port; the earlier 2026-08-07
  BadgeLink removal's "native USB is physically impossible here" verdict was wrong), and
  `badgevms/drivers/badgelink/` vendors `badgeteam/esp32-component-badgelink` with its `badgelink_fs.c`
  reused against this repo's own `/SD0` FATFS mount. Build-verified via the NAS `espressif/idf:v5.5.1`
  docker image (both `badgevms.bin` and `network_adapter.bin`, stack-usage gate green) after two
  fixes: `"usb"` added to `badgevms/CMakeLists.txt`'s `PRIV_REQUIRES` (for `esp_private/usb_phy.h`)
  and `CONFIG_TINYUSB_VENDOR_COUNT=1` added to `sdkconfig.defaults` (TinyUSB's vendor class is
  compiled out otherwise). Boot-verified on a physical badge with the mux forced on (no crash). A
  new app-facing `bv_usb_device_set_mode()`/`bv_usb_device_get_mode()` (`badgevms/usb_device_bridge.c`
  + `include/badgevms/usb_device.h`) replaces the earlier boot-time-only flag — bound to the diamond
  key on `cj_launcher`'s HOME screen (why2025-apps repo, build-verified there too), with a status
  toast + persistent "BadgeLink active" indicator. `badgevms/drivers/usb_msc.c` adds the
  `tanmatsu-usb-msc`-equivalent USB mass-storage mode end-to-end in the API and a small
  `cj_usb_msc` launcher tile. Wired to `FLASH0` first (not `SD0`) — no spare SD card was available
  to safely iterate mount-ownership bugs against, and a bad mount on `FLASH0` is a firmware bug
  fixed by reflashing, not a data-loss risk. `esp_tinyusb`'s own `tinyusb_msc_new_storage_spiflash()`
  owns the FAT mount; `badgevms/drivers/fatfs.c` gets a `fatfs_wrap_mounted_spi()` so BadgeVMS's own
  kernel VFS can share that same mount instead of registering a second, competing one.

  **2026-08-16, hardware-confirmed end-to-end**: the diamond-key toggle initially showed no effect
  on real hardware (`bv_usb_device_set_mode()` returned `true`, state tracked correctly, but the
  bottom port kept showing the C6's `303a:1001` identity, not BadgeLink's `16d0:0f9a`). Root cause:
  GPIO2 needed `gpio_reset_pin()` before `gpio_set_direction()`/`gpio_set_level()` — without it the
  pin never left its default IOMUX state and the mux silently stayed on the C6 regardless of what
  the software did. Same bug class this codebase already hit once on GPIO3 (`board_bringup.c`'s
  vibrator-motor fix). After the fix (plus a genuinely clean rebuild — `build/`, `managed_components/`,
  and `sdkconfig` all removed and regenerated from scratch): `16d0:0f9a "MCS WHY2025 badge"`
  enumerates, and `badgelink.py fs list`/`fs download` both succeed against the badge's real `/SD0`
  with byte-correct file content. See `docs/design/badgelink-usb-port.md`.

  **2026-08-16, MSC activation crash root-caused and fixed**: switching to MSC mode from the
  `cj_usb_msc` launcher tile crashed the calling app task every time (`Task N caused an unhandled
  exception`), then wedged the compositor's input-event queue so the badge never recovered without
  a power cycle. A `mcause`/`mepc`/`g_panic_abort_details` dump added to `__wrap_xt_unhandled_exception`
  (`badgevms/task.c`) traced it to `abort()` inside `dlfree()` (`badgevms/thirdparty/dlmalloc.c`) —
  not memory corruption, but a heap-arena mismatch: BadgeVMS gives every app task its own private
  dlmalloc arena (`badgevms/memory_get_malloc_info.h`'s `get_malloc_state()` resolves dlmalloc's
  "global" state per calling task), and `usb_msc_init()` allocates `esp_tinyusb`'s storage/FAT
  structures once at boot in the *kernel's* arena. `usb_msc_activate()`/`deactivate()`'s unmount
  path frees those same structures — but it used to run straight on the calling app's own task,
  so it freed kernel-arena memory from the app's arena, which `dlfree()`'s own `USAGE_ERROR_ACTION`
  correctly caught and aborted on. Fixed by giving `usb_device.c` its own dedicated kernel-side
  worker task (`create_kernel_task()`, same untagged-task pattern as Zeus/Hades) that the actual
  mode switch always runs on now — `usb_device_switch_to()` just posts a request to it and blocks
  on a semaphore, so the free() always happens in the kernel arena that allocated the memory in the
  first place. Hardware-confirmed: MSC now activates without crashing.

## [1.4.1] - 2026-08-14

### Fixed
- **C6 LoRa never started, showing as the LoRa status LED stuck red** — `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS`
  defaulted to 3, but the C6 registers 4 custom RPC callbacks (echo, display backlight, keyboard
  backlight, then LoRa); the table was full by the time LoRa's own registration ran, so it silently
  failed with `ESP_ERR_NO_MEM` and every P4-side LoRa RPC call got `ESP_ERR_NOT_FOUND` forever.
  Shipped in v1.4.0 undetected. Fixed by raising the limit to 6 in `sdkconfig.defaults`
  (GitHub issue #82).

## [1.4.0] - 2026-08-14

### Added
- **`cj_launcher` now ships baked into the firmware image** as a recovery safety net
  (`flash_storage/skel/BADGEVMS/APPS/`) — a corrupted launcher can't run its own self-update to
  fix itself, so a plain firmware reflash now always restores a working one. The app-repository
  self-update flow stays the normal route for end users; this is a fallback, not a replacement.

### Changed
- **C6: `nicolaielectronics/sx126x` LoRa driver upgraded 0.0.3 → 0.3.0** — picks up automatic
  LDRO activation, automatic image-rejection calibration, and packet-status functions corrected
  against the SX1261/62/68 datasheet. Fixed two breaking API changes this pulled in
  (`sx126x_set_modulation_params_lora()`'s new `automatic_ldro` param, and
  `sx126x_get_packet_status_lora()`'s new float-based signature) and worked around a real bug in
  the new driver's SNR computation (missing sign-extension on the raw byte, corrupting every
  negative SNR reading — routine for real LoRa links). See `lora_protocol_server.c` for the
  fix and its reasoning.

### Fixed
- **Color-value-dependent render glitch (stripe/flicker)** — root-caused to MIPI DSI lane
  signal integrity between the P4 and the display panel, not the PPA/compositor path
  originally suspected. Fixed by lowering `LCD_MIPI_DSI_LANE_BITRATE_MBPS` from 1000 to 700
  in `badgevms/drivers/st7703.c`. See [issue #77](https://github.com/CJvanSoest/why2025-dutchvms/issues/77)
  for the full test log; closes #65.
- **`CONFIG_SCREEN_TYPE_MOUNTAIN` never actually compiled** — a copy-paste bug in
  `badgevms/drivers/st7703.h`'s DPI config macro (two struct fields merged onto one line with
  a missing comma) meant nobody had build-tested that screen variant before. Fixed.
- **CI failed to configure on every checkout path except one specific NAS docker mount** — the
  committed `dependencies.lock` had an absolute filesystem path baked into the
  `espressif/esp_hosted` local-override entry (ESP-IDF's component manager always writes these
  as absolute, never portable). Stopped tracking `dependencies.lock`; every environment now
  regenerates its own.

## [1.3.19] - 2026-08-07

### Fixed
- **`why2025_sponsors` startup entry no longer exists but was still listed
  in `init.toml`**, so the boot supervisor retried launching its missing
  ELF once per second for the entire life of the boot: `start_app()` fails
  before a pid exists, so the `run_once` guard's NVS flag never got
  persisted (only a genuine process exit persists it). The entry is
  removed.

## [1.3.18] - 2026-08-07

### Changed
- Launcher supervision (the "no windows open -> relaunch the launcher"
  watchdog) moved from the compositor's own task loop into the boot
  supervisor (`init.c`), consolidating boot-time responsibility in one
  place.
- The PPA damage-rectangle splitting workaround was split out into
  host-testable pure geometry (`rect_math.c`), covered by a new host test
  (`ctest`).

### Removed
- **BadgeLink UART transport.** Confirmed non-viable on this hardware: the
  P4's native-USB pins aren't routed to any external connector, and
  BadgeLink's COBS framing isn't resilient against console logs sharing
  the same UART. `deploy_protocol.c` remains the actual working deploy
  path `badge_deploy.py` uses.

### Fixed
- **The launcher never started on boot (blank screen).** The UART
  deploy-listener task ran at a priority that outranked `app_main`, so it
  permanently starved the boot sequence before `run_init()` was ever
  reached; lowered to tie with `app_main`'s own priority instead.
- **The C6 radio firmware version always read back empty.** The P4 mirrored
  the C6's `chip_type` wire field as 1 byte instead of the enum's real
  4-byte size, misreading `version_string` 3 bytes early; now pinned with
  compile-time size/offset asserts on both sides.
- **`flash_binary()` write failures during C6 auto-reflash were silently
  ignored** — a real write failure could leave the C6 with stale/partial
  firmware with no indication anything went wrong.
- **A stale-pixel rendering artefact** in the PPA damage-rectangle split:
  one unchecked write could overflow into the rectangle array's own count
  field at capacity, corrupting the iteration that followed.
- Several memory-safety issues found in a follow-up quality review: an OOM
  during boot's `init.toml` parsing could crash via a NULL-pointer write
  instead of returning a clean error; a path-parsing buffer leaked on
  every malformed path; `application_launch()` leaked one struct per app
  launch; a failed task spawn could leak thread state or pin a parent
  task's refcount; and a failed `init.toml` rewrite could leave the boot
  supervisor's config missing entirely instead of keeping the old file.

## [1.3.17] - 2026-08-04

### Changed
- **Pragmatic fix for task #115: disables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.**
  Hardware evidence gathered this session (raw `otadata` partition reads via
  `esptool`, independent of any log capture) showed
  `validate_ota_partition()`'s call to
  `esp_ota_mark_app_valid_cancel_rollback()` never successfully confirms a
  freshly-OTA'd partition -- the new image boots and runs correctly, but the
  bootloader silently reverts it to the previous partition on the next
  reset, and subsequent OTA attempts then fail with
  `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`. The actual root cause was not found
  after exhausting four independent diagnostic channels (UART, SD, NVS
  flag, NVS stack-watermark); deep root-causing is deferred to a future
  session with JTAG access (task #115, see memory
  `why2025_ota_confirm_never_executes_task115.md`). Comparison research
  confirmed Tanmatsu's launcher (Nicolai-Electronics) never enables app
  rollback at all and has never needed an equivalent confirm step --
  disabling it here brings WHY2025 in line with that approach: OTA updates
  now apply directly via `esp_ota_set_boot_partition()` with nothing left
  to fail to confirm, at the cost of losing the auto-revert-on-bad-boot
  safety net rollback provided.

## [1.3.16] - 2026-08-04

### Changed
- **Diagnostic-only release for task #115.** Even `validate_ota_partition()`'s
  own long-standing internal diagnostics and the far-downstream "Entering
  main supervision loop..." print never appear in the serial capture across
  the whole multi-day log history -- not specific to recent changes,
  pointing at something structural rather than a logic bug in this
  session's edits. This codebase has a known prior stack-overflow bug
  (`Launcher_Context`, fixed by making it `static`) with a similar
  silent-corruption-not-immediate-crash signature. Records
  `uxTaskGetStackHighWaterMark()` for the init task at both `cj_dbg115`
  checkpoints to rule stack pressure in or out, independent of print
  reliability.

## [1.3.15] - 2026-08-04

### Changed
- **Diagnostic-only release for task #115.** v1.3.14's SD marker-file test
  came back negative, but that channel is confounded by task #112's
  already-confirmed intermittent SD-write corruption. Switched the
  `run_init()`/`validate_ota_partition()` marker test to NVS (internal
  flash, the same medium `validate_ota_partition()` itself writes to),
  written as a blob so why2025-apps' `bv_nvs_get_blob()` bridge can read it
  back and show it on the launcher's About screen -- no cable needed for
  the next hardware check.

## [1.3.14] - 2026-08-04

### Changed
- **Diagnostic-only release for task #115.** v1.3.13's `esp_rom_printf()`
  diagnostics around `run_init()`/`validate_ota_partition()` still never
  reached the serial capture (hardware-confirmed), ruling out ESP-IDF
  log/stdio buffering as the explanation. Added a second, UART-independent
  differential test: SD marker files (`SD0:run_init_entered.txt`,
  `SD0:run_init_validated.txt`) written at `run_init()` entry and right
  after `validate_ota_partition()` returns, to check via SD access whether
  that code genuinely executes regardless of what is silencing the console.

## [1.3.13] - 2026-08-04

### Changed
- **Diagnostic-only release for task #115.** Raw `otadata` flash reads
  (independent of any serial log capture) confirmed every P4 OTA update this
  session writes and commits successfully but never gets confirmed valid:
  the new partition stays `ESP_OTA_IMG_PENDING_VERIFY` even after 10+
  minutes of fully functional operation, and a subsequent reboot rolls back
  to the previous partition. The existing `ESP_LOGE`/`printf` diagnostics
  around `run_init()`/`validate_ota_partition()` never once appeared in the
  serial log across dozens of boots, despite `run_init()`'s observable
  effects (apps launching) clearly happening. Swapped that diagnostic
  bracket to `esp_rom_printf()` -- a direct ROM-level UART write bypassing
  the ESP-IDF log/stdio buffering layers -- to get visibility into what
  actually happens in there on the next hardware test.

## [1.3.12] - 2026-08-04

### Changed
- **Version-only release, no functional changes.** Cut solely to give a
  badge already running v1.3.11 a newer release to WiFi-OTA update to, so
  why2025-apps' `sync_c6_bundle()` v1.14.4 filename-collision experiment for
  task #112 could be exercised end-to-end (short, dot-free 8.3-native SD
  filenames instead of the real C6 asset names, to test a FAT shortname-
  alias hypothesis).

## [1.3.11] - 2026-08-04

### Changed
- **Version-only release, no functional changes.** Cut solely to give a
  badge already running v1.3.10 a newer release to WiFi-OTA update to, so
  the C6-bundle-sync step (which only runs after a *successful* P4 update,
  not on every "Update Firmware" press) could be exercised end-to-end while
  testing a power-draw mitigation for task #112 in why2025-apps'
  sync_c6_bundle().

## [1.3.10] - 2026-08-04

### Changed
- **Version-only release, no functional changes.** Cut solely to give a
  badge already running v1.3.9 a newer release to WiFi-OTA update to, so
  the download flow itself (Settings -> Update Firmware) could be validated
  end-to-end after v1.3.9's `curl.c` buffer-size fix -- v1.3.9 itself could
  only ever be flashed directly, since there was no newer release yet for
  it to update *to*.

## [1.3.9] - 2026-08-03

### Fixed
- **WiFi-OTA firmware updates could still fail to download even after
  v1.3.1's fix for the same class of bug** -- a live end-to-end test of
  v1.3.8's WiFi-OTA update flow got past version comparison and OTA session
  setup, then failed on GitHub's release-asset redirect with
  `HTTP_CLIENT: Out of buffer`. 4096 bytes (the value v1.3.1 landed on)
  wasn't enough combined header room for the presigned
  `objects.githubusercontent.com` redirect URL plus GitHub's own response
  headers; `curl.c`'s `buffer_size`/`buffer_size_tx` are now 8192.

## [1.3.8] - 2026-08-03

### Fixed
- **A WiFi firmware update needing a C6 radio reflash could still crash and
  roll back after v1.3.7** -- v1.3.7 correctly stopped `flash_slave_c6_if_needed()`
  from touching the C6 while the P4 partition was still unconfirmed, but a
  second, independent problem remained: ESP-Hosted's own SDIO transport
  unconditionally resets the C6 again right before bring-up on every boot,
  with only a ~1.5s budget to talk to it again afterward -- discarding
  whatever settle time the C6 had already been given and re-triggering the
  same too-soon-after-reset race. Switched to ESP-Hosted's own
  `RESET_ONLY_IF_NECESSARY` strategy (`CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`)
  instead of the default reset-on-every-boot, so the host tries talking to
  the (already-settled) C6 first and only resets+retries if that genuinely
  fails -- plus `CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE`
  as a safety net (self-restart instead of a hard abort) if the C6 truly
  isn't responding.
- **A C6 radio-firmware image staged for flashing could get corrupted, causing
  a bad-MD5 write and contributing to the same crash-and-rollback symptom** --
  `flash_binary()` read each chunk of the image from the badge's internal
  flash storage without checking whether the read actually returned the full
  chunk. A short read (the same flaky-read class `application.c`'s manifest
  retry loop already works around) left stale bytes from the previous chunk
  mixed into what got flashed to the C6. Short reads are now retried (seeking
  back to the start of the chunk first) up to 3 times before giving up outright
  -- the C6 is never flashed from a chunk that isn't known to be complete.

## [1.3.7] - 2026-08-03

### Fixed
- **A WiFi firmware update that also needed a C6 radio reflash could crash
  and permanently roll the P4 firmware back**, with no error visible to the
  user beyond a badge that appeared to hang and restart repeatedly. Root
  cause, confirmed on real hardware: `flash_slave_c6_if_needed()` reset the
  C6 co-processor and immediately let `wifi_create()` proceed to
  `start_wifi()`, which tried to bring the SDIO link back up before the
  freshly-reflashed C6 had finished booting its own firmware —
  `esp_wifi_init()` failed, aborting the whole system before
  `validate_ota_partition()` (`run_init()`) ever got a chance to confirm the
  new P4 partition as valid, so ESP-IDF's bootloader rolled it back on the
  next boot. Fixed the same way Tanmatsu's own update flow separates these
  two steps: `flash_slave_c6_if_needed()` now skips the C6 reflash entirely
  while the P4 partition is still unconfirmed, deferring it to the next
  (already-stable) boot, and waits 3 seconds after resetting the C6 before
  returning to give it real time to boot before SDIO bring-up is attempted.

## [1.3.6] - 2026-08-03

### Changed
- **Diagnostic-only release**: v1.3.5's `validate_ota_partition()` fix did
  not resolve the silent-rollback issue — hardware testing (reconnect-
  tolerant serial capture across two consecutive reboots) showed the OTA
  partition still reverting, and `validate_ota_partition()`'s own log line
  never appeared in the boot log at all despite the fix being present in the
  running v1.3.5 image. Added unconditional `ESP_LOGE` diagnostics at
  `run_init()`'s entry, around every branch of `validate_ota_partition()`,
  and around the `run_init()` call site in `why2025_firmware.c`'s `app_main()`,
  so the next hardware test can show conclusively whether/where this code
  path actually executes. No functional change.

## [1.3.5] - 2026-08-03

### Fixed
- **A firmware update could silently revert to a much older version after a
  few reboots** — `validate_ota_partition()` (which cancels ESP-IDF's
  pending-rollback state for the running OTA image) ran as the *last* step
  of `run_init()`, after NVS/config-loading steps with their own, unrelated
  failure modes that return early on failure. If any of those failed on a
  given boot, the OTA partition was never confirmed valid, and ESP-IDF's own
  bootloader silently rolled it back to the previous partition on the next
  boot — observed on real hardware across a run of WiFi-OTA updates.
  `validate_ota_partition()` now runs unconditionally as the very first
  thing `run_init()` does.

## [1.3.4] - 2026-08-03

### Fixed
- **The LED matrix animation stopped moving entirely (v1.3.3 regression)** —
  v1.3.3 pinned `mtx_refresh_task_hw`/`mtx_refresh_task_bb`/`mtx_demo_task`
  and `ws2812_task` all to core 0 to fix a flicker; co-pinning the
  animation-content task (`mtx_demo_task`, priority 2) onto the same core as
  the non-yielding, priority-5 hardware refresh task starved it almost
  completely under strict FreeRTOS priority scheduling, instead of letting
  it run on whichever core the scheduler previously found room on. Only
  `mtx_refresh_task_hw`/`mtx_refresh_task_bb` (the genuinely timing-critical
  ones) stay pinned to core 0 now; `mtx_demo_task` and `ws2812_task` are
  unpinned again.

## [1.3.3] - 2026-08-03

### Fixed
- **The LED matrix animation flickered during a WiFi firmware download** —
  the LED tasks (`mtx_refresh_task_hw`/`mtx_refresh_task_bb`/`mtx_demo_task`
  in `led_matrix_pca9698.c`, `ws2812_task` in `status_led_ws2812.c`) were
  never core-pinned and could land on core 1, the same core app processes
  always run on; a CPU-heavy app (confirmed via a real WiFi-OTA download)
  starved the LED matrix's tight, unyielding refresh loop of time-slices at
  equal priority. All four are now pinned to core 0, keeping the LED
  subsystem off the core app code runs on.

## [1.3.2] - 2026-08-03

### Changed
- **`docs/guides/Flashing.md` no longer claims WiFi-OTA is "proposed but not
  yet built"** — Settings → Update Firmware has been the real update path
  since task #73; the doc is corrected to point there for routine updates
  and reserve the manual `esptool` steps for first flash, recovery, and C6
  radio-firmware changes.

## [1.3.1] - 2026-08-03

### Fixed
- **The WiFi firmware-update flow (Settings → Update Firmware) failed to
  download the new release**, reporting "Download failed, firmware not
  changed" — GitHub's release-asset redirect response (a `Location` header
  pointing at `objects.githubusercontent.com`, plus several other headers)
  exceeded the HTTP client's default 512-byte header buffer, aborting the
  request before it could follow the redirect. The buffer is now sized
  generously enough to handle it. Found via a real hardware test of v1.3.0's
  WiFi-OTA flow (task #86) — this had never been exercised against an actual
  GitHub release before.

## [1.3.0] - 2026-08-03

### Added
- **PAX and LVGL prototype apps** (`sdk_apps/pax_test`, `sdk_apps/lvgl_test`)
  — standalone example apps evaluating both graphics libraries as candidate
  future app-side renderers; both build and now run on real hardware (see
  `docs/pax_lvgl_design_proposal.md`).
- **Releases now also publish ready-to-flash merged flash images**
  (`esp32p4-update.bin`, `esp32p4-factory-erases-storage.bin`,
  `esp32c6-update.bin`) alongside the existing individual pieces, so
  flashing a badge from scratch or updating one over cable is a single
  `esptool write_flash 0x0 <file>` per chip instead of four hand-typed
  offset pairs (GitHub issue #10).
- **Releases now also publish the ESP32-C6 radio co-processor bundle**
  (`bootloader.bin`/`partition-table.bin`/`network_adapter.bin` + `.md5`
  sidecars) alongside `badgevms.bin`, so the launcher's WiFi-OTA flow can
  keep the C6 radio firmware in sync automatically.

### Fixed
- **Display brightness below roughly 80% was barely visible** — the
  backlight PWM duty cycle was linear against a perceptually nonlinear
  panel response; fixed with a gamma-2.2 correction curve so the 0-100%
  brightness range now dims evenly.
- **Apps could permanently fail to (re)launch after being opened a couple
  of times, sometimes surviving a reboot** — `why_sbrk()`'s heap-shrink path
  released far more memory than requested and silently unmapped live
  memory, corrupting later reads and writes; the shrink amount is now
  correctly clamped to the requested delta.
- **The PAX graphics library prototype failed to load at all** — three
  small, independent gaps in the app-side ELF loader's symbol resolution (a
  stray `__thread` variable, unresolvable long-double libgcc helpers, a
  missing `aligned_alloc` kernel export); all three are now resolved and
  PAX runs on real hardware.
- **The LVGL prototype's vendored library contained an accidental full
  self-duplication** of its own `src/libs/` directory, breaking the build
  via wrong-depth includes; the duplicate is removed.

## [1.2.0] - 2026-07-11

### Added
- **Automatic clock sync over WiFi (SNTP)** — the badge previously kept its
  power-on default clock (seconds since boot) unless set manually or over
  BLE; it now syncs to real time on every WiFi connect, best-effort and
  non-blocking.
- **App-facing kernel APIs**: LED-matrix control, the 4 RGBW status LEDs,
  display-brightness plumbing, and a PSRAM kernel-heap usage query, plus a
  `DELETE` command in the UART deploy protocol.
- **BadgeLink is now a proper Kconfig option** (`CJ_BADGEVMS_ENABLE_BADGELINK`)
  instead of a source-level `#define`, with a dedicated (non-blocking) CI
  build job so the experimental transport can't silently bit-rot.

### Changed
- **cj_meshcore's Home screen is now a tile grid** (Nodes/DM/Channel/Advert/
  Tools/Settings/About/Exit) instead of a TAB-cycled tab bar, matching the
  layout of the Tanmatsu MeshCore reference port. Opening a channel now goes
  straight to that channel's chat (read and compose) instead of a separate
  read-only channel list. The radio now configures itself and starts
  listening automatically on app start instead of requiring a manual key
  press first.
- **LoRa packet transmission no longer blocks the shared esp-hosted Rx
  thread** on the C6 co-processor — transmits are now queued to a dedicated
  task and acknowledged asynchronously once they actually complete, and a
  radio-operation mutex now guards each full logical radio operation
  (config apply, transmit, RX-mode re-arm) so they can no longer interleave
  mid-sequence.
- **`badgevms_i2c_bus.c` split into four focused driver files** (the generic
  I2C bus, the PCA9698 LED-matrix driver, the WS2812 status-LED driver, and
  board bring-up) — no behavior change, easier to find and change any one
  of them independently.

### Fixed
- **MeshCore adverts and messages were never recognized by other MeshCore
  nodes**, despite transmitting cleanly and passing every local check. Two
  independent bugs stacked: the kernel's LoRa request buffer was sized only
  for small control messages and silently rejected any transmit payload
  over 64 bytes (surfacing to the user only as an unexplained "radio busy"
  error), and — once that was fixed — the C6 co-processor firmware radiated
  an internal length-prefix byte as the first byte of the actual
  over-the-air payload, shifting every packet ever sent by one byte. Fixed
  and confirmed against an independent MeshCore receiver.
- **Demo/placeholder flash apps removed from the catalog** (Snake, the old
  system-settings app, `hello`, `sdl_test`, `sdl2_test`, and the WHY2025
  name-badge/sponsors apps) — none were part of this fork's actual feature
  set.
- **Keyboard driver (tca8418) silently corrupted multi-key input** — a
  buffer-offset bug meant that when two or more key events were pending in
  one poll, only the last one actually landed in the caller's buffer (all
  at offset 0), while the return value claimed every event was written; an
  out-of-bounds read on the vendor's "no key" sentinel value is also fixed.
- **`application_destroy()` left a ghost, permanently-unreusable launcher
  entry** — it only deleted the app's install directory, never its manifest,
  so a removed app kept showing up in the launcher and its unique
  identifier could never be reused without a manual SD-card fix.
- Kernel mutex-take failures (which should be unreachable in normal
  operation) now go through a diagnosable panic path instead of a bare,
  unexplained `abort()`.
- `hades()` no longer force-kills orphaned child tasks while holding the
  process table lock, shrinking the window in which that could leave an
  unrelated kernel lock stuck.

### Security
- **Path traversal in the UART deploy protocol** — a crafted VMS path with a
  `.` or `..` segment could make PUT/GET/LIST/DELETE (including the
  recursive delete) escape the intended SD/flash sandbox. Deploy protocol
  paths are now validated before use, and `badge_deploy.py`'s `delete`
  command now also rejects such paths client-side and requires confirmation.

### Removed
- **`ota_wifi_update` and `why2025_ota`** (the WHY2025 handout on-device
  updater and its intended replacement) moved to `Archive/` — see
  `Archive/README.md`. Firmware updates are published via GitHub Releases
  and flashed manually (`docs/guides/Flashing.md`); an on-device updater was
  never wired up on this fork.
