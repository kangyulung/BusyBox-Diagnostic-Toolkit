/* vi: set sw=4 ts=4: */
// clang-format off
//config:config MY_PROC
//config: 	bool "my_proc (Resource Analyzer)"
//config: 	default y
//config:   select FEATURE_TOP_SMP_PROCESS
//config:   select FEATURE_PS_ADDITIONAL_COLUMNS
//config: 	help
//config: 	  Interactive process viewer and resource analyzer.
//config: 	  Supports real-time monitoring and hierarchical tree view.

//applet:IF_MY_PROC(APPLET(my_proc, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_PROC) += diag_proc.o
//kbuild:lib-$(CONFIG_MY_PROC) += libdiag.o


//usage:#define my_proc_trivial_usage "[-t] [-d <seconds>] [-n <count>] [-p <pid>] [-b]"
//usage:#define my_proc_full_usage "\n\n"
//usage:	  "Analyze process resources\n"
//usage:	"\n	   -t	Show process tree snapshot"
//usage:	"\n	   -d	Delay between updates"
//usage:	"\n	   -n	Number of iterations"
//usage:	"\n	   -p	Monitor specific PID"
//usage:	"\n	   -b	Batch mode (non-interactive)"
// clang-format on

#include "libdiag.h"

/*
 * Represents a single process node in the system.
 * Extends the basic diag_node_base_t for tree construction.
 */
typedef struct proc_node {
	diag_node_base_t base;
	procps_status_t *pinfo;
	unsigned
		cpu_pcnt_10; /* CPU usage percentage * 10 (for fixed-point formatting) */
	unsigned
		mem_pcnt_10; /* Memory usage percentage * 10 (for fixed-point formatting) */
	char time_str[16];
} proc_node_t;

/* View mode for the process list */
typedef enum { VIEW_TOP, VIEW_TREE } view_mode_t;

/*
 * Context structure storing the global state and configuration
 * for the process viewer.
 */
typedef struct {
	view_mode_t view_mode;
	char sort_mode;
	int iterations;
	double delay;
	int target_pid;
	bool batch_mode;
	int width, height;
} proc_ctx_t;

/* Global context initialized with default values */
static proc_ctx_t G = {VIEW_TOP, 'P', -1, 1.0, -1, false, 80, 24};

/* Updates the terminal width and height in the global context */
static void update_term_size(void)
{
	if (G.batch_mode) {
		G.width = 80;
		G.height = 10000;
		return;
	}
	get_terminal_width_height(STDOUT_FILENO, &G.width, &G.height);
}

/* Returns a human-readable label for the current sort mode */
static const char *get_sort_label(void)
{
	switch (G.sort_mode) {
	case 'P':
		return "CPU%";
	case 'M':
		return "RSS";
	case 'V':
		return "VSZ";
	case 'I':
		return "PID";
	case 'O':
		return "PPID";
	case 'U':
		return "USER";
	case 'S':
		return "SID";
	case 'C':
		return "COMMAND";
	default:
		return "PID";
	}
}

/*
 * Comparison function used for qsort.
 * Sorts process nodes based on the currently selected sort mode in G.sort_mode.
 */
static int sort_func(const void *a, const void *b)
{
	proc_node_t *na = *(proc_node_t **) a;
	proc_node_t *nb = *(proc_node_t **) b;
	procps_status_t *pa = na->pinfo;
	procps_status_t *pb = nb->pinfo;
	int res = 0;
	switch (G.sort_mode) {
	case 'P':
		res = DIAG_CMP(nb->cpu_pcnt_10, na->cpu_pcnt_10);
		break;
	case 'M':
		res = DIAG_CMP(pb->rss, pa->rss);
		break;
	case 'V':
		res = DIAG_CMP(pb->vsz, pa->vsz);
		break;
	case 'I':
		res = DIAG_CMP(pa->pid, pb->pid);
		break;
	case 'O':
		res = DIAG_CMP(pa->ppid, pb->ppid);
		break;
	case 'U':
		res = DIAG_CMP(pa->uid, pb->uid);
		break;
	case 'S':
		res = DIAG_CMP(pa->sid, pb->sid);
		break;
	case 'C':
		res = strcmp(pa->comm, pb->comm);
		break;
	}
	return res ? res : DIAG_CMP(pa->pid, pb->pid);
}

