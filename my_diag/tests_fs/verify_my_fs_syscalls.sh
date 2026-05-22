#!/bin/bash
# vi: set sw=4 ts=4:
# Syscall-level verification: my_fs -f vs filefrag -v.
#
# Collect per-tool syscall tables with strace -c and check whether my_fs truly
# eliminated the redundant statfs() calls (Option A: use fstat.st_blksize
# instead of an extra statfs).
#
# Difference from bench_my_fs_warmed.sh:
#   bench measures "time"  (noisy, requires noise floor / median)
#   this script measures "syscall count" (deterministic, unaffected by CPU
#   frequency or scheduling)
#
# Usage: sudo bash verify_my_fs_syscalls.sh [path/to/busybox]
# Requirements: root (loop device), strace, mkfs.ext4, filefrag

BUSYBOX="${1:-./busybox}"
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

die() { echo "ERROR: $*" >&2; exit 1; }
has_cmd() { command -v "$1" >/dev/null 2>&1; }

# ── Prerequisites ─────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ]   || die "root required (loop device and mount)"
has_cmd strace         || die "strace not found"
has_cmd mkfs.ext4      || die "mkfs.ext4 not found"
has_cmd filefrag       || die "filefrag not found"
[ -x "$BUSYBOX" ]      || die "busybox not found: $BUSYBOX"

# ── Create fragmented test file (same logic as bench_my_fs_warmed.sh, simplified) ──
EXT4_IMG="$TMPDIR_WORK/verify.ext4.img"
LOOP_MNT="$TMPDIR_WORK/mnt_verify"
mkdir -p "$LOOP_MNT"
dd if=/dev/zero of="$EXT4_IMG" bs=1M count=8 status=none
mkfs.ext4 -q -F -b 4096 "$EXT4_IMG"
LOOP_DEV=$(losetup --find --show "$EXT4_IMG")
mount "$LOOP_DEV" "$LOOP_MNT"

dd if=/dev/zero of="$LOOP_MNT/_z" bs=4k count=1 status=none
i=0
while [ $i -lt 1200 ] && cp "$LOOP_MNT/_z" "$LOOP_MNT/sp_$i" 2>/dev/null; do
    i=$((i+1))
done
n=$i
rm -f "$LOOP_MNT/_z"
sync
for i in $(seq 0 2 $((n-1))); do rm -f "$LOOP_MNT/sp_$i"; done
sync

TESTFILE="$LOOP_MNT/frag_target"
chunks=$(( n / 4 ))
[ "$chunks" -gt 200 ] && chunks=200
dd if=/dev/urandom of="$TESTFILE" bs=4k count="$chunks" status=none
sync
EXTENT_COUNT=$(filefrag "$TESTFILE" 2>/dev/null | awk '{print $2}')

echo "Test file: $TESTFILE ($EXTENT_COUNT extents)"
echo ""

# ── Collect syscall counts ────────────────────────────────────────
# strace -c writes its summary to stderr; -o redirects it to a file so the
# tool's own stderr does not interfere.
MY_TRACE="$TMPDIR_WORK/my_fs.strace"
REF_TRACE="$TMPDIR_WORK/filefrag.strace"

strace -c -o "$MY_TRACE"  "$BUSYBOX" my_fs -f "$TESTFILE" >/dev/null 2>&1
strace -c -o "$REF_TRACE" filefrag -v "$TESTFILE"          >/dev/null 2>&1

# Extract the call count for a single syscall from a strace -c table.
# strace -c format: % time | seconds | usecs/call | calls | errors | syscall
# The errors column may be blank, so use the syscall name as an anchor rather
# than relying on a fixed field position.
count_syscall() {
    local tracefile=$1 syscall=$2
    awk -v sc="$syscall" '
        $NF == sc {
            # Scan right-to-left from NF-1; the first all-digit field is "calls".
            # This handles the optional errors column without assuming a fixed position.
            for (i = NF-1; i >= 1; i--) {
                if ($i ~ /^[0-9]+$/) { print $i; exit }
            }
        }
    ' "$tracefile"
}

# Syscalls of interest (also shows libc vs. static-linking differences)
WATCH="statfs fstatfs fstat newfstatat stat ioctl openat open close read write mmap brk"

# ── Print comparison table ────────────────────────────────────────
printf "%-14s | %-12s | %-12s\n" "syscall" "my_fs -f" "filefrag -v"
printf "%s\n" "$(printf '%.0s-' {1..45})"
for sc in $WATCH; do
    a=$(count_syscall "$MY_TRACE"  "$sc")
    b=$(count_syscall "$REF_TRACE" "$sc")
    a=${a:-0}; b=${b:-0}
    # Skip syscalls with zero calls in both tools (keep output concise)
    [ "$a" = "0" ] && [ "$b" = "0" ] && continue
    printf "%-14s | %12s | %12s\n" "$sc" "$a" "$b"
done

echo ""
echo "Key check (Option A: print_file_frag no longer makes an extra statfs call):"
my_statfs=$(count_syscall "$MY_TRACE" statfs)
my_statfs=${my_statfs:-0}
if [ "$my_statfs" = "0" ]; then
    echo "  ✓ my_fs -f makes no statfs() calls (Option A effective)"
else
    echo "  ✗ my_fs -f still calls statfs() $my_statfs time(s) (Option A not effective — did you run compile.sh?)"
fi

echo ""
echo "Full strace -c output:"
echo "── my_fs -f ──"
cat "$MY_TRACE"
echo ""
echo "── filefrag -v ──"
cat "$REF_TRACE"
