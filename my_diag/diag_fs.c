/* vi: set sw=4 ts=4: */
//config:config MY_FS
//config:   bool "my_fs (Filesystem Health Checker)"
//config:   default y
//config:   help
//config:     Filesystem health checker: inode usage and fragmentation analysis.

//applet:IF_MY_FS(APPLET(my_fs, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_FS) += diag_fs.o
//kbuild:lib-$(CONFIG_MY_FS) += libdiag.o

//usage:#define my_fs_trivial_usage "[-h] [-i] [-r] [-t TYPE] [-x TYPE] [-f FILE] [-F PATH] [-s] [PATH]..."
//usage:#define my_fs_full_usage "\n\n"
//usage:       "Show filesystem disk space and inode usage\n"
//usage:     "\n	-h		Human-readable sizes (K/M/G/T)"
//usage:     "\n	-i		Show inode usage instead of block usage"
//usage:     "\n	-r		Show dual Use% (user/real) and reserved blocks"
//usage:     "\n	-t TYPE		Only show filesystems of TYPE"
//usage:     "\n	-x TYPE		Exclude filesystems of TYPE"
//usage:     "\n	-f FILE		Fragmentation analysis for FILE"
//usage:     "\n	-F PATH		Fragmentation statistics for filesystem at PATH"
//usage:     "\n	-s		Interactive TUI (D=disk I=inode R=reserved F=frag H=human Q=quit)"

#include "libbb.h"
#include "libdiag.h"
#include <ftw.h>
#include <mntent.h>
#include <termios.h>
#include <ctype.h>
#include <signal.h>

/* 掛載點清單節點（解析自 /proc/mounts）。
 * 三個字串為 xstrdup 配置，由 free_mount_list 釋放；
 * 避免 PATH_MAX 固定陣列導致每節點浪費 ~8 KB 清零成本。 */
typedef struct mount_node {
    char              *device;
    char              *mountpoint;
    char              *fstype;
    struct mount_node *next;
} mount_node_t;

/* 單一掛載點的展示用欄位，由 get_fs_entry() 計算填入。
 * 三個字串為 xstrdup 配置，需呼叫 free_fs_entry() 釋放。 */
typedef struct {
    char          *device;
    char          *path;
    char          *fstype;
    /* 容量欄位（預設模式） */
    uint64_t      total_1k;
    uint64_t      used_1k;
    uint64_t      avail_1k;
    unsigned      use_pct;       /* ceiling round-up（user 視角，對齊 df） */
    /* inode 欄位（-i 模式） */
    unsigned long total_inodes;
    unsigned long used_inodes;
    unsigned long free_inodes;
    unsigned      iuse_pct;      /* ceiling round-up */
    /* 雙視角欄位（-r 模式） */
    uint64_t      rootresv_1k;   /* (f_bfree - f_bavail) × f_frsize / 1024 */
    unsigned      use_pct_real;  /* ceiling(used_real / total * 100)，含保留區 */
} fs_entry_t;

static void free_fs_entry(fs_entry_t *e)
{
    if (!e) return;
    free(e->device); e->device = NULL;
    free(e->path);   e->path   = NULL;
    free(e->fstype); e->fstype = NULL;
}

/* 解析掛載表，回傳掛載點清單（linked list，順序與檔案相同）。
 * 使用 setmntent/getmntent 正確處理路徑中的 octal 逸脫（\040 → 空格等）。
 * bb_path_mtab_file 依 BusyBox 編譯設定自動選 /etc/mtab 或 /proc/mounts。 */
static mount_node_t *get_mount_list(void)
{
    FILE          *fp;
    struct mntent *entry;
    mount_node_t  *head = NULL, *tail = NULL;

    fp = setmntent(bb_path_mtab_file, "r");
    if (!fp) return NULL;

    while ((entry = getmntent(fp)) != NULL) {
        mount_node_t *node = xzalloc(sizeof(mount_node_t));
        node->device     = xstrdup(entry->mnt_fsname);
        node->mountpoint = xstrdup(entry->mnt_dir);
        node->fstype     = xstrdup(entry->mnt_type);
        if (!head) head = node;
        else       tail->next = node;
        tail = node;
    }
    endmntent(fp);
    return head;
}

static void free_mount_list(mount_node_t *head)
{
    while (head) {
        mount_node_t *next = head->next;
        free(head->device);
        free(head->mountpoint);
        free(head->fstype);
        free(head);
        head = next;
    }
}

/* 呼叫 diag_read_fs() 並將原始欄位換算為展示用數值 */
static int get_fs_entry(const char *path, fs_entry_t *e)
{
    diag_fs_t fs;

    if (diag_read_fs(path, &fs) != 0)
        return -1;

    uint64_t used     = fs.total_bytes - fs.free_bytes_priv;
    uint64_t avail    = fs.free_bytes;
    uint64_t nonr_tot = used + avail;

    e->path = xstrdup(path);
    e->total_1k = fs.total_bytes / 1024;
    e->used_1k  = used / 1024;
    e->avail_1k = avail / 1024;
    /* ceiling(used / nonr_tot * 100)，與 df 的 Use% 計算一致 */
    e->use_pct  = (nonr_tot > 0)
                  ? (unsigned)((used * 100 + nonr_tot - 1) / nonr_tot)
                  : 0;

    /* inode 欄位 */
    unsigned long used_in = (fs.total_inodes >= fs.free_inodes)
                            ? fs.total_inodes - fs.free_inodes : 0;
    e->total_inodes = fs.total_inodes;
    e->used_inodes  = used_in;
    e->free_inodes  = fs.free_inodes;
    e->iuse_pct     = (fs.total_inodes > 0)
                      ? (unsigned)((used_in * 100 + fs.total_inodes - 1) / fs.total_inodes)
                      : 0;

    /* 雙視角欄位：rootresv_1k = 保留區大小（root 專用），use_pct_real = 含保留區的使用率 */
    e->rootresv_1k = (fs.free_bytes_priv > fs.free_bytes)
                     ? (fs.free_bytes_priv - fs.free_bytes) / 1024 : 0;
    {
        uint64_t used_real = (fs.total_bytes >= fs.free_bytes_priv)
                             ? fs.total_bytes - fs.free_bytes_priv : 0;
        e->use_pct_real = (fs.total_bytes > 0)
                          ? (unsigned)((used_real * 100 + fs.total_bytes - 1) / fs.total_bytes)
                          : 0;
    }

    return 0;
}

