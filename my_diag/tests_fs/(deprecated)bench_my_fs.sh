#!/bin/bash
# vi: set sw=4 ts=4:
#
# !!! DEPRECATED — use bench_my_fs_warmed.sh instead !!!
#
# This script has a measurement-ordering bias: startup baselines are measured
# while the system is still cool (cold CPU governor, partial page cache),
# while my_fs cases are measured later after the system has warmed up. As a
# result, my_fs app-time can appear negative (below the noise floor).
#
# bench_my_fs_warmed.sh fixes this with explicit CPU + page-cache warm-up
# and median-of-3 measurement. Use it for any data going into reports.
#
# This script is retained only for quick "did I regress?" checks during local
# development, where ~3 s runtime matters more than absolute accuracy.
#
# Performance benchmark: my_fs vs GNU df / util-linux filefrag.
# Usage: bash '(deprecated)bench_my_fs.sh' [path/to/busybox]
# Requires: /usr/bin/time (GNU time) or bash built-in time, df, filefrag
# Loop device cases require root.
#
# Output: Markdown table to stdout (redirect to a .md file if desired).

# ── Deprecation notice (prints to stderr) ──────────────────────────
cat >&2 <<'EOF'
============================================================================
  ⚠  (deprecated)bench_my_fs.sh is DEPRECATED.

  This script has measurement-ordering bias that can cause my_fs app-time
  to appear negative. For any data that will go into a report, use:

      bash my_diag/tests/bench_my_fs_warmed.sh

  Continuing in 2 seconds for quick local checks...
============================================================================
EOF
sleep 2

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

# Compute application-layer time: total − startup baseline.
# May be negative or zero if the application logic is below measurement noise.
app_ms() {
    local total=$1 base=$2
    awk "BEGIN { printf \"%.1f\", $total - $base }"
}

# Print one benchmark row with both total and application-layer figures.
# $1=label  $2=my_total  $3=my_app  $4=ref_total  $5=ref_app
row() {
    local label=$1 a=$2 a_app=$3 b=$4 b_app=$5
    local ratio app_ratio note=""
    ratio=$(awk "BEGIN { if ($b>0) printf \"%.2f\", $a/$b; else print \"N/A\" }")
    if awk "BEGIN { exit !($a_app > 0 && $b_app > 0) }"; then
        app_ratio=$(awk "BEGIN { printf \"%.2f\", $a_app/$b_app }")
    else
        app_ratio="—"
    fi
    awk "BEGIN { exit ($ratio <= 1.50) }" && note=" ⚠"
    printf "| %-35s | %6s ms | %6s ms | %6s ms | %6s ms | %5s | %5s |%s\n" \
        "$label" "$a" "$a_app" "$b" "$b_app" "$ratio" "$app_ratio" "$note"
}

# Print the table header with the dynamic-line separator.
print_table_header() {
    local ref_label=$1
    printf "| %-35s | %-9s | %-9s | %-9s | %-9s | %-5s | %-5s |\n" \
        "Case" "my_fs tot" "my_fs app" "$ref_label tot" "$ref_label app" "Total" "App"
    printf "|%s|%s|%s|%s|%s|%s|%s|\n" \
        "$(printf '%37s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%7s' '' | tr ' ' '-')" \
        "$(printf '%7s' '' | tr ' ' '-')"
}

# ── Header ────────────────────────────────────────────────────────
echo "# my_fs Performance Benchmark"
echo ""
printf -- "- Date: %s\n" "$(date)"
printf -- "- Kernel: %s\n" "$(uname -r)"
printf -- "- Runs per case: %d\n" "$REPEAT"
printf -- "- busybox: %s\n" "$BUSYBOX"
echo ""

# ── Startup baselines ─────────────────────────────────────────────
SHELL_BASE=$(avg_ms true)
BB_BASE=$(avg_ms $BUSYBOX true)
REF_BASE=$(avg_ms /bin/true)
ARCH_OVERHEAD=$(awk "BEGIN { printf \"%.1f\", $BB_BASE - $REF_BASE }")

echo "## Startup Baselines"
echo ""
printf -- "- Shell loop only (builtin \`true\`, no fork): **%s ms/iter** (sanity check; should be ≈ 0)\n" "$SHELL_BASE"
printf -- "- Dynamic-linked binary (\`/bin/true\`): **%s ms/iter**  — baseline for \`df\` / \`filefrag\`\n" "$REF_BASE"
printf -- "- BusyBox static binary (\`./busybox true\`): **%s ms/iter**  — baseline for \`my_fs\`\n" "$BB_BASE"
printf -- "- Fixed architectural overhead (static − dynamic): **%s ms/iter**\n" "$ARCH_OVERHEAD"
echo ""
echo "App time = total − startup baseline.  \"—\" means at or below the noise floor."
echo "Ratio = my_fs / reference tool.  Goal: Total Ratio ≤ 1.50 (within 50%)."
echo ""

echo "## Disk Usage (\`my_fs\` vs \`df\`)"
echo ""
print_table_header "df"

# Case 1: single path /
t_mine=$(avg_ms $BUSYBOX my_fs /)
t_ref=$(avg_ms df /)
mine_app=$(app_ms "$t_mine" "$BB_BASE")
ref_app=$(app_ms "$t_ref" "$REF_BASE")
row "my_fs / vs df /" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

# Case 2: all mounts (no args)
t_mine=$(avg_ms $BUSYBOX my_fs)
t_ref=$(avg_ms df)
mine_app=$(app_ms "$t_mine" "$BB_BASE")
ref_app=$(app_ms "$t_ref" "$REF_BASE")
row "my_fs (all mounts) vs df" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

# Case 3: -h
t_mine=$(avg_ms $BUSYBOX my_fs -h)
t_ref=$(avg_ms df -h)
mine_app=$(app_ms "$t_mine" "$BB_BASE")
ref_app=$(app_ms "$t_ref" "$REF_BASE")
row "my_fs -h vs df -h" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

# Case 4: -i
t_mine=$(avg_ms $BUSYBOX my_fs -i)
t_ref=$(avg_ms df -i)
mine_app=$(app_ms "$t_mine" "$BB_BASE")
ref_app=$(app_ms "$t_ref" "$REF_BASE")
row "my_fs -i vs df -i" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

# Case 5: -t tmpfs
t_mine=$(avg_ms $BUSYBOX my_fs -t tmpfs)
t_ref=$(avg_ms df -t tmpfs)
mine_app=$(app_ms "$t_mine" "$BB_BASE")
ref_app=$(app_ms "$t_ref" "$REF_BASE")
row "my_fs -t tmpfs vs df -t tmpfs" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

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

    print_table_header "filefrag"

    t_mine=$(avg_ms $BUSYBOX my_fs -f "$TESTFILE")
    t_ref=$(avg_ms filefrag -v "$TESTFILE")
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$REF_BASE")
    row "my_fs -f vs filefrag -v (1 MB)" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

    umount "$LOOP_MNT"
    losetup -d "$LOOP_DEV"
    LOOP_DEV=""; LOOP_MNT=""
fi
