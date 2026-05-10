/* vi: set sw=4 ts=4: */
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


#include "libdiag.h"
#include <sys/sysinfo.h>
#include <termios.h>
#include <ctype.h>
#define SAFE_COMPARE(a, b) (((a) > (b)) - ((a) < (b)))


typedef struct proc_node {
    diag_node_base_t base;
    procps_status_t *pinfo;
    double cpu_pcnt;
    double mem_pcnt;
    char time_str[16];
} proc_node_t;

typedef enum { VIEW_TOP, VIEW_TREE } view_mode_t;

typedef struct {
    view_mode_t view_mode;
    char sort_mode;
    int iterations;
    double delay;
    int target_pid;
    bool batch_mode;
    int width, height;
} proc_ctx_t;

static proc_ctx_t G = { VIEW_TOP, 'P', -1, 1.0, -1, false, 80, 24 };

#define COLOR_CYAN   (G.batch_mode ? "" : DIAG_CYAN)
#define COLOR_YELLOW (G.batch_mode ? "" : DIAG_YELLOW)
#define COLOR_GREEN  (G.batch_mode ? "" : DIAG_GREEN)
#define COLOR_RED    (G.batch_mode ? "" : DIAG_RED)
#define COLOR_RESET  (G.batch_mode ? "" : DIAG_RESET)
#define CLR_EOL      (G.batch_mode ? "" : DIAG_CLR_EOL)
#define CLR_SCR      (G.batch_mode ? "" : DIAG_CLR_SCR)

static void update_term_size(void) {
    if (G.batch_mode) {
        G.width = 80;
        G.height = 10000;
        return;
    }
    get_terminal_width_height(STDOUT_FILENO, &G.width, &G.height);
}

static const char* get_sort_label(void) {
    switch (G.sort_mode) {
        case 'P': return "CPU%"; case 'M': return "RSS";
        case 'V': return "VSZ";  case 'I': return "PID";
        case 'O': return "PPID"; case 'U': return "USER";
        case 'S': return "STAT"; case 'C': return "COMMAND";
        default:  return "PID";
    }
}

static int sort_func(const void *a, const void *b) {
    procps_status_t *pa = (*(proc_node_t**)a)->pinfo;
    procps_status_t *pb = (*(proc_node_t**)b)->pinfo;
    int res = 0;
    switch (G.sort_mode) {
        case 'P': res = SAFE_COMPARE(pb->utime + pb->stime, pa->utime + pa->stime); break;
        case 'M': res = SAFE_COMPARE(pb->rss, pa->rss); break;
        case 'V': res = SAFE_COMPARE(pb->vsz, pa->vsz); break;
        case 'I': res = SAFE_COMPARE(pa->pid, pb->pid); break;
        case 'O': res = SAFE_COMPARE(pa->ppid, pb->ppid); break;
        case 'U': res = SAFE_COMPARE(pa->uid, pb->uid); break;
        case 'S': res = SAFE_COMPARE(pa->state[0], pb->state[0]); break; // 修正：直接比較字元
        case 'C': res = strcmp(pa->comm, pb->comm); break;
    }
    return res ? res : SAFE_COMPARE(pa->pid, pb->pid);
}

static proc_node_t* find_node_by_pid(proc_node_t **arr, int size, int pid) {
    int low = 0, high = size - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        if (arr[mid]->pinfo->pid == pid) return arr[mid];
        if (arr[mid]->pinfo->pid < pid) low = mid + 1;
        else high = mid - 1;
    }
    return NULL;
}

static int sort_by_pid(const void *a, const void *b) {
    procps_status_t *pa = (*(proc_node_t**)a)->pinfo;
    procps_status_t *pb = (*(proc_node_t**)b)->pinfo;
    return SAFE_COMPARE(pa->pid, pb->pid);
}

static void print_tree_rich(proc_node_t *curr, int indent, uint64_t mask) {
    while (curr) {
        bool has_sibling = (curr->base.sibling != NULL);
        int safe_indent = (indent > 60) ? 60 : indent;

        for (int i = 0; i < safe_indent; i++)
            printf((mask & (1ULL << i)) ? "│   " : "    ");
        
        printf(has_sibling ? "├── " : "└── ");
        printf("%-6d %-15.15s [%c] %8s %5.1f%% %5.1f%% %8s%s\n",
               curr->pinfo->pid, curr->pinfo->comm, curr->pinfo->state[0],
               make_human_readable_str(curr->pinfo->rss * 1024ULL, 1, 0), 
               curr->cpu_pcnt, curr->mem_pcnt, curr->time_str, CLR_EOL);

        if (curr->base.child) {
            uint64_t next_mask = mask;
            if (has_sibling) next_mask |= (1ULL << (indent % 64));
            else next_mask &= ~(1ULL << (indent % 64));

            print_tree_rich((proc_node_t*)curr->base.child, indent + 1, next_mask);
        }
        curr = (proc_node_t*)curr->base.sibling;
    }
}

