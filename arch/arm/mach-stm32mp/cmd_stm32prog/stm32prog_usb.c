// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <console.h>
#include <dfu.h>
#include <g_dnl.h>
#include <usb.h>
#include <watchdog.h>
#include "stm32prog.h"

struct stm32prog_data *stm32prog_data;

static int stm32prog_get_alternate(struct stm32prog_data *data)
{
	if (data->cur_part)
		return data->cur_part->alt_id;
	else
		return -EINVAL;
}

static int stm32prog_set_phase(struct stm32prog_data *data, u8 phase,
			       u32 offset)
{
	struct stm32prog_part_t *part;
	int i;

	if (phase == data->phase) {
		data->offset = offset;
		data->dfu_seq = 0;
		return 0;
	}

	/* found partition */
	for (i = 0; i < data->part_nb; i++) {
		part = &data->part_array[i];
		if (part->id == phase) {
			data->cur_part = part;
			data->phase = phase;
			data->offset = offset;
			data->dfu_seq = 0;
			return 0;
		}
	}

	return  -EINVAL;
}

static int stm32prog_cmd_write(u64 offset, void *buf, long *len)
{
	u8 phase;
	u32 address;
	u8 *pt = buf;
	void (*entry)(void);
	int ret;

	if (*len < 5) {
		pr_err("size not allowed\n");
		return  -EINVAL;
	}
	if (offset) {
		pr_err("invalid offset\n");
		return  -EINVAL;
	}
	phase = pt[0];
	address = (pt[1] << 24) | (pt[2] << 16) | (pt[3] << 8) | pt[4];
	if (phase == PHASE_RESET) {
		entry = (void *)address;
		printf("## Starting application at 0x%x ...\n", address);
		(*entry)();
		printf("## Application terminated\n");
		return 0;
	}
	/* set phase and offset */
	ret = stm32prog_set_phase(stm32prog_data, phase, address);
	if (ret)
		pr_err("failed: %d\n", ret);
	return ret;
}

#define PHASE_MIN_SIZE	9
static int stm32prog_cmd_read(u64 offset, void *buf, long *len)
{
	u32 destination = DEFAULT_ADDRESS; /* destination address */
	u32 dfu_offset;
	u8 *pt_buf = buf;
	int phase;
	char *err_msg;
	int length;

	if (*len < PHASE_MIN_SIZE) {
		pr_err("request exceeds allowed area\n");
		return  -EINVAL;
	}
	if (offset) {
		*len = 0; /* EOF for second request */
		return 0;
	}
	phase = stm32prog_data->phase;
	if (phase == PHASE_FLASHLAYOUT)
		destination = STM32_DDR_BASE;
	dfu_offset = stm32prog_data->offset;

	/* mandatory header, size = PHASE_MIN_SIZE */
	*pt_buf++ = (u8)(phase & 0xFF);
	*pt_buf++ = (u8)(destination);
	*pt_buf++ = (u8)(destination >> 8);
	*pt_buf++ = (u8)(destination >> 16);
	*pt_buf++ = (u8)(destination >> 24);
	*pt_buf++ = (u8)(dfu_offset);
	*pt_buf++ = (u8)(dfu_offset >> 8);
	*pt_buf++ = (u8)(dfu_offset >> 16);
	*pt_buf++ = (u8)(dfu_offset >> 24);

	if (phase == PHASE_RESET || phase == PHASE_DO_RESET) {
		err_msg = stm32prog_get_error(stm32prog_data);
		length = strlen(err_msg);
		if (length + PHASE_MIN_SIZE > *len)
			length = *len - PHASE_MIN_SIZE;

		memcpy(pt_buf, err_msg, length);
		*len = PHASE_MIN_SIZE + length;
		stm32prog_do_reset(stm32prog_data);
	} else if (phase == PHASE_FLASHLAYOUT) {
		*pt_buf++ = stm32prog_data->part_nb ? 1 : 0;
		*len = PHASE_MIN_SIZE + 1;
	} else {
		*len = PHASE_MIN_SIZE;
	}

	return 0;
}

/* DFU access to virtual partition */

