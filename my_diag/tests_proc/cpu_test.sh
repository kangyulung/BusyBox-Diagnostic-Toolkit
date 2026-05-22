#!/bin/bash

# Show usage information
show_help() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -m, --memory <MB>     Memory load size in MB"
    echo "  -c, --cpu <cores>     Number of CPU cores to keep busy at 100%"
    echo "  -t, --time <seconds>  Test duration in seconds (default: indefinite)"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Example: $0 -m 512 -c 2 -t 60  (hold 512 MB of memory and 2 CPU cores for 60 seconds)"
}

# Default values
MEM_TO_ALLOC=0
CPU_CORES=0
DURATION=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--memory)
            MEM_TO_ALLOC="$2"
            shift 2
            ;;
        -c|--cpu)
            CPU_CORES="$2"
            shift 2
            ;;
        -t|--time)
            DURATION="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            show_help
            exit 1
            ;;
    esac
done

# Check that at least one workload was requested
if [ "$MEM_TO_ALLOC" -eq 0 ] && [ "$CPU_CORES" -eq 0 ]; then
    echo "ERROR: Specify a memory load (-m) or CPU core count (-c)."
    show_help
    exit 1
fi

# Cleanup handler
cleanup() {
    echo -e "\n[+] Releasing resources and cleaning up..."
    # Kill all background CPU load processes started by this script
    if [ -n "$CPU_PIDS" ]; then
        kill $CPU_PIDS 2>/dev/null
    fi
    # Release memory-holding variables
    unset MEM_HOLDER
    echo "[+] Cleanup complete. Exiting."
    exit 0
}

# Handle Ctrl+C or termination signals
trap cleanup SIGINT SIGTERM EXIT

# ==================== 1. Memory Load (RAM) ====================
# Fill RAM directly with a Bash array so the memory is charged to this Bash
# process's RSS.
if [ "$MEM_TO_ALLOC" -gt 0 ]; then
    echo "[+] Allocating memory: ${MEM_TO_ALLOC} MB..."
    
    # Create an approximately 1 MB string as the base block (1,048,576 bytes).
    # Use repeated characters to speed up allocation.
    BLOCK=$(printf '%1048576s' 'X')
    
    # Declare an array to hold allocated memory.
    declare -a MEM_HOLDER
    
    for ((i=1; i<=MEM_TO_ALLOC; i++)); do
        MEM_HOLDER[$i]="$BLOCK"
        # Print progress every 100 MB to avoid looking stuck on large allocations.
        if (( i % 100 == 0 )); then
            echo "[+] Successfully allocated $i MB..."
        fi
    done
    echo "[+] Memory allocation complete. This Bash process now holds about ${MEM_TO_ALLOC} MB RSS."
fi

# ==================== 2. CPU Load ====================
CPU_PIDS=""
if [ "$CPU_CORES" -gt 0 ]; then
    echo "[+] Starting $CPU_CORES CPU load process(es)..."
    for i in $(seq 1 "$CPU_CORES"); do
        # Run sha1sum in the background to keep one CPU core busy.
        sha1sum /dev/zero & 
        CPU_PIDS="$CPU_PIDS $!"
    done
    echo "[+] CPU load processes are running in the background."
fi

# ==================== 3. Duration Control ====================
if [ "$DURATION" -gt 0 ]; then
    echo "[+] Resources will be held for $DURATION seconds..."
    sleep "$DURATION"
else
    echo "[+] My PID is: $$"
    echo "[+] Resources are being held. Observe my_proc from another terminal. Press [Ctrl+C] to stop."
    while true; do
        sleep 1
    done
fi
