// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */
#define pr_fmt(fmt) "%s: " fmt, __func__
#include <common.h>
#include <dm.h>
#include <errno.h>
#include <fdtdec.h>
#include <regmap.h>
#include <remoteproc.h>
#include <reset.h>
#include <syscon.h>
#include <asm/arch/stm32mp1_smc.h>

#define RCC_GCR_HOLD_BOOT	0
#define RCC_GCR_RELEASE_BOOT	1

/**
 * struct stm32_copro_privdata - power processor private data
 * @reset_ctl:	reset controller handle
 * @hold_boot_regmap
 * @hold_boot_offset
 * @hold_boot_mask
 * @secured_soc:	TZEN flag (register protection)
 */
struct stm32_copro_privdata {
	struct reset_ctl reset_ctl;
	struct regmap *hold_boot_regmap;
	uint hold_boot_offset;
	uint hold_boot_mask;
	bool secured_soc;
	};

/**
 * st_of_to_priv() - generate private data from device tree
 * @dev:	corresponding ti remote processor device
 * @priv:	pointer to driver specific private data
 *
 * Return: 0 if all went ok, else corresponding -ve error
 */
static int st_of_to_priv(struct udevice *dev,
			 struct stm32_copro_privdata *priv)
{
	struct regmap *regmap;
	const fdt32_t *cell;
	uint tz_offset, tz_mask, tzen;
	int len, ret;

	regmap = syscon_phandle_to_regmap(dev, "st,syscfg-holdboot");
	if (IS_ERR(regmap)) {
		dev_dbg(dev, "unable to find holdboot regmap (%ld)\n",
			PTR_ERR(regmap));
		return -EINVAL;
	}

	priv->hold_boot_regmap = regmap;

	cell = dev_read_prop(dev, "st,syscfg-holdboot", &len);
	if (len < 3 * sizeof(fdt32_t)) {
		dev_dbg(dev, "holdboot offset and mask not available\n");
		return -EINVAL;
	}

	priv->hold_boot_offset = fdtdec_get_number(cell + 1, 1);

	priv->hold_boot_mask = fdtdec_get_number(cell + 2, 1);

	regmap = syscon_phandle_to_regmap(dev, "st,syscfg-tz");
	if (IS_ERR(regmap)) {
		dev_dbg(dev, "unable to find tz regmap (%ld)\n",
			PTR_ERR(regmap));
		return -EINVAL;
	}

	cell = dev_read_prop(dev, "st,syscfg-tz", &len);
	if (len < 3 * sizeof(fdt32_t)) {
		dev_dbg(dev, "tz offset and mask not available\n");
		return -EINVAL;
	}

	tz_offset = fdtdec_get_number(cell + 1, 1);

	tz_mask = fdtdec_get_number(cell + 2, 1);

	ret = regmap_read(regmap, tz_offset, &tzen);
	if (ret) {
		dev_dbg(dev, "failed to read soc secure state\n");
		return ret;
	}

	priv->secured_soc = !!(tzen & tz_mask);

	return reset_get_by_index(dev, 0, &priv->reset_ctl);
}

/**
 * stm32_copro_probe() - Basic probe
 * @dev:	corresponding STM32 remote processor device
 *
 * Return: 0 if all went ok, else corresponding -ve error
 */
static int stm32_copro_probe(struct udevice *dev)
{
	struct stm32_copro_privdata *priv;
	int ret;

	priv = dev_get_priv(dev);

	ret = st_of_to_priv(dev, priv);

	dev_dbg(dev, "probed (%d)\n", ret);

	return ret;
}

