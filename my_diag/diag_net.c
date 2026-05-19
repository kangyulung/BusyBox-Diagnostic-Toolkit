/* vi: set sw=4 ts=4: */
// clang-format off
//config:config MY_NET
//config:   bool "my_net (Network Connection Monitor)"
//config:   default y
//config:   help
//config:     Network connection state monitor: TCP/UDP socket listing,
//config:     TCP state machine tracking, connection anomaly detection.
//config:     Reads /proc/net/tcp[6] and /proc/net/udp[6].
//config:     Compatible with ss(8) and netstat(8) output format.

//applet:IF_MY_NET(APPLET(my_net, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_NET) += diag_net.o
//kbuild:lib-$(CONFIG_MY_NET) += libdiag.o

//usage:#define my_net_trivial_usage "[-t] [-u] [-a] [-l] [-n] [-s STATE] [-w SEC] [-b] [-p]"
//usage:#define my_net_full_usage "\n\n"
//usage:       "Show network connections and socket statistics\n"
//usage:     "\n	-t		TCP sockets (default)"
//usage:     "\n	-u		UDP sockets"
//usage:     "\n	-a		All sockets (TCP + UDP)"
//usage:     "\n	-l		Listening sockets only"
//usage:     "\n	-n		Numeric output (no hostname resolution)"
//usage:     "\n	-s STATE	Filter by TCP state (ESTABLISHED, TIME_WAIT, LISTEN, ...)"
//usage:     "\n	-w SEC		Watch mode: auto-refresh every SEC seconds (Q to quit)"
//usage:     "\n	-b		Batch mode (plain text output, suitable for scripts)"
//usage:     "\n	-p		Show PID/program (requires root for all processes)"
// clang-format on

#include "libbb.h"
#include "libdiag.h"
#include <termios.h>
#include <ctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/syscall.h>

/* ═══════════════════════════════════════════════════════════════
 * 資料結構
 * ═══════════════════════════════════════════════════════════════ */

/* 解析後的單一 socket 記錄 */
typedef struct net_entry {
    char              proto[8];
    /* --- 改存 raw binary，延遲格式化 --- */
    uint32_t          laddr4, raddr4;       /* IPv4 (host byte order) */
    uint32_t          laddr6[4], raddr6[4]; /* IPv6 (已 htonl) */
    uint16_t          lport, rport;
    uint8_t           is_ipv6;
    /* ---------------------------------- */
    int               state;
    unsigned          uid;
    unsigned long     inode;
    pid_t             pid;
    char              comm[32];
    struct net_entry *next;
} net_entry_t;

/* inode -> PID 對照表（掃描 /proc/PID/fd/ 建立） */
#define INODE_MAP_MAX 4096
typedef struct {
    unsigned long inode;
    pid_t         pid;
    char          comm[32];
} inode_map_t;

static inode_map_t g_imap[INODE_MAP_MAX];
static int         g_imap_cnt = 0;

/* ── 靜態記憶體池：消除 per-entry xzalloc ── */
#define ENTRY_POOL_MAX  1024
static net_entry_t *g_entry_pool = NULL;
static int          g_pool_idx   = 0;

static void ensure_pool(void) {
    if (!g_entry_pool)
        g_entry_pool = xmalloc(sizeof(net_entry_t) * ENTRY_POOL_MAX);
}

static net_entry_t *pool_alloc(void)
{
    if (g_pool_idx >= ENTRY_POOL_MAX) return NULL;
    net_entry_t *e = &g_entry_pool[g_pool_idx++];
    memset(e, 0, sizeof(*e));
    return e;
}

/* ── UID → 使用者名稱快取（避免重複 getpwuid 呼叫） ── */
#define UID_CACHE_MAX   32
static struct {
    unsigned    uid;
    char        name[32];   /* 直接內嵌，避免二次 heap 分配 */
    int         valid;
} g_uid_cache[UID_CACHE_MAX];
static int g_uid_cache_cnt = 0;

static const char *diag_uid2uname(unsigned uid)
{
    int i;
    /* 線性搜尋：uid 種類通常 < 10，不需 hash */
    for (i = 0; i < g_uid_cache_cnt; i++) {
        if (g_uid_cache[i].uid == uid)
            return g_uid_cache[i].name;
    }
    /* 快取未命中：查詢並存入 */
    if (g_uid_cache_cnt < UID_CACHE_MAX) {
        const char *n = uid2uname(uid);   /* libbb 原始查詢 */
        g_uid_cache[g_uid_cache_cnt].uid = uid;
        safe_strncpy(g_uid_cache[g_uid_cache_cnt].name, n,
                     sizeof(g_uid_cache[0].name));
        return g_uid_cache[g_uid_cache_cnt++].name;
    }
    return uid2uname(uid);   /* 池滿時直接查詢，不快取 */
}