static proc_node_t* fetch_proc_list(void) {
    proc_node_t *list = NULL;
    procps_status_t *p = NULL;
    int flags = PSSCAN_PID | PSSCAN_PPID | PSSCAN_COMM | PSSCAN_RSS | 
                PSSCAN_VSZ | PSSCAN_UTIME | PSSCAN_STIME | PSSCAN_STATE |
                PSSCAN_NICE | PSSCAN_UIDGID | PSSCAN_TASKS;

    while ((p = procps_scan(p, flags)) != NULL) {
        if (G.target_pid > 0 && p->pid != G.target_pid) continue;

        proc_node_t *n = xzalloc(sizeof(*n));
        n->pinfo = xmalloc(sizeof(*p));
        memcpy(n->pinfo, p, sizeof(*p));
        n->base.next = (diag_node_base_t*)list;
        list = n;
    }
    return list;
}

static void free_proc_list(proc_node_t *head) {
    while (head) {
        proc_node_t *tmp = head;
        head = (proc_node_t*)head->base.next;
        free(tmp->pinfo);
        free(tmp);
    }
}

static void print_header(diag_sys_snap_t *snap) {
    if (G.batch_mode) return;

    printf(CLR_SCR);
    printf("%s[MY_PROC]%s Mode: %s%s%s | Sort: %s%s%s | Width: %d%s\n",
           COLOR_CYAN, COLOR_RESET, 
           COLOR_YELLOW, G.view_mode == VIEW_TREE ? "TREE" : "LIST", COLOR_RESET,
           COLOR_GREEN, get_sort_label(), COLOR_RESET, G.width, CLR_EOL);
    
    printf("Mem: %6s total, %6s free | Load: %.2f %.2f %.2f%s\n",
           make_human_readable_str(snap->total_mem_kb * 1024ULL, 1, 0), 
           make_human_readable_str(snap->free_mem_kb * 1024ULL, 1, 0), 
           snap->load_avg[0], snap->load_avg[1], snap->load_avg[2], CLR_EOL);
}

static bool handle_input(struct termios *old_t) {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    char c;

    if (G.batch_mode || poll(&pfd, 1, 0) <= 0) return true;
    if (read(STDIN_FILENO, &c, 1) <= 0) return true;
    c = toupper(c);
    if (c == 'Q') return false;
    if (c == 'T') {
        G.view_mode = (G.view_mode == VIEW_TREE) ? VIEW_TOP : VIEW_TREE;
        printf("\033[2J");
    } else if (strchr("PMIVOUSC", c)) {
        G.sort_mode = c;
    } else if (c == 'K') {
        int pid_to_kill = diag_ui_ask_int("Enter PID to kill (0 to cancel): ", old_t);
        if (pid_to_kill > 0) {
            if (kill(pid_to_kill, SIGTERM) == 0) sleep(1);
            else bb_perror_msg("kill failed");
        }
    }
    return true;
}

static void prepare_display_data(proc_node_t *head, proc_node_t **prev_arr, int prev_cnt, unsigned long long diff, unsigned long total_mem) {
    for (proc_node_t *n = head; n; n = (proc_node_t*)n->base.next) {
        n->cpu_pcnt = 0.0;
        if (prev_arr && diff > 0) {
            proc_node_t *p = find_node_by_pid(prev_arr, prev_cnt, n->pinfo->pid);
            if (p) {
                unsigned long ticks = (n->pinfo->utime + n->pinfo->stime) - (p->pinfo->utime + p->pinfo->stime);
                n->cpu_pcnt = (double)ticks * 100.0 / diff;
            }
        }
        n->mem_pcnt = (total_mem > 0) ? (n->pinfo->rss * 100.0 / total_mem) : 0.0;
        diag_format_time(n->time_str, n->pinfo->utime, n->pinfo->stime);
    }
}

static void display_tree(proc_node_t *list, proc_node_t **sorted_arr, int cnt) {

    for (int i = 0; i < cnt; i++) {
        sorted_arr[i]->base.id = sorted_arr[i]->pinfo->pid;
        sorted_arr[i]->base.parent_id = sorted_arr[i]->pinfo->ppid;
        sorted_arr[i]->base.child = NULL;
        sorted_arr[i]->base.sibling = NULL;
    }

    diag_node_base_t *root_list = diag_link_tree((diag_node_base_t**)sorted_arr, cnt);

    printf("%-7s %-18s %-4s %9s %6s %6s %8s%s\n", "PID", "COMMAND", "STAT", "RSS", "CPU%", "MEM%", "TIME", CLR_EOL);
    if (!G.batch_mode) printf("----------------------------------------------------------------------%s\n", CLR_EOL);
    print_tree_rich((proc_node_t*)root_list, 0, 0);
}

