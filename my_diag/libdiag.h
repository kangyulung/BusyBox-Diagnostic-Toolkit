#ifndef LIBDIAG_H
#define LIBDIAG_H

#include "libbb.h"
#include <linux/fiemap.h>

/* ANSI 控制序列：用於 UI 渲染 */
#define DIAG_CLR_EOL "\033[K"   /* 清除至行尾 */
#define DIAG_CLR_SCR "\033[H"   /* 游標移至左上角 */
#define DIAG_RESET   "\e[0m"    /* 重置顏色與格式 */
#define DIAG_RED     "\e[1;31m"
#define DIAG_GREEN   "\e[1;32m"
#define DIAG_YELLOW  "\e[1;33m"
#define DIAG_CYAN    "\e[1;36m"
#define DIAG_HIDE    "\033[?25l" /* 隱藏游標 */
#define DIAG_SHOW    "\033[?25h" /* 顯示游標 */

/* 樹狀結構基礎節點：用於建立行程間的父子關係 */
typedef struct diag_node_base {
    int id;                         /* 節點唯一識別碼 (如 PID) */
    int parent_id;                  /* 父節點識別碼 (如 PPID) */
    struct diag_node_base *child;   /* 第一個子節點 */
    struct diag_node_base *sibling; /* 下一個兄弟節點 */
    struct diag_node_base *next;    /* 線性鏈表指標，用於遍歷所有節點 */
} diag_node_base_t;


/* 檔案系統資訊結構 (統計磁碟使用狀況) */
typedef struct {
    unsigned long total_inodes;
    unsigned long free_inodes;
    uint64_t total_bytes;           /* 總容量 */
    uint64_t free_bytes;            /* 非 root 用戶可用空間 */
    uint64_t free_bytes_priv;       /* 含 root 保留區的剩餘空間 */
} diag_fs_t;

/* 網路連線狀態 (解析自 /proc/net/tcp) */
typedef struct {
    char local_addr[48];            /* 本地端位址與埠號 */
    char remote_addr[48];           /* 遠端位址與埠號 */
    int state;                      /* TCP 狀態碼 */
} diag_net_t;

/* 單檔碎片分析結果 */
typedef struct {
    uint64_t              file_size;    /* 檔案大小（bytes） */
    uint32_t              extent_count; /* extent 總數 */
    uint32_t              block_size;   /* fstat.st_blksize；caller 用於 byte↔block 換算，免再呼叫 statfs */
    struct fiemap_extent *extents;      /* 詳細清單；NULL 表示未收集（需呼叫 diag_free_frag 釋放） */
} diag_frag_t;

/* 通用解析工具 */
/* 系統狀態快照 (全域統計) */
typedef struct {
    unsigned long total_mem_kb;
    unsigned long free_mem_kb;
    double load_avg[3];             /* 1, 5, 15 分鐘平均負載 */
    unsigned long long cpu_total_ticks; /* CPU 累計總滴答數 */
} diag_sys_snap_t;

/* 通用解析與格式化工具 */
char* diag_find_key(const char *buf, const char *key);
long diag_get_val(const char *buf, const char *key);
unsigned long long get_cpu_usage_ticks(void);
char* diag_format_time(char *buf, unsigned long long utime, unsigned long long stime);

/* 系統資訊採集函數 */
int diag_read_fs(const char *path, diag_fs_t *f);
const char* diag_get_tcp_state(int state);
void diag_get_sys_snap(diag_sys_snap_t *snap);

/* 終端 UI 模式控制 */
void diag_ui_mode_raw(struct termios *old);    /* 開啟 Raw mode 以處理單鍵輸入 */
void diag_ui_mode_normal(struct termios *old); /* 恢復標準終端模式 */
int  diag_ui_ask_int(const char *prompt, struct termios *old); /* 彈出式詢問數值 */

/* 樹狀結構建構工具 */
diag_node_base_t** diag_nodes_to_array(diag_node_base_t *list, int *out_cnt);
diag_node_base_t* diag_link_tree(diag_node_base_t **nodes, int count);

/*
 * 對單一正規檔案執行 FIEMAP ioctl，填入 f->file_size 與 f->extent_count。
 * collect_extents=1 → 同時填充 f->extents（動態配置，需呼叫 diag_free_frag 釋放）
 * collect_extents=0 → f->extents 保持 NULL，僅取 extent_count
 * 回傳 0 成功、-1 失敗（errno 已設定）；path 為目錄或不支援 FIEMAP 的 fs 均回傳 -1
 */
int diag_read_fragmentation(const char *path, diag_frag_t *f, int collect_extents);
void diag_free_frag(diag_frag_t *f);

#endif