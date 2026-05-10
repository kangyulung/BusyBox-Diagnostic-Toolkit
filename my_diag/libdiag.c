#include "libdiag.h"
#include <sys/vfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>

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

/*int diag_read_proc(int pid, diag_proc_t *p) {
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
}*/

int diag_read_proc(int pid, diag_proc_t *p) {
    char path[64];
    char *buf;
    
    memset(p, 0, sizeof(diag_proc_t));
    p->pid = pid;

    // 1. 讀取 /proc/[pid]/stat (涵蓋大部分欄位)
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    buf = xmalloc_open_read_close(path, NULL);
    if (buf) {
        char *s_end = strrchr(buf, ')'); // 找最後一個括號
        char *s_start = strchr(buf, '(');
        
        if (s_start && s_end) {
            // 解析名稱 (comm)
            int len = s_end - s_start - 1;
            if (len > sizeof(p->comm) - 1) len = sizeof(p->comm) - 1;
            strncpy(p->comm, s_start + 1, len);
            p->comm[len] = '\0';

            /* 
               從 s_end + 2 開始解析欄位（跳過 ") "）
               對應 stat 格式索引 (從第 3 個欄位 state 開始):
               %c(3) %d(4:ppid) %*d(5) %*d(6) %*d(7) %*d(8) %*u(9) %*u(10) %*u(11) %*u(12) %*u(13) 
               %lu(14:utime) %lu(15:stime) %*d(16) %*d(17) 
               %d(18:priority) %d(19:nice) %d(20:threads) %*d(21) %llu(22:starttime)
            */
            sscanf(s_end + 2, 
                   "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %d %d %d %*d %llu",
                   &p->state, &p->ppid, &p->utime, &p->stime, 
                   &p->priority, &p->nice, &p->threads, &p->start_time);
        }
        free(buf);
    }

    // 2. 獲取記憶體資訊 (雖然 stat 有 rss，但 status 的單位通常較準確，或繼續用你的 diag_get_val)
    // 技巧：如果你追求極致效能，stat 的第 23 欄位其實就是 RSS (以 pages 為單位)
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    buf = xmalloc_open_read_close(path, NULL);
    if (buf) {
        p->vmsize = diag_get_val(buf, "VmSize");
        p->rss = diag_get_val(buf, "VmRSS");
        
        // 額外資訊：UID (如果你不想用額外的系統呼叫)
        p->uid = diag_get_val(buf, "Uid"); // diag_get_val 通常會抓第一個數字
        free(buf);
    }
    
    return 0;
}

/* --- 檔案系統健康檢測 (服務 diag_fs) --- */

int diag_read_fs(const char *path, diag_fs_t *f) {
    struct statfs s;
    if (statfs(path, &s) != 0) return -1;

    f->total_inodes   = s.f_files;
    f->free_inodes    = s.f_ffree;
    /* f_frsize 是實際片段大小，df 用此欄位計算；f_bsize 是最佳傳輸大小，virtiofs 等 fs 兩者差距可達 256x */
    f->total_bytes    = (uint64_t)s.f_blocks * s.f_frsize;
    f->free_bytes     = (uint64_t)s.f_bavail * s.f_frsize;
    f->free_bytes_priv = (uint64_t)s.f_bfree  * s.f_frsize;
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

/* --- 檔案碎片分析（服務 diag_fs）--- */

int diag_read_fragmentation(const char *path, diag_frag_t *f, int collect_extents)
{
    struct stat sb;
    struct fiemap *fm;
    int fd;

    memset(f, 0, sizeof(*f));

    /* O_NOFOLLOW：不追蹤符號連結，避免意外跨越掛載點 */
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;

    if (fstat(fd, &sb) != 0) { close(fd); return -1; }
    if (!S_ISREG(sb.st_mode)) { close(fd); errno = EINVAL; return -1; }
    f->file_size = (uint64_t)sb.st_size;

    /* 第一次呼叫：fm_extent_count=0 → 核心只回傳 fm_mapped_extents（extent 總數），不複製陣列 */
    fm = xzalloc(sizeof(*fm));
    fm->fm_start        = 0;
    fm->fm_length       = FIEMAP_MAX_OFFSET;
    fm->fm_flags        = 0;
    fm->fm_extent_count = 0;

    if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
        free(fm);
        close(fd);
        return -1;
    }
    f->extent_count = fm->fm_mapped_extents;

    if (collect_extents && f->extent_count > 0) {
        /* 第二次呼叫：配置足夠大的緩衝區取回所有 extent 詳細資料 */
        size_t sz = sizeof(*fm) + sizeof(struct fiemap_extent) * f->extent_count;
        free(fm);
        fm = xzalloc(sz);
        fm->fm_start        = 0;
        fm->fm_length       = FIEMAP_MAX_OFFSET;
        fm->fm_flags        = 0;
        fm->fm_extent_count = f->extent_count;

        if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
            free(fm);
            close(fd);
            return -1;
        }
        f->extents = xmalloc(sizeof(struct fiemap_extent) * fm->fm_mapped_extents);
        memcpy(f->extents, fm->fm_extents,
               sizeof(struct fiemap_extent) * fm->fm_mapped_extents);
        f->extent_count = fm->fm_mapped_extents;
    }

    free(fm);
    close(fd);
    return 0;
}

void diag_free_frag(diag_frag_t *f)
{
    if (f && f->extents) {
        free(f->extents);
        f->extents = NULL;
    }
}