#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>

#include <asm/reboot.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/setup.h>
#include <asm/bootinfo.h>
#include <asm/addrspace.h>
#include <asm/idle.h>
#include <asm/time.h>

#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/delay.h>

#include <asm/mach-realtek/realtek_mem.h>

#define REALTEK_WATCHDOG_TIMER_REG	0x311C

const char *get_system_type(void)
{
#ifdef CONFIG_SOC_RTL8196E
	return "Realtek RTL8196E";
#endif

#ifdef CONFIG_SOC_RTL8197D
	return "Realtek RTL8197D";
#endif

#ifdef CONFIG_SOC_RTL8197F
	return "Realtek RTL8197F";
#endif


}

static inline void wait_instruction(void)
{
#ifndef CONFIG_SOC_RTL8197F
	__asm__(
	"       .set    push            \n"
	"       sleep                   \n"
	"       .set    pop             \n");
#endif
}

static void realtek_machine_restart(char *command)
{
	/* Disable all interrupts */
	local_irq_disable();

	/* Use watchdog to reset the system */
	sr_w32(0x00, REALTEK_WATCHDOG_TIMER_REG);

	for (;;)
		wait_instruction();
}

#ifndef CONFIG_SOC_RTL8197F
static void realtek_wait(void)
{
	if (!need_resched())
		wait_instruction();
	local_irq_enable();
}
#endif

static void realtek_halt(void)
{
	while (1)
		wait_instruction();
}

void __init plat_mem_setup(void)
{
	void *dtb;

	_machine_restart = realtek_machine_restart;
	_machine_halt = realtek_halt;

#ifndef CONFIG_SOC_RTL8197F
	// 8197F uses the r4k wait
	cpu_wait = realtek_wait;
#endif

	/* 6.x: единая точка получения DTB вместо fw_passed_dtb/__dtb_start */
	dtb = get_fdt();

	__dt_setup_arch(dtb);
}

__iomem void *_sys_membase;

void __init device_tree_init(void)
{
	struct device_node *np;
	struct resource res;

	unflatten_and_copy_device_tree();

	np = of_find_compatible_node(NULL, NULL, "realtek,rtl819x-sysc");
	if (!np)
		panic("Failed to find realtek,rtl819x-sysc node");

	if (of_address_to_resource(np, 0, &res))
		panic("Failed to get resource for realtek,rtl819x-sysc");

	_sys_membase = ioremap(res.start, resource_size(&res));
	if(!_sys_membase)
		panic("Failed to map memory for rtl819x-sysc");

	pr_info("BOOTSTRAP = %x %x %x %x\n", sr_r32(0x00), sr_r32(0x04), sr_r32(0x08), sr_r32(0x10));


#ifdef CONFIG_SOC_RTL8197F
	/*
	 * Открываем ядро свитча.
	 *
	 * Без этого все его регистры читаются нулями, а запись таблицы групп
	 * не проходит: ядро молча недоступно, и драйвер честно упирается в
	 * это, ничего не перепутав. Раньше мы этого не замечали - при входе
	 * в командный режим загрузчик сам поднимает сеть и заодно ставит эти
	 * разряды, а весь проект мы грузились именно оттуда.
	 *
	 * Значение снято сравнением двух ЖИВЫХ состояний, а не выведено из
	 * описания, которого у нас нет: у загрузчика после его настройки сети
	 * регистр 0x10 равен 0x80003800, при загрузке прямо с флеша -
	 * 0x80000000. Разница ровно в разрядах 0x3800.
	 */
	sr_w32(sr_r32(0x10) | 0x3800, 0x10);
	pr_info("realtek: swcore 0x10 -> %08x\n", sr_r32(0x10));
#endif

#ifdef CONFIG_SOC_RTL8197D
	/* Voodoo from SDK */
	if((sr_r32(0x00)&0xf)<3)
	{
		sr_w32((sr_r32(0x88) & ( ~(3<<5)&~(0xF<<0))), 0x88);
		sr_w32((sr_r32(0x88)|(1<<4)), 0x88);
		sr_w32(sr_r32(0x88) & (~(3<<7)), 0x88);
	}
#endif
}


#ifdef CONFIG_SOC_RTL8197F

/* Прямой доступ к регистрам SoC (адреса KSEG1, отображение не нужно). */
#define REG32(reg)		(*((volatile unsigned int *)(reg)))
#define RTL_SYSTEM_BASE		0xB8000000