/* Hold boot bit management */
static int stm32_copro_set_hold_boot(struct udevice *dev, bool hold)
{
	struct stm32_copro_privdata *priv;
	uint status, val;
	int ret;

	priv = dev_get_priv(dev);

	val = hold ? RCC_GCR_HOLD_BOOT : RCC_GCR_RELEASE_BOOT;

	if (priv->secured_soc) {
		return stm32_smc_exec(STM32_SMC_RCC, STM32_SMC_REG_WRITE,
				      priv->hold_boot_offset, val);
	}

	ret = regmap_read(priv->hold_boot_regmap, priv->hold_boot_offset,
			  &status);
	if (ret) {
		dev_err(dev, "failed to read status of mcu\n");
		return ret;
	}

	status &= ~priv->hold_boot_mask;
	status |= val;

	ret = regmap_write(priv->hold_boot_regmap, priv->hold_boot_offset,
			   status);
	if (ret)
		dev_err(dev, "failed to set hold boot\n");

	return ret;
}

static ulong stm32_copro_da_to_pa(struct udevice *dev, ulong da)
{
	/* to update with address translate by DT range  */

	/* CM4 boot at address 0x0 = RETRAM alias, not available for CA7 load */
	if (da >= 0 && da < STM32_RETRAM_SIZE)
		return (da + STM32_RETRAM_BASE);

	return da;
}

/**
 * stm32_copro_load() - Loadup the STM32 Cortex-M4 remote processor
 * @dev:	corresponding STM32 remote processor device
 * @addr:	Address in memory where image binary is stored
 * @size:	Size in bytes of the image binary
 *
 * Return: 0 if all went ok, else corresponding -ve error
 */
static int stm32_copro_load(struct udevice *dev, ulong addr, ulong size)
{
	struct stm32_copro_privdata *priv;
	phys_addr_t loadaddr;
	int ret;

	priv = dev_get_priv(dev);

	stm32_copro_set_hold_boot(dev, true);

	ret = reset_assert(&priv->reset_ctl);
	if (ret) {
		dev_dbg(dev, "Unable assert reset line (ret=%d)\n", ret);
		return ret;
	}

	/* by default load for copro BOOT address = 0x0 */
	loadaddr = stm32_copro_da_to_pa(dev, 0x0);
	dev_dbg(dev, "Loading binary from 0x%08lX, size 0x%08lX to 0x%08lX\n",
		addr, size, loadaddr);

	memcpy((void *)loadaddr, (void *)addr, size);

	dev_dbg(dev, "Complete!\n");
	return 0;
}

/**
 * stm32_copro_start() - Start Cortex-M4 coprocessor
 * @dev:	corresponding st remote processor device
 *
 * Return: 0 if all went ok, else corresponding -ve error
 */
static int stm32_copro_start(struct udevice *dev)
{
	int ret;

	/* move hold boot from true to false start the copro */
	ret = stm32_copro_set_hold_boot(dev, false);
	if (ret)
		return ret;

	/*
	 * Once copro running, reset hold boot flag to avoid copro
	 * rebooting autonomously
	 */
	return stm32_copro_set_hold_boot(dev, true);
}

/**
 * stm32_copro_reset() - Reset Cortex-M4 coprocessor
 * @dev:	corresponding st remote processor device
 *
 * Return: 0 if all went ok, else corresponding -ve error
 */
static int stm32_copro_reset(struct udevice *dev)
{
	struct stm32_copro_privdata *priv;
	int ret;

	priv = dev_get_priv(dev);

	stm32_copro_set_hold_boot(dev, true);

	ret = reset_assert(&priv->reset_ctl);
	if (ret) {
		dev_dbg(dev, "Unable assert reset line (ret=%d)\n", ret);
		return ret;
	}

	return 0;
}

static const struct dm_rproc_ops stm32_copro_ops = {
	.load = stm32_copro_load,
	.start = stm32_copro_start,
	.reset = stm32_copro_reset,
	.da_to_pa = stm32_copro_da_to_pa,
};

static const struct udevice_id stm32_copro_ids[] = {
	{.compatible = "st,stm32mp1-rproc"},
	{}
};

U_BOOT_DRIVER(stm32_copro) = {
	.name = "stm32_m4_proc",
	.of_match = stm32_copro_ids,
	.id = UCLASS_REMOTEPROC,
	.ops = &stm32_copro_ops,
	.probe = stm32_copro_probe,
	.priv_auto_alloc_size = sizeof(struct stm32_copro_privdata),
};
