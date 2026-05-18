/* vi: set sw=4 ts=4: */
// clang-format off
//config:config MY_NET
//config:   bool "my_net"
//config:   default y
//config:   help
//config:     Simple hello world test.

//applet:IF_MY_NET(APPLET(my_net, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MY_NET) += diag_net.o

//usage:#define my_net_trivial_usage "None"
//usage:#define my_net_full_usage "None"
// clang-format on

#include "libbb.h"
#include "libdiag.h"

/* 函數名必須與 APPLET 的第一個參數一致，並加上 _main */
int my_net_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int my_net_main(int argc, char **argv)
{
	printf("Hello, BusyBox Custom Folder!\n");
	return EXIT_SUCCESS;
}