#!/bin/bash
# vi: set sw=4 ts=4:
# Regression tests for the my_fs applet.
# Usage: bash test_my_fs.sh [path/to/busybox]
# Loop device tests (T10-T16) require root privileges.

BUSYBOX="${1:-./busybox}"
PASS=0; FAIL=0; SKIP=0

TMPDIR_WORK=$(mktemp -d)
LOOP_DEV=""
LOOP_MNT=""

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'

pass() { printf "${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "${RED}FAIL${NC} %s\n" "$1"; FAIL=$((FAIL+1)); }
skip() { printf "${YELLOW}SKIP${NC} %s\n" "$1"; SKIP=$((SKIP+1)); }

cleanup() {
    if [ -n "$LOOP_MNT" ] && mountpoint -q "$LOOP_MNT" 2>/dev/null; then
        umount "$LOOP_MNT" 2>/dev/null
    fi
    [ -n "$LOOP_DEV" ] && losetup -d "$LOOP_DEV" 2>/dev/null
    rm -rf "$TMPDIR_WORK"
}
trap cleanup EXIT

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# Extract 1K-block columns from df/my_fs output for a given path.
# Handles df line-wrapping when the device name is long.
# $1=path, $2=column_index (2=total, 3=used, 4=avail)
df_col()   { df      "$1" 2>/dev/null | awk -v c="$2" 'NR>1 && NF>=5 {print $c; exit}'; }
myfs_col() { $BUSYBOX my_fs "$1" 2>/dev/null | awk -v c="$2" 'NR>1 && NF>=5 {print $c; exit}'; }

# inode columns: 2=Inodes 3=IUsed 4=IFree
df_icol()   { df -i      "$1" 2>/dev/null | awk -v c="$2" 'NR>1 && NF>=5 {print $c; exit}'; }
myfs_icol() { $BUSYBOX my_fs -i "$1" 2>/dev/null | awk -v c="$2" 'NR>1 && NF>=5 {print $c; exit}'; }

# Returns 0 if |a - b| <= tolerance (default 2).
within() {
    local a=$1 b=$2 tol=${3:-2}
    echo "$a" | grep -qE '^[0-9]+$' || return 1
    echo "$b" | grep -qE '^[0-9]+$' || return 1
    [ $(( a > b ? a - b : b - a )) -le $tol ]
}

# ── Preflight ─────────────────────────────────────────────────────
echo "=== my_fs regression tests ==="
printf "busybox : %s\n" "$BUSYBOX"
printf "date    : %s\n" "$(date)"
echo ""

if [ ! -x "$BUSYBOX" ]; then
    echo "ERROR: $BUSYBOX not found or not executable" >&2
    exit 1
fi

# ── Group 1: Capacity comparison on / ────────────────────────────
echo "--- Group 1: Capacity comparison (/) ---"

v1=$(myfs_col / 2); v2=$(df_col / 2)
if within "$v1" "$v2"; then pass "T01 total_1k  : my_fs=$v1  df=$v2"
else                        fail "T01 total_1k  : my_fs=$v1  df=$v2"; fi

v1=$(myfs_col / 3); v2=$(df_col / 3)
if within "$v1" "$v2"; then pass "T02 used_1k   : my_fs=$v1  df=$v2"
else                        fail "T02 used_1k   : my_fs=$v1  df=$v2"; fi

v1=$(myfs_col / 4); v2=$(df_col / 4)
if within "$v1" "$v2"; then pass "T03 avail_1k  : my_fs=$v1  df=$v2"
else                        fail "T03 avail_1k  : my_fs=$v1  df=$v2"; fi

# -h output should have a K/M/G/T suffix in the size column
hr=$($BUSYBOX my_fs -h / 2>/dev/null | awk 'NR>1 && NF>=5 {print $2; exit}')
if echo "$hr" | grep -qE '^[0-9]+(\.[0-9]+)?[KMGTPE]$'; then
    pass "T04 -h format : size=$hr"
else
    fail "T04 -h format : got '$hr'"
fi

# ── Group 2: Inode mode on / ──────────────────────────────────────
echo ""
echo "--- Group 2: Inode mode (/) ---"

v1=$(myfs_icol / 2); v2=$(df_icol / 2)
if within "$v1" "$v2"; then pass "T05 total_inodes: my_fs=$v1  df=$v2"
else                        fail "T05 total_inodes: my_fs=$v1  df=$v2"; fi

v1=$(myfs_icol / 4); v2=$(df_icol / 4)
if within "$v1" "$v2"; then pass "T06 free_inodes : my_fs=$v1  df=$v2"
else                        fail "T06 free_inodes : my_fs=$v1  df=$v2"; fi

# ── Group 3: Option flags ─────────────────────────────────────────
echo ""
echo "--- Group 3: Option flags ---"

# -r must include RootResv column header
if $BUSYBOX my_fs -r / 2>/dev/null | grep -q "RootResv"; then
    pass "T07 -r header : RootResv present"
else
    fail "T07 -r header : RootResv missing"
fi

# Use%(user) and Use%(real) should appear as two separate % values on data line
pct_count=$($BUSYBOX my_fs -r / 2>/dev/null | awk 'NR>1 && NF>=5 {print}' | grep -oE '[0-9]+%' | wc -l)
if [ "$pct_count" -ge 2 ]; then
    pass "T08 -r dual-% : found $pct_count %% values on data line"
else
    fail "T08 -r dual-% : expected >=2 %% values, got $pct_count"
fi

# -t tmpfs: output lines should be for tmpfs only (or empty if none mounted)
if df -t tmpfs >/dev/null 2>&1; then
    out=$($BUSYBOX my_fs -t tmpfs 2>/dev/null | awk 'NR>1 && NF>=5')
    if [ -n "$out" ]; then
        pass "T09 -t tmpfs  : entries found"
    else
        fail "T09 -t tmpfs  : expected entries, got none"
    fi
else
    skip "T09 -t tmpfs  : no tmpfs mounts available"
fi

# -x tmpfs: no line should reference a tmpfs mount point
tmpfs_mps=$(df -t tmpfs 2>/dev/null | awk 'NR>1 {print $NF}')
if [ -n "$tmpfs_mps" ]; then
    out=$($BUSYBOX my_fs -x tmpfs 2>/dev/null | awk 'NR>1 && NF>=5 {print $NF}')
    leaked=0
    for mp in $tmpfs_mps; do
        echo "$out" | grep -qF "$mp" && leaked=$((leaked+1))
    done
    if [ "$leaked" -eq 0 ]; then
        pass "T10 -x tmpfs  : no tmpfs entries leaked"
    else
        fail "T10 -x tmpfs  : $leaked tmpfs entry(ies) still shown"
    fi
else
    skip "T10 -x tmpfs  : no tmpfs mounts to exclude"
fi

# ── Group 4: ext4 loop device ────────────────────────────────────
echo ""
echo "--- Group 4: ext4 loop device ---"

if [ "$(id -u)" -ne 0 ]; then
    for t in T11 T12 T13 T14 T15; do skip "$t (not root)"; done
elif ! has_cmd mkfs.ext4; then
    for t in T11 T12 T13 T14 T15; do skip "$t (mkfs.ext4 not found)"; done
else
    EXT4_IMG="$TMPDIR_WORK/test.ext4.img"
    LOOP_MNT="$TMPDIR_WORK/mnt_ext4"
    mkdir -p "$LOOP_MNT"
    dd if=/dev/zero of="$EXT4_IMG" bs=1M count=64 status=none
    mkfs.ext4 -q -F "$EXT4_IMG"
    LOOP_DEV=$(losetup --find --show "$EXT4_IMG")
    mount "$LOOP_DEV" "$LOOP_MNT"

    v1=$(myfs_col "$LOOP_MNT" 2); v2=$(df_col "$LOOP_MNT" 2)
    if within "$v1" "$v2"; then pass "T11 ext4 total_1k    : my_fs=$v1  df=$v2"
    else                        fail "T11 ext4 total_1k    : my_fs=$v1  df=$v2"; fi

    v1=$(myfs_icol "$LOOP_MNT" 2); v2=$(df_icol "$LOOP_MNT" 2)
    if within "$v1" "$v2"; then pass "T12 ext4 total_inodes: my_fs=$v1  df=$v2"
    else                        fail "T12 ext4 total_inodes: my_fs=$v1  df=$v2"; fi

    # ext4 reserves 5% by default → RootResv should be > 0
    resv=$($BUSYBOX my_fs -r "$LOOP_MNT" 2>/dev/null | \
           awk 'NR>1 && NF>=5 {print $(NF-1)}')
    if echo "$resv" | grep -qE '^[0-9]+$' && [ "$resv" -gt 0 ]; then
        pass "T13 ext4 RootResv    : $resv KB (>0)"
    else
        fail "T13 ext4 RootResv    : expected >0, got '$resv'"
    fi

    # FIEMAP: extent count must match filefrag
    TESTFILE="$LOOP_MNT/testfile"
    dd if=/dev/urandom of="$TESTFILE" bs=4k count=8 status=none
    sync
    if ! has_cmd filefrag; then
        skip "T14 FIEMAP (filefrag not found)"
    else
        fc_mine=$($BUSYBOX my_fs -f "$TESTFILE" 2>/dev/null | \
                  grep -oE '[0-9]+ extent' | grep -oE '^[0-9]+')
        fc_ref=$(filefrag "$TESTFILE" 2>/dev/null | \
                 grep -oE '[0-9]+ extent' | grep -oE '^[0-9]+')
        if [ "$fc_mine" = "$fc_ref" ] && [ -n "$fc_mine" ]; then
            pass "T14 FIEMAP extents   : my_fs=$fc_mine  filefrag=$fc_ref"
        else
            fail "T14 FIEMAP extents   : my_fs=$fc_mine  filefrag=$fc_ref"
        fi
    fi

    # -F scan: should report at least 1 file scanned
    fscan=$($BUSYBOX my_fs -F "$LOOP_MNT" 2>/dev/null)
    if echo "$fscan" | grep -qE 'Scanned: [1-9][0-9]* files'; then
        cnt=$(echo "$fscan" | grep -oE 'Scanned: [0-9]+' | grep -oE '[0-9]+')
        pass "T15 -F scan ext4     : $cnt file(s) scanned"
    else
        fail "T15 -F scan ext4     : unexpected output"
    fi

    umount "$LOOP_MNT"
    losetup -d "$LOOP_DEV"
    LOOP_DEV=""; LOOP_MNT=""
fi

# ── Group 5: xfs loop device ─────────────────────────────────────
echo ""
echo "--- Group 5: xfs loop device ---"

if [ "$(id -u)" -ne 0 ]; then
    for t in T16 T17; do skip "$t (not root)"; done
elif ! has_cmd mkfs.xfs; then
    for t in T16 T17; do skip "$t (mkfs.xfs not found)"; done
else
    XFS_IMG="$TMPDIR_WORK/test.xfs.img"
    LOOP_MNT="$TMPDIR_WORK/mnt_xfs"
    mkdir -p "$LOOP_MNT"
    dd if=/dev/zero of="$XFS_IMG" bs=1M count=64 status=none
    mkfs.xfs -q -f "$XFS_IMG"
    LOOP_DEV=$(losetup --find --show "$XFS_IMG")
    mount "$LOOP_DEV" "$LOOP_MNT"

    v1=$(myfs_col "$LOOP_MNT" 2); v2=$(df_col "$LOOP_MNT" 2)
    if within "$v1" "$v2"; then pass "T16 xfs total_1k     : my_fs=$v1  df=$v2"
    else                        fail "T16 xfs total_1k     : my_fs=$v1  df=$v2"; fi

    v1=$(myfs_icol "$LOOP_MNT" 2); v2=$(df_icol "$LOOP_MNT" 2)
    if within "$v1" "$v2"; then pass "T17 xfs total_inodes : my_fs=$v1  df=$v2"
    else                        fail "T17 xfs total_inodes : my_fs=$v1  df=$v2"; fi

    umount "$LOOP_MNT"
    losetup -d "$LOOP_DEV"
    LOOP_DEV=""; LOOP_MNT=""
fi

# ── Summary ───────────────────────────────────────────────────────
echo ""
printf "=== Summary: ${GREEN}PASS=%d${NC}  ${RED}FAIL=%d${NC}  ${YELLOW}SKIP=%d${NC} ===\n" \
    "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
