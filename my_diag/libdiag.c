#include "libdiag.h"
#include <sys/vfs.h>

/* --- 內部通用解析工具 --- */

// 在 buffer 中尋找特定 Key 並回傳指向 Value 的指標
char* diag_find_key(const char *buf, const char *key) {
    char *ptr = strstr(buf, key);
    if (ptr) {
        ptr += strlen(key);
        while (*ptr == ' ' || *ptr == ':' || *ptr == '\t') ptr++;
        return ptr;
    }
    return NULL;
}

// 提取字串中的長整數數值
long diag_get_val(const char *buf, const char *key) {
    char *ptr = diag_find_key(buf, key);
    return ptr ? atol(ptr) : -1;
}

/* --- 行程資源分析模組 (服務 diag_proc) --- */

int diag_read_proc(int pid, diag_proc_t *p) {
    char path[64];
    char *buf;
    
    memset(p, 0, sizeof(diag_proc_t));
    p->pid = pid;

    // 1. 讀取 /proc/[pid]/stat 獲取 PPID 與名稱
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    buf = xmalloc_open_read_close(path, NULL);
    if (buf) {
        char *s = strrchr(buf, ')'); // 找最後一個括號以精確定位
        if (s) {
            //sscanf(s + 2, "%*c %d", &p->ppid);
            sscanf(s + 2, "%*c %d %*d %*d %*d %*d %*d %*d %*d %*d %*d %lu %lu", 
                   &p->ppid, &p->utime, &p->stime);
            char *start = strchr(buf, '(');
            if (start) {
                int len = s - start - 1;
                len = (len > 31) ? 31 : len;
                strncpy(p->comm, start + 1, len);
            }
        }
        free(buf);
    }

    // 2. 讀取 /proc/[pid]/status 獲取記憶體
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    buf = xmalloc_open_read_close(path, NULL);
    if (buf) {
        p->vmsize = diag_get_val(buf, "VmSize");
        p->rss = diag_get_val(buf, "VmRSS");
        free(buf);
    }
    return 0;
}

/* --- 檔案系統健康檢測 (服務 diag_fs) --- */

int diag_read_fs(const char *path, diag_fs_t *f) {
    struct statfs s;
    if (statfs(path, &s) != 0) return -1;

    f->total_inodes = s.f_files;
    f->free_inodes = s.f_ffree;
    f->total_bytes = (uint64_t)s.f_blocks * s.f_bsize;
    f->free_bytes = (uint64_t)s.f_bavail * s.f_bsize;
    return 0;
}

/* --- 網路連線狀態 (服務 diag_net) --- */

// TCP 狀態機對照表
const char* diag_get_tcp_state(int state) {
    static const char *tcp_states[] = {
        "UNKNOWN", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1",
        "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK",
        "LISTEN", "CLOSING"
    };
    if (state < 1 || state > 11) return tcp_states[0];
    return tcp_states[state];
}