/* ── 靜態 /proc 讀取緩衝：省去 xmalloc_open_read_close 的 heap 分配 ── */
#define PROC_NET_BUF  (256 * 1024)   /* 256 KB 足應對 ~4000 條連線 */
static char g_proc_buf[PROC_NET_BUF];

/* ── 輸出集中緩衝：減少 write() 次數與 printf 格式化開銷 ── */
#define OUT_BUF_SIZE  (128 * 1024)
static char g_out_buf[OUT_BUF_SIZE];
static int  g_out_pos = 0;

/* 把緩衝內容一次寫出 */
static void outbuf_flush(void)
{
    if (g_out_pos > 0) {
        fwrite(g_out_buf, 1, g_out_pos, stdout);
        g_out_pos = 0;
    }
}

/* 把字串 s（長度 len）追加到緩衝，自動觸發 flush */
static void outbuf_write(const char *s, int len)
{
    if (len <= 0) return;
    if (g_out_pos + len >= OUT_BUF_SIZE) outbuf_flush();
    memcpy(g_out_buf + g_out_pos, s, len);
    g_out_pos += len;
}

/* 快速整數轉十進位字串，回傳寫入長度 */
static int fast_uint(char *p, unsigned v)
{
    char tmp[10]; int i = 0, len;
    if (!v) { *p = '0'; return 1; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    len = i;
    while (i--) *p++ = tmp[i];
    return len;
}

/* 快速 IPv4 格式化：寫入 "a.b.c.d:port"，回傳長度 */
static int fast_format_ipv4(char *p, uint32_t addr, uint16_t port)
{
    unsigned char *b = (unsigned char *)&addr;
    char *s = p;
    s += fast_uint(s, b[0]); *s++ = '.';
    s += fast_uint(s, b[1]); *s++ = '.';
    s += fast_uint(s, b[2]); *s++ = '.';
    s += fast_uint(s, b[3]); *s++ = ':';
    s += fast_uint(s, port);
    *s = '\0';
    return (int)(s - p);
}
/*
 * 讀取 /proc/net/* 至靜態緩衝。
 * 回傳 g_proc_buf 指標（呼叫者不得 free），失敗回傳 NULL。
 * 注意：不支援並發呼叫（parse_net_file 本身是單執行緒）。
 */
static char *read_proc_net(const char *path)
{
    int     fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    n = read(fd, g_proc_buf, PROC_NET_BUF - 1);
    close(fd);
    if (n <= 0) return NULL;
    g_proc_buf[n] = '\0';
    return g_proc_buf;
}

/* 統計用 */
#define N_TCP_STATES 12
typedef struct {
    int counts[N_TCP_STATES + 1]; /* index = state code (1~11) */
    int tcp_total;
    int udp_total;
} net_stats_t;

/* 異常警告 */
#define WARN_MAX 8
static char g_warn[WARN_MAX][160];
static int  g_warn_cnt;

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[1]; /* flexible; accessed via pointer cast */
};

#define DENTS_BUF 4096

/* ═══════════════════════════════════════════════════════════════
 * 位址解析：/proc/net/tcp[6] 的 hex 格式 → 可讀字串
 * ═══════════════════════════════════════════════════════════════ */

/*
 * IPv4：8 hex chars + ':' + 4 hex chars
 * /proc/net/tcp 以 little-endian 32-bit 整數存放（x86 native），
 * 直接 cast 成 in_addr_t 再 inet_ntoa 即得正確點分十進位。
 */
static void parse_ipv4_addr(const char *hex, char *out, size_t outlen)
{
    unsigned long raw_addr;
    unsigned int  port;
    struct in_addr in;

    if (sscanf(hex, "%lX:%X", &raw_addr, &port) != 2) {
        snprintf(out, outlen, "?:?");
        return;
    }
    in.s_addr = (in_addr_t)raw_addr;
    snprintf(out, outlen, "%s:%u", inet_ntoa(in), port);
}

/*
 * IPv6：32 hex chars + ':' + 4 hex chars
 * /proc/net/tcp6 以 4 個 host-endian uint32 存放，需各別 htonl 後
 * 再組成 in6_addr 才能得到正確的網路位元組序。
 */
