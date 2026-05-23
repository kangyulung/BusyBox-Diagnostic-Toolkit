#include "libdiag.h"
#include <sys/vfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysinfo.h>

/* Read total CPU time ticks from /proc/stat */
static unsigned long long get_cpu_usage_ticks(void)
{
	unsigned long long utime = 0, ntime = 0, stime = 0, itime = 0;
	unsigned long long iowtime = 0, irq = 0, sirq = 0, steal = 0;
	char buf[256];
	FILE *fp = fopen_for_read("/proc/stat");
	if (!fp)
		return 0;

	if (fgets(buf, sizeof(buf), fp)) {
		/* Parse the first line of /proc/stat (cpu total) */
		if (sscanf(buf,
				   "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
				   &utime,
				   &ntime,
				   &stime,
				   &itime,
				   &iowtime,
				   &irq,
				   &sirq,
				   &steal) < 4) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	return utime + ntime + stime + itime + iowtime + irq + sirq + steal;
}

/* Convert CPU time ticks to human-readable format (HH:MM:SS or MM:SS.cc) */
char *
diag_format_time(char *buf, unsigned long long utime, unsigned long long stime)
{
	unsigned long long total_ticks = utime + stime;
	unsigned int hz = bb_clk_tck();
	unsigned long s = (total_ticks / hz);
	unsigned long m = s / 60;
	s %= 60;
	if (m >= 60)
		snprintf(buf, 16, "%lu:%02lu:%02lu", m / 60, m % 60, s);
	else
		snprintf(buf,
				 16,
				 "%02lu:%02lu.%02llu",
				 m,
				 s,
				 (total_ticks * 100 / hz) % 100);
	return buf;
}

/* --- Filesystem and Connection Monitoring --- */

/* Get filesystem usage (inodes and disk space) for the specified path */
int diag_read_fs(const char *path, diag_fs_t *f)
{
	struct statfs s;
	if (statfs(path, &s) != 0)
		return -1;

	f->total_inodes = s.f_files;
	f->free_inodes = s.f_ffree;
	/* f_frsize is the actual fragment size used by df; f_bsize is optimal transfer size, can differ by 256x in virtiofs etc. */
	f->total_bytes = (uint64_t) s.f_blocks * s.f_frsize;
	f->free_bytes = (uint64_t) s.f_bavail * s.f_frsize;
	f->free_bytes_priv = (uint64_t) s.f_bfree * s.f_frsize;
	return 0;
}

/* Convert TCP state code to string description */
const char *diag_get_tcp_state(int state)
{
	static const char *tcp_states[] = {NULL,
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
	if (state < 1 || state > DIAG_TCP_STATES_MAX)
		return tcp_states[0];
	return tcp_states[state];
}

/* --- File Fragmentation Analysis (for diag_fs) --- */

/* Max number of extents for a single ioctl; covers the vast majority of real files */
#define FIEMAP_INIT_COUNT 512

int diag_read_fragmentation(const char *path,
							diag_frag_t *f,
							int collect_extents)
{
	struct stat sb;
	struct fiemap *fm;
	int fd;

	memset(f, 0, sizeof(*f));

	/* O_NOFOLLOW: Do not follow symlinks, preventing accidental traversal across mount points */
	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return -1;

	if (fstat(fd, &sb) != 0) {
		close(fd);
		return -1;
	}
	if (!S_ISREG(sb.st_mode)) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	f->file_size = (uint64_t) sb.st_size;
	f->block_size = (uint32_t) sb.st_blksize;

	if (!collect_extents) {
		/* count-only path (-F directory scan): fm_extent_count=0 tells kernel to return only the total count */
		struct fiemap fm_stack;
		memset(&fm_stack, 0, sizeof(fm_stack));
		fm_stack.fm_start = 0;
		fm_stack.fm_length = FIEMAP_MAX_OFFSET;
		fm_stack.fm_flags = 0;
		fm_stack.fm_extent_count = 0;
		if (ioctl(fd, FS_IOC_FIEMAP, &fm_stack) != 0) {
			close(fd);
			return -1;
		}
		f->extent_count = fm_stack.fm_mapped_extents;
		close(fd);
		return 0;
	}

	/* collect_extents=1 (-f single file): try single ioctl with large buffer first.
	 * If the last extent has FIEMAP_EXTENT_LAST, all are retrieved;
	 * Otherwise, buffer is insufficient, fallback to count-only + exact size second call. */
	size_t sz = sizeof(*fm) + sizeof(struct fiemap_extent) * FIEMAP_INIT_COUNT;
	fm = xzalloc(sz);
	fm->fm_start = 0;
	fm->fm_length = FIEMAP_MAX_OFFSET;
	fm->fm_flags = 0;
	fm->fm_extent_count = FIEMAP_INIT_COUNT;

	if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
		free(fm);
		close(fd);
		return -1;
	}

	/* Determine if all extents have been retrieved */
	int all_done = (fm->fm_mapped_extents == 0) ||
				   (fm->fm_extents[fm->fm_mapped_extents - 1].fe_flags &
					FIEMAP_EXTENT_LAST);

	if (!all_done) {
		/* fallback: number of extents exceeds FIEMAP_INIT_COUNT, retrieve in two calls */
		uint32_t total;
		free(fm);
		fm = xzalloc(sizeof(*fm));
		fm->fm_start = 0;
		fm->fm_length = FIEMAP_MAX_OFFSET;
		fm->fm_flags = 0;
		fm->fm_extent_count = 0;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
			free(fm);
			close(fd);
			return -1;
		}
		total = fm->fm_mapped_extents;
		free(fm);

		sz = sizeof(*fm) + sizeof(struct fiemap_extent) * total;
		fm = xzalloc(sz);
		fm->fm_start = 0;
		fm->fm_length = FIEMAP_MAX_OFFSET;
		fm->fm_flags = 0;
		fm->fm_extent_count = total;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
			free(fm);
			close(fd);
			return -1;
		}
	}

	f->extent_count = fm->fm_mapped_extents;
	if (f->extent_count > 0) {
		f->extents = xmalloc(sizeof(struct fiemap_extent) * f->extent_count);
		memcpy(f->extents,
			   fm->fm_extents,
			   sizeof(struct fiemap_extent) * f->extent_count);
	}
	free(fm);
	close(fd);
	return 0;
}

