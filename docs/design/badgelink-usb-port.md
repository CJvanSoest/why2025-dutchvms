# BadgeLink / tanmatsu-usb-msc over native USB — porting roadmap

Design document, 2026-08-16. Supersedes the "physically impossible" verdict
in [`.claude/Components.md`](../../.claude/Components.md)'s former "Rejected:
BadgeLink" note — see the correction below.

**Status (2026-08-16): BadgeLink over native USB is hardware-confirmed
working end-to-end on a real badge, via the diamond key.** `badgelink.py fs
list`/`fs download` both succeed against the badge's actual `/SD0` over the
bottom USB-C port (`16d0:0f9a "MCS WHY2025 badge"`), file content verified
byte-correct. Items 1-4 (TinyUSB+mux, the badgelink component, the
`badgelink_fs.c` glue, and the diamond-key app trigger) are all real, and
all hardware-verified now, not just build-verified.

**The bug that blocked this and its fix:** the mux GPIO (GPIO2) needed
`gpio_reset_pin()` before `gpio_set_direction()`/`gpio_set_level()` --
without it, the pin never actually left its default IOMUX state and the mux
silently stayed on the C6 no matter what the software did (no crash, no
error, `bv_usb_device_set_mode()` still returned `true`, the state was
tracked as "active" -- just no physical effect). This codebase already hit
the identical bug class once before, on GPIO3 (`board_bringup.c`'s
vibrator-motor fix) -- same root cause: BadgeVMS has no badge-bsp-style
board-wide pin bring-up step before user driver code runs, so a pin can't be
assumed to start in a plain-GPIO-ready state just because Senna's badge-bsp-based
firmware (which does do that bring-up) never needed the same reset call.

`tanmatsu-usb-msc` has a build-verified kernel skeleton + launcher tile, but
the actual SD-card storage handoff is still a deliberate stub — see
"Implementation status" below for what landed and why MSC storage itself
isn't live yet.

## Corrected hardware fact: there is a USB mux

The BadgeLink removal on 2026-08-07 (`docs/CHANGELOG.md` `[1.3.18]`) rested on
two blockers. Blocker 2 (COBS framing vs. shared UART0 console logs) is still
correct for a UART transport. **Blocker 1 was wrong**: "the P4's native-USB
pins are not routed to any external connector" — they are, through a mux.

Confirmed 2026-08-16 by re-testing the *other* physical port with Senna
Hijlkema's (Senna-chan's) `tanmatsu-launcher` WHY2025 port
(`senna_idf6_native`, her own fork): the earlier test used the CH340
side-port (P4 UART bridge only) and saw nothing, which is expected — that
port was never the candidate. The bottom (previously assumed "C6-only") port
carries a **USB mux**, switched by a single GPIO:

```c
// senna_idf6_native/main/usb_device.c, usb_mode_set(), WHY2025 branch
case USB_DEVICE:
    tud_disconnect();
    gpio_set_level(2, 0); // Sets MUX to P4
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();
    break;
case USB_DEBUG:
    tud_disconnect();
    gpio_set_level(2, 1); // Sets MUX to C6
    break;
```

GPIO2 low routes the connector's D+/D- to the P4's own **High-Speed OTG PHY**
(`usb_new_phy()` with `USB_PHY_CTRL_OTG` / `USB_PHY_TARGET_INT` /
`USB_OTG_MODE_DEVICE`, TinyUSB brought up on `TINYUSB_PORT_HIGH_SPEED_0` —
see `usb_initialize()`, same file, lines 304-317); GPIO2 high (the default)
routes it to the C6's own native USB-Serial-JTAG debug interface instead.
Tested live on real WHY2025 hardware: the bottom port immediately enumerated
as `16d0:0f9a "MCS WHY2025"` (BadgeLink's vendor USB descriptor), and
`badgelink.py appfs list` worked against it on the first try (protocol
version 3 negotiated, correct AppFS listing).

**Practical consequence for DutchVMS:** BadgeLink over native USB does not
share a bus with `deploy_protocol.c` (which stays on UART0/CH340). The old
mutual-exclusion concern was specific to the *UART* variant and doesn't apply
here — the two deploy paths could coexist.

![Senna's manual USB HS wire mod on the M.2 connector](../images/senna_usb_hs_mod.jpg)

*Before the GPIO2-mux behavior above was understood, Senna had already found
a way to reach the P4's native-USB pins by hand: soldering fine wires
directly onto the M.2 connector, which she herself described as "hard as
fck." That hand mod is no longer necessary — the mux already exposes this in
software — but it's kept here as evidence the P4's HS-OTG PHY really is
reachable off the M.2 module, independent of the mux finding. Source: Senna,
shared via Discord, 2026-08-16.*

## What BadgeLink in DutchVMS would need

1. **A TinyUSB device-mode stack.** DutchVMS had none before this branch —
   the only USB components previously in the tree were *host*-mode
   (`espressif__usb_host_cdc_acm`, `usb_host_ch34x_vcp`, `usb_host_cp210x_vcp`).
   **Now implemented**: `badgevms/drivers/usb_device.c`, ported from the
   PHY/mux bring-up above (`usb_new_phy()` + `gpio_set_level(2, ...)`) — see
   "Implementation status" below.
2. **The `badgeteam__badgelink` managed component.** Nanopb protobuf + COBS
   framing, transport-agnostic (`badgelink_rxdata_cb()` in, a byte-send
   callback out) — no changes needed to this layer itself. **Now vendored**
   into `badgevms/drivers/badgelink/`.
3. **DutchVMS-specific protocol-handler glue.** Upstream's handlers
   (`badgelink_fs.c`, `badgelink_appfs.c`, `badgelink_nvs.c`,
   `badgelink_startapp.c`) are written against Tanmatsu's AppFS + app-launcher
   model, which DutchVMS doesn't have (BadgeVMS has its own kernel VFS —
   `why_fopen()` and friends in `wrapped_funcs.c` — and its own PIE-ELF
   process model in `task.c`). **Now implemented** — see "Implementation
   status" below for exactly how (short version: `badgelink_fs.c` needed no
   BadgeVMS-specific rewrite at all, just a path-prefix fix; `appfs`/
   `startapp` are explicit `StatusNotSupported` stubs).