/* 計算 uint64 值的十進位字元數 */
static int uint64_width(uint64_t v)
{
    int w = 1;
    while (v >= 10) { v /= 10; w++; }
    return w;
}

/* 將 KB 值換算為 human-readable 字串（K/M/G/T），0 直接印 "0" */
static char *fmt_human(uint64_t kb, char *buf, size_t buflen)
{
    static const char units[] = "KMGTPE";
    double val;
    int u;

    if (kb == 0) {
        snprintf(buf, buflen, "0");
        return buf;
    }
    val = (double)kb;
    u   = 0;
    while (val >= 1024.0 && u < (int)(sizeof(units) - 2)) {
        val /= 1024.0;
        u++;
    }
    if (val < 10.0) {
        /* ceiling（與 val>=10 分支一致，對齊 GNU df -h 行為） */
        int t = (int)(val * 10.0);
        if ((double)t < val * 10.0) t++;
        if (t >= 100)
            snprintf(buf, buflen, "%d%c", t / 10, units[u]);
        else
            snprintf(buf, buflen, "%d.%d%c", t / 10, t % 10, units[u]);
    } else {
        /* ceiling */
        uint64_t c = (uint64_t)val;
        if ((double)c < val) c++;
        snprintf(buf, buflen, "%llu%c", (unsigned long long)c, units[u]);
    }
    return buf;
}

/* 將原始計數值（非 KB）換算為 human-readable，0 直接印 "0" */
static char *fmt_human_count(uint64_t n, char *buf, size_t buflen)
{
    static const char units[] = "KMGTPE";
    double val = (double)n;
    int u = -1;

    if (n == 0) { snprintf(buf, buflen, "0"); return buf; }
    while (val >= 1024.0 && u < (int)(sizeof(units) - 2)) {
        val /= 1024.0;
        u++;
    }
    if (u < 0) {
        snprintf(buf, buflen, "%llu", (unsigned long long)n);
    } else if (val < 10.0) {
        /* ceiling（與 val>=10 分支一致，對齊 GNU df -h 行為） */
        int t = (int)(val * 10.0);
        if ((double)t < val * 10.0) t++;
        if (t >= 100)
            snprintf(buf, buflen, "%d%c", t / 10, units[u]);
        else
            snprintf(buf, buflen, "%d.%d%c", t / 10, t % 10, units[u]);
    } else {
        uint64_t c = (uint64_t)val;
        if ((double)c < val) c++;
        snprintf(buf, buflen, "%llu%c", (unsigned long long)c, units[u]);
    }
    return buf;
}

/*
 * 先掃一遍計算各欄最大寬度，再統一列印。
 * 支援三種輸出模式：
 *   inode=1  → -i inode 視圖
 *   reserved=1 → -r 雙視角 Use% + RootResv
 *   兩者皆 0 → 預設容量視圖（對齊 df）
 * 以上三種均可與 human=1 （-h）搭配。
 */
