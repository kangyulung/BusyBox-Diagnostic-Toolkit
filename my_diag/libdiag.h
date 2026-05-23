#ifndef LIBDIAG_H
#define LIBDIAG_H

#include "libbb.h"
#include <linux/fiemap.h>

/* ANSI control sequences for UI rendering */
#define DIAG_CLR_EOL "\033[K" /* Clear to end of line */
#define DIAG_CLR_SCR "\033[H" /* Move cursor to top-left corner */
#define DIAG_CLEAR                                                             \
	"\033[H\033[J" /* Move cursor to top-left and clear the entire screen */
#define DIAG_CLR_DOWN "\033[J" /* Clear from cursor to bottom of screen */
#define DIAG_INVERT "\e[7m"	   /* Inverse video (highlight) */
#define DIAG_RESET "\e[0m"	   /* Reset color and formatting */
#define DIAG_RED "\e[1;31m"
#define DIAG_GREEN "\e[1;32m"
#define DIAG_YELLOW "\e[1;33m"
#define DIAG_CYAN "\e[1;36m"
#define DIAG_HIDE "\033[?25l" /* Hide cursor */
#define DIAG_SHOW "\033[?25h" /* Show cursor */

/* Safe formatting for batch mode (outputs empty string in batch mode, otherwise ANSI codes) */
#define DIAG_ANSI(batch, ansi) ((batch) ? "" : (ansi))

/* Safe comparison macro, returns -1, 0, 1 (prevents subtraction overflow and simplifies qsort comparison) */
#define DIAG_CMP(a, b) (((a) > (b)) - ((a) < (b)))

/* Maximum number of TCP states */
#define DIAG_TCP_STATES_MAX 11

/* Tree structure base node: used to establish parent-child relationships between processes */
typedef struct diag_node_base {
	int id;							/* Unique node identifier (e.g., PID) */
	int parent_id;					/* Parent node identifier (e.g., PPID) */
	struct diag_node_base *child;	/* First child node */
	struct diag_node_base *sibling; /* Next sibling node */
	struct diag_node_base
		*next; /* Linear linked list pointer for iterating through all nodes */
} diag_node_base_t;

/* File system information structure (disk usage statistics) */
typedef struct {
	unsigned long total_inodes;
	unsigned long free_inodes;
	uint64_t total_bytes; /* Total capacity in bytes */
	uint64_t free_bytes;  /* Free space available to non-root users */
	uint64_t
		free_bytes_priv; /* Total free space including root-reserved blocks */
} diag_fs_t;

/* Single file fragmentation analysis results */
typedef struct {
	uint64_t file_size;	   /* File size in bytes */
	uint32_t extent_count; /* Total number of extents */
	uint32_t
		block_size; /* fstat.st_blksize; used by caller for byte-block conversions without calling statfs */
	struct fiemap_extent *
		extents; /* Detailed list of extents; NULL if not collected (requires diag_free_frag to free) */
} diag_frag_t;

/* System state snapshot (global statistics) */
typedef struct {
	unsigned long total_mem_kb;
	unsigned long free_mem_kb;
	double load_avg[3];					/* 1, 5, 15 minute load averages */
	unsigned long long cpu_total_ticks; /* Total accumulated CPU ticks */
} diag_sys_snap_t;

/* General parsing and formatting utilities */
char *
diag_format_time(char *buf, unsigned long long utime, unsigned long long stime);

/* System information collection functions */
int diag_read_fs(const char *path, diag_fs_t *f);
const char *diag_get_tcp_state(int state);
void diag_get_sys_snap(diag_sys_snap_t *snap);

/* Terminal UI mode controls */
void diag_tui_init(
	void); /* Initialize TUI (raw mode, hide cursor, register cleanup) */
void diag_tui_restore(
	void); /* Restore TUI (restore cursor and terminal mode) */
int diag_ui_ask_int(
	const char *
		prompt); /* Prompt for an integer value (automatically pauses and restores TUI) */
int diag_ui_read_key(
	char *
		out_key); /* Read a single keypress (automatically filters ESC sequences) */

/* Tree structure building utilities */
diag_node_base_t **diag_nodes_to_array(diag_node_base_t *list, int *out_cnt);
diag_node_base_t *diag_link_tree(diag_node_base_t **nodes, int count);
diag_node_base_t *diag_find_node(diag_node_base_t **arr, int size, int id);
int diag_node_cmp(const void *a, const void *b);
void diag_free_node_list(diag_node_base_t *head);

/*
 * Executes FIEMAP ioctl on a single regular file, filling f->file_size and f->extent_count.
 * collect_extents=1 -> also populates f->extents (dynamically allocated, must call diag_free_frag to free)
 * collect_extents=0 -> f->extents remains NULL, only retrieves extent_count
 * Returns 0 on success, -1 on failure (errno is set); returns -1 if path is a directory or unsupported fs
 */
int diag_read_fragmentation(const char *path,
							diag_frag_t *f,
							int collect_extents);
void diag_free_frag(diag_frag_t *f);

/* Unified wait and polling function (pure sleep in batch mode, polls stdin in TUI mode) */
void diag_delay(int ms, int batch_mode);

#endif