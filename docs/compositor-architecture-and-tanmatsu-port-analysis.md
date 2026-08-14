# Compositor rendering architecture, and a DutchVMS-on-Tanmatsu port — analysis & recommendations

Date: 2026-08-12. Author: analysis session (Claude Code), commissioned by CJ, prompted by Nicolai's
suggestion in the why2025-software forum thread ("will there be a port of DutchVMS for Tanmatsu?
... if you get that to run on top of the badge-bsp functions you might even be able to use one
source repo to build both versions").

## Summary

Two related questions, both grounded in source inspection (not inference from docs):

1. **What should DutchVMS's compositor rendering architecture be, long-term?** Today's session
   replaced the PPA-hardware compositor path with CPU-side rendering to route around an unresolved
   color bug, and that trade discovered a real, hardware-confirmed cost: CPU-bound rendering
   competes for scheduler time with app processes in a way PPA hardware offload never did. Multiple
   priority values were tried on hardware tonight (20 → froze/starved apps, 8 → still froze, just
   earlier; 5 → the only value confirmed safe). See §1.
2. **Is a DutchVMS port to Tanmatsu, on top of `badge-bsp`, actually feasible?** Yes, with real
   supporting evidence, but it's a substantial project, not a quick add-on. See §2.

**Headline recommendation:** treat the color bug as the thing to actually fix (not route around),
and treat the Tanmatsu port as a real but separately-scoped initiative — not something to fold into
routine bug-fixing sessions. Both are detailed below with the evidence behind them.

## 1. Compositor rendering architecture

### 1.1 What today established, hardware-confirmed

- The PPA-removal CPU-rendering rewrite (this session) fixed the original motivation (a real,
  unresolved value-dependent RGB565 color glitch — static "stripe" at blue=247, flicker for
  orange/amber hues) by removing the whole hardware path that bug lived in.
- That same rewrite makes `compositor()` (`badgevms/compositor/compositor.c`) do real,
  non-trivial CPU work every frame (`blit_rect_rotated()`, `pixel_functions.c`) instead of mostly
  waiting on PPA hardware to finish. The task's original priority (20) and core pinning (0) were
  never a problem before today because the task barely touched the CPU — confirmed by `git log -p
  -S "create_kernel_task(compositor"` showing that exact call, unchanged, since the file's original
  creation commit.
- Priority 20 pinned to core 0 (unchanged from before) starved
  `badgevms/drivers/led_matrix_pca9698.c`'s `mtx_refresh_task_hw` — that task has no `vTaskDelay` in
  its per-row I2C loop and needs undisturbed ~460 Hz core-0 timing (see its own "no flicker"
  comment). Visible as the ping-pong animation breaking into a top-to-bottom flicker.
- Moving the compositor to core 1 (freeing core 0 for the LED task) fixed the LED flicker but
  shifted the *same class* of starvation onto app processes instead: priority 20 vs. app-process
  priority 5 (`task.c`'s `zeus()`) meant FreeRTOS never preempted the compositor for a lower-priority
  app no matter how long it ran. A busy screen (several windows queued) could freeze every app on
  core 1, including keyboard input — this is not a metaphor, it reproduced on hardware as a stuck
  launcher spinner with dead keyboard input.
- Priority 5 (equal to app processes) fixed that: FreeRTOS's equal-priority round-robin
  time-slicing actually interleaves the two instead of one dominating. Hardware-confirmed stable —
  launcher starts, stays running, responds to input, across multiple reboots.
- **Priority is not a smoothly-tunable knob for this problem.** Tried priority 8 (a "modest gap"
  compromise) as an explicit experiment: reproduced the starvation on hardware, just manifesting as
  a black screen (stuck before the first frame drew) rather than a static spinner — the timing
  differed, the underlying mechanism (strictly-higher-priority task can't be preempted by a
  lower-priority one, regardless of how small the numeric gap is) did not. Any priority `> 5`
  reintroduces the same risk; only `== 5` (or an actual internal-yield redesign, not attempted) is
  confirmed safe.

### 1.2 Why Tanmatsu-launcher doesn't have this problem

Read directly from `Nicolai-Electronics/tanmatsu-launcher`'s source (not inferred): its rendering
calls (`display_get_buffer()` / draw calls / `display_blit_buffer()`) happen **inside `app_main()`'s
own loop** (`main/main.c`), not in a separate compositor task. Background things (WiFi, LoRa, audio
mixer, `plugin_service_task`) do run as their own FreeRTOS tasks, but the foreground UI — the thing
CJ was comparing our sluggishness against — is the main task itself. There is no separate
high-priority renderer that a separate app task could contend with, because there's only one task
doing both jobs.

That's also confirmed to be CPU-side rendering, not PPA: Tanmatsu's stack (`tanmatsu-launcher`,
`badge-bsp`, `pax-gfx`) has zero use of `driver/ppa.h`/`ppa_do_scale_rotate_mirror()` anywhere —
already established in `docs/tanmatsu-launcher-port-analysis.md` §5. So the smoothness isn't "PPA
hardware" (wrong theory, corrected mid-session) and it isn't "some clever scheduling trick" either —
it's architectural: a single-app, single-task design has no cross-task rendering-vs-app-logic
scheduling conflict to have in the first place.

### 1.3 What this means for DutchVMS

BadgeVMS's multi-process design (separate compositor task arbitrating between independent app
processes, each its own FreeRTOS task) is not a mistake to unwind — it's the reason an app crash
doesn't take the launcher down with it, and it's why several independent apps can exist as
separate, isolated processes at all. That is a real, deliberate trade against Tanmatsu-launcher's
single-monolithic-app model, and it's a trade DutchVMS should keep. But it means DutchVMS can never
get Tanmatsu-launcher's specific kind of "free" smoothness (no separate task, so no scheduling
conflict possible) without giving up process isolation. The actual available paths, in order of
soundness:

1. **Fix the PPA color bug instead of avoiding it — investigated, lead did not hold up.**
   [esp-idf#17531](https://github.com/espressif/esp-idf/issues/17531) was read in full (issue body +
   both comments, not just the title) and turned out to be a weaker lead than the earlier
   `docs/tanmatsu-launcher-port-analysis.md` §5 summary suggested:
   - The bug it documents is the PPA SRM engine's **integer-upscaling** interpolation math producing
     chromatic aberration (pure `0xffff`/`0x0000` test-pattern pixels corrupting to values like
     `0xe739`/`0xf7bd`) — nothing in the issue body or either comment mentions `rgb_swap`. That was
     this document's own inference, written before the issue's actual content was read, and it does
     not hold up.
   - A commenter (BitsForPeople) links a now-bot-walled esp32.com forum thread captioned "seems to be
     a hardware thing" — i.e. this is reported as a **silicon limitation**, not a driver/software bug
     with a code-level fix. The other commenter (demik, the OP) accepted that and shipped a
     workaround: stop using PPA for scaling entirely, keep it for rotation only, do scaling in
     software.
   - Critically, that mechanism may not even apply to our bug: WHY2025's compositor always calls PPA
     with `rotation = ROTATION_ANGLE_270` (`compositor.c:87`, hardcoded, unconditional) but `scale =
     fminf(scale_x, scale_y)` where `scale_x`/`scale_y` come from `window->rect` vs.
     `framebuffer->w/h` (`compositor.c:669-671`) — for the fullscreen, native-resolution case the
     original stripe/flicker bug was observed in (the launcher), those are equal, so `scale == 1.0`:
     no actual upscaling, just an exact-pixel 90°-family rotation remap. esp-idf#17531's described
     mechanism (interpolation artifacts from resampling) has no obvious reason to fire on a
     rotation-only, unscaled blit. So even setting the `rgb_swap` misreading aside, this specific
     issue's fix (avoid PPA scaling) targets a code path (`scale != 1.0`, e.g. non-native-resolution
     windows) that isn't the one the originally-reported bug ran on.
   - **Conclusion:** esp-idf#17531 does not stand up as an explanation for our stripe/flicker bug on
     closer reading, and should not be treated as a lead anymore. Isolating `rgb_swap` (setting it
     `false` and doing that channel swap in software instead, keeping PPA for scale/rotate/blit)
     remains a legitimate, independent thing to try — it's a real per-pixel hardware operation with
     its own potential bit-level bug — but it is no longer backed by a matching public report; it
     would be a genuine from-scratch hardware experiment (build, flash, compare against the known
     stripe/flicker repro), not "apply a documented fix." No further public lead is currently known.

   - **Update 2026-08-12, hardware-tested, negative result:** the `rgb_swap` isolation experiment
     above was built and flashed (`CJ_BADGEVMS_COMPOSITOR_USE_PPA=y` +
     `CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP=y` — the CPU pre-swaps R/B channels into a scratch
     buffer and tells PPA `rgb_swap=false`, see the Kconfig option's help text and
     `compositor.c`'s `copy_swap_rb_rect()`). Confirmed the launcher's window
     (`sdk_apps/badgevms_launcher/main.c:357`) uses `BADGEVMS_PIXELFORMAT_RGB565`, so this *did*
     exercise the swap path. **Result: the bug is unchanged** — CJ reports the same two-tone
     splitting on bright orange and the same broken "dot" tiles as before. This rules out PPA's
     hardware `rgb_swap` bit as the cause.
   - As a side effect, this same experiment also ruled out a second, independent hypothesis: the
     original PPA path (`compositor.c`, pre-experiment) never calls `esp_cache_msync()` on the
     app-owned input framebuffer (`framebuffer->framebuffer.pixels`) before handing it to PPA as a
     DMA source — only the *output* framebuffers get cache maintenance calls
     (`ESP_CACHE_MSYNC_FLAG_DIR_M2C`/`C2M` at various points in `compositor.c`). That's a real,
     independently-justifiable correctness gap (DMA hardware reading CPU-written memory without a
     flush is a latent bug on its own merits, cache-coherency-related color corruption would also
     plausibly look like a stable stripe + timing-dependent flicker). The `rgb_swap`-isolation
     experiment's scratch-buffer path happens to *always* flush
     (`esp_cache_msync(rgb_swap_scratch, ..., ESP_CACHE_MSYNC_FLAG_DIR_C2M)` before the PPA call) —
     so this experiment incidentally tested "properly cache-flushed input" too, for every window
     that hits the swap path (i.e. every `RGB565`/`BGRA8888`/`ARGB8888` window, which covers the
     launcher). Since the bug was still unchanged, **missing input cache-sync is also ruled out** as
     the (sole) explanation — though the underlying gap is still real and arguably worth fixing on
     general correctness grounds whenever the PPA path is next touched, independent of this bug.
   - **Update 2026-08-13, third hypothesis, also hardware-tested, also negative:** since the
     `rgb_swap` experiment above conflated two changes at once (software swap *and* an incidental
     cache flush), the cache-flush variable wasn't cleanly isolated. Built and flashed a clean
     isolated test: `CJ_BADGEVMS_COMPOSITOR_USE_PPA=y` with hardware `rgb_swap` completely
     untouched (exactly as the original path), plus *only* the missing
     `esp_cache_msync(framebuffer->framebuffer.pixels, ..., ESP_CACHE_MSYNC_FLAG_DIR_C2M)` added
     before the PPA call (`CJ_BADGEVMS_COMPOSITOR_PPA_INPUT_CACHE_FLUSH`, since folded into
     unconditional behavior — see below). **Result: bug unchanged again** — same orange
     splitting, same broken dot-tiles. Cache-coherency is now cleanly ruled out too, not just
     incidentally.
   - **Where this leaves things:** three independent, concrete hypotheses (esp-idf#17531's
     scaling mechanism, PPA's hardware `rgb_swap` bit, and input cache-coherency) are now all
     hardware-tested and excluded. No further public lead or static-analysis lead is known;
     further progress would need real register-level/JTAG hardware debugging, not another
     black-box software guess. **CJ's decision 2026-08-13: stop the PPA color-bug investigation
     and keep PPA enabled anyway**, accepting the unresolved visual bug as a known limitation in
     exchange for PPA's speed over the CPU-render path's real scheduling costs (§1.1). Shipped:
     `CJ_BADGEVMS_COMPOSITOR_USE_PPA` default flipped to `y`; the cache-flush fix stays in
     unconditionally (it's correct regardless of not fixing this bug, no reason to gate it behind
     a diagnostic flag); `CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP` stays available but off (ruled
     out, kept only in case it's worth revisiting). Next: CJ redirected effort to scoping a
     badge-bsp switch (§2) instead of continuing this investigation.
2. **Give the CPU render path real internal yield points**, if (1) turns out not to be viable. Not
   attempted this session — the fix applied was priority tuning, not restructuring
   `compositor()`'s per-window blit loop to periodically yield or chunk its work so it doesn't need
   an inherently-unsafe priority to feel responsive. This keeps CPU rendering as the permanent
   default (not just an escape-hatch fallback) at the cost of real engineering effort inside the
   render loop itself, and it's the only path that doesn't depend on ever getting PPA working.
3. **Keep the current state (priority 5, CPU path default, `CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA=y`
   as escape hatch)** as the pragmatic today-position, but not the intended end state — the "wat
   trager" (bit slower) feel CJ already reported is the visible cost of this compromise, and it's
   the one CJ is actively living with right now.

### 1.4 Full-screen color sweep test (2026-08-14) — refutes the B=247 threshold, workarounds confirmed safe

Built a diagnostic app (`why2025-apps/apps/cj_colortest`) that fills the whole screen with one
solid color at a time (3s each, hex code overlaid), cycling through ~34 colors: an amber/orange
sweep, a fine blue sweep centered on the previously-suspected B=247 threshold, a purple/lilac
sweep, and green/red/cyan/white/black controls. CJ filmed the badge running it and the video was
analyzed frame-by-frame (ffmpeg 1fps extraction). Because this app does a flat full-screen fill
with no PPA and no scaling — the simplest possible render path — this isolates the bug from
almost everything in §1.1–1.3 at once.

**Result table** (✗ = static horizontal banding visible, ✓ = clean solid fill):

| Color | Hex | R,G,B | Result |
|---|---|---|---|
| amber (task #21 workaround) | `E0AF68` | 224,175,104 | ✓ |
| dark orange | `CC6600` | 204,102,0 | ✗ |
| orange | `FFA500` | 255,165,0 | ✗ |
| bright orange | `FF7F00` | 255,127,0 | ✗ |
| pastel orange | `FFB347` | 255,179,71 | ✗ |
| light amber | `FFC080` | 255,192,128 | ✓ |
| goldenrod | `B8860B` | 184,134,11 | ✗ |
| deep orange | `FF4500` | 255,69,0 | ✓ |
| workaround blue (B=226) | `4A90E2` | 74,144,226 | ✓ |
| matte blue | `6B8CAE` | 107,140,174 | ✓ |
| sweep B=230..255 (`7AA2E6`…`7AA2FF`, 7 values incl. known-bad `7AA2F7`) | | R=122,G=162 fixed | ✗ (all 7) |
| pure blue | `0000FF` | 0,0,255 | ✓ |
| known-bad lilac | `BB9AF7` | 187,154,247 | ✗ |
| purple | `9D7CD8` | 157,124,216 | ✓ |
| dark purple | `6A4C93` | 106,76,147 | ✓ |
| green / red / cyan / white / black (all controls) | | | ✓ (all) |

**Findings:**

1. **Not hue-family dependent.** "All oranges" and "all blues" is too coarse a description —
   `FF4500` (deep orange) and `0000FF` (pure blue) are both completely clean, while `CC6600` and
   `7AA2F7` (same rough hue family) show severe banding. The bug fires on specific RGB565 values,
   not on a color family.
2. **The B=247 threshold theory is refuted.** The `7AA2xx` sweep varied *only* the blue channel
   from 230 to 255 (7 values, including the original known-bad `0x7AA2F7`) with R/G held fixed —
   **all seven showed identical, equally severe banding**, including B=230, well below the
   supposed 247 threshold. The original bug report's "B=247" was almost certainly just the
   specific tile color the launcher happened to use, not a real hardware/software threshold on
   the blue channel. Any future investigation should stop treating B=247 as a meaningful number.
3. **No RGB565-bit-truncation pattern found.** Compared the 5/6/5-bit-truncated values and
   discarded LSBs of every tested color looking for a shared rule (e.g. "G's low 2 bits ≠ 0") —
   nothing separates the ✗ set from the ✓ set cleanly. This makes a simple arithmetic/rounding bug
   in our own RGB565→panel conversion code less likely as the sole explanation.
4. **The existing task #21/#19 workaround colors are confirmed clean on real hardware by this
   test**: `E0AF68` (amber) and `4A90E2` (workaround blue) both rendered perfectly. The
   substitute-safe-colors strategy already shipped across the app suite is empirically validated,
   not just theoretically reasonable — keep relying on it while the root cause is chased further.
5. **Working hypothesis, unconfirmed:** given (a) this reproduces on the simplest possible render
   path (flat fill, no PPA, no scaling — so §1's three ruled-out PPA hypotheses stay ruled out),
   (b) the artifact is static horizontal banding (every-other-row-ish), and (c) it's tied to
   specific values rather than a hue family or a clean bit rule, the two remaining plausible
   explanations are: (i) the ST7703 panel's own color depth is lower than 16-bit RGB565 and it
   dithers/FRC-quantizes internally to fake full color depth — some input values could land on
   quantization boundaries that make the panel's dither pattern become visible as static banding
   instead of temporally-invisible noise; or (ii) a PSRAM/DSI-DMA signal-integrity issue where
   specific repeating bit-toggle patterns in the pixel stream hit an unfavorable timing margin on
   the framebuffer-to-panel DMA path — this would revive the *original* pre-PPA-focus hypothesis
   from `why2025-apps` commit `b4eedee` ("Likely a PSRAM/DMA bandwidth issue"), which was set aside
   when investigation shifted to PPA-specific theories that are now all ruled out. Neither is
   confirmed; both predict different outcomes from the Mountain-vs-Bono screen-type experiment
   (§2.4) that's the planned next step — a different panel-timing config changing which values
   band would support (i), no change would support (ii) since PSRAM/DMA timing isn't
   panel-dependent.

No software fix is being proposed yet — this session's goal was characterizing the bug precisely
enough to make the next hardware experiment (panel timing swap) diagnostic rather than another
blind guess.

### 1.5 Root cause found and fixed (2026-08-14): MIPI DSI lane signal integrity, not PPA/PSRAM/panel

Four further hardware experiments, run in sequence on CJ's badge, isolated the actual cause:

1. **Screen-type swap (`CONFIG_SCREEN_TYPE_MOUNTAIN` vs `_BONO`)** — different ST7703 init table,
   different DPI pixel clock (58MHz vs 47MHz), different porches. Along the way this surfaced a
   real, previously-unknown bug: the Mountain branch's DPI config macro in
   `badgevms/drivers/st7703.h` had never compiled (`.num_fbs = DISPLAY_FRAMEBUFFERS.video_timing =`
   — two struct fields merged onto one line with a missing comma, meaning nobody had ever
   build-tested `SCREEN_TYPE_MOUNTAIN`). Fixed that syntax bug, rebuilt, flashed. **Result: byte-
   identical banding** — same colors band, same colors stay clean, regardless of DPI clock/porch
   timing. Rules out panel-DPI-timing and the panel's own dithering-vs-timing-frequency theory.
2. **PSRAM speed (`CONFIG_SPIRAM_SPEED_200M` → `_80M`, Hex mode)** — the project's PSRAM ran at the
   fastest, most electrically demanding setting available. **Result: unchanged.** Rules out PSRAM
   read timing margin / SI on the SoC-internal PSRAM bus.
3. **Physical connector flex test** — gently pressed/flexed the display FPC connector (the one
   community members flagged as a stress point, one had epoxied theirs) while showing a known-bad
   color. **Result: no visible change.** Weakens (doesn't fully rule out) a loose/marginal
   mechanical connection at that specific connector.
4. **MIPI DSI lane bitrate (`LCD_MIPI_DSI_LANE_BITRATE_MBPS` 1000 → 700 Mbps)** in
   `badgevms/drivers/st7703.c` — this is the SoC-to-panel DSI signal path (2 data lanes), a
   completely different bus from PSRAM. **Result: fixed.** The stripe/flicker artifact from
   `0x7AA2F7`-class colors is gone at 700 Mbps. This is a genuine root cause, not a workaround: the
   original 1000 Mbps setting was simply too fast for this specific display cable/connector's
   signal integrity, corrupting a value-dependent subset of pixel data in transit.

**Fixed, shipped:** `LCD_MIPI_DSI_LANE_BITRATE_MBPS` in `badgevms/drivers/st7703.c` lowered from
1000 to 700. 700 was chosen as a conservative first safe value (not bisected to find the actual
maximum safe rate) — bandwidth headroom above the panel's minimum requirement is still comfortable
at 47MHz DPI clock (~1.86x vs. the naive content-only minimum), so there's likely room to tune
this higher later if render throughput ever becomes a bottleneck; not attempted this session since
CJ confirmed 700 already resolves the visible bug.

**Not fixed by this change — separate, pre-existing issue:** a "dots" artifact on the two left
Home-screen tiles is still visible, unrelated to this fix (confirmed: banding is gone, dots
persist). This is tracked separately, see GitHub issue #64 — do not conflate the two; this
session's evidence argues *against* the "possible unification" theory floated in #65's original
writeup (the two symptoms have independent root causes, since fixing one didn't touch the other).

This closes the long-open GitHub issue #65 (`Color-value-dependent PAX/PPA render glitch`). The
existing app-level color-substitution workarounds (task #19/#21 in `why2025-apps`) can eventually
be removed now that the underlying display bug is fixed, but that's a separate follow-up, not
done as part of this fix.

## 2. DutchVMS-on-Tanmatsu port feasibility

### 2.1 What's actually there (verified by cloning and reading source, not guessing)

- **`badgeteam/esp32-component-badge-bsp`** is a real, existing hardware abstraction component with
  dedicated implementations for both `targets/why2025/` and `targets/tanmatsu/` (also `mch2022`,
  `heltecv3`, `hackaday2025`, `esp32-p4-function-ev-board`, and others) — this is exactly the
  component Nicolai's suggestion refers to, not a hypothetical.
- Its `bsp/display.h` API (`bsp_display_get_parameters()`, `bsp_display_blit()`,
  `bsp_display_get_backlight_brightness/set_backlight_brightness()`, rotation, tearing-effect mode)
  is a clean, target-agnostic surface that BadgeVMS's own `badgevms/drivers/st7703.c` +
  `compositor.c`'s direct `lcd_device->_draw()` call could plausibly sit behind, if `st7703.c` were
  rewritten as a thin wrapper over `bsp_display_*()` instead of talking to the MIPI-DSI panel
  directly. Same shape applies to `bsp/input.h` (keyboard) vs. `badgevms/drivers/tca8418.c`, and
  `bsp/i2c.h` vs. `badgevms/drivers/badgevms_i2c_bus.c`.
- **Both P4-side targets are the same chip**: `tanmatsu-launcher`'s own `sdkconfigs/tanmatsu` and
  `sdkconfigs/esp32-p4-function-ev-board` both set `CONFIG_IDF_TARGET="esp32p4"` — same silicon,
  same toolchain, same instruction set as WHY2025's P4. No cross-architecture porting problem, only
  a peripheral/driver-abstraction one.
- **Real, meaningful display difference**: Tanmatsu is 800×480 (landscape, ST7701S controller,
  confirmed via Nicolai Electronics' own shop listing and `docs.tanmatsu.cloud/hardware/specifications`)
  vs. WHY2025's 720×720 (square, ST7703). Different aspect ratio, not just resolution — this is not
  a constant-swap, it's a real UI-layout consideration. BadgeVMS's compositor currently hardcodes
  `FRAMEBUFFER_MAX_W`/`FRAMEBUFFER_MAX_H` (720×720) in multiple places (`pixel_functions.c`'s
  `rotate_coordinates()`, the PPA workaround math in `rect_math.c`, window/tile layout assumptions
  in `cj_launcher`'s own UI code) — all of that would need to become a runtime value queried from
  `bsp_display_get_parameters()` rather than a compile-time constant, and any app/launcher UI that
  assumes a square screen (tile grids, centered layouts) would need real per-target layout work, not
  just a recompile.
- **C6 side already has a lower-effort path**: this repo's own `docs/tanmatsu-launcher-port-analysis.md`
  §3 already recommends evaluating a switch from our own `connectivity_esp_hosted/slave` fork to
  `Nicolai-Electronics/tanmatsu-radio`'s upstream-merged WHY2025 target — if DutchVMS moved to that
  radio firmware for its own sake, it would *also* be the same C6 firmware a Tanmatsu port would
  want, for free. This is a real point of leverage between the two initiatives, not a coincidence to
  ignore.
- **What would NOT carry over directly**: WHY2025-specific peripherals with no Tanmatsu equivalent —
  the LED matrix (`led_matrix_pca9698.c`, a WHY2025 add-on board Tanmatsu doesn't have) most
  obviously. Code that touches those would need to become conditionally-compiled or simply absent on
  a Tanmatsu build, the same way `badge-bsp` itself handles per-target feature availability.

### 2.2 What this is not

This is not a "swap a few `#define`s" job. It's rewriting BadgeVMS's entire hardware driver layer
(`badgevms/drivers/*`) to sit behind `badge-bsp`'s abstraction instead of talking to WHY2025's
specific peripherals directly, plus making the compositor's framebuffer geometry a runtime value
instead of a compile-time constant, plus real per-app UI-layout work for the aspect-ratio
difference, plus (optionally, but likely worthwhile) the C6 radio-firmware convergence from
`docs/tanmatsu-launcher-port-analysis.md` §3. Each of those is independently a real, multi-session
piece of work; doing all of them is a proper project, not a task.

### 2.3 Recommendation

**Worth pursuing, not worth starting opportunistically.** The technical case is genuinely sound —
same CPU, a real and already-proven abstraction layer, and a concrete point of leverage with the
already-planned C6 radio migration — which is more supporting evidence than most "should we port
this" questions get. But it should be scoped as its own initiative with a small first step, not
folded into ordinary bug-fixing sessions the way today's compositor work organically expanded to
cover LED matrix, launcher crashes, and version tagging all at once. Suggested first step, if CJ
wants to pursue this: get *only* the display driver rebased onto `bsp_display_*()` for the WHY2025
target first (prove the abstraction works and nothing regresses on hardware we can already test),
before ever attempting an actual Tanmatsu build. That mirrors how the PAX/LVGL evaluation in
`docs/pax_lvgl_design_proposal.md` was structured — isolated prototype first, integration decision
after there's hardware evidence, not before.

### 2.4 The display-driver-first step, in detail (2026-08-13)

§1's PPA color bug is now a closed, accepted trade-off (§1.3), not something CJ wants more
software-experiment cycles spent on. That reframes §2.3's suggested first step: it's no longer
just "the cheapest way to start the Tanmatsu port," it's *also* the only concrete remaining path
that could eliminate the PPA bug entirely (by removing PPA from the picture, not by fixing it) —
worth scoping properly rather than as a side effect of other work. Researched by cloning and
reading `badgeteam/esp32-component-badge-bsp` at HEAD (commit `9037f0e`), not inferred:

**What `targets/why2025/badge_bsp_display.c` actually does:**
- Zero PPA usage anywhere in the repo (`grep -rniE "ppa|PPA_OPERATION"` returns nothing). It's a
  plain `esp_lcd_panel_draw_bitmap()` blit over MIPI-DSI, gated by a flush semaphore from
  `on_color_trans_done`. Uses the DSI panel driver's own internal DMA2D for color-format
  conversion during transfer — a different, unrelated hardware block from the P4's PPA, so this
  genuinely would not carry the bug forward.
- Same panel IC family as WHY2025's known hardware: ST7703 via `esp_lcd_new_panel_st7703()`
  (espressif/esp_lcd_st7703 v2.0.2), MIPI-DSI 2-lane, 1000 Mbps, 720×720 RGB888, 47 MHz DPI. The
  init command table (`why2025_lcd_init_cmds.h`) should be diffed against DutchVMS's own
  `badgevms/drivers/st7703.c` init sequence before trusting it's the identical physical panel —
  not yet done.
- **Single-framebuffer only.** `bsp_display_configuration_t.num_fbs` exists in the public struct
  but the WHY2025 implementation hardcodes `num_fbs = 1` regardless — no double-buffering support
  as shipped. DutchVMS's compositor currently double/triple-buffers
  (`framebuffers[DISPLAY_FRAMEBUFFERS]` in `compositor.c`); dropping to single-buffered would need
  its own tearing/flicker evaluation on hardware, independent of the PPA question.
- **No compositor concept at all** (`grep -rniE "compositor|window_manage|layer"` across `bsp/` and
  both targets: nothing). `bsp_display_blit(x, y, x_end, y_end, buffer)` is the entire drawing API
  — DutchVMS's own window/damage-tracking compositor logic (`compositor.c`'s window stack, visible-
  region math, `rect_math.c`) would sit above it unchanged in spirit, just targeting a different
  final blit call than today's `lcd_device->_draw()`.
- **A real bug to fix or flag first**: `common/badge_bsp_device.c` calls
  `bsp_display_initialize(&configuration->display)` unconditionally for every target, but the
  WHY2025 target's own `bsp_display_initialize()` takes `void` — a signature mismatch at current
  upstream HEAD. This would need fixing (either upstream via a PR, or patched in a vendored copy)
  before any integration attempt compiles, let alone runs.
- Tanmatsu's equivalent driver differs in shape (config-struct constructor, real `num_fbs`/TE-pin/
  backlight-coprocessor support WHY2025's stub lacks) — the two targets are not drop-in identical
  even within badge-bsp itself, so "one shared driver" still means two real per-target
  implementations behind one shared *API*, not one shared *implementation*.
- Licensing: badge-bsp is MIT, DutchVMS is GPL-3.0 — MIT-into-GPL-3.0 is a standard, uncomplicated
  inclusion, no conflict.

**Pros / cons of switching WHY2025's display driver to badge-bsp:**

| | Badge-bsp switch | Stay on current custom driver + PPA-default |
|---|---|---|
| PPA color bug | Gone — different hardware path entirely, not routed around | Stays, accepted as a known trade-off (§1.3) |
| Rendering performance | CPU/DMA2D blit only, no hardware scale/rotate/mirror — likely closer to today's CPU-render path's cost profile than to PPA's, not yet benchmarked | PPA-accelerated, fastest path measured so far |
| Double-buffering | Not supported as shipped (`num_fbs` hardcoded to 1) — tearing/flicker risk unverified on hardware | Already double/triple-buffered, working |
| Code shared with Tanmatsu port | Real — same abstraction API, most of the leverage §2.1–2.3 already identified | None — stays WHY2025-only |
| Upstream maintenance | Rides Nicolai's/community fixes to badge-bsp for free going forward | Fully owned, fully understood, no external dependency risk |
| Immediate blockers | Signature mismatch in `badge_bsp_device.c` must be fixed/flagged before it even compiles; panel init sequence unverified against our exact hardware | None — already hardware-confirmed working today |
| Scope of change | Rewrites the driver layer (`badgevms/drivers/st7703.c` and friends) behind the compositor; compositor's own window/damage logic mostly unaffected | Zero — already shipped |
| Risk if it goes wrong | Real regression risk on a currently-working display path; needs isolated proof-of-concept before touching the shipping build | None — status quo |

**Phased plan, if pursued:**
1. **Proof of concept, isolated.** Add badge-bsp as a component in a throwaway branch/worktree, fix
   or locally patch the `bsp_display_initialize()` signature mismatch, write a minimal standalone
   test app (not integrated into DutchVMS's compositor yet) that calls `bsp_display_blit()` with a
   test pattern and confirms it renders correctly on real WHY2025 hardware — this alone validates
   the ST7703 init sequence matches our panel and that the ported driver works at all.
2. **Double-buffering check.** With single-buffering confirmed as the shipped WHY2025 behavior,
   deliberately test for tearing/flicker on hardware under realistic redraw load before deciding
   whether that's acceptable or needs upstream work to add real `num_fbs` support to the WHY2025
   target.
3. **Compositor integration.** Only after 1–2 pass: replace `compositor.c`'s `lcd_device->_draw()`
   call site (and `badgevms/drivers/st7703.c`'s direct panel access) with `bsp_display_blit()`,
   keeping all of `compositor.c`'s own window-stack/damage-tracking logic as-is above the new blit
   call. This is the point where the CPU-render vs. PPA question in §1 becomes moot — there's no
   PPA path to choose between anymore on this driver.
4. **Upstream the signature-mismatch fix** (step 1's blocker) as a PR to `badgeteam/esp32-
   component-badge-bsp`, regardless of whether DutchVMS ultimately adopts it — it's a real bug
   affecting any WHY2025-target user of that repo.

Not started — this is the plan CJ asked for, not yet executed. Given §2.3's own caution against
folding architecture changes into ordinary sessions, step 1 (isolated proof-of-concept) is the
right-sized next unit of work, not the full 4-step plan at once.

## 3. Follow-ups

- If CJ wants to reply to Nicolai's forum suggestion: the honest summary is "yes, feasible, `badge-bsp`
  is real and already supports both targets on the same chip — we'd want to start with the display
  driver alone before committing to a full port," not a firm yes/no.
- §1's PPA color bug investigation is closed (2026-08-13): three independent fix hypotheses
  hardware-tested and ruled out, no further lead known. §2.4's badge-bsp switch is now the single
  most actionable next step in this document — not because it fixes the PPA bug, but because it
  sidesteps it entirely by using different hardware, while also being real progress toward the
  Tanmatsu port §2 already recommends.
- §2.4's phased plan (step 1, isolated proof-of-concept) needs real hardware verification before
  being trusted, same caveat as every other compositor/driver claim in this repo — nothing in it
  has been built or flashed yet.

## See also

- `docs/tanmatsu-launcher-port-analysis.md` — the original Senna-chan port research this document
  builds on (hardware pin cross-validation, C6 radio-firmware opportunity, PPA-bug non-comparability
  finding).
- `docs/pax_lvgl_design_proposal.md` — same "isolated prototype before integration" methodology,
  applied to a different rendering-library decision.
