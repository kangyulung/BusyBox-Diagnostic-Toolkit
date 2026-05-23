/* vi: set sw=4 ts=4: */
// clang-format off
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
// clang-format on

#include "libdiag.h"
#include <ftw.h>
#include <mntent.h>


/* Mount list node parsed from the mount table.
 * The three strings are allocated with xstrdup() and freed by
 * free_mount_list().
 * This avoids fixed PATH_MAX arrays, which would waste about 8 KB of
 * zero-fill work per node. */
typedef struct mount_node {
	char *device;
	char *mountpoint;
	char *fstype;
	struct mount_node *next;
} mount_node_t;

/* Display fields for one mounted filesystem, filled by get_fs_entry().
 * The three strings are allocated with xstrdup() and must be released
 * with free_fs_entry(). */
typedef struct {
	char *device;
	char *path;
	/* Block usage fields, used by the default view. */
	uint64_t total_1k;
	uint64_t used_1k;
	uint64_t avail_1k;
	unsigned use_pct; /* Ceiling-rounded, user view, matching df. */
	/* Inode usage fields, used by -i. */
	unsigned long total_inodes;
	unsigned long used_inodes;
	unsigned long free_inodes;
	unsigned iuse_pct; /* Ceiling-rounded. */
	/* Dual-perspective fields, used by -r. */
	uint64_t rootresv_1k;  /* (f_bfree - f_bavail) * f_frsize / 1024 */
	unsigned use_pct_real; /* ceiling(used_real / total * 100), including reserved blocks */
} fs_entry_t;

/* L2: mounted-filesystem fragmentation statistics for -F PATH. */
#define L2_TOP_N 10

struct l2_top_entry {
	char path[PATH_MAX];
	uint32_t extents;
};

struct l2_ctx {
	uint64_t total;
	uint64_t frag;
	uint64_t skipped; /* FTW_F entries where FIEMAP failed... */
	uint64_t dist[4]; /* [0]=1, [1]=2-4, [2]=5-16, [3]=17+ extents */
	struct l2_top_entry top[L2_TOP_N];
	int top_count;
};

typedef enum {
	FS_VIEW_DF,
	FS_VIEW_INODE,
	FS_VIEW_RESERVED,
	FS_VIEW_FRAG,
} fs_view_t;

typedef struct {
	fs_view_t view_mode;
	bool human;
	bool is_tui;
	bool frag_ready;
	struct l2_ctx frag_cache;
	mount_node_t *mount_cache;
	time_t mount_cache_ts;
	struct l2_ctx l2;
} fs_ctx_t;

static fs_ctx_t G;

static void free_fs_entry(fs_entry_t *e)
{
	if (!e)
		return;
	free(e->device);
	e->device = NULL;
	free(e->path);
	e->path = NULL;
}

/* Parse the mount table and return a linked list in file order.
 * Use setmntent()/getmntent() so octal escapes in paths, such as
 * "\040" for spaces, are decoded correctly.
 * bb_path_mtab_file selects /etc/mtab or /proc/mounts according to the
 * BusyBox build configuration. */
static mount_node_t *get_mount_list(void)
{
	FILE *fp;
	struct mntent *entry;
	mount_node_t *head = NULL, *tail = NULL;

	fp = setmntent(bb_path_mtab_file, "r");
	if (!fp)
		return NULL;

	while ((entry = getmntent(fp)) != NULL) {
		mount_node_t *node = xzalloc(sizeof(mount_node_t));
		node->device = xstrdup(entry->mnt_fsname);
		node->mountpoint = xstrdup(entry->mnt_dir);
		node->fstype = xstrdup(entry->mnt_type);
		if (!head)
			head = node;
		else
			tail->next = node;
		tail = node;
	}
	endmntent(fp);
	return head;
}

static void free_mount_list(mount_node_t *head)
{
	while (head) {
		mount_node_t *next = head->next;
		free(head->device);
		free(head->mountpoint);
		free(head->fstype);
		free(head);
		head = next;
	}
}

/* Calculate percentage with ceiling rounding, matching df's Use% calculation */
static unsigned calc_use_pct(uint64_t used, uint64_t total)
{
	return (total > 0) ? (unsigned) ((used * 100 + total - 1) / total) : 0;
}

/* Read raw filesystem data with diag_read_fs() and derive display fields. */
static int get_fs_entry(const char *path, fs_entry_t *e)
{
	diag_fs_t fs;

	if (diag_read_fs(path, &fs) != 0)
		return -1;

	uint64_t used = fs.total_bytes - fs.free_bytes_priv;
	uint64_t avail = fs.free_bytes;
	uint64_t nonr_tot = used + avail;

	e->path = xstrdup(path);
	e->total_1k = fs.total_bytes / 1024;
	e->used_1k = used / 1024;
	e->avail_1k = avail / 1024;
	e->use_pct = calc_use_pct(used, nonr_tot);

	/* Inode fields. */
	unsigned long used_in = (fs.total_inodes >= fs.free_inodes)
								? fs.total_inodes - fs.free_inodes
								: 0;
	e->total_inodes = fs.total_inodes;
	e->used_inodes = used_in;
	e->free_inodes = fs.free_inodes;
	e->iuse_pct = calc_use_pct(used_in, fs.total_inodes);

	/* Dual-perspective fields: rootresv_1k is reserved space for root,
	 * and use_pct_real is usage including reserved blocks. */
	e->rootresv_1k = (fs.free_bytes_priv > fs.free_bytes)
						 ? (fs.free_bytes_priv - fs.free_bytes) / 1024
						 : 0;
	{
		uint64_t used_real = (fs.total_bytes >= fs.free_bytes_priv)
								 ? fs.total_bytes - fs.free_bytes_priv
								 : 0;
		e->use_pct_real = calc_use_pct(used_real, fs.total_bytes);
	}

	return 0;
}

