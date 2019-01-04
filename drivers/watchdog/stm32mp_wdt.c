// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <regmap.h>
#include <syscon.h>
#include <watchdog.h>
#include <asm/io.h>
#include <asm/arch/stm32.h>

/* IWDG registers */
#define IWDG_KR		0x00	/* Key register */
#define IWDG_PR		0x04	/* Prescaler Register */
#define IWDG_RLR	0x08	/* ReLoad Register */
#define IWDG_SR		0x0C	/* Status Register */

/* IWDG_KR register bit mask */
#define KR_KEY_RELOAD	0xAAAA	/* Reload counter enable */
#define KR_KEY_ENABLE	0xCCCC	/* Peripheral enable */
#define KR_KEY_EWA	0x5555	/* Write access enable */

/* IWDG_PR register bit values */
#define PR_256		0x06	/* Prescaler set to 256 */

/* IWDG_RLR register values */
#define RLR_MAX		0xFFF	/* Max value supported by reload register */

static fdt_addr_t stm32mp_wdt_base
	__attribute__((section(".data"))) = FDT_ADDR_T_NONE;

void hw_watchdog_reset(void)
{
	if (stm32mp_wdt_base != FDT_ADDR_T_NONE)
		writel(KR_KEY_RELOAD, stm32mp_wdt_base + IWDG_KR);
}

void hw_watchdog_init(void)
{
	struct regmap *map;

	map = syscon_get_regmap_by_driver_data(STM32MP_SYSCON_IWDG);
	if (!IS_ERR(map))
		stm32mp_wdt_base = map->ranges[0].start;
	else
		printf("%s: iwdg init error", __func__);
}

static int stm32mp_wdt_probe(struct udevice *dev)
{
	struct regmap *map = syscon_get_regmap(dev);
	struct clk clk;
	int ret, reload;
	u32 time_start;
	ulong regmap_base = map->ranges[0].start;

	debug("IWDG init\n");

	/* Enable clock */
	ret = clk_get_by_name(dev, "pclk", &clk);
	if (ret)
		return ret;

	ret = clk_enable(&clk);
	if (ret)
		return ret;

	/* Get LSI clock */
	ret = clk_get_by_name(dev, "lsi", &clk);
	if (ret)
		return ret;

	/* Prescaler fixed to 256 */
	reload = CONFIG_STM32MP_WATCHDOG_TIMEOUT_SECS *
		 clk_get_rate(&clk) / 256;
	if (reload > RLR_MAX + 1)
		/* Force to max watchdog counter reload value */
		reload = RLR_MAX + 1;
	else if (!reload)
		/* Force to min watchdog counter reload value */
		reload = clk_get_rate(&clk) / 256;

	/* Enable watchdog */
	writel(KR_KEY_ENABLE, regmap_base + IWDG_KR);

	/* Set prescaler & reload registers */
	writel(KR_KEY_EWA, regmap_base + IWDG_KR);
	writel(PR_256, regmap_base + IWDG_PR);
	writel(reload - 1, regmap_base + IWDG_RLR);

	/* Wait for the registers to be updated */
	time_start = get_timer(0);
	while (readl(regmap_base + IWDG_SR)) {
		if (get_timer(time_start) > CONFIG_SYS_HZ) {
			pr_err("Updating IWDG registers timeout");
			return -ETIMEDOUT;
		}
	}

	debug("IWDG init done\n");

	return 0;
}

static const struct udevice_id stm32mp_wdt_match[] = {
	{ .compatible = "st,stm32mp1-iwdg",
	  .data = STM32MP_SYSCON_IWDG },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(stm32mp_wdt) = {
	.name = "stm32mp-wdt",
	.id = UCLASS_SYSCON,
	.of_match = stm32mp_wdt_match,
	.probe = stm32mp_wdt_probe,
};
