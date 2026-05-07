#!/bin/bash

# ==========================================================
# 1. 環境設定
# ==========================================================
BB="./busybox"
APPLET="my_proc"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 檢查檔案
if [ ! -f "$BB" ]; then
    echo -e "${RED}錯誤: 找不到 $BB 執行檔${NC}"
    exit 1
fi

echo -e "${YELLOW}====================================================${NC}"
echo -e "${YELLOW}   my_proc 整合測試與效能分析 (終極修正版)   ${NC}"
echo -e "${YELLOW}====================================================${NC}"

# ==========================================================
# 2. 功能驗證 (使用過濾機制處理 ANSI Code)
# ==========================================================
echo -e "\n[1/3] 開始功能驗證..."

# 測試 1: 樹狀模式
echo -n " - Tree View (-t) 檢查: "
$BB $APPLET -t 2>&1 | head -n 20 > test_tree.log
if grep -q "|-" test_tree.log; then
    echo -e "${GREEN}通過${NC}"
else
    echo -e "${RED}失敗 (未見樹狀符號)${NC}"
fi

# 測試 2: Top 模式 (重點修正)
echo -n " - Top 模式內容檢查: "

# 執行 2 秒擷取輸出
$BB $APPLET -s 2>&1 | head -n 20 > test_top_raw.log

# 使用 sed 完美剝除標準 ANSI 轉義字元（包含顏色、游標定位等）
# 剝除後，再用 tr 去除可能殘留的其他不可見非標準字元
sed -r "s/\x1B\[[0-9;]*[a-zA-Z]//g" test_top_raw.log | tr -cd '[:print:]\n' > test_top_clean.log

# 進行關鍵字匹配驗證
if grep -qi "Uptime" test_top_clean.log && (grep -qi "VSZ" test_top_clean.log || grep -qi "RSS" test_top_clean.log); then
    echo -e "${GREEN}通過 (已完美過濾 ANSI 碼)${NC}"
else
    # 萬一還是匹配不到，輸出前 5 行給使用者 debug，避免讓人一頭霧水
    echo -e "${RED}失敗${NC}"
    echo -e "   ${YELLOW}[Debug 提示] 乾淨的輸出前三行為：${NC}"
    head -n 3 test_top_clean.log | sed 's/^/    /'
fi

# ==========================================================
# 3. 效能基準測試
# ==========================================================
echo -e "\n[2/3] 效能基準測試..."

# 速度測試
#TIME_MY_PROC=$( (time -p for i in {1..10}; do $BB $APPLET -t > /dev/null 2>&1; done) 2>&1 | grep real | awk '{print $2}' )
#TIME_PSTREE=$( (time -p for i in {1..10}; do pstree > /dev/null 2>&1; done) 2>&1 | grep real | awk '{print $2}' )

# 使用 $EPOCHREALTIME 計算精確到微秒（小數點後四位）的時間
# 測試 my_proc
START_MY=$EPOCHREALTIME
for i in {1..10}; do $BB $APPLET -t > /dev/null 2>&1; done
END_MY=$EPOCHREALTIME

# 測試 pstree
START_PS=$EPOCHREALTIME
for i in {1..10}; do pstree > /dev/null 2>&1; done 2>/dev/null
END_PS=$EPOCHREALTIME

# 計算差值（透過 awk 進行浮點數運算，保留四位小數）
TIME_MY_PROC=$(awk -v start="$START_MY" -v end="$END_MY" 'BEGIN { printf "%.4fs", end - start }')
TIME_PSTREE=$(awk -v start="$START_PS" -v end="$END_PS" 'BEGIN { printf "%.4fs", end - start }')

# 記憶體測試 (確保抓取到 Standard Tool)
$BB $APPLET -s > /dev/null 2>&1 &
PID_MY=$!
sleep 1.2
RSS_MY=$(ps -o rss= -p $PID_MY | tr -d ' ' || echo "0")
kill $PID_MY 2>/dev/null

top -b > /dev/null 2>&1 &
PID_TOP=$!
sleep 1.2
RSS_TOP=$(ps -o rss= -p $PID_TOP | tr -d ' ' || echo "0")
kill $PID_TOP 2>/dev/null

# ==========================================================
# 4. 數據報告
# ==========================================================
echo -e "\n[3/3] 效能對比報告"
echo "----------------------------------------------------"
printf "%-20s | %-15s | %-15s\n" "指標" "my_proc (BB)" "Standard Tool"
echo "----------------------------------------------------"
printf "%-20s | %-15s | %-15s\n" "樹狀生成速度(10次)" "${TIME_MY_PROC}s" "${TIME_PSTREE:-0.00}s"
printf "%-20s | %-15s | %-15s\n" "記憶體佔用 (RSS)" "${RSS_MY} KB" "${RSS_TOP} KB"
echo "----------------------------------------------------"

# 效益分析
if [ "$RSS_MY" -lt "$RSS_TOP" ] && [ "$RSS_MY" -ne 0 ]; then
    DIFF=$((RSS_TOP - RSS_MY))
    echo -e "${GREEN}結果分析：您的 Applet 比系統工具節省了 ${DIFF} KB 的實體記憶體。${NC}"
fi

# 清理
rm -f test_*.log test_*_raw.log test_*_clean.log
echo -e "\n測試完成。"