/* Return the number of decimal digits needed for a uint64_t value. */
static int uint64_width(uint64_t v)
{
	int w = 1;
	while (v >= 10) {
		v /= 10;
		w++;
	}
	return w;
}

/* Base human-readable formatter supporting both capacities and raw counts.
 * Zero is printed as "0". */
static char *fmt_human_base(uint64_t n, int is_kib, char *buf, size_t buflen)
{
	static const char units[] = "KMGTPE";
	double val = (double) n;
	int u = is_kib ? 0 : -1;

	if (n == 0) {
		snprintf(buf, buflen, "0");
		return buf;
	}
	while (val >= 1024.0 && u < (int) (sizeof(units) - 2)) {
		val /= 1024.0;
		u++;
	}
	if (u < 0) {
		snprintf(buf, buflen, "%llu", (unsigned long long) n);
	} else if (val < 10.0) {
			/* Apply ceiling rounding, matching the val >= 10 branch and GNU df -h behavior. */
		int t = (int) (val * 10.0);
		if ((double) t < val * 10.0)
			t++;
		if (t >= 100)
			snprintf(buf, buflen, "%d%c", t / 10, units[u]);
		else
			snprintf(buf, buflen, "%d.%d%c", t / 10, t % 10, units[u]);
	} else {
		uint64_t c = (uint64_t) val;
		if ((double) c < val)
			c++;
		snprintf(buf, buflen, "%llu%c", (unsigned long long) c, units[u]);
	}
	return buf;
}

static char *fmt_human(uint64_t kb, char *buf, size_t buflen)
{
	return fmt_human_base(kb, 1, buf, buflen);
}

static char *fmt_human_count(uint64_t n, char *buf, size_t buflen)
{
	return fmt_human_base(n, 0, buf, buflen);
}

/*
 * Scan once to compute maximum column widths, then print all rows.
 * Three output modes are supported:
 *   inode=1    -> -i inode view
 *   reserved=1 -> -r dual Use% + RootResv view
 *   both zero  -> default block view, matching df
 * All three modes can be combined with human=1 (-h).
 */
