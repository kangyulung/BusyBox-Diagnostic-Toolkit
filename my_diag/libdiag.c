#include "libdiag.h"
#include <sys/vfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysinfo.h>

/* 從 /proc/stat 讀取 CPU 總體時間標記 (CPU ticks) */
unsigned long long get_cpu_usage_ticks(void)
{
	unsigned long long utime = 0, ntime = 0, stime = 0, itime = 0;
	unsigned long long iowtime = 0, irq = 0, sirq = 0, steal = 0;
	char buf[256];
	FILE *fp = fopen_for_read("/proc/stat");
	if (!fp)
		return 0;

	if (fgets(buf, sizeof(buf), fp)) {
		/* 解析 /proc/stat 的第一行 (cpu 總計) */
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

/* 將 CPU 時間標記轉換為人類可讀的格式 (HH:MM:SS 或 MM:SS.cc) */
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

/* --- 檔案系統與連線監測 --- */

/* 取得指定路徑的檔案系統使用狀況 (Inodes 與 磁碟空間) */
int diag_read_fs(const char *path, diag_fs_t *f)
{
	struct statfs s;
	if (statfs(path, &s) != 0)
		return -1;

	f->total_inodes = s.f_files;
	f->free_inodes = s.f_ffree;
	/* f_frsize 是實際片段大小，df 用此欄位計算；f_bsize 是最佳傳輸大小，virtiofs 等 fs 兩者差距可達 256x */
	f->total_bytes = (uint64_t) s.f_blocks * s.f_frsize;
	f->free_bytes = (uint64_t) s.f_bavail * s.f_frsize;
	f->free_bytes_priv = (uint64_t) s.f_bfree * s.f_frsize;
	return 0;
}

/* 將 TCP 狀態代碼轉換為字串描述 */
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

/* --- 檔案碎片分析（服務 diag_fs）--- */

/* 一次 ioctl 能容納的 extent 數量上限；涵蓋絕大多數真實檔案 */
#define FIEMAP_INIT_COUNT 512

int diag_read_fragmentation(const char *path,
							diag_frag_t *f,
							int collect_extents)
{
	struct stat sb;
	struct fiemap *fm;
	int fd;

	memset(f, 0, sizeof(*f));

	/* O_NOFOLLOW：不追蹤符號連結，避免意外跨越掛載點 */
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
		/* count-only 路徑（-F 目錄掃描）：fm_extent_count=0 讓核心只回傳總數 */
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
		f->extent_count = fm->fm_mapped_extents;
		free(fm);
		close(fd);
		return 0;
	}

	/* collect_extents=1（-f 單檔）：先用大 buffer 嘗試單次 ioctl。
     * 若最後一個 extent 帶有 FIEMAP_EXTENT_LAST，代表全部取回；
     * 否則 buffer 不足，退回 count-only + 精確大小的第二次呼叫。 */
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

	/* 判斷是否已取得全部 extent */
	int all_done = (fm->fm_mapped_extents == 0) ||
				   (fm->fm_extents[fm->fm_mapped_extents - 1].fe_flags &
					FIEMAP_EXTENT_LAST);

	if (!all_done) {
		/* fallback：extent 數超過 FIEMAP_INIT_COUNT，用兩次呼叫取完 */
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

void diag_free_frag(diag_frag_t *f)
{
	if (f && f->extents) {
		free(f->extents);
		f->extents = NULL;
	}
}
/* --- 系統快照 (Memory & Load) --- */

/* 獲取當前系統資源快照，包含記憶體、負載與 CPU 標記 */
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

/* --- UI 終端模式切換 --- */

/* 切換至 Raw 模式 (禁用緩衝與回顯)，用於即時監控介面 */
void diag_ui_mode_raw(struct termios *old_t)
{

	set_termios_to_raw(STDIN_FILENO, old_t, 0);
	printf(DIAG_HIDE DIAG_CLR_SCR);
	fflush(stdout);
}

/* 恢復標準終端模式並顯示游標 */
void diag_ui_mode_normal(struct termios *old_t)
{

	printf(DIAG_SHOW);
	tcsetattr(STDIN_FILENO, TCSANOW, old_t);
	fflush(stdout);
}

static struct termios g_tui_saved_termios;
static volatile sig_atomic_t g_tui_active = 0;

void diag_tui_restore(void)
{
	static const char show_cursor[] = DIAG_SHOW;
	if (!g_tui_active)
		return;
	g_tui_active = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_saved_termios);
	write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
}

static void diag_tui_atexit(void) { diag_tui_restore(); }

static void diag_tui_sig_handler(int sig)
{
	diag_tui_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

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

/* 在 UI 執行期間提示使用者輸入整數 (會暫時恢復正常終端模式) */
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

	*out_key = (char) toupper((unsigned char)c);
	return 1;
}

/* 二元搜尋輔助函式：根據 ID 搜尋節點 */
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

/* 排序比較函式：根據 ID 升序排序 */
int diag_node_cmp(const void *a, const void *b)
{
	int id_a = (*(diag_node_base_t **) a)->id;
	int id_b = (*(diag_node_base_t **) b)->id;
	return DIAG_CMP(id_a, id_b);
}

/* 將鏈結串列轉換為指標陣列以便於排序與隨機存取 */
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

/* 根據 parent_id 將扁平串列重組為樹狀結構 */
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
			/* 建立親子關聯 */
			curr->sibling = parent->child;
			parent->child = curr;
		} else {
			/* 無父節點或指向自身者歸類為根節點 */
			curr->sibling = root_list;
			root_list = curr;
		}
	}
	return root_list;
}

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
		/* Batch 模式不依賴互動，純休眠以免讀到 EOF 引發 100% CPU 空轉 */
		usleep((useconds_t)ms * 1000);
	} else {
		struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
		safe_poll(&pfd, 1, ms);
	}
}
