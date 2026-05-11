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
#include <termios.h>
#include <ctype.h>

/* 掛載點清單節點（解析自 /proc/mounts） */
typedef struct mount_node {
    char              device[PATH_MAX];
    char              mountpoint[PATH_MAX];
    char              fstype[64];
    struct mount_node *next;
} mount_node_t;

/* 單一掛載點的展示用欄位，由 get_fs_entry() 計算填入 */
typedef struct {
    char          device[PATH_MAX];
    char          path[PATH_MAX];
    char          fstype[64];
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

/* 解析 /proc/mounts，回傳掛載點清單（linked list，順序與檔案相同） */
static mount_node_t *get_mount_list(void)
{
    char *buf = xmalloc_open_read_close("/proc/mounts", NULL);
    if (!buf) return NULL;

    mount_node_t *head = NULL, *tail = NULL;
    char *line = buf;

    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        char dev[PATH_MAX], mp[PATH_MAX], fst[64];
        if (sscanf(line, "%s %s %s", dev, mp, fst) == 3) {
            mount_node_t *node = xzalloc(sizeof(mount_node_t));
            safe_strncpy(node->device,     dev, sizeof(node->device));
            safe_strncpy(node->mountpoint, mp,  sizeof(node->mountpoint));
            safe_strncpy(node->fstype,     fst, sizeof(node->fstype));
            if (!head) head = node;
            else       tail->next = node;
            tail = node;
        }
        line = eol ? eol + 1 : NULL;
    }
    free(buf);
    return head;
}

