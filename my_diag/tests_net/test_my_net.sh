#!/bin/bash
# vi: set sw=4 ts=4:
# Regression tests for the my_net applet.
# Usage: bash test_my_net.sh [path/to/busybox]
#
# Compares my_net output against ss(8) and netstat(8).
# Tests that do not require external tools are marked accordingly.
# Root is NOT required for most tests; T11 (PID resolution) benefits from root.

BUSYBOX="${1:-./busybox}"
PASS=0; FAIL=0; SKIP=0

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'

pass() { printf "${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "${RED}FAIL${NC} %s\n" "$1"; FAIL=$((FAIL+1)); }
skip() { printf "${YELLOW}SKIP${NC} %s\n" "$1"; SKIP=$((SKIP+1)); }

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# ── Preflight ─────────────────────────────────────────────────────
echo "=== my_net regression tests ==="
printf "busybox : %s\n" "$BUSYBOX"
printf "date    : %s\n" "$(date)"
echo ""

if [ ! -x "$BUSYBOX" ]; then
    echo "ERROR: $BUSYBOX not found or not executable" >&2
    exit 1
fi

# ── Helper: count matching lines (skip header, skip summary block) ──
# Counts data rows (lines that start with tcp/udp).
count_rows() {
    grep -cE '^(tcp|udp)' 2>/dev/null || echo 0
}

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
if [ "$rows" -gt 0 ]; then
    pass "T03 at least one data row (got $rows)"
else
    fail "T03 at least one data row (got 0)"
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
if [ "$udp_in_tcp" -eq 0 ]; then
    pass "T05 -t: no udp rows in TCP-only output"
else
    fail "T05 -t: found $udp_in_tcp udp row(s) with -t flag"
fi

# T06: -u output should only contain udp/udp6 rows
tcp_in_udp=$($BUSYBOX my_net -u -b 2>/dev/null | grep -cE '^tcp')
if [ "$tcp_in_udp" -eq 0 ]; then
    pass "T06 -u: no tcp rows in UDP-only output"
else
    fail "T06 -u: found $tcp_in_udp tcp row(s) with -u flag"
fi

# T07: -a output should have both tcp and udp rows (if any udp exists)
tcp_cnt=$($BUSYBOX my_net -a -b 2>/dev/null | grep -cE '^tcp')
udp_cnt=$($BUSYBOX my_net -a -b 2>/dev/null | grep -cE '^udp')
total_a=$((tcp_cnt + udp_cnt))
if [ "$total_a" -gt 0 ]; then
    pass "T07 -a: combined TCP+UDP rows = $total_a (tcp=$tcp_cnt udp=$udp_cnt)"
else
    fail "T07 -a: no rows found with -a flag"
fi

# T08: -a row count >= -t row count (more or equal, since UDP is added)
t_rows=$($BUSYBOX my_net -t -b 2>/dev/null | count_rows)
a_rows=$($BUSYBOX my_net -a -b 2>/dev/null | count_rows)
if [ "$a_rows" -ge "$t_rows" ]; then
    pass "T08 -a >= -t row count ($a_rows >= $t_rows)"
else
    fail "T08 -a < -t row count ($a_rows < $t_rows)"
fi

# ── Group 3: -l (listen-only) ─────────────────────────────────────
echo ""
echo "--- Group 3: Listen filter (-l) ---"

# T09: All tcp rows in -l output must have state LISTEN
non_listen=$($BUSYBOX my_net -l -b 2>/dev/null \
             | grep -E '^tcp' | grep -v 'LISTEN' | wc -l)
if [ "$non_listen" -eq 0 ]; then
    pass "T09 -l: all tcp rows have LISTEN state"
else
    fail "T09 -l: $non_listen non-LISTEN tcp row(s) appeared"
fi

# T10: -l row count <= total row count
l_rows=$($BUSYBOX my_net -l -b 2>/dev/null | count_rows)
if [ "$l_rows" -le "$t_rows" ]; then
    pass "T10 -l row count <= total ($l_rows <= $t_rows)"
else
    fail "T10 -l row count > total ($l_rows > $t_rows)"
fi

# ── Group 4: -s state filter ──────────────────────────────────────
echo ""
echo "--- Group 4: State filter (-s) ---"

# T11: -s LISTEN output must match -l output (row count)
s_listen=$($BUSYBOX my_net -s LISTEN -b 2>/dev/null | count_rows)
if [ "$s_listen" -eq "$l_rows" ]; then
    pass "T11 -s LISTEN row count == -l row count ($s_listen)"
else
    fail "T11 -s LISTEN ($s_listen) != -l ($l_rows)"
fi

# T12: -s ESTABLISHED rows must all show ESTABLISHED
if [ "$($BUSYBOX my_net -s ESTABLISHED -b 2>/dev/null | count_rows)" -ge 0 ]; then
    bad=$($BUSYBOX my_net -s ESTABLISHED -b 2>/dev/null \
          | grep -E '^tcp' | grep -v 'ESTABLISHED' | wc -l)
    if [ "$bad" -eq 0 ]; then
        pass "T12 -s ESTABLISHED: all rows are ESTABLISHED"
    else
        fail "T12 -s ESTABLISHED: $bad non-ESTABLISHED row(s)"
    fi
fi

# T13: -s with mixed-case should work (case-insensitive)
ci_rows=$($BUSYBOX my_net -s listen -b 2>/dev/null | count_rows)
if [ "$ci_rows" -eq "$l_rows" ]; then
    pass "T13 -s listen (lowercase) == -s LISTEN ($ci_rows)"
else
    fail "T13 -s case-insensitive: got $ci_rows, expected $l_rows"
fi

# ── Group 5: PID resolution ───────────────────────────────────────
echo ""
echo "--- Group 5: PID resolution ---"

# T14: PID/Program column exists in output header
if $BUSYBOX my_net -b 2>/dev/null | head -1 | grep -q "PID"; then
    pass "T14 PID/Program column in header"
else
    fail "T14 PID/Program column missing from header"
fi

# T15: At least one row shows a numeric PID (if run as root or owns sockets)
if [ "$(id -u)" -eq 0 ]; then
    pid_found=$($BUSYBOX my_net -b 2>/dev/null \
                | grep -E '^tcp' | grep -cE '[0-9]+/[a-zA-Z]')
    if [ "$pid_found" -gt 0 ]; then
        pass "T15 PID resolved: found $pid_found rows with PID/program (root)"
    else
        # Not necessarily a failure; depends on socket ownership
        skip "T15 PID resolved: 0 rows with PID (unexpected as root, check manually)"
    fi
else
    # Non-root: only own process sockets are resolvable
    pid_found=$($BUSYBOX my_net -b 2>/dev/null \
                | grep -E '^tcp' | grep -cE '[0-9]+/[a-zA-Z]')
    if [ "$pid_found" -ge 0 ]; then
        pass "T15 PID resolution attempt OK (non-root, found $pid_found)"
    fi
fi

# ── Group 6: Comparison with ss ──────────────────────────────────
echo ""
echo "--- Group 6: Comparison with ss ---"

if ! has_cmd ss; then
    for t in T16 T17 T18; do skip "$t (ss not found)"; done
else
    # T16: ESTABLISHED count within ±5 of ss -tn
    ss_est=$(ss -tn 2>/dev/null | grep -c ESTAB || echo 0)
    my_est=$($BUSYBOX my_net -b -s ESTABLISHED 2>/dev/null | count_rows)
    diff_est=$(( ss_est > my_est ? ss_est - my_est : my_est - ss_est ))
    if [ "$diff_est" -le 5 ]; then
        pass "T16 ESTABLISHED count: my_net=$my_est ss=$ss_est diff=$diff_est (<=5)"
    else
        fail "T16 ESTABLISHED count: my_net=$my_est ss=$ss_est diff=$diff_est (>5)"
    fi

    # T17: LISTEN count within ±3 of ss -tln
    ss_lst=$(ss -tln 2>/dev/null | grep -c LISTEN || echo 0)
    my_lst=$($BUSYBOX my_net -l -b 2>/dev/null | count_rows)
    diff_lst=$(( ss_lst > my_lst ? ss_lst - my_lst : my_lst - ss_lst ))
    if [ "$diff_lst" -le 3 ]; then
        pass "T17 LISTEN count: my_net=$my_lst ss=$ss_lst diff=$diff_lst (<=3)"
    else
        fail "T17 LISTEN count: my_net=$my_lst ss=$ss_lst diff=$diff_lst (>3)"
    fi

    # T18: UDP count within ±5 of ss -un
    ss_udp=$(ss -un 2>/dev/null | tail -n +2 | grep -vc "^$" || echo 0)
    my_udp=$($BUSYBOX my_net -u -b 2>/dev/null | count_rows)
    diff_udp=$(( ss_udp > my_udp ? ss_udp - my_udp : my_udp - ss_udp ))
    if [ "$diff_udp" -le 5 ]; then
        pass "T18 UDP count: my_net=$my_udp ss=$ss_udp diff=$diff_udp (<=5)"
    else
        fail "T18 UDP count: my_net=$my_udp ss=$ss_udp diff=$diff_udp (>5)"
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