static void print_entries(const fs_entry_t *e, int n, int human, int inode, int reserved)
{
    int dev_w, col1_w, col2_w, col3_w, w, i;

    /* ── inode 模式（-i） ── */
    if (inode) {
        dev_w  = (int)strlen("Filesystem");
        col1_w = (int)strlen("Inodes");
        col2_w = (int)strlen("IUsed");
        col3_w = (int)strlen("IFree");

        if (human) {
            char buf[16];
            int dev_max = 0, c1_max = 0, c2_max = 0, c3_max = 0;

            for (i = 0; i < n; i++) {
                w = (int)strlen(e[i].device); if (w > dev_max) dev_max = w;
                w = (int)strlen(fmt_human_count((uint64_t)e[i].total_inodes, buf, sizeof(buf)));
                if (w > c1_max) c1_max = w;
                w = (int)strlen(fmt_human_count((uint64_t)e[i].used_inodes,  buf, sizeof(buf)));
                if (w > c2_max) c2_max = w;
                w = (int)strlen(fmt_human_count((uint64_t)e[i].free_inodes,  buf, sizeof(buf)));
                if (w > c3_max) c3_max = w;
            }
            /* 若 Inodes 表頭比最長資料寬，右對齊本身已提供視覺間距，device 欄不需加 +1（對齊 df 行為） */
            int i1pad = (c1_max < (int)strlen("Inodes")) ? 0 : 1;
            dev_w  = (dev_max + i1pad > (int)strlen("Filesystem")) ? dev_max + i1pad : (int)strlen("Filesystem");
            col1_w = (c1_max          > (int)strlen("Inodes"))     ? c1_max          : (int)strlen("Inodes");
            col2_w = (c2_max  + 1     > (int)strlen("IUsed"))      ? c2_max  + 1     : (int)strlen("IUsed");
            col3_w = (c3_max  + 1     > (int)strlen("IFree"))      ? c3_max  + 1     : (int)strlen("IFree");

            printf("%-*s %*s %*s %*s %5s %s\n",
                   dev_w, "Filesystem",
                   col1_w, "Inodes", col2_w, "IUsed", col3_w, "IFree",
                   "IUse%", "Mounted on");
            for (i = 0; i < n; i++) {
                char t[16], u_[16], f[16];
                printf("%-*s %*s %*s %*s %4u%% %s\n",
                       dev_w, e[i].device,
                       col1_w, fmt_human_count((uint64_t)e[i].total_inodes, t,  sizeof(t)),
                       col2_w, fmt_human_count((uint64_t)e[i].used_inodes,  u_, sizeof(u_)),
                       col3_w, fmt_human_count((uint64_t)e[i].free_inodes,  f,  sizeof(f)),
                       e[i].iuse_pct, e[i].path);
            }
        } else {
            for (i = 0; i < n; i++) {
                w = (int)strlen(e[i].device);
                if (w > dev_w) dev_w = w;
                w = uint64_width((uint64_t)e[i].total_inodes);
                if (w > col1_w) col1_w = w;
                w = uint64_width((uint64_t)e[i].used_inodes);
                if (w > col2_w) col2_w = w;
                w = uint64_width((uint64_t)e[i].free_inodes);
                if (w > col3_w) col3_w = w;
            }
            printf("%-*s %*s %*s %*s %5s %s\n",
                   dev_w, "Filesystem",
                   col1_w, "Inodes", col2_w, "IUsed", col3_w, "IFree",
                   "IUse%", "Mounted on");
            for (i = 0; i < n; i++) {
                printf("%-*s %*lu %*lu %*lu %4u%% %s\n",
                       dev_w, e[i].device,
                       col1_w, e[i].total_inodes,
                       col2_w, e[i].used_inodes,
                       col3_w, e[i].free_inodes,
                       e[i].iuse_pct, e[i].path);
            }
        }
        return;
    }

    /* ── 雙視角模式（-r）── */
    if (reserved) {
        int resv_w;

        dev_w  = (int)strlen("Filesystem");
        col1_w = (int)strlen("1K-blocks");
        col2_w = (int)strlen("Used");
        col3_w = (int)strlen("Available");
        resv_w = (int)strlen("RootResv");

        if (human) {
            char buf[16];
            int dev_max = 0, c1_max = 0, c2_max = 0, c3_max = 0, rv_max = 0;

            for (i = 0; i < n; i++) {
                w = (int)strlen(e[i].device); if (w > dev_max) dev_max = w;
                w = (int)strlen(fmt_human(e[i].total_1k,    buf, sizeof(buf))); if (w > c1_max) c1_max = w;
                w = (int)strlen(fmt_human(e[i].used_1k,     buf, sizeof(buf))); if (w > c2_max) c2_max = w;
                w = (int)strlen(fmt_human(e[i].avail_1k,    buf, sizeof(buf))); if (w > c3_max) c3_max = w;
                w = (int)strlen(fmt_human(e[i].rootresv_1k, buf, sizeof(buf))); if (w > rv_max) rv_max = w;
            }
            dev_w  = (dev_max + 1 > (int)strlen("Filesystem")) ? dev_max + 1 : (int)strlen("Filesystem");
            col1_w = (c1_max      > (int)strlen("Size"))        ? c1_max      : (int)strlen("Size");
            col2_w = (c2_max  + 1 > (int)strlen("Used"))        ? c2_max  + 1 : (int)strlen("Used");
            col3_w = (c3_max  + 1 > (int)strlen("Avail"))       ? c3_max  + 1 : (int)strlen("Avail");
            resv_w = (rv_max  + 1 > (int)strlen("RootResv"))    ? rv_max  + 1 : (int)strlen("RootResv");

            printf("%-*s %*s %*s %*s %4s %5s %*s %s\n",
                   dev_w, "Filesystem",
                   col1_w, "Size", col2_w, "Used", col3_w, "Avail",
                   "Use%", "RUse%", resv_w, "RootResv", "Mounted on");
            for (i = 0; i < n; i++) {
                char tbuf[16], ubuf[16], abuf[16], rbuf[16];
                printf("%-*s %*s %*s %*s %3u%% %4u%% %*s %s\n",
                       dev_w, e[i].device,
                       col1_w, fmt_human(e[i].total_1k,    tbuf, sizeof(tbuf)),
                       col2_w, fmt_human(e[i].used_1k,     ubuf, sizeof(ubuf)),
                       col3_w, fmt_human(e[i].avail_1k,    abuf, sizeof(abuf)),
                       e[i].use_pct, e[i].use_pct_real,
                       resv_w, fmt_human(e[i].rootresv_1k, rbuf, sizeof(rbuf)),
                       e[i].path);
            }
        } else {
            for (i = 0; i < n; i++) {
                w = (int)strlen(e[i].device);       if (w > dev_w)  dev_w  = w;
                w = uint64_width(e[i].total_1k);    if (w > col1_w) col1_w = w;
                w = uint64_width(e[i].used_1k);     if (w > col2_w) col2_w = w;
                w = uint64_width(e[i].avail_1k);    if (w > col3_w) col3_w = w;
                w = uint64_width(e[i].rootresv_1k); if (w > resv_w) resv_w = w;
            }
            printf("%-*s %*s %*s %*s %4s %5s %*s %s\n",
                   dev_w, "Filesystem",
                   col1_w, "1K-blocks", col2_w, "Used", col3_w, "Available",
                   "Use%", "RUse%", resv_w, "RootResv", "Mounted on");
            for (i = 0; i < n; i++) {
                printf("%-*s %*llu %*llu %*llu %3u%% %4u%% %*llu %s\n",
                       dev_w, e[i].device,
                       col1_w, (unsigned long long)e[i].total_1k,
                       col2_w, (unsigned long long)e[i].used_1k,
                       col3_w, (unsigned long long)e[i].avail_1k,
                       e[i].use_pct, e[i].use_pct_real,
                       resv_w, (unsigned long long)e[i].rootresv_1k,
                       e[i].path);
            }
        }
        return;
    }

    /* ── 預設容量模式（對齊 df）── */
    if (human) {
        char buf[16];
        int dev_max = 0, total_max = 0, used_max = 0, avail_max = 0;

        for (i = 0; i < n; i++) {
            w = (int)strlen(e[i].device);
            if (w > dev_max)   dev_max   = w;
            w = (int)strlen(fmt_human(e[i].total_1k, buf, sizeof(buf)));
            if (w > total_max) total_max = w;
            w = (int)strlen(fmt_human(e[i].used_1k,  buf, sizeof(buf)));
            if (w > used_max)  used_max  = w;
            w = (int)strlen(fmt_human(e[i].avail_1k, buf, sizeof(buf)));
            if (w > avail_max) avail_max = w;
        }

        dev_w  = (dev_max   + 1 > (int)strlen("Filesystem")) ? dev_max   + 1 : (int)strlen("Filesystem");
        col1_w = (total_max     > (int)strlen("Size"))       ? total_max     : (int)strlen("Size");
        col2_w = (used_max  + 1 > (int)strlen("Used"))       ? used_max  + 1 : (int)strlen("Used");
        col3_w = (avail_max + 1 > (int)strlen("Avail"))      ? avail_max + 1 : (int)strlen("Avail");

        printf("%-*s %*s %*s %*s %4s %s\n",
               dev_w, "Filesystem",
               col1_w, "Size", col2_w, "Used", col3_w, "Avail",
               "Use%", "Mounted on");
        for (i = 0; i < n; i++) {
            char tbuf[16], ubuf[16], abuf[16];
            printf("%-*s %*s %*s %*s %3u%% %s\n",
                   dev_w, e[i].device,
                   col1_w, fmt_human(e[i].total_1k, tbuf, sizeof(tbuf)),
                   col2_w, fmt_human(e[i].used_1k,  ubuf, sizeof(ubuf)),
                   col3_w, fmt_human(e[i].avail_1k, abuf, sizeof(abuf)),
                   e[i].use_pct, e[i].path);
        }
    } else {
        dev_w  = (int)strlen("Filesystem");
        col1_w = (int)strlen("1K-blocks");
        col2_w = (int)strlen("Used");
        col3_w = (int)strlen("Available");

        for (i = 0; i < n; i++) {
            w = (int)strlen(e[i].device);
            if (w > dev_w)  dev_w  = w;
            w = uint64_width(e[i].total_1k);
            if (w > col1_w) col1_w = w;
            w = uint64_width(e[i].used_1k);
            if (w > col2_w) col2_w = w;
            w = uint64_width(e[i].avail_1k);
            if (w > col3_w) col3_w = w;
        }

        printf("%-*s %*s %*s %*s %4s %s\n",
               dev_w, "Filesystem",
               col1_w, "1K-blocks", col2_w, "Used", col3_w, "Available",
               "Use%", "Mounted on");
        for (i = 0; i < n; i++) {
            printf("%-*s %*llu %*llu %*llu %3u%% %s\n",
                   dev_w, e[i].device,
                   col1_w, (unsigned long long)e[i].total_1k,
                   col2_w, (unsigned long long)e[i].used_1k,
                   col3_w, (unsigned long long)e[i].avail_1k,
                   e[i].use_pct, e[i].path);
        }
    }
}

