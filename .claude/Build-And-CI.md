# Build and CI

How the firmware builds and what CI checks. The everyday change loop is in
[Workflow.md](Workflow.md).

## Local build

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build          # builds badgevms.bin (P4) AND drives the
                       # connectivity_esp_hosted sub-build (C6, network_adapter.bin)
idf.py sdk            # one-time — produces sdk_dist/ for the separate
                       # why2025-apps repo's own build.sh
```

Requires ESP-IDF 5.5 installed locally. After a `git pull`, run `idf.py
fullclean` before rebuilding so `sdkconfig.defaults` changes actually take
effect.

## Building without a local toolchain

Most of this fork's own history was built this way — a NAS-hosted Docker
image running the same `espressif/idf:v5.5.1` image CI uses, reached over
SSH with key-based auth, no password needed for `docker` itself:

```sh
# one-time: tar the repo over to the build host (rsync/scp may not work on
# every NAS setup — tar-over-ssh is the fallback that always does)
tar --exclude='build*' --exclude='.git' --exclude='sdk_dist' -cf - . \
  | ssh build-host "tar -xf - -C /path/to/build/dir"

ssh build-host "docker run --rm -v /path/to/build/dir:/project -w /project \
  espressif/idf:v5.5.1 bash -c '. \$IDF_PATH/export.sh; idf.py build'"
```

For a `CJ_BADGEVMS_ENABLE_BADGELINK=y` build (the second CI job), add
`-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci-badgelink"` to the
`idf.py build` invocation.

For clang-format (CI pins **clang-format-18**, specifically 18.1.8 —
`.github/workflows/ci.yml` installs it from `apt.llvm.org`'s exact recipe;
a bare `apt-get install clang-format-18` on Ubuntu can silently resolve to a
different point release with different output, see Pitfalls.md):

```sh
docker run --rm -v /path/to/build/dir:/project -w /project \
  espressif/idf:v5.5.1-node bash -c '
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/llvm.asc
    echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" > /etc/apt/sources.list.d/llvm.list
    apt-get update -qq && apt-get install -y clang-format-18
    ln -sf /usr/bin/clang-format-18 /usr/local/bin/clang-format
    find badgevms -name "*.c" -o -name "*.h" | grep -v /thirdparty/ | \
      xargs -I{} clang-format --dry-run --Werror {}
  '
```

A `docker run --rm` container is thrown away after each invocation — a
`clang-format` install in one `docker run` doesn't persist to the next; do
setup and the actual check in the same shell invocation, or install once
into an image you keep reusing.

## CI

`.github/workflows/ci.yml` runs on every PR to `main` and on `main` itself:

- **`clang-format`** — `badgevms/*.c`/`*.h` only (`thirdparty/`/`nanopb/`
  excluded), against the pinned clang-format-18.1.8.
- **`ESP-IDF build`** — the default `idf.py build`, both P4 and C6 images,
  plus the stack-usage check (below).
- **`ESP-IDF build (badgelink)`** — the same build with
  `CJ_BADGEVMS_ENABLE_BADGELINK=y`, non-blocking (informational, not a
  required check), so the experimental BadgeLink transport can't silently
  bit-rot against the rest of `badgevms/` even though it's off by default.

`.github/workflows/release.yml` runs only on a `vX.Y.Z` tag push: builds
`badgevms.bin` in the same container and publishes it as a GitHub Release
asset. See `docs/guides/Releases.md`.

## The stack-usage gate

`badgevms/CMakeLists.txt` compiles the component with `-fstack-usage`,
emitting a `.su` sidecar file per translation unit under
`build/esp-idf/badgevms/`. `scripts/check_stack_usage.py --threshold 5120
--root build/esp-idf/badgevms` parses those and fails if any function's
stack frame exceeds 5120 bytes.

This exists because a large struct declared as a plain stack-local variable
once compiled cleanly and then blew the stack at runtime on a physical
badge (a launcher context struct) — a class of bug the compiler doesn't
warn about but `-fstack-usage` makes visible before it ships. Run it
locally after touching anything with large stack-local buffers or deep call
chains; a passing build is not enough on its own.

## Branch protection

`main` requires a PR with the `clang-format` and `ESP-IDF build` checks
green (`ESP-IDF build (badgelink)` is informational, not required).
`enforce_admins` is off, so a repo admin *can* push directly, but don't —
direct-to-main pushes are exactly what made this repo's changelog
unreliable before branch protection was turned on. One PR per issue, tied
to a GitHub issue where one exists — see [Workflow.md](Workflow.md).
