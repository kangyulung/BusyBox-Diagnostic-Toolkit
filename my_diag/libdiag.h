#ifndef LIBDIAG_H
#define LIBDIAG_H

#include "libbb.h"
#include <linux/fiemap.h>

/* 行程資訊結構 */
typedef struct {
    int pid;
    int ppid;
    unsigned int uid;
    char state;
    char comm[64];
    unsigned long vmsize;
    unsigned long rss;
    unsigned long utime;
    unsigned long stime;
    int threads;            /* 第 20 欄位：執行緒數量 */
    int priority;           /* 第 18 欄位：核心優先權 */
    int nice;               /* 第 19 欄位：Nice 值 (-20 ~ 19) */
    unsigned long long start_time; /* 第 22 欄位：啟動時間 (jiffies) */
} diag_proc_t;

/* 檔案系統資訊結構 */
typedef struct {
    unsigned long total_inodes;  /* statfs.f_files */
    unsigned long free_inodes;   /* statfs.f_ffree */
    uint64_t total_bytes;        /* f_blocks * f_frsize */
    uint64_t free_bytes;         /* f_bavail * f_frsize（非 root 可用） */
    uint64_t free_bytes_priv;    /* f_bfree  * f_frsize（含 root 保留區） */
} diag_fs_t;

/* 網路連線結構 */
typedef struct {
    char local_addr[48];
    char remote_addr[48];
    int state;
} diag_net_t;

/* 單檔碎片分析結果 */
typedef struct {
    uint64_t              file_size;    /* 檔案大小（bytes） */
    uint32_t              extent_count; /* extent 總數 */
    struct fiemap_extent *extents;      /* 詳細清單；NULL 表示未收集（需呼叫 diag_free_frag 釋放） */
} diag_frag_t;

/* 通用解析工具 */
char* diag_find_key(const char *buf, const char *key);
long diag_get_val(const char *buf, const char *key);

/* 各模組專用收集函數 */
int diag_read_proc(int pid, diag_proc_t *p);
int diag_read_fs(const char *path, diag_fs_t *f);
const char* diag_get_tcp_state(int state);

/*
 * 對單一正規檔案執行 FIEMAP ioctl，填入 f->file_size 與 f->extent_count。
 * collect_extents=1 → 同時填充 f->extents（動態配置，需呼叫 diag_free_frag 釋放）
 * collect_extents=0 → f->extents 保持 NULL，僅取 extent_count
 * 回傳 0 成功、-1 失敗（errno 已設定）；path 為目錄或不支援 FIEMAP 的 fs 均回傳 -1
 */
int diag_read_fragmentation(const char *path, diag_frag_t *f, int collect_extents);
void diag_free_frag(diag_frag_t *f);

#endif