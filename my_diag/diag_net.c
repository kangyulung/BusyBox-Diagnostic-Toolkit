/* vi: set sw=4 ts=4: */
// clang-format on
//config:config MY_NET
//config:   bool "my_net (Network Connection Monitor)"
//config:   default y
//config:   help
//config:     Network connection state monitor: TCP/UDP socket listing,
//config:     TCP state machine tracking, connection anomaly detection.
//config:     Reads /proc/net/tcp[6] and /proc/net/udp[6].

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
//usage:     "\n	-n		Numeric output (show UID instead of username)"
//usage:     "\n	-s STATE	Filter by TCP state (ESTABLISHED, TIME_WAIT, LISTEN, ...)"
//usage:     "\n	-w SEC		Watch mode: auto-refresh every SEC seconds (Q to quit)"
//usage:     "\n	-b		Batch mode (plain text output, suitable for scripts)"
//usage:     "\n	-p		Show PID/program (requires root for all processes)"
// clang-format on

#include "libdiag.h"
#include <netinet/in.h>
#include <arpa/inet.h>

/* ═══════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════ */

/* Parsed single socket record */
typedef struct net_entry {
	/* --- Store raw binary, format lazily --- */
	union {
		uint32_t laddr4;	/* IPv4 (network byte order) */
		uint32_t laddr6[4]; /* IPv6 (network byte order) */
	};
	union {
		uint32_t raddr4;
		uint32_t raddr6[4];
	};
	uint16_t lport, rport;
	uint8_t is_ipv6;
	uint8_t is_udp; /* Replaces original char proto[8] */
	/* ---------------------------------- */
	int state;
	unsigned uid;
	unsigned long inode;
	pid_t pid;
	char comm
		[16]; /* Reduced to 16 bytes, aligns with Linux kernel comm length */
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

/* -- Dynamic array: removes hard limits and provides cache-friendly layout -- */
static net_entry_t *g_entries = NULL;
static int g_entries_cnt = 0;
static int g_entries_cap = 0;

/*
 * Retrieves the next available net_entry_t from the dynamic array,
 * expanding the array capacity if necessary.
 */
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

/* Confirms the addition of the recently fetched entry to the array */
static void commit_entry(void)
{
	g_entries_cnt++;
}

/* Fast IPv4 formatting: write "a.b.c.d:port" */
static void fast_format_ipv4(char *p, uint32_t addr, uint16_t port)
{
	unsigned char *b = (unsigned char *) &addr;
	sprintf(p, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
}

/* For statistics */
typedef struct {
	int counts[DIAG_TCP_STATES_MAX + 1]; /* index = state code (1~11) */
	int tcp_total;
	int udp_total;
} net_stats_t;

/* ═══════════════════════════════════════════════════════════════
 * Address Parsing: /proc/net/tcp[6] hex format -> readable string
 * ═══════════════════════════════════════════════════════════════ */

/* Advances the string pointer past the current token */
static inline char *skip_token(char *p)
{
	while (*p && *p != ' ' && *p != '\n')
		p++;
	while (*p == ' ')
		p++;
	return p;
}

/* Parses an 8-character hexadecimal string into a 32-bit unsigned integer */
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

/*
 * Parses a single raw line from /proc/net/tcp or udp into a net_entry_t.
 * Returns 1 on success, 0 on failure.
 */
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

	p = skip_token(p); /* Skip tx_queue:rx_queue */
	p = skip_token(p); /* Skip tr:tm_when */
	p = skip_token(p); /* Skip retrnsmt */

	e->uid = (unsigned) strtoul(p, &p, 10);
	while (*p == ' ')
		p++;
	p = skip_token(p); /* Skip timeout */

	e->inode = strtoul(p, &p, 10);
	return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * inode -> PID Mapping (scan /proc/<pid>/fd/)
 * ═══════════════════════════════════════════════════════════════ */

/* Comparison function to sort network entries by their inode number */
static int cmp_by_inode(const void *a, const void *b)
{
	const net_entry_t *ea = *(const net_entry_t **) a;
	const net_entry_t *eb = *(const net_entry_t **) b;
	return DIAG_CMP(ea->inode, eb->inode);
}

/*
 * Resolves process IDs (PIDs) for network entries by scanning
 * file descriptors in /proc/<pid>/fd/ and matching socket inodes.
 */
static void resolve_pids(void)
{
	if (g_entries_cnt == 0)
		return;

	/* Create an array of pointers sorted by inode, replacing brute-force system-wide mapping */
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
		/* If all sockets are mapped, skip expensive I/O and just exhaust procps_scan to prevent memory leaks */
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

				safe_strncpy(
					name + baseofs, entry->d_name, sizeof(name) - baseofs);
				/* Use stack buffer for a single readlink, saving multiple syscalls and memory allocations of xmalloc_readlink */
				len = readlink(name, linkbuf, sizeof(linkbuf) - 1);
				if (len > 0) {
					linkbuf[len] = '\0';
					/* Discard expensive sscanf, use lightweight parsing with strncmp + strtoul */
					if (strncmp(linkbuf, "socket:[", 8) == 0) {
						inode = strtoul(linkbuf + 8, NULL, 10);

						/* Binary search among the connections we actually care about */
						int lo = 0, hi = g_entries_cnt - 1;
						while (lo <= hi) {
							int mid = (lo + hi) / 2;
							if (by_inode[mid]->inode == inode) {
								/* Prevent double deduction caused by shared FDs from fork */
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
 * Parse /proc/net/{tcp,tcp6,udp,udp6}
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Opens and parses the specified /proc/net/ file, applying filters
 * and populating the network entries array.
 */
static void parse_net_file(const char *path, int is_udp, int is_ipv6)
{
	FILE *fp = fopen_for_read(path);
	if (!fp)
		return;

	/* Provide a 64KB read buffer, significantly reducing read syscalls from once per 4KB to once per 64KB */
	char *io_buf = xmalloc(65536);
	setvbuf(fp, io_buf, _IOFBF, 65536);

	char line[512];
	/* First line is header, skip it directly */
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
				int remote_is_zero =
					(e->rport == 0) &&
					(is_ipv6 ? (e->raddr6[0] == 0 && e->raddr6[1] == 0 &&
								e->raddr6[2] == 0 && e->raddr6[3] == 0)
							 : (e->raddr4 == 0));
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
 * Statistics and Anomaly Detection
 * ═══════════════════════════════════════════════════════════════ */

/* Computes statistics for TCP states and total counts */
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
 * Formatted Output
 * ═══════════════════════════════════════════════════════════════ */

/* Prints the table header for the network connection list */
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

/* Formats an IPv6 address and port into a human-readable string */
static void
format_ipv6(const uint32_t addr6[4], uint16_t port, char *out, size_t outlen)
{
	struct in6_addr in6;
	char ip[INET6_ADDRSTRLEN];
	memcpy(&in6, addr6, sizeof(in6));
	inet_ntop(AF_INET6, &in6, ip, sizeof(ip));
	snprintf(out, outlen, "[%s]:%u", ip, (unsigned) port);
}

/* Prints a single formatted network entry */
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

	const char *proto_str = e->is_udp ? (e->is_ipv6 ? "udp6" : "udp")
									  : (e->is_ipv6 ? "tcp6" : "tcp");
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
			   proto_str,
			   state_str,
			   local,
			   remote,
			   pid_comm,
			   user,
			   eol);
	} else {
		printf("%-6s %-14s %-42s %-42s %s%s\n",
			   proto_str,
			   state_str,
			   local,
			   remote,
			   user,
			   eol);
	}
}

/* TCP state distribution summary + anomaly warnings */
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

	/* Anomaly detection and warning output */
	int anomalies = (st->counts[6] > 500) + (st->counts[8] > 20) +
					(st->counts[3] > 100) + (st->counts[9] > 50) +
					(st->counts[5] > 100) + (st->counts[1] > 10000);

	if (anomalies > 0) {
		printf("\n%s[ANOMALY DETECTED]%s%s\n",
			   DIAG_ANSI(G.batch_mode, DIAG_RED),
			   DIAG_ANSI(G.batch_mode, DIAG_RESET),
			   eol);

#define PRINT_WARN(fmt, ...)                                                   \
	printf("  %s! " fmt "%s%s\n",                                              \
		   DIAG_ANSI(G.batch_mode, DIAG_YELLOW),                               \
		   ##__VA_ARGS__,                                                      \
		   DIAG_ANSI(G.batch_mode, DIAG_RESET),                                \
		   eol)

		if (st->counts[6] > 500)
			PRINT_WARN("TIME_WAIT=%d (>500): high churn rate or "
					   "net.ipv4.tcp_tw_reuse not enabled",
					   st->counts[6]);
		if (st->counts[8] > 20)
			PRINT_WARN("CLOSE_WAIT=%d (>20): possible connection leak (app not "
					   "calling close())",
					   st->counts[8]);
		if (st->counts[3] > 100)
			PRINT_WARN("SYN_RECV=%d (>100): possible SYN flood attack",
					   st->counts[3]);
		if (st->counts[9] > 50)
			PRINT_WARN("LAST_ACK=%d (>50): peer not responding to FIN (network "
					   "issue or remote crash)",
					   st->counts[9]);
		if (st->counts[5] > 100)
			PRINT_WARN("FIN_WAIT2=%d (>100): many half-open connections (check "
					   "net.ipv4.tcp_fin_timeout)",
					   st->counts[5]);
		if (st->counts[1] > 10000)
			PRINT_WARN(
				"ESTABLISHED=%d (>10000): unusually high connection count",
				st->counts[1]);
#undef PRINT_WARN
	}
}

/* ═══════════════════════════════════════════════════════════════
 * Core: Single Scan and Display
 * ═══════════════════════════════════════════════════════════════ */

/* procfs data sources table */
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

/*
 * Performs a single scan of the selected network files,
 * resolves PIDs if requested, and prints the gathered data and summary.
 */
static void do_scan(void)
{
	g_entries_cnt = 0;

	for (size_t i = 0; i < ARRAY_SIZE(g_sources); i++) {
		if (g_sources[i].is_udp && !G.show_udp)
			continue;
		if (!g_sources[i].is_udp && !G.show_tcp)
			continue;

		parse_net_file(
			g_sources[i].path, g_sources[i].is_udp, g_sources[i].is_ipv6);
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
		printf("(no matching connections)%s\n",
			   DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL));

	if (G.show_tcp)
		print_summary(&st);

	fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
 * Watch Mode (-w, interactive auto-refresh)
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

			/* Clear screen and print header */
			printf(DIAG_CLR_SCR);
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
		if (!G.batch_mode)
			printf(DIAG_CLR_DOWN);

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

/*
 * Main entry point for the my_net applet.
 * Parses arguments and dispatches to either single scan or watch mode.
 */
int my_net_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_net_main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOFBF, 65536);
	char *opt_s = NULL;
	int interval = 2;

	unsigned opts = getopt32(argv, "tualnpbs:w:+", &opt_s, &interval);

	G.show_tcp = (opts & OPT_t) || (opts & OPT_a) ||
				 (!(opts & OPT_t) && !(opts & OPT_u));
	G.show_udp = (opts & OPT_u) || (opts & OPT_a);
	G.listen_only = (opts & OPT_l);
	G.numeric = (opts & OPT_n);
	G.show_pid = (opts & OPT_p);
	G.batch_mode = (opts & OPT_b) || !isatty(STDOUT_FILENO);
	G.filter_state = (opts & OPT_s);
	G.interval = (interval < 1) ? 1 : interval;

	/* Parse state string early to avoid re-parsing every second in Watch mode */
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