/* vi: set sw=4 ts=4: */
//config:config MY_PROC
//config:	bool "my_proc (Resource Analyzer)"
//config:	default y
//config:	help
//config:	  Simple process resource analyzer with tree view support.

//applet:IF_MY_PROC(APPLET(my_proc, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_PROC) += diag_proc.o
//kbuild:lib-$(CONFIG_MY_PROC) += libdiag.o

//usage:#define my_proc_trivial_usage "[-ts]"
//usage:#define my_proc_full_usage "\n\n"
//usage:       "Analyze process resources\n"
//usage:     "\n	-t	Show process tree"
//usage:     "\n	-s	Show real-time CPU usage (top mode)"

#include "libbb.h"
#include "libdiag.h"
#include <sys/sysinfo.h>
#include <pwd.h>

// 建立一個節點結構來存儲所有行程，避免重複掃描 /proc
typedef struct proc_node {
    diag_proc_t info;
    struct proc_node *next;
} proc_node_t;

// 獲取總記憶體（KB）
static unsigned long get_total_mem(void) {
    struct sysinfo info;
    if (sysinfo(&info) == 0) return (info.totalram * info.mem_unit) / 1024;
    return 0;
}

// 獲取 Uptime 字串
static char* get_uptime_str(void) {
    struct sysinfo info;
    sysinfo(&info);
    int h = info.uptime / 3600;
    int m = (info.uptime % 3600) / 60;
    return xasprintf("%dh %dm", h, m);
}

// 1. 職責拆分：只負責建立資料清單
static proc_node_t* get_proc_list(void) {
    DIR *dir = xopendir("/proc");
    struct dirent *entry;
    proc_node_t *head = NULL;

    while ((entry = readdir(dir))) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        proc_node_t *new_node = xzalloc(sizeof(proc_node_t));
        if (diag_read_proc(pid, &new_node->info) == 0) {
            new_node->next = head;
            head = new_node;
        } else {
            free(new_node);
        }
    }
    closedir(dir);
    return head;
}

// 2. 職責拆分：印出原本的 top 列表
static void print_list(proc_node_t *head) {
    printf("%-8s %-8s %-15s %-10s %-10s\n", "PID", "PPID", "NAME", "VmSize", "RSS");
    printf("------------------------------------------------------------\n");
    proc_node_t *curr = head;
    while (curr) {
        printf("%-8d %-8d %-15s %-10lu %-10lu\n", 
            curr->info.pid, curr->info.ppid, curr->info.comm, 
            curr->info.vmsize, curr->info.rss);
        curr = curr->next;
    }
}

// 3. 職責拆分：專門負責記憶體釋放
static void free_proc_list(proc_node_t *head) {
    while (head) {
        proc_node_t *tmp = head;
        head = head->next;
        free(tmp);
    }
}

// 修改後的樹狀列印：從記憶體 List 找子節點，而非從磁碟
static void print_tree_fast(proc_node_t *head, int target_ppid, int indent) {
    proc_node_t *curr = head;
    while (curr) {
        if (curr->info.ppid == target_ppid) {
            printf("%*s|- %d: %s (RSS: %lu KB)\n", 
                   indent * 2, "", curr->info.pid, curr->info.comm, curr->info.rss);
            print_tree_fast(head, curr->info.pid, indent + 1);
        }
        curr = curr->next;
    }
}

