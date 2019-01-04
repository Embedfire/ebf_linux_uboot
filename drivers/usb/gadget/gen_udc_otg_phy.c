// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <generic-phy.h>
#include <dm/uclass.h>

#include "dwc2_udc_otg_priv.h"

void otg_phy_init(struct dwc2_udc *dev)
{
	struct dwc2_plat_otg_data *pdata = dev->pdata;
	struct udevice *phy_dev;
	struct phy phy;

	if (uclass_get_device_by_of_offset(UCLASS_PHY,
					   pdata->phy_of_node, &phy_dev)) {
		pr_err("failed to found usb phy\n");
		hang();
		return;
	}

	phy.dev = phy_dev;
	phy.id = pdata->regs_phy;

	if (generic_phy_init(&phy)) {
		pr_err("failed to init usb phy\n");
		generic_phy_power_off(&phy);
		return;
	}

	if (generic_phy_power_on(&phy)) {
		pr_err("unable to power on the phy\n");
		return;
	}

	pr_debug("USB Generic PHY Enable\n");
}

void otg_phy_off(struct dwc2_udc *dev)
{
	struct dwc2_plat_otg_data *pdata = dev->pdata;
	struct udevice *phy_dev;
	struct phy phy;
	int ret;

	uclass_get_device_by_of_offset(UCLASS_PHY,
				       pdata->phy_of_node, &phy_dev);
	phy.dev = phy_dev;
	phy.id = pdata->regs_phy;

	ret = generic_phy_power_off(&phy);
	if (ret) {
		pr_err("failed to power off usb phy\n");
		return;
	}
	ret = generic_phy_exit(&phy);
	if (ret) {
		pr_err("failed to power off usb phy\n");
		return;
	}

	pr_debug("USB Generic PHY Disable\n");
}
