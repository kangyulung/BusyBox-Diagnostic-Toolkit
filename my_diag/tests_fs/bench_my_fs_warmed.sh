#!/bin/bash
# vi: set sw=4 ts=4:
# Performance benchmark (with system warm-up): my_fs vs GNU df / util-linux filefrag.
#
# Differs from (deprecated)bench_my_fs.sh in three ways:
#   1. Runs a CPU-intensive busy loop before measurement, encouraging the host
#      CPU governor to settle at the performance frequency.
#   2. Pre-runs every measured binary (with every relevant argv) WARMUP_ROUNDS
#      times to fully warm the page cache, TLB, and branch predictor.
#   3. Measures each case three times and reports the median, reducing the
#      effect of one-off scheduler jitter.
#
# These changes mitigate the systematic ordering bias seen in (deprecated)bench_my_fs.sh,
# where the first-measured `./busybox true` baseline ran cooler than the
# later-measured my_fs cases (causing my_fs app-time to appear negative).
#
# Trade-off: this script takes notably longer than (deprecated)bench_my_fs.sh
# (about 25-40 s total) because warm-up now covers every measured binary,
# not just the startup baselines.
#
# Usage: bash bench_my_fs_warmed.sh [path/to/busybox]
# Output: Markdown table to stdout.

BUSYBOX="${1:-./busybox}"
REPEAT=500              # iterations per measurement round
MIN_APP_MS=$(awk "BEGIN { printf \"%.2f\", 5 / $REPEAT }")  # noise floor: 5 iter-equiv
WARMUP_ROUNDS=200       # iterations of each binary during warm-up
MEDIAN_OF=3             # measurement rounds per case (odd number; median is reported)
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

# Single-round average (same as (deprecated)bench_my_fs.sh's avg_ms).
avg_ms_once() {
    local elapsed
    elapsed=$( { TIMEFORMAT='%R'; time for i in $(seq 1 "$REPEAT"); do "$@" >/dev/null 2>&1; done; } 2>&1 )
    awk "BEGIN { printf \"%.2f\", $elapsed * 1000 / $REPEAT }"
}

# Median of $MEDIAN_OF rounds, each round being a REPEAT-iter average.
avg_ms() {
    local i results
    results=""
    for i in $(seq 1 "$MEDIAN_OF"); do
        results="$results $(avg_ms_once "$@")"
    done
    echo "$results" | tr ' ' '\n' | grep -v '^$' | sort -n | \
        awk -v n="$MEDIAN_OF" 'NR == int(n/2)+1 { print }'
}

# Compute application-layer time (total - startup baseline).
app_ms() {
    local total=$1 base=$2
    awk "BEGIN { printf \"%.2f\", $total - $base }"
}

# Status symbol (four tiers):
#   (none)  Total ≤ 1.50 — proposal target met
#   ℹ       Total > 1.50 but App ≤ 1.50 — architectural (BusyBox startup tax)
#   ⚠?      Total > 1.50 and App at or below noise floor — cannot decide
#   ⚠       Total > 1.50 and App > 1.50 — application slower than reference
row() {
    local label=$1 a=$2 a_app=$3 b=$4 b_app=$5 min_ref=${6:-0}
    local ratio app_ratio note=""
    ratio=$(awk "BEGIN { if ($b>0) printf \"%.2f\", $a/$b; else print \"N/A\" }")
    if awk "BEGIN { exit !($a_app > 0 && $b_app > $min_ref) }"; then
        app_ratio=$(awk "BEGIN { printf \"%.2f\", $a_app/$b_app }")
    else
        app_ratio="—"
    fi
    if awk "BEGIN { exit ($ratio <= 1.50) }"; then
        if [ "$app_ratio" = "—" ]; then
            note=" ⚠?"
        elif awk "BEGIN { exit ($app_ratio <= 1.50) }"; then
            note=" ⚠"
        else
            note=" ℹ"
        fi
    fi
    printf "| %-40s | %6s ms | %6s ms | %6s ms | %6s ms | %5s | %5s |%s\n" \
        "$label" "$a" "$a_app" "$b" "$b_app" "$ratio" "$app_ratio" "$note"
}

print_table_header() {
    local ref_label=$1
    printf "| %-40s | %-9s | %-9s | %-9s | %-9s | %-5s | %-5s |\n" \
        "Case" "my_fs tot" "my_fs app" "ref tot" "ref app" "Total" "App"
    printf "|%s|%s|%s|%s|%s|%s|%s|\n" \
        "$(printf '%42s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%11s' '' | tr ' ' '-')" \
        "$(printf '%7s' '' | tr ' ' '-')" \
        "$(printf '%7s' '' | tr ' ' '-')"
}

