// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Google, Inc
 * Copyright 2020 NXP
 * Written by Simon Glass <sjg@chromium.org>
 */

#include <common.h>
#include <log.h>
#include <mmc.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <dm/device.h>
#include <dm/lists.h>
#include <linux/compat.h>
#include <ramblockdev.h>
#include <mapmem.h>
#ifdef CONFIG_BLK
static unsigned long rambox_blk_read(struct udevice *dev,
				     unsigned long start, lbaint_t blkcnt,
				     void *buffer)
{
	unsigned long lseek;

	struct udevice* ramblock_dev = dev_get_parent(dev);

	struct ram_block_dev *plat = dev_get_platdata(ramblock_dev);

	struct blk_desc *block_dev = dev_get_uclass_platdata(dev);

	lseek = plat->begin_addr + start*block_dev->blksz;

	memcpy((char*)buffer,(char*)lseek,blkcnt*block_dev->blksz);

	return blkcnt;
};

static unsigned long rambox_blk_write(struct udevice *dev,
				      unsigned long start, lbaint_t blkcnt,
				      const void *buffer)
{
	unsigned long lseek;

	struct udevice* ramblock_dev = dev_get_parent(dev);

	struct ram_block_dev *plat = dev_get_platdata(ramblock_dev);

	struct blk_desc *block_dev = dev_get_uclass_platdata(dev);

	lseek = plat->begin_addr + start*block_dev->blksz;

	memcpy((char*)lseek,(char*)buffer,blkcnt*block_dev->blksz);

	return blkcnt;
};

static const struct blk_ops rambox_blk_ops = {
	.read	= rambox_blk_read,
	.write	= rambox_blk_write,
};

U_BOOT_DRIVER(rambox_blk) = {
	.name		= "rambox_blk",
	.id		= UCLASS_BLK,
	.ops		= &rambox_blk_ops,
};
#endif /* CONFIG_BLK */


UCLASS_DRIVER(ramblock) = {
	.id		= UCLASS_RAMBLOCK,
	.name		= "ramblock",
	.flags		= DM_UC_FLAG_SEQ_ALIAS,
};
