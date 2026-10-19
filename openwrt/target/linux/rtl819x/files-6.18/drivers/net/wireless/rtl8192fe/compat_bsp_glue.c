// SPDX-License-Identifier: GPL-2.0
/*
 * Мелочи, которые у вендора приходят из его ядра (BSP Realtek), а в нашем
 * ядре 6.18 их нет.
 *
 * Всё здесь - данные или пустышки для путей, не нужных первому запуску.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/delay.h>

/*
 * Адреса регистров приёма/передачи UART0. Ядро Realtek выбирало их при
 * загрузке по ID кристалла: у 8197F они смещены на 0x24, у 8197F-VG (наш,
 * ID 0x8197_0000) - нет. То же правило в нашем prom.c (UART_TX).
 * Драйверу они нужны только в заводском режиме измерений (MP, 8192cd_mp.c),
 * который мы не включаем; значения всё равно ставим верные для VG.
 */
unsigned int bsp_uart0_rbr = 0xB8147000;	/* BSP_UART0_RBR_8197F_VG */
unsigned int bsp_uart0_thr = 0xB8147000;	/* BSP_UART0_THR_8197F_VG */

/*
 * Указатели, через которые ядро Realtek (4.4, CONFIG_RTL_8197F_VG) звало
 * GPIO на стороне радиочасти. Драйвер при загрузке сам кладёт сюда свои
 * функции (gpio_func_ptr_init). В нашем ядре никто их не зовёт - пусть
 * просто будут.
 */
typedef int (*RTLWIFINIC_GPIO_read_ptr_t)(unsigned int);
typedef void (*RTLWIFINIC_GPIO_write_ptr_t)(unsigned int, unsigned int);
RTLWIFINIC_GPIO_read_ptr_t RTLWIFINIC_GPIO_read_ptr;
RTLWIFINIC_GPIO_write_ptr_t RTLWIFINIC_GPIO_write_ptr;

/*
 * Подъём PCIe0 перед картой RTL8812FE (5 ГГц).
 *
 * У вендора эта процедура живёт в ядре Realtek (BSP 8197F, arch/mips/
 * rtl8197f/pci.c; в 8192cd_host.c она под __ECOS/__OSK__). В нашем 6.18
 * её нет - переносим ДОСЛОВНО из hh71vm-openwrt (SDK 3.6.0 + запись
 * MDIO 0x08=0x3101 для VG). Сток RX23302 печатает ровно её строки:
 * «Do MDIO_RESET», «40MHz», «Find PCIE Port, Device:Vender ID=f81210ec»,
 * а регистр 0xB8000100 в стоке = 0x0B - то, что оставляет MDIO-сброс ниже.
 *
 * Шина PCI ядру не нужна: драйвер сам ходит в окна PCIe (TYPE_PCI_DIRECT:
 * конфиг 0xB8B10000, память 0xB9000000, прерывание CPU 5 - его наше ядро
 * уже разводит: IRR2[23:20]=5, GIMR бит 21).
 *
 * Порядок важен: регистры PCIe (0xB8B0xxxx) читать только ПОСЛЕ включения
 * тактов (биты 14/18 CLK_MANAGE), иначе шина повиснет - как с воротами
 * радио 2.4 ГГц (0xB8000064).
 *
 * pcie5g=0 - не трогать PCIe вовсе (только 2.4 ГГц, контрольный прогон).
 */
static int pcie5g = 1;
module_param(pcie5g, int, 0444);
MODULE_PARM_DESC(pcie5g, "1 = bring up PCIe0 + RTL8812FE (5 GHz), 0 = do not touch PCIe");

#define RX_REG32(a)		(*(volatile unsigned int *)(a))
#define RX_SYS_CLK_MANAGE	0xB8000010
#define RX_SYS_ENABLE		0xB8000050	/* бит 1 = PERST# карты */
#define RX_SYS_PCIE_PHY		0xB8000100	/* MDIO-сброс PHY */
#define RX_SYS_HW_STRAP		0xB8000008	/* бит 24 = кварц 40 МГц */
#define RX_ECO_SN		0xB8000000	/* ID кристалла */
#define RX_PCIE_RC_CFG		0xB8B00000
#define RX_PCIE_EP_CFG		0xB8B10000
#define RX_PCIE_RC_MDIO		0xB8B01000
#define RX_PCIE_RC_PWRCR	0xB8B01008

