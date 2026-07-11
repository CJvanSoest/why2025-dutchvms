# Assets

Generated image assets for this repo, mirroring the pattern used in
[CJvanSoest/meshcore](https://github.com/CJvanSoest/meshcore)'s `assets/`:
a small script next to its output, so the image can be regenerated rather
than hand-edited.

- **`generate_logo.py`** → **`dutchvms-logo.svg`** — the DutchVMS logo used
  in [README.md](../README.md). Geometrically matches the real boot-splash
  windmill in `why2025-apps/apps/cj_launcher/main.c`'s `draw_splash_view()`
  (same sail/cap/body/balcony proportions, same `NEON_ORANGE`/`NEON_DIM`
  palette), captured as a single static frame at the splash animation's own
  "X-shape" starting rotation rather than the live per-frame pixel drawing
  the badge itself does.

Regenerate after changing it:

```sh
cd Assets && python3 generate_logo.py
```

Pure Python standard library, no dependencies.
