#!/usr/bin/env python3
"""Check that the P4's hand-copied LoRa wire structs still match the C6's.

There is no shared header between badgevms/ and connectivity_esp_hosted/ (the
C6 tree has to stay diffable against upstream esp-hosted), so the wire structs
are duplicated by hand and drift silently. That already happened twice: once on
lora_protocol_mode_params_t::mode, and once on lora_protocol_status_params_t::
chip_type, where a uint8_t against the slave's 4-byte enum put version_string
three bytes early and it always read back empty.

Comparing the text does not work: the two sides legitimately spell the same
wire field differently (`lora_protocol_chip_t` vs `uint32_t`). So compare what
actually goes on the wire. Both definitions are compiled into one host program
with static asserts on sizeof and every field offset.

    scripts/check_wire_sync.py            # exit 1 on drift
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P4 = os.path.join(ROOT, "badgevms/drivers/lora_proto_client.c")
C6 = os.path.join(ROOT, "connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol.h")

# Wire structs both sides must agree on. lora_protocol_lora_packet_t is skipped:
# it ends in a flexible array member, so there is no fixed size to compare.
STRUCTS = [
    "lora_protocol_header_t",
    "lora_protocol_mode_params_t",
    "lora_protocol_config_params_t",
    "lora_protocol_status_params_t",
    "lora_protocol_rx_stats_t",
]


def read(path):
    return re.sub(r"/\*.*?\*/", "", open(path, errors="replace").read(), flags=re.S)


def grab(text, names):
    """Return {name: (body, [field names])} for the requested typedefs."""
    out = {}
    for m in re.finditer(
        r"typedef\s+(struct|enum)\s*\{(.*?)\}\s*(?:__attribute__\(\(packed\)\)\s*)?(\w+)\s*;", text, re.S):
        kind, body, name = m.groups()
        if name not in names:
            continue
        fields = []
        for line in body.split("\n"):
            line = re.sub(r"//.*", "", line).strip()
            fm = re.match(r"[A-Za-z_][\w\s\*]*?\**\s*(\w+)\s*(\[[^\]]*\])?\s*;", line)
            if fm:
                fields.append(fm.group(1))
        out[name] = (m.group(0), fields)
    return out


def enum_deps(text, needed):
    """Enum typedefs the struct bodies refer to, so the C6 copy compiles alone."""
    out = []
    for m in re.finditer(r"typedef\s+enum\s*\{.*?\}\s*(\w+)\s*;", text, re.S):
        if m.group(1) in needed:
            out.append(m.group(0))
    return out


def main():
    p4_text, c6_text = read(P4), read(C6)
    p4 = grab(p4_text, set(STRUCTS))
    c6 = grab(c6_text, set(STRUCTS))

    missing = [s for s in STRUCTS if s not in p4 or s not in c6]
    if missing:
        print(f"wire-sync: cannot find {', '.join(missing)} on both sides")
        return 2

    all_bodies = " ".join(b for b, _ in list(p4.values()) + list(c6.values()))
    enums = enum_deps(c6_text, set(re.findall(r"\b(lora_protocol_\w+_t)\b", all_bodies)) - set(STRUCTS))

    src = ["#include <stdint.h>", "#include <stdbool.h>", "#include <stddef.h>",
           "#define LORA_PROTOCOL_VERSION_STRING_LENGTH 16"]
    src += enums
    for name in STRUCTS:                      # C6 side, renamed
        src.append(re.sub(r"\b%s\b" % name, name + "__c6", c6[name][0]))
    for name in STRUCTS:                      # P4 side, renamed
        src.append(re.sub(r"\b%s\b" % name, name + "__p4", p4[name][0]))
    for name in STRUCTS:
        src.append(f'_Static_assert(sizeof({name}__c6) == sizeof({name}__p4), "size differs: {name}");')
        common = [f for f in c6[name][1] if f in p4[name][1]]
        for f in common:
            src.append(f'_Static_assert(offsetof({name}__c6, {f}) == offsetof({name}__p4, {f}),'
                       f' "offset differs: {name}.{f}");')
        only_c6 = [f for f in c6[name][1] if f not in p4[name][1]]
        only_p4 = [f for f in p4[name][1] if f not in c6[name][1]]
        if only_c6 or only_p4:
            print(f"wire-sync: {name} field names differ "
                  f"(C6 only: {only_c6 or '-'}, P4 only: {only_p4 or '-'})")
    src.append("int main(void) { return 0; }")

    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "wire.c")
        open(c, "w").write("\n".join(src) + "\n")
        r = subprocess.run(["cc", "-std=c11", "-o", os.path.join(d, "wire"), c],
                           capture_output=True, text=True)
    if r.returncode != 0:
        print("wire-sync: VIOLATION — the P4 copy no longer matches the C6 wire layout\n")
        for line in r.stderr.split("\n"):
            if "static assertion failed" in line or "_Static_assert" in line:
                print("  " + line.strip())
        print(f"\n  P4: {os.path.relpath(P4, ROOT)}")
        print(f"  C6: {os.path.relpath(C6, ROOT)}")
        return 1

    print(f"wire-sync: OK ({len(STRUCTS)} structs match in size and field offsets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
