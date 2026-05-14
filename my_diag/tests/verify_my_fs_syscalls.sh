#!/bin/bash
# vi: set sw=4 ts=4:
# Syscall-level verification: my_fs -f vs filefrag -v.
#
# 用 strace -c 蒐集每隻工具呼叫的 syscall 表，比對 my_fs 是否真的少做了
# 多餘的 statfs()（Option A：用 fstat.st_blksize 取代額外 statfs）。
#
# 與 bench_my_fs_warmed.sh 的差異：
#   bench 量「時間」（受雜訊影響、需 noise floor / median）
#   本腳本量「syscall 次數」（deterministic、不受 CPU 頻率與調度影響）
#
# 用法：sudo bash verify_my_fs_syscalls.sh [path/to/busybox]
# 需求：root（loop device）、strace、mkfs.ext4、filefrag

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

# ── 環境檢查 ──────────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ]   || die "需要 root（loop device 與 mount）"
has_cmd strace         || die "找不到 strace"
has_cmd mkfs.ext4      || die "找不到 mkfs.ext4"
has_cmd filefrag       || die "找不到 filefrag"
[ -x "$BUSYBOX" ]      || die "找不到 busybox: $BUSYBOX"

# ── 建立碎片化測試檔（與 bench_my_fs_warmed.sh 同邏輯，簡化版）──
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

# ── 蒐集 syscall 次數 ─────────────────────────────────────────────
# strace -c 的 summary 寫在 stderr；-o 指定輸出檔可避免被工具本身的 stderr 干擾
MY_TRACE="$TMPDIR_WORK/my_fs.strace"
REF_TRACE="$TMPDIR_WORK/filefrag.strace"

strace -c -o "$MY_TRACE"  "$BUSYBOX" my_fs -f "$TESTFILE" >/dev/null 2>&1
strace -c -o "$REF_TRACE" filefrag -v "$TESTFILE"          >/dev/null 2>&1

# 從 strace -c 表抽出單一 syscall 的呼叫次數（calls 欄）。
# strace -c 格式：% time | seconds | usecs/call | calls | errors | syscall
# 取倒數第三或第二欄需小心 errors 欄可能空白，這裡用 syscall 名稱作 anchor。
count_syscall() {
    local tracefile=$1 syscall=$2
    awk -v sc="$syscall" '
        $NF == sc {
            # calls 欄：errors 欄若存在於 NF-1，否則於 NF-2
            # 為簡化，假設輸出穩定：calls 在固定位置時較複雜，
            # 改用「倒數第二個欄位若為純數字則為 calls，否則為 NF-2」
            for (i = NF-1; i >= 1; i--) {
                if ($i ~ /^[0-9]+$/) { print $i; exit }
            }
        }
    ' "$tracefile"
}

# 想關注的 syscalls（不同 libc / static 連結差異也順便看）
WATCH="statfs fstatfs fstat newfstatat stat ioctl openat open close read write mmap brk"

# ── 輸出比較表 ────────────────────────────────────────────────────
printf "%-14s | %-12s | %-12s\n" "syscall" "my_fs -f" "filefrag -v"
printf "%s\n" "$(printf '%.0s-' {1..45})"
for sc in $WATCH; do
    a=$(count_syscall "$MY_TRACE"  "$sc")
    b=$(count_syscall "$REF_TRACE" "$sc")
    a=${a:-0}; b=${b:-0}
    # 全部 0 的 syscall 略過（精簡輸出）
    [ "$a" = "0" ] && [ "$b" = "0" ] && continue
    printf "%-14s | %12s | %12s\n" "$sc" "$a" "$b"
done

echo ""
echo "Key check (Option A: print_file_frag 不再多呼叫 statfs)："
my_statfs=$(count_syscall "$MY_TRACE" statfs)
my_statfs=${my_statfs:-0}
if [ "$my_statfs" = "0" ]; then
    echo "  ✓ my_fs -f 已無 statfs() 呼叫（Option A 生效）"
else
    echo "  ✗ my_fs -f 仍呼叫 statfs() $my_statfs 次（Option A 未生效，請確認已 compile.sh）"
fi

echo ""
echo "Full strace -c 輸出："
echo "── my_fs -f ──"
cat "$MY_TRACE"
echo ""
echo "── filefrag -v ──"
cat "$REF_TRACE"
