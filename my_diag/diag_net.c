/* vi: set sw=4 ts=4: */
// clang-format on
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

#include "libdiag.h"
#include <netinet/in.h>
#include <arpa/inet.h>

/* ═══════════════════════════════════════════════════════════════
 * 資料結構
 * ═══════════════════════════════════════════════════════════════ */

/* 解析後的單一 socket 記錄 */
typedef struct net_entry {
	/* --- 改存 raw binary，延遲格式化 --- */
	union {
		uint32_t laddr4;       /* IPv4 (host byte order) */
		uint32_t laddr6[4];    /* IPv6 (已 htonl) */
	};
	union {
		uint32_t raddr4;
		uint32_t raddr6[4];
	};
	uint16_t lport, rport;
	uint8_t is_ipv6;
	uint8_t is_udp;        /* 替代原有的 char proto[8] */
	/* ---------------------------------- */
	int state;
	unsigned uid;
	unsigned long inode;
	pid_t pid;
	char comm[16];         /* 縮減至 16 bytes，對齊 Linux 核心 comm 長度 */
} net_entry_t;

typedef struct {
	bool show_tcp;
	bool show_udp;
	bool listen_only;
	bool filter_state;
	int target_state;
	bool show_pid;
	bool numeric;
	bool batch_mode;
	int interval;
} net_ctx_t;

static net_ctx_t G;

/* ── 動態陣列：消除硬限制並提供快取友好佈局 ── */
static net_entry_t *g_entries = NULL;
static int g_entries_cnt = 0;
static int g_entries_cap = 0;

static net_entry_t *get_next_entry(void)
{
	if (g_entries_cnt >= g_entries_cap) {
		g_entries_cap = g_entries_cap ? g_entries_cap * 2 : 1024;
		g_entries = xrealloc(g_entries, g_entries_cap * sizeof(*g_entries));
	}
	net_entry_t *e = &g_entries[g_entries_cnt];
	memset(e, 0, sizeof(*e));
	return e;
}

static void commit_entry(void)
{
	g_entries_cnt++;
}

