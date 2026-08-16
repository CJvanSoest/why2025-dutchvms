# Tanmatsu-launcher port to WHY2025 (Senna-chan) — comparison & recommendations

Date: 2026-08-12. Author: analysis session (Claude Code), commissioned by CJ.

## Summary

Senna Hijlkema ([Senna-chan](https://github.com/Senna-chan)) got Nicolai Electronics' official
**Tanmatsu-launcher** running on WHY2025 hardware and published her forks + setup instructions
publicly. This document compares her port against DutchVMS (our BadgeVMS fork) to answer three
questions: what's reusable, does she have fixes for our known problems, and how does her IDF 6.0.2
stack compare to our IDF 5.5.1 pin.

**Headline finding:** Tanmatsu-launcher and DutchVMS are two completely separate OS/firmware
projects sharing only the physical board. Most of our BadgeVMS-specific bugs have no code
equivalent in her stack — that's an architecture fact, not a gap in this research. Where real value
exists: confirmed hardware pin data (cross-validation, not new information), and one genuinely
actionable opportunity — the C6 radio firmware.

## 1. Architecture: DutchVMS vs. Tanmatsu-launcher

| | DutchVMS (ours) | Tanmatsu-launcher (Senna's port) |
|---|---|---|
| Upstream | [BadgeVMS](https://gitlab.com/why2025/team-badge/firmware) (HP van Braam, GitLab) | [Nicolai-Electronics/tanmatsu-launcher](https://github.com/Nicolai-Electronics/tanmatsu-launcher) |
| OS model | Multi-process VMS-style kernel, own compositor | Single monolithic launcher app on top of ESP-IDF + `badge-bsp` |
| App model | PIE ELF `.elf` + `<uid>.json` manifest, VMS paths, own allocator (dlmalloc) | Tanmatsu's own app install/launch model, `badgelink`-deployed |
| Deploy | Custom UART `deploy_protocol.c` (magic-sentinel framing) or SD card | `badgelink` protocol + `make DEVICE=why2025 flashmonitor` |
| Rendering | Own compositor (`badgevms/compositor/compositor.c`), PPA hardware for scale/rotate/mirror | `pax-graphics` CPU renderer + direct MIPI-DSI DMA blit |
| C6 firmware | Own fork of `connectivity_esp_hosted/slave` | `Nicolai-Electronics/tanmatsu-radio`, WHY2025 target merged upstream |
| ESP-IDF | v5.5.1 (P4 and C6 both) | v6.0.2 (launcher/P4), v5.5.3 (radio/C6) |

Because the app model, allocator, deploy pipeline and compositor are all different code, most
symptom-level bugs we've hit don't have a comparable code path in her stack — see §6.

## 2. Confirmed hardware facts

The WHY2025 board is identical hardware regardless of firmware, so pin-level facts cross-validate
even though the two codebases are unrelated. Her `why2025_hardware.h` files (one per chip) give a
third independent source alongside our own KiCad tracing and Nicolai's earlier `tanmatsu-radio` PR.

- **Display/keyboard backlight** — `BSP_DISPLAY_BL=15`, `BSP_KEYBOARD_BL=10`. Matches our own KiCad
  trace exactly. Now confirmed by three independent sources (our KiCad, Nicolai's PR #24, Senna's
  fork).
- **Keyboard IRQ** — `BSP_KBD_INT=2` (GPIO2) on the P4 side (badge-bsp). Matches the "bridge INT_KEY
  to IO2" mod she described. Worth a quick check that our own TCA8418 keyboard init uses the same
  pin.

  ![Senna's hand-soldered TCA8418 interrupt patch wire](images/senna_tca8418_interrupt_mod.jpg)

  *Photo shows the patch wire soldered directly onto the shared generic IO-header pins, bridging the
  keyboard controller's INT line to GPIO2 — the physical side of the "bridge INT_KEY to IO2" mod
  referenced above. We did not replicate this hand mod: the real problem turned out to be the
  TCA8418 driver's 1000ms IRQ-fallback timeout, not a missing wire, and our own synchronous-polling
  keyboard task (`badgevms/drivers/tca8418.c`'s `tca8418_keyboard_task`) already sidesteps needing
  the interrupt at all — see our PR against
  [`esp32-component-tca8418#1`](https://github.com/Nicolai-Electronics/esp32-component-tca8418/pulls).
  Renze's own conclusion (add-on interference risk favors a fast poll over the physical interrupt
  line) independently matches that PR. Worth noting for anyone who *does* want the interrupt path:
  GPIO2 is the same pin Senna's `usb_device.c` drives as the BadgeLink USB-mux select
  (see [`docs/design/badgelink-usb-port.md`](design/badgelink-usb-port.md)) — the hand-soldered
  interrupt mod and native-USB device mode would contend for the same physical pin if both were
  wired up at once; not verified against her running firmware, but worth checking before combining
  the two. Source: Senna, shared via Discord, 2026-08-16.*
- **LoRa SPI (C6 side)** — `SCK=6, MISO=2, MOSI=7, CS=4, DIO1=5, BUSY=11, RESET=1`. **Identical** to
  our own pin table recorded during the Pad 1 work. Third independent confirmation.
- **C6↔P4 SDIO/host link (C6 side, new data point)**:
  ```c
  BSP_SDIO_CMD 18   BSP_SDIO_CLK 19   BSP_SDIO_D0 20   BSP_SDIO_D1 21   BSP_SDIO_D2 22   BSP_SDIO_D3 23
  BSP_HOST_INT 0    BSP_HOST_BOOT 9   BSP_HOST_TX 16   BSP_HOST_RX 17
  ```
  Not previously recorded on our side as an explicit table — worth a diff against our own
  `connectivity_esp_hosted`-side pin config.
- Not everything in her fork is finished reference material: her `badge-bsp` backlight function for
  WHY2025 (`badge_bsp_display.c`) is currently a no-op stub (`return ESP_OK`); actual brightness
  control landed elsewhere (the launcher branch), and a trailing `// WHY Specific things for
  backlight` comment with no code under it suggests unfinished work. Don't treat her repo as a
  polished reference for the display driver specifically.

## 3. Opportunity: align C6 radio firmware with `tanmatsu-radio` upstream

This is the most actionable finding in this document.

DutchVMS already has an internal initiative ("Pad 1") to move the C6 off our own
`connectivity_esp_hosted/slave` fork onto Tanmatsu's `lora_protocol_server` + esp-hosted v2.12.3, to
get WiFi and LoRa working simultaneously on the C6 (our current MeshCore setup sacrifices WiFi while
LoRa is active). Last known status (`pad1_esp_hosted_upgrade` session log): LoRa RX confirmed
working end-to-end against a real T-Beam; TX/full stability not yet finished.

**New information from this research:** `Nicolai-Electronics/tanmatsu-radio` now has an **official,
upstream-merged WHY2025 target** — [PR #24](https://github.com/Nicolai-Electronics/tanmatsu-radio/pulls)
by Senna-chan, merged 2026-08-01, described by her as "tested to be working on my badge at
BornHack." This is functionally the same firmware Pad 1 has been building toward, except maintained
by Nicolai Electronics rather than by us.

**Correction (2026-08-12, per CJ):** Nicolai accepted the WHY2025-target PR into `tanmatsu-radio`
but does not maintain that target himself going forward — keeping it working requires community
developers (including us, if we take this route) to submit their own follow-up PRs as upstream
`tanmatsu-radio` evolves. This doesn't invalidate the recommendation below, but it changes the
framing: switching to `tanmatsu-radio` trades *our* maintenance of a bespoke C6 fork for *our*
maintenance of a set of upstream PRs against someone else's repo — less existing burden to carry,
not "someone else now does this for us."

**Recommendation:** evaluate switching DutchVMS' C6 to track `tanmatsu-radio`'s WHY2025 target
directly instead of continuing to maintain our own `connectivity_esp_hosted/slave` fork. Bugs that
currently live in *our* vendored flash/flasher code — not in Nicolai's — could potentially go away
entirely:
- The `slave_c6_flasher.c` "stuck reflashing every boot" bug (see the C6 SDIO crash-loop and
  dual-chip NAS-flash session notes) is in our own flash-verify logic; it has no reason to exist if
  we stop maintaining that code path.
- Some of the C6 SDIO crash-loop root causes were in our own reflash-timing code, not stock
  ESP-Hosted behavior.

**Caveat:** our P4-side `badgevms/drivers/lora_proto_client.c` already speaks Tanmatsu's wire
protocol. Nicolai's `tanmatsu-lora` component has a real history of ABI breaks between client/server
versions (GET_CONFIG-asymmetry, missing-REQUIRES packaging bugs — see the `tanmatsu_radio_fork`
session log for the recurring pattern). Any move to track `tanmatsu-radio` upstream directly needs
the same lockstep-version discipline already used for the meshcore project, not a one-off pin bump.

This is a recommendation for a follow-up task, not a completed migration.

## 4. ESP-IDF 5.5.1 → 6.0.2: comparison and recommendation

Prior internal research (`Tanmatsu/IDF6-ELF-migratie-plan.md`, 2026-08-04) deliberately parked an
IDF 6.0 upgrade for two reasons: (1) the whole Tanmatsu ecosystem was still on IDF 5.5.x, and (2)
our vendored `ed25519.c` (meshcore) uses legacy `mbedtls_ecp_*` calls directly for X25519, which
Mbed TLS v4.0 (shipped with IDF 6.0) removes without a compatibility shim — a real, non-trivial
rewrite to PSA Crypto.

**New signal:** Nicolai's own `tanmatsu-launcher` upstream has moved to **IDF v6.0.2**
(commit `db589fd`, "Build with ESP-IDF v6.0.2"), and Senna's WHY2025 branch inherits that. However
the ecosystem move is **not uniform**: `tanmatsu-radio` (the C6 firmware) stays pinned to **v5.5.3**,
and `badge-bsp` is version-agnostic (`idf: >=5.3`).

Inspecting the actual `db589fd` migration diff: it's lightweight — an SDK-path/Makefile rework, a
`sys/dirent.h` → `dirent.h` header fix (a picolibc/newlib difference), dependency version bumps, and
two `-Wno-error=` flags for stricter default warnings. **No mbedtls/PSA rewrite, no legacy peripheral
driver changes** — because that codebase doesn't have custom crypto or legacy driver usage to begin
with. This means **it is not a validated precedent for our specific `ed25519.c` blocker** — we would
still be the first project in this hardware ecosystem to hit that wall.

**Recommendation:**
1. Treat this as a **partial**, not full, trigger of the migration plan's fase-2 condition
   ("upstream templates/BSP/radio move `IDF_BRANCH` to 6.x") — true for the launcher, not yet for
   radio/BSP.
2. Still don't migrate opportunistically. If/when we do: do the `ed25519.c` → PSA Crypto rewrite as
   an isolated, host-verifiable sub-project first (as the existing plan already recommends),
   independent of whether the rest of the P4 kernel side can build under 6.0.
3. Whichever C6 firmware we end up on (our own fork, or `tanmatsu-radio` per §3) should track its own
   upstream's IDF pin (currently 5.5.3) — don't get ahead of it on IDF 6.0.
4. `Tanmatsu/IDF6-ELF-migratie-plan.md` should get a short addendum noting this new signal; the
   overall "wait" recommendation stands.

## 5. Color/PPA render glitch — no comparable data, one useful external lead

Our unresolved PPA color bug (`why2025-ppa-color-render-glitch` — a static two-tone "stripe" for
certain colors and a time-varying flicker for orange/amber hues) lives in
`badgevms/compositor/compositor.c:720-782`, in the `ppa_do_scale_rotate_mirror()` call with
`rgb_swap=true` for RGB565.

Direct source inspection (not just search) of `tanmatsu-launcher`, `badge-bsp` (including Senna's
`why2025` branch), and `pax-graphics` found **zero use of the PPA hardware anywhere** in that stack.
Tanmatsu renders entirely via `pax-graphics`' CPU/software renderer and flushes directly to the
MIPI-DSI panel in RGB888 via `esp_lcd_panel_draw_bitmap()` — no `ppa_do_scale_rotate_mirror`, no
`driver/ppa.h`, no `rgb_swap`. `pax-graphics` has scaffolding for a future PPA-accelerated renderer
(`CONFIG_PAX_COMPILE_ESP32P4_PPA_RENDERER`) but zero actual driver code behind it — the
`esp_driver_ppa` CMake dependency line is commented out.

**Conclusion: her stack cannot exhibit this bug class, since it never exercises that hardware path.**
This neither confirms nor refutes our root-cause hypotheses — it just provides no data.

One useful external lead did turn up: [esp-idf#17531](https://github.com/espressif/esp-idf/issues/17531)
documents a confirmed, color-value-dependent RGB565 artifact bug in the same PPA SRM hardware/driver
(IDF v5.4.2/v5.5.1, ESP32-P4 rev 1.0) — a different trigger (integer upscaling producing a gray/yellow
tint at black/white boundaries) than ours, but it establishes that the PPA SRM color-conversion path
has real precedent for subtle, value-dependent bugs. Espressif's official chip errata document has no
PPA entry. No community reports of similar stripe/flicker artifacts were found in the
Nicolai-Electronics or badgeteam GitHub orgs.

**Suggested next step:** read the esp-idf#17531 thread for the engineers' root-cause explanation, and
consider whether avoiding the PPA's `rgb_swap` path (doing the channel swap in software instead, the
way Tanmatsu avoids the whole PPA color-conversion path) would sidestep the bug as a workaround.

## 6. PAX rendering reference

Tanmatsu-launcher is the original reference implementation of PAX on this hardware family. This is
relevant to our own open PAX/LVGL migration risk assessment (`why2025-multi-render-library-risk`),
specifically the "no shared PAX C++ build pipeline" blocker we identified. Worth a look at how
Nicolai's own build integrates PAX's C++ compile step as a reference point — not a direct port, just
inspiration for generalizing our own `build_app()` pipeline.

## 7. Not comparable — architecture-specific to DutchVMS

These known issues are specific to BadgeVMS' own architecture (dlmalloc, the PIE-ELF loader,
`deploy_protocol.c`, `slave_c6_flasher.c`) and have no equivalent code path in Tanmatsu-launcher's
badgelink-based flow. No comparison data exists to gain here — they remain our own work to solve:

- sbrk-shrink heap corruption (`why_sbrk()`)
- Launcher stack-overflow class of bug (`Launcher_Context` on stack)
- Manifest read-race (`application_list()`)
- OTA-confirm never executes (task #115)
- `deploy_protocol.c` PUT streaming / UART overrun
- C6-bundle-sync filename bug
- `_inbox` staging `flash_args` bug

## 8. Other notes

- **BadgeLink / USB-MSC over native USB — see the dedicated design doc.** 2026-08-16: re-testing the
  bottom USB-C port (not the CH340 side-port) with Senna's launcher confirmed a physical USB mux
  (GPIO2-selected) that reaches the P4's own native High-Speed OTG PHY — `16d0:0f9a "MCS WHY2025"`
  enumerated, `badgelink.py appfs list` worked immediately. This corrects the "native-USB pins not
  routed to any external connector" conclusion in [`.claude/Components.md`](../.claude/Components.md)'s
  former "Rejected: BadgeLink" note. Full porting analysis, required building blocks and open
  questions: [`docs/design/badgelink-usb-port.md`](design/badgelink-usb-port.md).
- **Licensing:** `tanmatsu-tadoom` (her DOOM port, forked from `nullislandspace/tanmatsu-tadoom`) has
  no license declared on GitHub despite CC0/MIT boilerplate text in the README. DOOM engine code is
  normally GPL-2.0. Relevant only if we ever look at reusing anything from that repo.
- **App model convergence:** Tanmatsu's separate ELF-applet experiment (`tanmatsu-template-elf`, see
  the IDF6 research doc) and BadgeVMS' PIE-ELF model are conceptually converging (small,
  independently-loadable binary next to a host stack/launcher) — not actionable, just worth noting.

## 9a. 2026-08-14 update: IDF 5.5.1 build attempt — hard dead end, not just a pin issue

Attempted §4's "try IDF 5.5.1 first, fall back to JTAG only if that fails" plan. Result: **structurally
blocked, confirmed via the component registry, not worth retrying with different pins.**

Relaxing `main/idf_component.yml`'s `idf: ">=6.0.2"` to `">=5.5.1"` on current WHY2025-branch HEAD
(`f92d3dc`) surfaced two real dependency-resolution failures in sequence:
1. `nicolaielectronics/tanmatsu-wifi` pinned `=1.3.1` requires `idf>=6.0.2` — downgrading to the
   pre-migration `^1.2.0` pin resolved this one cleanly.
2. `badgeteam/badge-bsp` pinned `=1.4.0` requires `idf>=6.0.2` — this one does **not** have a working
   downgrade. Checked the full badge-bsp version history via the component registry API
   (`https://components.espressif.com/api/components/badgeteam/badge-bsp`): `v1.0.5` (2026-07-22) is
   the last version targeting `idf>=5.3`; `v1.1.0` (2026-07-27) already jumped to `idf>=6.0.2`. Cross-
   referenced against badge-bsp's own git history: **WHY2025 board support was added in commit
   `5a10b20` ("Added WHY2025 as a target", 2026-07-31) — 4 days *after* the IDF6 bump, and is only
   included in tagged releases `v1.3.0`/`v1.4.0`, both already on `idf>=6.0.2`.** There is no
   badge-bsp version, at any point in its history, that supports both the WHY2025 target and
   IDF<6.0.2 — this isn't a pin that can be bumped, WHY2025 support in badge-bsp simply doesn't exist
   pre-IDF6.

**Why this happened**: badge-bsp's WHY2025 target is a fairly self-contained addition (`5a10b20`:
~1170 new lines, almost entirely new files under `targets/why2025/`, only +12 lines touching shared
`CMakeLists.txt`/`Kconfig`/`idf_component.yml`) — so it's not that WHY2025 support is deeply entangled
with IDF6-only core BSP code. More likely explanation: it was simply developed and released after the
IDF6 migration had already landed, so nobody had a reason to target the older IDF branch.

**Theoretical remaining option, not attempted**: manually backport the `targets/why2025/` files from
`5a10b20` onto badge-bsp `v1.0.5` as a local `override_path` component (the same pattern DutchVMS
already uses for `esp_wifi_remote`). Not pursued tonight — it's a materially bigger undertaking (fork
+ maintain a component, not a version pin) with its own open question: badge-bsp 1.4.0 pins
`espressif/esp_lcd_st7703: "=2.0.2"` for the WHY2025 display, and it's not yet confirmed whether 2.0.2
itself has its own IDF6-only requirement (DutchVMS's own working `esp_lcd_st7703` pin is `1.0.3`/
`1.0.4`, from before the 2.x line existed) — that would need its own investigation before backporting
badge-bsp is worth attempting.

**Conclusion**: the IDF 5.5.1 path is closed for now. The only way to get Senna's WHY2025 launcher
port running today is the original IDF 6.0.2 build with its silent-boot-hang problem, which needs
JTAG to debug further (see §4's original framing). No physical badge action was needed for this
attempt — it never got past dependency resolution on the NAS build, so nothing was ever flashed.

## 9. Follow-ups

- **BadgeLink/USB-MSC hardware-verification spike** — see
  [`docs/design/badgelink-usb-port.md`](design/badgelink-usb-port.md)'s "Recommended next step": port
  just the TinyUSB+GPIO2-mux init into a DutchVMS test build to confirm the P4 HS-OTG PHY enumerates
  the same way it does under Senna's launcher, before starting the larger protocol-glue work.
- Update the `why2025-upstream-nicolai-check` memory — it predates both the working (community,
  unofficial) WHY2025 launcher port and the partial IDF 6.0 move, and its "no launcher-layer support
  exists" conclusion is now stale.
- If CJ wants to, a short pointer to this document in the why2025-software forum thread Senna offered
  to host would close the loop with her — optional, only if explicitly requested.