static void print_entries(const fs_entry_t *e, int n)
{
	int dev_w, col1_w, col2_w, col3_w, w, i;
	const char *eol = DIAG_ANSI(!G.is_tui, DIAG_CLR_EOL);

	/* Inode view (-i). */
	if (G.view_mode == FS_VIEW_INODE) {
		dev_w = (int) strlen("Filesystem");
		col1_w = (int) strlen("Inodes");
		col2_w = (int) strlen("IUsed");
		col3_w = (int) strlen("IFree");

		if (G.human) {
			char buf[16];
			int dev_max = 0, c1_max = 0, c2_max = 0, c3_max = 0;

			for (i = 0; i < n; i++) {
				w = (int) strlen(e[i].device);
				if (w > dev_max)
					dev_max = w;
				w = (int) strlen(fmt_human_count(
					(uint64_t) e[i].total_inodes, buf, sizeof(buf)));
				if (w > c1_max)
					c1_max = w;
				w = (int) strlen(fmt_human_count(
					(uint64_t) e[i].used_inodes, buf, sizeof(buf)));
				if (w > c2_max)
					c2_max = w;
				w = (int) strlen(fmt_human_count(
					(uint64_t) e[i].free_inodes, buf, sizeof(buf)));
				if (w > c3_max)
					c3_max = w;
			}
			/* If the Inodes header is wider than the longest data value, right
			 * alignment already provides the visual spacing, so the device column
			 * does not need +1 padding. This matches df behavior. */
			int i1pad = (c1_max < (int) strlen("Inodes")) ? 0 : 1;
			dev_w = (dev_max + i1pad > (int) strlen("Filesystem"))
						? dev_max + i1pad
						: (int) strlen("Filesystem");
			col1_w = (c1_max > (int) strlen("Inodes")) ? c1_max
													   : (int) strlen("Inodes");
			col2_w = (c2_max + 1 > (int) strlen("IUsed"))
						 ? c2_max + 1
						 : (int) strlen("IUsed");
			col3_w = (c3_max + 1 > (int) strlen("IFree"))
						 ? c3_max + 1
						 : (int) strlen("IFree");

			printf("%-*s %*s %*s %*s %5s %s%s\n",
				   dev_w,
				   "Filesystem",
				   col1_w,
				   "Inodes",
				   col2_w,
				   "IUsed",
				   col3_w,
				   "IFree",
				   "IUse%",
				   "Mounted on", eol);
			for (i = 0; i < n; i++) {
				char t[16], u_[16], f[16];
				printf("%-*s %*s %*s %*s %4u%% %s%s\n",
					dev_w,
					e[i].device,
					col1_w,
					fmt_human_count((uint64_t) e[i].total_inodes, t, sizeof(t)),
					col2_w,
					fmt_human_count(
						(uint64_t) e[i].used_inodes, u_, sizeof(u_)),
					col3_w,
					fmt_human_count((uint64_t) e[i].free_inodes, f, sizeof(f)),
					e[i].iuse_pct,
					e[i].path, eol);
			}
		} else {
			for (i = 0; i < n; i++) {
				w = (int) strlen(e[i].device);
				if (w > dev_w)
					dev_w = w;
				w = uint64_width((uint64_t) e[i].total_inodes);
				if (w > col1_w)
					col1_w = w;
				w = uint64_width((uint64_t) e[i].used_inodes);
				if (w > col2_w)
					col2_w = w;
				w = uint64_width((uint64_t) e[i].free_inodes);
				if (w > col3_w)
					col3_w = w;
			}
			printf("%-*s %*s %*s %*s %5s %s%s\n",
				   dev_w,
				   "Filesystem",
				   col1_w,
				   "Inodes",
				   col2_w,
				   "IUsed",
				   col3_w,
				   "IFree",
				   "IUse%",
				   "Mounted on", eol);
			for (i = 0; i < n; i++) {
				printf("%-*s %*lu %*lu %*lu %4u%% %s%s\n",
					   dev_w,
					   e[i].device,
					   col1_w,
					   e[i].total_inodes,
					   col2_w,
					   e[i].used_inodes,
					   col3_w,
					   e[i].free_inodes,
					   e[i].iuse_pct,
					   e[i].path, eol);
			}
		}
		return;
	}

	/* Dual-perspective view (-r). */
	if (G.view_mode == FS_VIEW_RESERVED) {
		int resv_w;

		dev_w = (int) strlen("Filesystem");
		col1_w = (int) strlen("1K-blocks");
		col2_w = (int) strlen("Used");
		col3_w = (int) strlen("Available");
		resv_w = (int) strlen("RootResv");

		if (G.human) {
			char buf[16];
			int dev_max = 0, c1_max = 0, c2_max = 0, c3_max = 0, rv_max = 0;

			for (i = 0; i < n; i++) {
				w = (int) strlen(e[i].device);
				if (w > dev_max)
					dev_max = w;
				w = (int) strlen(fmt_human(e[i].total_1k, buf, sizeof(buf)));
				if (w > c1_max)
					c1_max = w;
				w = (int) strlen(fmt_human(e[i].used_1k, buf, sizeof(buf)));
				if (w > c2_max)
					c2_max = w;
				w = (int) strlen(fmt_human(e[i].avail_1k, buf, sizeof(buf)));
				if (w > c3_max)
					c3_max = w;
				w = (int) strlen(fmt_human(e[i].rootresv_1k, buf, sizeof(buf)));
				if (w > rv_max)
					rv_max = w;
			}
			dev_w = (dev_max + 1 > (int) strlen("Filesystem"))
						? dev_max + 1
						: (int) strlen("Filesystem");
			col1_w =
				(c1_max > (int) strlen("Size")) ? c1_max : (int) strlen("Size");
			col2_w = (c2_max + 1 > (int) strlen("Used")) ? c2_max + 1
														 : (int) strlen("Used");
			col3_w = (c3_max + 1 > (int) strlen("Avail"))
						 ? c3_max + 1
						 : (int) strlen("Avail");
			resv_w = (rv_max + 1 > (int) strlen("RootResv"))
						 ? rv_max + 1
						 : (int) strlen("RootResv");

			printf("%-*s %*s %*s %*s %4s %5s %*s %s%s\n",
				   dev_w,
				   "Filesystem",
				   col1_w,
				   "Size",
				   col2_w,
				   "Used",
				   col3_w,
				   "Avail",
				   "Use%",
				   "RUse%",
				   resv_w,
				   "RootResv",
				   "Mounted on", eol);
			for (i = 0; i < n; i++) {
				char tbuf[16], ubuf[16], abuf[16], rbuf[16];
				printf("%-*s %*s %*s %*s %3u%% %4u%% %*s %s%s\n",
					   dev_w,
					   e[i].device,
					   col1_w,
					   fmt_human(e[i].total_1k, tbuf, sizeof(tbuf)),
					   col2_w,
					   fmt_human(e[i].used_1k, ubuf, sizeof(ubuf)),
					   col3_w,
					   fmt_human(e[i].avail_1k, abuf, sizeof(abuf)),
					   e[i].use_pct,
					   e[i].use_pct_real,
					   resv_w,
					   fmt_human(e[i].rootresv_1k, rbuf, sizeof(rbuf)),
					   e[i].path, eol);
			}
		} else {
			for (i = 0; i < n; i++) {
				w = (int) strlen(e[i].device);
				if (w > dev_w)
					dev_w = w;
				w = uint64_width(e[i].total_1k);
				if (w > col1_w)
					col1_w = w;
				w = uint64_width(e[i].used_1k);
				if (w > col2_w)
					col2_w = w;
				w = uint64_width(e[i].avail_1k);
				if (w > col3_w)
					col3_w = w;
				w = uint64_width(e[i].rootresv_1k);
				if (w > resv_w)
					resv_w = w;
			}
			printf("%-*s %*s %*s %*s %4s %5s %*s %s%s\n",
				   dev_w,
				   "Filesystem",
				   col1_w,
				   "1K-blocks",
				   col2_w,
				   "Used",
				   col3_w,
				   "Available",
				   "Use%",
				   "RUse%",
				   resv_w,
				   "RootResv",
				   "Mounted on", eol);
			for (i = 0; i < n; i++) {
				printf("%-*s %*llu %*llu %*llu %3u%% %4u%% %*llu %s%s\n",
					   dev_w,
					   e[i].device,
					   col1_w,
					   (unsigned long long) e[i].total_1k,
					   col2_w,
					   (unsigned long long) e[i].used_1k,
					   col3_w,
					   (unsigned long long) e[i].avail_1k,
					   e[i].use_pct,
					   e[i].use_pct_real,
					   resv_w,
					   (unsigned long long) e[i].rootresv_1k,
					   e[i].path, eol);
			}
		}
		return;
	}

	/* Default block view, matching df. */
	if (G.human) {
		char buf[16];
		int dev_max = 0, total_max = 0, used_max = 0, avail_max = 0;

		for (i = 0; i < n; i++) {
			w = (int) strlen(e[i].device);
			if (w > dev_max)
				dev_max = w;
			w = (int) strlen(fmt_human(e[i].total_1k, buf, sizeof(buf)));
			if (w > total_max)
				total_max = w;
			w = (int) strlen(fmt_human(e[i].used_1k, buf, sizeof(buf)));
			if (w > used_max)
				used_max = w;
			w = (int) strlen(fmt_human(e[i].avail_1k, buf, sizeof(buf)));
			if (w > avail_max)
				avail_max = w;
		}

		dev_w = (dev_max + 1 > (int) strlen("Filesystem"))
					? dev_max + 1
					: (int) strlen("Filesystem");
		col1_w = (total_max > (int) strlen("Size")) ? total_max
													: (int) strlen("Size");
		col2_w = (used_max + 1 > (int) strlen("Used")) ? used_max + 1
													   : (int) strlen("Used");
		col3_w = (avail_max + 1 > (int) strlen("Avail"))
					 ? avail_max + 1
					 : (int) strlen("Avail");

		printf("%-*s %*s %*s %*s %4s %s%s\n",
			   dev_w,
			   "Filesystem",
			   col1_w,
			   "Size",
			   col2_w,
			   "Used",
			   col3_w,
			   "Avail",
			   "Use%",
			   "Mounted on", eol);
		for (i = 0; i < n; i++) {
			char tbuf[16], ubuf[16], abuf[16];
			printf("%-*s %*s %*s %*s %3u%% %s%s\n",
				   dev_w,
				   e[i].device,
				   col1_w,
				   fmt_human(e[i].total_1k, tbuf, sizeof(tbuf)),
				   col2_w,
				   fmt_human(e[i].used_1k, ubuf, sizeof(ubuf)),
				   col3_w,
				   fmt_human(e[i].avail_1k, abuf, sizeof(abuf)),
				   e[i].use_pct,
				   e[i].path, eol);
		}
	} else {
		dev_w = (int) strlen("Filesystem");
		col1_w = (int) strlen("1K-blocks");
		col2_w = (int) strlen("Used");
		col3_w = (int) strlen("Available");

		for (i = 0; i < n; i++) {
			w = (int) strlen(e[i].device);
			if (w > dev_w)
				dev_w = w;
			w = uint64_width(e[i].total_1k);
			if (w > col1_w)
				col1_w = w;
			w = uint64_width(e[i].used_1k);
			if (w > col2_w)
				col2_w = w;
			w = uint64_width(e[i].avail_1k);
			if (w > col3_w)
				col3_w = w;
		}

		printf("%-*s %*s %*s %*s %4s %s%s\n",
			   dev_w,
			   "Filesystem",
			   col1_w,
			   "1K-blocks",
			   col2_w,
			   "Used",
			   col3_w,
			   "Available",
			   "Use%",
			   "Mounted on", eol);
		for (i = 0; i < n; i++) {
			printf("%-*s %*llu %*llu %*llu %3u%% %s%s\n",
				   dev_w,
				   e[i].device,
				   col1_w,
				   (unsigned long long) e[i].total_1k,
				   col2_w,
				   (unsigned long long) e[i].used_1k,
				   col3_w,
				   (unsigned long long) e[i].avail_1k,
				   e[i].use_pct,
				   e[i].path, eol);
		}
	}
}

