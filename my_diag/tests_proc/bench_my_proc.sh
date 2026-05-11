#!/bin/bash

# --- Paths ---
BB="./busybox"
APPLET="my_proc"
LOG_FILE="bench_result.log"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Check tools
for cmd in strace awk "$BB"; do
    if ! command -v "$cmd" &> /dev/null && [ ! -f "$cmd" ]; then
        echo -e "${RED}Error: Tool $cmd not found${NC}"
        exit 1
    fi
done

ITERATIONS=50
DELAY=0.1
TOTAL_SLEEP_MS=$(awk -v n="$ITERATIONS" -v d="$DELAY" 'BEGIN { printf "%.0f", n * d * 1000 }')

echo -e "${YELLOW}Starting benchmark (Iterations: $ITERATIONS)...${NC}"
echo "--------------------------------------------------------"

# --- 1. Execution Time ---
echo -n "[1/3] Testing execution time... "
S_TOP=$(date +%s%N)
top -b -n "$ITERATIONS" -d "$DELAY" > /dev/null 2>&1
E_TOP=$(date +%s%N)
DIFF_TOP=$((( (E_TOP - S_TOP) / 1000000 )- TOTAL_SLEEP_MS))
[ $DIFF_TOP -le 0 ] && DIFF_TOP=1 # Ensure non-zero for calculation

# Test my_proc
S_MY=$(date +%s%N)
$BB $APPLET -b -n "$ITERATIONS" -d "$DELAY" > /dev/null 2>&1
E_MY=$(date +%s%N)
DIFF_MY=$((( (E_MY - S_MY) / 1000000 )- TOTAL_SLEEP_MS))
[ $DIFF_MY -le 0 ] && DIFF_MY=1

echo "Done"

# --- 2. Memory Peak (using VmHWM: Peak Resident Set Size) ---
echo -n "[2/3] Testing memory consumption... "

top -b -n 1 > /dev/null 2>&1 &
TOP_PID=$!
RSS_TOP=$(grep "VmHWM" /proc/$TOP_PID/status 2>/dev/null | awk '{print $2}')
wait $TOP_PID 2>/dev/null

# Capture Peak RSS for my_proc
$BB $APPLET -b -n 1 > /dev/null 2>&1 &
MY_PID=$!
RSS_MY=$(grep "VmHWM" /proc/$MY_PID/status 2>/dev/null | awk '{print $2}')
wait $MY_PID 2>/dev/null

echo "Done"

# --- 3. System Calls (Capture total count) ---
echo -n "[3/3] Testing system calls... "
# Use strace -c to analyze total syscalls
SYSCALL_TOP=$(strace -c top -b -n 1 > /dev/null 2>&1 | awk '/total/ {print $NF}' | tr -dc '0-9')
SYSCALL_MY=$(strace -c $BB $APPLET -b -n 1 > /dev/null 2>&1 | awk '/total/ {print $NF}' | tr -dc '0-9')

# Handle cases where strace -c might fail (e.g., Docker)
if [ -z "$SYSCALL_TOP" ]; then
    SYSCALL_TOP=$(strace top -b -n 1 2>&1 > /dev/null | wc -l)
    SYSCALL_MY=$(strace $BB $APPLET -b -n 1 2>&1 > /dev/null | wc -l)
fi

SYSCALL_TOP=${SYSCALL_TOP:-0}
SYSCALL_MY=${SYSCALL_MY:-0}

echo "Done"
echo "--------------------------------------------------------"

analyze_gap() {
    local label=$1
    local v_orig=$2
    local v_my=$3
    local unit=$4

    if [ -z "$v_orig" ] || [ "$v_orig" -eq 0 ] || [ -z "$v_my" ]; then
        printf "%-15s | %-13s | %-12s | %-10s | %b\n" "$label" "ERR" "ERR" "N/A" "${RED}FAIL${NC}"
        return
    fi

    local gap=$(awk -v v1="$v_my" -v v2="$v_orig" 'BEGIN { printf "%.2f", (v1 - v2) * 100 / v2 }')
    printf "%-15s | %-13s | %-12s | %-10s%% | " "$label" "$v_orig $unit" "$v_my $unit" "$gap"

    # Optimization target: gap within 50%
    if awk -v g="$gap" 'BEGIN { exit !(g <= 50) }'; then
        echo -e "${GREEN}PASS${NC}"
    else
        echo -e "${RED}FAIL (>50%)${NC}"
    fi
}

echo -e "${YELLOW}Performance Comparison Report (top vs. $APPLET)${NC}"
echo "Metric          | Original Tool | $APPLET      | Gap (%)     | Result"
echo "----------------|---------------|--------------|-------------|-------"
analyze_gap "Execution Time" "$DIFF_TOP" "$DIFF_MY" "ms"
analyze_gap "Peak Memory" "$RSS_TOP" "$RSS_MY" "KB"
analyze_gap "Syscall Count" "$SYSCALL_TOP" "$SYSCALL_MY" "times"
echo "--------------------------------------------------------"
