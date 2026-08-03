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

## [1.3.6] - 2026-08-03

### Changed
- **Diagnostic-only release**: v1.3.5's `validate_ota_partition()` fix did
  not resolve the silent-rollback issue — hardware testing (reconnect-
  tolerant serial capture across two consecutive reboots) showed the OTA
  partition still reverting, and `validate_ota_partition()`'s own log line
  never appeared in the boot log at all despite the fix being present in the
  running v1.3.5 image. Added unconditional, unmissable `ESP_LOGE` diagnostics
  at `run_init()`'s entry, around every branch of `validate_ota_partition()`,
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
