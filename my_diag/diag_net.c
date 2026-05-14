/* vi: set sw=4 ts=4: */
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

//usage:#define my_net_trivial_usage "[-t] [-u] [-a] [-l] [-n] [-s STATE] [-w SEC] [-b]"
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

#include "libbb.h"
#include "libdiag.h"
#include <termios.h>
#include <ctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

/* ═══════════════════════════════════════════════════════════════
 * 資料結構
 * ═══════════════════════════════════════════════════════════════ */

/* 解析後的單一 socket 記錄 */
typedef struct net_entry {
    char              proto[8];     /* "tcp" / "tcp6" / "udp" / "udp6" */
    char              local[64];    /* "addr:port" 字串 */
    char              remote[64];
    int               state;        /* TCP 狀態碼（0x01~0x0B）；UDP 固定 0x07 */
    unsigned          uid;
    unsigned long     inode;
    pid_t             pid;          /* 0 = 未解析或無權限 */
    char              comm[32];     /* 行程名稱 */
    struct net_entry *next;
} net_entry_t;

/* inode -> PID 對照表（掃描 /proc/PID/fd/ 建立） */
#define INODE_MAP_MAX 8192
typedef struct {
    unsigned long inode;
    pid_t         pid;
    char          comm[32];
} inode_map_t;

static inode_map_t g_imap[INODE_MAP_MAX];
static int         g_imap_cnt = 0;

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

/* ═══════════════════════════════════════════════════════════════
 * inode → PID 對照表（掃描 /proc/<pid>/fd/）
 * ═══════════════════════════════════════════════════════════════ */

static void build_inode_map(void)
{
    DIR           *proc_dp, *fd_dp;
    struct dirent *proc_de, *fd_de;
    char           fd_dir[64], link_target[80], link_path[96];
    char           comm_path[64];
    ssize_t        len;

    g_imap_cnt = 0;
    proc_dp = opendir("/proc");
    if (!proc_dp) return;

    while ((proc_de = readdir(proc_dp)) != NULL) {
        pid_t pid;
        FILE *fp;
        char  comm_buf[32];

        if (!isdigit((unsigned char)proc_de->d_name[0])) continue;
        pid = (pid_t)atoi(proc_de->d_name);

        snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", (int)pid);
        fd_dp = opendir(fd_dir);
        if (!fd_dp) continue;

        /* 讀行程名稱（comm）*/
        comm_buf[0] = '\0';
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
        fp = fopen(comm_path, "r");
        if (fp) {
            if (fgets(comm_buf, sizeof(comm_buf), fp))
                comm_buf[strcspn(comm_buf, "\n")] = '\0';
            fclose(fp);
        }

        while ((fd_de = readdir(fd_dp)) != NULL &&
               g_imap_cnt < INODE_MAP_MAX) {
            unsigned long inode;

            if (!isdigit((unsigned char)fd_de->d_name[0])) continue;
            snprintf(link_path, sizeof(link_path), "%s/%s",
                     fd_dir, fd_de->d_name);
            len = readlink(link_path, link_target, sizeof(link_target) - 1);
            if (len <= 0) continue;
            link_target[len] = '\0';

            /* 符號連結格式：socket:[<inode>] */
            if (sscanf(link_target, "socket:[%lu]", &inode) == 1) {
                g_imap[g_imap_cnt].inode = inode;
                g_imap[g_imap_cnt].pid   = pid;
                safe_strncpy(g_imap[g_imap_cnt].comm, comm_buf,
                             sizeof(g_imap[g_imap_cnt].comm));
                g_imap_cnt++;
            }
        }
        closedir(fd_dp);
    }
    closedir(proc_dp);
}

static void lookup_inode(unsigned long inode,
                         pid_t *out_pid, char *out_comm, size_t commlen)
{
    for (int i = 0; i < g_imap_cnt; i++) {
        if (g_imap[i].inode == inode) {
            *out_pid = g_imap[i].pid;
            safe_strncpy(out_comm, g_imap[i].comm, commlen);
            return;
        }
    }
    *out_pid = 0;
    safe_strncpy(out_comm, "-", commlen);
}

/* ═══════════════════════════════════════════════════════════════
 * 解析 /proc/net/{tcp,tcp6,udp,udp6}
 * ═══════════════════════════════════════════════════════════════ */

