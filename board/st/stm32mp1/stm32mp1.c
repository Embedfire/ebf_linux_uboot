// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */
#include <config.h>
#include <common.h>
#include <adc.h>
#include <bootm.h>
#include <dm.h>
#include <clk.h>
#include <console.h>
#include <environment.h>
#include <fdt_support.h>
#include <generic-phy.h>
#include <g_dnl.h>
#include <i2c.h>
#include <led.h>
#include <misc.h>
#include <mtd.h>
#include <mtd_node.h>
#include <netdev.h>
#include <phy.h>
#include <remoteproc.h>
#include <reset.h>
#include <syscon.h>
#include <usb.h>
#include <watchdog.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/stm32.h>
#include <asm/arch/stm32mp1_smc.h>
#include <asm/arch/sys_proto.h>
#include <jffs2/load_kernel.h>
#include <power/regulator.h>
#include <usb/dwc2_udc.h>

/* SYSCFG registers */
#define SYSCFG_BOOTR		0x00
#define SYSCFG_PMCSETR		0x04
#define SYSCFG_IOCTRLSETR	0x18
#define SYSCFG_ICNR		0x1C
#define SYSCFG_CMPCR		0x20
#define SYSCFG_CMPENSETR	0x24
#define SYSCFG_PMCCLRR		0x44

#define SYSCFG_BOOTR_BOOT_MASK		GENMASK(2, 0)
#define SYSCFG_BOOTR_BOOTPD_SHIFT	4

#define SYSCFG_IOCTRLSETR_HSLVEN_TRACE		BIT(0)
#define SYSCFG_IOCTRLSETR_HSLVEN_QUADSPI	BIT(1)
#define SYSCFG_IOCTRLSETR_HSLVEN_ETH		BIT(2)
#define SYSCFG_IOCTRLSETR_HSLVEN_SDMMC		BIT(3)
#define SYSCFG_IOCTRLSETR_HSLVEN_SPI		BIT(4)

#define SYSCFG_CMPCR_SW_CTRL		BIT(1)
#define SYSCFG_CMPCR_READY		BIT(8)

#define SYSCFG_CMPENSETR_MPU_EN		BIT(0)

#define SYSCFG_PMCSETR_ETH_CLK_SEL	BIT(16)
#define SYSCFG_PMCSETR_ETH_REF_CLK_SEL	BIT(17)

#define SYSCFG_PMCSETR_ETH_SELMII	BIT(20)

#define SYSCFG_PMCSETR_ETH_SEL_MASK	GENMASK(23, 21)
#define SYSCFG_PMCSETR_ETH_SEL_GMII_MII	(0 << 21)
#define SYSCFG_PMCSETR_ETH_SEL_RGMII	(1 << 21)
#define SYSCFG_PMCSETR_ETH_SEL_RMII	(4 << 21)

/*
 * Get a global data pointer
 */
DECLARE_GLOBAL_DATA_PTR;

#define USB_LOW_THRESHOLD_UV		200000
#define USB_WARNING_LOW_THRESHOLD_UV	660000
#define USB_START_LOW_THRESHOLD_UV	1230000
#define USB_START_HIGH_THRESHOLD_UV	2150000

int checkboard(void)
{
	int ret;
	char *mode;
	u32 otp;
	struct udevice *dev;
	const char *fdt_compat;
	int fdt_compat_len;

	if (IS_ENABLED(CONFIG_STM32MP1_OPTEE))
		mode = "op-tee";
	else if (IS_ENABLED(CONFIG_STM32MP1_TRUSTED))
		mode = "trusted";
	else
		mode = "basic";

	printf("Board: stm32mp1 in %s mode", mode);
	fdt_compat = fdt_getprop(gd->fdt_blob, 0, "compatible",
				 &fdt_compat_len);
	if (fdt_compat && fdt_compat_len)
		printf(" (%s)", fdt_compat);
	puts("\n");

	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_GET_DRIVER(stm32mp_bsec),
					  &dev);

	if (!ret)
		ret = misc_read(dev, STM32_BSEC_SHADOW(BSEC_OTP_BOARD),
				&otp, sizeof(otp));
	if (!ret && otp) {
		printf("Board: MB%04x Var%d Rev.%c-%02d\n",
		       otp >> 16,
		       (otp >> 12) & 0xF,
		       ((otp >> 8) & 0xF) - 1 + 'A',
		       otp & 0xF);
	}

	return 0;
}