/*
 * Recursively prints the process tree with visual tree lines.
 * Uses a bitmask to keep track of vertical lines needed at each indentation level.
 */
static void print_tree_rich(proc_node_t *curr, int indent, uint64_t mask)
{
	while (curr) {
		bool has_sibling = (curr->base.sibling != NULL);
		int safe_indent = (indent > 60) ? 60 : indent;

		for (int i = 0; i < safe_indent; i++)
			printf((mask & (1ULL << i)) ? "│   " : "    ");

		printf(has_sibling ? "├── " : "└── ");
		printf("%-6d %-15.15s [%c] %8s %3u.%1u%% %3u.%1u%% %8s%s\n",
			   curr->pinfo->pid,
			   curr->pinfo->comm,
			   curr->pinfo->state[0],
			   make_human_readable_str(curr->pinfo->rss * 1024ULL, 1, 0),
			   curr->cpu_pcnt_10 / 10,
			   curr->cpu_pcnt_10 % 10,
			   curr->mem_pcnt_10 / 10,
			   curr->mem_pcnt_10 % 10,
			   curr->time_str,
			   DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL));

		if (curr->base.child) {
			uint64_t next_mask = mask;
			if (has_sibling)
				next_mask |= (1ULL << (indent % 64));
			else
				next_mask &= ~(1ULL << (indent % 64));

			print_tree_rich(
				(proc_node_t *) curr->base.child, indent + 1, next_mask);
		}
		curr = (proc_node_t *) curr->base.sibling;
	}
}

/*
 * Scans the procfs to build a linked list of all current processes.
 * Optionally filters by a target PID if specified in the global context.
 */
static proc_node_t *fetch_proc_list(void)
{
	proc_node_t *list = NULL;
	procps_status_t *p = NULL;
	int flags = PSSCAN_PID | PSSCAN_PPID | PSSCAN_COMM | PSSCAN_RSS |
				PSSCAN_VSZ | PSSCAN_UTIME | PSSCAN_STIME | PSSCAN_STATE |
				PSSCAN_NICE | PSSCAN_UIDGID;

	while ((p = procps_scan(p, flags)) != NULL) {
		if (G.target_pid > 0 && p->pid != G.target_pid)
			continue;

		proc_node_t *n = xzalloc(sizeof(*n) + sizeof(*p));
		n->pinfo = (procps_status_t *) (n + 1);
		memcpy(n->pinfo, p, sizeof(*p));
		n->base.id = p->pid;
		n->base.parent_id = p->ppid;
		n->base.next = (diag_node_base_t *) list;
		list = n;
	}
	return list;
}

/*
 * Prints the top header including system memory and load averages.
 * Skipped in batch mode.
 */
static void print_header(diag_sys_snap_t *snap)
{
	if (G.batch_mode)
		return;

	printf(DIAG_CLR_SCR);
	printf(DIAG_CYAN "[MY_PROC]" DIAG_RESET " Mode: " DIAG_YELLOW
					 "%s" DIAG_RESET " | Sort: " DIAG_GREEN "%s" DIAG_RESET
					 " | Width: %d" DIAG_CLR_EOL "\n",
		   G.view_mode == VIEW_TREE ? "TREE" : "LIST",
		   get_sort_label(),
		   G.width);

	printf("Mem: %6s total, %6s free | Load: %.2f %.2f %.2f" DIAG_CLR_EOL "\n",
		   make_human_readable_str(snap->total_mem_kb * 1024ULL, 1, 0),
		   make_human_readable_str(snap->free_mem_kb * 1024ULL, 1, 0),
		   snap->load_avg[0],
		   snap->load_avg[1],
		   snap->load_avg[2]);
}

/*
 * Handles user input in interactive mode.
 * Returns false if the user requests to quit (e.g., presses 'Q'), true otherwise.
 */
static bool handle_input(void)
{
	char c;

	if (G.batch_mode)
		return true;
	while (1) {
		int res = diag_ui_read_key(&c);
		if (res < 0)
			return false; /* EOF occurred */
		if (res == 0)
			break; /* No input */

		if (c == 'Q')
			return false;
		if (c == 'T') {
			G.view_mode = (G.view_mode == VIEW_TREE) ? VIEW_TOP : VIEW_TREE;
			printf(DIAG_CLEAR);
		} else if (c && strchr("PMIVOUSC", c)) {
			G.sort_mode = c;
		} else if (c == 'K') {
			int pid_to_kill =
				diag_ui_ask_int("Enter PID to kill (0 to cancel): ");
			if (pid_to_kill > 0) {
				if (kill(pid_to_kill, SIGTERM) == 0)
					sleep1();
				else
					bb_perror_msg("kill failed");
			}
		}
	}
	return true;
}

