#!/bin/bash
# vi: set sw=4 ts=4:
# Performance benchmark: my_fs vs GNU df / util-linux filefrag.
# Usage: bash bench_my_fs.sh [path/to/busybox]
# Requires: /usr/bin/time (GNU time) or bash built-in time, df, filefrag
# Loop device cases require root.
#
# Output: Markdown table to stdout (redirect to a .md file if desired).

BUSYBOX="${1:-./busybox}"
REPEAT=100       # number of runs per case for averaging (100 keeps loop time >50 ms, well above bash time resolution)
TMPDIR_WORK=$(mktemp -d)
LOOP_DEV=""
LOOP_MNT=""

cleanup() {
    if [ -n "$LOOP_MNT" ] && mountpoint -q "$LOOP_MNT" 2>/dev/null; then
        umount "$LOOP_MNT" 2>/dev/null
    fi
    [ -n "$LOOP_DEV" ] && losetup -d "$LOOP_DEV" 2>/dev/null
    rm -rf "$TMPDIR_WORK"
}
trap cleanup EXIT

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# Run command $REPEAT times, return average wall-clock time in milliseconds.
# Times the entire loop to avoid sub-millisecond precision loss on fast commands.
avg_ms() {
    local elapsed
    elapsed=$( { TIMEFORMAT='%R'; time for i in $(seq 1 "$REPEAT"); do "$@" >/dev/null 2>&1; done; } 2>&1 )
    awk "BEGIN { printf \"%.1f\", $elapsed * 1000 / $REPEAT }"
}

# Print one benchmark row.
# $1=label  $2=cmd_a_ms  $3=cmd_b_ms  $4=cmd_a_name  $5=cmd_b_name
row() {
    local label=$1 a=$2 b=$3 ratio
    ratio=$(awk "BEGIN { if ($b>0) printf \"%.2f\", $a/$b; else print \"N/A\" }")
    # Check if within 50% threshold (ratio <= 1.5)
    local note=""
    awk "BEGIN { exit ($ratio <= 1.50) }" && note=" ⚠ >50%"
    printf "| %-35s | %9s ms | %9s ms | %6s |%s\n" \
        "$label" "$a" "$b" "$ratio" "$note"
}

# ── Header ────────────────────────────────────────────────────────
echo "# my_fs Performance Benchmark"
echo ""
printf -- "- Date: %s\n" "$(date)"
printf -- "- Kernel: %s\n" "$(uname -r)"
printf -- "- Runs per case: %d\n" "$REPEAT"
printf -- "- busybox: %s\n" "$BUSYBOX"
echo ""
echo "Ratio = my_fs / reference tool.  Goal: ratio ≤ 1.50 (within 50%)."
echo ""
echo "## Disk Usage (\`my_fs\` vs \`df\`)"
echo ""
printf "| %-35s | %-12s | %-12s | %-6s |\n" \
    "Case" "my_fs (avg)" "df (avg)" "Ratio"
printf "|%s|%s|%s|%s|\n" \
    "$(printf '%37s' '' | tr ' ' '-')" \
    "$(printf '%14s' '' | tr ' ' '-')" \
    "$(printf '%14s' '' | tr ' ' '-')" \
    "$(printf '%8s' '' | tr ' ' '-')"

# Case 1: single path /
t_mine=$(avg_ms $BUSYBOX my_fs /)
t_ref=$(avg_ms df /)
row "my_fs / vs df /" "$t_mine" "$t_ref"

# Case 2: all mounts (no args)
t_mine=$(avg_ms $BUSYBOX my_fs)
t_ref=$(avg_ms df)
row "my_fs (all mounts) vs df" "$t_mine" "$t_ref"

# Case 3: -h
t_mine=$(avg_ms $BUSYBOX my_fs -h)
t_ref=$(avg_ms df -h)
row "my_fs -h vs df -h" "$t_mine" "$t_ref"

# Case 4: -i
t_mine=$(avg_ms $BUSYBOX my_fs -i)
t_ref=$(avg_ms df -i)
row "my_fs -i vs df -i" "$t_mine" "$t_ref"

# Case 5: -t tmpfs
t_mine=$(avg_ms $BUSYBOX my_fs -t tmpfs)
t_ref=$(avg_ms df -t tmpfs)
row "my_fs -t tmpfs vs df -t tmpfs" "$t_mine" "$t_ref"

# ── FIEMAP section (needs ext4 loop device) ───────────────────────
echo ""
echo "## Fragmentation Analysis (\`my_fs -f\` vs \`filefrag\`)"
echo ""

if [ "$(id -u)" -ne 0 ]; then
    echo "> Skipped: loop device tests require root."
elif ! has_cmd mkfs.ext4; then
    echo "> Skipped: mkfs.ext4 not found."
elif ! has_cmd filefrag; then
    echo "> Skipped: filefrag not found."
else
    # Create ext4 loop device with a test file
    EXT4_IMG="$TMPDIR_WORK/bench.ext4.img"
    LOOP_MNT="$TMPDIR_WORK/mnt_bench"
    mkdir -p "$LOOP_MNT"
    dd if=/dev/zero of="$EXT4_IMG" bs=1M count=128 status=none
    mkfs.ext4 -q -F "$EXT4_IMG"
    LOOP_DEV=$(losetup --find --show "$EXT4_IMG")
    mount "$LOOP_DEV" "$LOOP_MNT"

    # Create a test file (~1 MB)
    TESTFILE="$LOOP_MNT/bench_file"
    dd if=/dev/urandom of="$TESTFILE" bs=4k count=256 status=none
    sync

    printf "| %-35s | %-12s | %-12s | %-6s |\n" \
        "Case" "my_fs (avg)" "filefrag avg" "Ratio"
    printf "|%s|%s|%s|%s|\n" \
        "$(printf '%37s' '' | tr ' ' '-')" \
        "$(printf '%14s' '' | tr ' ' '-')" \
        "$(printf '%14s' '' | tr ' ' '-')" \
        "$(printf '%8s' '' | tr ' ' '-')"

    t_mine=$(avg_ms $BUSYBOX my_fs -f "$TESTFILE")
    t_ref=$(avg_ms filefrag -v "$TESTFILE")
    row "my_fs -f vs filefrag -v (1 MB)" "$t_mine" "$t_ref"

    umount "$LOOP_MNT"
    losetup -d "$LOOP_DEV"
    LOOP_DEV=""; LOOP_MNT=""
fi
