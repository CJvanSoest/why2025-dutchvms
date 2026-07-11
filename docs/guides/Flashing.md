# Flashing

How to get DutchVMS onto a badge for the first time, and how to update an
already-flashed badge to a newer release. There is no on-device updater —
every flash here is a manual `esptool` step (see `docs/CHANGELOG.md`'s
"Removed" entry for why).

The badge has two USB-C ports that talk to two different chips:

| Port | Chip | Typical device name |
|---|---|---|
| Side | ESP32-P4 (app processor) | `/dev/ttyUSB0` (Linux, CH340 UART bridge) / `/dev/cu.wchusbserial...` (macOS) |
| Bottom | ESP32-C6 (radio co-processor) | `/dev/ttyACM0` (Linux, native USB-JTAG/Serial) / `/dev/cu.usbmodem...` (macOS) |

Flashing the wrong port at the wrong chip's binaries will not work — the
esptool `--chip` flag will simply fail to talk to the target, it will not
brick anything.

## First-time flash (blank badge, or recovering from a bad flash)

You need both the P4 firmware (`badgevms.bin` + friends) and the C6
co-processor firmware (`network_adapter.bin` + friends). Build both first:

```bash
. $IDF_PATH/export.sh          # or: source ~/esp/esp-idf/export.sh
idf.py build                   # builds badgevms.bin (P4) AND drives the
                                # connectivity_esp_hosted sub-build (C6)
```

This produces `build/badgevms.bin` (P4) and
`build/connectivity_esp_hosted/network_adapter.bin` (C6) — the exact paths
are printed at the end of the build.

**Flash the P4** (side port):

```bash
python -m esptool --chip esp32p4 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyUSB0 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xd000  build/ota_data_initial.bin \
  0x10000 build/badgevms.bin
```

**Flash the C6** (bottom port — switch the USB cable):

```bash
python -m esptool --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyACM0 \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0     build/connectivity_esp_hosted/bootloader/bootloader.bin \
  0x8000  build/connectivity_esp_hosted/partition_table/partition-table.bin \
  0xd000  build/connectivity_esp_hosted/ota_data_initial.bin \
  0x10000 build/connectivity_esp_hosted/network_adapter.bin
```

Neither command touches the `storage` partition (the SD/flash filesystem
holding installed apps) — see below.

### Storage (apps + user data)

The `storage` partition (`storage.bin`, offset `0x410000`) holds the flash
catalog and any flash-installed apps. It is deliberately **not** flashed on
a normal update — re-flashing it wipes user data (installed apps, WiFi
credentials, MeshCore identity/contacts, etc.) on every reboot. Only flash
it for a genuinely blank badge, or when you intend to reset storage to
factory defaults:

```bash
python -m esptool --chip esp32p4 -b 460800 --port /dev/ttyUSB0 \
  write_flash 0x410000 build/storage.bin
```

## Updating an existing badge

1. Download `badgevms.bin` from the release you want (GitHub Releases page,
   or build it yourself per above).
2. Flash it with the same P4 command as above, using the downloaded
   `badgevms.bin` in place of `build/badgevms.bin`. The bootloader,
   partition table, and `ota_data_initial.bin` rarely change between
   releases, but flashing them again is harmless and safest by default.
3. Only re-flash the C6 (`network_adapter.bin`) if the release notes say the
   LoRa/radio wire protocol changed — see `docs/CHANGELOG.md`. P4 and C6
   firmware must be a matching pair for anything that touches that
   protocol (`badgevms/drivers/lora_proto_client.c` /
   `connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol_server.c`);
   flashing a new P4 against a stale C6 (or vice versa) can silently break
   LoRa without any error.
4. Do **not** flash `storage.bin` — that would erase installed apps and
   saved settings (see above).

This is the only supported path today. An SD-card-based installer and a real
OTA-over-WiFi path (no esptool, no computer) are proposed but not yet built —
see [docs/design/SD-and-OTA-Updates.md](../design/SD-and-OTA-Updates.md).

### Installing/updating individual apps

Apps (the `cj_*` apps in the separate
[why2025-apps](https://github.com/CJvanSoest/why2025-apps) repo, plus the
[why2025-app-repository](https://github.com/CJvanSoest/why2025-app-repository)
store) don't need a firmware reflash at all — they're pushed straight to the
SD card over the same P4 UART using the deploy protocol
(`badgevms/deploy_protocol.c`, client: `why2025-apps/tools/badge_deploy.py`):

```bash
python3 tools/badge_deploy.py --port /dev/ttyUSB0 \
  put path/to/app.elf "SD0:[BADGEVMS.APPS.app_name]app_name.elf"
```

Files larger than a few hundred KB (like `network_adapter.bin`) will hit the
protocol's request-buffer limit — those go through the C6 flash steps above
instead, not `badge_deploy.py put`.

## Building the app SDK (only needed to build apps, not firmware)

```bash
idf.py sdk
```

Produces `sdk_dist/` (headers + libs) that the separate apps repo's
`build.sh` compiles against. See that repo's own docs for the app build
flow.

## See also

- [Releases.md](Releases.md) — how a release is cut and versioned
- [DUTCHVMS.md](../../DUTCHVMS.md) — what this fork changes vs upstream BadgeVMS
- [docs/design/SD-and-OTA-Updates.md](../design/SD-and-OTA-Updates.md) — proposed SD-card install + OTA path
- [README.md](../../README.md) — building from source, SDK, example apps
