#ifndef LIBDIAG_H
#define LIBDIAG_H

#include "libbb.h"

/* 行程資訊結構 */
typedef struct {
    int pid;
    int ppid;
    char comm[32];
    unsigned long vmsize;
    unsigned long rss;
} diag_proc_t;

/* 檔案系統資訊結構 */
typedef struct {
    unsigned long total_inodes;
    unsigned long free_inodes;
    uint64_t total_bytes;
    uint64_t free_bytes;
} diag_fs_t;

/* 網路連線結構 */
typedef struct {
    char local_addr[48];
    char remote_addr[48];
    int state;
} diag_net_t;

/* 通用解析工具 */
char* diag_find_key(const char *buf, const char *key);
long diag_get_val(const char *buf, const char *key);

/* 各模組專用收集函數 */
int diag_read_proc(int pid, diag_proc_t *p);
int diag_read_fs(const char *path, diag_fs_t *f);
const char* diag_get_tcp_state(int state);

#endif