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

/* ── 動態陣列：消除硬限制並提供快取友好佈局 ── */
static net_entry_t *g_entries = NULL;
static int g_entries_cnt = 0;
static int g_entries_cap = 0;

static net_entry_t *alloc_entry(void)
{
	if (g_entries_cnt >= g_entries_cap) {
		g_entries_cap = g_entries_cap ? g_entries_cap * 2 : 1024;
		g_entries = xrealloc(g_entries, g_entries_cap * sizeof(*g_entries));
	}
	net_entry_t *e = &g_entries[g_entries_cnt++];
	memset(e, 0, sizeof(*e));
	return e;
}

/* 快速 IPv4 格式化：寫入 "a.b.c.d:port"，回傳長度 */
static int fast_format_ipv4(char *p, uint32_t addr, uint16_t port)
{
	unsigned char *b = (unsigned char *) &addr;
	char *s = p;
	s = utoa_to_buf(b[0], s, 4);
	*s++ = '.';
	s = utoa_to_buf(b[1], s, 4);
	*s++ = '.';
	s = utoa_to_buf(b[2], s, 4);
	*s++ = '.';
	s = utoa_to_buf(b[3], s, 4);
	*s++ = ':';
	s = utoa_to_buf(port, s, 6);
	*s = '\0';
	return (int) (s - p);
}