/* ── L1：-f FILE 單檔碎片分析 ── */

static int print_file_frag(const char *path)
{
    diag_frag_t f;
    uint64_t blk_size, blocks, expected_phy;
    uint32_t i;

    if (diag_read_fragmentation(path, &f, 1) != 0) {
        bb_perror_msg("%s", path);
        return EXIT_FAILURE;
    }

    /* block_size 由 libdiag 從 fstat.st_blksize 帶出，免再呼叫 statfs */
    blk_size = f.block_size > 0 ? (uint64_t)f.block_size : 4096;
    blocks   = (f.file_size + blk_size - 1) / blk_size;

    printf("File size of %s is %llu (%llu block%s of %llu bytes)\n",
           path,
           (unsigned long long)f.file_size,
           (unsigned long long)blocks,
           blocks == 1 ? "" : "s",
           (unsigned long long)blk_size);

    if (f.extent_count > 0) {
        printf(" ext:     logical_offset:        physical_offset: length:   expected: flags:\n");
        expected_phy = 0;
        for (i = 0; i < f.extent_count; i++) {
            struct fiemap_extent *e = &f.extents[i];
            uint64_t log_start = e->fe_logical  / blk_size;
            uint64_t log_end   = (e->fe_logical  + e->fe_length - 1) / blk_size;
            uint64_t phy_start = e->fe_physical / blk_size;
            uint64_t phy_end   = (e->fe_physical + e->fe_length - 1) / blk_size;
            uint64_t len       = e->fe_length / blk_size;
            char flags[64] = "", exp_str[24] = "";

            if (i > 0 && phy_start != expected_phy)
                snprintf(exp_str, sizeof(exp_str), "%llu", (unsigned long long)expected_phy);

            /* 以逗號連接，避免多旗標時黏成 "last,eofunknown " */
#define ADD_FLAG(bit, name) do {                  \
                if (e->fe_flags & (bit)) {        \
                    if (flags[0]) strcat(flags, ","); \
                    strcat(flags, (name));        \
                }                                 \
            } while (0)
            ADD_FLAG(FIEMAP_EXTENT_LAST,     "last,eof");
            ADD_FLAG(FIEMAP_EXTENT_UNKNOWN,  "unknown");
            ADD_FLAG(FIEMAP_EXTENT_DELALLOC, "delalloc");
            ADD_FLAG(FIEMAP_EXTENT_ENCODED,  "encoded");
#undef ADD_FLAG

            printf(" %3u:  %7llu..%8llu:  %9llu..%10llu: %6llu: %10s  %s\n",
                   i,
                   (unsigned long long)log_start, (unsigned long long)log_end,
                   (unsigned long long)phy_start,  (unsigned long long)phy_end,
                   (unsigned long long)len,
                   exp_str, flags);

            expected_phy = phy_start + len;
        }
    }

    printf("%s: %u extent%s found\n", path,
           f.extent_count, f.extent_count == 1 ? "" : "s");
    diag_free_frag(&f);
    return EXIT_SUCCESS;
}