static void board_key_check(void)
{
#if defined(CONFIG_FASTBOOT) || defined(CONFIG_CMD_STM32PROG)
	ofnode node;
	struct gpio_desc gpio;
	enum forced_boot_mode boot_mode = BOOT_NORMAL;

	node = ofnode_path("/config");
	if (!ofnode_valid(node)) {
		debug("%s: no /config node?\n", __func__);
		return;
	}
#ifdef CONFIG_FASTBOOT
	if (gpio_request_by_name_nodev(node, "st,fastboot-gpios", 0,
				       &gpio, GPIOD_IS_IN)) {
		debug("%s: could not find a /config/st,fastboot-gpios\n",
		      __func__);
	} else {
		if (dm_gpio_get_value(&gpio)) {
			puts("Fastboot key pressed, ");
			boot_mode = BOOT_FASTBOOT;
		}

		dm_gpio_free(NULL, &gpio);
	}
#endif
#ifdef CONFIG_CMD_STM32PROG
	if (gpio_request_by_name_nodev(node, "st,stm32prog-gpios", 0,
				       &gpio, GPIOD_IS_IN)) {
		debug("%s: could not find a /config/st,stm32prog-gpios\n",
		      __func__);
	} else {
		if (dm_gpio_get_value(&gpio)) {
			puts("STM32Programmer key pressed, ");
			boot_mode = BOOT_STM32PROG;
		}
		dm_gpio_free(NULL, &gpio);
	}
#endif

	if (boot_mode != BOOT_NORMAL) {
		puts("entering download mode...\n");
		clrsetbits_le32(TAMP_BOOT_CONTEXT,
				TAMP_BOOT_FORCED_MASK,
				boot_mode);
	}
#endif
}

#if defined(CONFIG_USB_GADGET) && defined(CONFIG_USB_GADGET_DWC2_OTG)

/*
 * DWC2 registers should be defined in drivers
 * OTG: drivers/usb/gadget/dwc2_udc_otg_regs.h
 * HOST: ./drivers/usb/host/dwc2.h
 */
#define DWC2_GOTGCTL_OFFSET		0x00
#define DWC2_GGPIO_OFFSET		0x38

#define DWC2_GGPIO_VBUS_SENSING		BIT(21)

#define DWC2_GOTGCTL_AVALIDOVEN		BIT(4)
#define DWC2_GOTGCTL_AVALIDOVVAL	BIT(5)
#define DWC2_GOTGCTL_BVALIDOVEN		BIT(6)
#define DWC2_GOTGCTL_BVALIDOVVAL	BIT(7)
#define DWC2_GOTGCTL_BSVLD		BIT(19)

#define STM32MP_GUSBCFG			0x40002407

static struct dwc2_plat_otg_data stm32mp_otg_data = {
	.regs_otg = FDT_ADDR_T_NONE,
	.usb_gusbcfg = STM32MP_GUSBCFG,
	.priv = NULL, /* pointer to udevice for stusb1600 when present */
};

static struct reset_ctl usbotg_reset;

/* STMicroelectronics STUSB1600 Type-C controller */
#define STUSB1600_CC_CONNECTION_STATUS		0x0E

/* STUSB1600_CC_CONNECTION_STATUS bitfields */
#define STUSB1600_CC_ATTACH			BIT(0)

static int stusb1600_init(ofnode node)
{
	struct udevice *dev, *bus;
	int ret;
	u32 chip_addr;

	ret = ofnode_read_u32(node, "reg", &chip_addr);
	if (ret)
		return -EINVAL;

	ret = uclass_get_device_by_ofnode(UCLASS_I2C, ofnode_get_parent(node),
					  &bus);
	if (ret) {
		printf("bus for stusb1600 not found\n");
		return -ENODEV;
	}

	ret = dm_i2c_probe(bus, chip_addr, 0, &dev);
	if (!ret)
		stm32mp_otg_data.priv = dev;

	return ret;
}

static int stusb1600_cable_connected(void)
{
	struct udevice *stusb1600_dev = stm32mp_otg_data.priv;
	u8 status;

	if (dm_i2c_read(stusb1600_dev,
			STUSB1600_CC_CONNECTION_STATUS,
			&status, 1))
		return 0;

	return status & STUSB1600_CC_ATTACH;
}