static void free_mount_list(mount_node_t *head)
{
    while (head) {
        mount_node_t *next = head->next;
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

    safe_strncpy(e->path, path, sizeof(e->path));
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
        /* round-half-up（對齊 df 行為，避免 C printf 的 round-half-to-even） */
        int t = (int)(val * 10.0 + 0.5);
        if (t >= 100)
            snprintf(buf, buflen, "%d%c", t / 10, units[u]);
        else
            snprintf(buf, buflen, "%d.%d%c", t / 10, t % 10, units[u]);
    } else {
        /* ceiling（對齊 df 行為） */
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
        int t = (int)(val * 10.0 + 0.5);
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

static void print_file_frag(const char *path)
{
    diag_frag_t f;
    struct statfs sfs;
    uint64_t blk_size, blocks, expected_phy;
    uint32_t i;

    if (diag_read_fragmentation(path, &f, 1) != 0) {
        bb_perror_msg("%s", path);
        return;
    }

    /* f_bsize：filesystem 宣告的 block size，filefrag 用此欄位換算 */
    blk_size = (statfs(path, &sfs) == 0 && sfs.f_bsize > 0) ? (uint64_t)sfs.f_bsize : 4096;
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

            if (e->fe_flags & FIEMAP_EXTENT_LAST)     strcat(flags, "last,eof");
            if (e->fe_flags & FIEMAP_EXTENT_UNKNOWN)  strcat(flags, "unknown ");
            if (e->fe_flags & FIEMAP_EXTENT_DELALLOC) strcat(flags, "delalloc ");
            if (e->fe_flags & FIEMAP_EXTENT_ENCODED)  strcat(flags, "encoded ");

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

    if (diag_read_fragmentation(path, &f, 0) != 0) return 0;

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

static void print_frag_stat(const char *path)
{
    double frag_pct;
    int i;

    memset(&g_l2, 0, sizeof(g_l2));
    printf("Scanning %s ...\n\n", path);
    nftw(path, l2_nftw_cb, 16, FTW_MOUNT | FTW_PHYS);

    frag_pct = (g_l2.total > 0)
               ? (double)g_l2.frag * 100.0 / (double)g_l2.total : 0.0;
    printf("Scanned: %llu files  Fragmented: %llu (%.1f%%)\n\n",
           (unsigned long long)g_l2.total,
           (unsigned long long)g_l2.frag, frag_pct);

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

/* 收集所有有效掛載點的 fs_entry_t；*out 需呼叫 free() 釋放 */
static int collect_all_entries(fs_entry_t **out)
{
    mount_node_t *mounts = get_mount_list();
    dev_t         seen_dev[256];
    mount_node_t *seen_node[256];
    int           seen_n = 0, n = 0, i;
    mount_node_t *m;

    for (m = mounts; m; m = m->next) {
        fs_entry_t tmp;
        struct stat sb;
        if (get_fs_entry(m->mountpoint, &tmp) != 0 || tmp.total_1k == 0)
            continue;
        if (stat(m->mountpoint, &sb) != 0)
            continue;
        int found = 0;
        for (i = 0; i < seen_n; i++) {
            if (seen_dev[i] == sb.st_dev) { seen_node[i] = m; found = 1; break; }
        }
        if (!found && seen_n < 256) {
            seen_dev[seen_n]  = sb.st_dev;
            seen_node[seen_n] = m;
            seen_n++;
        }
    }

    fs_entry_t *entries = xzalloc((seen_n ? seen_n : 1) * sizeof(fs_entry_t));
    for (i = 0; i < seen_n; i++) {
        if (get_fs_entry(seen_node[i]->mountpoint, &entries[n]) != 0) continue;
        safe_strncpy(entries[n].device, seen_node[i]->device, sizeof(entries[n].device));
        safe_strncpy(entries[n].fstype, seen_node[i]->fstype, sizeof(entries[n].fstype));
        n++;
    }
    free_mount_list(mounts);
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
    printf("Scanned: %llu files  Fragmented: %llu (%.1f%%)" DIAG_CLR_EOL "\n\n",
           (unsigned long long)c->total,
           (unsigned long long)c->frag, pct);
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
    if (poll(&pfd, 1, 0) <= 0) return 0;
    if (read(STDIN_FILENO, out, 1) <= 0) return 0;
    *out = (char)toupper((unsigned char)*out);
    return 1;
}

static void show_fs_tui(void)
{
    struct termios old_t;
    struct pollfd  pfd = { STDIN_FILENO, POLLIN, 0 };

    if (!isatty(STDOUT_FILENO))
        bb_error_msg_and_die("-s requires a terminal");

    diag_ui_mode_raw(&old_t);
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
            fs_entry_t *entries = NULL;
            int n = collect_all_entries(&entries);
            print_entries(entries, n, g_tui_human,
                          g_tui_view == FS_VIEW_INODE,
                          g_tui_view == FS_VIEW_RESERVED);
            free(entries);
        }
        fflush(stdout);

        poll(&pfd, 1, 1000);
    }

    printf(DIAG_SHOW);
    fflush(stdout);
    diag_ui_mode_normal(&old_t);
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

    if (has_f) { print_file_frag(opt_f); return EXIT_SUCCESS; }
    if (has_F) { print_frag_stat(opt_F); return EXIT_SUCCESS; }
    if (has_s) { g_tui_human = human; show_fs_tui(); return EXIT_SUCCESS; }

    mount_node_t *mounts = get_mount_list();
    fs_entry_t   *entries;
    int           n = 0, i;

    if (!argv[0]) {
        dev_t        seen_dev[256];
        mount_node_t *seen_node[256];
        int          seen_n = 0;
        mount_node_t *m;

        /* 第一遍：過濾虛擬 fs 與 -t/-x 類型，以 st_dev 去重，保留每裝置最後出現的掛載點 */
        for (m = mounts; m; m = m->next) {
            fs_entry_t tmp;
            if (get_fs_entry(m->mountpoint, &tmp) != 0 || tmp.total_1k == 0)
                continue;
            if (has_t && strcmp(m->fstype, opt_t) != 0) continue;
            if (has_x && strcmp(m->fstype, opt_x) == 0) continue;
            struct stat sb;
            if (stat(m->mountpoint, &sb) != 0)
                continue;
            int found = 0;
            for (i = 0; i < seen_n; i++) {
                if (seen_dev[i] == sb.st_dev) {
                    seen_node[i] = m;
                    found = 1;
                    break;
                }
            }
            if (!found && seen_n < 256) {
                seen_dev[seen_n]  = sb.st_dev;
                seen_node[seen_n] = m;
                seen_n++;
            }
        }

        /* 第二遍：收集 fs_entry_t 到陣列，供 print_entries 計算動態欄寬 */
        entries = xzalloc((seen_n ? seen_n : 1) * sizeof(fs_entry_t));
        for (i = 0; i < seen_n; i++) {
            if (get_fs_entry(seen_node[i]->mountpoint, &entries[n]) != 0)
                continue;
            safe_strncpy(entries[n].device, seen_node[i]->device, sizeof(entries[n].device));
            safe_strncpy(entries[n].fstype, seen_node[i]->fstype, sizeof(entries[n].fstype));
            n++;
        }
    } else {
        /* 有參數：對每個路徑收集 fs_entry_t */
        char **arg;
        int   argc_n = 0;
        for (arg = argv; *arg; arg++) argc_n++;

        entries = xzalloc(argc_n * sizeof(fs_entry_t));
        for (arg = argv; *arg; arg++) {
            mount_node_t *m, *best = NULL;
            size_t best_len = 0;

            if (get_fs_entry(*arg, &entries[n]) != 0) {
                bb_perror_msg("%s", *arg);
                continue;
            }
            for (m = mounts; m; m = m->next) {
                size_t len = strlen(m->mountpoint);
                if (strncmp(*arg, m->mountpoint, len) == 0 && len > best_len) {
                    best = m;
                    best_len = len;
                }
            }
            if (best) {
                if (has_t && strcmp(best->fstype, opt_t) != 0) continue;
                if (has_x && strcmp(best->fstype, opt_x) == 0) continue;
                safe_strncpy(entries[n].device, best->device, sizeof(entries[n].device));
                safe_strncpy(entries[n].fstype, best->fstype, sizeof(entries[n].fstype));
            } else {
                safe_strncpy(entries[n].device, *arg, sizeof(entries[n].device));
            }
            n++;
        }
    }

    print_entries(entries, n, human, inode, reserved);
    free(entries);
    free_mount_list(mounts);
    return EXIT_SUCCESS;
}
