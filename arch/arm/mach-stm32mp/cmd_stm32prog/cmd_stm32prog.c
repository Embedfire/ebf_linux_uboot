// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <command.h>
#include <dfu.h>
#include "stm32prog.h"

DECLARE_GLOBAL_DATA_PTR;

static void enable_vidconsole(void)
{
#ifdef CONFIG_DM_VIDEO
	char *stdname;
	char buf[64];

	stdname = env_get("stdout");
	if (!strstr(stdname, "vidconsole")) {
		snprintf(buf, sizeof(buf), "%s,vidconsole", stdname);
		env_set("stdout", buf);
	}

	stdname = env_get("stderr");
	if (!strstr(stdname, "vidconsole")) {
		snprintf(buf, sizeof(buf), "%s,vidconsole", stdname);
		env_set("stderr", buf);
	}
#endif
}

static int do_stm32prog(cmd_tbl_t *cmdtp, int flag, int argc,
			char * const argv[])
{
	struct stm32prog_data *data;
	ulong	addr, size;
	int dev;
	enum stm32prog_link_t link = LINK_UNDEFINED;
	bool reset = false;

	if (argc < 3 ||  argc > 5)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "serial")) {
		link = LINK_SERIAL;
	} else {
		if (!strcmp(argv[1], "usb")) {
			link = LINK_USB;
		} else {
			pr_err("not supported link=%s\n", argv[1]);
			return CMD_RET_USAGE;
		}
	}
	dev = (int)simple_strtoul(argv[2], NULL, 10);

	addr = STM32_DDR_BASE;
	size = 0;
	if (argc > 3) {
		addr = simple_strtoul(argv[3], NULL, 16);
		if (!addr)
			return CMD_RET_FAILURE;
	}
	if (argc > 4)
		size = simple_strtoul(argv[4], NULL, 16);

	enable_vidconsole();

	data = stm32prog_init(link, dev, addr, size);
	if (!data)
		return CMD_RET_FAILURE;

	switch (link) {
	case LINK_SERIAL:
		reset = stm32prog_serial_loop(data);
		break;
	case LINK_USB:
		reset = stm32prog_usb_loop(data, dev);
		break;
	default:
		break;
	}

	stm32prog_clean(data);

	puts("Download done\n");
	if (reset) {
		puts("Reset...\n");
		run_command("reset", 0);
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(stm32prog, 5, 0, do_stm32prog,
	   "<link> <dev> [<addr>] [<size>]\n"
	   "start communication with tools STM32Cubeprogrammer on <link> with Flashlayout at <addr>",
	   "<link> = serial|usb\n"
	   "<dev>  = device instance\n"
	   "<addr> = address of flashlayout\n"
	   "<size> = size of flashlayout\n"
);