/* ── L2：-F PATH 掛載點碎片統計 ── */

#define L2_TOP_N 10

struct l2_top_entry {
    char     path[PATH_MAX];
    uint32_t extents;
    uint64_t size;
};

struct l2_ctx {
    uint64_t            total;
    uint64_t            frag;
    uint64_t            skipped;   /* FTW_F 但 FIEMAP 失敗（無權限/不支援）*/
    uint64_t            dist[4];   /* [0]=1, [1]=2-4, [2]=5-16, [3]=17+ */
    struct l2_top_entry top[L2_TOP_N];
    int                 top_count;
};

static struct l2_ctx g_l2;

static int l2_nftw_cb(const char *path, const struct stat *sb,
                      int typeflag, struct FTW *ftwbuf)
{
    diag_frag_t f;
    int i, min_idx;

    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_F) return 0;

    if (diag_read_fragmentation(path, &f, 0) != 0) {
        /* 一般檔案但無法做 FIEMAP（無讀取權限 / fs 不支援），
         * 計入 skipped 以免 total 與碎片率分母被低估 */
        g_l2.skipped++;
        return 0;
    }

    g_l2.total++;

    if      (f.extent_count <= 1)  g_l2.dist[0]++;
    else if (f.extent_count <= 4)  g_l2.dist[1]++;
    else if (f.extent_count <= 16) g_l2.dist[2]++;
    else                           g_l2.dist[3]++;

    if (f.extent_count > 1) g_l2.frag++;

    if (g_l2.top_count < L2_TOP_N) {
        safe_strncpy(g_l2.top[g_l2.top_count].path, path, PATH_MAX);
        g_l2.top[g_l2.top_count].extents = f.extent_count;
        g_l2.top[g_l2.top_count].size    = f.file_size;
        g_l2.top_count++;
    } else {
        /* 清單已滿：以 extent_count 最小的項目作為替換候選 */
        min_idx = 0;
        for (i = 1; i < L2_TOP_N; i++) {
            if (g_l2.top[i].extents < g_l2.top[min_idx].extents)
                min_idx = i;
        }
        if (f.extent_count > g_l2.top[min_idx].extents) {
            safe_strncpy(g_l2.top[min_idx].path, path, PATH_MAX);
            g_l2.top[min_idx].extents = f.extent_count;
            g_l2.top[min_idx].size    = f.file_size;
        }
    }

    return 0;
}

static int cmp_top_entry(const void *a, const void *b)
{
    const struct l2_top_entry *ea = (const struct l2_top_entry *)a;
    const struct l2_top_entry *eb = (const struct l2_top_entry *)b;
    return (ea->extents > eb->extents) ? -1 : (ea->extents < eb->extents) ? 1 : 0;
}

static int print_frag_stat(const char *path)
{
    double frag_pct;
    int i;

    memset(&g_l2, 0, sizeof(g_l2));
    printf("Scanning %s ...\n\n", path);
    /* nftw 回傳 -1 表示連 root path 都無法走訪（不存在/無權限等）；
     * callback 一律回 0，故 0 = 正常走完 */
    if (nftw(path, l2_nftw_cb, 16, FTW_MOUNT | FTW_PHYS) < 0) {
        bb_perror_msg("%s", path);
        return EXIT_FAILURE;
    }

    frag_pct = (g_l2.total > 0)
               ? (double)g_l2.frag * 100.0 / (double)g_l2.total : 0.0;
    printf("Scanned: %llu files  Fragmented: %llu (%.1f%%)  Skipped: %llu\n",
           (unsigned long long)g_l2.total,
           (unsigned long long)g_l2.frag, frag_pct,
           (unsigned long long)g_l2.skipped);
    if (g_l2.skipped > 0)
        printf("(skipped = no read permission or filesystem without FIEMAP;"
               " run as root for full coverage)\n");
    printf("\n");

    printf("Fragmentation distribution:\n");
    printf("  %-10s  %s\n",   "Extents", "Files");
    printf("  %-10s  %llu\n", "1",    (unsigned long long)g_l2.dist[0]);
    printf("  %-10s  %llu\n", "2-4",  (unsigned long long)g_l2.dist[1]);
    printf("  %-10s  %llu\n", "5-16", (unsigned long long)g_l2.dist[2]);
    printf("  %-10s  %llu\n", "17+",  (unsigned long long)g_l2.dist[3]);

    if (g_l2.top_count > 0) {
        qsort(g_l2.top, g_l2.top_count, sizeof(g_l2.top[0]), cmp_top_entry);
        printf("\nTop %d most fragmented:\n", g_l2.top_count);
        printf("  %7s  %s\n", "Extents", "File");
        for (i = 0; i < g_l2.top_count; i++)
            printf("  %7u  %s\n", g_l2.top[i].extents, g_l2.top[i].path);
    }
    return EXIT_SUCCESS;
}

