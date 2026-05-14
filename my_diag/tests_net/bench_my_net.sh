#!/bin/bash
# vi: set sw=4 ts=4:
# Performance benchmark: my_net vs ss(8) / netstat(8)
# Usage: bash bench_my_net.sh [path/to/busybox]
#
# Measures wall-clock time and system call count for equivalent operations.
# Goal: my_net overhead within 50% of reference tool (ratio <= 1.50).
#
# Output: Markdown table to stdout (redirect to .md if desired).
# Example: bash bench_my_net.sh ./busybox > bench_result.md

BUSYBOX="${1:-./busybox}"
REPEAT=20   # runs per case for averaging

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# ── Preflight ─────────────────────────────────────────────────────
if [ ! -x "$BUSYBOX" ]; then
    echo "ERROR: $BUSYBOX not found or not executable" >&2
    exit 1
fi

# ── Helper: average wall-clock time in ms ─────────────────────────
# Runs the command $REPEAT times in a single loop to amortize startup;
# then divides by REPEAT.
avg_ms() {
    local elapsed
    elapsed=$( {
        TIMEFORMAT='%R'
        time for i in $(seq 1 "$REPEAT"); do
            "$@" >/dev/null 2>&1
        done
    } 2>&1 )
    awk "BEGIN { printf \"%.2f\", $elapsed * 1000 / $REPEAT }"
}

# ── Helper: total syscall count (single run) ──────────────────────
# Uses strace -c; falls back to counting raw strace lines if -c fails.
syscall_count() {
    local cnt
    cnt=$(strace -c "$@" >/dev/null 2>&1 | awk '/total/ {print $NF}' | tr -dc '0-9')
    if [ -z "$cnt" ]; then
        cnt=$(strace "$@" 2>&1 >/dev/null | wc -l)
    fi
    echo "${cnt:-0}"
}

# ── Helper: peak RSS in KB (VmHWM from /proc) ─────────────────────
peak_rss() {
    "$@" >/dev/null 2>&1 &
    local pid=$!
    local rss
    rss=$(grep VmHWM /proc/$pid/status 2>/dev/null | awk '{print $2}')
    wait "$pid" 2>/dev/null
    echo "${rss:-0}"
}

# ── Table row printer ─────────────────────────────────────────────
# $1=label  $2=my_val  $3=ref_val  $4=unit
row() {
    local label="$1" my_val="$2" ref_val="$3" unit="$4"
    local ratio note color

    if [ -z "$ref_val" ] || [ "$ref_val" = "0" ]; then
        printf "| %-38s | %10s %s | %10s %s | %6s | %s\n" \
            "$label" "$my_val" "$unit" "N/A" "$unit" "N/A" "SKIP"
        return
    fi

    ratio=$(awk "BEGIN { printf \"%.2f\", $my_val / $ref_val }")
    if awk "BEGIN { exit !($ratio <= 1.50) }"; then
        color="$GREEN"; note="PASS"
    else
        color="$RED";   note="FAIL (>50%)"
    fi
    printf "| %-38s | %10s %s | %10s %s | %6s | %b%s%b\n" \
        "$label" "$my_val" "$unit" "$ref_val" "$unit" \
        "$ratio" "$color" "$note" "$NC"
}

header() {
    echo ""
    echo "## $1"
    echo ""
    printf "| %-38s | %-13s | %-13s | %-6s | %s\n" \
        "Case" "my_net" "Reference" "Ratio" "Result"
    printf "|%s|%s|%s|%s|%s\n" \
        "$(printf '%40s' '' | tr ' ' '-')" \
        "$(printf '%15s' '' | tr ' ' '-')" \
        "$(printf '%15s' '' | tr ' ' '-')" \
        "$(printf '%8s'  '' | tr ' ' '-')" \
        "$(printf '%12s' '' | tr ' ' '-')"
}