# Warm-up
warmup_disk() {
    echo "Warming up CPU and page cache for disk-usage cases..." >&2

    # 1. Spin CPU to push host governor toward performance.
    awk 'BEGIN { x=0; for (i=0; i<5000000; i++) x+=i }' >/dev/null

    # 2. Pre-run baselines.
    local i
    for i in $(seq 1 "$WARMUP_ROUNDS"); do
        $BUSYBOX true >/dev/null 2>&1
        /bin/true     >/dev/null 2>&1
    done

    # 3. Pre-run every (binary, argv) pair we will measure.
    for i in $(seq 1 "$WARMUP_ROUNDS"); do
        $BUSYBOX my_fs          >/dev/null 2>&1
        $BUSYBOX my_fs /        >/dev/null 2>&1
        $BUSYBOX my_fs -h       >/dev/null 2>&1
        $BUSYBOX my_fs -i       >/dev/null 2>&1
        $BUSYBOX my_fs -t tmpfs >/dev/null 2>&1
        df                       >/dev/null 2>&1
        df /                     >/dev/null 2>&1
        df -h                    >/dev/null 2>&1
        df -i                    >/dev/null 2>&1
        df -t tmpfs              >/dev/null 2>&1
        $BUSYBOX df              >/dev/null 2>&1
        $BUSYBOX df /            >/dev/null 2>&1
        $BUSYBOX df -h           >/dev/null 2>&1
        $BUSYBOX df -i           >/dev/null 2>&1
    done
    echo "Disk warm-up complete." >&2
}

warmup_frag() {
    local testfile=$1
    echo "Warming up for fragmentation cases..." >&2
    local i
    for i in $(seq 1 "$WARMUP_ROUNDS"); do
        $BUSYBOX my_fs -f "$testfile" >/dev/null 2>&1
        filefrag -v "$testfile"        >/dev/null 2>&1
    done
}

# Create a fragmented test file:
#   1. Fill the filesystem close to capacity with sequentially allocated spacers.
#   2. Delete even-indexed spacers so isolated 4 KB holes remain between the
#      odd-indexed spacers.
#   3. Write the target file. When the allocator cannot find two contiguous
#      blocks, each 4 KB chunk lands in a separate hole, producing many
#      independent extents.
create_frag_file() {
    local mnt=$1
    local target="$mnt/frag_target"
    local i n

    # Create one 4 KB zero-filled template, then copy it repeatedly to avoid
    # forking dd for every spacer.
    dd if=/dev/zero of="$mnt/_z" bs=4k count=1 status=none
    printf "  Filling filesystem with 4 KB spacers..." >&2
    i=0
    while [ $i -lt 1200 ] && cp "$mnt/_z" "$mnt/sp_$i" 2>/dev/null; do
        i=$((i+1))
    done
    n=$i
    rm -f "$mnt/_z"
    sync
    printf " %d files.\n" "$n" >&2

    # Delete even-indexed spacers so each even block becomes a hole separated by
    # odd-indexed spacers.
    printf "  Freeing alternating spacers..." >&2
    for i in $(seq 0 2 $((n-1))); do
        rm -f "$mnt/sp_$i"
    done
    sync
    printf " done.\n" >&2

    # Write the target file so each 4 KB chunk is forced into an isolated hole,
    # producing separate extents.
    local chunks=$(( n / 4 ))
    [ "$chunks" -gt 200 ] && chunks=200
    printf "  Writing %d-chunk fragmented target..." "$chunks" >&2
    dd if=/dev/urandom of="$target" bs=4k count="$chunks" status=none
    sync
    local extents
    extents=$(filefrag "$target" 2>/dev/null | awk '{print $2}')
    printf " %s extents.\n" "$extents" >&2
}

# Header
echo "# my_fs Performance Benchmark (warmed)"
echo ""
printf -- "- Date: %s\n" "$(date)"
printf -- "- Kernel: %s\n" "$(uname -r)"
printf -- "- Runs per round: %d, rounds per case: %d (median reported)\n" "$REPEAT" "$MEDIAN_OF"
printf -- "- Warm-up rounds per binary: %d\n" "$WARMUP_ROUNDS"
printf -- "- busybox: %s\n" "$BUSYBOX"
echo ""

warmup_disk

# Startup baselines
SHELL_BASE=$(avg_ms true)
BB_BASE=$(avg_ms $BUSYBOX true)
REF_BASE=$(avg_ms /bin/true)
ARCH_OVERHEAD=$(awk "BEGIN { printf \"%.2f\", $BB_BASE - $REF_BASE }")