static void parse_ipv6_addr(const char *hex, char *out, size_t outlen)
{
    unsigned int port;
    uint32_t     w[4];
    struct in6_addr in6;
    char         ip_str[INET6_ADDRSTRLEN];
    const char  *colon = strchr(hex, ':');

    if (!colon || (colon - hex) != 32) {
        snprintf(out, outlen, "?:?");
        return;
    }
    if (sscanf(hex, "%8X%8X%8X%8X:%X",
               &w[0], &w[1], &w[2], &w[3], &port) != 5) {
        snprintf(out, outlen, "?:?");
        return;
    }
    /* 每個 32-bit word 在 LE 系統上以 host byte order 存放，需轉回 network order */
    w[0] = htonl(w[0]); w[1] = htonl(w[1]);
    w[2] = htonl(w[2]); w[3] = htonl(w[3]);
    memcpy(&in6, w, sizeof(in6));
    inet_ntop(AF_INET6, &in6, ip_str, sizeof(ip_str));
    snprintf(out, outlen, "[%s]:%u", ip_str, port);
}

static uint32_t parse_hex8(const char *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)p[i];
        v = (v << 4) | (isdigit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

static int parse_proc_line_raw(const char *line, int is_ipv6,
                                uint32_t *laddr4, uint16_t *lport,
                                uint32_t *raddr4, uint16_t *rport,
                                uint32_t laddr6[4], uint32_t raddr6[4],
                                int *state_out, unsigned *uid_out,
                                unsigned long *inode_out)
{
    char *p = (char *)line;

    while (*p == ' ') p++;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ') p++;

    if (is_ipv6) {
        laddr6[0] = htonl(parse_hex8(p)); p += 8;
        laddr6[1] = htonl(parse_hex8(p)); p += 8;
        laddr6[2] = htonl(parse_hex8(p)); p += 8;
        laddr6[3] = htonl(parse_hex8(p)); p += 8;
        if (*p != ':') return 0; p++;
        *lport = (uint16_t)strtoul(p, &p, 16);
        while (*p == ' ') p++;

        raddr6[0] = htonl(parse_hex8(p)); p += 8;
        raddr6[1] = htonl(parse_hex8(p)); p += 8;
        raddr6[2] = htonl(parse_hex8(p)); p += 8;
        raddr6[3] = htonl(parse_hex8(p)); p += 8;
        if (*p != ':') return 0; p++;
        *rport = (uint16_t)strtoul(p, &p, 16);
        *laddr4 = 0; *raddr4 = 0;
    } else {
        *laddr4 = parse_hex8(p); p += 8;
        if (*p != ':') return 0; p++;
        *lport = (uint16_t)strtoul(p, &p, 16);
        while (*p == ' ') p++;

        *raddr4 = parse_hex8(p); p += 8;
        if (*p != ':') return 0; p++;
        *rport = (uint16_t)strtoul(p, &p, 16);
    }
    while (*p == ' ') p++;

    *state_out = (int)strtoul(p, &p, 16);
    while (*p == ' ') p++;

    strtoul(p, &p, 16); if (*p == ':') { p++; strtoul(p, &p, 16); }
    while (*p == ' ') p++;
    strtoul(p, &p, 16); if (*p == ':') { p++; strtoul(p, &p, 16); }
    while (*p == ' ') p++;
    strtoul(p, &p, 16);
    while (*p == ' ') p++;

    *uid_out = (unsigned)strtoul(p, &p, 10);
    while (*p == ' ') p++;
    strtoul(p, &p, 10);
    while (*p == ' ') p++;

    *inode_out = strtoul(p, &p, 10);
    return 1;
}

static int parse_proc_line(const char *line, int is_ipv6,
                            char *local_out,  size_t local_len,
                            char *remote_out, size_t remote_len,
                            int *state_out, unsigned *uid_out,
                            unsigned long *inode_out)
{
    char *p = (char *)line;

    /* 跳過行首空白與 sl 序號（十進位）及其後的 ':' */
    while (*p == ' ') p++;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ') p++;

    if (is_ipv6) {
        /* local: 4 × 8 hex（無分隔符）→ htonl → in6_addr */
        uint32_t la[4], ra[4];
        struct in6_addr in6;
        char ip[INET6_ADDRSTRLEN];
        unsigned int lport, rport;

        la[0] = htonl(parse_hex8(p)); p += 8;
        la[1] = htonl(parse_hex8(p)); p += 8;
        la[2] = htonl(parse_hex8(p)); p += 8;
        la[3] = htonl(parse_hex8(p)); p += 8;
        if (*p != ':') return 0; p++;
        lport = (unsigned int)strtoul(p, &p, 16);
        while (*p == ' ') p++;

        ra[0] = htonl(parse_hex8(p)); p += 8;
        ra[1] = htonl(parse_hex8(p)); p += 8;
        ra[2] = htonl(parse_hex8(p)); p += 8;
        ra[3] = htonl(parse_hex8(p)); p += 8;
        if (*p != ':') return 0; p++;
        rport = (unsigned int)strtoul(p, &p, 16);

        memcpy(&in6, la, sizeof(in6));
        inet_ntop(AF_INET6, &in6, ip, sizeof(ip));
        snprintf(local_out,  local_len,  "[%s]:%u", ip, lport);
        memcpy(&in6, ra, sizeof(in6));
        inet_ntop(AF_INET6, &in6, ip, sizeof(ip));
        snprintf(remote_out, remote_len, "[%s]:%u", ip, rport);

    } else {
        struct in_addr lin, rin;
        unsigned int lport, rport;

        lin.s_addr = (in_addr_t)parse_hex8(p); p += 8;
        if (*p != ':') return 0; p++;
        lport = (unsigned int)strtoul(p, &p, 16);
        while (*p == ' ') p++;

        rin.s_addr = (in_addr_t)parse_hex8(p); p += 8;
        if (*p != ':') return 0; p++;
        rport = (unsigned int)strtoul(p, &p, 16);

        snprintf(local_out,  local_len,  "%s:%u", inet_ntoa(lin), lport);
        snprintf(remote_out, remote_len, "%s:%u", inet_ntoa(rin), rport);
    }
    while (*p == ' ') p++;

    /* st */
    *state_out = (int)strtoul(p, &p, 16);
    while (*p == ' ') p++;

    /* tx:rx（略過）*/
    strtoul(p, &p, 16); if (*p == ':') { p++; strtoul(p, &p, 16); }
    while (*p == ' ') p++;

    /* tr:when（略過）*/
    strtoul(p, &p, 16); if (*p == ':') { p++; strtoul(p, &p, 16); }
    while (*p == ' ') p++;

    /* retrnsmt（略過）*/
    strtoul(p, &p, 16);
    while (*p == ' ') p++;

    /* uid（十進位）*/
    *uid_out = (unsigned)strtoul(p, &p, 10);
    while (*p == ' ') p++;

    /* timeout（略過）*/
    strtoul(p, &p, 10);
    while (*p == ' ') p++;

    /* inode */
    *inode_out = strtoul(p, &p, 10);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * inode → PID 對照表（掃描 /proc/<pid>/fd/）
 * ═══════════════════════════════════════════════════════════════ */

static int imap_cmp(const void *a, const void *b) {
    const inode_map_t *ia = a, *ib = b;
    if (ia->inode < ib->inode) return -1;
    if (ia->inode > ib->inode) return  1;
    return 0;
}

static void build_inode_map(void)
{
    int           proc_fd, pid_fd, comm_fd, fd_dirfd, r;
    long          nread, off, fn, fo;
    struct linux_dirent64 *d, *fd_d;
    pid_t         pid;
    char          dents_buf[DENTS_BUF];
    char          fd_dents[DENTS_BUF];
    char          comm_buf[32];
    char          link[80];
    ssize_t       lr;
    unsigned long inode;

    g_imap_cnt = 0;
    proc_fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (proc_fd < 0) return;

    while ((nread = syscall(SYS_getdents64, proc_fd,
                            dents_buf, DENTS_BUF)) > 0) {
        for (off = 0; off < nread; ) {
            d = (struct linux_dirent64 *)(dents_buf + off);
            off += (long)d->d_reclen;

            if (!isdigit((unsigned char)d->d_name[0])) continue;
            pid = (pid_t)atoi(d->d_name);

            pid_fd = openat(proc_fd, d->d_name, O_RDONLY | O_DIRECTORY);
            if (pid_fd < 0) continue;

            memset(comm_buf, 0, sizeof(comm_buf));
            comm_fd = openat(pid_fd, "comm", O_RDONLY);
            if (comm_fd >= 0) {
                r = read(comm_fd, comm_buf, sizeof(comm_buf) - 1);
                if (r > 0) {
                    comm_buf[r] = '\0';
                    comm_buf[strcspn(comm_buf, "\n")] = '\0';
                }
                close(comm_fd);
            }

            fd_dirfd = openat(pid_fd, "fd", O_RDONLY | O_DIRECTORY);
            if (fd_dirfd >= 0) {
                while ((fn = syscall(SYS_getdents64, fd_dirfd,
                                     fd_dents, DENTS_BUF)) > 0) {
                    for (fo = 0; fo < fn; ) {
                        fd_d = (struct linux_dirent64 *)(fd_dents + fo);
                        fo += (long)fd_d->d_reclen;
                        if (!isdigit((unsigned char)fd_d->d_name[0])) continue;

                        lr = readlinkat(fd_dirfd, fd_d->d_name,
                                        link, sizeof(link) - 1);
                        if (lr <= 0) continue;
                        link[lr] = '\0';

                        if (sscanf(link, "socket:[%lu]", &inode) == 1
                            && g_imap_cnt < INODE_MAP_MAX) {
                            g_imap[g_imap_cnt].inode = inode;
                            g_imap[g_imap_cnt].pid   = pid;
                            safe_strncpy(g_imap[g_imap_cnt].comm,
                                         comm_buf,
                                         sizeof(g_imap[0].comm));
                            g_imap_cnt++;
                        }
                    }
                }
                close(fd_dirfd);
            }
            close(pid_fd);
        }
    }
    close(proc_fd);

    if (g_imap_cnt > 1)
        qsort(g_imap, g_imap_cnt, sizeof(g_imap[0]), imap_cmp);
}

static void lookup_inode(unsigned long inode,
                         pid_t *out_pid, char *out_comm, size_t commlen)
{
    int lo = 0, hi = g_imap_cnt - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_imap[mid].inode == inode) {
            *out_pid = g_imap[mid].pid;
            safe_strncpy(out_comm, g_imap[mid].comm, commlen);
            return;
        }
        if (g_imap[mid].inode < inode) lo = mid + 1;
        else hi = mid - 1;
    }
    *out_pid = 0;
    safe_strncpy(out_comm, "-", commlen);
}