/* ── P5：互動式 TUI 模式（-s） ── */

typedef enum {
    FS_VIEW_DF,
    FS_VIEW_INODE,
    FS_VIEW_RESERVED,
    FS_VIEW_FRAG,
} fs_view_t;

static fs_view_t     g_tui_view       = FS_VIEW_DF;
static int           g_tui_human      = 0;
static int           g_tui_frag_ready = 0;
static struct l2_ctx g_tui_frag_cache;

/* 掛載表快取：TTL 內重複使用同一份清單，避免每秒重讀 /proc/mounts */
#define MOUNT_CACHE_TTL 10
static mount_node_t *g_tui_mount_cache    = NULL;
static time_t        g_tui_mount_cache_ts = 0;

static void tui_refresh_mount_cache(void)
{
    time_t now = time(NULL);
    mount_node_t *fresh;
    if (g_tui_mount_cache && (now - g_tui_mount_cache_ts) < MOUNT_CACHE_TTL)
        return;
    /* 取新清單成功才替換舊的；失敗則保留舊 cache 且不更新 ts，
     * 下一輪會再重試，避免暫時讀取失敗就把畫面清成空表 */
    fresh = get_mount_list();
    if (!fresh)
        return;
    free_mount_list(g_tui_mount_cache);
    g_tui_mount_cache    = fresh;
    g_tui_mount_cache_ts = now;
}

/* 掃描 mounts，依 st_dev 去重，回傳 fs_entry_t 陣列。
 * 回傳筆數；*out 需逐筆 free_fs_entry() 後再 free() 陣列。
 * has_t/opt_t、has_x/opt_x 為類型過濾（傳 0/NULL 即不過濾，供 TUI 用）。
 * 採「第一遍即暫存 statfs 結果並轉移字串擁有權」，每唯一掛載點只 statfs 一次；
 * seen 三陣列依 mount 數動態配置，無固定 256 上限。
 * mounts 生命週期由呼叫端管理，此函式不釋放它。 */
static int collect_dedup_entries(mount_node_t *mounts,
                                 int has_t, const char *opt_t,
                                 int has_x, const char *opt_x,
                                 fs_entry_t **out)
{
    int           mount_count = 0, seen_n = 0, n = 0, i;
    mount_node_t *m;

    for (m = mounts; m; m = m->next) mount_count++;
    if (mount_count == 0) mount_count = 1;   /* xzalloc(0) 防呆 */

    dev_t         *seen_dev   = xzalloc(mount_count * sizeof(dev_t));
    mount_node_t **seen_node  = xzalloc(mount_count * sizeof(mount_node_t *));
    fs_entry_t    *seen_entry = xzalloc(mount_count * sizeof(fs_entry_t));

    for (m = mounts; m; m = m->next) {
        fs_entry_t  tmp;
        struct stat sb;
        memset(&tmp, 0, sizeof(tmp));
        if (has_t && strcmp(m->fstype, opt_t) != 0) continue;
        if (has_x && strcmp(m->fstype, opt_x) == 0) continue;
        if (get_fs_entry(m->mountpoint, &tmp) != 0 || tmp.total_1k == 0) {
            free_fs_entry(&tmp);
            continue;
        }
        if (stat(m->mountpoint, &sb) != 0) {
            free_fs_entry(&tmp);
            continue;
        }
        int found = 0;
        for (i = 0; i < seen_n; i++) {
            if (seen_dev[i] == sb.st_dev) {
                seen_node[i]  = m;
                /* 同裝置 bind mount：先釋放舊條目字串，再轉移 tmp 擁有權 */
                free_fs_entry(&seen_entry[i]);
                seen_entry[i] = tmp;
                found = 1;
                break;
            }
        }
        if (!found) {
            /* seen_n 必 < mount_count（每唯一掛載點僅 +1），無需上限檢查 */
            seen_dev[seen_n]   = sb.st_dev;
            seen_node[seen_n]  = m;
            seen_entry[seen_n] = tmp;   /* 轉移擁有權 */
            seen_n++;
        }
    }

    fs_entry_t *entries = xzalloc((seen_n ? seen_n : 1) * sizeof(fs_entry_t));
    for (i = 0; i < seen_n; i++) {
        entries[n] = seen_entry[i];                  /* 轉移字串擁有權 */
        entries[n].device = xstrdup(seen_node[i]->device);
        entries[n].fstype = xstrdup(seen_node[i]->fstype);
        n++;
    }
    free(seen_dev);
    free(seen_node);
    free(seen_entry);   /* 字串擁有權已轉移至 entries，只釋放陣列 */
    *out = entries;
    return n;
}

static const char *tui_view_name(void)
{
    switch (g_tui_view) {
    case FS_VIEW_DF:       return "Disk";
    case FS_VIEW_INODE:    return "Inode";
    case FS_VIEW_RESERVED: return "Reserved";
    case FS_VIEW_FRAG:     return "Fragment";
    default:               return "?";
    }
}