// 輔助函數：取得系統總消耗時間 (jiffies) 自 /proc/stat
static unsigned long long get_system_total_ticks(void) {
    char buf[256];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    if (fgets(buf, sizeof(buf), f)) {
        sscanf(buf, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    }
    fclose(f);
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

// 增加排序需要的比較函數
static int sort_by_mem(const void *a, const void *b) {
    return (int)((*(proc_node_t **)b)->info.rss - (*(proc_node_t **)a)->info.rss);
}

static int sort_by_cpu(const void *a, const void *b) {
    unsigned long long cpu_a = (*(proc_node_t **)a)->info.utime + (*(proc_node_t **)a)->info.stime;
    unsigned long long cpu_b = (*(proc_node_t **)b)->info.utime + (*(proc_node_t **)b)->info.stime;
    // 這裡用 prev 的差值排序會更準確，但結構受限時先以總量排或傳入 diff 資料
    return (cpu_b > cpu_a) - (cpu_b < cpu_a);
}

static void show_top_with_cpu(void) {
    proc_node_t *prev_list = get_proc_list();
    unsigned long long prev_total_ticks = get_system_total_ticks();
    unsigned long total_mem = get_total_mem();
    struct termios old_termios;
    char sort_mode = 'P'; 
    
    set_termios_to_raw(STDIN_FILENO, &old_termios, 0);

    while (1) {
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        proc_node_t *curr_list;
        unsigned long long curr_total_ticks;
        unsigned long long system_diff;
        struct sysinfo s_info;
        proc_node_t **sort_array;
        int count = 0, idx = 0;
        char *uptime;

        if (poll(&pfd, 1, 0) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                c = toupper(c);
                if (c == 'Q') break;
                if (c == 'M' || c == 'P') sort_mode = c;
                if (c == 'T') { // 進入臨時樹狀模式
                    printf("\033[H\033[J"); // 清屏
                    printf("Process Tree View (Current Snap):\n");
                    printf("----------------------------------\n");
                    print_tree_fast(curr_list, 0, 0);
                    printf("\nPress any key to return to Top mode...");
                    
                    // 等待使用者按鍵
                    poll(&pfd, 1, -1); 
                    read(STDIN_FILENO, &c, 1);
                    continue; // 繼續 Top 刷新
                }
                if (c == 'K') {
                    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
                    printf("\nEnter PID to kill: ");
                    char buf[16];
                    if (fgets(buf, sizeof(buf), stdin)) {
                        int pid_to_kill = atoi(buf);
                        if (pid_to_kill > 0) kill(pid_to_kill, SIGTERM);
                    }
                    set_termios_to_raw(STDIN_FILENO, &old_termios, 0);
                }
            }
        }

        curr_list = get_proc_list();
        curr_total_ticks = get_system_total_ticks();
        system_diff = curr_total_ticks - prev_total_ticks;
        sysinfo(&s_info);
        
        for (proc_node_t *n = curr_list; n; n = n->next) count++;
        sort_array = xmalloc(sizeof(proc_node_t *) * count);
        for (proc_node_t *n = curr_list; n; n = n->next) sort_array[idx++] = n;

        qsort(sort_array, count, sizeof(proc_node_t *), (sort_mode == 'M') ? sort_by_mem : sort_by_cpu);

        uptime = get_uptime_str();
        printf("\033[H\033[J");
        // 修正 Load Average 顯示
        printf("Uptime: %s | Load: %.2f, %.2f, %.2f\n", uptime, 
               s_info.loads[0]/65536.0, s_info.loads[1]/65536.0, s_info.loads[2]/65536.0);
        printf("Mem: %luK total, %luK free | Sort: [%s]\n", total_mem, (s_info.freeram * s_info.mem_unit)/1024, (sort_mode == 'P' ? "CPU" : "MEM"));
        free(uptime);

        // 整合 print_list 的資訊：加入 VSZ
        printf("\n%-6s %-10s %-4s %-8s %-8s %-6s %-6s %-15s\n", 
               "PID", "USER", "STAT", "VSZ", "RSS", "%CPU", "%MEM", "COMMAND");
        printf("----------------------------------------------------------------------\n");

        for (int i = 0; i < count && i < 20; i++) {
            proc_node_t *curr = sort_array[i];
            double cpu_pcnt = 0.0;
            double mem_pcnt = (total_mem > 0) ? ((double)curr->info.rss * 100.0 / total_mem) : 0.0;
            struct passwd *pw = getpwuid(curr->info.uid);
            const char *user = pw ? pw->pw_name : "unknown";

            if (prev_list && system_diff > 0) {
                // ...現有的 CPU 計算邏輯...
                for (proc_node_t *p = prev_list; p; p = p->next) {
                    if (p->info.pid == curr->info.pid) {
                        unsigned long diff = (curr->info.utime + curr->info.stime) - (p->info.utime + p->info.stime);
                        cpu_pcnt = (double)diff * 100.0 / system_diff;
                        break;
                    }
                }
            }

            // 輸出包含 VmSize (VSZ)
            printf("%-6d %-10s %-4c %-8lu %-8lu %-6.1f %-6.1f %-15s\n", 
                   curr->info.pid, user, curr->info.state, 
                   curr->info.vmsize, curr->info.rss,
                   cpu_pcnt, mem_pcnt, curr->info.comm);
        }

        printf("\n(Q:Quit, M:Sort Mem, P:Sort CPU, T:Tree, K:Kill)\n");

        free(sort_array);
        if (prev_list) free_proc_list(prev_list);
        prev_list = curr_list;
        prev_total_ticks = curr_total_ticks;
        poll(&pfd, 1, 1000); 
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
}

int my_proc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_proc_main(int argc, char **argv)
{
    unsigned opts = getopt32(argv, "ts");
    
    // 如果沒有參數，或者有 -s 參數，預設進入 Top 模式
    if (argc == 1 || (opts & 2)) {
        show_top_with_cpu();
        return EXIT_SUCCESS;
    }

    // 處理 -t 參數 (Tree View)
    if (opts & 1) {
        proc_node_t *head = get_proc_list();
        printf("Process Tree:\n");
        print_tree_fast(head, 0, 0);
        free_proc_list(head);
        return EXIT_SUCCESS;
    }

    // 其他情況（雖然在此邏輯下不太會發生）顯示一般列表
    proc_node_t *head = get_proc_list();
    print_list(head);
    free_proc_list(head);

    return EXIT_SUCCESS;
}

// 建議優化 show_top_with_cpu 內的搜尋邏輯
// 可以增加一個簡單的快取或至少在 my_proc_main 中呼叫它