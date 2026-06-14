#!/usr/bin/env bash
# test_partition_guard.sh — self-test for partition_guard.py (issue #59).
#
# Proves the guard is a REAL gate, not cosmetic: it must EXIT 0 on the shipped
# layouts and EXIT NON-ZERO on each class of bad layout (overflow, misalignment,
# overlap, unequal OTA slots, single-`factory` regression). A guard that never
# fails gives false assurance; this test fails CI if the guard stops catching a
# deliberately-broken table.
#
# Runs anywhere python3 is available (CI host + dev). No hardware/network.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
GUARD="$HERE/partition_guard.py"

# Pick a real Python 3 (CI has python3; some dev hosts only have `python`, and on
# Windows the `python3` alias can be a non-functional Store stub — probe it).
if python3 --version >/dev/null 2>&1; then
    PY=python3
elif python --version >/dev/null 2>&1; then
    PY=python
else
    echo "ERROR: no working python interpreter found" >&2
    exit 2
fi

PASS=0
FAIL=0
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

# expect_exit <expected-rc> <name> -- <cmd...>
expect_exit() {
    local want="$1"; shift
    local name="$1"; shift
    shift  # drop the literal '--'
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$name (exit $got)"; else bad "$name (got $got, want $want)"; fi
}

echo "== partition_guard self-test =="

# 1. The SHIPPED layouts must pass (default mode validates both CSVs).
( cd "$REPO_ROOT" && "$PY" "$GUARD" >/dev/null 2>&1 )
if [ $? -eq 0 ]; then ok "shipped layouts (partitions.csv + partitions_4mb.csv) pass"; \
                 else bad "shipped layouts pass"; fi

# 2. Each shipped CSV passes in single-file mode at its real flash size.
expect_exit 0 "partitions.csv @16MB passes" -- \
    "$PY" "$GUARD" "$REPO_ROOT/partitions.csv" $((16*1024*1024))
expect_exit 0 "partitions_4mb.csv @4MB passes" -- \
    "$PY" "$GUARD" "$REPO_ROOT/partitions_4mb.csv" $((4*1024*1024))

# 3. BAD: a layout whose top partition runs off the end of flash (overflow).
cat > "$TMPDIR/overflow.csv" <<'CSV'
nvs,        data, nvs,      0x9000,    0x6000
otadata,    data, ota,      0xf000,    0x2000
phy_init,   data, phy,      0x11000,   0x1000
ota_0,      app,  ota_0,    0x20000,   0x300000
ota_1,      app,  ota_1,    0x320000,  0x300000
CSV
# ota_1 ends at 0x620000 > 4MB (0x400000) -> overflow.
expect_exit 1 "overflow past flash size is caught" -- \
    "$PY" "$GUARD" "$TMPDIR/overflow.csv" $((4*1024*1024))

# 4. BAD: an app partition that is not 64 KB-aligned.
cat > "$TMPDIR/misalign.csv" <<'CSV'
nvs,        data, nvs,      0x9000,    0x6000
otadata,    data, ota,      0xf000,    0x2000
phy_init,   data, phy,      0x11000,   0x1000
ota_0,      app,  ota_0,    0x21000,   0x180000
ota_1,      app,  ota_1,    0x1A1000,  0x180000
CSV
# ota_0 @0x21000 is 4 KB-aligned but NOT 64 KB-aligned -> MMU violation.
expect_exit 1 "non-64KB-aligned app partition is caught" -- \
    "$PY" "$GUARD" "$TMPDIR/misalign.csv" $((4*1024*1024))

# 5. BAD: two partitions that overlap.
cat > "$TMPDIR/overlap.csv" <<'CSV'
nvs,        data, nvs,      0x9000,    0x6000
otadata,    data, ota,      0xf000,    0x2000
phy_init,   data, phy,      0x11000,   0x1000
ota_0,      app,  ota_0,    0x20000,   0x190000
ota_1,      app,  ota_1,    0x1A0000,  0x180000
CSV
# ota_0 ends at 0x1B0000 but ota_1 starts at 0x1A0000 -> overlap.
expect_exit 1 "overlapping partitions are caught" -- \
    "$PY" "$GUARD" "$TMPDIR/overlap.csv" $((4*1024*1024))

# 6. BAD: unequal OTA slots (A/B rollback needs equal slots).
cat > "$TMPDIR/unequal.csv" <<'CSV'
nvs,        data, nvs,      0x9000,    0x6000
otadata,    data, ota,      0xf000,    0x2000
phy_init,   data, phy,      0x11000,   0x1000
ota_0,      app,  ota_0,    0x20000,   0x180000
ota_1,      app,  ota_1,    0x1A0000,  0x100000
CSV
expect_exit 1 "unequal ota_0/ota_1 sizes are caught" -- \
    "$PY" "$GUARD" "$TMPDIR/unequal.csv" $((4*1024*1024))

# 7. BAD: single-`factory` regression (OTA disabled).
cat > "$TMPDIR/factory.csv" <<'CSV'
nvs,        data, nvs,      0x9000,    0x6000
phy_init,   data, phy,      0xf000,    0x1000
factory,    app,  factory,  0x10000,   0x300000
CSV
expect_exit 1 "single-factory (OTA-disabled) layout is caught" -- \
    "$PY" "$GUARD" "$TMPDIR/factory.csv" $((4*1024*1024))

echo "== self-test: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