static void tui_print_header(void)
{
    printf("\033[H\033[J");
    printf(DIAG_CYAN "[MY_FS]" DIAG_RESET
           " View: " DIAG_YELLOW "%s" DIAG_RESET
           "  Human: %s"
           DIAG_CLR_EOL "\n",
           tui_view_name(),
           g_tui_human ? (DIAG_GREEN "on" DIAG_RESET) : "off");
    printf("D=disk  I=inode  R=reserved  F=frag(scan)  H=human  Q=quit"
           DIAG_CLR_EOL "\n");
    printf("------------------------------------------------------------"
           DIAG_CLR_EOL "\n");
}

/* 對 / 執行 nftw 碎片統計，結果存入快取 */
static void tui_do_frag_scan(void)
{
    printf("\033[H\033[J");
    printf(DIAG_YELLOW "Scanning / for fragmentation, please wait..."
           DIAG_RESET DIAG_CLR_EOL "\n");
    fflush(stdout);
    memset(&g_l2, 0, sizeof(g_l2));
    nftw("/", l2_nftw_cb, 16, FTW_MOUNT | FTW_PHYS);
    g_tui_frag_cache = g_l2;
    g_tui_frag_ready = 1;
}

static void tui_print_frag_view(void)
{
    int i;
    double pct;
    struct l2_ctx *c = &g_tui_frag_cache;

    if (!g_tui_frag_ready) {
        printf("  (press F to start fragmentation scan on /)" DIAG_CLR_EOL "\n");
        return;
    }
    pct = (c->total > 0) ? (double)c->frag * 100.0 / (double)c->total : 0.0;

    printf("Scan path: /" DIAG_CLR_EOL "\n");
    printf("Scanned: %llu files  Fragmented: %llu (%.1f%%)  Skipped: %llu"
           DIAG_CLR_EOL "\n\n",
           (unsigned long long)c->total,
           (unsigned long long)c->frag, pct,
           (unsigned long long)c->skipped);
    printf("Fragmentation distribution:" DIAG_CLR_EOL "\n");
    printf("  %-10s  %s" DIAG_CLR_EOL "\n",   "Extents", "Files");
    printf("  %-10s  %llu" DIAG_CLR_EOL "\n", "1",    (unsigned long long)c->dist[0]);
    printf("  %-10s  %llu" DIAG_CLR_EOL "\n", "2-4",  (unsigned long long)c->dist[1]);
    printf("  %-10s  %llu" DIAG_CLR_EOL "\n", "5-16", (unsigned long long)c->dist[2]);
    printf("  %-10s  %llu" DIAG_CLR_EOL "\n", "17+",  (unsigned long long)c->dist[3]);

    if (c->top_count > 0) {
        qsort(c->top, c->top_count, sizeof(c->top[0]), cmp_top_entry);
        printf("\nTop %d most fragmented:" DIAG_CLR_EOL "\n", c->top_count);
        printf("  %7s  %s" DIAG_CLR_EOL "\n", "Extents", "File");
        for (i = 0; i < c->top_count; i++)
            printf("  %7u  %s" DIAG_CLR_EOL "\n",
                   c->top[i].extents, c->top[i].path);
    }
    printf("\n  [cached - press F to re-scan]" DIAG_CLR_EOL "\n");
}

/* 非阻塞讀取一個按鍵（toupper 正規化後存入 *out），回傳 1 有輸入 / 0 無輸入 */
static int tui_read_key(char *out)
{
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    if (safe_poll(&pfd, 1, 0) <= 0) return 0;
    if (read(STDIN_FILENO, out, 1) <= 0) return 0;
    *out = (char)toupper((unsigned char)*out);
    return 1;
}

/* TUI 異常離開的還原機制：
 * raw mode 一旦開啟，必須在「正常 Q 離開」「Ctrl-C / SIGTERM / SIGHUP」
 * 「迴圈中 x* allocator die」三種路徑都還原 termios 與游標，
 * 否則 shell 會卡在 raw mode、游標維持隱藏，需手動 stty sane / reset。 */
static struct termios     g_tui_saved_termios;
static volatile sig_atomic_t g_tui_active = 0;

/* 還原 termios 與游標。可能在 signal handler 中執行，
 * 故僅用 async-signal-safe 的 tcsetattr() 與 write()，不碰 stdio。 */
static void tui_restore(void)
{
    static const char show_cursor[] = DIAG_SHOW;
    if (!g_tui_active)
        return;
    g_tui_active = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_saved_termios);
    write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
}

/* atexit hook：涵蓋 BusyBox x* allocator 在迴圈中 die 的情況 */
static void tui_atexit(void)
{
    tui_restore();
}

/* fatal signal handler：還原後回復預設處置並重發訊號，
 * 讓行程以正確的 128+signo 狀態結束。 */
