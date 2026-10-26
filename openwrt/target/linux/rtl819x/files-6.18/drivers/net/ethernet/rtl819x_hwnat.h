/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Аппаратный NAT коммутатора RTL8197F - скелет, см. docs/hwnat-plan.md.
 * Собирается только с CONFIG_NET_RTL819X_HWNAT; без неё драйвер Ethernet
 * получает пустые заглушки и не меняется.
 */

#ifndef _RTL819X_HWNAT_H
#define _RTL819X_HWNAT_H

#include <linux/netdevice.h>

#if IS_ENABLED(CONFIG_NET_RTL819X_HWNAT)

int rtl819x_hwnat_init(struct net_device *dev);
void rtl819x_hwnat_exit(struct net_device *dev);
int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data);

#else

static inline int rtl819x_hwnat_init(struct net_device *dev)
{
	return 0;
}

static inline void rtl819x_hwnat_exit(struct net_device *dev)
{
}

#endif

#endif /* _RTL819X_HWNAT_H */