/*
 * Настройка выводов ровно так, как её делает ЗАВОДСКАЯ прошивка.
 *
 * Снято дизассемблером стокового ядра (функция 0x801ccd10, 29.08.2026):
 *     *(0xB8000800) = 0
 *     *(0xB8000804) = 0
 *     *(0xB8000808) = 0x660
 *
 * Ровно эти три записи делал и наш прежний init_8367r, перенесённый из
 * вендорского кода. Я тогда назвал их слепым обнулением и ошибся: они
 * верные, просто раньше выполнялись только вместе с вендорским драйвером.
 * Теперь вынесены наружу, чтобы работали и с ванильным DSA.
 *
 * Регистр 0xB8000100 НЕ трогаем: в стоковой функции инициализации свитча
 * записи в него нет. Значения оттуда (bootcode 0x1A, сток 0x0B) печатаем
 * для сведения, но не меняем — менять то, чего не видел в работающей
 * системе, мы уже пробовали, и это стоило дня.
 *
 * arch_initcall выполняется раньше probe драйвера GPIO (тот через
 * module_platform_driver попадает в device_initcall).
 */
#define RTL_PIN_MUX_0800	0xB8000800
#define RTL_PIN_MUX_0804	0xB8000804
#define RTL_PIN_MUX_0808	0xB8000808
#define RTL_PIN_MUX_0100	0xB8000100
#define RTL_PIN_MUX_0834	0xB8000834

static int __init rtl_nastroit_vyvody(void)
{
	pr_info("realtek: pin mux DO    0800=%08x 0804=%08x 0808=%08x 0834=%08x\n",
		REG32(RTL_PIN_MUX_0800), REG32(RTL_PIN_MUX_0804),
		REG32(RTL_PIN_MUX_0808), REG32(RTL_PIN_MUX_0834));

	REG32(RTL_PIN_MUX_0800) = 0x00000000;
	REG32(RTL_PIN_MUX_0804) = 0x00000000;
	REG32(RTL_PIN_MUX_0808) = 0x00000660;

	/* Регистр 0x834 — из ЗАГРУЗЧИКА (смещение 0x0037DC в распакованном).
	 * Ни вендорский код, ни стоковое ядро его не трогают, а загрузчик
	 * настраивает прямо перед инициализацией SMI. Похоже, именно он
	 * отдаёт выводы 55 и 56 под GPIO. Пишем то же самое. */
	REG32(RTL_PIN_MUX_0834) = (REG32(RTL_PIN_MUX_0834) & 0xF00FFFFF) | 0x03300000;

	pr_info("realtek: pin mux after 0800=%08x 0804=%08x 0808=%08x 0834=%08x\n",
		REG32(RTL_PIN_MUX_0800), REG32(RTL_PIN_MUX_0804),
		REG32(RTL_PIN_MUX_0808), REG32(RTL_PIN_MUX_0834));
	return 0;
}
arch_initcall(rtl_nastroit_vyvody);

#endif /* CONFIG_SOC_RTL8197F */

void __init plat_time_init(void)
{
	of_clk_init(NULL);
	timer_probe();
}

/*
 * Тип корпуса чипа RTL8197F.
 *
 * Перенесено ДОСЛОВНО из вендорского кода процессора
 * (SDK: arch/mips/rtl8197f/gpio.c, функция rtl819x_bond_option).
 * Константы оттуда же, bspchip.h строки 551-553, адрес регистра — строка 275:
 * BSP_BOND_OPTION = SYSTEM_REG_BASE + 0xC, а SYSTEM_REG_BASE = 0xB8000000.
 *
 * Значение выбирает таблицы настройки радиотракта, поэтому угадывать его
 * нельзя: разбор значений взят у вендора байт в байт, включая то, что под
 * каждый тип корпуса отведено по несколько кодов.
 */
#define RTL_BSP_BOND_OPTION	0xB800000C
#define RTL_BSP_BOND_97FB	1
#define RTL_BSP_BOND_97FN	2
#define RTL_BSP_BOND_97FS	3

/* объявление рядом с определением: ядро требует его для всего вывешенного наружу */
unsigned int rtl819x_bond_option(void);

unsigned int rtl819x_bond_option(void)
{
	unsigned int type = 0, ret = 0;

	type = __raw_readl((void __iomem *)RTL_BSP_BOND_OPTION) & 0xf;

	switch (type) {
	case 0x0:	/* 97FB */
		ret = RTL_BSP_BOND_97FB;
		break;
	case 0x4:	/* 97FN */
	case 0x5:
	case 0x6:
		ret = RTL_BSP_BOND_97FN;
		break;
	case 0xa:	/* 97FS */
	case 0xb:
	case 0xc:
		ret = RTL_BSP_BOND_97FS;
	}

	pr_info("realtek: 8197F package type: code %u, value %u\n", type, ret);
	return ret;
}
EXPORT_SYMBOL(rtl819x_bond_option);
