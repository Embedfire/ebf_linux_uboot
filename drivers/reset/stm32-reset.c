// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Patrice Chotard, <patrice.chotard@st.com> for STMicroelectronics.
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <reset-uclass.h>
#include <stm32_rcc.h>
#include <asm/io.h>

#ifdef CONFIG_STM32MP1_TRUSTED
#include <asm/arch/stm32mp1_smc.h>

#define RCC_APB5RST_BANK 0x62
#define RCC_AHB5RST_BANK 0x64
#endif /* CONFIG_STM32MP1_TRUSTED */

/* reset clear offset for STM32MP RCC */
#define RCC_CL 0x4

struct stm32_reset_priv {
	fdt_addr_t base;
};

static int stm32_reset_request(struct reset_ctl *reset_ctl)
{
	return 0;
}

static int stm32_reset_free(struct reset_ctl *reset_ctl)
{
	return 0;
}

static int stm32_reset_assert(struct reset_ctl *reset_ctl)
{
	struct stm32_reset_priv *priv = dev_get_priv(reset_ctl->dev);
	int bank = (reset_ctl->id / BITS_PER_LONG) * 4;
	int offset = reset_ctl->id % BITS_PER_LONG;
#ifdef CONFIG_STM32MP1_TRUSTED
	int rcc_bank = reset_ctl->id / BITS_PER_LONG;
#endif /* CONFIG_STM32MP1_TRUSTED */

	debug("%s: reset id = %ld bank = %d offset = %d)\n", __func__,
	      reset_ctl->id, bank, offset);

	if (dev_get_driver_data(reset_ctl->dev) == STM32MP1)
		/* reset assert is done in rcc set register */
#ifdef CONFIG_STM32MP1_TRUSTED
		if (rcc_bank == RCC_APB5RST_BANK ||
		    rcc_bank == RCC_AHB5RST_BANK)
			stm32_smc_exec(STM32_SMC_RCC, STM32_SMC_REG_WRITE,
				       bank, BIT(offset));
		else
#endif /* CONFIG_STM32MP1_TRUSTED */
			writel(BIT(offset), priv->base + bank);
	else
		setbits_le32(priv->base + bank, BIT(offset));

	return 0;
}

static int stm32_reset_deassert(struct reset_ctl *reset_ctl)
{
	struct stm32_reset_priv *priv = dev_get_priv(reset_ctl->dev);
	int bank = (reset_ctl->id / BITS_PER_LONG) * 4;
	int offset = reset_ctl->id % BITS_PER_LONG;
#ifdef CONFIG_STM32MP1_TRUSTED
	int rcc_bank = reset_ctl->id / BITS_PER_LONG;
#endif /* CONFIG_STM32MP1_TRUSTED */

	debug("%s: reset id = %ld bank = %d offset = %d)\n", __func__,
	      reset_ctl->id, bank, offset);

	if (dev_get_driver_data(reset_ctl->dev) == STM32MP1)
		/* reset deassert is done in rcc clr register */
#ifdef CONFIG_STM32MP1_TRUSTED
		if (rcc_bank == RCC_APB5RST_BANK ||
		    rcc_bank == RCC_AHB5RST_BANK)
			stm32_smc_exec(STM32_SMC_RCC, STM32_SMC_REG_WRITE,
				       bank + RCC_CL, BIT(offset));
		else
#endif /* CONFIG_STM32MP1_TRUSTED */
			writel(BIT(offset), priv->base + bank + RCC_CL);
	else
		clrbits_le32(priv->base + bank, BIT(offset));

	return 0;
}

static const struct reset_ops stm32_reset_ops = {
	.request	= stm32_reset_request,
	.free		= stm32_reset_free,
	.rst_assert	= stm32_reset_assert,
	.rst_deassert	= stm32_reset_deassert,
};

static int stm32_reset_probe(struct udevice *dev)
{
	struct stm32_reset_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr(dev);
	if (priv->base == FDT_ADDR_T_NONE) {
		/* for MFD, get address of parent */
		priv->base = dev_read_addr(dev->parent);
		if (priv->base == FDT_ADDR_T_NONE)
			return -EINVAL;
	}

	return 0;
}

U_BOOT_DRIVER(stm32_rcc_reset) = {
	.name			= "stm32_rcc_reset",
	.id			= UCLASS_RESET,
	.probe			= stm32_reset_probe,
	.priv_auto_alloc_size	= sizeof(struct stm32_reset_priv),
	.ops			= &stm32_reset_ops,
};
