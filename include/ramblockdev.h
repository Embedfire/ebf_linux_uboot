/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2013, Henrik Nordstrom <henrik@henriknordstrom.net>
 */

#ifndef __RAMBLOCK_DEV__
#define __RAMBLOCK_DEV__

struct ram_block_dev {
#ifndef CONFIG_BLK
	struct blk_desc blk_dev;
#endif
	unsigned long begin_addr;
};

int ramblock_init(void);

#endif