static void board_usbotg_init(void)
{
	ofnode usb1600_node;
	int node;
	int count;
	struct fdtdec_phandle_args args;
	struct udevice *dev;
	const void *blob = gd->fdt_blob;
	struct clk clk;

	/* find the usb otg node */
	node = fdt_node_offset_by_compatible(blob, -1, "snps,dwc2");
	if (node < 0) {
		debug("Not found usb_otg device\n");
		return;
	}

	if (!fdtdec_get_is_enabled(blob, node)) {
		debug("stm32 usbotg is disabled in the device tree\n");
		return;
	}

	/* Enable clock */
	if (fdtdec_parse_phandle_with_args(blob, node, "clocks",
					   "#clock-cells", 0, 0, &args)) {
		debug("usbotg has no clocks defined in the device tree\n");
		return;
	}

	if (uclass_get_device_by_of_offset(UCLASS_CLK, args.node, &dev))
		return;

	if (args.args_count != 1)
		return;

	clk.dev = dev;
	clk.id = args.args[0];

	if (clk_enable(&clk)) {
		debug("Failed to enable usbotg clock\n");
		return;
	}

	/* Reset */
	if (fdtdec_parse_phandle_with_args(blob, node, "resets",
					   "#reset-cells", 0, 0, &args)) {
		debug("usbotg has no resets defined in the device tree\n");
		goto clk_err;
	}

	if ((uclass_get_device_by_of_offset(UCLASS_RESET, args.node, &dev)) ||
	    args.args_count != 1)
		goto clk_err;

	usbotg_reset.dev = dev;
	usbotg_reset.id = args.args[0];

	/* Phy */
	if (!(fdtdec_parse_phandle_with_args(blob, node, "phys",
					     "#phy-cells", 0, 0, &args))) {
		stm32mp_otg_data.phy_of_node =
			fdt_parent_offset(blob, args.node);
		if (stm32mp_otg_data.phy_of_node <= 0) {
			debug("Not found usbo phy device\n");
			goto clk_err;
		}
		stm32mp_otg_data.regs_phy = fdtdec_get_uint(blob, args.node,
							    "reg", -1);
	}

	/* Parse and store data needed for gadget */
	stm32mp_otg_data.regs_otg = fdtdec_get_addr(blob, node, "reg");
	if (stm32mp_otg_data.regs_otg == FDT_ADDR_T_NONE) {
		debug("usbotg: can't get base address\n");
		goto clk_err;
	}

	stm32mp_otg_data.rx_fifo_sz = fdtdec_get_int(blob, node,
						     "g-rx-fifo-size", 0);
	stm32mp_otg_data.np_tx_fifo_sz = fdtdec_get_int(blob, node,
							"g-np-tx-fifo-size", 0);

	count = fdtdec_get_int_array_count(blob, node, "g-tx-fifo-size",
			&stm32mp_otg_data.tx_fifo_sz_array[DWC2_SIZE_OFFS],
			ARRAY_SIZE(stm32mp_otg_data.tx_fifo_sz_array));

	if (count != -FDT_ERR_NOTFOUND)
		stm32mp_otg_data.tx_fifo_sz_array[DWC2_SIZE_NB_OFFS] = count;

	/* if node stusb1600 is present, means DK1 or DK2 board */
	usb1600_node = ofnode_by_compatible(ofnode_null(), "st,stusb1600");
	if (ofnode_valid(usb1600_node)) {
		stusb1600_init(usb1600_node);
		return;
	}

	/* Enable voltage level detector */
	if (!(fdtdec_parse_phandle_with_args(blob, node, "usb33d-supply",
					     NULL, 0, 0, &args)))
		if (!uclass_get_device_by_of_offset(UCLASS_REGULATOR,
						    args.node, &dev))
			if (regulator_set_enable(dev, true)) {
				debug("Failed to enable usb33d\n");
				goto clk_err;
			}

	return;

clk_err:
	clk_disable(&clk);
}

int board_usb_init(int index, enum usb_init_type init)
{
	if (init == USB_INIT_HOST)
		return 0;

	if (stm32mp_otg_data.regs_otg == FDT_ADDR_T_NONE)
		return -EINVAL;

	/* Reset usbotg */
	reset_assert(&usbotg_reset);
	udelay(2);
	reset_deassert(&usbotg_reset);

	/* if the board embed an USB1600 chip */
	if (stm32mp_otg_data.priv)
		/* Override A/B session valid bits */
		stm32mp_otg_data.usb_gotgctl = DWC2_GOTGCTL_AVALIDOVEN |
					       DWC2_GOTGCTL_AVALIDOVVAL |
					       DWC2_GOTGCTL_BVALIDOVEN |
					       DWC2_GOTGCTL_BVALIDOVVAL;
	else
		/* Enable vbus sensing */
		setbits_le32(stm32mp_otg_data.regs_otg + DWC2_GGPIO_OFFSET,
			     DWC2_GGPIO_VBUS_SENSING);

	return dwc2_udc_probe(&stm32mp_otg_data);
}

int g_dnl_board_usb_cable_connected(void)
{
	if (stm32mp_otg_data.priv)
		return stusb1600_cable_connected();

	return readl(stm32mp_otg_data.regs_otg + DWC2_GOTGCTL_OFFSET) &
		DWC2_GOTGCTL_BSVLD;
}

#define STM32MP1_G_DNL_DFU_PRODUCT_NUM 0xdf11
#define STM32MP1_G_DNL_FASTBOOT_PRODUCT_NUM 0x0afb

int g_dnl_bind_fixup(struct usb_device_descriptor *dev, const char *name)
{
	if (!strcmp(name, "usb_dnl_dfu"))
		put_unaligned(STM32MP1_G_DNL_DFU_PRODUCT_NUM, &dev->idProduct);
	else if (!strcmp(name, "usb_dnl_fastboot"))
		put_unaligned(STM32MP1_G_DNL_FASTBOOT_PRODUCT_NUM,
			      &dev->idProduct);
	else
		put_unaligned(CONFIG_USB_GADGET_PRODUCT_NUM, &dev->idProduct);

	return 0;
}
#endif /* CONFIG_USB_GADGET */

