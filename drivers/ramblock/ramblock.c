// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2013 Henrik Nordstrom <henrik@henriknordstrom.net>
 */

#include <common.h>
#include <blk.h>
#include <dm.h>
#include <env.h>
#include <fdtdec.h>
#include <part.h>
#include <os.h>
#include <malloc.h>
#include <ramblockdev.h>
#include <dm/device_compat.h>
#include <linux/errno.h>
#include <dm/device-internal.h>

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_BLK

int ramblock_init(void)
{
	int ret, i;
	struct uclass *uc;
	struct udevice *dev;

	ret = uclass_get(UCLASS_RAMBLOCK, &uc);
	if (ret)
		return ret;

	/*
	 * Try to add them in sequence order. Really with driver model we
	 * should allow holes, but the current MMC list does not allow that.
	 * So if we request 0, 1, 3 we will get 0, 1, 2.
	 */
	for (i = 0; ; i++) {
		ret = uclass_get_device_by_seq(UCLASS_MMC, i, &dev);
		if (ret == -ENODEV)
			break;
	}
	uclass_foreach_dev(dev, uc) {
		ret = device_probe(dev);
		if (ret)
			pr_err("%s - probe failed: %d\n", dev->name, ret);
	}

	return 0;
}


static int ramblock_bind(struct udevice *dev)
{
	struct blk_desc *bdesc;
	struct udevice *bdev;
	int ret, devnum = -1;

	ret = dev_read_alias_seq(dev, &devnum);
	debug("%s: alias ret=%d, devnum=%d\n", __func__, ret, devnum);
	
	ret = blk_create_devicef(dev, "rambox_blk", "blk", IF_TYPE_RAMBLOCK,
			devnum, 512, 0, &bdev);

	if (ret) {
		printf("Cannot create block device\n");
		return ret;
	}

	bdesc = dev_get_uclass_platdata(bdev);

	bdesc->removable = 1;

	bdesc->part_type = IF_TYPE_RAMBLOCK;

	return 0;
}
#endif

static int ramblock_probe(struct udevice *dev)
{
	struct ram_block_dev *plat = dev_get_platdata(dev);

	fdt_addr_t addr;

	char buf[8*2];

	addr = dev_read_addr(dev);

	plat->begin_addr = (unsigned long)addr;

	sprintf(buf, "0x%lx", addr);

	env_set("ramblock_addr",buf);

	return 0;
}

#ifdef CONFIG_BLK

static const struct udevice_id ramblock_ids[] = {
	{ .compatible = "fsl,ramblock", },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(fsl_ramblock) = {
	.name		= "fsl_ramblock",
	.id		= UCLASS_RAMBLOCK,
	.of_match = ramblock_ids,
#if CONFIG_IS_ENABLED(BLK)
	.bind	= ramblock_bind,
#endif
	.probe	= ramblock_probe,
	.platdata_auto_alloc_size = sizeof(struct ram_block_dev),
};
#else
U_BOOT_LEGACY_BLK(sandbox_host) = {
	.if_typename	= "ram",
	.if_type	= IF_TYPE_HOST,
	.max_devs	= CONFIG_HOST_MAX_DEVICES,
	.get_dev	= host_get_dev_err,
};
#endif
