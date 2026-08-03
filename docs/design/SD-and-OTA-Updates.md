# SD-card firmware install + OTA updates — design proposal

Design document for [why2025-dutchvms#6](https://github.com/CJvanSoest/why2025-dutchvms/issues/6).

> **Status (2026-08-03): §2 (WiFi OTA) and §3 (C6 bundle sync) are
> implemented and merged** — see the status note at the top of each section
> below for exactly what shipped and where. **§1 (SD-card install) is the
> only part of this proposal that's still just a plan, not code.** This
> document originally described all three as "no code yet"; that's no longer
> true for two of the three, kept here only as the historical proposal text
> for what became §2/§3.

## Problem

The only documented update path today ([Flashing.md](../guides/Flashing.md)) is
esptool on a computer, with exact partition offsets typed by hand. That's fine
for a developer but not for "I got a badge, I want the latest firmware" —
DutchVMS needs (1) a way to install new firmware from an SD card, no computer
required, and (2) a real OTA path for people who do have WiFi.

## What already exists (don't re-invent this)

Both pieces are further along than they look:

- **`badgevms/ota.c`/`badgevms/include/badgevms/ota.h`** already wraps the
  ESP-IDF OTA partition APIs end to end: `ota_session_open()` picks the
  inactive `ota_0`/`ota_1` app partition, `ota_write()` streams bytes into it,
  `ota_session_commit()` finalizes and flips the boot partition, and
  `validate_ota_partition()` (called from `init.c`) confirms the new image
  booted successfully and cancels the ESP-IDF rollback safety net — if a new
  image never reaches that call (crash loop), the bootloader automatically
  falls back to the previous partition on its own. This is a generic,
  already-shipped, already-safe **write sink** for a new P4 firmware image.
  Nothing currently calls it.
- **`Archive/sdk_apps/why2025_ota/ota_update.c`** (moved to `Archive/` because
  it was never wired up, not because it's broken) already implements a
  complete GitHub-Releases OTA flow for the P4: `update_firmware()` resolves
  `releases/latest` on `CJvanSoest/why2025-dutchvms`, streams the
  `badgevms.bin` asset straight into an `ota_session_*` through `curl.c`, and
  `check_for_firmware_updates()` compares versions with `ota_get_running_version()`.
  This is a working reference implementation for the WiFi path, not a
  from-scratch task.
- **`flash_slave_c6_if_needed()`** (`drivers/esp-serial-flasher/slave_c6_flasher.c`,
  called from `wifi_create()` on every boot) already does exactly the
  "install from SD, MD5-gated, zero user action" flow this issue asks for —
  just for the C6 co-processor, not the P4. It reads
  `APPS:[why2025_firmware_ota_c6]{bootloader,partition-table,network_adapter}.bin`
  + `.md5` sidecars from the `storage` FAT partition and reflashes the C6 over
  UART1 only where the MD5 differs. This is the proven pattern to mirror for
  the P4's own binary.
- **[`docs/radio_c6_updater_proposal.md`](https://github.com/CJvanSoest/why2025-apps/blob/main/docs/radio_c6_updater_proposal.md)**
  (why2025-apps repo) already worked out a real gap in the existing design:
  `ota_session_*` only writes the `ota_0`/`ota_1` **app** partition. The C6
  binaries under `why2025_firmware_ota_c6/` live in the separate `storage`
  FAT partition, which a P4 app-OTA never touches — so a P4-only OTA update
  can silently leave the C6 firmware stale. Its recommendation (fetch a
  second small bundle and write it directly into the already-mounted
  `storage` filesystem, no new OTA partition) is adopted below.

## Proposal

Two independent trigger paths, one shared write path, and the same C6-bundle
fix for both.

### 1. SD-card install (no WiFi, no computer)

Mirror `flash_slave_c6_if_needed()`'s exact convention for the P4's own
binary: stage `badgevms.bin` + `badgevms.bin.md5` at
`APPS:[why2025_firmware_ota_p4]` on the SD card (same `<name>.bin` +
`<name>.bin.md5` sidecar shape `get_why2025_binaries()` already reads for the
C6, so a release ZIP can ship one flat "drop these files on the SD card"
folder covering both chips).

At boot, alongside the existing `flash_slave_c6_if_needed()` call: check
whether `badgevms.bin.md5` on SD differs from the running partition's stored
MD5 (`esp_ota_get_partition_description()`/`esp_app_desc_t` doesn't carry an
MD5 today — add one, or compare against `ota_get_running_version()` plus a
version string embedded in the staged bundle, whichever is simpler once
implemented). On mismatch: `ota_session_open()`, stream the SD file through
`ota_write()` in chunks (same shape as `why2025_ota`'s `firmware_cb()`, just
reading `why_fopen()` instead of a curl callback), `ota_session_commit()`,
reboot. No WiFi, no BLE, no computer — copy two files to the SD card, power
on.

Guard against a reflash loop the same way the version-compare already
implies: only reflash on an actual MD5 change, and only from the *inactive*
partition (which `ota_session_open()` already guarantees) so a bad image
can't overwrite the last known-good one before it's even validated.

### 2. OTA over WiFi (revive `why2025_ota`, don't rewrite it)

> **IMPLEMENTED** (task #73). Not un-archived after all — reimplemented
> directly in `why2025-apps/apps/cj_launcher/main.c` as `do_firmware_update()`,
> wired to Settings → **Update Firmware**. It fetches
> `releases/latest` from `CJvanSoest/why2025-dutchvms`, compares the release
> tag against `ota_get_running_version()` with `strverscmp()` (skips with
> "Already up to date" if the running version is >= the release tag), and on
> a real update streams `badgevms.bin` straight into an `ota_session_*` via
> curl, same shape as originally proposed below. **Not yet hardware-tested**
> (task #86) — build-verified only so far. **Known risk to watch for during
> that test**: the running version string is whatever `git describe --tags`
> produced at the last build (e.g. `v1.2.0-45-g1a2b3c4` for a dev build off a
> branch, not a clean tag) — `strverscmp()` comparing that against a plain
> release tag like `1.3.0` may not behave as a human would expect (leading
> non-digit vs. digit characters), so "Already up to date" firing incorrectly
> on a dev-flashed badge is a real possibility worth confirming, not assuming
> away.
>
> The text below is kept as the original proposal for reference.

Un-archive and adapt `Archive/sdk_apps/why2025_ota/ota_update.c`:

- `update_firmware()`/`check_for_firmware_updates()` need no logic changes —
  audit for staleness against the current `badgevms/ota.h` signature and the
  now-corrected launcher Settings-tile text (see below) before re-enabling.
- Wire it into `cj_launcher`'s Settings tile, which per the existing proposal
  doc currently shows placeholder text claiming a firmware update also
  updates the C6 — false today, true once step 3 below lands. Fix the text
  now regardless of when the rest ships (`docs/PROJECT_SETUP.md`'s OTA row
  has the same stale claim).
- Add the same MD5-based "is there anything to do" check before opening an
  OTA session, so pressing "check for updates" on an already-current badge
  is a no-op instead of a needless flash-partition write.

### 3. Keep the C6 in sync on either path

> **IMPLEMENTED** (task #85). `release.yml` now generates `.md5` sidecars
> for `bootloader.bin`/`partition-table.bin`/`network_adapter.bin` and
> attaches all 6 files (3 binaries + 3 sidecars) as GitHub Release assets.
> `cj_launcher`'s `sync_c6_bundle()` downloads them into
> `SD0:[BADGEVMS.APPS.why2025_firmware_ota_c6]` right after a successful P4
> OTA commit, exactly as proposed below — no C6/kernel changes were needed.
> It degrades gracefully against an older release missing these assets (each
> missing file is skipped and reported, the P4 update still lands). **Not
> yet exercised via an actual tagged release**: the current `latest` release
> (`v1.2.0`, 2026-07-11) predates this workflow change and only has
> `badgevms.bin` attached — the C6-sync path has only been read/build-verified
> in code, never actually run against a real release's assets. Cutting a new
> release tag is what will exercise this path for the first time.

Per `radio_c6_updater_proposal.md`'s recommendation:

- CI (`release.yml`) attaches the three existing C6 build artifacts
  (`bootloader.bin`, `partition-table.bin`, `network_adapter.bin`) + their
  `.md5` sidecars as extra GitHub Release assets — `storage_staging_add_connectivity`
  already produces them, this is just also uploading them.
- New `update_c6_bundle()` in `ota_update.c` (WiFi path) and the SD-card
  installer (path 1 above) both write these 4 files straight to
  `APPS:[why2025_firmware_ota_c6]<name>` — a plain file write into the
  already-mounted `storage` FAT filesystem, not an OTA-partition operation,
  exactly where `get_why2025_binaries()` already reads from.
- `flash_slave_c6_if_needed()` itself needs no changes: it already reflashes
  the C6 on its own the next boot after the MD5s change underneath it.

### Why not a second OTA partition pair for `storage`

Already ruled out by the existing proposal: `partitions.csv` would need a
second slot pair sized for the whole `storage` filesystem (currently ~11.6 MB,
holding user-installed apps and settings too, not just the 3 C6 files) for a
dual-bank swap of 3 small binaries. A plain file write to the one `storage`
partition, gated by the same MD5 check already in place, is proportionate.

## Open questions to resolve before implementing

Only §1 (SD-card install) remains unbuilt, so these open questions now only
apply to that piece — §2/§3's real equivalent questions were answered by how
they actually shipped (WiFi path version-gates on `strverscmp()` against the
release tag, no separate MD5-of-running-partition tracking was needed; C6
sync piggybacks on the existing GitHub Release rather than a separate ZIP).

- Exact SD-card MD5/version staleness check for the P4 binary (see §1) —
  needs a concrete mechanism, not just "compare something."
  `ota_get_running_version()` returns `esp_app_desc_t.version` (from
  `PROJECT_VER`/`git describe --tags`); the staged bundle would need the same
  string alongside it, or its own MD5 sidecar compared against a stored MD5
  of the currently-running partition (which isn't tracked anywhere today and
  would need adding).
- Whether the SD-card installer runs unconditionally at every boot (like the
  C6 one does) or only on a user action (Settings-tile "check SD card"
  button) — unconditional is more "just works" for a non-technical user, but
  means every boot pays an SD read + MD5 compare.
- Packaging: does a release ZIP ship one `why2025_firmware_ota_p4/` +
  `why2025_firmware_ota_c6/` folder pair to drop directly into `SD0:[APPS]`,
  documented in [Flashing.md](../guides/Flashing.md)?

## See also

- [Flashing.md](../guides/Flashing.md) — current (esptool-only) install/update instructions
- [Releases.md](../guides/Releases.md) — what a release publishes today
- `Archive/sdk_apps/why2025_ota/` — the reference WiFi-OTA implementation this proposal revives
- [radio_c6_updater_proposal.md](https://github.com/CJvanSoest/why2025-apps/blob/main/docs/radio_c6_updater_proposal.md) (why2025-apps repo) — the C6/storage-partition analysis this builds on