echo "## Startup Baselines"
echo ""
printf -- "- Shell loop only (builtin \`true\`, no fork): **%s ms/iter** (sanity check; should be ≈ 0)\n" "$SHELL_BASE"
printf -- "- Dynamic-linked binary (\`/bin/true\`): **%s ms/iter**  — baseline for \`df\` / \`filefrag\`\n" "$REF_BASE"
printf -- "- BusyBox static binary (\`./busybox true\`): **%s ms/iter**  — baseline for \`my_fs\`\n" "$BB_BASE"
printf -- "- Fixed architectural overhead (static − dynamic): **%s ms/iter**\n" "$ARCH_OVERHEAD"
echo ""
echo "App time = total − startup baseline.  \"—\" means my_fs app ≤ 0 or ref app ≤ noise floor (noise floor = 0 ms for disk cases, ${MIN_APP_MS} ms for fragmentation cases)."
echo "Ratio = my_fs / reference tool.  Goal: Total Ratio ≤ 1.50 (within 50%)."
echo ""
echo "Status legend (appears after each row):"
echo "  (none)  Total ≤ 1.50              — proposal target met"
echo "  ℹ       Total > 1.50, App ≤ 1.50  — architectural (BusyBox startup tax, not a code issue)"
echo "  ⚠?      Total > 1.50, App at noise floor — cannot decide if application is slower"
echo "  ⚠       Total > 1.50, App > 1.50  — application logic actually slower than reference"
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

# Case 2: all mounts
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

# Same-framework section (my_fs vs BusyBox df)
# Both sides run inside the same $BUSYBOX binary, so the BusyBox static
# startup tax is identical and cancels out in the Total ratio.
# Therefore this section's Total ratio is the authoritative metric, without
# architectural-startup-tax interference. The App ratio uses ./busybox true as
# the baseline on both sides and is kept only for continuity.
# BusyBox df has no -t TYPE filter, so the -t tmpfs case is skipped.
echo ""
echo "## Disk Usage — Same Framework (\`my_fs\` vs BusyBox \`df\`)"
echo ""

if ! $BUSYBOX df / >/dev/null 2>&1; then
    echo "> Skipped: this build has no BusyBox \`df\` applet (CONFIG_DF disabled)."
else
    echo "Both sides run inside the same \`$BUSYBOX\` binary, so the BusyBox startup"
    echo "tax is identical and cancels in the Total ratio. Here **Total ratio is the"
    echo "authoritative metric** (no architectural-tax confound): Total > 1.50 means"
    echo "\`my_fs\` application logic is genuinely slower than BusyBox \`df\`, not a"
    echo "framework artefact. App columns are baselined against \`./busybox true\`."
    echo ""
    print_table_header "bb df"

    # Case 1: single path /
    t_mine=$(avg_ms $BUSYBOX my_fs /)
    t_ref=$(avg_ms $BUSYBOX df /)
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$BB_BASE")
    row "my_fs / vs bb df /" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

    # Case 2: all mounts
    t_mine=$(avg_ms $BUSYBOX my_fs)
    t_ref=$(avg_ms $BUSYBOX df)
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$BB_BASE")
    row "my_fs (all mounts) vs bb df" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

    # Case 3: -h
    t_mine=$(avg_ms $BUSYBOX my_fs -h)
    t_ref=$(avg_ms $BUSYBOX df -h)
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$BB_BASE")
    row "my_fs -h vs bb df -h" "$t_mine" "$mine_app" "$t_ref" "$ref_app"

    # Case 4: -i
    t_mine=$(avg_ms $BUSYBOX my_fs -i)
    t_ref=$(avg_ms $BUSYBOX df -i)
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$BB_BASE")
    row "my_fs -i vs bb df -i" "$t_mine" "$mine_app" "$t_ref" "$ref_app"
fi

# FIEMAP section
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
    EXT4_IMG="$TMPDIR_WORK/bench.ext4.img"
    LOOP_MNT="$TMPDIR_WORK/mnt_bench"
    mkdir -p "$LOOP_MNT"
    dd if=/dev/zero of="$EXT4_IMG" bs=1M count=8 status=none
    mkfs.ext4 -q -F -b 4096 "$EXT4_IMG"
    LOOP_DEV=$(losetup --find --show "$EXT4_IMG")
    mount "$LOOP_DEV" "$LOOP_MNT"

    create_frag_file "$LOOP_MNT"
    TESTFILE="$LOOP_MNT/frag_target"
    EXTENT_COUNT=$(filefrag "$TESTFILE" 2>/dev/null | awk '{print $2}')

    warmup_frag "$TESTFILE"

    print_table_header "filefrag"

    t_mine=$(avg_ms $BUSYBOX my_fs -f "$TESTFILE")
    t_ref=$(avg_ms filefrag -v "$TESTFILE")
    mine_app=$(app_ms "$t_mine" "$BB_BASE")
    ref_app=$(app_ms "$t_ref" "$REF_BASE")
    row "my_fs -f vs filefrag -v ($EXTENT_COUNT extents)" "$t_mine" "$mine_app" "$t_ref" "$ref_app" "$MIN_APP_MS"

    umount "$LOOP_MNT"
    losetup -d "$LOOP_DEV"
    LOOP_DEV=""; LOOP_MNT=""
fi
