/* vi: set sw=4 ts=4: */
//config:config MY_PROC
//config:	bool "my_proc (Resource Analyzer)"
//config:	default y
//config:	help
//config:	  Simple process resource analyzer with tree view support.

//applet:IF_MY_PROC(APPLET(my_proc, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_PROC) += diag_proc.o
//kbuild:lib-$(CONFIG_MY_PROC) += libdiag.o

//usage:#define my_proc_trivial_usage "[-t]"
//usage:#define my_proc_full_usage "\n\n"
//usage:       "Analyze process resources\n"
//usage:     "\n	-t	Show process tree"

#include "libbb.h"
#include "libdiag.h"

// 遞迴列印行程樹
static void print_tree(int target_ppid, int indent) {
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/proc");
    if (!dir) return;

    while ((entry = readdir(dir)) != NULL) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        diag_proc_t info;
        if (diag_read_proc(pid, &info) == 0) {
            if (info.ppid == target_ppid) {
                // 根據深度縮排
                for (int i = 0; i < indent; i++) printf("  ");
                printf("|- %d: %s (RSS: %lu KB)\n", info.pid, info.comm, info.rss);
                
                // 繼續尋找子行程
                print_tree(info.pid, indent + 1);
            }
        }
    }
    closedir(dir);
}

// 簡易版 top 模式
static void show_top(void) {
    printf("\033[H\033[J"); // 清除螢幕
    printf("%-8s %-8s %-15s %-10s %-10s\n", "PID", "PPID", "NAME", "VmSize", "RSS");
    printf("------------------------------------------------------------\n");

    DIR *dir = opendir("/proc");
    struct dirent *entry;
    if (!dir) return;

    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 20) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        diag_proc_t info;
        if (diag_read_proc(pid, &info) == 0) {
            printf("%-8d %-8d %-15s %-10lu %-10lu\n", 
                info.pid, info.ppid, info.comm, info.vmsize, info.rss);
            count++;
        }
    }
    closedir(dir);
}

int my_proc_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_proc_main(int argc, char **argv)
{
    unsigned opts;
    opts = getopt32(argv, "t");

    if (opts & 1) { // -t 參數：顯示樹狀圖
        printf("Process Tree (starting from PID 1):\n");
        print_tree(1, 0);
    } else {
        // 預設執行一次性掃描（若要循環可加入 while(1) 與 sleep）
        show_top();
    }

    return EXIT_SUCCESS;
}