/* ═══════════════════════════════════════════════════════════════
 * 解析 /proc/net/{tcp,tcp6,udp,udp6}
 * ═══════════════════════════════════════════════════════════════ */

static net_entry_t *parse_net_file(const char *path, const char *proto,
                                   int is_ipv6, int resolve_pid,
                                   int listen_only,       
                                   int filter_state, int target_state)
{
    char e_local_tmp[64], e_remote_tmp[64];
    char *buf = read_proc_net(path);
    if (!buf) return NULL;

    net_entry_t *head = NULL, *tail = NULL;

    /* 第一行為表頭，直接跳過 */
    char *line = strchr(buf, '\n');
    if (line) line++;

    while (line && *line) {
        char *eol = strchr(line, '\n');         
        if (eol) *eol = '\0';

        int state_hex; unsigned uid; unsigned long inode;
        uint32_t la4 = 0, ra4 = 0, la6[4] = {0}, ra6[4] = {0};
        uint16_t lp = 0, rp = 0;

        if (!parse_proc_line_raw(line, is_ipv6,
                                &la4, &lp, &ra4, &rp,
                                la6, ra6,
                                &state_hex, &uid, &inode))
            goto next_line;

        if (listen_only && state_hex != 10) goto next_line;
        if (filter_state && state_hex != target_state) goto next_line;

        {
            net_entry_t *e = pool_alloc();
            if (!e) break;
            safe_strncpy(e->proto, proto, sizeof(e->proto));
            e->laddr4  = la4;  e->raddr4  = ra4;
            e->lport   = lp;   e->rport   = rp;
            e->is_ipv6 = (uint8_t)is_ipv6;
            if (is_ipv6) {
                memcpy(e->laddr6, la6, 16);
                memcpy(e->raddr6, ra6, 16);
            }
            e->state = state_hex;
            e->uid   = uid;
            e->inode = inode;
            if (resolve_pid)
                lookup_inode(inode, &e->pid, e->comm, sizeof(e->comm));
            else {
                e->pid = 0;
                e->comm[0] = '-'; e->comm[1] = '\0';
            }
            if (!head) head = e;
            else        tail->next = e;
            tail = e;
        }

        next_line:                            
            line = eol ? eol + 1 : NULL;
    }                
    return head;
}

