/* vi: set sw=4 ts=4: */
//config:config MY_PROC
//config: 	bool "my_proc (Resource Analyzer)"
//config: 	default y
//config: 	help
//config: 	  Simple process resource analyzer with tree view support.

//applet:IF_MY_PROC(APPLET(my_proc, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_PROC) += diag_proc.o
//kbuild:lib-$(CONFIG_MY_PROC) += libdiag.o

//usage:#define my_proc_trivial_usage "[-ts]"
//usage:#define my_proc_full_usage "\n\n"
//usage:	  "Analyze process resources\n"
//usage:	"\n	   -t	Show process tree"
//usage:	"\n	   -s	Show real-time CPU usage (top mode)"

#include "libbb.h"
#include "libdiag.h"
#include <sys/sysinfo.h>
#include <pwd.h>
#include <termios.h>
#include <ctype.h>
#define SAFE_COMPARE(a, b) (((a) > (b)) - ((a) < (b)))

typedef struct proc_node {
    diag_proc_t info;
    struct proc_node *next;
} proc_node_t;

typedef enum { VIEW_TOP, VIEW_TREE } view_mode_t;

static char g_sort_mode = 'P'; 
static view_mode_t g_view_mode = VIEW_TOP;

static const char* get_sort_label(void) {
    switch (g_sort_mode) {
        case 'P': return "CPU%"; case 'M': return "RSS";
        case 'V': return "VSZ";  case 'I': return "PID";
        case 'O': return "PPID"; case 'U': return "USER";
        case 'S': return "STAT"; case 'C': return "COMMAND";
        default:  return "PID";
    }
}

static int sort_func(const void *a, const void *b) {
    diag_proc_t *pa = &(*(proc_node_t**)a)->info;
    diag_proc_t *pb = &(*(proc_node_t**)b)->info;
    int res = 0;

    switch (g_sort_mode) {
        /* 注意：這裡要改成大寫的 SAFE_COMPARE */
        case 'P': res = SAFE_COMPARE(pb->utime + pb->stime, pa->utime + pa->stime); break;
        case 'M': res = SAFE_COMPARE(pb->rss, pa->rss); break;
        case 'V': res = SAFE_COMPARE(pb->vmsize, pa->vmsize); break;
        case 'I': res = SAFE_COMPARE(pa->pid, pb->pid); break;
        case 'O': res = SAFE_COMPARE(pa->ppid, pb->ppid); break;
        case 'U': res = SAFE_COMPARE(pa->uid, pb->uid); break;
        case 'S': res = SAFE_COMPARE(pa->state, pb->state); break;
        case 'C': res = strcmp(pa->comm ? pa->comm : "", pb->comm ? pb->comm : ""); break;
    }
    return res ? res : SAFE_COMPARE(pa->pid, pb->pid);
}

static double calc_cpu(proc_node_t *curr, proc_node_t *prev_list, unsigned long long diff) {
    if (!prev_list || diff <= 0) return 0.0;
    for (proc_node_t *p = prev_list; p; p = p->next) {
        if (p->info.pid == curr->info.pid) {
            unsigned long ticks = (curr->info.utime + curr->info.stime) - (p->info.utime + p->info.stime);
            return (double)ticks * 100.0 / diff;
        }
    }
    return 0.0;
}

static void print_tree_rich(proc_node_t *head, proc_node_t *prev_list, unsigned long long diff, int ppid, int indent, unsigned long total_mem) {
    proc_node_t *curr = head;
    while (curr) {
        if (curr->info.ppid == ppid) {
            double cpu = calc_cpu(curr, prev_list, diff);
            double mem = (total_mem > 0) ? (curr->info.rss * 100.0 / total_mem) : 0.0;
            printf("%*s|- %-5d %-15s [%c] %8luK %5.1f%% %5.1f%%\033[K\n", 
                   indent * 2, "", curr->info.pid, curr->info.comm, curr->info.state,
                   curr->info.rss, cpu, mem);
            print_tree_rich(head, prev_list, diff, curr->info.pid, indent + 1, total_mem);
        }
        curr = curr->next;
    }
}

