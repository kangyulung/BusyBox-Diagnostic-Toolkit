#!/bin/bash

# --- Paths ---
BB_my_proc="./busybox my_proc"
BB_top="./busybox top"
GNU_top="top"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Default values
ITERATIONS=${ITERATIONS:-500}
DELAY=${DELAY:-0.1}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--iterations)
            if [[ -n "$2" && "$2" != -* ]]; then
                ITERATIONS="$2"
                shift 2
            else
                echo "Error: Argument for $1 is missing or invalid."
                exit 1
            fi
            ;;
        -d|--delay)
            if [[ -n "$2" && "$2" != -* ]]; then
                DELAY="$2"
                shift 2
            else
                echo "Error: Argument for $1 is missing or invalid."
                exit 1
            fi
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -i, --iterations <num>  Number of iterations (default: 5)"
            echo "  -d, --delay <secs>      Delay between updates in seconds (default: 0.1)"
            echo "  -h, --help              Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown parameter: $1"
            exit 1
            ;;
    esac
done

echo -e "${YELLOW}Starting benchmark (Iterations: $ITERATIONS)...${NC}"
echo "---------------------------------------------------------------------"

# --- 1. Execution Time ---
echo -n "[1/3] Testing execution time... "

export TIMEFORMAT="%U %S"

# Test BB_top
BB_TOP_TIMES=$({ time $BB_top -b -n "$ITERATIONS" -d "$DELAY" >/dev/null 2>&1; } 2>&1)
BB_DIFF_TOP=$(echo "$BB_TOP_TIMES" | awk '{printf "%.0f", ($1 + $2) * 1000}')
[ -z "$BB_DIFF_TOP" ] || [ "$BB_DIFF_TOP" -le 0 ] && BB_DIFF_TOP=1

# Test GNU_top
GNU_TOP_TIMES=$({ time $GNU_top -b -n "$ITERATIONS" -d "$DELAY" >/dev/null 2>&1; } 2>&1)
GNU_DIFF_TOP=$(echo "$GNU_TOP_TIMES" | awk '{printf "%.0f", ($1 + $2) * 1000}')
[ -z "$GNU_DIFF_TOP" ] || [ "$GNU_DIFF_TOP" -le 0 ] && GNU_DIFF_TOP=1

# Test my_proc
MY_TIMES=$({ time $BB_my_proc -b -n "$ITERATIONS" -d "$DELAY" >/dev/null 2>&1; } 2>&1)
BB_DIFF_MY=$(echo "$MY_TIMES" | awk '{printf "%.0f", ($1 + $2) * 1000}')
[ -z "$BB_DIFF_MY" ] || [ "$BB_DIFF_MY" -le 0 ] && BB_DIFF_MY=1

echo "Done"

# --- 2. Memory Peak (using VmHWM: Peak Resident Set Size) ---
echo -n "[2/3] Testing memory consumption... "

get_peak_mem() {
    local pid=$1
    local max_mem=1
    while [ -d "/proc/$pid" ]; do
        local mem=""
        
        while read -r key val _; do
            if [ "$key" = "VmHWM:" ]; then
                mem=$val
                break
            fi
        done < "/proc/$pid/status" 2>/dev/null

        if [ -n "$mem" ] && [ "$mem" -gt "$max_mem" ]; then
            max_mem=$mem
        fi
        sleep 0.05
    done
    echo "$max_mem"
}

$BB_top -b -n "$ITERATIONS" -d "$DELAY" > /dev/null 2>&1 &
BB_TOP_PID=$!
BB_RSS_TOP=$(get_peak_mem $BB_TOP_PID)
wait $BB_TOP_PID 2>/dev/null

$GNU_top -b -n "$ITERATIONS" -d "$DELAY" > /dev/null 2>&1 &
GNU_TOP_PID=$!
GNU_RSS_TOP=$(get_peak_mem $GNU_TOP_PID)
wait $GNU_TOP_PID 2>/dev/null

# Capture Peak RSS for my_proc
$BB_my_proc -b -n "$ITERATIONS" -d "$DELAY" > /dev/null 2>&1 &
BB_MY_PID=$!
BB_RSS_MY=$(get_peak_mem $BB_MY_PID)
wait $BB_MY_PID 2>/dev/null

echo "Done"


# --- 3. System Calls (Capture total count) ---
echo -n "[3/3] Testing system calls... "
SYSCALL_BB_TOP=$(strace -c $BB_top -b -n "$ITERATIONS" -d "$DELAY" 2>&1 >/dev/null | awk '/total/ {print $4}' | tr -dc '0-9')
SYSCALL_GNU_TOP=$(strace -c $GNU_top -b -n "$ITERATIONS" -d "$DELAY" 2>&1 >/dev/null | awk '/total/ {print $4}' | tr -dc '0-9')
SYSCALL_BB_MY=$(strace -c $BB_my_proc -b -n "$ITERATIONS" -d "$DELAY" 2>&1 >/dev/null | awk '/total/ {print $4}' | tr -dc '0-9')

if [ -z "$SYSCALL_BB_TOP" ]; then
    SYSCALL_BB_TOP=$(strace $BB_top -b -n "$ITERATIONS" -d "$DELAY" 2>&1 | wc -l)
    SYSCALL_GNU_TOP=$(strace $GNU_top -b -n "$ITERATIONS" -d "$DELAY" 2>&1 | wc -l)
    SYSCALL_BB_MY=$(strace $BB_my_proc -b -n "$ITERATIONS" -d "$DELAY" 2>&1 | wc -l)
fi

SYSCALL_BB_TOP=${SYSCALL_BB_TOP:-0}
SYSCALL_GNU_TOP=${SYSCALL_GNU_TOP:-0}
SYSCALL_BB_MY=${SYSCALL_BB_MY:-0}
echo "Done"
echo "---------------------------------------------------------------------"

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

    if awk -v g="$gap" 'BEGIN { exit !(g <= 50) }'; then
        echo -e "${GREEN}PASS${NC}"
    else
        echo -e "${RED}FAIL (>50%)${NC}"
    fi
}

echo -e "${YELLOW}Performance Comparison Report---Iterations: $ITERATIONS${NC}"
echo "Metric          | top (busybox) | my_proc      | Gap (%)     | Result"
echo "----------------|---------------|--------------|-------------|-------"
analyze_gap "Execution Time" "$BB_DIFF_TOP" "$BB_DIFF_MY" "ms"
analyze_gap "Peak Memory" "$BB_RSS_TOP" "$BB_RSS_MY" "KB"
analyze_gap "Syscall Count" "$SYSCALL_BB_TOP" "$SYSCALL_BB_MY" "times"
echo "---------------------------------------------------------------------"
echo "Metric          | top (GNU)     | my_proc      | Gap (%)     | Result"
echo "----------------|---------------|--------------|-------------|-------"
analyze_gap "Execution Time" "$GNU_DIFF_TOP" "$BB_DIFF_MY" "ms"
analyze_gap "Peak Memory" "$GNU_RSS_TOP" "$BB_RSS_MY" "KB"
analyze_gap "Syscall Count" "$SYSCALL_GNU_TOP" "$SYSCALL_BB_MY" "times"
echo "---------------------------------------------------------------------"
