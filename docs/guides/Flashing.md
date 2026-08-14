# Flashing

How to get DutchVMS onto a badge for the first time, and how to recover a
badge that can't already run an OTA-capable image. If the badge is already
running DutchVMS and has WiFi, Launcher → Settings → **Update Firmware** is
the easier path for routine updates (see
[why2025-apps/docs/UPDATE_GUIDE.md](https://github.com/CJvanSoest/why2025-apps/blob/main/docs/UPDATE_GUIDE.md))
— everything below is the manual `esptool` path this repo still needs for a
genuinely blank/bricked badge, or for a C6 radio-firmware update (WiFi-OTA
only updates the P4 automatically; the C6 bundle still needs the badge to
already be able to reach it, see step 3 under "Updating an existing badge").

The badge has two USB-C ports that talk to two different chips:

| Port | Chip | Typical device name |
|---|---|---|
| Side | ESP32-P4 (app processor) | `/dev/ttyUSB0` (Linux, CH340 UART bridge) / `/dev/cu.wchusbserial...` (macOS) |
| Bottom | ESP32-C6 (radio co-processor) | `/dev/ttyACM0` (Linux, native USB-JTAG/Serial) / `/dev/cu.usbmodem...` (macOS) |

Flashing the wrong port at the wrong chip's binaries will not work — the
esptool `--chip` flag will simply fail to talk to the target, it will not
brick anything.

> **Double-check `--chip` and the release asset name before running any
> `write_flash` command.** Every release publishes an `esp32p4-*.bin` and an
> `esp32c6-*.bin` — both are flashed the same way (`write_flash 0x0
> <file>.bin`), so a copy-paste mistake that swaps them fails loudly
> (`--chip` rejects the wrong image) rather than bricking anything, but it's
> an easy mix-up. Separately, `esp32p4-update.bin` and
> `esp32p4-factory-erases-storage.bin` look almost identical in a release's
> asset list — only the factory image wipes installed apps/WiFi/MeshCore
> identity (see "Storage" below). Read the filename, not just the chip.

## First-time flash (blank badge, or recovering from a bad flash)

Each GitHub Release publishes one merged image per chip, ready to flash at
offset `0x0` — no need to build from source or juggle multiple offsets:

**Flash the P4** (side port):

```bash
python -m esptool --chip esp32p4 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyUSB0 \
  write_flash 0x0 esp32p4-factory-erases-storage.bin
```

**Flash the C6** (bottom port — switch the USB cable):

```bash
python -m esptool --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyACM0 \
  write_flash 0x0 esp32c6-update.bin
```

That's it — both commands include everything (bootloader, partition table,
OTA metadata, app image), and the P4 one also includes a blank `storage`
partition, so this is the only path that needs the "factory" image (see
"Storage" below for why that matters).

### Building from source instead

If you're building the firmware yourself rather than downloading a release,
build first and use the same multi-offset commands the release workflow
merges together — this is exactly what's inside `esp32p4-factory-erases-storage.bin`/
`esp32c6-update.bin`, just as separate files instead of one:

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
holding installed apps) — see below. If you're building from source and want
a blank badge fully set up in one go, flash `build/storage.bin` at `0x410000`
as well (same offset the release's factory image uses internally).

### Storage (apps + user data)

The `storage` partition (`storage.bin`, offset `0x410000`) holds the flash
catalog and any flash-installed apps. It is deliberately **not** included in
`esp32p4-update.bin` or a from-source update flash — re-flashing it wipes
user data (installed apps, WiFi credentials, MeshCore identity/contacts,
etc.) on every reboot. Only flash it (or use the `-factory-erases-storage`
image) for a genuinely blank badge, or when you intend to reset storage to
factory defaults:

```bash
python -m esptool --chip esp32p4 -b 460800 --port /dev/ttyUSB0 \
  write_flash 0x410000 build/storage.bin
```

## Getting the DutchVMS launcher onto a fresh badge

**As of v1.4.0, the factory `storage.bin` already bakes in a working
`cj_launcher`** (`flash_storage/skel/BADGEVMS/APPS/badgevms_launcher/`,
alongside the small diagnostic apps `cj_i2c_scan`/`cj_lora_info` and the C6
OTA bundle) — added as a recovery safety net, since a corrupted launcher
can't run its own self-update to fix itself (see `docs/CHANGELOG.md`
`[1.4.0]`). A badge freshly flashed with `esp32p4-factory-erases-storage.bin`
(or `build/storage.bin` from source) now boots straight into DutchVMS with no
extra step needed.

The version baked in is whatever was committed at release time, so it can be
older than what's in the store — after first boot, Launcher → Settings →
Update Firmware or the APP REPO tile picks up the current published version.
If you're building from source and the skel's `badgevms_launcher/` is
missing or you want a newer build baked in, regenerate it with
`why2025-apps/apps/install-to-firmware-skel.sh` before `idf.py build` (see
that script's own comments — it reads each app's `unique_identifier` from
its manifest, so the target directory name won't match the source folder
name for this app).

**Recovering an already-provisioned badge with a corrupted launcher:**
using the factory image/`storage.bin` to restore the baked-in launcher also
wipes every other installed app, WiFi credentials, and MeshCore identity —
it's provisioning a blank badge, not a surgical launcher-only fix. For a
badge that already has real data on it, either pull the SD card and copy a
known-good `cj_launcher.elf` + manifest onto it directly with a card reader
(no reflash, nothing else touched — see `docs/KNOWN_ISSUES.md` #13 for a
worked example), or deploy a fresh build over UART instead (see below) if
the deploy listener is still reachable.