static net_entry_t *parse_net_file(const char *path, const char *proto,
                                   int is_ipv6, int resolve_pid)
{
    char *buf = xmalloc_open_read_close(path, NULL);
    if (!buf) return NULL;

    net_entry_t *head = NULL, *tail = NULL;

    /* 第一行為表頭，直接跳過 */
    char *line = strchr(buf, '\n');
    if (line) line++;

    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        /*
         * 格式（/proc/net/tcp）：
         *   sl  local_addr  rem_addr  st  tx:rx  tr:when  retrnsmt  uid  timeout  inode
         */
        char          local_hex[64], remote_hex[64];
        int           state_hex;
        unsigned      uid;
        unsigned long inode;

        int n = sscanf(line,
                       " %*d: %63s %63s %X %*X:%*X %*X:%*X %*X %u %*d %lu",
                       local_hex, remote_hex, &state_hex, &uid, &inode);
        if (n == 5) {
            net_entry_t *e = xzalloc(sizeof(*e));
            safe_strncpy(e->proto, proto, sizeof(e->proto));
            e->state = state_hex;
            e->uid   = uid;
            e->inode = inode;

            if (is_ipv6) {
                parse_ipv6_addr(local_hex,  e->local,  sizeof(e->local));
                parse_ipv6_addr(remote_hex, e->remote, sizeof(e->remote));
            } else {
                parse_ipv4_addr(local_hex,  e->local,  sizeof(e->local));
                parse_ipv4_addr(remote_hex, e->remote, sizeof(e->remote));
            }

            if (resolve_pid) {
                lookup_inode(inode, &e->pid, e->comm, sizeof(e->comm));
            } else {
                e->pid    = 0;
                e->comm[0] = '-'; e->comm[1] = '\0';
            }

            if (!head) head = e;
            else        tail->next = e;
            tail = e;
        }
        line = eol ? eol + 1 : NULL;
    }
    free(buf);
    return head;
}

static void free_net_list(net_entry_t *head)
{
    while (head) {
        net_entry_t *next = head->next;
        free(head);
        head = next;
    }
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

static void print_entry(const net_entry_t *e, int show_pid, int batch)
{
    /* UDP 的 state 欄位沒有意義，顯示 "-" */
    const char *state_str = (strncmp(e->proto, "udp", 3) == 0)
                            ? "-"
                            : diag_get_tcp_state(e->state);
    const char *user = uid2uname(e->uid);
    const char *eol  = batch ? "" : DIAG_CLR_EOL;
    char pid_prog[36];

    if (show_pid) {
        if (e->pid > 0)
            snprintf(pid_prog, sizeof(pid_prog), "%d/%s", (int)e->pid, e->comm);
        else
            safe_strncpy(pid_prog, "-", sizeof(pid_prog));

        printf("%-6s %-14s %-42s %-42s %-16s %s%s\n",
               e->proto, state_str,
               e->local, e->remote,
               pid_prog, user, eol);
    } else {
        printf("%-6s %-14s %-42s %-42s %s%s\n",
               e->proto, state_str,
               e->local, e->remote,
               user, eol);
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

    if (show_pid) build_inode_map();

    for (size_t i = 0; i < ARRAY_SIZE(g_sources); i++) {
        if (g_sources[i].is_udp  && !show_udp)  continue;
        if (!g_sources[i].is_udp && !show_tcp)  continue;

        net_entry_t *part = parse_net_file(g_sources[i].path,
                                           g_sources[i].proto,
                                           g_sources[i].is_ipv6,
                                           show_pid);
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
    char    *opt_s = NULL;  /* -s STATE  */
    char    *opt_w = NULL;  /* -w SEC    */
    unsigned opts;

    /*
     * getopt32 選項字串：t u a l n b s: w:
     * 對應 bit 位置：
     *   bit 0 = t, bit 1 = u, bit 2 = a, bit 3 = l,
     *   bit 4 = n, bit 5 = b, bit 6 = s（附參數）, bit 7 = w（附參數）
     */
    opts = getopt32(argv, "tualnbs:w:", &opt_s, &opt_w);

    int show_tcp    =  1;                   /* 預設顯示 TCP */
    int show_udp    = (opts & (1 << 1));    /* -u */
    int show_all    = (opts & (1 << 2));    /* -a */
    int listen_only = (opts & (1 << 3));    /* -l */
    /* -n：位址已為 numeric 格式（/proc/net/tcp 本身不做 DNS），保留供未來擴充 */
    int batch       = (opts & (1 << 5));    /* -b */
    int filter_state= (opts & (1 << 6));    /* -s */
    int has_watch   = (opts & (1 << 7));    /* -w */

    if (show_all) { show_tcp = 1; show_udp = 1; }
    if (!show_udp) show_tcp = 1;    /* 至少顯示 TCP */

    /*
     * PID 解析需要讀取 /proc/<pid>/fd/ 的符號連結，
     * 一般使用者只能讀自己的行程，root 可讀全部。
     * 無論如何嘗試，無權限的 fd 會被 readlink 靜默忽略。
     */
    int show_pid = 1;

    int interval = 2;
    if (has_watch && opt_w) {
        interval = atoi(opt_w);
        if (interval < 1) interval = 1;
    }

    if (has_watch && !batch) {
        do_watch(show_tcp, show_udp, listen_only,
                 filter_state, opt_s ? opt_s : "",
                 show_pid, interval);
    } else {
        /* batch 模式或輸出不是 tty 時，強制純文字輸出 */
        int is_batch = batch || !isatty(STDOUT_FILENO);
        do_scan(show_tcp, show_udp, listen_only,
                filter_state, opt_s ? opt_s : "",
                show_pid, is_batch);
    }

    return EXIT_SUCCESS;
}
