#!/bin/bash

# vi: set sw=4 ts=4:

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [OPTIONS]"
    echo "Set up a virtual network connection environment to test my_net."
    echo ""
    echo "Options:"
    echo "  -d, --dynamic   Start dynamic simulation mode (random connections continuously created/dropped)"
    echo "  -w, --warn      Start anomaly simulation mode (generates massive connections to trigger warnings)"
    echo "  -h, --help      Display this help message and exit"
    echo ""
    echo "Running without options starts a static virtual network environment."
    exit 0
fi

# Check for root privileges (required for -p to read /proc/PID/fd of other processes)
if [ "$EUID" -ne 0 ]; then
  echo "WARNING: Some tests (like -p for PID) require root privileges. Running with sudo is recommended."
fi

# Check if nc (netcat) is installed
if ! command -v nc &> /dev/null; then
    echo "ERROR: This script requires 'nc' (netcat) to establish connections. Please install it (e.g., apt install netcat-openbsd)"
    exit 1
fi

# Check IPv6 connectivity
HAS_IPV6=0
if command -v ping6 &> /dev/null; then
    if ping6 -c 1 -W 1 2001:4860:4860::8888 >/dev/null 2>&1; then
        HAS_IPV6=1
    fi
elif command -v ping &> /dev/null; then
    # Some systems use ping -6
    if ping -6 -c 1 -W 1 2001:4860:4860::8888 >/dev/null 2>&1; then
        HAS_IPV6=1
    fi
fi
if [ "$HAS_IPV6" -eq 0 ]; then
    echo "WARNING: No external IPv6 connectivity detected. Skipping related tests."
fi

DYNAMIC_MODE=0
if [ "$1" = "-d" ] || [ "$1" = "--dynamic" ]; then
    DYNAMIC_MODE=1
fi

WARN_MODE=0
HAS_IPTABLES=0
if [ "$1" = "-w" ] || [ "$1" = "--warn" ]; then
    WARN_MODE=1
fi