static proc_node_t* fetch_proc_list(void) {
    proc_node_t *list = NULL;
    DIR *dir = xopendir("/proc");
    struct dirent *e;
    while ((e = readdir(dir))) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        proc_node_t *n = xzalloc(sizeof(*n));
        if (diag_read_proc(pid, &n->info) == 0) {
            n->next = list;
            list = n;
        } else free(n);
    }
    closedir(dir);
    return list;
}

static void free_proc_list(proc_node_t *head) {
    while (head) {
        proc_node_t *tmp = head;
        head = head->next;
        // 注意：如果 diag_proc_t 內部有動態分配的字串（如 pa->comm），
        // 記得也要在這裡 free(tmp->info.comm)，但目前 BusyBox 實作通常是固定陣列。
        free(tmp);
    }
}

static const char* get_time_str(unsigned long long start_ticks) {
    static char buf[16];
    static long hz = 0;
    if (hz == 0) hz = sysconf(_SC_CLK_TCK);

    struct sysinfo si;
    sysinfo(&si);

    // 計算自啟動以來的總秒數
    unsigned long total_sec = si.uptime - (start_ticks / hz);
    
    if (total_sec < 3600) {
        // 不滿一小時顯示 分:秒
        snprintf(buf, sizeof(buf), "%02lu:%02lu", total_sec / 60, total_sec % 60);
    } else {
        // 超過一小時顯示 小時h分鐘m
        snprintf(buf, sizeof(buf), "%2luh%02lu", total_sec / 3600, (total_sec / 60) % 60);
    }
    return buf;
}