/* Frees dynamically allocated memory within a fragmentation structure */
void diag_free_frag(diag_frag_t *f)
{
	if (f && f->extents) {
		free(f->extents);
		f->extents = NULL;
	}
}
/* --- System Snapshot (Memory & Load) --- */

/* Get current system resource snapshot, including memory, load, and CPU ticks */
void diag_get_sys_snap(diag_sys_snap_t *snap)
{
	memset(snap, 0, sizeof(*snap));
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		snap->total_mem_kb =
			(si.totalram * (unsigned long long) si.mem_unit) / 1024;
		snap->free_mem_kb =
			(si.freeram * (unsigned long long) si.mem_unit) / 1024;
		snap->load_avg[0] = si.loads[0] / 65536.0;
		snap->load_avg[1] = si.loads[1] / 65536.0;
		snap->load_avg[2] = si.loads[2] / 65536.0;
	}
	snap->cpu_total_ticks = get_cpu_usage_ticks();
}

/* --- UI Terminal Mode Switching --- */

static struct termios g_tui_saved_termios;
static volatile sig_atomic_t g_tui_active = 0;

/* Restores the terminal to its normal state, exiting raw mode and showing the cursor */
void diag_tui_restore(void)
{
	static const char show_cursor[] = DIAG_SHOW;
	if (!g_tui_active)
		return;
	g_tui_active = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_saved_termios);
	write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
}

/* atexit handler to ensure terminal restoration upon normal exit */
static void diag_tui_atexit(void)
{
	diag_tui_restore();
}