4. **A mode-switch trigger, hardware-verified.** GPIO2 defaults to the C6
   side, so something has to flip it to reach the P4's BadgeLink identity.
   Senna's launcher wires this through
   `badgelink_set_usb_mode_callback(usb_mode_set_from_badgelink)`
   (`senna_idf6_native/main/main.c:671`) — i.e. BadgeLink itself can issue the
   mode switch once *some* channel to it exists. The exact bootstrap sequence
   (what channel is live by default, whether a menu item or button-hold is
   also involved) wasn't traced end-to-end in this research and needs
   confirming on a DutchVMS test build before relying on it.
5. Cross-reference: a working USB deploy path is also relevant to
   [`docs/KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) `#10` (SD-card write
   corruption) and `#13` (deploy PUT UART overrun on large files) — both are
   about the *existing* SD/UART deploy paths being unreliable for large
   files. BadgeLink-over-USB wouldn't fix either bug, but could be a more
   reliable escape hatch for exactly the case `#13` currently warns
   against (`cj_launcher.elf` over UART).

### Implementation status (2026-08-16)

Items 1-3 now have real code on `feature/badgelink-usb-port`, not yet
build-verified (no NAS docker / physical badge available in this session)
or flashed:

- **Item 1 (TinyUSB + mux)**: `badgevms/drivers/usb_device.c` +
  `usb_device.h`, ported from Senna's reference almost line-for-line (PHY
  config, GPIO2 toggle, vendor-class descriptors). Added `espressif/esp_tinyusb`
  as a new managed dependency in `badgevms/idf_component.yml` (pinned
  `^2.2.1` to match what her port and `tanmatsu-usb-msc` both use — **not
  confirmed to resolve cleanly against this repo's IDF 5.5.1 pin**, her port
  is on IDF 6.0.2; check this first if the component-manager fetch fails).
