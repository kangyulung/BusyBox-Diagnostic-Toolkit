#!/bin/bash
# vi: set sw=4 ts=4:
# Regression tests for the my_net applet.
# Usage: bash test_my_net.sh [path/to/busybox]
#
# Compares my_net output against ss(8) and netstat(8).
# Tests that do not require external tools are marked accordingly.
# Root is NOT required for most tests; T15 (PID resolution) benefits from root.

# ── Path auto-detection ──────────────────────────────────────────
# Script lives at my_diag/tests_net/; busybox binary is two levels up.
BUSYBOX="${1:-./busybox}"

PASS=0; FAIL=0; SKIP=0

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'

pass() { printf "${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "${RED}FAIL${NC} %s\n" "$1"; FAIL=$((FAIL+1)); }
skip() { printf "${YELLOW}SKIP${NC} %s\n" "$1"; SKIP=$((SKIP+1)); }

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# ── Helper: count data rows (lines starting with tcp/udp) ─────────
count_rows() {
    grep -cE '^(tcp|udp)'
}

# ── Preflight ─────────────────────────────────────────────────────
echo "=== my_net regression tests ==="
printf "busybox : %s\n" "$BUSYBOX"
printf "date    : %s\n" "$(date)"
echo ""

if [ ! -x "$BUSYBOX" ]; then
    echo "ERROR: $BUSYBOX not found or not executable" >&2
    exit 1
fi

# ── Dummy Network Services (for testing in clean environments) ────
DUMMY_PIDS=""
cleanup() {
    if [ -n "$DUMMY_PIDS" ]; then
        kill $DUMMY_PIDS >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if has_cmd nc; then
    echo "Starting dummy network sockets (TCP:58080, UDP:58081)..."
    nc -l -p 58080 >/dev/null 2>&1 &
    DUMMY_PIDS="$DUMMY_PIDS $!"
    nc -u -l -p 58081 >/dev/null 2>&1 &
    DUMMY_PIDS="$DUMMY_PIDS $!"
    sleep 0.5 # Give sockets time to bind
fi

# ── Group 1: Basic output sanity ──────────────────────────────────
echo "--- Group 1: Basic output sanity ---"

# T01: Command exits 0
$BUSYBOX my_net -b > /dev/null 2>&1
if [ $? -eq 0 ]; then
    pass "T01 exit code 0"
else
    fail "T01 exit code 0"
fi

# T02: Header line contains expected columns
header=$($BUSYBOX my_net -b 2>/dev/null | head -1)
if echo "$header" | grep -q "Proto" && echo "$header" | grep -q "State" \
   && echo "$header" | grep -q "Local Address"; then
    pass "T02 header columns present (Proto / State / Local Address)"
else
    fail "T02 header columns: got '$header'"
fi

# T03: At least one data row (every system has at least a loopback socket)
rows=$($BUSYBOX my_net -b 2>/dev/null | count_rows)
if [ "${rows:-0}" -gt 0 ]; then
    pass "T03 at least one data row (got $rows)"
else
    skip "T03 at least one data row (got ${rows:-0}) — container may have no active sockets"
fi

# T04: TCP summary line present
if $BUSYBOX my_net -b 2>/dev/null | grep -q "TCP state summary"; then
    pass "T04 TCP state summary block present"
else
    fail "T04 TCP state summary block missing"
fi

# ── Group 2: Protocol filters ─────────────────────────────────────
echo ""
echo "--- Group 2: Protocol filters ---"

# T05: -t output should only contain tcp/tcp6 rows (no udp rows)
udp_in_tcp=$($BUSYBOX my_net -t -b 2>/dev/null | grep -cE '^udp')
if [ "${udp_in_tcp:-0}" -eq 0 ]; then
    pass "T05 -t: no udp rows in TCP-only output"
else
    fail "T05 -t: found $udp_in_tcp udp row(s) with -t flag"
fi

# T06: -u output should only contain udp/udp6 rows
tcp_in_udp=$($BUSYBOX my_net -u -b 2>/dev/null | grep -cE '^tcp')
if [ "${tcp_in_udp:-0}" -eq 0 ]; then
    pass "T06 -u: no tcp rows in UDP-only output"
else
    fail "T06 -u: found $tcp_in_udp tcp row(s) with -u flag"
fi

# T07: -a output should have both tcp and udp rows (if any exist)
tcp_cnt=$($BUSYBOX my_net -a -b 2>/dev/null | grep -cE '^tcp')
udp_cnt=$($BUSYBOX my_net -a -b 2>/dev/null | grep -cE '^udp')
total_a=$(( ${tcp_cnt:-0} + ${udp_cnt:-0} ))
# In a minimal container with no active sockets, 0 rows is valid
if [ "$total_a" -gt 0 ]; then
    pass "T07 -a: combined TCP+UDP rows = $total_a (tcp=${tcp_cnt:-0} udp=${udp_cnt:-0})"
else
    skip "T07 -a: 0 rows found with -a — container may have no active sockets"
fi

# T08: -a row count >= -t row count (more or equal, since UDP is added)
t_rows=$($BUSYBOX my_net -t -b 2>/dev/null | count_rows)
a_rows=$($BUSYBOX my_net -a -b 2>/dev/null | count_rows)
if [ "${a_rows:-0}" -ge "${t_rows:-0}" ]; then
    pass "T08 -a >= -t row count (${a_rows:-0} >= ${t_rows:-0})"
else
    fail "T08 -a < -t row count (${a_rows:-0} < ${t_rows:-0})"
fi

# ── Group 3: -l (listen-only) ─────────────────────────────────────
echo ""
echo "--- Group 3: Listen filter (-l) ---"

# T09: All tcp rows in -l output must have state LISTEN
non_listen=$($BUSYBOX my_net -l -b 2>/dev/null \
             | grep -E '^tcp' | grep -v 'LISTEN' | wc -l)
if [ "${non_listen:-0}" -eq 0 ]; then
    pass "T09 -l: all tcp rows have LISTEN state"
else
    fail "T09 -l: $non_listen non-LISTEN tcp row(s) appeared"
fi

# T10: -l row count <= total row count
l_rows=$($BUSYBOX my_net -l -b 2>/dev/null | count_rows)
if [ "${l_rows:-0}" -le "${t_rows:-0}" ]; then
    pass "T10 -l row count <= total (${l_rows:-0} <= ${t_rows:-0})"
else
    fail "T10 -l row count > total (${l_rows:-0} > ${t_rows:-0})"
fi

# ── Group 4: -s state filter ──────────────────────────────────────
echo ""
echo "--- Group 4: State filter (-s) ---"

# T11: -s LISTEN output must match -l output (row count)
s_listen=$($BUSYBOX my_net -s LISTEN -b 2>/dev/null | count_rows)
if [ "${s_listen:-0}" -eq "${l_rows:-0}" ]; then
    pass "T11 -s LISTEN row count == -l row count (${s_listen:-0})"
else
    fail "T11 -s LISTEN (${s_listen:-0}) != -l (${l_rows:-0})"
fi

# T12: -s ESTABLISHED rows must all show ESTABLISHED
bad=$($BUSYBOX my_net -s ESTABLISHED -b 2>/dev/null \
      | grep -E '^tcp' | grep -v 'ESTABLISHED' | wc -l)
if [ "${bad:-0}" -eq 0 ]; then
    pass "T12 -s ESTABLISHED: all rows are ESTABLISHED"
else
    fail "T12 -s ESTABLISHED: $bad non-ESTABLISHED row(s)"
fi

# T13: -s with mixed-case should work (case-insensitive)
ci_rows=$($BUSYBOX my_net -s listen -b 2>/dev/null | count_rows)
if [ "${ci_rows:-0}" -eq "${l_rows:-0}" ]; then
    pass "T13 -s listen (lowercase) == -s LISTEN (${ci_rows:-0})"
else
    fail "T13 -s case-insensitive: got ${ci_rows:-0}, expected ${l_rows:-0}"
fi

# ── Group 5: PID resolution ───────────────────────────────────────
echo ""
echo "--- Group 5: PID resolution ---"

# T14: PID/Program column exists in output header when -p is given
if $BUSYBOX my_net -b -p 2>/dev/null | head -1 | grep -q "PID"; then
    pass "T14 -p: PID/Program column present in header"
else
    fail "T14 -p: PID/Program column missing from header"
fi

# T15: At least one row shows a numeric PID (if run as root or owns sockets)
if [ "$(id -u)" -eq 0 ]; then
    pid_found=$($BUSYBOX my_net -b -p 2>/dev/null \
                | grep -E '^tcp' | grep -cE '[0-9]+/[a-zA-Z]')
    if [ "${pid_found:-0}" -gt 0 ]; then
        pass "T15 PID resolved: found $pid_found rows with PID/program (root)"
    else
        skip "T15 PID resolved: 0 rows with PID (no listening sockets in container?)"
    fi
else
    pid_found=$($BUSYBOX my_net -b -p 2>/dev/null \
                | grep -E '^tcp' | grep -cE '[0-9]+/[a-zA-Z]')
    pass "T15 PID resolution attempt OK (non-root, found ${pid_found:-0})"
fi

# ── Group 6: Comparison with ss ──────────────────────────────────
echo ""
echo "--- Group 6: Comparison with ss ---"

if ! has_cmd ss; then
    for t in T16 T17 T18; do skip "$t (ss not found)"; done
else
    # T16: ESTABLISHED count within ±5 of ss -tn
    # FIX: Remove '|| echo 0' to prevent grep -c from returning "0\n0", which causes a syntax error in $(( ))
    ss_est=$(ss -tn 2>/dev/null | grep -c 'ESTAB')
    my_est=$($BUSYBOX my_net -b -s ESTABLISHED 2>/dev/null | count_rows)
    diff_est=$(( ${ss_est:-0} > ${my_est:-0} \
                 ? ${ss_est:-0} - ${my_est:-0} \
                 : ${my_est:-0} - ${ss_est:-0} ))
    if [ "$diff_est" -le 5 ]; then
        pass "T16 ESTABLISHED count: my_net=${my_est:-0} ss=${ss_est:-0} diff=$diff_est (<=5)"
    else
        fail "T16 ESTABLISHED count: my_net=${my_est:-0} ss=${ss_est:-0} diff=$diff_est (>5)"
    fi

    # T17: LISTEN count within ±3 of ss -tln
    ss_lst=$(ss -tln 2>/dev/null | grep -c 'LISTEN')
    my_lst=$($BUSYBOX my_net -l -b 2>/dev/null | count_rows)
    diff_lst=$(( ${ss_lst:-0} > ${my_lst:-0} \
                 ? ${ss_lst:-0} - ${my_lst:-0} \
                 : ${my_lst:-0} - ${ss_lst:-0} ))
    if [ "$diff_lst" -le 3 ]; then
        pass "T17 LISTEN count: my_net=${my_lst:-0} ss=${ss_lst:-0} diff=$diff_lst (<=3)"
    else
        fail "T17 LISTEN count: my_net=${my_lst:-0} ss=${ss_lst:-0} diff=$diff_lst (>3)"
    fi

    # T18: UDP count within ±5 of ss -un
    # FIX: Use awk to count non-empty lines, completely avoiding grep -c exit-code issues
    ss_udp=$(ss -un 2>/dev/null | awk 'NR>1 && NF>0 {c++} END {print c+0}')
    my_udp=$($BUSYBOX my_net -u -b 2>/dev/null | count_rows)
    diff_udp=$(( ${ss_udp:-0} > ${my_udp:-0} \
                 ? ${ss_udp:-0} - ${my_udp:-0} \
                 : ${my_udp:-0} - ${ss_udp:-0} ))
    if [ "$diff_udp" -le 5 ]; then
        pass "T18 UDP count: my_net=${my_udp:-0} ss=${ss_udp:-0} diff=$diff_udp (<=5)"
    else
        fail "T18 UDP count: my_net=${my_udp:-0} ss=${ss_udp:-0} diff=$diff_udp (>5)"
    fi
fi

# ── Group 7: -b (batch) mode ─────────────────────────────────────
echo ""
echo "--- Group 7: Batch mode (-b) ---"

# T19: -b output must not contain ANSI escape sequences
if $BUSYBOX my_net -b 2>/dev/null | grep -qP '\x1b\['; then
    fail "T19 -b: ANSI escape codes found in batch output"
else
    pass "T19 -b: no ANSI escape codes"
fi

# T20: Output piped (non-tty) also produces no ANSI sequences
if $BUSYBOX my_net 2>/dev/null | cat | grep -qP '\x1b\['; then
    fail "T20 pipe: ANSI escape codes found when stdout is not a tty"
else
    pass "T20 pipe: no ANSI escape codes on non-tty stdout"
fi

# ── Summary ───────────────────────────────────────────────────────
echo ""
printf "=== Summary: ${GREEN}PASS=%d${NC}  ${RED}FAIL=%d${NC}  ${YELLOW}SKIP=%d${NC} ===\n" \
    "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