/* Signal handler to ensure terminal restoration upon abnormal termination */
static void diag_tui_sig_handler(int sig)
{
	diag_tui_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

/* Initializes the terminal for TUI mode (enters raw mode and hides the cursor) */
void diag_tui_init(void)
{
	static int registered = 0;
	if (g_tui_active)
		return;
	set_termios_to_raw(STDIN_FILENO, &g_tui_saved_termios, 0);
	printf(DIAG_HIDE DIAG_CLR_SCR);
	fflush(stdout);
	g_tui_active = 1;
	if (!registered) {
		atexit(diag_tui_atexit);
		bb_signals(BB_FATAL_SIGS, diag_tui_sig_handler);
		registered = 1;
	}
}

/* Prompt user for integer input during UI execution (temporarily restores normal terminal mode) */
int diag_ui_ask_int(const char *prompt)
{

	char buf[32];
	int res = 0;
	diag_tui_restore();
	printf("\n%s", prompt);
	fflush(stdout);

	if (fgets(buf, sizeof(buf), stdin)) {
		res = atoi(buf);
	}

	diag_tui_init();
	return res;
}

/*
 * Non-blocking read of a single key press from standard input.
 * Converts to uppercase before returning.
 * Returns 1 on successful read, 0 if no input is ready, -1 on error/EOF.
 */
int diag_ui_read_key(char *out_key)
{
	struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
	char c;

	if (safe_poll(&pfd, 1, 0) <= 0)
		return 0;

	if (safe_read(STDIN_FILENO, &c, 1) <= 0)
		return -1;

	if (c == 27) {
		while (safe_poll(&pfd, 1, 0) > 0 && safe_read(STDIN_FILENO, &c, 1) > 0)
			continue;
		return 0;
	}

	*out_key = (char) toupper((unsigned char) c);
	return 1;
}

/* Binary search helper: search for node by ID */
diag_node_base_t *diag_find_node(diag_node_base_t **arr, int size, int id)
{
	int low = 0, high = size - 1;
	while (low <= high) {
		int mid = (low + high) / 2;
		if (arr[mid]->id == id)
			return arr[mid];
		if (arr[mid]->id < id)
			low = mid + 1;
		else
			high = mid - 1;
	}
	return NULL;
}

/* Sorting comparison function: ascending order by ID */
int diag_node_cmp(const void *a, const void *b)
{
	int id_a = (*(diag_node_base_t **) a)->id;
	int id_b = (*(diag_node_base_t **) b)->id;
	return DIAG_CMP(id_a, id_b);
}

/* Convert linked list to pointer array for easy sorting and random access */
diag_node_base_t **diag_nodes_to_array(diag_node_base_t *list, int *out_cnt)
{
	int cnt = 0;
	diag_node_base_t *curr = list;
	diag_node_base_t **arr;

	while (curr) {
		cnt++;
		curr = curr->next;
	}

	if (cnt == 0) {
		*out_cnt = 0;
		return NULL;
	}

	arr = xmalloc(sizeof(diag_node_base_t *) * cnt);
	curr = list;
	for (int i = 0; i < cnt; i++) {
		arr[i] = curr;
		curr = curr->next;
	}

	*out_cnt = cnt;
	return arr;
}

/* Reconstruct flat list into a tree structure based on parent_id */
diag_node_base_t *diag_link_tree(diag_node_base_t **nodes, int count)
{

	diag_node_base_t *root_list = NULL;
	qsort(nodes, count, sizeof(diag_node_base_t *), diag_node_cmp);

	for (int i = count - 1; i >= 0; i--) {
		diag_node_base_t *curr = nodes[i];
		diag_node_base_t *parent =
			(curr->parent_id > 0)
				? diag_find_node(nodes, count, curr->parent_id)
				: NULL;

		if (parent && parent != curr) {
			/* Establish parent-child relationship */
			curr->sibling = parent->child;
			parent->child = curr;
		} else {
			/* Nodes without a parent or pointing to themselves are classified as root nodes */
			curr->sibling = root_list;
			root_list = curr;
		}
	}
	return root_list;
}

/* Frees the memory allocated for a linked list of nodes */
void diag_free_node_list(diag_node_base_t *head)
{
	while (head) {
		diag_node_base_t *tmp = head;
		head = head->next;
		free(tmp);
	}
}

void diag_delay(int ms, int batch_mode)
{
	if (batch_mode) {
		/* Batch mode does not rely on interaction, sleep only to avoid 100% CPU spinning on EOF */
		usleep((useconds_t) ms * 1000);
	} else {
		struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
		safe_poll(&pfd, 1, ms);
	}
}