- **Item 2 (badgelink component)**: vendored into
  `badgevms/drivers/badgelink/` — same location the 2026-07-10 UART attempt
  used (`git show 418ae4c` in this repo's own history), source files
  extracted from that exact commit rather than re-copied from the Tanmatsu
  reference tree, since they were already adapted and reviewed once. Wired
  into `badgevms/CMakeLists.txt`'s `SRCS`/`INCLUDE_DIRS`.
- **Item 3 (protocol-handler glue)**: also reused verbatim from `418ae4c`'s
  `badgelink_fs.c` (plain libc `fopen`/`opendir`/etc., **not** `why_fopen()`
  — deliberately, since BadgeLink's own thread and the raw-USB-callback path
  are plain FreeRTOS tasks with no BadgeVMS per-task `thread` struct, and
  `why_*` calls need one; `deploy_protocol.c` and the old
  `badgelink_transport_uart.c` hit the same constraint for the same reason).
  Already retargeted from upstream's `/sd` prefix to this repo's actual
  `/SD0` FATFS mount point (`fatfs.c`'s `fatfs_create_sd("SD0", ...)`,
  confirmed by reading that file, not assumed). `badgelink_appfs.c` stays a
  `StatusNotSupported` stub (no AppFS equivalent in BadgeVMS) and
  `badgelink_startapp.c` stays a `StatusNotSupported` stub too (no
  slug-addressed app store to map "start app" onto) — both are the same
  considered decision `418ae4c` already made, not something this session
  changed.
- Wired into boot via `usb_device_init()` in `why2025_firmware.c`, right
  after `deploy_protocol_init()` — non-fatal on failure, same pattern.
  TinyUSB comes up but the mux stays pointed at the C6 by default; nothing
  auto-enables BadgeLink at boot any more (see item 4).
- **Item 4 (mode-switch trigger) — implemented 2026-08-16, launcher-only
  (not global/system-wide).** `usb_device.c` now tracks a 3-state
  `usb_device_mode_t` (DEBUG/BADGELINK/MSC) with a single entry point,
  `usb_device_switch_to()`. App-facing surface:
  `badgevms/include/badgevms/usb_device.h`'s `bv_usb_device_set_mode()`/
  `bv_usb_device_get_mode()`, wired through a new
  `badgevms/usb_device_bridge.c` (same thin-wrapper shape as
  `status_led_bridge.c`). `apps/cj_launcher/main.c` (why2025-apps repo)
  binds the physical diamond key to it **on the HOME screen only** —
  toggles BadgeLink, shows a status toast, and a persistent "BadgeLink
  active" indicator top-right whenever it's on. Worth knowing: the diamond
  key already meant something else on the VIEW_APPS screen before this
  ("update all installed SD apps from the repo", `KEY_SCANCODE_DIAMOND` in
  that view's own switch-case) — no code conflict since the two are
  different `switch (ctx->current_view)` arms, but the same physical key
  now does two different things depending which screen is showing.
  Build-verified (firmware + `cj_launcher`, NAS docker + `idf.py sdk` +
  `apps/build.sh`), not yet flashed/tried on real hardware.
- **`tanmatsu-usb-msc` — kernel skeleton + launcher tile exist, storage
  itself still deliberately stubbed.** `badgevms/drivers/usb_msc.c`'s
  `usb_msc_activate()`/`usb_msc_deactivate()` always return `false` — see
  that file's own comment for why (esp_tinyusb's `tinyusb_msc.h` wants to
  own the SD card's FAT mount itself; BadgeVMS's own `fatfs.c` already
  mounts it at boot; getting that handoff wrong risks corrupting the card,
  not just crashing, and that's not something to guess at without a spare
  SD card to test against). `BV_USB_MODE_MSC` exists end-to-end in the app
  API and a small launcher-style tile app,
  `apps/cj_usb_msc` (why2025-apps repo, same shape as `cj_i2c_scan`),
  calls the real API and honestly shows "not implemented yet" rather than
  faking it. Build-verified, not wired to real storage.

**Build verification (2026-08-16, NAS `espressif/idf:v5.5.1` docker), two
fixes needed beyond the initial port:**
1. `esp_tinyusb` *did* resolve cleanly against this repo's IDF 5.5.1 pin (the
   open question item 1 originally flagged) — no version conflict, pulls in
   `espressif__tinyusb` itself as a transitive dependency.
2. First build failed: `usb_device.c:58:10: fatal error: esp_private/usb_phy.h:
   No such file or directory`. `esp_new_phy()`'s header lives in ESP-IDF's
   `usb` component, which nothing in `badgevms/CMakeLists.txt` had
   `PRIV_REQUIRES`'d before (it was only present transitively, via
   `esp_tinyusb`'s own deps, which doesn't propagate private-header include
   paths to *this* component) — fixed by adding `"usb"` to `PRIV_REQUIRES`.
3. Second build failed: `implicit declaration of function 'tud_vendor_write'`
   (and `_write_flush`/`_available`/`_read`). TinyUSB's vendor class is
   compiled out unless `CONFIG_TINYUSB_VENDOR_COUNT > 0` — nothing had ever
   set it, since this is the first vendor-class USB code in the tree. Fixed
   via `sdkconfig.defaults` (`CONFIG_TINYUSB_VENDOR_COUNT=1`); the RX/TX
   FIFO and endpoint sizes esp_tinyusb picks by default for ESP32P4 (512
   bytes) already match the high-speed endpoint size `usb_device.c` declares,
   so no further tuning was needed there.

After both fixes: clean build, both `badgevms.bin` and `network_adapter.bin`
produced, stack-usage gate green. **2026-08-16, later the same day: flashed
to a physical badge with BadgeLink forced on at boot (an early, since-removed
version of this code used a compile-time flag for that spike) — booted
clean, no crash around the mux/TinyUSB init.** Boot-verified, not yet
independently confirmed the bottom port enumerates as `16d0:0f9a` from a
second machine. The mode-switch is now app-driven (item 4, below) rather
than a boot-time flag.