/* L1: single-file fragmentation analysis for -f FILE. */

static int print_file_frag(const char *path)
{
	diag_frag_t f;
	uint64_t blk_size, blocks, expected_phy;
	uint32_t i;

	if (diag_read_fragmentation(path, &f, 1) != 0) {
		bb_perror_msg("%s", path);
		return EXIT_FAILURE;
	}

	/* block_size comes from fstat().st_blksize in libdiag, avoiding another statfs() call. */
	blk_size = f.block_size > 0 ? (uint64_t) f.block_size : 4096;
	blocks = (f.file_size + blk_size - 1) / blk_size;

	printf("File size of %s is %llu (%llu block%s of %llu bytes)\n",
		   path,
		   (unsigned long long) f.file_size,
		   (unsigned long long) blocks,
		   blocks == 1 ? "" : "s",
		   (unsigned long long) blk_size);

	if (f.extent_count > 0) {
		printf(" ext:     logical_offset:        physical_offset: length:   "
			   "expected: flags:\n");
		expected_phy = 0;
		for (i = 0; i < f.extent_count; i++) {
			struct fiemap_extent *e = &f.extents[i];
			uint64_t log_start = e->fe_logical / blk_size;
			uint64_t log_end = (e->fe_logical + e->fe_length - 1) / blk_size;
			uint64_t phy_start = e->fe_physical / blk_size;
			uint64_t phy_end = (e->fe_physical + e->fe_length - 1) / blk_size;
			uint64_t len = e->fe_length / blk_size;
			char flags[64] = "", exp_str[24] = "";

			if (i > 0 && phy_start != expected_phy)
				snprintf(exp_str,
						 sizeof(exp_str),
						 "%llu",
						 (unsigned long long) expected_phy);

				/* Join flags with commas so multiple flags do not collapse into strings
				 * like "last,eofunknown". */
#define ADD_FLAG(bit, name)                                                    \
	do {                                                                       \
		if (e->fe_flags & (bit)) {                                             \
			if (flags[0])                                                      \
				strcat(flags, ",");                                            \
			strcat(flags, (name));                                             \
		}                                                                      \
	} while (0)
			ADD_FLAG(FIEMAP_EXTENT_LAST, "last,eof");
			ADD_FLAG(FIEMAP_EXTENT_UNKNOWN, "unknown");
			ADD_FLAG(FIEMAP_EXTENT_DELALLOC, "delalloc");
			ADD_FLAG(FIEMAP_EXTENT_ENCODED, "encoded");
#undef ADD_FLAG

			printf(" %3u:  %7llu..%8llu:  %9llu..%10llu: %6llu: %10s  %s\n",
				   i,
				   (unsigned long long) log_start,
				   (unsigned long long) log_end,
				   (unsigned long long) phy_start,
				   (unsigned long long) phy_end,
				   (unsigned long long) len,
				   exp_str,
				   flags);

			expected_phy = phy_start + len;
		}
	}

	printf("%s: %u extent%s found\n",
		   path,
		   f.extent_count,
		   f.extent_count == 1 ? "" : "s");
	diag_free_frag(&f);
	return EXIT_SUCCESS;
}

