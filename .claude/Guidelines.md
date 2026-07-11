# Working on this project with Claude

Rules and context for an AI pair programmer (or any contributor using one) on
DutchVMS. This is the entry point for the `.claude/` set. Read the one that
matches what you are about to do:

- **Guidelines.md** (this file): the mental model, where code goes, hard
  rules, conventions, the green gate.
- **[Components.md](Components.md)**: what lives where in `badgevms/` and
  `connectivity_esp_hosted/slave/`.
- **[Data-Flows.md](Data-Flows.md)**: cold start, the P4↔C6 LoRa RPC, and the
  app install/launch flow, with real function names.
- **[Build-And-CI.md](Build-And-CI.md)**: build invocation, the NAS-docker
  build pattern, and what the 3 CI jobs check.
- **[Workflow.md](Workflow.md)**: how to carry a change from first read to a
  green, physically-verified commit.
- **[Pitfalls.md](Pitfalls.md)**: traps that already cost real time or
  shipped a real bug here. Read before trusting a tool or an assumption.

Read these together with the root [CLAUDE.md](../CLAUDE.md). `.claude/` is
the same knowledge as the design docs in `docs/`, written as working rules.

## What this project is

Not a single app — a kernel plus a co-processor firmware, built together:

- **`badgevms/`** — the ESP32-P4 kernel. Process/task management
  (`task.c`), device registry (`device.c`), the windowing compositor
  (`compositor/`), drivers (`drivers/`), the UART app-deploy protocol
  (`deploy_protocol.c`), the OTA partition API (`ota.c`), app manifest
  lifecycle (`application.c`). ESP-IDF v5.5.1, C11, FreeRTOS. Most kernel
  state lives behind a mutex or is owned by exactly one task — see
  `.claude/Pitfalls.md` for what happens when that's not respected.
- **`connectivity_esp_hosted/slave/`** — the ESP32-C6 co-processor firmware,
  a fork of Espressif's esp-hosted slave. `main/tanmatsu/lora/` is the
  first-party addition: a custom LoRa protocol server driving an external
  SX126x over SPI, exposed to the P4 as custom esp-hosted events. `main/`
  otherwise tracks upstream esp-hosted closely — don't restructure it to
  match `badgevms/`'s style, it has to stay diffable against upstream.