Deploying `cj_launcher` manually over UART (below) is still the right move
if you're running an older release whose skel predates v1.4.0, or if you
just want the very latest build without waiting for the next firmware
release. **You do not need to build it yourself** — a current, ready-to-use
build is published in the
[why2025-app-repository](https://github.com/CJvanSoest/why2025-app-repository)
(the same store `cj_launcher`'s own APP REPO tile pulls from once it's
running), so grab the two files straight from there:

```bash
curl -LO https://raw.githubusercontent.com/CJvanSoest/why2025-app-repository/main/badgevms_launcher/cj_launcher.elf
curl -LO https://raw.githubusercontent.com/CJvanSoest/why2025-app-repository/main/badgevms_launcher/badgevms_launcher.json

python3 tools/badge_deploy.py --port /dev/ttyUSB0 \
  put cj_launcher.elf 'SD0:[BADGEVMS.APPS.badgevms_launcher]cj_launcher.elf'
python3 tools/badge_deploy.py --port /dev/ttyUSB0 \
  put badgevms_launcher.json 'SD0:[BADGEVMS.APPS]badgevms_launcher.json'
```

(`tools/badge_deploy.py` itself still comes from a
[why2025-apps](https://github.com/CJvanSoest/why2025-apps) checkout — only
the two files being deployed no longer need building from source. If you'd
rather build `cj_launcher` yourself instead of using the published build,
that's still `./apps/build.sh cj_launcher`, see that repo's README.)

No cable or command line at all also works: the SD card is a normal FAT
filesystem, so with the badge powered off you can pull the microSD card,
put it in a card reader, and copy the same two downloaded files directly
into `BADGEVMS/APPS/badgevms_launcher/cj_launcher.elf` (create that folder)
and `BADGEVMS/APPS/badgevms_launcher.json` (next to it, **not** inside the
folder — the launcher only reads the sibling copy). Put the card back in
and power the badge on.

`cj_launcher`'s manifest intentionally sets `"unique_identifier":
"badgevms_launcher"` — that's not a leftover from forking the reference
launcher, it's what makes the boot supervisor (`application_launch
("badgevms_launcher")` in `badgevms/init.c`) pick it up as *the* launcher.
Reboot the badge after both `put`s; DutchVMS should now boot straight into
it. Every other app (`cj_storage`, `cj_files`, MeshCore, etc.) can then be
installed the same way, or via the launcher's own APP REPO / WHY APPS tiles
once it's running.

## Updating an existing badge

Download the merged update image for the chip(s) you need to update and
flash it at offset `0x0` — same bytes as the individual-file commands below,
just one file:

```bash
# P4 (side port) -- always safe, never touches storage
python -m esptool --chip esp32p4 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyUSB0 \
  write_flash 0x0 esp32p4-update.bin

# C6 (bottom port) -- only if the release notes say the LoRa/radio wire
# protocol changed, see step 3 below
python -m esptool --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset --port /dev/ttyACM0 \
  write_flash 0x0 esp32c6-update.bin
```

Equivalent, if you'd rather flash the individual pieces (e.g. to sanity-check
exactly what's going where, or because you built from source and don't have
a merged image):

1. Download `badgevms.bin` from the release you want (GitHub Releases page,
   or build it yourself per above).
2. Flash it with the same P4 multi-offset command as the "Building from
   source" section above, using the downloaded `badgevms.bin` in place of
   `build/badgevms.bin`. The bootloader, partition table, and
   `ota_data_initial.bin` rarely change between releases, but flashing them
   again is harmless and safest by default.
3. Only re-flash the C6 (`network_adapter.bin`, or `esp32c6-update.bin`) if
   the release notes say the LoRa/radio wire protocol changed — see
   `docs/CHANGELOG.md`. P4 and C6 firmware must be a matching pair for
   anything that touches that protocol
   (`badgevms/drivers/lora_proto_client.c` /
   `connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol_server.c`);
   flashing a new P4 against a stale C6 (or vice versa) can silently break
   LoRa without any error.
4. Do **not** flash `storage.bin`, and do **not** use the
   `-factory-erases-storage` image for a normal update — either would erase
   installed apps and saved settings (see "Storage" above).

This manual esptool path is what's needed for a blank/bricked badge or a C6
radio-firmware change. A real OTA-over-WiFi path for routine P4 updates now
exists (Launcher → Settings → Update Firmware, which also syncs the C6
bundle) — see [docs/design/SD-and-OTA-Updates.md](../design/SD-and-OTA-Updates.md)
for how that's implemented. An SD-card-based installer (no esptool, no WiFi,
no computer at all) is still only a proposal, not built.

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
- [docs/design/merged-flash-images-proposal.md](../design/merged-flash-images-proposal.md) — design rationale for the merged images this guide uses
- [DUTCHVMS.md](../../DUTCHVMS.md) — what this fork changes vs upstream BadgeVMS
- [docs/design/SD-and-OTA-Updates.md](../design/SD-and-OTA-Updates.md) — proposed SD-card install + OTA path
- [README.md](../../README.md) — building from source, SDK, example apps
