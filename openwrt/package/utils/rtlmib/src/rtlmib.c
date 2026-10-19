// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtlmib - замена "iwpriv wlanX set_mib/get_mib" для вендорского драйвера
 * rtl8192cd (openwrt-rx23302).
 *
 *   rtlmib wlan0 set channel=6
 *   rtlmib wlan0 get channel        (печатает сырые байты ответа в hex)
 *
 * Драйвер принимает строку "имя=значение" через SIOCDEVPRIVATE+1 (set) и
 * имя через SIOCDEVPRIVATE+2 (get), указатель и длина - в iwreq.u.data
 * (8192cd_ioctl.c: RTL8192CD_IOCTL_SET_MIB/GET_MIB).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

struct rtl_point { void *pointer; unsigned short length; unsigned short flags; };
struct rtl_iwreq {
	char name[IFNAMSIZ];
	union { struct rtl_point data; char pad[16]; } u;
};

#define RTL_SET_MIB (SIOCDEVPRIVATE + 0x1)
#define RTL_GET_MIB (SIOCDEVPRIVATE + 0x2)

int main(int argc, char **argv)
{
	struct rtl_iwreq wrq;
	char buf[1024];
	int fd, rc, i, set;

	if (argc != 4 || (strcmp(argv[2], "set") && strcmp(argv[2], "get"))) {
		fprintf(stderr, "usage: %s IFACE set NAME=VALUE | get NAME\n", argv[0]);
		return 2;
	}
	set = !strcmp(argv[2], "set");
	memset(&wrq, 0, sizeof(wrq));
	memset(buf, 0, sizeof(buf));
	strncpy(wrq.name, argv[1], IFNAMSIZ - 1);
	strncpy(buf, argv[3], sizeof(buf) - 1);
	wrq.u.data.pointer = buf;
	wrq.u.data.length = strlen(buf) + 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }
	rc = ioctl(fd, set ? RTL_SET_MIB : RTL_GET_MIB, &wrq);
	if (rc < 0) { perror(set ? "set_mib" : "get_mib"); close(fd); return 1; }
	if (!set) {
		/* ядро 6.x не копирует ifreq обратно для SIOCDEVPRIVATE (need_copyout
		 * = false), поэтому длину ответа не узнать - печатаем первые 32 байта */
		for (i = 0; i < 32; i++)
			printf("%02x%s", (unsigned char)buf[i], (i + 1) % 16 ? " " : "\n");
		printf("\n");
	}
	close(fd);
	return 0;
}