/*
 * Calculates CPU and memory usage percentages for each process,
 * and formats the total CPU time into a string.
 */
static void prepare_display_data(proc_node_t *head,
								 proc_node_t **prev_arr,
								 int prev_cnt,
								 unsigned long long diff,
								 unsigned long total_mem)
{
	for (proc_node_t *n = head; n; n = (proc_node_t *) n->base.next) {
		n->cpu_pcnt_10 = 0;
		if (prev_arr && diff > 0) {
			proc_node_t *p = (proc_node_t *) diag_find_node(
				(diag_node_base_t **) prev_arr, prev_cnt, n->base.id);
			if (p) {
				long ticks = (long) ((n->pinfo->utime + n->pinfo->stime) -
									 (p->pinfo->utime + p->pinfo->stime));
				if (ticks > 0)
					n->cpu_pcnt_10 = (unsigned) ((ticks * 1000ULL) / diff);
			}
		}
		n->mem_pcnt_10 =
			(total_mem > 0) ? (unsigned) ((n->pinfo->rss * 1000ULL) / total_mem)
							: 0;
		diag_format_time(n->time_str, n->pinfo->utime, n->pinfo->stime);
	}
}

/*
 * Displays the list of processes in a hierarchical tree view.
 */
static void display_tree(proc_node_t **sorted_arr, int cnt)
{

	diag_node_base_t *root_list =
		diag_link_tree((diag_node_base_t **) sorted_arr, cnt);

	printf("%-7s %-18s %-4s %9s %6s %6s %8s%s\n",
		   "PID",
		   "COMMAND",
		   "STAT",
		   "RSS",
		   "CPU%",
		   "MEM%",
		   "TIME",
		   DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL));
	if (!G.batch_mode)
		printf("---------------------------------------------------------------"
			   "-------" DIAG_CLR_EOL "\n");
	print_tree_rich((proc_node_t *) root_list, 0, 0);
}

/*
 * Displays the list of processes in a flat, sorted table view.
 */
static void display_list(proc_node_t **sorted_arr, int cnt)
{

	qsort(sorted_arr, cnt, sizeof(proc_node_t *), sort_func);
	int fixed_width = 6 + 1 + 6 + 1 + 6 + 1 + 10 + 1 + 4 + 1 + 4 + 1 + 8 + 1 +
					  8 + 1 + 6 + 1 + 6 + 1 + 8 + 1;
	int comm_width = G.width - fixed_width - 1;
	if (comm_width < 10)
		comm_width = 10;
	if (G.batch_mode) {
		printf("%-6s %-6s %-6s %-10s %-4s %-4s %8s %8s %6s %6s %8s COMMAND\n",
			   "PID",
			   "PPID",
			   "SID",
			   "USER",
			   "STAT",
			   "NI",
			   "VSZ",
			   "RSS",
			   "%CPU",
			   "%MEM",
			   "TIME");
	} else {
		printf("%-6s %-6s %-6s %-10s %-4s %-4s %8s %8s %6s %6s %8s "
			   "%.*s" DIAG_CLR_EOL "\n",
			   "PID",
			   "PPID",
			   "SID",
			   "USER",
			   "STAT",
			   "NI",
			   "VSZ",
			   "RSS",
			   "%CPU",
			   "%MEM",
			   "TIME",
			   comm_width,
			   "COMMAND");
	}

	for (int i = 0; i < cnt; i++) {

		if (!G.batch_mode && i >= (G.height - 6))
			break;
		proc_node_t *cn = sorted_arr[i];
		const char *user = get_cached_username(cn->pinfo->uid);
		printf("%-6d %-6d %-6d %-10.10s %-4c %-4d %8s %8s %4u.%1u %4u.%1u "
			   "%8s %.*s%s\n",
			   cn->pinfo->pid,
			   cn->pinfo->ppid,
			   cn->pinfo->sid,
			   user,
			   cn->pinfo->state[0],
			   cn->pinfo->niceness,
			   make_human_readable_str(cn->pinfo->vsz * 1024ULL, 1, 0),
			   make_human_readable_str(cn->pinfo->rss * 1024ULL, 1, 0),
			   cn->cpu_pcnt_10 / 10,
			   cn->cpu_pcnt_10 % 10,
			   cn->mem_pcnt_10 / 10,
			   cn->mem_pcnt_10 % 10,
			   cn->time_str,
			   G.batch_mode ? 256 : comm_width,
			   cn->pinfo->comm,
			   DIAG_ANSI(G.batch_mode, DIAG_CLR_EOL));
	}
}