# ═══════════════════════════════════════════════════════════════════
# Report Header
# ═══════════════════════════════════════════════════════════════════
echo "# my_net Performance Benchmark"
echo ""
printf -- "- Date    : %s\n" "$(date)"
printf -- "- Kernel  : %s\n" "$(uname -r)"
printf -- "- busybox : %s\n" "$BUSYBOX"
printf -- "- Runs    : %d per case\n" "$REPEAT"
echo ""
echo "Ratio = my\_net / reference tool."
echo "Goal  : ratio ≤ 1.50 (overhead within 50% of reference)."

# ═══════════════════════════════════════════════════════════════════
# Section 1: Wall-Clock Time vs ss
# ═══════════════════════════════════════════════════════════════════
if ! has_cmd ss; then
    echo ""
    echo "## Wall-Clock Time (skipped: ss not found)"
else
    header "Wall-Clock Time — \`my_net\` vs \`ss\`"

    # 1a. All TCP connections
    t_my=$(avg_ms  $BUSYBOX my_net -b)
    t_ss=$(avg_ms  ss -tn)
    row "TCP only  (my_net -b  vs  ss -tn)"  "$t_my" "$t_ss" "ms"

    # 1b. All TCP+UDP (-a)
    t_my=$(avg_ms  $BUSYBOX my_net -a -b)
    t_ss=$(avg_ms  ss -tnup)
    row "TCP+UDP   (my_net -a -b  vs  ss -tnup)" "$t_my" "$t_ss" "ms"

    # 1c. Listening only (-l)
    t_my=$(avg_ms  $BUSYBOX my_net -l -b)
    t_ss=$(avg_ms  ss -tln)
    row "LISTEN    (my_net -l -b  vs  ss -tln)" "$t_my" "$t_ss" "ms"

    # 1d. State filter
    t_my=$(avg_ms  $BUSYBOX my_net -s ESTABLISHED -b)
    t_ss=$(avg_ms  ss -tn state established)
    row "ESTAB     (my_net -s ESTABLISHED -b  vs  ss -tn state established)" \
        "$t_my" "$t_ss" "ms"

    # 1e. UDP only
    t_my=$(avg_ms  $BUSYBOX my_net -u -b)
    t_ss=$(avg_ms  ss -un)
    row "UDP only  (my_net -u -b  vs  ss -un)" "$t_my" "$t_ss" "ms"
fi

# ═══════════════════════════════════════════════════════════════════
# Section 2: Wall-Clock Time vs netstat (if available)
# ═══════════════════════════════════════════════════════════════════
if ! has_cmd netstat; then
    echo ""
    echo "## Wall-Clock Time vs netstat (skipped: netstat not found)"
else
    header "Wall-Clock Time — \`my_net\` vs \`netstat\`"

    t_my=$(avg_ms  $BUSYBOX my_net -b)
    t_ns=$(avg_ms  netstat -tn)
    row "TCP       (my_net -b  vs  netstat -tn)" "$t_my" "$t_ns" "ms"

    t_my=$(avg_ms  $BUSYBOX my_net -a -b)
    t_ns=$(avg_ms  netstat -tnup)
    row "TCP+UDP   (my_net -a -b  vs  netstat -tnup)" "$t_my" "$t_ns" "ms"

    t_my=$(avg_ms  $BUSYBOX my_net -l -b)
    t_ns=$(avg_ms  netstat -tln)
    row "LISTEN    (my_net -l -b  vs  netstat -tln)" "$t_my" "$t_ns" "ms"
fi

# ═══════════════════════════════════════════════════════════════════
# Section 3: System Call Count (single run)
# ═══════════════════════════════════════════════════════════════════
if ! has_cmd strace; then
    echo ""
    echo "## System Call Count (skipped: strace not found)"
else
    header "System Call Count (single run) — \`my_net\` vs \`ss\`"

    if has_cmd ss; then
        sc_my=$(syscall_count $BUSYBOX my_net -b)
        sc_ss=$(syscall_count ss -tn)
        row "TCP syscalls  (my_net -b  vs  ss -tn)" "$sc_my" "$sc_ss" "calls"

        sc_my=$(syscall_count $BUSYBOX my_net -a -b)
        sc_ss=$(syscall_count ss -tnup)
        row "TCP+UDP       (my_net -a -b  vs  ss -tnup)" "$sc_my" "$sc_ss" "calls"
    else
        echo "> Skipped: ss not found"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