/* 快速 IPv4 格式化：寫入 "a.b.c.d:port"，回傳長度 */
static void fast_format_ipv4(char *p, uint32_t addr, uint16_t port)
{
	unsigned char *b = (unsigned char *) &addr;
	sprintf(p, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
}

/* 統計用 */
typedef struct {
	int counts[DIAG_TCP_STATES_MAX + 1]; /* index = state code (1~11) */
	int tcp_total;
	int udp_total;
} net_stats_t;

/* ═══════════════════════════════════════════════════════════════
 * 位址解析：/proc/net/tcp[6] 的 hex 格式 → 可讀字串
 * ═══════════════════════════════════════════════════════════════ */

static inline char *skip_token(char *p)
{
	while (*p && *p != ' ' && *p != '\n')
		p++;
	while (*p == ' ')
		p++;
	return p;
}

static uint32_t parse_hex8(const char *p)
{
	uint32_t v = 0;
	int i;
	for (i = 0; i < 8; i++) {
		unsigned char c = (unsigned char) p[i];
		v = (v << 4) | (isdigit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
	}
	return v;
}

static int parse_proc_line_raw(const char *line, net_entry_t *e)
{
	char *p = (char *) line;

	while (*p == ' ')
		p++;
	while (isdigit((unsigned char) *p))
		p++;
	if (*p != ':')
		return 0;
	p++;
	while (*p == ' ')
		p++;

	if (e->is_ipv6) {
		e->laddr6[0] = parse_hex8(p);
		p += 8;
		e->laddr6[1] = parse_hex8(p);
		p += 8;
		e->laddr6[2] = parse_hex8(p);
		p += 8;
		e->laddr6[3] = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		e->lport = (uint16_t) strtoul(p, &p, 16);
		while (*p == ' ')
			p++;

		e->raddr6[0] = parse_hex8(p);
		p += 8;
		e->raddr6[1] = parse_hex8(p);
		p += 8;
		e->raddr6[2] = parse_hex8(p);
		p += 8;
		e->raddr6[3] = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		e->rport = (uint16_t) strtoul(p, &p, 16);
	} else {
		e->laddr4 = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		e->lport = (uint16_t) strtoul(p, &p, 16);
		while (*p == ' ')
			p++;

		e->raddr4 = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		e->rport = (uint16_t) strtoul(p, &p, 16);
	}
	while (*p == ' ')
		p++;

	e->state = (int) strtoul(p, &p, 16);
	while (*p == ' ')
		p++;

	p = skip_token(p); /* 跳過 tx_queue:rx_queue */
	p = skip_token(p); /* 跳過 tr:tm_when */
	p = skip_token(p); /* 跳過 retrnsmt */

	e->uid = (unsigned) strtoul(p, &p, 10);
	while (*p == ' ')
		p++;
	p = skip_token(p); /* 跳過 timeout */

	e->inode = strtoul(p, &p, 10);
	return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * inode → PID 對照表（掃描 /proc/<pid>/fd/）
 * ═══════════════════════════════════════════════════════════════ */

static int cmp_by_inode(const void *a, const void *b)
{
	const net_entry_t *ea = *(const net_entry_t **) a;
	const net_entry_t *eb = *(const net_entry_t **) b;
	return DIAG_CMP(ea->inode, eb->inode);
}

static void resolve_pids(void)
{
	if (g_entries_cnt == 0)
		return;

	/* 建立以 inode 排序的指標陣列，取代暴力的全系統 mapping */
	net_entry_t **by_inode = xmalloc(g_entries_cnt * sizeof(*by_inode));
	for (int i = 0; i < g_entries_cnt; i++)
		by_inode[i] = &g_entries[i];

	qsort(by_inode, g_entries_cnt, sizeof(*by_inode), cmp_by_inode);

	procps_status_t *proc = NULL;
	struct dirent *entry;
	DIR *d_fd;
	char name[64];
	unsigned baseofs;
	int unmapped = g_entries_cnt;

	while ((proc = procps_scan(proc, PSSCAN_PID | PSSCAN_COMM)) != NULL) {
		/* 如果所有 Socket 皆已映射完成，略過耗時的 I/O，僅空轉耗盡 procps_scan 以防 Memory Leak */
		if (unmapped == 0)
			continue;

		baseofs = sprintf(name, "/proc/%u/fd/", proc->pid);
		d_fd = opendir(name);
		if (d_fd) {
			while ((entry = readdir(d_fd)) != NULL) {
				char linkbuf[64];
				ssize_t len;
				unsigned long inode;

				if (!isdigit((unsigned char) entry->d_name[0]))
					continue;

				safe_strncpy(name + baseofs, entry->d_name, sizeof(name) - baseofs);
				/* 直接使用 stack buffer 進行單次 readlink，省去 xmalloc_readlink 的多次 syscall 與記憶體配置 */
				len = readlink(name, linkbuf, sizeof(linkbuf) - 1);
				if (len > 0) {
					linkbuf[len] = '\0';
					/* 拋棄高耗能的 sscanf，改用 strncmp + strtoul 輕量解析 */
					if (strncmp(linkbuf, "socket:[", 8) == 0) {
						inode = strtoul(linkbuf + 8, NULL, 10);

						/* 在我們真正關心的連線中進行二元搜尋 */
						int lo = 0, hi = g_entries_cnt - 1;
						while (lo <= hi) {
							int mid = (lo + hi) / 2;
							if (by_inode[mid]->inode == inode) {
								/* 防止 fork 共用 FD 導致重複扣減 */
								if (by_inode[mid]->pid == 0) {
									by_inode[mid]->pid = proc->pid;
									safe_strncpy(by_inode[mid]->comm,
												 proc->comm,
												 sizeof(by_inode[mid]->comm));
									unmapped--;
								}
								break;
							}
							if (by_inode[mid]->inode < inode)
								lo = mid + 1;
							else
								hi = mid - 1;
						}
					}
				}
			}
			closedir(d_fd);
		}
	}
	free(by_inode);
}

/* ═══════════════════════════════════════════════════════════════
 * 解析 /proc/net/{tcp,tcp6,udp,udp6}
 * ═══════════════════════════════════════════════════════════════ */

static void parse_net_file(const char *path, int is_udp, int is_ipv6)
{
	FILE *fp = fopen_for_read(path);
	if (!fp)
		return;

	/* 提供 64KB 的讀取緩衝區，將原本 4KB 觸發一次的 read syscall 大幅降至每 64KB 觸發一次 */
	char *io_buf = xmalloc(65536);
	setvbuf(fp, io_buf, _IOFBF, 65536);

	char line[512];
	/* 第一行為表頭，直接跳過 */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		free(io_buf);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		net_entry_t *e = get_next_entry();
		e->is_ipv6 = (uint8_t) is_ipv6;
		e->is_udp = (uint8_t) is_udp;

		if (!parse_proc_line_raw(line, e))
			continue;

		if (G.listen_only) {
			if (is_udp) {
				int remote_is_zero = (e->rport == 0) && 
					(is_ipv6 ? (e->raddr6[0] == 0 && e->raddr6[1] == 0 && e->raddr6[2] == 0 && e->raddr6[3] == 0) : (e->raddr4 == 0));
				if (!remote_is_zero)
					continue;
			} else if (e->state != 10) {
				continue;
			}
		}
		if (G.filter_state && e->state != G.target_state)
			continue;

		e->pid = 0;
		e->comm[0] = '-';
		commit_entry();
	}
	fclose(fp);
	free(io_buf);
}

/* ═══════════════════════════════════════════════════════════════
 * 統計與異常偵測
 * ═══════════════════════════════════════════════════════════════ */

static void count_states(net_stats_t *st)
{
	memset(st, 0, sizeof(*st));
	for (int i = 0; i < g_entries_cnt; i++) {
		net_entry_t *e = &g_entries[i];
		if (e->is_udp) {
			st->udp_total++;
		} else {
			st->tcp_total++;
			if (e->state >= 1 && e->state <= DIAG_TCP_STATES_MAX)
				st->counts[e->state]++;
		}
	}
}

/* ═══════════════════════════════════════════════════════════════
 * 格式化輸出
 * ═══════════════════════════════════════════════════════════════ */

static void print_header(void)
{
	const char *eol = DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL);

	if (G.show_pid) {
		printf("%-6s %-14s %-42s %-42s %-16s %s%s\n",
			   "Proto",
			   "State",
			   "Local Address",
			   "Foreign Address",
			   "PID/Program",
			   "User",
			   eol);
	} else {
		printf("%-6s %-14s %-42s %-42s %s%s\n",
			   "Proto",
			   "State",
			   "Local Address",
			   "Foreign Address",
			   "User",
			   eol);
	}
	if (!G.batch_mode)
		printf("%.128s%s\n",
			   "----------------------------------------------"
			   "----------------------------------------------"
			   "----------------------------------------------",
			   DIAG_CLR_EOL);
}

static void
format_ipv6(const uint32_t addr6[4], uint16_t port, char *out, size_t outlen)
{
	struct in6_addr in6;
	char ip[INET6_ADDRSTRLEN];
	memcpy(&in6, addr6, sizeof(in6));
	inet_ntop(AF_INET6, &in6, ip, sizeof(ip));
	snprintf(out, outlen, "[%s]:%u", ip, (unsigned) port);
}

static void print_entry(const net_entry_t *e)
{
	char local[64], remote[64];
	const char *eol = DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL);

	if (e->is_ipv6) {
		format_ipv6(e->laddr6, e->lport, local, sizeof(local));
		format_ipv6(e->raddr6, e->rport, remote, sizeof(remote));
	} else {
		fast_format_ipv4(local, e->laddr4, e->lport);
		fast_format_ipv4(remote, e->raddr4, e->rport);
	}

	const char *proto_str =
		e->is_udp ? (e->is_ipv6 ? "udp6" : "udp") : (e->is_ipv6 ? "tcp6" : "tcp");
	const char *state_str = e->is_udp ? "-" : diag_get_tcp_state(e->state);
	if (!state_str)
		state_str = "UNKNOWN";
	const char *user;
	char uid_buf[16];

	if (G.numeric) {
		snprintf(uid_buf, sizeof(uid_buf), "%u", e->uid);
		user = uid_buf;
	} else {
		user = get_cached_username(e->uid);
	}

	if (G.show_pid) {
		char pid_comm[32];
		if (e->pid > 0) {
			snprintf(pid_comm,
					 sizeof(pid_comm),
					 "%u/%s",
					 (unsigned) e->pid,
					 e->comm);
		} else {
			strcpy(pid_comm, "-");
		}
		printf("%-6s %-14s %-42s %-42s %-16s %s%s\n",
			   proto_str, state_str, local, remote, pid_comm, user, eol);
	} else {
		printf("%-6s %-14s %-42s %-42s %s%s\n",
			   proto_str, state_str, local, remote, user, eol);
	}
}

