# Workflow

How to carry a change from first read to a green, physically-verified
commit. The rules behind each step are in [Guidelines.md](Guidelines.md);
the traps are in [Pitfalls.md](Pitfalls.md).

## 1. Understand before editing

- Decide which side of the P4/C6 boundary the change is on — or whether it's
  actually in the separate `why2025-apps` repo instead. See
  [Components.md](Components.md) and [Data-Flows.md](Data-Flows.md).
- If the symptom looks like a wire-protocol issue, check both sides of the
  boundary before assuming which one is wrong — verify against the
  independent reference implementation where one exists (the Tanmatsu
  MeshCore port, `CJvanSoest/meshcore`, is the reference for anything
  MeshCore-protocol-shaped even though that code lives in a different repo
  entirely).

## 2. Make the change in the right place

- Kernel-private code stays kernel-private (no new app-facing symbol without
  a header under `include/badgevms/`).
- A new driver gets its own file with one responsibility — see the
  `badgevms_i2c_bus.c` split in Pitfalls.md for what happens when that
  slips.
- A wire-format change on either side of the P4↔C6 boundary must be made on
  **both** sides in the same PR, or it silently breaks LoRa (no build error,
  no runtime error — see Data-Flows.md).
- New source file in `badgevms/`? Add it to `badgevms/CMakeLists.txt`'s
  `SRCS` list (alphabetically), and give it the existing GPL header block.

## 3. Verify the change

There's no host-test harness in this repo (unlike the apps-repo-side
MeshCore protocol code, which does have one) — the actual gate here is:

```sh
idf.py build                                                              # compiles + links, both P4 and C6
python3 scripts/check_stack_usage.py --threshold 5120 --root build/esp-idf/badgevms
clang-format --dry-run --Werror $(find badgevms -name '*.c' -o -name '*.h' | grep -v /thirdparty/)
```

All three run without physical hardware — see
[Build-And-CI.md](Build-And-CI.md) for the no-local-toolchain path (this is
the common case; most of this fork's history was verified this way).

## 4. Verify on physical hardware when the change touches...

...a driver, the compositor, the boot sequence, the radio, or anything else
CI genuinely can't exercise. Flash the badge (both P4 and C6 if the change
touches the wire boundary) and capture the boot log:

```sh
python -m esptool --chip esp32p4 -b 460800 --port /dev/ttyUSB0 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin \
  0x10000 build/badgevms.bin
```

See `docs/guides/Flashing.md` for the full command set (both chips, exact
offsets) and which USB port is which chip. A clean boot log — no unexpected
`ESP_LOGE`, the expected subsystems starting, WiFi/radio coming up if
relevant — is the actual proof a driver or boot-path change works; a green
`idf.py build` alone is not.

## 5. Commit

- Imperative subject, explain *why* in the body (not a restatement of the
  diff), `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` trailer
  on AI-authored commits.
- One PR per issue where a GitHub issue exists (branch protection on `main`
  requires a PR; see Build-And-CI.md). Reference the issue
  (`Closes #N`) in the PR body.

## 6. Push and confirm CI

- Confirm the PR's checks are green before merging — `clang-format` and
  `ESP-IDF build` are required; `ESP-IDF build (badgelink)` is informational
  but worth checking too.
- Report outcomes honestly. "CI is green" and "I flashed a badge and
  confirmed it boots" are different claims — say which one you actually did.
  Several real bugs on this fork (the LoRa off-by-one, the tca8418
  buffer-offset bug) compiled and linked cleanly for a long time before
  anyone actually exercised the affected code path.

## What each gate does and does not prove

| Gate | Proves | Does not prove |
|---|---|---|
| `idf.py build` | Both firmwares compile and link | Runtime correctness, radio interop, that a driver initializes against real silicon |
| `check_stack_usage.py` | No single function's stack frame exceeds the threshold | Actual stack-depth-under-load safety (nested calls across functions can still overflow) |
| `clang-format --dry-run` | `badgevms/` formatting matches CI | Nothing about `connectivity_esp_hosted/`, which isn't checked at all |
| A physical badge boot log | The change works on real hardware, right now, on this badge | Nothing about a different badge revision, a cold boot with fresh flash, or a scenario not exercised during the boot window captured |