## What `tanmatsu-usb-msc` would additionally need

[`tanmatsu-usb-msc`](https://github.com/Senna-chan/tanmatsu-usb-msc) (also
Senna-chan's; cloned locally at `_build-flash-test/why2025_usbmsc_app`)
builds against the same WHY2025 PHY/mux foundation as item 1 above — it uses
the identical `usb_new_phy()`/GPIO2 pattern, just registering a USB
Mass-Storage-Class endpoint (`tinyusb_msc_new_storage_*`) instead of
BadgeLink's vendor class. That part of its code hadn't been hardware-verified
as of this research (its `main.c` still carries an ad-hoc, un-upstreamed
`hosted_reset_slave_callback()` stub and was last touched by an AI-assisted
"fix more here and there" commit) — with today's mux confirmation it's
plausible rather than speculative, but still unverified on its own.

Porting it into DutchVMS means:
- Reusing the same TinyUSB+mux foundation as BadgeLink (item 1 above).
- Replacing the vendor-class descriptor with an MSC storage-class
  registration, backed by DutchVMS's own SD-card and flash-partition access
  instead of badge-bsp's storage APIs.
- **A three-way mode select**, not two-way: C6-debug (default) /
  BadgeLink-device / MSC-device all want the same physical USB slot, so only
  one can be active at a time — the mode-switch design in item 4 above needs
  to account for a third state, not just BadgeLink vs. debug.

## Recommended next step

Items 1-4 are code and build-verified; the boot-time BadgeLink-forced-on
spike is hardware-boot-verified (see above). Still open:
- Confirm the bottom USB-C port actually enumerates as `16d0:0f9a` from a
  second machine and `badgelink.py fs list /SD0` works, ideally triggered
  the real way now (diamond key on the launcher's HOME screen) rather than
  the removed boot-time flag.
- Flash the updated `cj_launcher`/new `cj_usb_msc` to a real badge and try
  the diamond key + the tile. **Don't deploy `cj_launcher.elf` over UART**
  for this (`docs/KNOWN_ISSUES.md` `#13` — known overrun risk for exactly
  this file) — use the app-repository publish flow or an SD-card copy
  instead, per `docs/guides/Flashing.md`.
- MSC: find/borrow a spare SD card, then design and verify the actual
  mount-ownership handoff between `fatfs.c` and `tinyusb_msc.h` before
  `usb_msc.c` stops being a stub.

## Today's Senna-launcher bug fixes — relevance to DutchVMS

Senna fixed four things in `senna_idf6_native`/`senna_idf551` on 2026-08-16
(see `docs/tanmatsu-launcher-port-analysis.md` for the general comparison).
Checked each against DutchVMS:

- **USB-mode-switch typo** (`pdTICKS_TO_MS` used instead of `pdMS_TO_TICKS`
  in `usb_device.c`, turning a 0.5s delay into 50s) — lives entirely in code
  DutchVMS doesn't have yet. Not applicable today; **relevant to remember**
  when porting item 1 above — use the correct macro from the start.
- **SD-mount fallback** (mount even when card-detect reports
  `ESP_ERR_NOT_SUPPORTED`, because WHY2025 has no SD-detect GPIO) — the
  underlying hardware fact (no SD-detect pin) is shared with DutchVMS, but
  DutchVMS's own SD mount path is separate code, not badge-bsp's. Worth a
  follow-up check of DutchVMS's own SD-mount logic for the same assumption,
  but not confirmed as the same bug — `KNOWN_ISSUES.md` `#10` (SD-card write
  corruption) is a different symptom (corrupted content, not a failure to
  mount) with no established link to this.
- **TCA8418 navigation-speed fix** (the IRQ-fallback-timeout was 1000ms) —
  not applicable: DutchVMS already avoids the interrupt entirely and polls
  synchronously (`badgevms/drivers/tca8418.c`'s `tca8418_keyboard_task`,
  100ms poll loop), which is the same conclusion Senna's fix converges on.
  See `tanmatsu-launcher-port-analysis.md` §2.
- **Keyboard double-fire** (press *and* release both triggering text entry in
  badge-bsp's input handler) — DutchVMS's own driver
  (`badgevms/drivers/tca8418.c:127-175`, `scancode_to_event()`) explicitly
  sets `event.type` to `EVENT_KEY_DOWN` or `EVENT_KEY_UP` based on the
  scancode's press/release bit and emits both as distinct events; nothing
  found there merges them into a single always-fire text-entry path. No
  matching symptom is recorded in `KNOWN_ISSUES.md` either. Treat as
  **not applicable**, though whichever code consumes these events downstream
  (compositor / app-facing text input) wasn't independently re-audited here.