static int l2_nftw_cb(const char *path,
					  const struct stat *sb,
					  int typeflag,
					  struct FTW *ftwbuf)
{
	diag_frag_t f;
	int i, min_idx;

	(void) sb;
	(void) ftwbuf;
	if (typeflag != FTW_F)
		return 0;

	if (diag_read_fragmentation(path, &f, 0) != 0) {
		/* Regular file, but FIEMAP failed because permission was denied or the
		 * filesystem does not support it. Count it as skipped so total and the
		 * fragmentation-rate denominator are not understated. */
		G.l2.skipped++;
		return 0;
	}

	/* Skip empty files: 0 extents means no data is on disk, so there is no
	 * fragmentation to report. Without this guard, empty files would land in
	 * dist[0] (the "1 extent" bucket), inflating both total and dist[0]. */
	if (f.extent_count == 0)
		return 0;

	G.l2.total++;

	if (f.extent_count == 1)
		G.l2.dist[0]++;
	else if (f.extent_count <= 4)
		G.l2.dist[1]++;
	else if (f.extent_count <= 16)
		G.l2.dist[2]++;
	else
		G.l2.dist[3]++;

	if (f.extent_count > 1)
		G.l2.frag++;

	if (G.l2.top_count < L2_TOP_N) {
		safe_strncpy(G.l2.top[G.l2.top_count].path, path, PATH_MAX);
		G.l2.top[G.l2.top_count].extents = f.extent_count;
		G.l2.top_count++;
	} else {
		/* The top-file list is full; use the entry with the smallest extent_count
		 * as the replacement candidate. This keeps the N files with the highest
		 * extent counts. */
		min_idx = 0;
		for (i = 1; i < L2_TOP_N; i++) {
			if (G.l2.top[i].extents < G.l2.top[min_idx].extents)
				min_idx = i;
		}
		if (f.extent_count > G.l2.top[min_idx].extents) {
			safe_strncpy(G.l2.top[min_idx].path, path, PATH_MAX);
			G.l2.top[min_idx].extents = f.extent_count;
		}
	}

