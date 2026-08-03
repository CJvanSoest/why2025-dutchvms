# Single merged flash images for GitHub Releases — design proposal

Design document for [why2025-dutchvms#10](https://github.com/CJvanSoest/why2025-dutchvms/issues/10)
("Ready to flash to 0x0 github releases").

> **Status (2026-08-03): IMPLEMENTED**, merged in
> [PR #19](https://github.com/CJvanSoest/why2025-dutchvms/pull/19) (task #92).
> `release.yml`'s "Build merged flash images" step now produces exactly what
> this document proposed, with every "Open question"/"Risk" below resolved
> as noted inline:
> - Assets are `esp32p4-update.bin`, `esp32p4-factory-erases-storage.bin`,
>   `esp32c6-update.bin` — the factory/update naming-confusion risk was
>   resolved by spelling the consequence into the filename itself (the
>   document's own suggested mitigation), not by keeping the shorter
>   `esp32p4-factory.bin` this proposal originally used.
> - C6 *is* merge-binned too (the "confirm this is wanted" open question —
>   yes, for symmetry with the P4 asset and issue #10's literal ask).
> - The C6's `ota_data_initial.bin` is read straight from
>   `connectivity_esp_hosted/slave/build/ota_data_initial.bin` — confirmed
>   NOT present in the flattened `build/c6firmware/` copy, so CI reads it
>   from its original sub-build location rather than adding it to that
>   `INSTALL_COMMAND`.
> - `Flashing.md` was restructured to lead with the one-line merged commands
>   for both first-time flash and updates, with the explicit multi-offset
>   form demoted to a "Building from source instead" subsection — plus a
>   bold warning about chip/asset-name mixups.
>
> Build-verified (byte-identical to individually-flashing the pieces) but,
> like the OTA work in [SD-and-OTA-Updates.md](SD-and-OTA-Updates.md), **not
> yet exercised through an actual tagged GitHub Release run** — the current
> `latest` release (`v1.2.0`) predates this workflow change.
>
> The rest of this document is kept as the original proposal for reference.

## Problem

Today a release only publishes the individual pieces
([Releases.md](../guides/Releases.md), [Flashing.md](../guides/Flashing.md)):
`badgevms.bin` for the P4, `bootloader.bin`/`partition-table.bin`/
`network_adapter.bin` for the C6. Flashing either chip from scratch means
typing four separate `offset file` pairs into one `esptool write_flash`
invocation by hand (see Flashing.md's "First-time flash" section). That's
easy to get wrong (wrong offset, missed file, transposed P4/C6 pair) and it's
the opposite of what issue #10 asks for: download one file per chip, run

```bash
esptool --chip esp32p4 write_flash 0x0 esp32p4.bin
esptool --chip esp32c6 write_flash 0x0 esp32c6.bin
```

and be done.

## What already exists (don't re-invent this)

- `esptool.py merge_bin` (bundled with the `esptool` package `idf.py build`
  already depends on) takes the same `offset file` pairs as `write_flash` and
  writes them into a single flat output file instead of flashing them,
  padding the gaps with `0xFF`. Flashing that output file at offset `0x0`
  reproduces exactly what flashing the individual pieces at their real
  offsets would have done — merge_bin doesn't change any offset, it just
  precomputes the padding between them into one blob.
- ESP-IDF's own `idf.py merge-bin` wraps that same esptool command using
  `build/flasher_args.json`, which the build already generates. **This is
  the trap, not the solution** — see below.
- The exact per-chip offsets are already documented in Flashing.md and don't
  need to be rediscovered: P4 bootloader `0x2000`, partition table `0x8000`,
  `ota_data_initial.bin` `0xd000`, `badgevms.bin` `0x10000`; C6 bootloader
  `0x0`, partition table `0x8000`, `ota_data_initial.bin` `0xd000`,
  `network_adapter.bin` `0x10000`.

### The trap: `flasher_args.json` already includes `storage`

Checked directly against a built tree (`build/flasher_args.json`,
`build/flash_project_args`): the P4's flasher args include a fifth entry
beyond the four Flashing.md documents by hand —

```
0x410000 storage.bin
```

— because `CMakeLists.txt` registers it there itself:

```cmake
# Add our binaries to the flash target
# bootloader, partition-table, otadata and ota_0 are done automatically
esptool_py_flash_to_partition(flash "storage" "${CMAKE_BINARY_DIR}/storage.bin")
```

That's intentional and correct for `idf.py flash` on a dev box doing a full
factory flash. But it means running `idf.py merge-bin` as-is — or blindly
scripting `esptool merge_bin` from `flasher_args.json` — silently produces a
merged image that **also reflashes `storage`**, i.e. exactly the user-data
wipe Flashing.md's "Storage (apps + user data)" section warns against doing
on every normal update. Any implementation of this issue has to build its
own explicit `offset file` list for the "update" image rather than trusting
`flasher_args.json`/`idf.py merge-bin` to leave `storage` out. (The C6 side
has no such trap — its `flasher_args.json` only ever has the four files
already in Flashing.md, there's no storage partition on that chip.)

## Proposal

Two merged image variants per release, mirroring the distinction Flashing.md
already draws between "First-time flash" and "Updating an existing badge" —
this issue doesn't remove that distinction, it collapses each side of it
into one file.

### 1. `esp32p4-update.bin` / `esp32c6-update.bin` — the common case

What most users want most of the time: update firmware, keep installed apps,
WiFi credentials, and MeshCore identity intact. Built with an explicit file
list (never `flasher_args.json`), no `storage`:

```bash
# P4
python -m esptool --chip esp32p4 merge_bin -o esp32p4-update.bin \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xd000  build/ota_data_initial.bin \
  0x10000 build/badgevms.bin

# C6
python -m esptool --chip esp32c6 merge_bin -o esp32c6-update.bin \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0     build/connectivity_esp_hosted/bootloader/bootloader.bin \
  0x8000  build/connectivity_esp_hosted/partition_table/partition-table.bin \
  0xd000  build/connectivity_esp_hosted/ota_data_initial.bin \
  0x10000 build/connectivity_esp_hosted/network_adapter.bin
```

Flashed with:

```bash
esptool --chip esp32p4 write_flash 0x0 esp32p4-update.bin
esptool --chip esp32c6 write_flash 0x0 esp32c6-update.bin
```

Same bytes at the same offsets as today's four-line commands, just one file
and one line each.

### 2. `esp32p4-factory.bin` — first-time / recovery flash only

Same as above plus `storage` (only needed on the P4 — the C6 has no such
partition, so `esp32c6-update.bin` and a hypothetical "factory" C6 image
would be identical; don't publish a separate C6 factory asset):

```bash
python -m esptool --chip esp32p4 merge_bin -o esp32p4-factory.bin \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000   build/bootloader/bootloader.bin \
  0x8000   build/partition_table/partition-table.bin \
  0xd000   build/ota_data_initial.bin \
  0x10000  build/badgevms.bin \
  0x410000 build/storage.bin
```

This one is large (`storage.bin` is ~11.6 MiB, so the merged file is roughly
that size) and, same as today's manual `storage.bin` flash, **wipes
everything on the badge**. It only replaces the "First-time flash" section
of Flashing.md — the four-separate-offsets P4/C6 commands for a blank badge
— not the update path.

### Release workflow changes (`.github/workflows/release.yml`)

Add a step after the existing build that runs the three `merge_bin` commands
above (P4 update, C6 update, P4 factory) and attach the three outputs as
extra release assets alongside the existing `badgevms.bin` +
`bootloader.bin`/`partition-table.bin`/`network_adapter.bin` + `.md5`
sidecars — **keep publishing the individual files too**, since
`cj_launcher`'s `do_firmware_update()`/`sync_c6_bundle()` (task #73) already
matches release assets by exact literal name
(`FW_ASSET_P4`/`FW_ASSET_C6_BOOT`/`FW_ASSET_C6_PART`/`FW_ASSET_C6_APP`, see
the comment block at the top of `release.yml`) and reads them individually
into the OTA partition / `storage` FAT filesystem respectively, not as a
flat merged image. The merged files are new, additive assets for the manual
esptool path; they don't replace anything the launcher's own OTA flow
consumes.

```yaml
- name: Build merged flash images
  shell: bash
  run: |
    . "$IDF_PATH/export.sh"
    python -m esptool --chip esp32p4 merge_bin -o esp32p4-update.bin \
      --flash_mode dio --flash_size 16MB --flash_freq 80m \
      0x2000  build/bootloader/bootloader.bin \
      0x8000  build/partition_table/partition-table.bin \
      0xd000  build/ota_data_initial.bin \
      0x10000 build/badgevms.bin
    python -m esptool --chip esp32p4 merge_bin -o esp32p4-factory.bin \
      --flash_mode dio --flash_size 16MB --flash_freq 80m \
      0x2000   build/bootloader/bootloader.bin \
      0x8000   build/partition_table/partition-table.bin \
      0xd000   build/ota_data_initial.bin \
      0x10000  build/badgevms.bin \
      0x410000 build/storage.bin
    python -m esptool --chip esp32c6 merge_bin -o esp32c6-update.bin \
      --flash_mode dio --flash_size 4MB --flash_freq 80m \
      0x0     build/c6firmware/bootloader.bin \
      0x8000  build/c6firmware/partition-table.bin \
      0xd000  build/connectivity_esp_hosted/ota_data_initial.bin \
      0x10000 build/c6firmware/network_adapter.bin
```

Note `build/c6firmware/` is the `CMakeLists.txt`-flattened copy the existing
workflow already reads `bootloader.bin`/`partition-table.bin`/
`network_adapter.bin` from — `ota_data_initial.bin` isn't copied there today
and would need adding to that `INSTALL_COMMAND`, or reading straight from
`connectivity_esp_hosted/slave/build/ota_data_initial.bin` instead (needs
verifying the exact path once this is actually built; not verified in this
pass — see Open questions).

Extend the existing "Verify firmware artifacts exist" step to check the
three new files, and add them to the `files:` list in the
`softprops/action-gh-release` step. Extend `docs/guides/Flashing.md`'s
"First-time flash" and "Updating an existing badge" sections to offer the
one-line merged-image command as the primary path, keeping the existing
multi-offset commands as the "what it actually does / building from source"
explanation underneath — a released binary's opaque merged offsets are
harder to sanity-check than an explicit list, so the explicit form stays
documented for anyone triaging a bad flash.

## How this relates to the existing OTA work (task #73, docs/design/SD-and-OTA-Updates.md)

Purely additive, not a replacement or overlap:

- Task #73's `cj_launcher` "Update Firmware" flow flashes `badgevms.bin`
  through `ota_write()`/`ota_session_commit()` (the ESP-IDF OTA partition
  API) and separately syncs the three C6 files into the `storage` FAT
  filesystem for `flash_slave_c6_if_needed()` to pick up on next boot — see
  `release.yml`'s own header comment and
  [SD-and-OTA-Updates.md](SD-and-OTA-Updates.md). None of that reads a
  merged flat image or writes to a raw offset; it stays exactly as-is.
- This issue is only about the **esptool-over-cable path**
  (Flashing.md), for a badge that either can't reach that OTA flow yet (a
  genuinely blank chip, per Flashing.md's own framing) or is being flashed
  by a developer/support person who doesn't have `cj_launcher` running to
  drive it. It makes that manual path a one-liner; it doesn't add a new
  update mechanism, and it doesn't change what a badge that's already
  running DutchVMS does on its own.
- The proposed SD-card installer in SD-and-OTA-Updates.md §1 is a third,
  separate path (no computer, no WiFi) and is unaffected by this issue
  either way — it stages `badgevms.bin` on the SD card and streams it
  through `ota_write()`, never through esptool or a raw offset.

## Risks

- **P4 and C6 both use offset `0x0` as the merge_bin "start of image"
  convention, and both are flashed with the same `write_flash 0x0
  <file>.bin` shape.** The only thing preventing `esp32c6-update.bin` from
  being written to the P4 (or vice versa) is remembering to pass the correct
  `--chip` flag and plugging into the correct USB port (see Flashing.md's
  port table). `esptool`'s `--chip` flag does reject a binary chip-magic
  mismatch before writing anything destructive, so this fails loudly rather
  than bricking either chip — but it's still an easy mistake to make when
  copy-pasting two near-identical commands, and worth calling out explicitly
  in the updated Flashing.md text (bold warning, not just implied by
  the existing port table).
- **A `-factory` and an `-update` asset that look almost identical in the
  release asset list** (`esp32p4-factory.bin` vs. `esp32p4-update.bin`) is a
  second, adjacent way to destroy user data by picking the wrong one — the
  filenames need to make the consequence obvious at a glance, not just
  differ by one word. Consider naming that leads with the consequence, e.g.
  `esp32p4-full-erases-storage.bin`, or keeping `-factory` but calling it out
  in bold in both the release notes template (Releases.md) and Flashing.md.
- **Publishing pre-merged binaries makes the exact offsets opaque** — anyone
  debugging a bad flash from a merged image can't see at a glance what went
  where without re-deriving it from `flasher_args.json` or this document.
  Mitigated by keeping the explicit multi-offset commands documented
  alongside the merged ones (see above), not removing them.
- **`ota_data_initial.bin`'s exact build path for the C6 side isn't
  currently copied into `build/c6firmware/`** by the existing
  `connectivity_esp_hosted` `ExternalProject_Add` `INSTALL_COMMAND` (only
  `network_adapter.bin`, `partition-table.bin`, `bootloader.bin` are). The
  workflow snippet above assumes it either gets added there or is read from
  its original `connectivity_esp_hosted/slave/build/` location — not
  confirmed against a real CI run in this pass, flag for whoever implements
  this to verify the path (or add it to `BUILD_BYPRODUCTS`/`INSTALL_COMMAND`
  the same way the other three files already are).
- **`idf.py merge-bin` (the ESP-IDF-native command) must not be used
  as-is** for the update image, for the `storage`-inclusion reason detailed
  above — the implementation has to call `esptool merge_bin` directly with
  an explicit file list. Using `idf.py merge-bin` unmodified anywhere in
  this workflow is itself a regression risk worth a code-review comment if
  it shows up in a PR.

## Open questions to resolve before implementing

- Exact final asset names (see the `-factory`/`-update` naming risk above) —
  needs a decision before `release.yml` and `cj_launcher`-adjacent docs
  reference them anywhere.
- Where `ota_data_initial.bin` actually lives for the C6 sub-build once this
  is implemented for real (see risk above) — a five-minute check against an
  actual `idf.py build` output, not assumed here.
- Whether to also merge-bin the C6 the same way when nothing on that side
  benefits from it beyond "one file instead of four" (no storage-partition
  trap to avoid on C6) — probably still worth it for symmetry with the P4
  asset and issue #10's literal ask (`esp32c6.bin`), but confirm that's
  actually wanted rather than assumed.
- Whether Flashing.md's "First-time flash" section should *lead* with the
  merged-image command (with the multi-offset form kept as an appendix) or
  the reverse — leaning toward merged-first per the issue's intent, final
  call belongs to whoever reviews the Flashing.md diff.

## See also

- [Flashing.md](../guides/Flashing.md) — current (multi-offset) flash/update instructions
- [Releases.md](../guides/Releases.md) — how a release is cut and published today
- [SD-and-OTA-Updates.md](SD-and-OTA-Updates.md) — the separate SD-card/OTA proposal this doesn't overlap with
- [why2025-dutchvms#10](https://github.com/CJvanSoest/why2025-dutchvms/issues/10) — the issue this document answers