static void show_top_with_cpu(void) {
    proc_node_t *prev_list = NULL;
    unsigned long long prev_ticks = 0;
    struct termios old_t;
    struct sysinfo si;
    sysinfo(&si);
    unsigned long total_mem = (si.totalram * si.mem_unit) / 1024;

    set_termios_to_raw(STDIN_FILENO, &old_t, 0);
    printf("\033[2J\033[?25l");

    while (1) {
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        proc_node_t *curr_list = fetch_proc_list();
        unsigned long long curr_ticks = 0;
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            unsigned long long u, n, s, i, io, ir, sir, st;
            if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u,&n,&s,&i,&io,&ir,&sir,&st) == 8)
                curr_ticks = u+n+s+i+io+ir+sir+st;
            fclose(f);
        }
        unsigned long long diff = curr_ticks - prev_ticks;

        if (poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(0, &c, 1) > 0) {
                c = toupper(c);
                if (c == 'Q') { free_proc_list(curr_list); break; }
                if (c == 'T') { g_view_mode = !g_view_mode; printf("\033[2J"); }
                if (strchr("PMIVOUSC", c)) g_sort_mode = c;
                if (c == 'K') {
                    // 1. 暫時恢復原本的終端機設定，並顯示游標
                    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
                    printf("\033[?25h\033[H\033[J"); // 移動到頂部並清空，方便輸入
                    
                    printf("\e[1;31m[KILL PROCESS]\e[0m\n");
                    printf("Enter PID to kill (or 0 to cancel): ");
                    
                    char buf[16];
                    fflush(stdout);
                    if (fgets(buf, sizeof(buf), stdin)) {
                        int pid_to_kill = atoi(buf);
                        if (pid_to_kill > 0) {
                            if (kill(pid_to_kill, SIGTERM) == 0) {
                                printf("Sent SIGTERM to PID %d\n", pid_to_kill);
                            } else {
                                printf("Kill failed: %s\n", strerror(errno));
                            }
                            sleep(1); // 讓使用者看一下結果
                        }
                    }

                    // 2. 切回 Raw mode 並再次隱藏游標
                    set_termios_to_raw(STDIN_FILENO, &old_t, 0);
                    printf("\033[?25l\033[2J");
                }
            }
        }

        sysinfo(&si);
        printf("\033[H\e[1;36m[MY_PROC]\e[0m Mode: \e[1;33m%s\e[0m | Sort By: \e[1;32m%s\e[0m\033[K\n", 
               g_view_mode ? "TREE" : "LIST", get_sort_label());
        printf("Mem: %luK total, %luK free | Load: %.2f\033[K\n", 
               total_mem, (si.freeram * si.mem_unit)/1024, si.loads[0]/65536.0);

        if (g_view_mode == VIEW_TREE) {
            printf("\n%-7s %-15s %-4s %9s %6s %6s\033[K\n", "PID", "COMMAND", "STAT", "RSS", "CPU%", "MEM%");
            printf("------------------------------------------------------------\033[K\n");
            print_tree_rich(curr_list, prev_list, diff, 0, 0, total_mem);
        } else {
            int cnt = 0;
            for (proc_node_t *n = curr_list; n; n = n->next) cnt++;
            proc_node_t **arr = xzalloc(sizeof(void*) * cnt);
            cnt = 0;
            for (proc_node_t *n = curr_list; n; n = n->next) arr[cnt++] = n;
            qsort(arr, cnt, sizeof(void*), sort_func);


            // 修改後的 Header，將 PPID 放在 PID 之後
            printf("\n%-6s %-6s %-4s %-10s %-4s %-4s %-10s %-6s %-6s %-8s %-15s\033[K\n", 
                "PID", "PPID", "THR", "USER", "STAT", "NI", "RSS", "%CPU", "%MEM", "TIME", "COMMAND");
            printf("----------------------------------------------------------------------------------------------------\033[K\n");

            for (int i = 0; i < cnt && i < 28; i++) {
                proc_node_t *cn = arr[i];
                struct passwd *pw = getpwuid(cn->info.uid);
                double cp = calc_cpu(cn, prev_list, diff);
                double me = (total_mem > 0) ? (cn->info.rss * 100.0 / total_mem) : 0.0;
                
                // 渲染每一行數據
                printf("%-6d %-6d %-4d %-10.10s %-4c %-4d %-10lu %-6.1f %-6.1f %-8s %-15s\033[K\n", 
                    cn->info.pid,
                    cn->info.ppid,          // 新增 PPID
                    cn->info.threads, 
                    pw ? pw->pw_name : "???",
                    cn->info.state,
                    cn->info.nice,
                    cn->info.rss,
                    cp,
                    me,
                    get_time_str(cn->info.start_time), // 這是你之前算的行程存活時間
                    cn->info.comm);
            }

            /*printf("\n%-6s %-6s %-10s %-4s %-8s %-8s %-5s %-5s %-15s\033[K\n", 
                   "PID", "PPID", "USER", "STAT", "VSZ", "RSS", "%CPU", "%MEM", "COMMAND");
            printf("--------------------------------------------------------------------------------\033[K\n");
            for (int i = 0; i < cnt && i < 28; i++) {
                proc_node_t *cn = arr[i];
                struct passwd *pw = getpwuid(cn->info.uid);
                printf("%-6d %-6d %-10s %-4c %-8lu %-8lu %-5.1f %-5.1f %-15s\033[K\n", 
                       cn->info.pid, cn->info.ppid, pw ? pw->pw_name : "???", 
                       cn->info.state, cn->info.vmsize, cn->info.rss, calc_cpu(cn, prev_list, diff), 
                       (total_mem > 0) ? (cn->info.rss * 100.0 / total_mem) : 0.0, cn->info.comm);
            }*/
            free(arr);
        }
        printf("\033[J\n\e[7m SORT: (P)CPU (M)RSS (V)VSZ (I)PID (O)PPID (U)USER (S)STAT (C)CMD | (T)TREE (K)KILL (Q)QUIT \e[0m\033[K");
        fflush(stdout);

        if (prev_list) free_proc_list(prev_list);
        prev_list = curr_list;
        prev_ticks = curr_ticks;
        poll(&pfd, 1, 1000);
    }
    printf("\033[?25h\033[2J\033[H");
    tcsetattr(0, TCSANOW, &old_t);
}



/* 進入點：處理參數與預設行為 */
int my_proc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_proc_main(int argc, char **argv)
{
    unsigned opts;
    // 禁用 stdout 快取以獲得即時流暢感
    setvbuf(stdout, NULL, _IONBF, 0);
    
    // 解析參數：-t (Tree), -s (Top/Stats)
    opts = getopt32(argv, "ts");

    if (opts & 1) { // -t: 單次樹狀快照
        proc_node_t *list = fetch_proc_list();
        printf("Process Tree Snapshot:\n");
        print_tree_rich(list, NULL, 0, 0, 0, 0); // 靜態快照不計 CPU/MEM%
        free_proc_list(list);
        return EXIT_SUCCESS;
    }

    // 預設或 -s: 進入即時互動模式
    show_top_with_cpu();

    return EXIT_SUCCESS;
}