/*
 * Main loop for the top-like process viewer.
 * Periodically fetches process data, computes usage, and updates the display.
 */
static void show_top_with_cpu(void)
{

	diag_sys_snap_t snap;
	proc_node_t *prev_list = NULL;
	proc_node_t **prev_sort_arr = NULL;
	int prev_cnt = 0;
	unsigned long long prev_ticks = 0;

	if (!isatty(STDOUT_FILENO))
		G.batch_mode = true;
	if (!G.batch_mode) {
		diag_tui_init();
	}

	while (G.iterations != 0) {

		update_term_size();
		diag_get_sys_snap(&snap);
		unsigned long long curr_ticks = snap.cpu_total_ticks;
		unsigned long long diff =
			(prev_ticks > 0) ? (curr_ticks - prev_ticks) : 0;
		proc_node_t *curr_list = fetch_proc_list();
		int curr_cnt = 0;
		proc_node_t **curr_sort_arr = (proc_node_t **) diag_nodes_to_array(
			(diag_node_base_t *) curr_list, &curr_cnt);

		if (prev_sort_arr && diff > 0) {
			qsort(prev_sort_arr,
				  prev_cnt,
				  sizeof(diag_node_base_t *),
				  diag_node_cmp);
		}

		if (!handle_input()) {
			if (curr_list)
				diag_free_node_list((diag_node_base_t *) curr_list);
			free(curr_sort_arr);
			break;
		}

		prepare_display_data(
			curr_list, prev_sort_arr, prev_cnt, diff, snap.total_mem_kb);
		print_header(&snap);
		if (G.view_mode == VIEW_TREE) {
			display_tree(curr_sort_arr, curr_cnt);
		} else {
			display_list(curr_sort_arr, curr_cnt);
		}

		if (!G.batch_mode) {
			printf(DIAG_CLR_DOWN
				   "\n" DIAG_INVERT
				   " SORT: (P)CPU (M)RSS (V)VSZ (I)PID (O)PPID "
				   "(U)USER (S)SID (C)CMD | (T)TREE (K)KILL (Q)QUIT " DIAG_RESET
					   DIAG_CLR_EOL);
			fflush(stdout);
		}

		if (prev_list)
			diag_free_node_list((diag_node_base_t *) prev_list);
		if (prev_sort_arr)
			free(prev_sort_arr);

		prev_list = curr_list;
		prev_sort_arr = curr_sort_arr;
		prev_cnt = curr_cnt;
		prev_ticks = curr_ticks;

		if (G.iterations > 0)
			G.iterations--;
		if (G.iterations == 0)
			break;

		diag_delay((int) (G.delay * 1000), G.batch_mode);
	}

	if (prev_list)
		diag_free_node_list((diag_node_base_t *) prev_list);
	if (prev_sort_arr)
		free(prev_sort_arr);

	if (!G.batch_mode) {
		diag_tui_restore();
		printf("\n");
		fflush(stdout);
	}
}

/* Command line options flags */
enum {
	OPT_t = (1 << 0),
	OPT_d = (1 << 1),
	OPT_n = (1 << 2),
	OPT_p = (1 << 3),
	OPT_b = (1 << 4),
};

/*
 * Main entry point for the my_proc applet.
 * Parses command-line arguments and starts the process viewer.
 */
int my_proc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_proc_main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOFBF, 65536);
	char *delay_str = NULL;
	int iterations = -1;
	int pid = -1;
	int opts;

	opts = getopt32(argv, "td:n:+p:+b", &delay_str, &iterations, &pid);

	if (delay_str)
		G.delay = atof(delay_str);
	if (opts & OPT_n)
		G.iterations = iterations;
	if (opts & OPT_p)
		G.target_pid = pid;
	if (opts & OPT_b)
		G.batch_mode = true;
	if (opts & OPT_t)
		G.view_mode = VIEW_TREE;

	show_top_with_cpu();
	return EXIT_SUCCESS;
}