/* TCP 狀態分布摘要 + 異常警告 */
static void print_summary(const net_stats_t *st)
{
	const char *eol = DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL);

	printf("\n%sTCP state summary%s (total=%d)%s\n",
		   DIAG_ANSI(G.batch_mode, DIAG_CYAN),
		   DIAG_ANSI(G.batch_mode, DIAG_RESET),
		   st->tcp_total,
		   eol);
	for (int i = 1; i <= DIAG_TCP_STATES_MAX; i++) {
		if (st->counts[i] > 0)
			printf("  %-15s %d%s\n", diag_get_tcp_state(i), st->counts[i], eol);
	}
	if (st->udp_total > 0)
		printf("UDP total        %d%s\n", st->udp_total, eol);

	/* 異常偵測與警告輸出 */
	int anomalies = (st->counts[6] > 500) + (st->counts[8] > 20) +
					(st->counts[3] > 100) + (st->counts[9] > 50) +
					(st->counts[5] > 100) + (st->counts[1] > 10000);

	if (anomalies > 0) {
		printf("\n%s[ANOMALY DETECTED]%s%s\n",
			   DIAG_ANSI(G.batch_mode, DIAG_RED),
			   DIAG_ANSI(G.batch_mode, DIAG_RESET),
			   eol);

#define PRINT_WARN(fmt, ...) \
		printf("  %s! " fmt "%s%s\n", \
			   DIAG_ANSI(G.batch_mode, DIAG_YELLOW), \
			   ##__VA_ARGS__, \
			   DIAG_ANSI(G.batch_mode, DIAG_RESET), \
			   eol)

		if (st->counts[6] > 500)
			PRINT_WARN("TIME_WAIT=%d (>500): high churn rate or net.ipv4.tcp_tw_reuse not enabled", st->counts[6]);
		if (st->counts[8] > 20)
			PRINT_WARN("CLOSE_WAIT=%d (>20): possible connection leak (app not calling close())", st->counts[8]);
		if (st->counts[3] > 100)
			PRINT_WARN("SYN_RECV=%d (>100): possible SYN flood attack", st->counts[3]);
		if (st->counts[9] > 50)
			PRINT_WARN("LAST_ACK=%d (>50): peer not responding to FIN (network issue or remote crash)", st->counts[9]);
		if (st->counts[5] > 100)
			PRINT_WARN("FIN_WAIT2=%d (>100): many half-open connections (check net.ipv4.tcp_fin_timeout)", st->counts[5]);
		if (st->counts[1] > 10000)
			PRINT_WARN("ESTABLISHED=%d (>10000): unusually high connection count", st->counts[1]);
#undef PRINT_WARN
	}
}