# Section 4: Peak Memory (VmHWM) — single run each
# ═══════════════════════════════════════════════════════════════════
header "Peak Resident Memory (VmHWM, single run)"

rss_my=$(peak_rss $BUSYBOX my_net -b)
if has_cmd ss; then
    rss_ref=$(peak_rss ss -tn)
    row "my_net -b  vs  ss -tn" "$rss_my" "$rss_ref" "KB"
fi
if has_cmd netstat; then
    rss_ref=$(peak_rss netstat -tn)
    row "my_net -b  vs  netstat -tn" "$rss_my" "$rss_ref" "KB"
fi

# ═══════════════════════════════════════════════════════════════════
# Section 5: Output Correctness Spot-Check
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "## Correctness Spot-Check"
echo ""

check() {
    local label="$1" result="$2"
    if [ "$result" = "ok" ]; then
        printf "${GREEN}PASS${NC} %s\n" "$label"
    else
        printf "${RED}FAIL${NC} %s — %s\n" "$label" "$result"
    fi
}

# ESTABLISHED count vs ss (allow ±5 tolerance)
if has_cmd ss; then
    my_est=$($BUSYBOX my_net -s ESTABLISHED -b 2>/dev/null | grep -cE '^tcp' || echo 0)
    ss_est=$(ss -tn 2>/dev/null | grep -c ESTAB || echo 0)
    diff_e=$(( my_est > ss_est ? my_est - ss_est : ss_est - my_est ))
    if [ "$diff_e" -le 5 ]; then
        check "ESTABLISHED count matches ss (±5): my=$my_est ss=$ss_est" "ok"
    else
        check "ESTABLISHED count" "my=$my_est ss=$ss_est diff=$diff_e (>5)"
    fi

    # LISTEN count vs ss (allow ±3)
    my_lst=$($BUSYBOX my_net -l -b 2>/dev/null | grep -cE '^tcp' || echo 0)
    ss_lst=$(ss -tln 2>/dev/null | grep -c LISTEN || echo 0)
    diff_l=$(( my_lst > ss_lst ? my_lst - ss_lst : ss_lst - my_lst ))
    if [ "$diff_l" -le 3 ]; then
        check "LISTEN count matches ss (±3): my=$my_lst ss=$ss_lst" "ok"
    else
        check "LISTEN count" "my=$my_lst ss=$ss_lst diff=$diff_l (>3)"
    fi
fi

# Batch mode produces no ANSI escape codes
if $BUSYBOX my_net -b 2>/dev/null | grep -qP '\x1b\['; then
    check "-b batch mode: no ANSI codes" "ANSI codes found in output"
else
    check "-b batch mode: no ANSI codes" "ok"
fi

# Header columns present
hdr=$($BUSYBOX my_net -b 2>/dev/null | head -1)
if echo "$hdr" | grep -q "Proto" && echo "$hdr" | grep -q "State"; then
    check "Output header contains Proto + State columns" "ok"
else
    check "Output header" "missing columns: $hdr"
fi

# TCP summary block present
if $BUSYBOX my_net -b 2>/dev/null | grep -q "TCP state summary"; then
    check "TCP state summary block present" "ok"
else
    check "TCP state summary block" "missing"
fi

# ═══════════════════════════════════════════════════════════════════
# Analysis
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "## Analysis"
echo ""
echo "- Time cases measure \`/proc/net/tcp\` read + address parsing + PID resolution overhead."
echo "- PID resolution (\`/proc/PID/fd\` scan) is the main overhead vs \`ss\` which uses netlink."
echo "- A ratio > 1.50 suggests optimization opportunities:"
echo "  * Cache inode→PID map across calls (if used in watch mode)"
echo "  * Use \`openat\`/\`getdents\` instead of \`opendir\`/\`readdir\` for fd scanning"
echo "  * Skip PID resolution with \`-n\` flag for faster raw output"
echo "- Memory overhead is expected: BusyBox binary includes all applets."