/* board interface eth init */
/* this is a weak define that we are overriding */
int board_interface_eth_init(int interface_type, bool eth_clk_sel_reg,
			     bool eth_ref_clk_sel_reg)
{
	u8 *syscfg;
	u32 value;

	syscfg = (u8 *)syscon_get_first_range(STM32MP_SYSCON_SYSCFG);

	if (!syscfg)
		return -ENODEV;

	switch (interface_type) {
	case PHY_INTERFACE_MODE_MII:
		value = SYSCFG_PMCSETR_ETH_SEL_GMII_MII |
			SYSCFG_PMCSETR_ETH_REF_CLK_SEL;
		debug("%s: PHY_INTERFACE_MODE_MII\n", __func__);
		break;
	case PHY_INTERFACE_MODE_GMII:
		if (eth_clk_sel_reg)
			value = SYSCFG_PMCSETR_ETH_SEL_GMII_MII |
				SYSCFG_PMCSETR_ETH_CLK_SEL;
		else
			value = SYSCFG_PMCSETR_ETH_SEL_GMII_MII;
		debug("%s: PHY_INTERFACE_MODE_GMII\n", __func__);
		break;
	case PHY_INTERFACE_MODE_RMII:
		if (eth_ref_clk_sel_reg)
			value = SYSCFG_PMCSETR_ETH_SEL_RMII |
				SYSCFG_PMCSETR_ETH_REF_CLK_SEL;
		else
			value = SYSCFG_PMCSETR_ETH_SEL_RMII;
		debug("%s: PHY_INTERFACE_MODE_RMII\n", __func__);
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		if (eth_clk_sel_reg)
			value = SYSCFG_PMCSETR_ETH_SEL_RGMII |
				SYSCFG_PMCSETR_ETH_CLK_SEL;
		else
			value = SYSCFG_PMCSETR_ETH_SEL_RGMII;
		debug("%s: PHY_INTERFACE_MODE_RGMII\n", __func__);
		break;
	default:
		debug("%s: Do not manage %d interface\n",
		      __func__, interface_type);
		/* Do not manage others interfaces */
		return -EINVAL;
	}

	/* clear and set ETH configuration bits */
	writel(SYSCFG_PMCSETR_ETH_SEL_MASK | SYSCFG_PMCSETR_ETH_SELMII |
	       SYSCFG_PMCSETR_ETH_REF_CLK_SEL | SYSCFG_PMCSETR_ETH_CLK_SEL,
	       syscfg + SYSCFG_PMCCLRR);
	writel(value, syscfg + SYSCFG_PMCSETR);

	return 0;
}

