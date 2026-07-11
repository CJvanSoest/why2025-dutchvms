#!/usr/bin/env python3
"""Check per-function stack frame sizes from GCC -fstack-usage output.

Why this exists
----------------
A large struct (`Launcher_Context`) was once declared as a plain
stack-local variable in cj_launcher's run_launcher(). It compiled with
zero warnings/errors and crashed the physical badge on every boot with a
stack-protection fault at runtime -- these PIE-ELF apps run with small
stacks and a normal build gives zero signal about frame size. This script
closes that gap by scanning the .su sidecar files GCC emits with
-fstack-usage (one per translation unit, format:
"file:line:col:function<TAB>bytes<TAB>{static,dynamic,bounded}") and
failing CI if any single function's frame exceeds a threshold.

Usage:
    check_stack_usage.py --threshold BYTES [--root DIR ...] [--top N]

Exit status is non-zero if any function's stack frame size is strictly
greater than --threshold bytes.
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass


@dataclass
class Frame:
    location: str  # "file.c:line:col"
    function: str
    bytes_: int
    qualifier: str
    su_file: str


def find_su_files(roots: list[str]) -> list[str]:
    su_files = []
    for root in roots:
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in filenames:
                if name.endswith(".su"):
                    su_files.append(os.path.join(dirpath, name))
    return sorted(su_files)


def parse_su_file(path: str) -> list[Frame]:
    frames = []
    with open(path, "r", errors="replace") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                print(
                    f"WARNING: {path}:{lineno}: unexpected .su line format, skipping: {line!r}",
                    file=sys.stderr,
                )
                continue
            loc_and_func, bytes_str, qualifier = fields
            # loc_and_func is "file:line:col:function_name" -- function
            # names in C don't contain ':', so split from the right on the
            # first 3 colons is unnecessary; split with maxsplit from left
            # works since the file path itself may (rarely) contain ':'
            # only on Windows, which we don't build on here.
            parts = loc_and_func.split(":")
            if len(parts) < 4:
                print(
                    f"WARNING: {path}:{lineno}: cannot parse location/function, skipping: {line!r}",
                    file=sys.stderr,
                )
                continue
            function = parts[-1]
            location = ":".join(parts[:-1])
            try:
                bytes_ = int(bytes_str)
            except ValueError:
                print(
                    f"WARNING: {path}:{lineno}: non-integer byte count, skipping: {line!r}",
                    file=sys.stderr,
                )
                continue
            frames.append(Frame(location=location, function=function, bytes_=bytes_, qualifier=qualifier, su_file=path))
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--threshold",
        type=int,
        required=True,
        help="Max allowed stack frame size in bytes for a single function.",
    )
    parser.add_argument(
        "--root",
        action="append",
        dest="roots",
        default=None,
        help="Directory to search for .su files (repeatable). Defaults to current directory.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="How many top offenders to print regardless of pass/fail (default: 20).",
    )
    args = parser.parse_args()

    roots = args.roots or ["."]
    su_files = find_su_files(roots)

    if not su_files:
        print(f"ERROR: no .su files found under {roots} -- did the build run with -fstack-usage?", file=sys.stderr)
        return 2

    all_frames: list[Frame] = []
    for su_file in su_files:
        all_frames.extend(parse_su_file(su_file))

    if not all_frames:
        print(f"ERROR: found {len(su_files)} .su file(s) but parsed zero stack frames.", file=sys.stderr)
        return 2

    all_frames.sort(key=lambda fr: fr.bytes_, reverse=True)

    print(f"Scanned {len(su_files)} .su file(s), {len(all_frames)} function(s).")
    print(f"Stack frame size distribution: max={all_frames[0].bytes_}  "
          f"min={all_frames[-1].bytes_}  threshold={args.threshold}")
    print()
    print(f"Top {min(args.top, len(all_frames))} stack frame(s) by size:")
    for fr in all_frames[: args.top]:
        marker = "  ** OVER THRESHOLD **" if fr.bytes_ > args.threshold else ""
        print(f"  {fr.bytes_:>8} bytes  {fr.location}  {fr.function}() [{fr.qualifier}]{marker}")

    offenders = [fr for fr in all_frames if fr.bytes_ > args.threshold]
    if offenders:
        print()
        print(f"FAIL: {len(offenders)} function(s) exceed the {args.threshold}-byte stack frame threshold:")
        for fr in offenders:
            print(f"  {fr.bytes_:>8} bytes  {fr.location}  {fr.function}()")
        return 1

    print()
    print(f"OK: all {len(all_frames)} function(s) are within the {args.threshold}-byte stack frame threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
