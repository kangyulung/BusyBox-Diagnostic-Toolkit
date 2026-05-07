#!/bin/bash

# 顯示說明
show_help() {
    echo "使用方法: $0 [選項]"
    echo "選項:"
    echo "  -m, --memory <MB>   指定要佔用的記憶體大小 (單位: MB)"
    echo "  -c, --cpu <核數>    指定要 100% 佔用的 CPU 核心數量"
    echo "  -t, --time <秒>     測試持續時間 (秒)，預設為無限"
    echo "  -h, --help          顯示此說明"
    echo ""
    echo "範例: $0 -m 512 -c 2 -t 60  (佔用 512MB 記憶體與 2 顆 CPU，持續 60 秒)"
}

# 預設變數
MEM_TO_ALLOC=0
CPU_CORES=0
DURATION=0

# 解析參數
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
            echo "未知參數: $1"
            show_help
            exit 1
            ;;
    esac
done

# 檢查是否至少指定了一項任務
if [ "$MEM_TO_ALLOC" -eq 0 ] && [ "$CPU_CORES" -eq 0 ]; then
    echo "錯誤: 請指定要佔用的記憶體 (-m) 或 CPU 核心數 (-c)！"
    show_help
    exit 1
fi

# 清理資源的函數
cleanup() {
    echo -e "\n[+] 正在釋放資源並清理環境..."
    # 砍掉所有由本腳本產生的背景 CPU 負載行程
    if [ -n "$CPU_PIDS" ]; then
        kill $CPU_PIDS 2>/dev/null
    fi
    # 釋放記憶體變數
    unset MEM_HOLDER
    echo "[+] 清理完成。離開。"
    exit 0
}

# 監聽 Ctrl+C 或結束訊號
trap cleanup SIGINT SIGTERM EXIT

# ==================== 1. 記憶體佔用 (RAM) ====================
# 這次我們直接利用 Bash 陣列把變數塞滿 RAM，這樣記憶體就會算在該 Bash 行程的 RSS 帳上！
if [ "$MEM_TO_ALLOC" -gt 0 ]; then
    echo "[+] 正在分配記憶體: ${MEM_TO_ALLOC} MB..."
    
    # 產生一個約 1MB 大小的隨機字串作為基底 (1,048,576 bytes)
    # 為了加速分配，我們用重複字元建立
    BLOCK=$(printf '%1048576s' 'X')
    
    # 宣告一個陣列來持有記憶體
    declare -a MEM_HOLDER
    
    for ((i=1; i<=MEM_TO_ALLOC; i++)); do
        MEM_HOLDER[$i]="$BLOCK"
        # 每分配 100MB 顯示一次進度，避免大記憶體時畫面卡住
        if (( i % 100 == 0 )); then
            echo "[+] 已成功分配 $i MB..."
        fi
    done
    echo "[+] 記憶體分配完畢！當前 Bash 行程已佔用約 ${MEM_TO_ALLOC} MB RSS。"
fi

# ==================== 2. CPU 佔用 ====================
CPU_PIDS=""
if [ "$CPU_CORES" -gt 0 ]; then
    echo "[+] 正在啟動 $CPU_CORES 個 CPU 100% 佔用行程..."
    for i in $(seq 1 "$CPU_CORES"); do
        # 透過無限迴圈跑 sha1sum 佔用單核，放到背景
        sha1sum /dev/zero & 
        CPU_PIDS="$CPU_PIDS $!"
    done
    echo "[+] CPU 行程已在背景啟動。"
fi

# ==================== 3. 時間控制 ====================
if [ "$DURATION" -gt 0 ]; then
    echo "[+] 資源將佔用 $DURATION 秒..."
    sleep "$DURATION"
else
    echo "[+] 我的 PID 是: $$"
    echo "[+] 資源已佔用。請在另一個終端機觀察 my_proc。按下 [Ctrl+C] 終止。"
    while true; do
        sleep 1
    done
fi