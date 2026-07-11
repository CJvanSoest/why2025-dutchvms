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