static void display_list(proc_node_t *list, proc_node_t **sorted_arr, int cnt) {

    qsort(sorted_arr, cnt, sizeof(proc_node_t*), sort_func);
    int fixed_width = 6+1 + 6+1 + 4+1 + 10+1 + 4+1 + 4+1 + 10+1 + 10+1 + 6+1 + 6+1 + 8+1;
    int comm_width = G.batch_mode ? 0 : (G.width - fixed_width);
    if (!G.batch_mode && comm_width < 10) comm_width = 10;
    if (G.batch_mode) {
        printf("PID    PPID   THR  USER       STAT NI   VSZ        RSS        %%CPU   %%MEM   TIME     COMMAND\n");
    } else {
        printf("%-6s %-6s %-4s %-10s %-4s %-4s %-10s %-10s %-6s %-6s %-8s %-*.*s%s\n",
               "PID", "PPID", "THR", "USER", "STAT", "NI", "VSZ", "RSS", "%CPU", "%MEM", "TIME", comm_width, comm_width, "COMMAND", CLR_EOL);
    }

    for (int i = 0; i < cnt; i++) {

        if (!G.batch_mode && i >= (G.height - 6)) break;
        proc_node_t *cn = sorted_arr[i];
        const char *user = uid2uname(cn->pinfo->uid);
        printf("%-6d %-6d %-4d %-10.10s %-4c %-4d %-10s %-10s %-6.1f %-6.1f %-8s %-*.*s%s\n",
               cn->pinfo->pid, cn->pinfo->ppid, 0, user,
               cn->pinfo->state[0], cn->pinfo->niceness, 
               make_human_readable_str(cn->pinfo->vsz * 1024ULL, 1, 0), 
               make_human_readable_str(cn->pinfo->rss * 1024ULL, 1, 0), 
               cn->cpu_pcnt, cn->mem_pcnt,
               cn->time_str, comm_width, comm_width, cn->pinfo->comm, CLR_EOL);
    }
}

static void show_top_with_cpu(void) {

    diag_sys_snap_t snap;
    proc_node_t *prev_list = NULL;
    proc_node_t **prev_sort_arr = NULL;
    int prev_cnt = 0;
    unsigned long long prev_ticks = 0;
    struct termios old_t;

    if (!isatty(STDOUT_FILENO)) G.batch_mode = true;
    if (!G.batch_mode) {
        diag_ui_mode_raw(&old_t);
    }

    while (G.iterations != 0) {
        
        update_term_size();
        diag_get_sys_snap(&snap);
        unsigned long long curr_ticks = snap.cpu_total_ticks;
        unsigned long long diff = (prev_ticks > 0) ? (curr_ticks - prev_ticks) : 0;
        proc_node_t *curr_list = fetch_proc_list();
        int curr_cnt = 0;
        proc_node_t **curr_sort_arr = (proc_node_t**)diag_nodes_to_array((diag_node_base_t*)curr_list, &curr_cnt);

        if (curr_sort_arr && prev_sort_arr && diff > 0) {
            qsort(curr_sort_arr, curr_cnt, sizeof(proc_node_t*), sort_by_pid);
        }

        if (!handle_input(&old_t)) {
            if (curr_list) free_proc_list(curr_list);
            free(curr_sort_arr);
            break;
        }

        prepare_display_data(curr_list, prev_sort_arr, prev_cnt, diff, snap.total_mem_kb);
        print_header(&snap); 
        if (G.view_mode == VIEW_TREE) {
            display_tree(curr_list, curr_sort_arr, curr_cnt);
        } else {
            display_list(curr_list, curr_sort_arr, curr_cnt);
        }

        if (!G.batch_mode) {
            printf("\033[J\n\e[7m SORT: (P)CPU (M)RSS (V)VSZ (I)PID (O)PPID (U)USER (S)STAT (C)CMD | (T)TREE (K)KILL (Q)QUIT \e[0m%s", CLR_EOL);
            fflush(stdout);
        }

        if (prev_list) free_proc_list(prev_list);
        if (prev_sort_arr) free(prev_sort_arr);
        
        prev_list = curr_list;
        prev_sort_arr = curr_sort_arr;
        prev_cnt = curr_cnt;
        prev_ticks = curr_ticks;

        if (G.iterations > 0) G.iterations--;
        if (G.iterations == 0) break;
        
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        poll(&pfd, 1, (int)(G.delay * 1000));
    }

    if (!G.batch_mode) {
        diag_ui_mode_normal(&old_t);
        // 退出時清空最後一行提示並確保游標換行，避免干擾 Shell 提示字元
        printf("\n");
        fflush(stdout);
    }
}

int my_proc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_proc_main(int argc, char **argv)
{
    char *delay_str = NULL;
    int iterations = -1;
    int pid = -1;
    int opts;

    G.delay = 1.0; 
    G.sort_mode = 'P';
    
    opts = getopt32(argv, "td:n:+p:+b", &delay_str, &iterations, &pid);

    if (delay_str) G.delay = atof(delay_str);
    if (opts & 4) G.iterations = iterations;
    if (opts & 8) G.target_pid = pid;
    if (opts & 16) G.batch_mode = true;
    if (opts & 1) G.view_mode = VIEW_TREE; // 如果有 -t，預設進入樹狀模式

    // 移除原本在這裡直接 return 的邏輯，讓所有模式都進入主循環
    show_top_with_cpu();
    return EXIT_SUCCESS;
}