	return 0;
}

static int cmp_top_entry(const void *a, const void *b)
{
	const struct l2_top_entry *ea = (const struct l2_top_entry *) a;
	const struct l2_top_entry *eb = (const struct l2_top_entry *) b;
	return (ea->extents > eb->extents)	 ? -1
		   : (ea->extents < eb->extents) ? 1
										 : 0;
}

static void print_frag_summary(struct l2_ctx *c, int batch_mode)
{
	int i;
	double frag_pct = (c->total > 0)
				   ? (double) c->frag * 100.0 / (double) c->total
				   : 0.0;
	const char *eol = DIAG_ANSI(batch_mode, DIAG_CLR_EOL);

	printf("Scanned: %llu files  Fragmented: %llu (%.1f%%)  Skipped: %llu%s\n",
		   (unsigned long long) c->total,
		   (unsigned long long) c->frag,
		   frag_pct,
		   (unsigned long long) c->skipped, eol);

	if (batch_mode && c->skipped > 0)
		printf("(skipped = no read permission or filesystem without FIEMAP;"
			   " run as root for full coverage)\n");

	printf("\nFragmentation distribution:%s\n", eol);
	printf("  %-10s  %s%s\n", "Extents", "Files", eol);
	printf("  %-10s  %llu%s\n", "1", (unsigned long long) c->dist[0], eol);
	printf("  %-10s  %llu%s\n", "2-4", (unsigned long long) c->dist[1], eol);
	printf("  %-10s  %llu%s\n", "5-16", (unsigned long long) c->dist[2], eol);
	printf("  %-10s  %llu%s\n", "17+", (unsigned long long) c->dist[3], eol);

	if (c->top_count > 0) {
		qsort(c->top, c->top_count, sizeof(c->top[0]), cmp_top_entry);
		printf("\nTop %d files by extent count:%s\n", c->top_count, eol);
		printf("  %7s  %s%s\n", "Extents", "File", eol);
		for (i = 0; i < c->top_count; i++)
			printf("  %7u  %s%s\n", c->top[i].extents, c->top[i].path, eol);
	}
}

static int print_frag_stat(const char *path)
{
	memset(&G.l2, 0, sizeof(G.l2));
	printf("Scanning %s ...\n\n", path);
	/* nftw() returns -1 when even the root path cannot be walked, such as
	 * when it does not exist or permission is denied.
	 * The callback always returns 0, so 0 means the walk completed normally. */
	if (nftw(path, l2_nftw_cb, 16, FTW_MOUNT | FTW_PHYS) < 0) {
		bb_perror_msg("%s", path);
		return EXIT_FAILURE;
	}
	print_frag_summary(&G.l2, 1);
	return EXIT_SUCCESS;
}

/* P5: interactive TUI mode (-s). */

/* Mount table cache: reuse the same list within the TTL to avoid reading
 * /proc/mounts every second. */
#define MOUNT_CACHE_TTL 10

static void tui_refresh_mount_cache(void)
{
	time_t now = time(NULL);
	mount_node_t *fresh;
	if (G.mount_cache && (now - G.mount_cache_ts) < MOUNT_CACHE_TTL)
		return;
	/* Replace the old list only after a fresh list is read successfully.
	 * On failure, keep the old cache and leave the timestamp unchanged so the
	 * next refresh retries instead of clearing the TUI to an empty table. */
	fresh = get_mount_list();
	if (!fresh)
		return;
	free_mount_list(G.mount_cache);
	G.mount_cache = fresh;
	G.mount_cache_ts = now;
}

/* Scan mounts, deduplicate by st_dev, and return an fs_entry_t array.
 * The return value is the number of entries. The caller must call
 * free_fs_entry() for each element in *out, then free() the array.
 * has_t/opt_t and has_x/opt_x are filesystem-type filters; pass 0/NULL
 * to disable filtering, as the TUI does.
 * statfs is called once per mount entry that passes the type filter
 * (not once per unique device); for bind-mount duplicates with the
 * same st_dev, the later entry's statfs result overwrites the earlier
 * one via free_fs_entry() + ownership transfer, so the last bind mount
 * wins.
 * The seen arrays are sized by the mount count, with no fixed 256-entry
 * limit. mounts is owned by the caller and is not freed here. */
