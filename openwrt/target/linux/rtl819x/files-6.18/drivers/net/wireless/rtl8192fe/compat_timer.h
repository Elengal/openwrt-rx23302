/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Переходник для таймеров и мелочей, переименованных в ядре.
 *
 * del_timer и del_timer_sync убраны в ядре 6.15, вместо них timer_delete и
 * timer_delete_sync. Смысл тот же, отличается только имя, поэтому здесь
 * достаточно отображения: в драйвере таких мест около ста восьмидесяти.
 *
 * А вот init_timer отображением не закрыть: вместе с ней ядро 4.15 убрало у
 * таймера поле data и сменило подпись обработчика. Восстановить данные из
 * указателя на таймер можно только зная, в какую структуру он вложен, — и это
 * в каждом месте своё. Такие места переводятся по отдельности.
 */

#ifndef _RTL8192CD_COMPAT_TIMER_H
#define _RTL8192CD_COMPAT_TIMER_H

#include <linux/timer.h>
#include <linux/netdevice.h>

#ifndef del_timer_sync
#define del_timer_sync(t)	timer_delete_sync(t)
#endif
#ifndef del_timer
#define del_timer(t)		timer_delete(t)
#endif

/*
 * ioremap_nocache убрана в ядре 5.6: обычный ioremap и так даёт
 * некэшируемое отображение, отдельное имя стало лишним.
 */
#include <linux/io.h>
#ifndef ioremap_nocache
#define ioremap_nocache(a, s)	ioremap((a), (s))
#endif

/*
 * from_timer убран в ядре 6.16, теперь это timer_container_of.
 * Смысл тот же: получить объемлющую структуру по адресу таймера,
 * зная имя поля. Переименование, а не смена поведения.
 */
#ifndef from_timer
#define from_timer(var, ct, fld) timer_container_of(var, ct, fld)
#endif

#endif /* _RTL8192CD_COMPAT_TIMER_H */
