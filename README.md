# BusyBox Diagnostic Toolkit

[繁體中文](README.zh_TW.md) | **English**

A lightweight system diagnostic toolkit built on the BusyBox architecture. Implements three new applets in C, following POSIX/UNIX command-line interface conventions, compiled into a single static binary, with shared logic extracted into the internal library `libdiag`.

> **Course:** UNIX Systems Programming (Academic Year 114, Semester 2, Course 54015)
> **Option B · Direction 1 — System Diagnostics Toolkit**

---

## Applets Overview

| Applet | Source | Description |
|--------|--------|-------------|
| `my_fs` | `my_diag/diag_fs.c` | Filesystem health checker: disk usage, inode analysis, fragmentation |
| `my_proc` | `my_diag/diag_proc.c` | Process resource analyzer: real-time monitoring, tree view, interactive sorting |
| `my_net` | `my_diag/diag_net.c` | Network connection monitor: TCP/UDP socket listing, TCP state tracking |

---

## Quick Start

### Requirements

- **Docker**
  - macOS: [colima](https://github.com/abiosoft/colima) is recommended; Docker Desktop and other Docker Engines also work
  - Linux: Install Docker Engine directly
  - Windows: 
  
- **Bash** (to run `dev.sh` / `compile.sh` / `format.sh`)

### First-Time Setup

```bash
bash ./dev.sh
```

This command automatically:

1. Builds the Docker image (`busybox-dev-env`)
2. Starts the container and mounts the project directory
3. Runs `make defconfig` inside the container with static linking forced
4. Performs a full build and drops into an interactive bash (`root@...:/home/project/busybox#`)

> **Note:** The first run takes 5–10 minutes; subsequent runs are fast due to Docker caching.

### Rebuilding After Changes

Run inside the container:

```bash
# Rebuild only
bash ./compile.sh

# Format all my_diag/ .c / .h files with clang-format before building
bash ./compile.sh --fmt
```

`compile.sh` steps:

1. Remove auto-generated `my_diag/Config.in` and `my_diag/Kbuild` (prevents stale `INSERT` placeholders)
2. Clean all intermediate build artifacts (`.o`, `lib.a`, etc.) from `my_diag/`, preserving source files and documentation
3. (If `--fmt` is passed) Run `format.sh` to apply `clang-format -i` to `my_diag/*.c` and `my_diag/*.h`
4. `make defconfig` with static linking enabled; on non-x86 platforms (e.g., ARM64 colima) SHA hardware acceleration is automatically disabled
5. Full rebuild with `make -j$(nproc)`

> `format.sh` can also be run standalone (`bash ./format.sh`) to format without triggering a build.

### Running the Applets

```bash
./busybox my_fs  [OPTION]... [PATH]...
./busybox my_proc [OPTION]...
./busybox my_net  [OPTION]...
```

---

## my_fs — Filesystem Health Checker

### Features

- Disk usage display compatible with GNU `df(1)` output format
- Inode usage analysis (`-i`), compatible with `df -i`
- Dual-perspective Use% (user view / real view including root-reserved blocks) with reserved block info (`-r`)
- Per-file FIEMAP extent analysis (`-f FILE`), modeled on `filefrag -v` output
- Mount-point fragmentation statistics (`-F PATH`): full `nftw` traversal with Top 10 most fragmented files
- Interactive TUI live-monitoring mode (`-s`)

### Options

```
my_fs [-h] [-i] [-r] [-t TYPE] [-x TYPE] [-f FILE] [-F PATH] [-s] [PATH]...

  -h          Human-readable sizes (K/M/G/T)
  -i          Show inode usage instead of block usage
  -r          Dual-perspective Use% (user/real) with root-reserved block columns
  -t TYPE     Show only filesystems of TYPE (e.g. ext4, tmpfs)
  -x TYPE     Exclude filesystems of TYPE
  -f FILE     FIEMAP extent analysis for FILE
  -F PATH     Fragmentation statistics for the filesystem at PATH
  -s          Interactive TUI (D/I/R/F/H/Q to switch views)
```

**TUI keys:** `D` Disk capacity · `I` Inode · `R` Reserved blocks · `F` Fragmentation · `H` Toggle human-readable · `Q` Quit

### Examples

```bash
# List all mounted filesystems (human-readable)
./busybox my_fs -h

# Check inode usage on the root filesystem
./busybox my_fs -i /

# Show dual-perspective Use% with reserved blocks (most useful on ext4)
./busybox my_fs -rh /

# Analyze extent layout of a single file
./busybox my_fs -f /etc/passwd

# Scan fragmentation under a mount point
./busybox my_fs -F /

# Launch TUI in human-readable mode
./busybox my_fs -sh
```

---

## my_proc — Process Resource Analyzer

### Features

- Real-time process list with automatic per-second updates of CPU and memory statistics
- Tree view built from PPID relationships to visualize parent-child hierarchies
- Multi-dimensional sorting (CPU / RSS / VSZ / PID / PPID / User / State / Command)
- Interactive operations: switch views, change sort order, and send signals to terminate processes at runtime
- Batch mode (disables terminal control sequences; suitable for logging or scripting)

### Options

```
my_proc [-t] [-d <seconds>] [-n <count>] [-p <pid>] [-b]

  -t       Start in tree view
  -d SEC   Update interval in seconds (default: 1.0)
  -n NUM   Exit after NUM updates
  -p PID   Monitor only the specified PID and related processes
  -b       Batch mode (disables TUI; suitable for file redirection)
```

### Interactive Keys

| Key | Action |
|-----|--------|
| `T` | Toggle between list view (TOP) and tree view (TREE) |
| `Q` | Quit |
| `K` | Kill process (prompts for PID, sends SIGTERM) |
| `P` | Sort by CPU usage (default) |
| `M` | Sort by RSS |
| `V` | Sort by VSZ |
| `I` | Sort by PID |
| `O` | Sort by PPID |
| `U` | Sort by username |
| `S` | Sort by process state |
| `C` | Sort by command name |

### Output Columns

| Column | Description |
|--------|-------------|
| `PID` | Process ID |
| `PPID` | Parent process ID |
| `THR` | Thread count |
| `USER` | Process owner |
| `STAT` | Process state (R: Running, S: Sleeping, D: Uninterruptible sleep, Z: Zombie, T: Stopped) |
| `NI` | Nice value (scheduling priority) |
| `VSZ` | Total virtual memory size |
| `RSS` | Resident Set Size (physical memory in use) |
| `%CPU` | CPU usage since last update |
| `%MEM` | Physical memory usage percentage |
| `TIME` | Total CPU time consumed by the process |
| `COMMAND` | Command name |

### Examples

```bash
# Start interactive real-time monitoring (default list view, sorted by CPU)
./busybox my_proc

# Start in tree view
./busybox my_proc -t

# Update every 3 seconds, exit after 5 iterations (suitable for scripts)
./busybox my_proc -b -d 3 -n 5

# Monitor only PID 1234
./busybox my_proc -p 1234

# Export a process snapshot to a file
./busybox my_proc -b -n 1 > proc_snapshot.txt
```

---

## my_net — Network Connection Monitor

### Features

- Reads `/proc/net/tcp[6]` and `/proc/net/udp[6]` to list socket information; output format is compatible with `ss(8)` and `netstat(8)`
- TCP state machine tracking: decodes the kernel's hex state codes into human-readable names (ESTABLISHED, TIME_WAIT, LISTEN, etc.)
- Connection anomaly detection: after each scan, compares per-state counts against thresholds and warns of SYN floods, connection leaks, TIME_WAIT accumulation, and more
- PID/program name resolution: builds an inode→PID map by scanning `/proc/<pid>/fd/` with `getdents64(2)` directly; binary search provides O(log n) lookup
- Watch mode: periodic screen refresh suitable for real-time terminal monitoring

### Options

```
my_net [-t] [-u] [-a] [-l] [-n] [-s STATE] [-w SEC] [-b] [-p]

  -t          TCP sockets (default)
  -u          UDP sockets
  -a          All sockets (TCP + UDP)
  -l          Listening sockets only
  -n          Numeric mode (all addresses are printed numerically)
  -s STATE    Filter by TCP state name (case-insensitive)
  -w SEC      Watch mode: refresh every SEC seconds (minimum 1; press Q to quit)
  -b          Batch mode (disables ANSI color; suitable for pipes and redirection)
  -p          Show PID/program name (root required for full visibility)
```

### Output Columns

| Column | Description |
|--------|-------------|
| `Proto` | Protocol: `tcp`, `tcp6`, `udp`, or `udp6` |
| `State` | TCP state name; `-` for UDP (stateless) |
| `Local Address` | Local IP:Port (IPv6 shown as `[addr]:port`) |
| `Foreign Address` | Remote IP:Port; `0.0.0.0:0` for listening sockets |
| `PID/Program` | Owning process as `pid/name` (only with `-p`; `-` if unresolvable) |
| `User` | Username of the socket owner (32-entry UID cache avoids redundant lookups) |

### TCP State Reference

| State | Code | Meaning |
|-------|------|---------|
| `ESTABLISHED` | 0x01 | Data transfer in progress |
| `SYN_SENT` | 0x02 | Active open: SYN sent, awaiting SYN-ACK |
| `SYN_RECV` | 0x03 | Passive open: SYN received, SYN-ACK sent |
| `FIN_WAIT1` | 0x04 | Active close: FIN sent, awaiting FIN or ACK |
| `FIN_WAIT2` | 0x05 | FIN acknowledged; awaiting remote FIN |
| `TIME_WAIT` | 0x06 | Waiting 2×MSL before full close |
| `CLOSE` | 0x07 | Connection fully closed |
| `CLOSE_WAIT` | 0x08 | Passive close: remote FIN received, local close pending |
| `LAST_ACK` | 0x09 | Passive close FIN sent; awaiting final ACK |
| `LISTEN` | 0x0A | Server socket accepting connections |
| `CLOSING` | 0x0B | Both sides closing simultaneously |

### Anomaly Detection Thresholds

After each scan, if any count exceeds its threshold, an `[ANOMALY DETECTED]` block is appended after the TCP state summary:

| State | Threshold | Likely Cause |
|-------|-----------|--------------|
| `TIME_WAIT` | > 500 | High connection churn; consider enabling `net.ipv4.tcp_tw_reuse` |
| `CLOSE_WAIT` | > 20 | Application not calling `close(2)` — connection leak |
| `SYN_RECV` | > 100 | Possible SYN flood attack |
| `LAST_ACK` | > 50 | Peer not responding to FIN (network fault or remote crash) |
| `FIN_WAIT2` | > 100 | Half-open connection backlog; check `net.ipv4.tcp_fin_timeout` |
| `ESTABLISHED` | > 10000 | Unusually high connection count; possible resource leak |

### Examples

```bash
# List all TCP connections (default)
./busybox my_net

# Show all TCP connections with PID and program name (root for full view)
./busybox my_net -p

# Show only listening sockets with owner
./busybox my_net -l -p

# List all sockets (TCP + UDP) in batch mode
./busybox my_net -a -b

# Filter to ESTABLISHED connections only
./busybox my_net -s ESTABLISHED

# Show UDP sockets with PID
./busybox my_net -u -p

# Watch all connections, refreshing every 3 seconds
./busybox my_net -a -w 3

# Count ESTABLISHED connections and compare with ss(8)
./busybox my_net -s ESTABLISHED -b | grep -cE '^tcp'
ss -tn | grep -c ESTAB

# Log connection state every 10 seconds to a file
./busybox my_net -a -b -w 10 >> /var/log/net_snapshot.log
```

---

## Directory Structure

```
.
├── my_diag/                # Project source code (the only directory that may be modified)
│   ├── libdiag.h / .c      # Shared internal library (diag_fs_t, diag_frag_t, etc.)
│   ├── diag_fs.c           # my_fs applet
│   ├── diag_proc.c         # my_proc applet
│   ├── diag_net.c          # my_net applet
│   ├── Config.src          # BusyBox config template (compile.sh generates Config.in)
│   ├── Kbuild.src          # BusyBox build template (compile.sh generates Kbuild)
│   ├── my_fs.1             # my_fs man page
│   ├── my_proc.1           # my_proc man page
│   ├── my_net.1            # my_net man page
│   ├── tests_fs/           # my_fs test scripts
│   ├── tests_proc/         # my_proc test scripts
│   └── tests_net/          # my_net test scripts
├── compile.sh              # In-container rebuild script (supports --fmt flag)
├── format.sh               # Format my_diag/*.c / *.h with clang-format
├── dev.sh                  # First-time environment setup script
├── Dockerfile              # Development container definition
├── local-docs/             # Course documents and development knowledge base (not versioned)
├── CONTRIBUTING.md         # Contribution guide (→ my_diag/CONTRIBUTING.md)
└── README.upstream         # Original BusyBox README
```

---

## Troubleshooting

| Symptom | Solution |
|---------|----------|
| First run is slow (5–10 min) | Expected; Docker image needs to install dependencies and perform a full build |
| Need a second shell into the container | `docker exec -it <container_id> bash` |
| Modified `Dockerfile` but changes are not picked up | `docker build --no-cache -t busybox-dev-env .` |

---

## Contributing

Please read [`my_diag/CONTRIBUTING.md`](my_diag/CONTRIBUTING.md) first.

- **Never push directly to `master`** — all work must go through a `feature/xxx` or `fix/xxx` branch
- Do not modify files outside `my_diag/`
- Coding style: tab indentation, snake_case, `diag_` prefix for internal functions
- Every new applet must include a man page and a Bash test script

---

## License

This toolkit is released under the GNU GPL v2, consistent with the upstream BusyBox project. See [`LICENSE`](LICENSE) for details.