/* ═══════════════════════════════════════════════════════════════
 * 核心：單次掃描與顯示
 * ═══════════════════════════════════════════════════════════════ */

/* procfs 資料來源表 */
static const struct {
	const char *path;
	int is_ipv6;
	int is_udp;
} g_sources[] = {
	{"/proc/net/tcp", 0, 0},
	{"/proc/net/tcp6", 1, 0},
	{"/proc/net/udp", 0, 1},
	{"/proc/net/udp6", 1, 1},
};

static void do_scan(void)
{
	g_entries_cnt = 0;

	for (size_t i = 0; i < ARRAY_SIZE(g_sources); i++) {
		if (g_sources[i].is_udp && !G.show_udp)
			continue;
		if (!g_sources[i].is_udp && !G.show_tcp)
			continue;

		parse_net_file(g_sources[i].path,
					   g_sources[i].is_udp,
					   g_sources[i].is_ipv6);
	}

	if (G.show_pid)
		resolve_pids();

	net_stats_t st;
	count_states(&st);

	print_header();

	int printed = 0;
	for (int i = 0; i < g_entries_cnt; i++) {
		print_entry(&g_entries[i]);
		printed++;
	}

	if (printed == 0)
		printf("(no matching connections)%s\n", DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL));

	if (G.show_tcp)
		print_summary(&st);

	fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
 * Watch 模式（-w，互動式自動更新）
 * ═══════════════════════════════════════════════════════════════ */

