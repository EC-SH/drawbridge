#!/usr/bin/env python3
# partition_guard.py — dual-OTA partition-table layout guard.
#
# Issue #59 [M-7]: the previous guard (inline in ci.yml) validated ONLY
# partitions.csv against a hard-coded 16 MB and had no overlap or alignment
# checks. partitions_4mb.csv (the SIP_CONSTRAINED classic-ESP32 layout) was
# unchecked, so a dual-OTA / overflow / misalignment regression in the 4 MB
# layout passed CI and was caught only at flash time.
#
# This standalone, host-runnable script loops the guard over EVERY configured
# layout with its correct flash size and adds the missing assertions:
#   * dual-OTA shape (ota_0 + ota_1 + otadata present; no single `factory` slot)
#   * equal, app-typed OTA slots
#   * every partition fits inside the layout's flash size
#   * NO partition overlaps another (sorted-by-offset adjacency check)
#   * app partitions are 64 KB-aligned (ESP32-S3 MMU flash-mapping requirement)
#   * data partitions are 4 KB-aligned (flash sector size)
#
# Exit code 0 = all layouts pass; 1 = at least one violation (CI-blocking).
#
# Usage:
#   partition_guard.py                  # validate the built-in LAYOUTS
#   partition_guard.py FILE FLASH_BYTES # validate a single CSV (used by the
#                                         self-test to feed a deliberately-bad
#                                         layout and assert a non-zero exit)

import re
import sys

# (csv path, flash size in bytes). Both shipped layouts are guarded.
LAYOUTS = [
    ("partitions.csv", 16 * 1024 * 1024),       # ESP32-S3 16 MB (default)
    ("partitions_4mb.csv", 4 * 1024 * 1024),     # classic ESP32 4 MB (SIP_CONSTRAINED)
]

APP_ALIGN = 0x10000   # 64 KB — app-partition MMU requirement
DATA_ALIGN = 0x1000   # 4 KB  — flash sector size


def parse_num(s):
    s = s.strip()
    m = re.fullmatch(r'(0[xX][0-9a-fA-F]+|\d+)([KkMm]?)', s)
    if not m:
        raise ValueError("cannot parse number %r" % s)
    base = int(m.group(1), 0)
    suf = m.group(2).lower()
    if suf == 'k':
        base *= 1024
    elif suf == 'm':
        base *= 1024 * 1024
    return base


def parse_csv(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = [p.strip() for p in line.split(',')]
            if len(parts) < 5:
                continue
            name, ptype, subtype, off, size = parts[:5]
            rows.append((name, ptype, subtype, parse_num(off), parse_num(size)))
    return rows


def check_layout(path, flash):
    """Return a list of violation strings for one CSV (empty == pass)."""
    try:
        rows = parse_csv(path)
    except (OSError, ValueError) as e:
        return ["could not read/parse %s: %s" % (path, e)]

    if not rows:
        return ["%s has no partition rows" % path]

    errors = []
    names = {r[0] for r in rows}

    # --- dual-OTA shape ---
    for required in ("ota_0", "ota_1", "otadata"):
        if required not in names:
            errors.append("missing required partition '%s'" % required)
    if "factory" in names:
        errors.append("partition 'factory' present — layout regressed to single-slot (OTA disabled)")

    # --- per-partition bounds + alignment ---
    for name, ptype, subtype, off, size in rows:
        if off + size > flash:
            errors.append("partition '%s' ends at 0x%x, exceeds flash 0x%x"
                          % (name, off + size, flash))
        align = APP_ALIGN if ptype == "app" else DATA_ALIGN
        if off % align != 0:
            errors.append("partition '%s' offset 0x%x is not %d-byte aligned (type=%s)"
                          % (name, off, align, ptype))

    # --- OTA slot equality + type ---
    slots = {r[0]: r for r in rows if r[0] in ("ota_0", "ota_1")}
    if "ota_0" in slots and "ota_1" in slots:
        if slots["ota_0"][4] != slots["ota_1"][4]:
            errors.append("ota_0 size (0x%x) != ota_1 size (0x%x)"
                          % (slots["ota_0"][4], slots["ota_1"][4]))
        for s in ("ota_0", "ota_1"):
            if slots[s][1] != "app":
                errors.append("%s is type '%s', expected 'app'" % (s, slots[s][1]))

    # --- overlap check (sort by offset; each must start at/after prev end) ---
    ordered = sorted(rows, key=lambda r: r[3])
    for prev, cur in zip(ordered, ordered[1:]):
        prev_end = prev[3] + prev[4]
        if cur[3] < prev_end:
            errors.append("partition '%s' (0x%x) overlaps '%s' (ends 0x%x)"
                          % (cur[0], cur[3], prev[0], prev_end))

    return errors


def report(path, flash, errors):
    if errors:
        print("PARTITION GUARD: FAIL — %s (flash %d MB)" % (path, flash // (1024 * 1024)))
        for e in errors:
            print("  - " + e)
    else:
        print("PARTITION GUARD: PASS — %s (flash %d MB, dual-OTA layout intact)"
              % (path, flash // (1024 * 1024)))


def main(argv):
    # Single-file mode: validate one CSV at an explicit flash size (self-test).
    if len(argv) == 3:
        path, flash = argv[1], int(argv[2], 0)
        errors = check_layout(path, flash)
        report(path, flash, errors)
        return 1 if errors else 0

    # Default mode: validate every shipped layout.
    any_fail = False
    for path, flash in LAYOUTS:
        errors = check_layout(path, flash)
        report(path, flash, errors)
        if errors:
            any_fail = True
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