static int collect_dedup_entries(mount_node_t *mounts,
								 int has_t,
								 const char *opt_t,
								 int has_x,
								 const char *opt_x,
								 fs_entry_t **out)
{
	int mount_count = 0, seen_n = 0, n = 0, i;
	mount_node_t *m;

	for (m = mounts; m; m = m->next)
		mount_count++;
	if (mount_count == 0)
		mount_count = 1; /* Avoid xzalloc(0). */

	dev_t *seen_dev = xzalloc(mount_count * sizeof(dev_t));
	mount_node_t **seen_node = xzalloc(mount_count * sizeof(mount_node_t *));
	fs_entry_t *seen_entry = xzalloc(mount_count * sizeof(fs_entry_t));

	for (m = mounts; m; m = m->next) {
		fs_entry_t tmp;
		struct stat sb;
		memset(&tmp, 0, sizeof(tmp));
		if (has_t && strcmp(m->fstype, opt_t) != 0)
			continue;
		if (has_x && strcmp(m->fstype, opt_x) == 0)
			continue;
		if (get_fs_entry(m->mountpoint, &tmp) != 0 || tmp.total_1k == 0) {
			free_fs_entry(&tmp);
			continue;
		}
		if (stat(m->mountpoint, &sb) != 0) {
			free_fs_entry(&tmp);
			continue;
		}
		int found = 0;
		for (i = 0; i < seen_n; i++) {
			if (seen_dev[i] == sb.st_dev) {
				seen_node[i] = m;
				/* Bind mount on the same device: free the old entry strings before
				 * transferring ownership of tmp. */
				free_fs_entry(&seen_entry[i]);
				seen_entry[i] = tmp;
				found = 1;
				break;
			}
		}
		if (!found) {
			/* seen_n must be less than mount_count because it increments only once
			 * per unique st_dev (device), so no extra bounds check is needed. */
			seen_dev[seen_n] = sb.st_dev;
			seen_node[seen_n] = m;
			seen_entry[seen_n] = tmp; /* Transfer ownership. */
			seen_n++;
		}
	}

	fs_entry_t *entries = xzalloc((seen_n ? seen_n : 1) * sizeof(fs_entry_t));
	for (i = 0; i < seen_n; i++) {
		entries[n] = seen_entry[i]; /* Transfer string ownership. */
		entries[n].device = xstrdup(seen_node[i]->device);
		n++;
	}
	free(seen_dev);
	free(seen_node);
	free(seen_entry); /* Strings were moved into entries; free only the array. */
	*out = entries;
	return n;
}

static const char *tui_view_name(void)
{
	static const char *const view_names[] = {
		"Disk", "Inode", "Reserved", "Fragment"
	};
	if (G.view_mode >= FS_VIEW_DF && G.view_mode <= FS_VIEW_FRAG)
		return view_names[G.view_mode];
	return "?";
}

static void tui_print_header(void)
{
	printf(DIAG_CLR_SCR);
	printf(DIAG_CYAN "[MY_FS]" DIAG_RESET " View: " DIAG_YELLOW "%s" DIAG_RESET
					 "  Human: %s" DIAG_CLR_EOL "\n",
		   tui_view_name(),
		   G.human ? (DIAG_GREEN "on" DIAG_RESET) : "off");
	printf("D=disk  I=inode  R=reserved  F=frag(scan)  H=human  "
		   "Q=quit" DIAG_CLR_EOL "\n");
	printf("-----------------------------------------------------------"
		   "-" DIAG_CLR_EOL "\n");
}

/* Run nftw() fragmentation statistics on / and store the result in cache. */
static void tui_do_frag_scan(void)
{
	printf(DIAG_CLR_SCR);
	printf(
		DIAG_YELLOW
		"Scanning / for fragmentation, please wait..." DIAG_RESET DIAG_CLR_EOL
		"\n");
	fflush(stdout);
	memset(&G.l2, 0, sizeof(G.l2));
	nftw("/", l2_nftw_cb, 16, FTW_MOUNT | FTW_PHYS);
	G.frag_cache = G.l2;
	G.frag_ready = true;
}

static void tui_print_frag_view(void)
{
	if (!G.frag_ready) {
		printf("  (press F to start fragmentation scan on /)" DIAG_CLR_EOL
			   "\n");
		return;
	}

	printf("Scan path: /" DIAG_CLR_EOL "\n");
	print_frag_summary(&G.frag_cache, 0);
	printf("\n  [cached - press F to re-scan]" DIAG_CLR_EOL "\n");
}

static void show_fs_tui(void)
{
	/* Require both stdin and stdout to be ttys. Input is read through
	 * STDIN poll/read; if stdin is redirected, raw mode would be entered but no
	 * key could be read, leaving Ctrl-C as the only escape. */
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		bb_error_msg_and_die("-s requires a terminal");

	diag_tui_init();

	while (1) {
		char c = 0;
		diag_ui_read_key(&c);

		if (c == 'Q')
			break;
		else if (c == 'D')
			G.view_mode = FS_VIEW_DF;
		else if (c == 'I')
			G.view_mode = FS_VIEW_INODE;
		else if (c == 'R')
			G.view_mode = FS_VIEW_RESERVED;
		else if (c == 'F') {
			G.view_mode = FS_VIEW_FRAG;
			tui_do_frag_scan();
		} else if (c == 'H')
			G.human = !G.human;

		tui_print_header();
		if (G.view_mode == FS_VIEW_FRAG) {
			tui_print_frag_view();
		} else {
			tui_refresh_mount_cache();
			fs_entry_t *entries = NULL;
			int i, n = collect_dedup_entries(
					   G.mount_cache, 0, NULL, 0, NULL, &entries);
			print_entries(entries, n);
			for (i = 0; i < n; i++)
				free_fs_entry(&entries[i]);
			free(entries);
		}
		printf(DIAG_CLR_DOWN);
		fflush(stdout);

		diag_delay(1000, 0);
	}

	free_mount_list(G.mount_cache);
	G.mount_cache = NULL;

	diag_tui_restore();
	printf("\n");
	fflush(stdout);
}

