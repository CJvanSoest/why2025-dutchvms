# BadgeLink / tanmatsu-usb-msc over native USB — porting roadmap

Design document, 2026-08-16. Supersedes the "physically impossible" verdict
in [`.claude/Components.md`](../../.claude/Components.md)'s former "Rejected:
BadgeLink" note — see the correction below. Status: **plan, no code yet**,
same convention as [SD-and-OTA-Updates.md](SD-and-OTA-Updates.md) §1.

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

1. **A TinyUSB device-mode stack.** DutchVMS currently has none — the only
   USB components in the tree are *host*-mode (`espressif__usb_host_cdc_acm`,
   `usb_host_ch34x_vcp`, `usb_host_cp210x_vcp`). The PHY/mux bring-up in
   `usb_device.c` above (`usb_new_phy()` + `gpio_set_level(2, ...)`) is
   reusable as a direct reference for a new `badgevms/drivers/usb_device.c`.
2. **The `badgeteam__badgelink` managed component.** Already present in this
   workspace as a reference (vendored copy at
   `_build-flash-test/tanmatsu_radio_why2025/managed_components/
   badgeteam__badgelink/`) — nanopb protobuf + COBS framing, transport-agnostic
   (`badgelink_rxdata_cb()` in, a byte-send callback out). No changes needed
   to this layer itself.
3. **DutchVMS-specific protocol-handler glue — the actual new work.**
   Upstream's handlers (`badgelink_fs.c`, `badgelink_appfs.c`,
   `badgelink_nvs.c`, `badgelink_startapp.c`) are written against Tanmatsu's
   AppFS + app-launcher model, which DutchVMS doesn't have (BadgeVMS has its
   own kernel VFS — `why_fopen()` and friends in `wrapped_funcs.c` — and its
   own PIE-ELF process model in `task.c`). This needs a from-scratch mapping
   from BadgeLink's fs/nvs/start-app commands onto BadgeVMS's own APIs. The
   previously-removed `badgelink_transport_uart.c` (still readable at
   `_build-flash-test/firmware/badgevms/badgelink_transport_uart.c`) already
   solved part of this — avoiding `why_open()` in favor of lower-level calls —
   and is a usable starting point even though its UART transport itself is
   being replaced.
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

A small, low-risk hardware-verification spike: port *only* item 1 (TinyUSB
init + GPIO2 mux toggle, no protocol glue) into a DutchVMS test build and
confirm the P4's HS-OTG PHY enumerates over the bottom port the same way it
does under Senna's launcher. That validates the hardware path independently
of BadgeVMS's own kernel integration, before committing to the larger
glue-layer work in item 3.

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