#ifdef CONFIG_LED
static int get_led(struct udevice **dev, char *led_string)
{
	char *led_name;
	int ret;

	led_name = fdtdec_get_config_string(gd->fdt_blob, led_string);
	if (!led_name) {
		pr_debug("%s: could not find %s config string\n",
			 __func__, led_string);
		return -ENOENT;
	}
	ret = led_get_by_label(led_name, dev);
	if (ret) {
		debug("%s: get=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}

static int setup_led(enum led_state_t cmd)
{
	struct udevice *dev;
	int ret;

	ret = get_led(&dev, "u-boot,boot-led");
	if (ret)
		return ret;

	ret = led_set_state(dev, cmd);
	return ret;
}
#endif /* CONFIG_LED */

static void __maybe_unused led_error_blink(u32 nb_blink)
{
#ifdef CONFIG_LED
	int ret;
	struct udevice *led;
	u32 i;
#endif

	if (!nb_blink)
		return;

#ifdef CONFIG_LED
	ret = get_led(&led, "u-boot,error-led");
	if (!ret) {
		/* make u-boot,error-led blinking */
		/* if U32_MAX and 125ms interval, for 17.02 years */
		for (i = 0; i < 2 * nb_blink; i++) {
			led_set_state(led, LEDST_TOGGLE);
			mdelay(125);
			WATCHDOG_RESET();
		}
		led_set_state(led, LEDST_ON);
	}
#endif

	/* infinite: the boot process must be stopped */
	if (nb_blink == U32_MAX)
		hang();
}

#ifdef CONFIG_ADC
static int board_check_usb_power(void)
{
	struct ofnode_phandle_args adc_args;
	struct udevice *adc;
	ofnode node;
	unsigned int raw;
	int max_uV = 0;
	int min_uV = USB_START_HIGH_THRESHOLD_UV;
	int ret, uV, adc_count;
	u32 nb_blink;
	u8 i;
	node = ofnode_path("/config");
	if (!ofnode_valid(node)) {
		debug("%s: no /config node?\n", __func__);
		return -ENOENT;
	}

	/*
	 * Retrieve the ADC channels devices and get measurement
	 * for each of them
	 */
	adc_count = ofnode_count_phandle_with_args(node, "st,adc_usb_pd",
						   "#io-channel-cells");
	if (adc_count < 0) {
		if (adc_count == -ENOENT)
			return 0;

		pr_err("%s: can't find adc channel (%d)\n", __func__,
		       adc_count);

		return adc_count;
	}

	for (i = 0; i < adc_count; i++) {
		if (ofnode_parse_phandle_with_args(node, "st,adc_usb_pd",
						   "#io-channel-cells", 0, i,
						   &adc_args)) {
			pr_debug("%s: can't find /config/st,adc_usb_pd\n",
				 __func__);
			return 0;
		}

		ret = uclass_get_device_by_ofnode(UCLASS_ADC, adc_args.node,
						  &adc);

		if (ret) {
			pr_err("%s: Can't get adc device(%d)\n", __func__,
			       ret);
			return ret;
		}

		ret = adc_channel_single_shot(adc->name, adc_args.args[0],
					      &raw);
		if (ret) {
			pr_err("%s: single shot failed for %s[%d]!\n",
			       __func__, adc->name, adc_args.args[0]);
			return ret;
		}
		/* Convert to uV */
		if (!adc_raw_to_uV(adc, raw, &uV)) {
			if (uV > max_uV)
				max_uV = uV;
			if (uV < min_uV)
				min_uV = uV;
			pr_debug("%s: %s[%02d] = %u, %d uV\n", __func__,
				 adc->name, adc_args.args[0], raw, uV);
		} else {
			pr_err("%s: Can't get uV value for %s[%d]\n",
			       __func__, adc->name, adc_args.args[0]);
		}
	}

	/*
	 * If highest value is inside 1.23 Volts and 2.10 Volts, that means
	 * board is plugged on an USB-C 3A power supply and boot process can
	 * continue.
	 */
	if (max_uV > USB_START_LOW_THRESHOLD_UV &&
	    max_uV <= USB_START_HIGH_THRESHOLD_UV &&
	    min_uV <= USB_LOW_THRESHOLD_UV)
		return 0;

	pr_err("****************************************************\n");

	/*
	 * If highest and lowest value are either both below
	 * USB_LOW_THRESHOLD_UV or both above USB_LOW_THRESHOLD_UV, that
	 * means USB TYPE-C is in unattached mode, this is an issue, make
	 * u-boot,error-led blinking and stop boot process.
	 */
	if ((max_uV > USB_LOW_THRESHOLD_UV &&
	     min_uV > USB_LOW_THRESHOLD_UV) ||
	     (max_uV <= USB_LOW_THRESHOLD_UV &&
	     min_uV <= USB_LOW_THRESHOLD_UV)) {
		pr_err("* ERROR USB TYPE-C connection in unattached mode   *\n");
		pr_err("* Check that USB TYPE-C cable is correctly plugged *\n");
		/* with 125ms interval, led will blink for 17.02 years ....*/
		nb_blink = U32_MAX;
	}

	if (max_uV > USB_LOW_THRESHOLD_UV &&
	    max_uV <= USB_WARNING_LOW_THRESHOLD_UV &&
	    min_uV <= USB_LOW_THRESHOLD_UV) {
		pr_err("*        WARNING 500mA power supply detected       *\n");
		nb_blink = 2;
	}

	if (max_uV > USB_WARNING_LOW_THRESHOLD_UV &&
	    max_uV <= USB_START_LOW_THRESHOLD_UV &&
	    min_uV <= USB_LOW_THRESHOLD_UV) {
		pr_err("*       WARNING 1.5mA power supply detected        *\n");
		nb_blink = 3;
	}

	/*
	 * If highest value is above 2.15 Volts that means that the USB TypeC
	 * supplies more than 3 Amp, this is not compliant with TypeC specification
	 */
	if (max_uV > USB_START_HIGH_THRESHOLD_UV) {
		pr_err("*      USB TYPE-C charger not compliant with       *\n");
		pr_err("*                   specification                  *\n");
		pr_err("****************************************************\n\n");
		/* with 125ms interval, led will blink for 17.02 years ....*/
		nb_blink = U32_MAX;
	} else {
		pr_err("*     Current too low, use a 3A power supply!      *\n");
		pr_err("****************************************************\n\n");
	}

	led_error_blink(nb_blink);

	return 0;
}
#endif /* CONFIG_ADC */

static void sysconf_init(void)
{
#ifndef CONFIG_STM32MP1_TRUSTED
	u8 *syscfg;
#ifdef CONFIG_DM_REGULATOR
	struct udevice *pwr_dev;
	struct udevice *pwr_reg;
	struct udevice *dev;
	int ret;
	u32 otp = 0;
#endif
	u32 bootr;

	syscfg = (u8 *)syscon_get_first_range(STM32MP_SYSCON_SYSCFG);
	debug("SYSCFG: init @0x%p\n", syscfg);

	/* interconnect update : select master using the port 1 */
	/* LTDC = AXI_M9 */
	/* GPU  = AXI_M8 */
	/* today information is hardcoded in U-Boot */
	writel(BIT(9), syscfg + SYSCFG_ICNR);
	debug("[0x%x] SYSCFG.icnr = 0x%08x (LTDC and GPU)\n",
	      (u32)syscfg + SYSCFG_ICNR, readl(syscfg + SYSCFG_ICNR));

	/* disable Pull-Down for boot pin connected to VDD */
	bootr = readl(syscfg + SYSCFG_BOOTR);
	bootr &= ~(SYSCFG_BOOTR_BOOT_MASK << SYSCFG_BOOTR_BOOTPD_SHIFT);
	bootr |= (bootr & SYSCFG_BOOTR_BOOT_MASK) << SYSCFG_BOOTR_BOOTPD_SHIFT;
	writel(bootr, syscfg + SYSCFG_BOOTR);
	debug("[0x%x] SYSCFG.bootr = 0x%08x\n",
	      (u32)syscfg + SYSCFG_BOOTR, readl(syscfg + SYSCFG_BOOTR));

#ifdef CONFIG_DM_REGULATOR
	/* High Speed Low Voltage Pad mode Enable for SPI, SDMMC, ETH, QSPI
	 * and TRACE. Needed above ~50MHz and conditioned by AFMUX selection.
	 * The customer will have to disable this for low frequencies
	 * or if AFMUX is selected but the function not used, typically for
	 * TRACE. Otherwise, impact on power consumption.
	 *
	 * WARNING:
	 *   enabling High Speed mode while VDD>2.7V
	 *   with the OTP product_below_2v5 (OTP 18, BIT 13)
	 *   erroneously set to 1 can damage the IC!
	 *   => U-Boot set the register only if VDD < 2.7V (in DT)
	 *      but this value need to be consistent with board design
	 */
	ret = syscon_get_by_driver_data(STM32MP_SYSCON_PWR, &pwr_dev);
	if (!ret) {
		ret = uclass_get_device_by_driver(UCLASS_MISC,
						  DM_GET_DRIVER(stm32mp_bsec),
						  &dev);
		if (ret) {
			pr_err("Can't find stm32mp_bsec driver\n");
			return;
		}

		ret = misc_read(dev, STM32_BSEC_SHADOW(18), &otp, 4);
		if (!ret)
			otp = otp & BIT(13);

		ret = uclass_get_device_by_driver(UCLASS_PMIC,
						  DM_GET_DRIVER(stm32mp_pwr_pmic),
						  &dev);

		/* get VDD = vdd-supply */
		ret = device_get_supply_regulator(dev, "vdd-supply", &pwr_reg);

		/* check if VDD is Low Voltage */
		if (!ret) {
			if (regulator_get_value(pwr_reg) < 2700000) {
				writel(SYSCFG_IOCTRLSETR_HSLVEN_TRACE |
				       SYSCFG_IOCTRLSETR_HSLVEN_QUADSPI |
				       SYSCFG_IOCTRLSETR_HSLVEN_ETH |
				       SYSCFG_IOCTRLSETR_HSLVEN_SDMMC |
				       SYSCFG_IOCTRLSETR_HSLVEN_SPI,
				       syscfg + SYSCFG_IOCTRLSETR);

				if (!otp)
					pr_err("product_below_2v5=0: HSLVEN protected by HW\n");
			} else {
				if (otp)
					pr_err("product_below_2v5=1: HSLVEN update is destructive, no update as VDD>2.7V\n");
			}
		} else {
			debug("VDD unknown");
		}
	}
#endif
	debug("[0x%x] SYSCFG.IOCTRLSETR = 0x%08x\n",
	      (u32)syscfg + SYSCFG_IOCTRLSETR,
	      readl(syscfg + SYSCFG_IOCTRLSETR));

	/* activate automatic I/O compensation
	 * warning: need to ensure CSI enabled and ready in clock driver
	 */
	writel(SYSCFG_CMPENSETR_MPU_EN, syscfg + SYSCFG_CMPENSETR);

	while (!(readl(syscfg + SYSCFG_CMPCR) & SYSCFG_CMPCR_READY))
		;
	clrbits_le32(syscfg + SYSCFG_CMPCR, SYSCFG_CMPCR_SW_CTRL);

	debug("[0x%x] SYSCFG.cmpcr = 0x%08x\n",
	      (u32)syscfg + SYSCFG_CMPCR, readl(syscfg + SYSCFG_CMPCR));
#endif
}

#ifdef CONFIG_DM_REGULATOR
/* Fix to make I2C1 usable on DK2 for touchscreen usage in kernel */
static int dk2_i2c1_fix(void)
{
	ofnode node;
	struct gpio_desc hdmi, audio;
	int ret = 0;

	node = ofnode_path("/soc/i2c@40012000/hdmi-transmitter@39");
	if (!ofnode_valid(node)) {
		pr_debug("%s: no hdmi-transmitter@39 ?\n", __func__);
		return -ENOENT;
	}

	if (gpio_request_by_name_nodev(node, "reset-gpios", 0,
				       &hdmi, GPIOD_IS_OUT)) {
		pr_debug("%s: could not find reset-gpios\n",
			 __func__);
		return -ENOENT;
	}

	node = ofnode_path("/soc/i2c@40012000/cs42l51@4a");
	if (!ofnode_valid(node)) {
		pr_debug("%s: no cs42l51@4a ?\n", __func__);
		return -ENOENT;
	}

	if (gpio_request_by_name_nodev(node, "reset-gpios", 0,
				       &audio, GPIOD_IS_OUT)) {
		pr_debug("%s: could not find reset-gpios\n",
			 __func__);
		return -ENOENT;
	}

	/* before power up, insure that HDMI anh AUDIO IC is under reset */
	ret = dm_gpio_set_value(&hdmi, 1);
	if (ret) {
		pr_err("%s: can't set_value for hdmi_nrst gpio", __func__);
		goto error;
	}
	ret = dm_gpio_set_value(&audio, 1);
	if (ret) {
		pr_err("%s: can't set_value for audio_nrst gpio", __func__);
		goto error;
	}

	/* power-up audio IC */
	regulator_autoset_by_name("v1v8_audio", NULL);

	/* power-up HDMI IC */
	regulator_autoset_by_name("v1v2_hdmi", NULL);
	regulator_autoset_by_name("v3v3_hdmi", NULL);

error:
	return ret;
}
#endif

enum env_location env_get_location(enum env_operation op, int prio)
{
	u32 bootmode = get_bootmode();

	if (prio)
		return ENVL_UNKNOWN;

	switch (bootmode & TAMP_BOOT_DEVICE_MASK) {
#ifdef CONFIG_ENV_IS_IN_EXT4
	case BOOT_FLASH_SD:
	case BOOT_FLASH_EMMC:
		return ENVL_EXT4;
#endif
#ifdef CONFIG_ENV_IS_IN_UBI
	case BOOT_FLASH_NAND:
		return ENVL_UBI;
#endif
#ifdef CONFIG_ENV_IS_IN_SPI_FLASH
	case BOOT_FLASH_NOR:
		return ENVL_SPI_FLASH;
#endif
	default:
		return ENVL_NOWHERE;
	}
}

#if defined(CONFIG_ENV_IS_IN_EXT4)
const char *env_ext4_get_intf(void)
{
	u32 bootmode = get_bootmode();

	switch (bootmode & TAMP_BOOT_DEVICE_MASK) {
	case BOOT_FLASH_SD:
	case BOOT_FLASH_EMMC:
		return "mmc";
	default:
		return "";
	}
}

const char *env_ext4_get_dev_part(void)
{
	static char *const dev_part[] = {"0:auto", "1:auto", "2:auto"};
	u32 bootmode = get_bootmode();

	return dev_part[(bootmode & TAMP_BOOT_INSTANCE_MASK) - 1];
}
#endif

static __maybe_unused bool board_is_dk2(void)
{
	if (CONFIG_IS_ENABLED(TARGET_STM32MP157C_DK2) &&
	    of_machine_is_compatible("st,stm32mp157c-dk2"))
		return true;

	return false;
}

/* board dependent setup after realloc */
int board_init(void)
{
	struct udevice *dev;

	/* address of boot parameters */
	gd->bd->bi_boot_params = STM32_DDR_BASE + 0x100;

	/* probe all PINCTRL for hog */
	for (uclass_first_device(UCLASS_PINCTRL, &dev);
	     dev;
	     uclass_next_device(&dev)) {
		pr_debug("probe pincontrol = %s\n", dev->name);
	}

	board_key_check();

	if (IS_ENABLED(CONFIG_LED))
		led_default_state();

#ifdef CONFIG_DM_REGULATOR
	if (board_is_dk2())
		dk2_i2c1_fix();

	regulators_enable_boot_on(_DEBUG);
#endif

#ifdef CONFIG_ADC
	board_check_usb_power();
#endif /* CONFIG_ADC */

	sysconf_init();

#if defined(CONFIG_USB_GADGET) && defined(CONFIG_USB_GADGET_DWC2_OTG)
	board_usbotg_init();
#endif

	return 0;
}

int board_late_init(void)
{

	char *boot_device;
#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	const void *fdt_compat;
	int fdt_compat_len;
	int ret;
	u32 otp;
	struct udevice *dev;
	char buf[10];

	fdt_compat = fdt_getprop(gd->fdt_blob, 0, "compatible",
				 &fdt_compat_len);
	if (fdt_compat && fdt_compat_len) {
		if (strncmp(fdt_compat, "st,", 3) != 0)
			env_set("board_name", fdt_compat);
		else
			env_set("board_name", fdt_compat + 3);
	}
	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_GET_DRIVER(stm32mp_bsec),
					  &dev);

	if (!ret)
		ret = misc_read(dev, STM32_BSEC_SHADOW(BSEC_OTP_BOARD),
				&otp, sizeof(otp));
	if (!ret && otp) {
		snprintf(buf, sizeof(buf), "0x%04x", otp >> 16);
		env_set("board_id", buf);

		snprintf(buf, sizeof(buf), "0x%04x",
			 ((otp >> 8) & 0xF) - 1 + 0xA);
		env_set("board_rev", buf);
	}
#endif

	/* Check the boot-source to disable bootdelay */
	boot_device = env_get("boot_device");
	if (!strcmp(boot_device, "serial") || !strcmp(boot_device, "usb"))
		env_set("bootdelay", "0");

	return 0;
}

void board_quiesce_devices(void)
{
#ifdef CONFIG_LED
	setup_led(LEDST_OFF);
#endif
}

#ifdef CONFIG_SYS_MTDPARTS_RUNTIME

#define MTDPARTS_LEN		256
#define MTDIDS_LEN		128

/**
 * The mtdparts_nand0 and mtdparts_nor0 variable tends to be long.
 * If we need to access it before the env is relocated, then we need
 * to use our own stack buffer. gd->env_buf will be too small.
 *
 * @param buf temporary buffer pointer MTDPARTS_LEN long
 * @return mtdparts variable string, NULL if not found
 */
static const char *env_get_mtdparts(const char *str, char *buf)
{
	if (gd->flags & GD_FLG_ENV_READY)
		return env_get(str);
	if (env_get_f(str, buf, MTDPARTS_LEN) != -1)
		return buf;
	return NULL;
}

/**
 * update the variables "mtdids" and "mtdparts" with content of mtdparts_<dev>
 */
static void board_get_mtdparts(const char *dev,
			       char *mtdids,
			       char *mtdparts)
{
	char env_name[32] = "mtdparts_";
	char tmp_mtdparts[MTDPARTS_LEN];
	const char *tmp;

	/* name of env variable to read = mtdparts_<dev> */
	strcat(env_name, dev);
	tmp = env_get_mtdparts(env_name, tmp_mtdparts);
	if (tmp) {
		/* mtdids: "<dev>=<dev>, ...." */
		if (mtdids[0] != '\0')
			strcat(mtdids, ",");
		strcat(mtdids, dev);
		strcat(mtdids, "=");
		strcat(mtdids, dev);

		/* mtdparts: "mtdparts=<dev>:<mtdparts_<dev>>;..." */
		if (mtdparts[0] != '\0')
			strncat(mtdparts, ";", MTDPARTS_LEN);
		else
			strcat(mtdparts, "mtdparts=");
		strncat(mtdparts, dev, MTDPARTS_LEN);
		strncat(mtdparts, ":", MTDPARTS_LEN);
		strncat(mtdparts, tmp, MTDPARTS_LEN);
	}
}

void board_mtdparts_default(const char **mtdids, const char **mtdparts)
{
	struct mtd_info *mtd;
	struct udevice *dev;
	static char parts[3 * MTDPARTS_LEN + 1];
	static char ids[MTDIDS_LEN + 1];
	static bool mtd_initialized;

	if (mtd_initialized) {
		*mtdids = ids;
		*mtdparts = parts;
		return;
	}

	memset(parts, 0, sizeof(parts));
	memset(ids, 0, sizeof(ids));

	/* probe all MTD devices */
	for (uclass_first_device(UCLASS_MTD, &dev);
	     dev;
	     uclass_next_device(&dev)) {
		pr_debug("mtd device = %s\n", dev->name);
	}

	mtd = get_mtd_device_nm("nand0");
	if (!IS_ERR_OR_NULL(mtd)) {
		board_get_mtdparts("nand0", ids, parts);
		put_mtd_device(mtd);
	}

	mtd = get_mtd_device_nm("spi-nand0");
	if (!IS_ERR_OR_NULL(mtd)) {
		board_get_mtdparts("spi-nand0", ids, parts);
		put_mtd_device(mtd);
	}

	if (!uclass_get_device(UCLASS_SPI_FLASH, 0, &dev)) {
		board_get_mtdparts("nor0", ids, parts);
	}

	mtd_initialized = true;
	*mtdids = ids;
	*mtdparts = parts;
	debug("%s:mtdids=%s & mtdparts=%s\n", __func__, ids, parts);
}
#endif

#if defined(CONFIG_OF_BOARD_SETUP)
int ft_board_setup(void *blob, bd_t *bd)
{
	int off;
#ifdef CONFIG_FDT_FIXUP_PARTITIONS
	struct node_info nodes[] = {
		{ "st,stm32f469-qspi",		MTD_DEV_TYPE_NOR,  },
		{ "st,stm32mp15-fmc2",		MTD_DEV_TYPE_NAND, },
	};
	fdt_fixup_mtdparts(blob, nodes, ARRAY_SIZE(nodes));
#endif

	/* Update DT if coprocessor started */
	off = fdt_path_offset(blob, "/m4");
	if (off > 0) {
		if (env_get("copro_state")) {
			fdt_setprop_empty(blob, off, "early-booted");
		} else {
			fdt_delprop(blob, off, "early-booted");
			writel(0, TAMP_COPRO_RSC_TBL_ADDRESS);
		}
	}

	return 0;
}
#endif

static void board_stm32copro_image_process(ulong fw_image, size_t fw_size)
{
	int ret, id = 0; /* Copro id fixed to 0 as only one coproc on mp1 */
	unsigned int rsc_size;
	ulong rsc_addr;

	if (!rproc_is_initialized())
		if (rproc_init()) {
			printf("Remote Processor %d initialization failed\n",
			       id);
			return;
		}

	ret = rproc_load_rsc_table(id, fw_image, fw_size, &rsc_addr, &rsc_size);
	if (ret && ret != -ENODATA)
		return;

	ret = rproc_load(id, fw_image, fw_size);
	printf("Load Remote Processor %d with data@addr=0x%08lx %u bytes:%s\n",
	       id, fw_image, fw_size, ret ? " Failed!" : " Success!");

	if (!ret) {
		ret = rproc_start(id);
		printf("Start firmware:%s\n", ret ? " Failed!" : " Success!");
		if (!ret)
			env_set("copro_state", "booted");
	}
}

U_BOOT_FIT_LOADABLE_HANDLER(IH_TYPE_STM32COPRO, board_stm32copro_image_process);