static void free_net_list(net_entry_t *head)
{
    (void)head;
    g_pool_idx = 0;   /* 重置索引即可，pool 記憶體靜態，無需逐節點 free */
    g_uid_cache_cnt = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * 統計與異常偵測
 * ═══════════════════════════════════════════════════════════════ */

static void count_states(net_entry_t *head, net_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    for (net_entry_t *e = head; e; e = e->next) {
        if (strncmp(e->proto, "udp", 3) == 0) {
            st->udp_total++;
        } else {
            st->tcp_total++;
            if (e->state >= 1 && e->state <= N_TCP_STATES)
                st->counts[e->state]++;
        }
    }
}

/*
 * 異常偵測閾值（參考 Linux 核心文件與 RFC 793）：
 *   TIME_WAIT > 500  → 高連線週轉率 or tcp_tw_reuse 未啟用
 *   CLOSE_WAIT > 20  → 應用程式未正確關閉 socket（connection leak）
 *   SYN_RECV > 100   → 可能遭受 SYN flood 攻擊
 *   LAST_ACK > 50    → 遠端未回應 FIN-ACK（對端已失聯）
 *   FIN_WAIT2 > 100  → half-open 連線積壓（tcp_fin_timeout 過長）
 *   ESTABLISHED > 10000 → 連線數異常高，可能有資源洩漏
 */
static void detect_anomalies(const net_stats_t *st)
{
    g_warn_cnt = 0;

#define ADD_WARN(fmt, ...) \
    do { if (g_warn_cnt < WARN_MAX) \
        snprintf(g_warn[g_warn_cnt++], sizeof(g_warn[0]), \
                 fmt, ##__VA_ARGS__); } while (0)

    if (st->counts[6]  > 500)
        ADD_WARN("TIME_WAIT=%d (>500): high churn rate or "
                 "net.ipv4.tcp_tw_reuse not enabled",
                 st->counts[6]);
    if (st->counts[8]  > 20)
        ADD_WARN("CLOSE_WAIT=%d (>20): possible connection leak "
                 "(app not calling close())",
                 st->counts[8]);
    if (st->counts[3]  > 100)
        ADD_WARN("SYN_RECV=%d (>100): possible SYN flood attack",
                 st->counts[3]);
    if (st->counts[9]  > 50)
        ADD_WARN("LAST_ACK=%d (>50): peer not responding to FIN "
                 "(network issue or remote crash)",
                 st->counts[9]);
    if (st->counts[5]  > 100)
        ADD_WARN("FIN_WAIT2=%d (>100): many half-open connections "
                 "(check net.ipv4.tcp_fin_timeout)",
                 st->counts[5]);
    if (st->counts[1]  > 10000)
        ADD_WARN("ESTABLISHED=%d (>10000): unusually high connection count",
                 st->counts[1]);
#undef ADD_WARN
}

/* ═══════════════════════════════════════════════════════════════
 * 格式化輸出
 * ═══════════════════════════════════════════════════════════════ */

static void print_header(int show_pid, int batch)
{
    const char *eol = batch ? "" : DIAG_CLR_EOL;

    if (show_pid) {
        printf("%-6s %-14s %-42s %-42s %-16s %s%s\n",
               "Proto", "State",
               "Local Address", "Foreign Address",
               "PID/Program", "User", eol);
    } else {
        printf("%-6s %-14s %-42s %-42s %s%s\n",
               "Proto", "State",
               "Local Address", "Foreign Address",
               "User", eol);
    }
    if (!batch)
        printf("%.128s%s\n",
               "----------------------------------------------"
               "----------------------------------------------"
               "---------------------------------",
               DIAG_CLR_EOL);
}

static void format_ipv4(uint32_t addr, uint16_t port, char *out, size_t outlen)
{
    struct in_addr in;
    in.s_addr = addr;
    snprintf(out, outlen, "%s:%u", inet_ntoa(in), (unsigned)port);
}

static void format_ipv6(const uint32_t addr6[4], uint16_t port,
                        char *out, size_t outlen)
{
    struct in6_addr in6;
    char ip[INET6_ADDRSTRLEN];
    memcpy(&in6, addr6, sizeof(in6));
    inet_ntop(AF_INET6, &in6, ip, sizeof(ip));
    snprintf(out, outlen, "[%s]:%u", ip, (unsigned)port);
}

static void print_entry(const net_entry_t *e, int show_pid, int batch)
{
    const char *state_str = (strncmp(e->proto, "udp", 3) == 0)
                            ? "-" : diag_get_tcp_state(e->state);
    const char *user = diag_uid2uname(e->uid);
    const char *eol  = batch ? "" : DIAG_CLR_EOL;
    char local[64], remote[64];

    if (e->is_ipv6) {
        format_ipv6(e->laddr6, e->lport, local,  sizeof(local));
        format_ipv6(e->raddr6, e->rport, remote, sizeof(remote));
    } else {
        fast_format_ipv4(local,  e->laddr4, e->lport);   /* 快速格式化 */
        fast_format_ipv4(remote, e->raddr4, e->rport);
    }

    if (show_pid) {
        char pid_prog[36];
        if (e->pid > 0)
            snprintf(pid_prog, sizeof(pid_prog), "%d/%s", (int)e->pid, e->comm);
        else
            safe_strncpy(pid_prog, "-", sizeof(pid_prog));
        printf("%-6s %-14s %-42s %-42s %-16s %s%s\n",
               e->proto, state_str, local, remote, pid_prog, user, eol);
    } else {
        printf("%-6s %-14s %-42s %-42s %s%s\n",
               e->proto, state_str, local, remote, user, eol);
    }
}

/* TCP 狀態分布摘要 + 異常警告 */
static void print_summary(const net_stats_t *st, int batch)
{
    static const char *state_names[] = {
        NULL,
        "ESTABLISHED", "SYN_SENT",  "SYN_RECV",  "FIN_WAIT1",
        "FIN_WAIT2",   "TIME_WAIT", "CLOSE",      "CLOSE_WAIT",
        "LAST_ACK",    "LISTEN",    "CLOSING"
    };
    const char *eol = batch ? "" : DIAG_CLR_EOL;

    printf("\n%sTCP state summary%s (total=%d)%s%s\n",
           batch ? "" : DIAG_CYAN,
           batch ? "" : DIAG_RESET,
           st->tcp_total, eol,
           batch ? "" : DIAG_CLR_EOL);
    for (int i = 1; i <= 11; i++) {
        if (st->counts[i] > 0)
            printf("  %-15s %d%s\n", state_names[i], st->counts[i], eol);
    }
    if (st->udp_total > 0)
        printf("UDP total        %d%s\n", st->udp_total, eol);

    if (g_warn_cnt > 0) {
        printf("\n%s[ANOMALY DETECTED]%s%s\n",
               batch ? "" : DIAG_RED,
               batch ? "" : DIAG_RESET, eol);
        for (int i = 0; i < g_warn_cnt; i++)
            printf("  %s! %s%s%s\n",
                   batch ? "" : DIAG_YELLOW,
                   g_warn[i],
                   batch ? "" : DIAG_RESET, eol);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 篩選邏輯
 * ═══════════════════════════════════════════════════════════════ */

/*
 * 根據命令列旗標決定是否顯示此 entry：
 *   listen_only=1 → 只顯示 LISTEN (state=10=0x0A)
 *   filter_state=1 → 以 state_str（不分大小寫）比對 TCP 狀態名稱
 */
static int entry_visible(const net_entry_t *e,
                          int listen_only, int filter_state,
                          const char *state_str)
{
    if (listen_only && e->state != 10) return 0;
    if (filter_state && state_str && *state_str) {
        const char *es = diag_get_tcp_state(e->state);
        if (strcasecmp(es, state_str) != 0) return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * 核心：單次掃描與顯示
 * ═══════════════════════════════════════════════════════════════ */

/* procfs 資料來源表 */
static const struct {
    const char *path;
    const char *proto;
    int         is_ipv6;
    int         is_udp;
} g_sources[] = {
    { "/proc/net/tcp",  "tcp",  0, 0 },
    { "/proc/net/tcp6", "tcp6", 1, 0 },
    { "/proc/net/udp",  "udp",  0, 1 },
    { "/proc/net/udp6", "udp6", 1, 1 },
};

static void do_scan(int show_tcp, int show_udp,
                    int listen_only, int filter_state, const char *state_str,
                    int show_pid, int batch)
{
    net_entry_t *list = NULL, *tail = NULL;

    ensure_pool();   /* ← 補上，確保 pool 已初始化 */

    /* 把 state_str 轉成數字，供 parse_net_file 做早期過濾 */
    int target_state = -1;
    if (filter_state && state_str && *state_str) {
        static const char *names[] = {
            NULL,"ESTABLISHED","SYN_SENT","SYN_RECV","FIN_WAIT1",
            "FIN_WAIT2","TIME_WAIT","CLOSE","CLOSE_WAIT","LAST_ACK",
            "LISTEN","CLOSING"
        };
        for (int i = 1; i <= 11; i++)
            if (strcasecmp(names[i], state_str) == 0) { target_state = i; break; }
    }

    if (show_pid) build_inode_map();

    for (size_t i = 0; i < ARRAY_SIZE(g_sources); i++) {
        if (g_sources[i].is_udp  && !show_udp)  continue;
        if (!g_sources[i].is_udp && !show_tcp)  continue;

        net_entry_t *part = parse_net_file(
                                g_sources[i].path,
                                g_sources[i].proto,
                                g_sources[i].is_ipv6,
                                show_pid,
                                listen_only,         
                                filter_state,        
                                target_state);
        if (!part) continue;

        if (!list) list = part;
        else        tail->next = part;
        tail = part;
        while (tail->next) tail = tail->next;
    }

    net_stats_t st;
    count_states(list, &st);
    detect_anomalies(&st);

    print_header(show_pid, batch);

    int printed = 0;
    for (net_entry_t *e = list; e; e = e->next) {
        if (!entry_visible(e, listen_only, filter_state, state_str)) continue;
        print_entry(e, show_pid, batch);
        printed++;
    }

    if (printed == 0)
        printf("(no matching connections)%s\n",
               batch ? "" : DIAG_CLR_EOL);

    if (show_tcp) print_summary(&st, batch);

    fflush(stdout);
    free_net_list(list);
}

/* ═══════════════════════════════════════════════════════════════
 * Watch 模式（-w，互動式自動更新）
 * ═══════════════════════════════════════════════════════════════ */

static void do_watch(int show_tcp, int show_udp,
                     int listen_only, int filter_state, const char *state_str,
                     int show_pid, int interval_sec)
{
    struct termios old_t;
    struct pollfd  pfd = { STDIN_FILENO, POLLIN, 0 };
    char           key;

    if (!isatty(STDOUT_FILENO))
        bb_error_msg_and_die("-w requires a terminal; "
                             "use -b for non-interactive output");

    diag_ui_mode_raw(&old_t);

    while (1) {
        /* 非阻塞讀取按鍵 */
        if (poll(&pfd, 1, 0) > 0 && read(STDIN_FILENO, &key, 1) > 0) {
            if (toupper((unsigned char)key) == 'Q')
                break;
        }

        /* 清屏並印表頭 */
        printf("\033[H\033[J");
        printf(DIAG_CYAN "[MY_NET]" DIAG_RESET
               " Refresh: %ds"
               "  TCP:%s UDP:%s Listen-only:%s"
               "  Press Q to quit"
               DIAG_CLR_EOL "\n",
               interval_sec,
               show_tcp  ? "on" : "off",
               show_udp  ? "on" : "off",
               listen_only ? "on" : "off");
        printf("%.90s" DIAG_CLR_EOL "\n",
               "------------------------------------------------------"
               "------------------------------------------------------");

        do_scan(show_tcp, show_udp, listen_only, filter_state, state_str,
                show_pid, 0 /* batch=0 → TUI 模式 */);

        fflush(stdout);
        poll(&pfd, 1, interval_sec * 1000);
    }

    diag_ui_mode_normal(&old_t);
    printf("\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */

int my_net_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_net_main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOFBF, 65536);
    char     *opt_s = NULL;
    char     *opt_w = NULL;
    unsigned  opts;
    int       opt_t, opt_u, show_all;
    int       show_tcp, show_udp;
    int       listen_only, filter_state, has_watch, batch, show_pid;
    int       interval, is_batch;

    /*
     * getopt32 選項字串：t u a l n p b s: w:
     * bit 0=t  1=u  2=a  3=l  4=n  5=p  6=b  7=s  8=w
     */
    opts = getopt32(argv, "tualnpbs:w:", &opt_s, &opt_w);

    opt_t        = (opts & (1 << 0));
    opt_u        = (opts & (1 << 1));
    show_all     = (opts & (1 << 2));
    listen_only  = (opts & (1 << 3));
    /* bit 4 = n（numeric，保留供未來擴充） */
    show_pid     = (opts & (1 << 5));   /* -p */
    batch        = (opts & (1 << 6));   /* -b */
    filter_state = (opts & (1 << 7));   /* -s */
    has_watch    = (opts & (1 << 8));   /* -w */

    if (show_all) {
        show_tcp = 1; show_udp = 1;
    } else if (opt_u && !opt_t) {
        show_tcp = 0; show_udp = 1;
    } else if (opt_t && opt_u) {
        show_tcp = 1; show_udp = 1;
    } else if (opt_t) {
        show_tcp = 1; show_udp = 0;
    } else {
        show_tcp = 1; show_udp = 0;    /* 預設：僅 TCP */
    }

    interval = 2;
    if (has_watch && opt_w) {
        interval = atoi(opt_w);
        if (interval < 1) interval = 1;
    }

    if (has_watch && !batch) {
        do_watch(show_tcp, show_udp, listen_only,
                 filter_state, opt_s ? opt_s : "",
                 show_pid, interval);
    } else {
        is_batch = batch || !isatty(STDOUT_FILENO);
        do_scan(show_tcp, show_udp, listen_only,
                filter_state, opt_s ? opt_s : "",
                show_pid, is_batch);
    }

    return EXIT_SUCCESS;
}