#!/bin/bash

# 檢查是否具備 root 權限（因為 -p 參數需要讀取其他進程的 /proc/PID/fd）
if [ "$EUID" -ne 0 ]; then
  echo "警告: 部分測試（如 -p 顯示 PID）需要 root 權限，建議使用 sudo 執行。"
fi

# 檢查系統是否有 nc (netcat)
if ! command -v nc &> /dev/null; then
    echo "錯誤: 本腳本需要 'nc' (netcat) 來建立連線，請先安裝（例如: apt install netcat-openbsd）"
    exit 1
fi

echo "=== 開始建立虛擬網路連線環境 ==="

# 1. 建立 TCP LISTEN 狀態 (IPv4 與 IPv6)
echo "[+] 建立 TCP LISTEN 連線..."
nc -l -k -p 8080 >/dev/null 2>&1 &
PID_LISTEN4=$!
nc -6 -l -k -p 8081 >/dev/null 2>&1 &
PID_LISTEN6=$!

# 2. 建立 TCP ESTABLISHED 狀態 (IPv4 與 IPv6)
echo "[+] 建立 TCP ESTABLISHED 連線..."
nc -l -p 8082 >/dev/null 2>&1 &
PID_EST_SERVER4=$!
sleep 0.2
nc 127.0.0.1 8082 >/dev/null 2>&1 &
PID_EST_CLIENT4=$!

nc -6 -l -p 8083 >/dev/null 2>&1 &
PID_EST_SERVER6=$!
sleep 0.2
nc -6 ::1 8083 >/dev/null 2>&1 &
PID_EST_CLIENT6=$!

# 3. 建立 UDP 狀態 (IPv4 與 IPv6)
echo "[+] 建立 UDP 監聽..."
nc -u -l -k -p 9090 >/dev/null 2>&1 &
PID_UDP4=$!
nc -6 -u -l -k -p 9091 >/dev/null 2>&1 &
PID_UDP6=$!

# 4. 模擬特殊的 TCP 狀態: CLOSE_WAIT
# 原理：Client 連上 Server 後，Server 主動關閉連線(發送FIN)，但 Client 不呼叫 close()，Client 就會卡在 CLOSE_WAIT
echo "[+] 模擬 TCP CLOSE_WAIT 狀態..."
nc -l -p 8084 >/dev/null 2>&1 &
PID_CW_SERVER=$!
sleep 0.2
# 讓 python 建立連線後，只關閉讀取端或等待 Server 斷開，這裡用一個簡單的背景對接
# 或者最簡單的方式：用 python 快速製造一個卡在 CLOSE_WAIT 的連線
python3 -c '
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 8084))
print("Client connected, waiting for server to drop...")
time.sleep(1) # 等 server 砍掉
' &
PID_CW_CLIENT=$!
sleep 0.2
kill $PID_CW_SERVER # Server 死掉發送 FIN，Python 進程如果還在 sleep 沒 close 該 socket，就會維持 CLOSE_WAIT

echo "------------------------------------------------"
echo "虛擬連線建立完畢！請在「同一個終端機」或「另開視窗」執行你的 my_net 進行測試。"
echo "推薦測試指令："
echo "  1. 顯示全部連線與 PID:  sudo ./my_net -a -p"
echo "  2. 只看監聽狀態:        ./my_net -l"
echo "  3. 觀察模式（按 Q 退出）: ./my_net -w 1 -a -p"
echo "------------------------------------------------"
echo "按 [ENTER] 鍵將自動清除所有虛擬連線並離開..."
read

# 清除所有背景進程
echo "[-] 正在清理背景連線進程..."
kill $PID_LISTEN4 $PID_LISTEN6 $PID_EST_SERVER4 $PID_EST_CLIENT4 $PID_EST_SERVER6 $PID_EST_CLIENT6 $PID_UDP4 $PID_UDP6 $PID_CW_CLIENT 2>/dev/null
echo "=== 環境清理完畢 ==="