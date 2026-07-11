# Releases

The fixed format for every release of DutchVMS, so release entries and notes
always read the same way. This is the single source for how a release looks;
[CHANGELOG.md](../CHANGELOG.md) is where the entries live. Follow this exactly
rather than inventing per-release wording.

## Versioning

Semantic Versioning: `MAJOR.MINOR.PATCH`, tagged `vMAJOR.MINOR.PATCH`.

- **MAJOR** — a change that breaks interop or forces user action: a wire
  format change, an NVS/settings migration that drops old data, a required
  C6 co-processor firmware version, or a removed feature.
- **MINOR** — a new feature or a meaningful behaviour change that stays
  backward compatible.
- **PATCH** — bug fixes, robustness, docs, and internal work with no
  user-visible feature change.

When in doubt between two levels, pick the higher one. `PROJECT_VER` (shown
on the badge's About/panic screens) is read at build time from `git describe
--tags` (see the top-level `CMakeLists.txt`), not from a hardcoded define —
tagging the commit is what sets the version, nothing else.

## The CHANGELOG entry (the canonical format)

Every release is one section in `CHANGELOG.md`, newest first, in the Keep a
Changelog shape. Work in progress accumulates under `## [Unreleased]`; cutting
a release renames that heading to the version and date.

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- One feature per bullet, phrased as what the user gains.

### Changed
- One change per bullet, phrased as what is now different.

### Fixed
- One fix per bullet: the symptom, then the cause in a clause.

### Removed
- What is gone and what replaces it (or why it is gone).
```

Section order is fixed: **Added, Changed, Deprecated, Removed, Fixed,
Security.** Omit a section that has no entries for the release. Never reorder
them and never invent new section names.

## How an entry is written

- **English, plain, present tense for the result.** "Adds a Preset row to
  Settings", "Fixes the C6 length-prefix bug", not past-tense narration.
- **User facing first.** Lead with what the user sees or gains; put the
  mechanism in a trailing clause only when it helps.
- **One change per bullet.** Bold the feature name or subsystem at the start
  when it helps scanning: `**Auto radio start** — ...`.
- **Name the cause for fixes.** State the symptom, then the cause: "Adverts
  were never recognized because the request buffer silently rejected
  payloads over 64 bytes; ...".
- **No AI attribution** anywhere in an entry, a tag message, or a release
  note.
- **Reference a PR or issue** in parentheses when one exists: `(PR #14)`,
  `(GitHub issue #1)`.

## Cutting a release

1. Move the accumulated `## [Unreleased]` content in `docs/CHANGELOG.md` to a
   new `## [X.Y.Z] - YYYY-MM-DD` heading, then leave a fresh empty
   `## [Unreleased]` above it.
2. Make sure CI is green on the release commit (`.github/workflows/ci.yml`:
   clang-format + the ESP-IDF build + stack-usage check). Never cut a release
   from red.
3. Tag the commit and push the tag:
   ```bash
   git tag vX.Y.Z
   git push github vX.Y.Z
   ```
   This triggers `.github/workflows/release.yml`, which builds the firmware
   in the same `espressif/idf:v5.5.1` container CI uses and publishes a
   GitHub Release with `badgevms.bin` attached.
4. Edit the published GitHub Release notes to be the CHANGELOG section body
   verbatim (the workflow auto-generates notes from commit history by
   default — replace them), so the release notes and the changelog entry
   never drift.
5. Flash the release to a physical badge per
   [Flashing.md](Flashing.md#updating-an-existing-badge) and confirm it
   boots before calling the release done.

## Release note skeleton (copy this)

```
Title:  vX.Y.Z

<the CHANGELOG [X.Y.Z] section, verbatim>
```

## See also

- [CHANGELOG.md](../CHANGELOG.md) — the entries themselves
- [Flashing.md](Flashing.md) — first flash and update instructions
- [DUTCHVMS.md](../../DUTCHVMS.md) — what this fork changes vs upstream BadgeVMS
