# Known issues — overview

This document collects the significant bugs/root-cause investigations from the WHY2025 badge project (DutchVMS firmware): what exactly was wrong, which files/code it affected, what fix attempts were made, and the result. Each item has its own GitHub issue with the full details.

Last updated: 2026-08-14.

## Table of contents

### Resolved

1. [BLE transport-timeout crash (cj_meshcore reset)](#1-ble-transport-timeout-crash-cj_meshcore-reset)
2. [C6 SDIO crash-loop → OTA rollback to an old version](#2-c6-sdio-crash-loop--ota-rollback-to-an-old-version)
3. [why_sbrk() shrink-path heap corruption](#3-why_sbrk-shrink-path-heap-corruption)
4. [cj_launcher boot loop (Launcher_Context stack overflow)](#4-cj_launcher-boot-loop-launcher_context-stack-overflow)
5. [application_list() manifest-read race](#5-application_list-manifest-read-race)
6. [cj_wifi_analyzer R/D hang (task priority)](#6-cj_wifi_analyzer-rd-hang-task-priority)
7. [Display backlight GPIO + PWM curve](#7-display-backlight-gpio--pwm-curve)
8. [PAX app-side symbol gaps](#8-pax-app-side-symbol-gaps)
9. [cj_launcher PAX splash performance](#9-cj_launcher-pax-splash-performance)
17. [Color-value-dependent render glitch (stripe/flicker)](#17-color-value-dependent-render-glitch-stripeflicker)
18. [C6 LoRa custom-RPC handler registration silently fails](#18-c6-lora-custom-rpc-handler-registration-silently-fails)

### Open / partially resolved

10. [SD-card write corruption (manifests + ELFs)](#10-sd-card-write-corruption-manifests--elfs)
11. [P4 OTA never gets confirmed (validate_ota_partition)](#11-p4-ota-never-gets-confirmed-validate_ota_partition)
12. [C6 radio sometimes reflashes on every boot](#12-c6-radio-sometimes-reflashes-on-every-boot)
13. [Deploy PUT: OOM + UART overrun on large files (closed, superseded)](#13-deploy-put-oom--uart-overrun-on-large-files-closed-superseded)
14. [Factory flash wipes WiFi credentials](#14-factory-flash-wipes-wifi-credentials)
15. [About tile crash after firmware update](#15-about-tile-crash-after-firmware-update)
16. [Stray render dots on Home tiles](#16-stray-render-dots-on-home-tiles)

---

## Resolved

### 1. BLE transport-timeout crash (cj_meshcore reset)
`cj_meshcore` crashed 100% reproducibly ~4-5s after opening. Root cause: a bug in Espressif's own `esp_hosted` component (`transport_drv_reconfigure()`) that never stops a 5s watchdog timer when the transport is already up (always the case here, since WiFi brings it up before BLE reconfigures later). Fix: local, git-tracked component override with the missing timer-stop added. Hardware-confirmed, PR #49 merged.
**Full details:** [issue #50](https://github.com/CJvanSoest/why2025-dutchvms/issues/50)

### 2. C6 SDIO crash-loop → OTA rollback to an old version
Firmware kept reverting to an older version after a few reboots. Turned out to be a crash loop around the C6 radio reflash logic (3 stacked bugs: reflash timing, ESP-Hosted resetting every boot, `flash_binary()` short-read corruption) that triggered ESP-IDF's own bootloader rollback. Resolved in v1.3.9.
**Full details:** [issue #51](https://github.com/CJvanSoest/why2025-dutchvms/issues/51)

### 3. why_sbrk() shrink-path heap corruption
App launch silently degraded after starting/closing apps a few times — manifest reads reported success but returned zero bytes. Root cause: `why_sbrk()`'s shrink path computed the new total heap size instead of the delta to release, unmapping too much memory. Fixed, verified via host-side dlmalloc simulation, hardware-confirmed, PR #17 merged.
**Full details:** [issue #52](https://github.com/CJvanSoest/why2025-dutchvms/issues/52)

### 4. cj_launcher boot loop (Launcher_Context stack overflow)
A large struct (`Launcher_Context`) as a stack-local variable in the launcher's entry point no longer fit on the app stack → immediate crash, infinite boot loop (the launcher is the first app). Fix: `static` instead of stack-local + `_Static_assert` as a safety net.
**Full details:** [issue #53](https://github.com/CJvanSoest/why2025-dutchvms/issues/53)

### 5. application_list() manifest-read race
The app list structurally showed only part of the installed apps — the same apps deterministically failed every boot. Fix: `application_list()` rewritten to snapshot all filenames first and `closedir()` before opening any files, plus a retry layer.
**Full details:** [issue #54](https://github.com/CJvanSoest/why2025-dutchvms/issues/54)

### 6. cj_wifi_analyzer R/D hang (task priority)
Rescan/Diagnostic hung due to a system-wide scheduling bug: the UART deploy listener (prio 6, core 0) fully starved the WiFi `hermes` task (prio 5, core 0) once the listener became active. Fix: listener priority lowered to 3.
**Full details:** [issue #55](https://github.com/CJvanSoest/why2025-dutchvms/issues/55)

### 7. Display backlight GPIO + PWM curve
The brightness tile didn't actually dim the screen (wrong GPIO traced, backlight PWM lives on the C6 not the P4), and the PWM curve gave barely any visible difference between 30-100%. Fix: correct GPIO (KiCad-verified) + gamma-2.2 curve. Hardware-confirmed.
**Full details:** [issue #56](https://github.com/CJvanSoest/why2025-dutchvms/issues/56)

### 8. PAX app-side symbol gaps
PAX apps wouldn't start — initial diagnosis suspected a missing kernel TLS runtime, which turned out to be wrong: three small gaps against the `symbols.yml` allow-list (`__tls_get_addr`, 7 long-double libgcc helpers, `aligned_alloc`). All three resolved app-side without a kernel change. Hardware-confirmed, PR #18 merged.
**Full details:** [issue #57](https://github.com/CJvanSoest/why2025-dutchvms/issues/57)

### 9. cj_launcher PAX splash performance
The boot-splash animation ran choppily. Two rounds of CPU draw-cost optimization had zero effect — the real bottleneck was a fixed 60ms `usleep()` after every frame plus a halved rotation counter. Fix: 60ms→15ms + stop halving the rotation. Hardware-confirmed.
**Full details:** [issue #58](https://github.com/CJvanSoest/why2025-dutchvms/issues/58)

### 17. Color-value-dependent render glitch (stripe/flicker)
Certain solid-fill colors (e.g. `0x7AA2F7`, `0xBB9AF7`) rendered as two different colors within one fill, or flickered over time. Three PPA-specific hypotheses were ruled out first (integer-upscaling interpolation, hardware `rgb_swap`, missing input cache-flush) and the bug was confirmed to reproduce identically with PPA fully disabled — so the compositor was never the cause. Root cause, found via 5 further hardware A/B tests (screen-panel timing swap, PSRAM speed, a physical connector flex test, and finally MIPI DSI lane bitrate): **MIPI DSI signal integrity** between the P4 and the display panel. Fix: `LCD_MIPI_DSI_LANE_BITRATE_MBPS` lowered from 1000 to 700 in `badgevms/drivers/st7703.c`. Also refutes the earlier "B=247 threshold" theory — a full blue-channel sweep banded identically at every value from 230 to 255. Hardware-confirmed.
**Full details:** [issue #65](https://github.com/CJvanSoest/why2025-dutchvms/issues/65) (closed), [issue #77](https://github.com/CJvanSoest/why2025-dutchvms/issues/77) (full test log)

### 18. C6 LoRa custom-RPC handler registration silently fails
`CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS` defaulted to 3 on the C6. `tanmatsu_main.c` registers 3 non-LoRa custom RPC callbacks (echo, display backlight, keyboard backlight) before `lora_protocol_server.c`'s `lora_initialize()` tries to register a 4th (`TANMATSU_EVENT_LORA`). The table was full, so that registration silently failed with `ESP_ERR_NO_MEM` ("No space for callback") — LoRa never started (no crash, C6 stayed quiet), so every P4-side LoRa RPC call got `ESP_ERR_NOT_FOUND` forever, which showed up on the badge as the LoRa status LED (LED0) stuck red after the 15s startup grace period, with an otherwise perfectly healthy boot. Shipped in v1.4.0 undetected — the sx126x driver upgrade in that release was the first time anyone watched LoRa init closely on hardware after a clean build. Fixed by raising the limit to 6 in `sdkconfig.defaults`. Hardware-confirmed (LED0 + LED1 both green, no more `ESP_ERR_NOT_FOUND` in the boot log).
**Full details:** [issue #82](https://github.com/CJvanSoest/why2025-dutchvms/issues/82)

---

## Open / partially resolved

### 10. SD-card write corruption (manifests + ELFs)
Files on the SD card (`badgevms_launcher.json`, `cj_launcher.elf`, and separately-observed `cj_hello.json`/`cj_files.json`) have twice been found corrupted at the correct file length but with garbage content partway through — two different garbage signatures seen (a repeating byte pattern, and all-zero). First occurrence followed a WiFi app-repo self-update download; second followed a plain firmware reflash+reboot with no download involved, and hit two apps (`cj_hello`, `cj_files`) that never write their own manifest. That rules out "specific to the launcher's download code" as the sole cause and points at the underlying SD/FAT write path more broadly (`badgevms/drivers/fatfs.c` + the wear-leveling library) not being safe against a reset/power-cycle interrupting a write. Both occurrences recovered by rewriting the affected files directly via an external SD card reader, bypassing the badge. Root cause not yet found. **2026-08-14: reproduced again** — a `cj_launcher.elf` UART deploy failed mid-transfer (see #13) and left the file fully truncated to 0 bytes rather than garbage-filled, a third distinct corruption signature; recovered the same way (direct SD card reader). No GitHub issue has actually been filed for this topic yet despite the number below — flagging rather than leaving a dangling/misleading link.
**Full details:** issue #65 was believed to be reserved for this when this doc was written, but that number was later used for a different, unrelated (now-closed) issue — see [#17](#17-color-value-dependent-render-glitch-stripeflicker) above. This SD-corruption topic still needs its own real issue filed.

### 11. P4 OTA never gets confirmed (validate_ota_partition)
The OTA partition was never marked valid despite the new image running fine — four independent instrumentation channels (UART, SD, NVS flag, NVS stack watermark) all showed zero evidence the confirmation code itself executes. **Workaround applied**: bootloader rollback disabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` off) since v1.3.17 — OTA updates have worked reliably since. The underlying mystery hasn't been found; that remains open and needs a JTAG session.
**Full details:** [issue #59](https://github.com/CJvanSoest/why2025-dutchvms/issues/59)

### 12. C6 radio sometimes reflashes on every boot
After a P4 reflash, the C6 radio started reflashing itself on every boot despite matching MD5s — an app-level resync via the launcher didn't fix it. **Workaround**: a direct esptool bin-flash of the C6 from the NAS (bypassing the app-level flash logic) works reliably. The underlying bug in `slave_c6_flasher.c`'s own flash-then-verify logic hasn't been found.
**2026-08-14 correction**: "works reliably" above is misleading — a direct esptool flash of the C6 alone does **not** stick. `flash_slave_c6_if_needed()` (`badgevms/drivers/esp-serial-flasher/slave_c6_flasher.c`) runs on every P4 boot and compares the on-chip C6 firmware's MD5 against the SD-staged `APPS:[why2025_firmware_ota_c6]` bundle, silently reflashing the C6 back to whatever's staged there on any mismatch. The P4 stays powered even with its own USB cable unplugged (shared board power), and this badge appears to reset one chip whenever the other's USB cable is (re)connected — so a cable-only esptool C6 flash gets silently reverted the next time the P4 reboots, which on this hardware can be triggered just by plugging in the other chip's cable. Confirmed by direct reproduction: an esptool flash that verified correct by hash immediately failed a `verify_flash` check a few minutes later, with the boot log showing the pre-fix firmware's behavior again. This is expected/correct behavior of the auto-heal mechanism itself, not a new bug — but it means any manual C6 firmware change (including this session's fix for #82) must also update the SD-staged bundle (3 `.bin` files + `.md5` sidecars), or it silently reverts.
**Full details:** [issue #60](https://github.com/CJvanSoest/why2025-dutchvms/issues/60)

### 13. Deploy PUT: OOM + UART overrun on large files (closed, superseded)
Two related bugs in the same UART deploy PUT code path (OOM on the whole-frame malloc, and a suspected RX FIFO overflow on large files above ~150-200KB). A streaming rewrite fixing the OOM half was built (PR #48) but never merged — blocked by a separate, not cleanly isolated intermittent early-boot heap crash. **2026-08-14: reproduced again** deploying a 203KB `cj_launcher.elf` — failed consistently across 3 attempts (2 with a "no response magic" timeout, 1 with a raw `OSError: Input/output error` on the serial write), corrupting the file to 0 bytes and crash-looping the launcher; recovered via direct SD-card-reader file replacement, not UART. Never root-caused. **Closed 2026-08-17**: badgelink and MSC are now the working deploy paths, so large-file UART PUT (and `cj_launcher` deploy over UART specifically) is no longer needed.
**Full details:** [issue #61](https://github.com/CJvanSoest/why2025-dutchvms/issues/61)

### 14. Factory flash wipes WiFi credentials
A full/factory esptool flash wipes the NVS partition (WiFi credentials). **Workaround**: non-destructive updates deliberately flash only the app partition (offset 0x10000), leaving NVS untouched. No structural prevention (e.g. automatic backup/restore) for cases where a real factory flash is actually needed.
**Full details:** [issue #62](https://github.com/CJvanSoest/why2025-dutchvms/issues/62)

### 15. About tile crash after firmware update
The About screen occasionally crashes right after a firmware update. No targeted root-cause diagnosis done yet; possibly (partly) related to issue #11's OTA `PENDING_VERIFY` state. As of 2026-08-05 still occurs occasionally, but less often than before.
**Full details:** [issue #63](https://github.com/CJvanSoest/why2025-dutchvms/issues/63)

### 16. Stray render dots on Home tiles
Render artifacts on the Apps/Storage tiles whose position shifts between frames. Not yet investigated. **2026-08-14: confirmed independent of #17's color-glitch fix** — the DSI-signal-integrity fix that resolved the stripe/flicker bug left these dots unchanged, so they have a separate root cause, not the shared one #17's issue originally speculated about.
**Full details:** [issue #64](https://github.com/CJvanSoest/why2025-dutchvms/issues/64)