static void do_watch(void)
{
	char key;

	if (!G.batch_mode) {
		if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
			bb_error_msg_and_die("-w requires a terminal; "
								 "use -b for non-interactive output");

		diag_tui_init();
	}

	while (1) {
		if (!G.batch_mode) {
			int quit = 0;
			int res;
			while ((res = diag_ui_read_key(&key)) > 0) {
				if (key == 'Q')
					quit = 1;
			}
			if (res < 0 || quit)
				break;

			/* 清屏並印表頭 */
			printf(DIAG_CLEAR);
			printf(DIAG_CYAN "[MY_NET]" DIAG_RESET " Refresh: %ds"
							 "  TCP:%s UDP:%s Listen-only:%s"
							 "  Press Q to quit" DIAG_CLR_EOL "\n",
				   G.interval,
				   G.show_tcp ? "on" : "off",
				   G.show_udp ? "on" : "off",
				   G.listen_only ? "on" : "off");
			printf("%.128s" DIAG_CLR_EOL "\n",
				   "------------------------------------------------------"
				   "------------------------------------------------------"
				   "------------------------------------------------------");
		}

		do_scan();

		fflush(stdout);
		diag_delay(G.interval * 1000, G.batch_mode);
	}

	if (!G.batch_mode) {
		diag_tui_restore();
		printf("\n");
		fflush(stdout);
	}
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */

enum {
	OPT_t = (1 << 0),
	OPT_u = (1 << 1),
	OPT_a = (1 << 2),
	OPT_l = (1 << 3),
	OPT_n = (1 << 4),
	OPT_p = (1 << 5),
	OPT_b = (1 << 6),
	OPT_s = (1 << 7),
	OPT_w = (1 << 8),
};

int my_net_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_net_main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOFBF, 65536);
	char *opt_s = NULL;
	int interval = 2;

	unsigned opts = getopt32(argv, "tualnpbs:w:+", &opt_s, &interval);

	G.show_tcp = (opts & OPT_t) || (opts & OPT_a) || (!(opts & OPT_t) && !(opts & OPT_u));
	G.show_udp = (opts & OPT_u) || (opts & OPT_a);
	G.listen_only = (opts & OPT_l);
	G.numeric = (opts & OPT_n);
	G.show_pid = (opts & OPT_p);
	G.batch_mode = (opts & OPT_b) || !isatty(STDOUT_FILENO);
	G.filter_state = (opts & OPT_s);
	G.interval = (interval < 1) ? 1 : interval;

	/* 提早解析狀態字串，避免 Watch 模式下每秒重複解析 */
	if (G.filter_state && opt_s && *opt_s) {
		G.target_state = -1;
		for (int i = 1; i <= DIAG_TCP_STATES_MAX; i++) {
			if (strcasecmp(diag_get_tcp_state(i), opt_s) == 0) {
				G.target_state = i;
				break;
			}
		}
	}

	if (opts & OPT_w)
		do_watch();
	else
		do_scan();

	return EXIT_SUCCESS;
}