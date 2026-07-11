# Archive

Code that shipped with the original WHY2025 handout firmware (or an early
DutchVMS iteration) but is no longer built, installed, or relevant to this
fork. Kept for reference instead of deleted outright, since it documents a
real design decision or a past approach worth remembering. Nothing in this
directory is compiled — `sdk_apps/CMakeLists.txt` has no `build_app()` call
for any of it.

## sdk_apps/

- **`ota_wifi_update/`** — the original WHY2025 handout OTA updater. No
  longer builds against the current APIs (WHY team's own comment: "doesn't
  build anymore due to api changes"). Never re-enabled.
- **`why2025_ota/`** — WHY team's intended replacement for the above.
  Functional at the time it was disabled, but this fork drives firmware
  updates manually via `badge_deploy.py` + GitHub Releases instead (see
  `docs/guides/Releases.md`), so an on-device updater app was never needed.
- **`sponsor_app/`**, **`bme690_test/`** — orphaned example apps: present in
  the source tree but never referenced by any `build_app()` call, so they
  were already dead weight before this cleanup (not a WHY2025 → DutchVMS
  decision, just discovered stale). `bme690_test` was a standalone test for
  the BME690 env sensor driver; the driver itself
  (`badgevms/drivers/bosch_bme690.c`) is still live and used elsewhere, only
  this standalone test app was unused.

If you need to resurrect something here, move it back under `sdk_apps/` and
re-add its `build_app()` call in `sdk_apps/CMakeLists.txt`.