void dfu_flush_callback(struct dfu_entity *dfu)
{
	if (!stm32prog_data)
		return;

	if (dfu->dev_type == DFU_DEV_VIRT) {
		if (dfu->data.virt.dev_num == PHASE_OTP)
			stm32prog_otp_start(stm32prog_data);
		else if (dfu->data.virt.dev_num == PHASE_PMIC)
			stm32prog_pmic_start(stm32prog_data);
		return;
	}

	if (dfu->dev_type == DFU_DEV_RAM) {
		if (dfu->alt == 0 &&
		    stm32prog_data->phase == PHASE_FLASHLAYOUT) {
			stm32prog_end_phase(stm32prog_data);
			/* waiting DFU DETACH for reenumeration */
		}
		return;
	}

	if (dfu->alt == stm32prog_get_alternate(stm32prog_data)) {
		stm32prog_end_phase(stm32prog_data);
		stm32prog_next_phase(stm32prog_data);
	}
}

void dfu_initiated_callback(struct dfu_entity *dfu)
{
	int phase;

	if (!stm32prog_data)
		return;

	phase = stm32prog_data->phase;
	if (dfu->alt == stm32prog_get_alternate(stm32prog_data)) {
		dfu->offset = stm32prog_data->offset;
		stm32prog_set_phase(stm32prog_data, phase, 0);
		pr_debug("dfu offset = 0x%llx\n", dfu->offset);
	}
}

int dfu_write_medium_virt(struct dfu_entity *dfu, u64 offset,
			  void *buf, long *len)
{
	if (dfu->dev_type != DFU_DEV_VIRT)
		return -EINVAL;

	switch (dfu->data.virt.dev_num) {
	case PHASE_CMD:
		return stm32prog_cmd_write(offset, buf, len);

	case PHASE_OTP:
		return stm32prog_otp_write(stm32prog_data, (u32)offset,
					   buf, len);

	case PHASE_PMIC:
		return stm32prog_pmic_write(stm32prog_data, (u32)offset,
					    buf, len);
	}
	*len = 0;
	return 0;
}

int dfu_read_medium_virt(struct dfu_entity *dfu, u64 offset,
			 void *buf, long *len)
{
	if (dfu->dev_type != DFU_DEV_VIRT)
		return -EINVAL;

	switch (dfu->data.virt.dev_num) {
	case PHASE_CMD:
		return stm32prog_cmd_read(offset, buf, len);

	case PHASE_OTP:
		return stm32prog_otp_read(stm32prog_data, (u32)offset,
					  buf, len);

	case PHASE_PMIC:
		return stm32prog_pmic_read(stm32prog_data, (u32)offset,
					   buf, len);
	}
	*len = 0;
	return 0;
}

int dfu_get_medium_size_virt(struct dfu_entity *dfu, u64 *size)
{
	if (dfu->dev_type != DFU_DEV_VIRT) {
		*size = 0;
		pr_debug("%s, invalid dev_type = %d\n",
			 __func__, dfu->dev_type);
		return -EINVAL;
	}

	switch (dfu->data.virt.dev_num) {
	case PHASE_CMD:
		*size = 512;
		break;
	case PHASE_OTP:
		*size = OTP_SIZE;
		break;
	case PHASE_PMIC:
		*size = PMIC_SIZE;
		break;
	}

	return 0;
}

/* USB download gadget for STM32 Programmer */

static const char product[] =
	"USB download gadget@Device ID /0x500, @Revision ID /0x0000";

bool stm32prog_usb_loop(struct stm32prog_data *data, int dev)
{
	int ret;

	stm32prog_data = data;
	g_dnl_set_product(product);
	if (stm32prog_data->phase == PHASE_FLASHLAYOUT) {
		ret = run_usb_dnl_gadget(dev, "usb_dnl_dfu");
		if (ret || stm32prog_data->phase == PHASE_DO_RESET)
			return ret;
		/* prepare the second enumeration with the FlashLayout */
		if (stm32prog_data->phase == PHASE_FLASHLAYOUT)
			stm32prog_dfu_init(data);
		/* found next selected partition */
		stm32prog_next_phase(data);
	}
	return (run_usb_dnl_gadget(dev, "usb_dnl_dfu") ||
		(stm32prog_data->phase == PHASE_DO_RESET));
}

int g_dnl_get_board_bcd_device_number(int gcnum)
{
	pr_debug("%s\n", __func__);
	return 0x200;
}
