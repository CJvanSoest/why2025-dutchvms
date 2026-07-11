# CLAUDE.md

Guidance for Claude Code (and any contributor) working in this repository.
Adapted from the same model used in [CJvanSoest/meshcore](https://github.com/CJvanSoest/meshcore)'s
`.claude/` handbook — same shape, content rewritten for what this repo
actually is.

## What this is

**DutchVMS** — [CJ van Soest](https://github.com/CJvanSoest)'s fork of
[BadgeVMS](https://gitlab.com/why2025/team-badge/firmware), the kernel/OS for
the WHY2025 conference badge. Two chips, one repo, built together:

- **ESP32-P4** (app processor) — `badgevms/`, the kernel: process/task
  management, the compositor, drivers, the app-facing syscall surface. Builds
  to `build/badgevms.bin`.
- **ESP32-C6** (radio co-processor) — `connectivity_esp_hosted/slave/`, a
  customized fork of Espressif's esp-hosted slave firmware serving standard
  WiFi/BT *and* a custom LoRa protocol server
  (`connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol_server.c`)
  on the same image. Builds to `network_adapter.bin` as a sub-build of the
  same `idf.py build`.

These two firmwares talk to each other over a small internal RPC (custom
`esp-hosted` events, see `.claude/Data-Flows.md`) and **must be flashed as a
matching pair** whenever that wire format changes — see
[docs/guides/Flashing.md](docs/guides/Flashing.md).

**The actual user-facing apps are not in this repo.** MeshCore, the storage
browser, WiFi analyzer, the launcher — all of that is the separate
[why2025-apps](https://github.com/CJvanSoest/why2025-apps) repo, compiled
independently against `sdk_dist/` (produced by `idf.py sdk` here) and
deployed onto the badge's SD card as position-independent ELFs, never linked
into this repo's own build. `sdk_apps/` in this repo is BadgeVMS SDK
*examples* only (framebuffer_test, curl_test, doomgeneric, ...), not the
fork's real apps.

## Start here

Before changing code, read the handbook in [`.claude/`](.claude):

- [`.claude/Guidelines.md`](.claude/Guidelines.md) — the index: mental model,
  where code goes, hard rules, conventions, the green gate.
- [`.claude/Components.md`](.claude/Components.md) — what lives where in
  `badgevms/` and `connectivity_esp_hosted/slave/`.
- [`.claude/Data-Flows.md`](.claude/Data-Flows.md) — cold start, the P4↔C6
  LoRa RPC, and the app install/launch flow, with real function names.
- [`.claude/Build-And-CI.md`](.claude/Build-And-CI.md) — build invocation,
  the NAS-docker pattern (most work here happens without a local IDF
  toolchain), and what the 3 CI jobs actually check.
- [`.claude/Workflow.md`](.claude/Workflow.md) — first read to a green,
  physically-verified commit.
- [`.claude/Pitfalls.md`](.claude/Pitfalls.md) — traps that already cost
  real time or shipped a real bug on this fork. Read before trusting a tool
  or an assumption here — several of these are non-obvious enough that they
  were rediscovered the hard way once already.

Design rationale and process docs live in [`docs/`](docs) —
[docs/design/](docs/design) for proposals not yet built,
[docs/guides/](docs/guides) for how-to (flashing, releases),
[docs/CHANGELOG.md](docs/CHANGELOG.md) for what shipped when. `.claude/` is
the same knowledge written as working rules for a contributor (human or AI)
about to touch code; when the two disagree, fix `.claude/` to match reality.

## Build, flash, test

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build                    # builds badgevms.bin (P4) AND drives the
                                 # connectivity_esp_hosted sub-build (C6)
idf.py sdk                      # one-time — produces sdk_dist/ for the
                                 # separate why2025-apps repo's build.sh
```

No local IDF toolchain? Most of this project's own history was built via a
NAS-hosted `espressif/idf:v5.5.1` Docker image instead — see
[`.claude/Build-And-CI.md`](.claude/Build-And-CI.md) for the exact pattern.
See [docs/guides/Flashing.md](docs/guides/Flashing.md) for getting either
binary onto a physical badge.

## Rules that matter here

1. **Do not modify vendored code.** `badgevms/thirdparty/` (cJSON, dlmalloc,
   tomlc17, khash) is a third-party drop. Leave it alone.
2. **`badgevms/` and `connectivity_esp_hosted/slave/` use different code
   styles, and only `badgevms/` is CI-format-checked.** Running
   `clang-format -i` across the whole tree will reformat
   `connectivity_esp_hosted/` files wholesale against a style CI doesn't
   enforce there — always check `git diff --stat` for an implausibly large
   diff before committing a "small" formatting-adjacent change. See
   [`.claude/Pitfalls.md`](.claude/Pitfalls.md).
3. **The P4↔C6 wire protocol is fragile and silent on mismatch.** A struct
   shape or framing change in `lora_protocol_server.c` (C6) must be mirrored
   exactly in `badgevms/drivers/lora_proto_client.c` (P4) — see
   `lora_protocol_config_params_t`'s own "WIRE-COMPATIBILITY WARNING"
   comment for the pattern to follow when extending a wire struct. Getting
   this wrong doesn't error, it just silently breaks LoRa.
4. **Physical hardware is the real gate for anything touching radio,
   compositor, drivers, or boot.** CI proves it compiles, links, and stays
   under the stack-usage threshold. It does not prove a driver initializes
   correctly, a radio packet round-trips, or the badge boots. Say plainly
   which one you actually verified.

## Conventions

- Commit messages: imperative subject, explain *why* not *what* in the
  body, `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` trailer
  on AI-authored commits (this fork's convention — differs from the
  upstream-mirror `meshcore` project's own no-trailer rule, don't copy that
  rule here).
- Comments are sparse: explain the non-obvious (a hardware quirk, a locking
  coupling, a workaround for a specific bug), never restate what the code
  does.
- All repo text (code, comments, commit messages, docs) is in English.
- Format touched `badgevms/*.c`/`*.h` files with `.clang-format`
  (clang-format-18, exact version pinned in `.github/workflows/ci.yml`)
  before committing — `connectivity_esp_hosted/` is excluded from the CI
  check, match its existing per-file style by hand instead.
