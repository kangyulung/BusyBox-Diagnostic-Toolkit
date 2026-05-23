# BusyBox Diagnostic Toolkit

**繁體中文** | [English](README.md)

基於 BusyBox 架構的輕量化系統診斷工具集，以 C 語言實作三個新 applet，遵循 POSIX/UNIX 命令列介面慣例，編譯為單一靜態執行檔，並共用內部函式庫 `libdiag`。

---

## 工具一覽

| Applet | 原始碼 | 功能簡述 |
|--------|--------|----------|
| `my_fs` | `my_diag/diag_fs.c` | 檔案系統健康檢測：磁碟用量、inode 使用率、碎片化分析 |
| `my_proc` | `my_diag/diag_proc.c` | 行程資源分析：即時監控、樹狀檢視、互動式排序 |
| `my_net` | `my_diag/diag_net.c` | 網路連線狀態監控：TCP/UDP socket 列表、TCP 狀態追蹤 |

---

## 快速開始

### 環境需求

- **Docker**
  - macOS: 建議使用 [colima](https://github.com/abiosoft/colima)，也可用 Docker Desktop 等其他 Docker Engine
  - Linux: 直接安裝 Docker Engine 即可
  - Windows: 建議使用 WSL2 搭配 Docker Desktop

- **Bash**（執行 `dev.sh` / `compile.sh` / `format.sh`）

### 首次建立開發環境

```bash
bash ./dev.sh
```

此指令會自動：

1. 建立 Docker image（`busybox-dev-env`）
2. 啟動容器並掛載專案目錄
3. 在容器內執行 `make defconfig` 並強制靜態連結
4. 完整編譯一次後丟出互動式 bash（`root@...:/home/project/busybox#`）

> **注意：** 首次執行約需 5–10 分鐘；後續會快取。

### 每次修改後重新編譯

進入容器後執行：

```bash
# 單純重新編譯
bash ./compile.sh

# 編譯前先以 clang-format 格式化 my_diag/ 內所有 .c / .h
bash ./compile.sh --fmt
```

`compile.sh` 執行流程：

1. 刪除自動生成的 `my_diag/Config.in` 與 `my_diag/Kbuild`（避免 `INSERT` 殘留）
2. 清除 `my_diag/` 內所有中間產物（`.o`、`lib.a` 等），保留原始碼與文件
3. （若傳入 `--fmt`）呼叫 `format.sh`，以 `clang-format -i` 原地格式化 `my_diag/*.c` 與 `my_diag/*.h`
4. `make defconfig`，並自動啟用靜態連結；在非 x86 環境（如 ARM64 colima）自動停用 SHA 硬體加速
5. `make -j$(nproc)` 完整重建

> `format.sh` 也可單獨執行（`bash ./format.sh`），僅做格式化而不觸發編譯。

### 執行工具

```bash
./busybox my_fs  [OPTION]... [PATH]...
./busybox my_proc [OPTION]...
./busybox my_net  [OPTION]...
```

---

## my_fs — 檔案系統健康檢測工具

### 功能

- 磁碟用量顯示，輸出格式相容 GNU `df(1)`
- inode 使用率分析（`-i`），相容 `df -i`
- 雙視角 Use%（使用者視角 / 實際含保留區視角）與 root 保留區資訊（`-r`）
- 單檔 FIEMAP extent 分析（`-f FILE`），模仿 `filefrag -v` 輸出
- 掛載點碎片化統計（`-F PATH`），以 `nftw` 全掃並列出 Top 10 最碎片化檔案
- 互動式 TUI 即時監控（`-s`）

### 選項

```
my_fs [-h] [-i] [-r] [-t TYPE] [-x TYPE] [-f FILE] [-F PATH] [-s] [PATH]...

  -h          人類可讀大小（K/M/G/T）
  -i          顯示 inode 用量而非 block 用量
  -r          雙視角 Use%（使用者 / 實際）與 root 保留區欄位
  -t TYPE     僅顯示指定檔案系統類型（例如 ext4、tmpfs）
  -x TYPE     排除指定檔案系統類型
  -f FILE     對單一檔案執行 FIEMAP extent 分析
  -F PATH     掃描掛載點下所有檔案的碎片化統計
  -s          互動式 TUI（D/I/R/F/H/Q 切換視圖）
```

**TUI 鍵位：** `D` 磁碟容量・`I` inode・`R` 保留區・`F` 碎片化・`H` 切換人類可讀・`Q` 離開

### 使用範例

```bash
# 列出所有掛載點（人類可讀）
./busybox my_fs -h

# 檢查根目錄 inode 使用率
./busybox my_fs -i /

# 顯示 ext4 的雙視角 Use%（最適合觀察保留區）
./busybox my_fs -rh /

# 分析單一檔案的 extent 配置
./busybox my_fs -f /etc/passwd

# 掃描掛載點碎片化
./busybox my_fs -F /

# 以人類可讀模式啟動 TUI
./busybox my_fs -sh
```

---

## my_proc — 行程資源分析器

### 功能

- 即時行程列表，自動每秒更新 CPU 與記憶體統計
- 樹狀檢視（依 PPID 關係建構父子階層）
- 多維度排序（CPU / RSS / VSZ / PID / PPID / User / SID / Command）
- 互動式操作：執行期間切換視圖、變更排序方式、送出訊號終止行程
- Batch 模式（關閉終端控制序列，適合日誌記錄或腳本使用）

### 選項

```
my_proc [-t] [-d <seconds>] [-n <count>] [-p <pid>] [-b]

  -t       以樹狀檢視啟動
  -d SEC   更新間隔（秒，預設 1.0）
  -n NUM   更新 NUM 次後自動結束
  -p PID   僅監控指定 PID 及其相關行程
  -b       Batch 模式（關閉 TUI，適合重導向至檔案）
```

### 互動鍵位

| 按鍵 | 功能 |
|------|------|
| `T` | 切換列表檢視（TOP）與樹狀檢視（TREE） |
| `Q` | 離開程式 |
| `K` | 終止行程（輸入 PID，送出 SIGTERM） |
| `P` | 依 CPU 使用率排序（預設） |
| `M` | 依 RSS 排序 |
| `V` | 依 VSZ 排序 |
| `I` | 依 PID 排序 |
| `O` | 依 PPID 排序 |
| `U` | 依使用者名稱排序 |
| `S` | 依 Session ID 排序 |
| `C` | 依命令名稱排序 |

### 輸出欄位

| 欄位 | 說明 |
|------|------|
| `PID` | 行程 ID |
| `PPID` | 父行程 ID |
| `SID` | Session ID (階段 ID) |
| `USER` | 行程擁有者 |
| `STAT` | 行程狀態（R 執行中、S 休眠、D 不可中斷休眠、Z 殭屍、T 已停止） |
| `NI` | Nice 值（排程優先度） |
| `VSZ` | 虛擬記憶體總量 |
| `RSS` | 常駐集合大小（實際佔用實體記憶體） |
| `%CPU` | 上次更新以來的 CPU 使用率 |
| `%MEM` | 實體記憶體使用率 |
| `TIME` | 行程累計 CPU 時間 |
| `COMMAND` | 命令名稱 |

### 使用範例

```bash
# 啟動互動式即時監控（預設列表視圖，依 CPU 排序）
./busybox my_proc

# 以樹狀視圖啟動
./busybox my_proc -t

# 每 3 秒更新，共輸出 5 次後結束（適合腳本）
./busybox my_proc -b -d 3 -n 5

# 僅監控 PID 1234
./busybox my_proc -p 1234

# 匯出行程快照至檔案
./busybox my_proc -b -n 1 > proc_snapshot.txt
```

---

## my_net — 網路連線狀態監控器

### 功能

- 讀取 `/proc/net/tcp[6]`、`/proc/net/udp[6]` 列出 socket 資訊
- TCP 狀態機追蹤：將核心的十六進位狀態碼解碼為人類可讀名稱（ESTABLISHED、TIME_WAIT、LISTEN 等）
- 連線異常偵測：每次掃描後比對各狀態計數與閾值，自動警示 SYN flood、連線洩漏、TIME_WAIT 堆積等問題
- PID/程式名稱解析：以 `getdents64(2)` 直接掃描 `/proc/<pid>/fd/` 建立 inode→PID 映射，二分搜尋達 O(log n) 查找
- Watch 模式：週期性更新畫面，適合終端機即時監控

### 選項

```
my_net [-t] [-u] [-a] [-l] [-n] [-s STATE] [-w SEC] [-b] [-p]

  -t          TCP sockets（預設）
  -u          UDP sockets
  -a          全部（TCP + UDP）
  -l          僅顯示 LISTEN socket
  -n          數字模式（不解析使用者名稱，直接顯示 UID）
  -s STATE    依 TCP 狀態名稱過濾（不分大小寫）
  -w SEC      Watch 模式，每 SEC 秒更新（最小 1 秒，Q 離開）
  -b          Batch 模式（關閉 ANSI 色彩，適合管線與重導向）
  -p          顯示 PID/程式名稱（完整資訊需 root）
```

### 輸出欄位

| 欄位 | 說明 |
|------|------|
| `Proto` | 協定：`tcp`、`tcp6`、`udp`、`udp6` |
| `State` | TCP 狀態名稱；UDP 固定顯示 `-`（無狀態） |
| `Local Address` | 本地 IP:Port（IPv6 以 `[addr]:port` 表示） |
| `Foreign Address` | 遠端 IP:Port；LISTEN socket 顯示 `0.0.0.0:0` |
| `PID/Program` | 擁有行程的 `pid/name`（僅 `-p` 時顯示，無法解析顯示 `-`） |
| `User` | socket 擁有者的使用者名稱（32 項 UID 快取避免重複查找） |

### TCP 狀態對照表

| 狀態名稱 | 代碼 | 說明 |
|----------|------|------|
| `ESTABLISHED` | 0x01 | 資料傳輸進行中 |
| `SYN_SENT` | 0x02 | 主動開啟：SYN 已送出，等待 SYN-ACK |
| `SYN_RECV` | 0x03 | 被動開啟：SYN 已收到，SYN-ACK 已送出 |
| `FIN_WAIT1` | 0x04 | 主動關閉：FIN 已送出，等待 FIN 或 ACK |
| `FIN_WAIT2` | 0x05 | FIN 已確認；等待遠端 FIN |
| `TIME_WAIT` | 0x06 | 等待 2×MSL 後完全關閉 |
| `CLOSE` | 0x07 | 連線已完全關閉 |
| `CLOSE_WAIT` | 0x08 | 被動關閉：遠端 FIN 已收到，本地關閉待處理 |
| `LAST_ACK` | 0x09 | 被動關閉 FIN 已送出，等待最後 ACK |
| `LISTEN` | 0x0A | 伺服器 socket 等待連線 |
| `CLOSING` | 0x0B | 雙方同時關閉 |

### 異常偵測閾值

每次掃描後若任一計數超過閾值，會在 TCP 狀態統計後附加 `[ANOMALY DETECTED]` 區塊：

| 狀態 | 閾值 | 可能原因 |
|------|------|----------|
| `TIME_WAIT` | > 500 | 連線翻轉過快，考慮開啟 `net.ipv4.tcp_tw_reuse` |
| `CLOSE_WAIT` | > 20 | 應用程式未呼叫 `close(2)`，可能有連線洩漏 |
| `SYN_RECV` | > 100 | 可能遭受 SYN flood 攻擊 |
| `LAST_ACK` | > 50 | 對端未回應 FIN（網路故障或遠端崩潰） |
| `FIN_WAIT2` | > 100 | 半開連線積壓，檢查 `net.ipv4.tcp_fin_timeout` |
| `ESTABLISHED` | > 10000 | 連線數異常偏高，可能有資源洩漏 |

### 使用範例

```bash
# 列出所有 TCP 連線（預設）
./busybox my_net

# 顯示所有 TCP 連線與 PID（完整資訊需 root）
./busybox my_net -p

# 僅顯示 LISTEN socket 及其擁有行程
./busybox my_net -l -p

# 列出所有 socket（TCP + UDP），Batch 模式輸出
./busybox my_net -a -b

# 過濾僅顯示 ESTABLISHED 連線
./busybox my_net -s ESTABLISHED

# 顯示 UDP socket 與 PID
./busybox my_net -u -p

# Watch 模式：每 3 秒更新所有 socket
./busybox my_net -a -w 3

# 計算 ESTABLISHED 連線數並與 ss(8) 比對
./busybox my_net -s ESTABLISHED -b | grep -cE '^tcp'
ss -tn | grep -c ESTAB

# 每 10 秒記錄一次連線狀態至檔案
./busybox my_net -a -b -w 10 >> /var/log/net_snapshot.log
```

---

## 目錄結構

```
.
├── my_diag/                # 本專題程式碼（唯一允許修改的目錄）
│   ├── libdiag.h / .c      # 共用內部函式庫（diag_fs_t、diag_frag_t 等）
│   ├── diag_fs.c           # my_fs applet
│   ├── diag_proc.c         # my_proc applet
│   ├── diag_net.c          # my_net applet
│   ├── Config.src          # BusyBox 設定模板（由 compile.sh 生成 Config.in）
│   ├── Kbuild.src          # BusyBox 建置模板（由 compile.sh 生成 Kbuild）
│   ├── my_fs.1             # my_fs man page
│   ├── my_proc.1           # my_proc man page
│   ├── my_net.1            # my_net man page
│   ├── tests_fs/           # my_fs 測試腳本
│   ├── tests_proc/         # my_proc 測試腳本
│   └── tests_net/          # my_net 測試腳本
├── compile.sh              # 容器內重新編譯腳本（支援 --fmt 旗標）
├── format.sh               # 以 clang-format 格式化 my_diag/*.c / *.h
├── dev.sh                  # 首次建立開發環境腳本
├── Dockerfile              # 開發容器定義
├── local-docs/             # 課程文件與開發知識庫（不納入版本控制）
├── CONTRIBUTING.md         # 貢獻指南（→ my_diag/CONTRIBUTING.md）
└── README.upstream         # BusyBox 原版說明文件
```

---

## 常見問題

| 狀況 | 解決方式 |
|------|----------|
| 首次執行很慢（5–10 分鐘） | 正常，Docker image 需要安裝依賴並完整編譯 |
| 想再進入同一個容器開第二個 shell | `docker exec -it <container_id> bash` |
| 修改 `Dockerfile` 但容器沒更新 | `docker build --no-cache -t busybox-dev-env .` |

---

## 貢獻方式

請先閱讀 [`my_diag/CONTRIBUTING.md`](my_diag/CONTRIBUTING.md)。

- **禁止直接 push `master`**，須走 `feature/xxx` 或 `fix/xxx` 分支
- 禁止修改 `my_diag/` 以外的檔案
- 編碼風格：Tab 縮排、snake_case、`diag_` 前綴（內部函式）
- 新 applet 必須附 man page 與 Bash 測試腳本

---

## 授權

本工具集以 GNU GPL v2 授權發布，與 BusyBox 上游保持一致。詳見 [`LICENSE`](LICENSE)。