- **`sdk_apps/`** — BadgeVMS SDK example apps only (curl_test,
  framebuffer_test, doomgeneric, ...), built as part of this repo for SDK
  regression coverage. Not the fork's real apps — those are
  [why2025-apps](https://github.com/CJvanSoest/why2025-apps), a separate
  repo built independently against `sdk_dist/`.

## The mental model

There is no component-layering system here like a multi-component ESP-IDF
project might have — `badgevms/` is one `idf_component_register()` target
(see `badgevms/CMakeLists.txt`'s `SRCS` list) with an internal split by
subsystem (drivers, compositor, kernel core), not enforced by the build the
way a `REQUIRES` graph would enforce it. The discipline instead is:

- **Kernel-private headers** (`badgevms/task.h`, `badgevms/why_io.h`, driver
  `*_internal.h` headers) are included by filename only from files in the
  same or a sibling directory within `badgevms/` — never exposed to apps.
- **App-facing headers** live under `badgevms/include/badgevms/` (`lora.h`,
  `ota.h`, `led_matrix.h`, `status_led.h`, `display.h`, `meminfo.h`, ...) and
  are the actual SDK surface `why2025-apps` builds against via `sdk_dist/`.
  Adding a kernel API app-visible means adding a header here, not just
  making a function non-`static`.
- **The C6 side is a separate translation unit tree entirely**, connected to
  the P4 only through the esp-hosted custom-event RPC — there is no shared
  header between `badgevms/` and `connectivity_esp_hosted/`, wire structs
  are duplicated by hand on both sides and must be kept in sync manually
  (see Data-Flows.md and the wire-compatibility rule in the root CLAUDE.md).

A change often doesn't go where the symptom shows: a "the LoRa packet never
arrived" bug can be a P4-side kernel request-buffer limit, a C6-side framing
bug, or an app-side protocol bug in the separate apps repo — all three have
been the actual root cause at different points on this fork (see
Pitfalls.md). Trace which side of the P4/C6 boundary — or which repo — the
symptom is actually on before picking a file.

## Where code goes

| Kind of code | Location | Notes |
|---|---|---|
| Kernel core (tasks, devices, memory, logical names) | `badgevms/*.c` at the top level | `task.c`/`device.c`/`memory.c`/`logical_names.c`. Kernel-private; app-facing wrappers live in a matching `*_bridge.c` + `include/badgevms/*.h` pair. |
| Compositor / windowing | `badgevms/compositor/` | Framebuffer allocation, window decorations, pixel functions. |
| Peripheral drivers | `badgevms/drivers/` | One driver, one responsibility, one file — see Pitfalls.md for what happens when that slips (the `badgevms_i2c_bus.c` split). |
| App-facing kernel API | `badgevms/include/badgevms/*.h` + a `*_bridge.c` in `badgevms/` | The actual SDK surface. A kernel feature isn't app-visible until it has both. |
| UART app-deploy protocol | `badgevms/deploy_protocol.c` | CRC-framed binary protocol, client is `why2025-apps/tools/badge_deploy.py`. Validate every path this touches — see the path-traversal fix in Pitfalls.md. |
| OTA partition write/rollback | `badgevms/ota.c` + `include/badgevms/ota.h` | Generic, already app-facing. Writes only the `ota_0`/`ota_1` app partition, never `storage` — see `docs/design/SD-and-OTA-Updates.md`. |
| C6 co-processor firmware (WiFi/BT slave + LoRa) | `connectivity_esp_hosted/slave/main/` | `tanmatsu/lora/lora_protocol_server.c` is the first-party LoRa addition; the rest tracks upstream esp-hosted. |
| Kernel build config, Kconfig options | `badgevms/Kconfig.projbuild`, `sdkconfig.defaults` | New experimental features get a Kconfig option (see `CJ_BADGEVMS_ENABLE_BADGELINK`) with a dedicated CI job, not a bare `#define`. |
| SDK examples | `sdk_apps/` | BadgeVMS SDK demos, built here for SDK regression coverage. Not the fork's real apps. |
| Design proposals (not yet implemented) | `docs/design/` | Written before code, reviewed, then implemented as a normal PR. |

## Hard rules (these have bitten real users)

- **Do not modify vendored code.** `badgevms/thirdparty/*` is a third-party
  drop (cJSON, dlmalloc, tomlc17, khash). Leave it as-is.
- **The P4↔C6 wire protocol has no version negotiation.** Structs like
  `lora_protocol_config_params_t` are shared *by convention*, not by a
  common header — extending one means appending fields at the end (never
  inserting/reordering) and bumping both firmwares together, exactly like
  that struct's own "WIRE-COMPATIBILITY WARNING" comment documents. Getting
  this wrong doesn't produce a build error or even a runtime error message;
  it silently breaks LoRa. This class of bug (a length-prefix byte radiated
  onto the air, a request buffer silently rejecting oversized payloads) is
  why MeshCore interop was broken for a long time before anyone found it —
  see Pitfalls.md.
- **`clang-format` only covers `badgevms/`.** `.github/workflows/ci.yml`'s
  format-check glob is `find badgevms -name '*.c' -o -name '*.h'`, excluding
  `thirdparty/`/`nanopb/`. Running `clang-format -i` on a
  `connectivity_esp_hosted/` file reformats it against the wrong style and
  produces an implausibly large diff for what should be a small change.
- **Kernel-level "should never happen" failures go through `why_die()`
  (`badgevms/why_io.h`/`wrapped_funcs.c`), not a bare `abort()`.** It's the
  same fatal outcome (the whole badge reboots — there is no per-task
  isolation for a corrupted kernel data structure) but logs a reason via
  `esp_system_abort()` instead of an undiagnosable bare abort.
- **Physical hardware is the real gate, CI is necessary but not
  sufficient.** The 3 CI jobs (clang-format, ESP-IDF build ×2, stack-usage)
  prove the code compiles, links, and doesn't blow a function's stack frame.
  They do not prove a driver initializes correctly against real silicon, a
  radio packet actually round-trips to another MeshCore node, or the badge
  boots past a given driver at all — several real bugs this session
  (the async-TX/mutex radio work, the `badgevms_i2c_bus.c` split) were only
  actually confirmed correct by flashing a physical badge and reading its
  boot log, not by CI alone.

## Conventions

- Commit messages: imperative subject, explain *why* not *what* in the
  body, `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` trailer on
  AI-authored commits.
- Comments are sparse: explain the non-obvious (a hardware quirk, a locking
  coupling, why a workaround exists), never restate what the code says.
- All repo text (code, comments, commit messages, docs) is in English.
- Format touched `badgevms/*.c`/`*.h` with `.clang-format`
  (clang-format-18.1.8, pinned in CI); match `connectivity_esp_hosted/`'s
  existing per-file style by hand since it isn't format-checked.

## Before you commit: all green

```sh
# Full build (both P4 badgevms.bin and the C6 network_adapter.bin sub-build)
idf.py build

# Stack-usage gate (mirrors the CI job exactly)
python3 scripts/check_stack_usage.py --threshold 5120 --root build/esp-idf/badgevms

# Format check on badgevms/ only (connectivity_esp_hosted/ is excluded, see above)
clang-format --dry-run --Werror $(find badgevms -name '*.c' -o -name '*.h' | grep -v /thirdparty/)
```

No local IDF toolchain? [`Build-And-CI.md`](Build-And-CI.md) has the exact
NAS-Docker invocation this fork's own history was mostly built with. When a
change touches a driver, the compositor, boot sequence, or the radio, the
green gate above is necessary but not sufficient — see the physical-hardware
rule above and [`Workflow.md`](Workflow.md) step 5.
