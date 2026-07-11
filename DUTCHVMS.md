# DutchVMS — CJ's fork of BadgeVMS

This is a fork of the [WHY2025 BadgeVMS firmware](https://gitlab.com/why2025/team-badge/firmware)
with a custom C6 co-processor firmware (WiFi/BT + a MeshCore LoRa radio
protocol server on the same image), a UART deploy protocol for pushing apps
without opening the badge, and DutchVMS launcher branding.

## Repos

This repo (`why2025-dutchvms`) is firmware only — the kernel, drivers, and
the C6 co-processor firmware. The actual user-facing apps (MeshCore,
storage viewer, WiFi analyzer, launcher, etc.) live in a separate repo,
kept apart so this fork stays a clean diff against upstream:

- **Firmware** — this repo, GitHub: [why2025-dutchvms](https://github.com/CJvanSoest/why2025-dutchvms) (public, GPL-3.0-or-later)
- **Apps** — [why2025-apps](https://github.com/CJvanSoest/why2025-apps) (private, MIT) — source for every `cj_*` app plus `tools/badge_deploy.py`
- **App store** — [why2025-app-repository](https://github.com/CJvanSoest/why2025-app-repository) (public) — the distribution index the launcher's APP REPO tile browses/installs from

GitHub is primary for both code and docs on all three. A separate NAS Gitea
instance is used only for devlogs/private notes, never for the code itself.

## Changes vs upstream

### 1. Custom C6 co-processor firmware (`connectivity_esp_hosted/slave/`)
The C6 runs this fork's own build, not stock ESP-Hosted: it serves the
standard ESP-Hosted WiFi/BT slave protocol *and* a custom LoRa protocol
server (`connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol_server.c`,
driving an external SX126x over SPI) on the same firmware image. WiFi and
MeshCore/LoRa are not mutually exclusive here — see the comment on
`CJ_BADGEVMS_ENABLE_WIFI` in `badgevms/drivers/wifi.c`. The P4 talks to it
via `badgevms/drivers/lora_proto_client.c`, exposed to apps as
`badgevms/include/badgevms/lora.h`.

The C6 image is staged on the SD card at `APPS:[why2025_firmware_ota_c6]`
and auto-flashed at boot by `flash_slave_c6_if_needed()`
(`badgevms/drivers/esp-serial-flasher/`) whenever its MD5 doesn't match
what's already on the chip. It can also be flashed directly over its own
USB port — see [docs/guides/Flashing.md](docs/guides/Flashing.md).

### 2. UART-deploy protocol (`badgevms/deploy_protocol.{c,h}`)
Kernel-task listener on UART0 RX implementing a CRC-framed binary protocol.
Lets apps be written to the SD card over the side USB-C (CH340) without
opening the badge.

- Wire format: `[MAGIC: DE AD BE EF][CMD:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]`
- CRC-16/X.25 (`esp_rom_crc16_le`-compatible)
- Commands: `PUT` (write file), `GET` (read file), `LIST` (directory), `DELETE` (recursive remove), `PING` (version probe)
- mkdir-p auto-create for parent directories
- Host-side client: `why2025-apps/tools/badge_deploy.py`

Init hook in `why2025_firmware.c` (after device-init, before `run_init`).

### 3. Automatic clock sync (`badgevms/drivers/wifi.c`)
The badge has no RTC battery; on every WiFi connect it kicks a best-effort,
non-blocking SNTP sync (`esp_netif_sntp_*`) instead of relying on a manual
clock-set or leaving the clock at "seconds since boot". This matters beyond
cosmetics: anything that timestamps outgoing data with `time(NULL)` (e.g.
MeshCore adverts) needs a plausible clock or peers silently reject it as
stale.

### 4. App-facing kernel APIs beyond stock BadgeVMS
LED-matrix control, the 4 RGBW status LEDs, display-brightness plumbing,
and a PSRAM kernel-heap usage query — see `badgevms/include/badgevms/`
(`led_matrix.h`, `status_led.h`, `display.h`, `meminfo.h`) and
`badgevms/symbols.yml` for how they're exposed to apps.

### 5. Splash + branding
The launcher app (in `why2025-apps/apps/cj_launcher`) shows a DutchVMS
splash with an animated wireframe windmill before the home screen. Not a
firmware change, but the visible identity of this fork.

## Build

Standard ESP-IDF 5.5 workflow — see [docs/guides/Flashing.md](docs/guides/Flashing.md)
for the full build + flash + update procedure:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build
idf.py sdk                    # one-time — produces sdk_dist/ for app builds
```

## Remotes

- `github` → [why2025-dutchvms](https://github.com/CJvanSoest/why2025-dutchvms) (GitHub, public, primary)
- `origin` → NAS-hosted Gitea mirror (devlogs/private notes only — not the primary remote)
- `upstream` → WHY-team GitLab (`https://gitlab.com/why2025/team-badge/firmware.git`)

Merging upstream changes:
```bash
git fetch upstream
git rebase upstream/main         # or merge -- depends on the situation
git push github
```

## Releases

Tag-driven: pushing a `vX.Y.Z` tag triggers `.github/workflows/release.yml`,
which builds firmware and publishes a GitHub Release with `badgevms.bin`
attached. There is no on-device updater consuming this automatically —
flashing a release onto a badge is a manual step. See
[docs/guides/Releases.md](docs/guides/Releases.md) for the full process and
[docs/CHANGELOG.md](docs/CHANGELOG.md) for what changed in each release.

## Archive

Code that shipped with the original WHY2025 handout but is no longer built
or relevant to this fork (an old on-device OTA updater, orphaned example
apps) lives in [Archive/](Archive) instead of being deleted — see
[Archive/README.md](Archive/README.md).
