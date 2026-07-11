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

### Added
- **Automatic clock sync over WiFi (SNTP)** — the badge previously kept its
  power-on default clock (seconds since boot) unless set manually or over
  BLE; it now syncs to real time on every WiFi connect, best-effort and
  non-blocking.
- **App-facing kernel APIs**: LED-matrix control, the 4 RGBW status LEDs,
  display-brightness plumbing, and a PSRAM kernel-heap usage query, plus a
  `DELETE` command in the UART deploy protocol.

### Changed
- **cj_meshcore's Home screen is now a tile grid** (Nodes/DM/Channel/Advert/
  Tools/Settings/About/Exit) instead of a TAB-cycled tab bar, matching the
  layout of the Tanmatsu MeshCore reference port. Opening a channel now goes
  straight to that channel's chat (read and compose) instead of a separate
  read-only channel list. The radio now configures itself and starts
  listening automatically on app start instead of requiring a manual key
  press first.

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

### Removed
- **`ota_wifi_update` and `why2025_ota`** (the WHY2025 handout on-device
  updater and its intended replacement) moved to `Archive/` — see
  `Archive/README.md`. Firmware updates are published via GitHub Releases
  and flashed manually (`docs/guides/Flashing.md`); an on-device updater was
  never wired up on this fork.