PIDS=()
cleanup() {
    echo "[-] Cleaning up background connection processes..."
    if [ ${#PIDS[@]} -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null
        sleep 0.2
        kill -9 "${PIDS[@]}" 2>/dev/null || true
    fi
    if [ -n "$DYNAMIC_PID" ]; then
        # Kill child processes (nc) first, then parent processes to prevent nc from becoming orphans
        pkill -P $DYNAMIC_PID 2>/dev/null
        kill -9 $DYNAMIC_PID 2>/dev/null
    fi
    if [ "$HAS_IPTABLES" -eq 1 ]; then
        iptables -D OUTPUT -p tcp -o lo --sport 60004 --tcp-flags SYN,ACK SYN,ACK -j DROP 2>/dev/null || true
        iptables -D OUTPUT -p tcp -o lo --sport 60005 --tcp-flags FIN FIN -j DROP 2>/dev/null || true
    fi
    # Ultimate failsafe: ensure no phantom netcat processes remain in the background (compat with minimal env)
    killall nc 2>/dev/null || pkill nc 2>/dev/null || true
    if command -v pidof >/dev/null 2>&1 && [ -n "$(pidof nc 2>/dev/null)" ]; then
        kill -9 $(pidof nc) 2>/dev/null
    fi
    echo "=== Cleanup completed ==="
}
trap cleanup EXIT

# Generate random 127.x.y.z IPs to increase Local Address variation
rand_local_ip() {
    echo "127.$((RANDOM % 254 + 1)).$((RANDOM % 254 + 1)).$((RANDOM % 254 + 1))"
}

if [ $DYNAMIC_MODE -eq 1 ]; then
    echo "=== Starting Dynamic Network Simulation Mode ==="
    echo "Will continuously generate random connections to simulate a fluctuating real-world network..."
    
    dynamic_worker() {
        local EXT_IPS=("8.8.8.8" "1.1.1.1" "142.250.181.110" "104.21.23.50" "9.9.9.9")
        local EXT_PORTS=("80" "443" "53")
        local EXT_IPS6=("2001:4860:4860::8888" "2606:4700:4700::1111" "2404:6800:4008:c01::8b" "2620:0:ccc::2")
        local WORKER_PIDS=()
        
        while true; do
            # Randomly select an action for smoother visual changes
            case $((RANDOM % 5)) in
                0) # Establish external IPv4 connection
                    local ip=${EXT_IPS[$RANDOM % ${#EXT_IPS[@]}]}
                    local port=${EXT_PORTS[$RANDOM % ${#EXT_PORTS[@]}]}
                    nc -w 10 $ip $port >/dev/null 2>&1 &
                    WORKER_PIDS+=($!)
                    ;;
                1) # Establish external IPv6 connection (if available)
                    if [ "$HAS_IPV6" -eq 1 ]; then
                        local ip6=${EXT_IPS6[$RANDOM % ${#EXT_IPS6[@]}]}
                        local port=${EXT_PORTS[$RANDOM % ${#EXT_PORTS[@]}]}
                        nc -6 -w 10 $ip6 $port >/dev/null 2>&1 &
                        WORKER_PIDS+=($!)
                    fi
                    ;;
                2) # Establish internal IPv4 interconnection
                    local local_ip=$(rand_local_ip)
                    local lport=$((RANDOM % 10000 + 40000))
                    nc -l -s $local_ip -p $lport >/dev/null 2>&1 &
                    WORKER_PIDS+=($!)
                    sleep 0.1
                    nc $local_ip $lport >/dev/null 2>&1 &
                    WORKER_PIDS+=($!)
                    ;;
                3) # Establish internal IPv6 interconnection
                    local lport=$((RANDOM % 10000 + 40000))
                    nc -6 -l -s ::1 -p $lport >/dev/null 2>&1 &
                    WORKER_PIDS+=($!)
                    sleep 0.1
                    nc -6 ::1 $lport >/dev/null 2>&1 &
                    WORKER_PIDS+=($!)
                    ;;
                4) # Establish UDP listener
                    local uport=$((RANDOM % 10000 + 50000))
                    if [ $((RANDOM % 2)) -eq 0 ]; then
                        nc -u -l -s $(rand_local_ip) -p $uport >/dev/null 2>&1 &
                    else
                        nc -6 -u -l -s ::1 -p $uport >/dev/null 2>&1 &
                    fi
                    WORKER_PIDS+=($!)
                    ;;
            esac
            
            # Control process count, randomly delete oldest connections to simulate disconnections
            if [ ${#WORKER_PIDS[@]} -gt 12 ]; then
                local kill_cnt=$((RANDOM % 3 + 1))
                for ((i=0; i<kill_cnt; i++)); do
                    kill ${WORKER_PIDS[$i]} 2>/dev/null
                done
                WORKER_PIDS=("${WORKER_PIDS[@]:$kill_cnt}")
            fi
            
            sleep 1.5
        done
    }
    
    dynamic_worker &
    DYNAMIC_PID=$!
    
elif [ $WARN_MODE -eq 1 ]; then
    echo "=== Starting Anomaly Simulation Mode ==="
    echo "NOTE: Simulating 10000+ connections requires a very high File Descriptor limit (ulimit -n)"
    
    ulimit -n 65535 2>/dev/null || echo "WARNING: Failed to raise ulimit. ESTABLISHED test might not reach 10000."

    if [ "$EUID" -eq 0 ] && command -v iptables &> /dev/null; then
        HAS_IPTABLES=1
        echo "[+] root and iptables detected. Injecting rules to block handshakes for SYN_RECV and LAST_ACK simulation..."
        iptables -I OUTPUT -p tcp -o lo --sport 60004 --tcp-flags SYN,ACK SYN,ACK -j DROP
        iptables -I OUTPUT -p tcp -o lo --sport 60005 --tcp-flags FIN FIN -j DROP
    else
        echo "WARNING: Root privileges and iptables are required to simulate SYN_RECV and LAST_ACK. Skipping these."
    fi

    python3 -c '
import socket, time, sys, resource
try:
    resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))
except:
    pass

servers = []
clients = []

def setup_server(port, backlog=20000):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(backlog)
    servers.append(s)
    return s

print("[+] Binding all test ports (to avoid Ephemeral Port conflicts)...")
s_est = setup_server(60000)
s_tw = setup_server(60001)
s_cw = setup_server(60002)
if sys.argv[1] == "1":
    s_sr = setup_server(60004)
    s_la = setup_server(60005)

print("[+] Generating 10005 ESTABLISHED connections (takes a few seconds)...")
for i in range(10005):
    try:
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.connect(("127.0.0.1", 60000))
        conn, _ = s_est.accept()
        clients.extend([c, conn])
    except Exception as e:
        print("  ! ESTABLISHED connection generation terminated early (Count: %d): %s" % (i, e))
        break

print("[+] Generating 520 TIME_WAIT...")
for i in range(520):
    c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    c.connect(("127.0.0.1", 60001))
    conn, _ = s_tw.accept()
    c.close()
    conn.close()

print("[+] Generating 120 CLOSE_WAIT & 120 FIN_WAIT2...")
for i in range(120):
    c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    c.connect(("127.0.0.1", 60002))
    conn, _ = s_cw.accept()
    c.close() # Client FIN_WAIT2
    clients.append(conn) # Server CLOSE_WAIT

if sys.argv[1] == "1":
    print("[+] Generating 110 SYN_RECV...")
    for i in range(110):
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.setblocking(False)
        try: c.connect(("127.0.0.1", 60004))
        except: pass
        clients.append(c)
        
    print("[+] Generating 60 LAST_ACK...")
    for i in range(60):
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.connect(("127.0.0.1", 60005))
        conn, _ = s_la.accept()
        c.close()
        conn.close()

print("[+] Anomaly states established. Keeping alive in the background...")
time.sleep(999999)
' "$HAS_IPTABLES" &
    PIDS+=($!)
    sleep 2
else
    echo "=== Starting Static Virtual Network Environment ==="
    
    IP1=$(rand_local_ip)
    IP2=$(rand_local_ip)
    IP3=$(rand_local_ip)

    # 1. Create TCP LISTEN states (IPv4 and IPv6)
    echo "[+] Creating TCP LISTEN connections (IP: $IP1)..."
    nc -l -s $IP1 -p 8080 >/dev/null 2>&1 &
    PIDS+=($!)
    nc -6 -l -p 8081 >/dev/null 2>&1 &
    PIDS+=($!)

    # 2. Create TCP ESTABLISHED states (IPv4 and IPv6)
    echo "[+] Creating TCP ESTABLISHED connections (IP: $IP2)..."
    nc -l -s $IP2 -p 8082 >/dev/null 2>&1 &
    PIDS+=($!)
    sleep 0.2
    nc $IP2 8082 >/dev/null 2>&1 &
    PIDS+=($!)

    nc -6 -l -p 8083 >/dev/null 2>&1 &
    PIDS+=($!)
    sleep 0.2
    nc -6 ::1 8083 >/dev/null 2>&1 &
    PIDS+=($!)
    
    # External connections (Simulate real outbound connections)
    echo "[+] Creating outbound TCP connection (to 8.8.8.8:53)..."
    nc -w 300 8.8.8.8 53 >/dev/null 2>&1 &
    PIDS+=($!)
    if [ "$HAS_IPV6" -eq 1 ]; then
        echo "[+] Creating outbound IPv6 TCP connection (to Google DNS)..."
        nc -6 -w 300 2001:4860:4860::8888 53 >/dev/null 2>&1 &
        PIDS+=($!)
    fi

    # 3. Create UDP states (IPv4 and IPv6)
    echo "[+] Creating UDP listeners (IP: $IP3)..."
    nc -u -l -s $IP3 -p 9090 >/dev/null 2>&1 &
    PIDS+=($!)
    nc -6 -u -l -p 9091 >/dev/null 2>&1 &
    PIDS+=($!)

    # 4. Simulate special TCP state: CLOSE_WAIT
    echo "[+] Simulating TCP CLOSE_WAIT state..."
    nc -l -s $IP1 -p 8084 >/dev/null 2>&1 &
    PID_CW_SERVER=$!
    PIDS+=($PID_CW_SERVER)
    sleep 0.2
    python3 -c '
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("'$IP1'", 8084))
time.sleep(1000)
' &
    PIDS+=($!)
    sleep 0.2
    kill $PID_CW_SERVER 2>/dev/null
fi

echo "------------------------------------------------"
if [ $DYNAMIC_MODE -eq 1 ]; then
    echo "Dynamic simulation environment started! Connections will continuously be created and dropped randomly."
elif [ $WARN_MODE -eq 1 ]; then
    echo "Anomaly simulation environment started! Please run the applet to verify if [ANOMALY DETECTED] warnings appear."
else
    echo "Virtual connections established!"
fi
echo "Please run your 'my_net' in the \"same terminal\" or a \"new window\" to test."
echo "Recommended test commands:"
echo "  1. Watch mode (Recommended):   sudo ./my_net -w 1 -a -p"
echo "  2. Show all connections & PID: sudo ./my_net -a -p"
echo "  3. Show only listening state:  ./my_net -l"
echo "------------------------------------------------"
echo "Press [ENTER] to automatically clean up all background processes and exit..."
read