static void rx_pcie_mdio_write(unsigned int regaddr, unsigned short val)
{
	volatile int count;

	RX_REG32(RX_PCIE_RC_MDIO) = ((regaddr & 0x1f) << 8) |
				    ((val & 0xffff) << 16) | (1 << 0);
	for (count = 0; count < 5555; count++)
		;	/* вендорская пауза */
}

static void rx_pcie_phy_reset(void)
{
	RX_REG32(RX_PCIE_RC_PWRCR) = 0x01;	/* bit7 PHY reset=0, bit0 LTSSM=1 */
	RX_REG32(RX_PCIE_RC_PWRCR) = 0x81;	/* bit7 PHY reset=1, bit0 LTSSM=1 */
}

int PCIE_reset_procedure_97F(unsigned int PCIeIdx, unsigned int mdioReset)
{
	volatile int count = 5;

	if (!pcie5g) {
		pr_info("rtl8192cd: PCIe%u: pcie5g=0, 5 GHz skipped\n", PCIeIdx);
		return 0;	/* вызывающий вернёт -ENODEV, 2.4 ГГц живёт */
	}
	if (PCIeIdx != 0) {
		pr_warn("rtl8192cd: PCIe%u: 8197F has only PCIe0\n", PCIeIdx);
		return 0;
	}

	pr_info("rtl8192cd: PCIe0 before: CLK_MANAGE=%08x ENABLE=%08x PCIE_PHY=%08x\n",
		RX_REG32(RX_SYS_CLK_MANAGE), RX_REG32(RX_SYS_ENABLE),
		RX_REG32(RX_SYS_PCIE_PHY));

	/* Turn On PCIE IP */
	RX_REG32(RX_SYS_CLK_MANAGE) |= (1 << 12) | (1 << 13) | (1 << 18);
	RX_REG32(RX_SYS_CLK_MANAGE) |= (1 << 14);
	mdelay(10);

	if (mdioReset) {
		printk("Do MDIO_RESET\n");
		mdelay(10);
		RX_REG32(RX_SYS_PCIE_PHY) = (1 << 3) | (0 << 1) | (0 << 0);	/* mdio reset=0 */
		RX_REG32(RX_SYS_PCIE_PHY) = (1 << 3) | (0 << 1) | (1 << 0);	/* mdio reset=1 */
		RX_REG32(RX_SYS_PCIE_PHY) = (1 << 3) | (1 << 1) | (1 << 0);	/* load_done=1 */
	}

	mdelay(10);
	rx_pcie_phy_reset();
	mdelay(10);

	if (mdioReset) {
		if (RX_REG32(RX_SYS_HW_STRAP) & (1 << 24)) {
			printk("40MHz\n");
			rx_pcie_mdio_write(0x0f, 0x12f6);
			rx_pcie_mdio_write(0x00, 0x0071);
			rx_pcie_mdio_write(0x06, 0x1ac1);
			if ((RX_REG32(RX_ECO_SN) & 0xFFFFF000) == 0x81970000)	/* IS_8197F_VG */
				rx_pcie_mdio_write(0x08, 0x3101);
		} else {
			rx_pcie_mdio_write(0x00, 0x0071);
			rx_pcie_mdio_write(0x06, 0x18c1);
		}
	}

	/* PCIE Host Reset */
	mdelay(10);
	rx_pcie_phy_reset();

	/* PCIE Device Reset (PERST#): по стандарту >=100 мс после питания */
	RX_REG32(RX_SYS_ENABLE) &= ~(1 << 1);
	mdelay(300);
	RX_REG32(RX_SYS_ENABLE) |= (1 << 1);

	/* Check link: LTSSM = 0x11 (L0) */
	mdelay(10);
	while (--count) {
		mdelay(10);
		if ((RX_REG32(RX_PCIE_RC_CFG + 0x728) & 0x1f) == 0x11)
			break;
	}
	if (count == 0) {
		/* такты оставляем: случайное чтение RC-регистров не повесит шину */
		printk("PCIE ->  Cannot LinkUP (0x728=%08x)\n",
		       RX_REG32(RX_PCIE_RC_CFG + 0x728));
		return 0;
	}

	RX_REG32(RX_PCIE_RC_CFG + 0x04) = 0x00100007;
	RX_REG32(RX_PCIE_EP_CFG + 0x04) = 0x00100007;
	printk("Find PCIE Port, Device:Vender ID=%x\n", RX_REG32(RX_PCIE_EP_CFG));
	return 1;
}

MODULE_DESCRIPTION("Realtek rtl8192fe 3.8.0 (RTL8197G+RTL8812F), port to Linux 6.18 for RX23302");