enum {
	OPT_h = (1 << 0),
	OPT_i = (1 << 1),
	OPT_r = (1 << 2),
	OPT_t = (1 << 3),
	OPT_x = (1 << 4),
	OPT_f = (1 << 5),
	OPT_F = (1 << 6),
	OPT_s = (1 << 7),
};

int my_fs_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_fs_main(int argc, char **argv)
{
	char *opt_t = NULL, *opt_x = NULL, *opt_f = NULL, *opt_F = NULL;
	unsigned opts =
		getopt32(argv, "hirt:x:f:F:s", &opt_t, &opt_x, &opt_f, &opt_F);
	int has_t = (opts & OPT_t);
	int has_x = (opts & OPT_x);
	int has_f = (opts & OPT_f);
	int has_F = (opts & OPT_F);
	argv += optind;

	G.human = (opts & OPT_h);
	if (opts & OPT_i)
		G.view_mode = FS_VIEW_INODE;
	else if (opts & OPT_r)
		G.view_mode = FS_VIEW_RESERVED;
	else
		G.view_mode = FS_VIEW_DF;

	G.is_tui = (opts & OPT_s);

	/* -f, -F, and -s are mutually exclusive action modes. Combining them is
	 * ambiguous, so report a usage error instead of silently choosing one. */
	if (!!has_f + !!has_F + !!G.is_tui > 1)
		bb_show_usage();

	if (has_f)
		return print_file_frag(opt_f);
	if (has_F)
		return print_frag_stat(opt_F);
	if (G.is_tui) {
		show_fs_tui();
		return EXIT_SUCCESS;
	}

	mount_node_t *mounts = get_mount_list();
	fs_entry_t *entries;
	int n = 0, i;
	int had_error = 0;

	if (!argv[0]) {
			/* No-argument mode must be able to enumerate the mount table.
			 * NULL means either setmntent() failed or the mount table is empty;
			 * both are abnormal on Linux because /proc/mounts should always have
			 * entries. Report the error explicitly instead of printing an empty
			 * table and returning 0. */
		if (!mounts) {
			bb_perror_msg("%s", bb_path_mtab_file);
			return EXIT_FAILURE;
		}
		n = collect_dedup_entries(mounts, has_t, opt_t, has_x, opt_x, &entries);
	} else {
			/* Arguments were provided: collect one fs_entry_t for each path. */
		char **arg;
		int argc_n = 0;
		for (arg = argv; *arg; arg++)
			argc_n++;

		entries = xzalloc(argc_n * sizeof(fs_entry_t));
		for (arg = argv; *arg; arg++) {
			mount_node_t *m, *best = NULL;
			size_t best_len = 0;
			char *canon;
			const char *cpath;

			if (get_fs_entry(*arg, &entries[n]) != 0) {
				bb_perror_msg("%s", *arg);
				had_error = 1;
				continue;
			}
				/* Match df behavior: first canonicalize the path with realpath,
				 * resolving relative paths, symlinks, and "..", then choose the
				 * longest mountpoint prefix with boundary checks.
				 * The boundary check prevents /foo from matching /foobar. The
				 * longest prefix selects the deepest mountpoint, which also handles
				 * multiple bind mounts on the same device but different mountpoints. */
			canon = xmalloc_realpath(*arg);
			cpath = canon ? canon : *arg;
			for (m = mounts; m; m = m->next) {
				size_t len = strlen(m->mountpoint);
				if (strncmp(cpath, m->mountpoint, len) != 0)
					continue;
				/* For mountpoint "/", mountpoint[len - 1] == '/' always matches.
				 * Otherwise cpath must end at len or have a path separator there. */
				if (!(m->mountpoint[len - 1] == '/' || cpath[len] == '\0' ||
					  cpath[len] == '/'))
					continue;
				if (!best || len > best_len) {
					best = m;
					best_len = len;
				}
			}
			free(canon);
			if (best) {
				if ((has_t && strcmp(best->fstype, opt_t) != 0) ||
					(has_x && strcmp(best->fstype, opt_x) == 0)) {
					free_fs_entry(&entries[n]); /* Free the already allocated path when filtered out. */
					continue;
				}
				entries[n].device = xstrdup(best->device);
				/* Match df's "Mounted on" column: show the actual mountpoint instead of
				 * the user-provided path. */
				free(entries[n].path);
				entries[n].path = xstrdup(best->mountpoint);
			} else {
				entries[n].device = xstrdup(*arg);
			}
			n++;
		}
	}

	print_entries(entries, n);
	for (i = 0; i < n; i++)
		free_fs_entry(&entries[i]);
	free(entries);
	free_mount_list(mounts);
	return had_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