static void tui_sig_handler(int sig)
{
    tui_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void show_fs_tui(void)
{
    struct pollfd  pfd = { STDIN_FILENO, POLLIN, 0 };

    if (!isatty(STDOUT_FILENO))
        bb_error_msg_and_die("-s requires a terminal");

    diag_ui_mode_raw(&g_tui_saved_termios);
    g_tui_active = 1;
    atexit(tui_atexit);
    bb_signals(BB_FATAL_SIGS, tui_sig_handler);
    printf(DIAG_HIDE);
    fflush(stdout);

    while (1) {
        char c = 0;
        tui_read_key(&c);

        if      (c == 'Q') break;
        else if (c == 'D') g_tui_view = FS_VIEW_DF;
        else if (c == 'I') g_tui_view = FS_VIEW_INODE;
        else if (c == 'R') g_tui_view = FS_VIEW_RESERVED;
        else if (c == 'F') { g_tui_view = FS_VIEW_FRAG; tui_do_frag_scan(); }
        else if (c == 'H') g_tui_human = !g_tui_human;

        tui_print_header();
        if (g_tui_view == FS_VIEW_FRAG) {
            tui_print_frag_view();
        } else {
            tui_refresh_mount_cache();
            fs_entry_t *entries = NULL;
            int i, n = collect_dedup_entries(g_tui_mount_cache,
                                             0, NULL, 0, NULL, &entries);
            print_entries(entries, n, g_tui_human,
                          g_tui_view == FS_VIEW_INODE,
                          g_tui_view == FS_VIEW_RESERVED);
            for (i = 0; i < n; i++)
                free_fs_entry(&entries[i]);
            free(entries);
        }
        fflush(stdout);

        safe_poll(&pfd, 1, 1000);
    }

    free_mount_list(g_tui_mount_cache);
    g_tui_mount_cache = NULL;

    g_tui_active = 0;
    printf(DIAG_SHOW);
    fflush(stdout);
    diag_ui_mode_normal(&g_tui_saved_termios);
    printf("\n");
    fflush(stdout);
}

int my_fs_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_fs_main(int argc, char **argv)
{
    char    *opt_t = NULL, *opt_x = NULL, *opt_f = NULL, *opt_F = NULL;
    unsigned opts     = getopt32(argv, "hirt:x:f:F:s", &opt_t, &opt_x, &opt_f, &opt_F);
    int      human    = (opts & (1 << 0));
    int      inode    = (opts & (1 << 1));
    int      reserved = (opts & (1 << 2));
    int      has_t    = (opts & (1 << 3));
    int      has_x    = (opts & (1 << 4));
    int      has_f    = (opts & (1 << 5));
    int      has_F    = (opts & (1 << 6));
    int      has_s    = (opts & (1 << 7));
    argv += optind;

    /* -f / -F / -s 為互斥的 action mode；同時給多個語意不明，
     * 明確報 usage error 而非靜默只跑其中一個 */
    if (!!has_f + !!has_F + !!has_s > 1)
        bb_show_usage();

    if (has_f) return print_file_frag(opt_f);
    if (has_F) return print_frag_stat(opt_F);
    if (has_s) { g_tui_human = human; show_fs_tui(); return EXIT_SUCCESS; }

    mount_node_t *mounts = get_mount_list();
    fs_entry_t   *entries;
    int           n = 0, i;
    int           had_error = 0;

    if (!argv[0]) {
        /* no-arg 模式必須能列舉掛載表；NULL = setmntent 開啟失敗
         * （Linux /proc/mounts 恆有資料，空清單不視為正常狀況），
         * 明確回報而非靜默印空表後回 0 */
        if (!mounts) {
            bb_perror_msg("%s", bb_path_mtab_file);
            return EXIT_FAILURE;
        }
        n = collect_dedup_entries(mounts, has_t, opt_t, has_x, opt_x, &entries);
    } else {
        /* 有參數：對每個路徑收集 fs_entry_t */
        char **arg;
        int   argc_n = 0;
        for (arg = argv; *arg; arg++) argc_n++;

        entries = xzalloc(argc_n * sizeof(fs_entry_t));
        for (arg = argv; *arg; arg++) {
            mount_node_t *m, *best = NULL;
            size_t best_len = 0;
            char *canon;
            const char *cpath;

            if (get_fs_entry(*arg, &entries[n]) != 0) {
                bb_perror_msg("%s", *arg);
                had_error = 1;
                continue;
            }
            /* 對齊 df：先 realpath 正規化為絕對路徑（解 relative path /
             * symlink / .. ），再做「邊界感知的最長掛載點前綴」比對。
             * 邊界檢查避免 /foo 誤吃 /foobar；以最長前綴選最深掛載點，
             * 在多個 bind mount（同裝置不同 mountpoint）時也能選對。 */
            canon = xmalloc_realpath(*arg);
            cpath = canon ? canon : *arg;
            for (m = mounts; m; m = m->next) {
                size_t len = strlen(m->mountpoint);
                if (strncmp(cpath, m->mountpoint, len) != 0) continue;
                /* mountpoint=="/" 時 mountpoint[len-1]=='/' 恆相符；
                 * 否則要求 cpath 在 len 處為結尾或路徑分隔符 */
                if (!(m->mountpoint[len - 1] == '/'
                      || cpath[len] == '\0' || cpath[len] == '/'))
                    continue;
                if (!best || len > best_len) {
                    best = m;
                    best_len = len;
                }
            }
            free(canon);
            if (best) {
                if ((has_t && strcmp(best->fstype, opt_t) != 0)
                 || (has_x && strcmp(best->fstype, opt_x) == 0)) {
                    free_fs_entry(&entries[n]);   /* 過濾掉時釋放已配置的 path */
                    continue;
                }
                entries[n].device = xstrdup(best->device);
                entries[n].fstype = xstrdup(best->fstype);
                /* Mounted on 對齊 df：顯示實際掛載點而非使用者輸入字串 */
                free(entries[n].path);
                entries[n].path = xstrdup(best->mountpoint);
            } else {
                entries[n].device = xstrdup(*arg);
            }
            n++;
        }
    }

    print_entries(entries, n, human, inode, reserved);
    for (i = 0; i < n; i++)
        free_fs_entry(&entries[i]);
    free(entries);
    free_mount_list(mounts);
    return had_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