/* 統計用 */
#define N_TCP_STATES 12
typedef struct {
	int counts[N_TCP_STATES + 1]; /* index = state code (1~11) */
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

static int parse_proc_line_raw(const char *line,
							   int is_ipv6,
							   uint32_t *laddr4,
							   uint16_t *lport,
							   uint32_t *raddr4,
							   uint16_t *rport,
							   uint32_t laddr6[4],
							   uint32_t raddr6[4],
							   int *state_out,
							   unsigned *uid_out,
							   unsigned long *inode_out)
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

	if (is_ipv6) {
		laddr6[0] = parse_hex8(p);
		p += 8;
		laddr6[1] = parse_hex8(p);
		p += 8;
		laddr6[2] = parse_hex8(p);
		p += 8;
		laddr6[3] = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		*lport = (uint16_t) strtoul(p, &p, 16);
		while (*p == ' ')
			p++;

		raddr6[0] = parse_hex8(p);
		p += 8;
		raddr6[1] = parse_hex8(p);
		p += 8;
		raddr6[2] = parse_hex8(p);
		p += 8;
		raddr6[3] = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		*rport = (uint16_t) strtoul(p, &p, 16);
		*laddr4 = 0;
		*raddr4 = 0;
	} else {
		*laddr4 = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		*lport = (uint16_t) strtoul(p, &p, 16);
		while (*p == ' ')
			p++;

		*raddr4 = parse_hex8(p);
		p += 8;
		if (*p != ':')
			return 0;
		p++;
		*rport = (uint16_t) strtoul(p, &p, 16);
	}
	while (*p == ' ')
		p++;

	*state_out = (int) strtoul(p, &p, 16);
	while (*p == ' ')
		p++;

	p = skip_token(p); /* 跳過 tx_queue:rx_queue */
	p = skip_token(p); /* 跳過 tr:tm_when */
	p = skip_token(p); /* 跳過 retrnsmt */

	*uid_out = (unsigned) strtoul(p, &p, 10);
	while (*p == ' ')
		p++;
	p = skip_token(p); /* 跳過 timeout */

	*inode_out = strtoul(p, &p, 10);
	return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * inode → PID 對照表（掃描 /proc/<pid>/fd/）
 * ═══════════════════════════════════════════════════════════════ */

static int cmp_by_inode(const void *a, const void *b)
{
	const net_entry_t *ea = *(const net_entry_t **) a;
	const net_entry_t *eb = *(const net_entry_t **) b;
	if (ea->inode < eb->inode)
		return -1;
	if (ea->inode > eb->inode)
		return 1;
	return 0;
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
	char name[sizeof("/proc/%u/fd/0123456789") + sizeof(int) * 3];
	unsigned baseofs;

	while ((proc = procps_scan(proc, PSSCAN_PID | PSSCAN_COMM)) != NULL) {
		baseofs = sprintf(name, "/proc/%u/fd/", proc->pid);
		d_fd = opendir(name);
		if (d_fd) {
			while ((entry = readdir(d_fd)) != NULL) {
				char linkbuf[64];
				ssize_t len;
				unsigned long inode;

				if (!isdigit((unsigned char) entry->d_name[0]))
					continue;

				safe_strncpy(name + baseofs, entry->d_name, 10);
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
								by_inode[mid]->pid = proc->pid;
								safe_strncpy(by_inode[mid]->comm,
											 proc->comm,
											 sizeof(by_inode[mid]->comm));
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

static void parse_net_file(const char *path,
								   int is_udp,
								   int is_ipv6,
								   int listen_only,
								   int filter_state,
								   int target_state)
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
		int state_hex;
		unsigned uid;
		unsigned long inode;
		uint32_t la4 = 0, ra4 = 0, la6[4] = {0}, ra6[4] = {0};
		uint16_t lp = 0, rp = 0;

		if (!parse_proc_line_raw(line,
								 is_ipv6,
								 &la4,
								 &lp,
								 &ra4,
								 &rp,
								 la6,
								 ra6,
								 &state_hex,
								 &uid,
								 &inode))
			continue;

		if (listen_only && state_hex != 10)
			continue;
		if (filter_state && state_hex != target_state)
			continue;

		{
			net_entry_t *e = alloc_entry();
			e->laddr4 = la4;
			e->raddr4 = ra4;
			e->lport = lp;
			e->rport = rp;
			e->is_ipv6 = (uint8_t) is_ipv6;
			e->is_udp = (uint8_t) is_udp;
			if (is_ipv6) {
				memcpy(e->laddr6, la6, 16);
				memcpy(e->raddr6, ra6, 16);
			}
			e->state = state_hex;
			e->uid = uid;
			e->inode = inode;
			e->pid = 0;
			e->comm[0] = '-';
			e->comm[1] = '\0';
		}
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
			if (e->state >= 1 && e->state <= N_TCP_STATES)
				st->counts[e->state]++;
		}
	}
}

/* ═══════════════════════════════════════════════════════════════
 * 格式化輸出
 * ═══════════════════════════════════════════════════════════════ */

static void print_header(int show_pid, int batch)
{
	const char *eol = batch ? "" : DIAG_CLR_EOL;

	if (show_pid) {
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
	if (!batch)
		printf("%.128s%s\n",
			   "----------------------------------------------"
			   "----------------------------------------------"
			   "---------------------------------",
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

static void print_entry(const net_entry_t *e, int show_pid, int batch)
{
	char line[256];
	char *p = line;
	const char *state_str, *user;
	char local[64], remote[64];
	const char *eol = batch ? "" : DIAG_CLR_EOL;

	/* proto（最多 6 字元，補空白對齊） */
	const char *proto_str = e->is_udp ? (e->is_ipv6 ? "udp6" : "udp") : (e->is_ipv6 ? "tcp6" : "tcp");
	int plen = (int) strlen(proto_str);
	memcpy(p, proto_str, plen);
	p += plen;
	/* 補到 7 字元（6 + 1 空格） */
	while (plen++ < 7)
		*p++ = ' ';

	/* state（最多 14 字元） */
	state_str = e->is_udp ? "-" : diag_get_tcp_state(e->state);
	int slen = (int) strlen(state_str);
	memcpy(p, state_str, slen);
	p += slen;
	while (slen++ < 15)
		*p++ = ' ';

	/* local address（最多 42 字元） */
	int llen;
	if (e->is_ipv6) {
		format_ipv6(e->laddr6, e->lport, local, sizeof(local));
		llen = (int) strlen(local);
		memcpy(p, local, llen);
	} else {
		llen = fast_format_ipv4(p, e->laddr4, e->lport);
	}
	p += llen;
	while (llen++ < 43)
		*p++ = ' ';

	/* remote address（最多 42 字元） */
	int rlen;
	if (e->is_ipv6) {
		format_ipv6(e->raddr6, e->rport, remote, sizeof(remote));
		rlen = (int) strlen(remote);
		memcpy(p, remote, rlen);
	} else {
		rlen = fast_format_ipv4(p, e->raddr4, e->rport);
	}
	p += rlen;

	/* PID 欄（只在 show_pid 時輸出） */
	if (show_pid) {
		while (rlen++ < 43)
			*p++ = ' ';
		if (e->pid > 0) {
			p = utoa_to_buf((unsigned) e->pid, p, 10);
			*p++ = '/';
			int clen = (int) strlen(e->comm);
			memcpy(p, e->comm, clen);
			p += clen;
			int pid_prog_len = (int) (p - line) - (7 + 15 + 43 + 43);
			while (pid_prog_len++ < 17)
				*p++ = ' ';
		} else {
			*p++ = '-';
			int i = 1;
			while (i++ < 17)
				*p++ = ' ';
		}
	} else {
		while (rlen++ < 43)
			*p++ = ' ';
	}

	/* user */
	user = get_cached_username(e->uid);
	int ulen = (int) strlen(user);
	memcpy(p, user, ulen);
	p += ulen;

	/* eol + newline */
	if (*eol) {
		memcpy(p, eol, strlen(eol));
		p += strlen(eol);
	}
	*p++ = '\n';

	fwrite(line, 1, (size_t)(p - line), stdout);
}

/* TCP 狀態分布摘要 + 異常警告 */
static void print_summary(const net_stats_t *st, int batch)
{
	static const char *state_names[] = {NULL,
										"ESTABLISHED",
										"SYN_SENT",
										"SYN_RECV",
										"FIN_WAIT1",
										"FIN_WAIT2",
										"TIME_WAIT",
										"CLOSE",
										"CLOSE_WAIT",
										"LAST_ACK",
										"LISTEN",
										"CLOSING"};
	const char *eol = batch ? "" : DIAG_CLR_EOL;

	printf("\n%sTCP state summary%s (total=%d)%s%s\n",
		   batch ? "" : DIAG_CYAN,
		   batch ? "" : DIAG_RESET,
		   st->tcp_total,
		   eol,
		   batch ? "" : DIAG_CLR_EOL);
	for (int i = 1; i <= 11; i++) {
		if (st->counts[i] > 0)
			printf("  %-15s %d%s\n", state_names[i], st->counts[i], eol);
	}
	if (st->udp_total > 0)
		printf("UDP total        %d%s\n", st->udp_total, eol);

	/* 異常偵測與警告輸出 */
	int anomalies = (st->counts[6] > 500) + (st->counts[8] > 20) +
					(st->counts[3] > 100) + (st->counts[9] > 50) +
					(st->counts[5] > 100) + (st->counts[1] > 10000);

	if (anomalies > 0) {
		printf("\n%s[ANOMALY DETECTED]%s%s\n",
			   batch ? "" : DIAG_RED,
			   batch ? "" : DIAG_RESET,
			   eol);

#define PRINT_WARN(fmt, ...) \
		printf("  %s! " fmt "%s%s\n", \
			   batch ? "" : DIAG_YELLOW, \
			   ##__VA_ARGS__, \
			   batch ? "" : DIAG_RESET, \
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
	const char *proto;
	int is_ipv6;
	int is_udp;
} g_sources[] = {
	{"/proc/net/tcp", "tcp", 0, 0},
	{"/proc/net/tcp6", "tcp6", 1, 0},
	{"/proc/net/udp", "udp", 0, 1},
	{"/proc/net/udp6", "udp6", 1, 1},
};

static void do_scan(int show_tcp,
					int show_udp,
					int listen_only,
					int filter_state,
					const char *state_str,
					int show_pid,
					int batch)
{
	g_entries_cnt = 0;
	clear_username_cache();

	/* 把 state_str 轉成數字，供 parse_net_file 做早期過濾 */
	int target_state = -1;
	if (filter_state && state_str && *state_str) {
		static const char *names[] = {NULL,
									  "ESTABLISHED",
									  "SYN_SENT",
									  "SYN_RECV",
									  "FIN_WAIT1",
									  "FIN_WAIT2",
									  "TIME_WAIT",
									  "CLOSE",
									  "CLOSE_WAIT",
									  "LAST_ACK",
									  "LISTEN",
									  "CLOSING"};
		for (int i = 1; i <= 11; i++)
			if (strcasecmp(names[i], state_str) == 0) {
				target_state = i;
				break;
			}
	}

	for (size_t i = 0; i < ARRAY_SIZE(g_sources); i++) {
		if (g_sources[i].is_udp && !show_udp)
			continue;
		if (!g_sources[i].is_udp && !show_tcp)
			continue;

		parse_net_file(g_sources[i].path,
										   g_sources[i].is_udp,
										   g_sources[i].is_ipv6,
										   listen_only,
										   filter_state,
										   target_state);
	}

	if (show_pid)
		resolve_pids();

	net_stats_t st;
	count_states(&st);

	print_header(show_pid, batch);

	int printed = 0;
	for (int i = 0; i < g_entries_cnt; i++) {
		print_entry(&g_entries[i], show_pid, batch);
		printed++;
	}

	if (printed == 0)
		printf("(no matching connections)%s\n", batch ? "" : DIAG_CLR_EOL);

	if (show_tcp)
		print_summary(&st, batch);

	fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
 * Watch 模式（-w，互動式自動更新）
 * ═══════════════════════════════════════════════════════════════ */

static void do_watch(int show_tcp,
					 int show_udp,
					 int listen_only,
					 int filter_state,
					 const char *state_str,
					 int show_pid,
					 int interval_sec)
{
	struct termios old_t;
	struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
	char key;

	if (!isatty(STDOUT_FILENO))
		bb_error_msg_and_die("-w requires a terminal; "
							 "use -b for non-interactive output");

	diag_ui_mode_raw(&old_t);

	while (1) {
		/* 非阻塞讀取按鍵 */
		if (poll(&pfd, 1, 0) > 0 && read(STDIN_FILENO, &key, 1) > 0) {
			if (toupper((unsigned char) key) == 'Q')
				break;
		}

		/* 清屏並印表頭 */
		printf("\033[H\033[J");
		printf(DIAG_CYAN "[MY_NET]" DIAG_RESET " Refresh: %ds"
						 "  TCP:%s UDP:%s Listen-only:%s"
						 "  Press Q to quit" DIAG_CLR_EOL "\n",
			   interval_sec,
			   show_tcp ? "on" : "off",
			   show_udp ? "on" : "off",
			   listen_only ? "on" : "off");
		printf("%.90s" DIAG_CLR_EOL "\n",
			   "------------------------------------------------------"
			   "------------------------------------------------------");

		do_scan(show_tcp,
				show_udp,
				listen_only,
				filter_state,
				state_str,
				show_pid,
				0 /* batch=0 → TUI 模式 */);

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
	char *opt_s = NULL;
	char *opt_w = NULL;
	unsigned opts;
	int opt_t, opt_u, show_all;
	int show_tcp, show_udp;
	int listen_only, filter_state, has_watch, batch, show_pid;
	int interval, is_batch;

	/*
     * getopt32 選項字串：t u a l n p b s: w:
     * bit 0=t  1=u  2=a  3=l  4=n  5=p  6=b  7=s  8=w
     */
	opts = getopt32(argv, "tualnpbs:w:", &opt_s, &opt_w);

	opt_t = (opts & (1 << 0));
	opt_u = (opts & (1 << 1));
	show_all = (opts & (1 << 2));
	listen_only = (opts & (1 << 3));
	/* bit 4 = n（numeric，保留供未來擴充） */
	show_pid = (opts & (1 << 5));	  /* -p */
	batch = (opts & (1 << 6));		  /* -b */
	filter_state = (opts & (1 << 7)); /* -s */
	has_watch = (opts & (1 << 8));	  /* -w */

	if (show_all) {
		show_tcp = 1;
		show_udp = 1;
	} else if (opt_u && !opt_t) {
		show_tcp = 0;
		show_udp = 1;
	} else if (opt_t && opt_u) {
		show_tcp = 1;
		show_udp = 1;
	} else if (opt_t) {
		show_tcp = 1;
		show_udp = 0;
	} else {
		show_tcp = 1;
		show_udp = 0; /* 預設：僅 TCP */
	}

	interval = 2;
	if (has_watch && opt_w) {
		interval = atoi(opt_w);
		if (interval < 1)
			interval = 1;
	}

	if (has_watch && !batch) {
		do_watch(show_tcp,
				 show_udp,
				 listen_only,
				 filter_state,
				 opt_s ? opt_s : "",
				 show_pid,
				 interval);
	} else {
		is_batch = batch || !isatty(STDOUT_FILENO);
		do_scan(show_tcp,
				show_udp,
				listen_only,
				filter_state,
				opt_s ? opt_s : "",
				show_pid,
				is_batch);
	}

	return EXIT_SUCCESS;
}