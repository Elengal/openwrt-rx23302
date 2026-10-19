/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Старые таймеры ядра (до 4.15) поверх новых - одним переходником на весь
 * драйвер. Для rtl8192fe 3.8.0 на ядре 6.18.
 *
 * Вендорский код живёт по старым правилам:
 *
 *     init_timer(&t);
 *     t.data     = (unsigned long)priv;
 *     t.function = handler;          // void handler(unsigned long data)
 *     mod_timer(&t, jiffies + x);
 *
 * В 4.15 у struct timer_list отняли поле data, а обработчик стал получать
 * указатель на сам таймер. В rtl8192cd это было 62
 * места, переписанных руками. Здесь 100 вызовов init_timer и больше 700
 * вызовов mod_timer/timer_pending/del_timer, поэтому иначе:
 *
 *   1. В исходниках драйвера (и только там - правка sed, см. git) тип
 *      "struct timer_list" заменён на struct rtl_legacy_timer: внутри
 *      настоящий таймер ядра плюс старые поля data, function, expires.
 *      Заголовки ядра НЕ трогаем: их struct timer_list (например внутри
 *      struct delayed_work) остаётся родной - раскладка не плывёт.
 *   2. init_timer() ставит общий обработчик-прокладку rtl_lt_fn: тот
 *      достаёт обёртку из таймера ядра и зовёт function(data), как
 *      старое ядро. data и function вендор пишет после init_timer -
 *      прокладка читает их в момент срабатывания.
 *   3. mod_timer/add_timer/timer_pending/del_timer[_sync] выбирают путь
 *      по типу аргумента (_Generic): обёртка - к нам, всё остальное -
 *      к настоящим функциям ядра.
 */
#ifndef _RTL8192CD_COMPAT_LEGACY_TIMER_H
#define _RTL8192CD_COMPAT_LEGACY_TIMER_H

#include <linux/timer.h>
#include <linux/jiffies.h>

struct rtl_legacy_timer {
	struct timer_list	kt;		/* настоящий таймер ядра */
	void			(*function)(unsigned long);
	unsigned long		data;
	unsigned long		expires;	/* вендор местами пишет и читает сам */
};

static inline void rtl_lt_fn(struct timer_list *kt)
{
	struct rtl_legacy_timer *t = container_of(kt, struct rtl_legacy_timer, kt);

	if (t->function)
		t->function(t->data);
}

static inline void rtl_lt_init(struct rtl_legacy_timer *t)
{
	t->function = NULL;
	t->data = 0;
	t->expires = 0;
	timer_setup(&t->kt, rtl_lt_fn, 0);
}

static inline int rtl_lt_mod(struct rtl_legacy_timer *t, unsigned long expires)
{
	t->expires = expires;
	return mod_timer(&t->kt, expires);
}

static inline void rtl_lt_add(struct rtl_legacy_timer *t)
{
	/* старый add_timer брал срок из поля expires */
	t->kt.expires = t->expires;
	add_timer(&t->kt);
}

static inline int rtl_lt_pending(const struct rtl_legacy_timer *t)
{
	return timer_pending(&t->kt);
}

static inline int rtl_lt_del(struct rtl_legacy_timer *t)
{
	return timer_delete(&t->kt);
}

static inline int rtl_lt_del_sync(struct rtl_legacy_timer *t)
{
	return timer_delete_sync(&t->kt);
}

/* Обёртка - к нам; настоящий struct timer_list - к ядру. Имя функции ядра
 * внутри собственного макроса повторно не раскрывается (правило C). */
#define init_timer(t)		rtl_lt_init(t)
#define mod_timer(t, e)		_Generic((t), struct rtl_legacy_timer *: rtl_lt_mod, \
					default: mod_timer)((t), (e))
#define add_timer(t)		_Generic((t), struct rtl_legacy_timer *: rtl_lt_add, \
					default: add_timer)(t)
#define timer_pending(t)	_Generic((t), struct rtl_legacy_timer *: rtl_lt_pending, \
					const struct rtl_legacy_timer *: rtl_lt_pending, \
					default: timer_pending)(t)
#undef  del_timer
#undef  del_timer_sync
#define del_timer(t)		_Generic((t), struct rtl_legacy_timer *: rtl_lt_del, \
					default: timer_delete)(t)
#define del_timer_sync(t)	_Generic((t), struct rtl_legacy_timer *: rtl_lt_del_sync, \
					default: timer_delete_sync)(t)

#endif /* _RTL8192CD_COMPAT_LEGACY_TIMER_H */
