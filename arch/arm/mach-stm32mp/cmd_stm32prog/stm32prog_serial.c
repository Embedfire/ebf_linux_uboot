// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <console.h>
#include <dfu.h>
#include <malloc.h>
#include <serial.h>
#include <watchdog.h>
#include <dm/lists.h>
#include <dm/device-internal.h>
#include "stm32prog.h"

struct udevice *down_serial_dev;

const u8 cmd_id[] = {
	GET_CMD_COMMAND,
	GET_VER_COMMAND,
	GET_ID_COMMAND,
	GET_PHASE_COMMAND,
	RM_COMMAND,
	READ_PART_COMMAND,
	START_COMMAND,
	DOWNLOAD_COMMAND
};

#define NB_CMD sizeof(cmd_id)

/* DFU support for serial *********************************************/
struct dfu_entity *stm32prog_get_entity(struct stm32prog_data *data)
{
	int alt_id;

	if (!data->cur_part)
		if (data->phase == PHASE_FLASHLAYOUT)
			alt_id = 0;
		else
			return NULL;
	else
		alt_id = data->cur_part->alt_id;

	return dfu_get_entity(alt_id);
}

static int stm32prog_write(struct stm32prog_data *data, u8 *buffer,
			   u32 buffer_size)
{
	struct dfu_entity *dfu_entity;
	u8 ret = 0;

	dfu_entity = stm32prog_get_entity(data);
	if (!dfu_entity)
		return -ENODEV;

	ret = dfu_write(dfu_entity,
			buffer,
			buffer_size,
			data->dfu_seq);

	if (ret) {
		stm32prog_err("DFU write failed [%d] cnt: %d",
			      ret, data->dfu_seq);
	}
	data->dfu_seq++;
	/* handle rollover as in driver/dfu/dfu.c */
	data->dfu_seq &= 0xffff;
	if (buffer_size == 0)
		data->dfu_seq = 0; /* flush done */

	return ret;
}

static int stm32prog_read(struct stm32prog_data *data, u8 phase, u32 offset,
			  u8 *buffer, u32 buffer_size)
{
	struct dfu_entity *dfu_entity;
	int ret;

	/* pr_debug("%s entry\n", __func__); */
	if (data->dfu_seq) {
		stm32prog_err("DFU write pending for phase %d, seq %d",
			      data->phase, data->dfu_seq);
		return -EINVAL;
	}
	if (phase == PHASE_FLASHLAYOUT || phase > PHASE_LAST_USER) {
		stm32prog_err("read failed : phase %d is invalid", phase);
		return -EINVAL;
	}
	if (data->read_phase <= PHASE_LAST_USER &&
	    phase != data->read_phase) {
		/* clear previous read session */
		dfu_entity = dfu_get_entity(data->read_phase - 1);
		if (dfu_entity)
			dfu_transaction_cleanup(dfu_entity);
	}
	dfu_entity = dfu_get_entity(phase - 1);
	if (!dfu_entity) {
		stm32prog_err("read failed : phase %d is unknown", phase);
		return -ENODEV;
	}
	/* clear pending read before to force offset */
	if (dfu_entity->inited &&
	    (data->read_phase != phase || data->offset != offset))
		dfu_transaction_cleanup(dfu_entity);

	/* initiate before to force offset */
	if (!dfu_entity->inited) {
		ret = dfu_transaction_initiate(dfu_entity, true);
			if (ret < 0) {
				stm32prog_err("DFU read init failed [%d] phase = %d offset = 0x%08x",
					      ret, phase, offset);
			return ret;
		}
	}
	/* force new offset */
	if (dfu_entity->offset != offset)
		dfu_entity->offset = offset;
	data->offset = offset;
	data->read_phase = phase;
	pr_debug("\nSTM32 download read %s offset=0x%x\n",
		 dfu_entity->name, offset);
	ret = dfu_read(dfu_entity, buffer, buffer_size,
		       dfu_entity->i_blk_seq_num);
	if (ret < 0) {
		stm32prog_err("DFU read failed [%d] phase = %d offset = 0x%08x",
			      ret, phase, offset);
		return ret;
	}
	if (ret < buffer_size) {
		data->offset = 0;
		data->read_phase = PHASE_END;
		memset(buffer + ret, 0, buffer_size - ret);
	} else {
		data->offset += ret;
	}
	/*pr_debug("%s exit ret=%d\n", __func__, ret);*/
	return ret;
}

/* UART access ***************************************************/
int stm32prog_serial_init(struct stm32prog_data *data, int link_dev)
{
	struct udevice *dev = NULL;
	int node;
	char alias[10];
	const char *path;
	struct dm_serial_ops *ops;
	/* no parity, 8 bits, 1 stop */
	u32 serial_config = SERIAL_DEFAULT_CONFIG;

	down_serial_dev = NULL;

	sprintf(alias, "serial%d", link_dev);
	path = fdt_get_alias(gd->fdt_blob, alias);
	if (!path) {
		pr_err("%s alias not found", alias);
		return -ENODEV;
	}
	node = fdt_path_offset(gd->fdt_blob, path);
	if (!uclass_get_device_by_of_offset(UCLASS_SERIAL, node,
					    &dev)) {
		down_serial_dev = dev;
	} else if (node > 0 &&
		   !lists_bind_fdt(gd->dm_root, offset_to_ofnode(node),
				   &dev)) {
		if (!device_probe(dev))
			down_serial_dev = dev;
	}
	/*pr_debug("alias=%s, path=%s, node = %d, dev=%0x\n",
	 *	 alias, path, node, (u32)down_serial_dev);
	 */
	if (!down_serial_dev) {
		pr_err("%s = %s device not found", alias, path);
		return -ENODEV;
	}

	/* force silent console on uart only when used */
	if (gd->cur_serial_dev == down_serial_dev)
		gd->flags |= GD_FLG_DISABLE_CONSOLE | GD_FLG_SILENT;
	else
		gd->flags &= ~(GD_FLG_DISABLE_CONSOLE | GD_FLG_SILENT);

	ops = serial_get_ops(down_serial_dev);

	if (!ops) {
		pr_err("%s = %s missing ops", alias, path);
		return -ENODEV;
	}
	if (!ops->setconfig) {
		pr_err("%s = %s missing setconfig", alias, path);
		return -ENODEV;
	}

	clrsetbits_le32(&serial_config, SERIAL_PAR_MASK, SERIAL_PAR_EVEN);

	return ops->setconfig(down_serial_dev, serial_config);
}

static void stm32prog_serial_flush(void)
{
	struct dm_serial_ops *ops = serial_get_ops(down_serial_dev);
	int err;

	do {
		err = ops->getc(down_serial_dev);
	} while (err != -EAGAIN);
}

static int stm32prog_serial_getc_err(void)
{
	struct dm_serial_ops *ops = serial_get_ops(down_serial_dev);
	int err;

	do {
		err = ops->getc(down_serial_dev);
		if (err == -EAGAIN)
			ctrlc();
	} while ((err == -EAGAIN) && (!had_ctrlc()));

	return err;
}

static u8 stm32prog_serial_getc(void)
{
	int err;

	err = stm32prog_serial_getc_err();

	return err >= 0 ? err : 0;
}

static bool stm32prog_serial_get_buffer(u8 *buffer, u32 *count)
{
	struct dm_serial_ops *ops = serial_get_ops(down_serial_dev);
	int err;

	do {
		err = ops->getc(down_serial_dev);
		if (err >= 0) {
			*buffer++ = err;
			*count -= 1;
		} else if (err == -EAGAIN) {
			ctrlc();
		} else {
			break;
		}
	} while (*count && !had_ctrlc());

	return !!(err < 0);
}

static void stm32prog_serial_putc(u8 w_byte)
{
	struct dm_serial_ops *ops = serial_get_ops(down_serial_dev);
	int err;

	do {
		err = ops->putc(down_serial_dev, w_byte);
	} while (err == -EAGAIN);
}

/* Helper function ************************************************/

static u8 stm32prog_header(struct stm32prog_data *data)
{
	u8 ret;
	u8 boot = 0;
	struct dfu_entity *dfu_entity;
	u64 size = 0;

	/*pr_debug("%s entry\n", __func__);*/

	dfu_entity = stm32prog_get_entity(data);
	if (!dfu_entity)
		return -ENODEV;

	printf("\nSTM32 download write %s\n", dfu_entity->name);

	/* force cleanup to avoid issue with previous read */
	dfu_transaction_cleanup(dfu_entity);

	ret = stm32prog_header_check(data->header_data,
				     &data->header);

	/* no header : max size is partition size */
	if (ret) {
		dfu_entity->get_medium_size(dfu_entity, &size);
		data->header.image_length = size;
	}

	/**** Flash the header if necessary for boot partition */
	if (data->phase < PHASE_FIRST_USER)
		boot = 1;

	/* write header if boot partition */
	if (boot) {
		if (ret) {
			stm32prog_err("invalid header (error %d)", ret);
		} else {
			ret = stm32prog_write(data,
					      (u8 *)data->header_data,
					      BL_HEADER_SIZE);
		}
	} else {
		if (ret)
			printf("  partition without checksum\n");
		ret = 0;
	}

	free(data->header_data);
	data->header_data = NULL;
	/*pr_debug("%s result=%d\n", __func__, ret);*/
	return ret;
}

static u8 stm32prog_start(struct stm32prog_data *data, u32 address)
{
	u8 ret = 0;
	struct dfu_entity *dfu_entity;

	/*pr_debug("%s entry\n", __func__);*/

	if (address < 0x100) {
		if (address == PHASE_OTP)
			return stm32prog_otp_start(data);

		if (address == PHASE_PMIC)
			return stm32prog_pmic_start(data);

		if (address == PHASE_RESET || address == PHASE_END) {
			data->cur_part = NULL;
			data->dfu_seq = 0;
			data->phase = address;
			return 0;
		}
		if (address != data->phase) {
			stm32prog_err("invalid received phase id %d, current phase is %d",
				      (u8)address, (u8)data->phase);
			return -EINVAL;
		}
	}
	/* check the last loaded partition */
	if (address == DEFAULT_ADDRESS || address == data->phase) {
		switch (data->phase) {
		case PHASE_END:
		case PHASE_RESET:
		case PHASE_DO_RESET:
			data->cur_part = NULL;
			data->phase = PHASE_DO_RESET;
			return 0;
		}
		dfu_entity = stm32prog_get_entity(data);
		if (!dfu_entity)
			return -ENODEV;

		if (data->dfu_seq) {
			ret = dfu_flush(dfu_entity, NULL, 0, data->dfu_seq);
			data->dfu_seq = 0;
			if (ret) {
				stm32prog_err("DFU flush failed [%d]", ret);
				return ret;
			}
		}
		printf("\n  received length = 0x%x\n", data->cursor);
		if (data->header.present) {
			if (data->cursor !=
			    (data->header.image_length + BL_HEADER_SIZE)) {
				stm32prog_err("transmission interrupted (length=0x%x expected=0x%x)",
					      data->cursor,
					      data->header.image_length +
					      BL_HEADER_SIZE);
				return -EIO;
			}
			if (data->header.image_checksum != data->checksum) {
				stm32prog_err("invalid checksum received (0x%x expected 0x%x)",
					      data->checksum,
					      data->header.image_checksum);
				return -EIO;
			}
			printf("\n  checksum OK (0x%x)\n", data->checksum);
		}

		stm32prog_end_phase(data);
		/* update DFU with received flashlayout */
		if (data->phase == PHASE_FLASHLAYOUT)
			stm32prog_dfu_init(data);
		/* found next selected partition */
		stm32prog_next_phase(data);
	} else {
		void (*entry)(void) = (void *)address;

		printf("## Starting application at 0x%x ...\n", address);
		(*entry)();
		printf("## Application terminated\n");
		ret = -ENOEXEC;
	}
	/*pr_debug("%s exit ret=%d, phase=0x%x, add=0x%x\n", __func__,
	 *	 ret, data->phase, address);
	 */
	return ret;
}

/*
 * Function Name  : get_address
 * Description    : Get address if it is valid
 * Input          : None
 * Output         : None
 * Return         : The address area or Error_32
 */
static u32 get_address(u8 *tmp_xor)
{
	u32 address = 0x0;
	u8 data;

	data = stm32prog_serial_getc();
	*tmp_xor ^= data;
	address |= ((u32)data) << 24;

	data = stm32prog_serial_getc();
	address |= ((u32)data) << 16;
	*tmp_xor ^= data;

	data = stm32prog_serial_getc();
	address |= ((u32)data) << 8;
	*tmp_xor ^= data;

	data = stm32prog_serial_getc();
	address |= ((u32)data);
	*tmp_xor ^= data;

	return address;
}

static void stm32prog_serial_result(u8 result)
{
	/* always flush fifo before to send result */
	stm32prog_serial_flush();
	stm32prog_serial_putc(result);
}

/* Command -----------------------------------------------*/
/*
 * Function Name  : get_cmd_command
 * Description    : Respond to Get command
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void get_cmd_command(struct stm32prog_data *data)
{
	u32 counter = 0x0;

	stm32prog_serial_putc(NB_CMD);
	stm32prog_serial_putc(USART_BL_VERSION);

	for (counter = 0; counter < NB_CMD; counter++)
		stm32prog_serial_putc(cmd_id[counter]);

	stm32prog_serial_result(ACK_BYTE);
}

/*
 * Function Name  : get_version_command
 * Description    : Respond to Get Version command
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void get_version_command(struct stm32prog_data *data)
{
	stm32prog_serial_putc(UBOOT_BL_VERSION);
	stm32prog_serial_result(ACK_BYTE);
}

/*
 * Function Name  : get_id_command
 * Description    : Respond to Get ID command
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void get_id_command(struct stm32prog_data *data)
{
	/* Send Device IDCode */
	stm32prog_serial_putc(0x1);
	stm32prog_serial_putc(DEVICE_ID_BYTE1);
	stm32prog_serial_putc(DEVICE_ID_BYTE2);
	stm32prog_serial_result(ACK_BYTE);
}

/*
 * Function Name  : get_phase_command
 * Description    : Respond to Get phase
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void get_phase_command(struct stm32prog_data *data)
{
	char *err_msg = NULL;
	u8 i, length = 0;
	u32 destination = DEFAULT_ADDRESS; /* destination address */
	int phase = data->phase;

	if (phase == PHASE_RESET || phase == PHASE_DO_RESET) {
		err_msg = stm32prog_get_error(data);
		length = strlen(err_msg);
	}
	if (phase == PHASE_FLASHLAYOUT)
		destination = STM32_DDR_BASE;

	stm32prog_serial_putc(length + 5);           /* Total length */
	stm32prog_serial_putc(phase & 0xFF);         /* partition ID */
	stm32prog_serial_putc(destination);          /* byte 1 of address */
	stm32prog_serial_putc(destination >> 8);     /* byte 2 of address */
	stm32prog_serial_putc(destination >> 16);    /* byte 3 of address */
	stm32prog_serial_putc(destination >> 24);    /* byte 4 of address */

	stm32prog_serial_putc(length);               /* Information length */
	for (i = 0; i < length; i++)
		stm32prog_serial_putc(err_msg[i]);
	stm32prog_serial_result(ACK_BYTE);

	if (phase == PHASE_RESET)
		stm32prog_do_reset(data);
}

/*
 * Function Name  : read_memory_command
 * Description    : Read data from memory
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void read_memory_command(struct stm32prog_data *data)
{
	u32 address = 0x0;
	u8 rcv_data = 0x0, tmp_xor = 0x0;
	u32 counter = 0x0;

	/* Read memory address */
	address = get_address(&tmp_xor);

	/* If address memory is not received correctly */
	rcv_data = stm32prog_serial_getc();
	if (rcv_data != tmp_xor) {
		stm32prog_serial_result(NACK_BYTE);
		return;
	}

	stm32prog_serial_result(ACK_BYTE);

	/* Read the number of bytes to be received:
	 * Max NbrOfData = Data + 1 = 256
	 */
	rcv_data = stm32prog_serial_getc();
	tmp_xor = ~rcv_data;
	if (stm32prog_serial_getc() != tmp_xor) {
		stm32prog_serial_result(NACK_BYTE);
		return;
	}

	/* If checksum is correct send ACK */
	stm32prog_serial_result(ACK_BYTE);

	/* Send data to the host:
	 * Number of data to read = data + 1
	 */
	for (counter = (rcv_data + 1); counter != 0; counter--)
		stm32prog_serial_putc(*(u8 *)(address++));
}

/*
 * Function Name  : start_command
 * Description    : Jump to user application in RAM or partition check
 * Input          : None
 * Output         : None
 * Return         : None
 */
static void start_command(struct stm32prog_data *data)
{
	u32 address = 0;
	u8 tmp_xor = 0x0;
	u8 ret, rcv_data;

	/* Read memory address */
	address = get_address(&tmp_xor);

	/* If address memory is not received correctly */
	rcv_data = stm32prog_serial_getc();
	if (rcv_data != tmp_xor) {
		stm32prog_serial_result(NACK_BYTE);
		return;
	}
	/* validate partition */
	ret = stm32prog_start(data,
			      address);

	if (ret)
		stm32prog_serial_result(ABORT_BYTE);
	else
		stm32prog_serial_result(ACK_BYTE);
}

/*
 * Function Name  : download_command
 * Description    : Write data to Flash
 * Input          : None
 * Output         : None
 * Return         : Result
 */
static void download_command(struct stm32prog_data *data)
{
	u32 address = 0x0;
	u8 my_xor = 0x0;
	u8 rcv_xor;
	u32 counter = 0x0, codesize = 0x0;
	u8 *ramaddress = 0;
	u8 rcv_data = 0x0;
	struct image_header_s *image_header = &data->header;
	u32 cursor = data->cursor;
	long size = 0;
	u8 operation;
	u32 packet_number;
	u32 result = ACK_BYTE;
	u8 ret;
	int i;
	bool error;
	int rcv;

	address = get_address(&my_xor);

	/* If address memory is not received correctly */
	rcv_xor = stm32prog_serial_getc();
	if (rcv_xor != my_xor) {
		result = NACK_BYTE;
		goto end;
	}

	/* If address valid send ACK */
	stm32prog_serial_result(ACK_BYTE);

	/* get packet number and operation type */
	operation = (u8)((u32)address >> 24);
	packet_number = ((u32)(((u32)address << 8))) >> 8;

	switch (operation) {
	/* supported operation */
	case PHASE_FLASHLAYOUT:
	case PHASE_OTP:
	case PHASE_PMIC:
		break;
	default:
		result = NACK_BYTE;
		goto end;
	}
	/* check the packet number */
	if (packet_number == 0) {
		/* erase: re-initialize the image_header struct */
		data->packet_number = 0;
		if (data->header_data)
			memset(data->header_data, 0, BL_HEADER_SIZE);
		else
			data->header_data = calloc(1, BL_HEADER_SIZE);
		cursor = 0;
		data->cursor = 0;
		data->checksum = 0;
		/*idx = cursor;*/
	} else {
		data->packet_number++;
	}

	/* Check with the number of current packet if the device receive
	 * the true packet
	 */
	if (packet_number != data->packet_number) {
		data->packet_number--;
		result = NACK_BYTE;
		goto end;
	}

	/*-- Read number of bytes to be written and data -----------*/

	/* Read the number of bytes to be written:
	 * Max NbrOfData = data + 1 <= 256
	 */
	rcv_data = stm32prog_serial_getc();

	/* NbrOfData to write = data + 1 */
	codesize = rcv_data + 0x01;

	if (codesize > USART_RAM_BUFFER_SIZE) {
		result = NACK_BYTE;
		goto end;
	}

	/* Checksum Initialization */
	my_xor = rcv_data;

	/* UART receive data and send to Buffer */
	counter = codesize;
	error = stm32prog_serial_get_buffer(data->buffer, &counter);

	/* read checksum */
	if (!error) {
		rcv = stm32prog_serial_getc_err();
		error = !!(rcv < 0);
		rcv_xor = rcv;
	}

	if (error) {
		printf("transmission error on packet %d, byte %d\n",
		       packet_number, codesize - counter);
		/* waiting end of packet before flush & NACK */
		mdelay(30);
		data->packet_number--;
		result = NACK_BYTE;
		goto end;
	}

	/* Compute Checksum */
	ramaddress = data->buffer;
	for (counter = codesize; counter != 0; counter--)
		my_xor ^= *(ramaddress++);

	/* If Checksum is incorrect */
	if (rcv_xor != my_xor) {
		printf("checksum error on packet %d\n",
		       packet_number);
		data->packet_number--;
		result = NACK_BYTE;
		goto end;
	}

	/* Update current position in buffer */
	data->cursor += codesize;

	if (operation == PHASE_OTP) {
		size = data->cursor - cursor;
		/* no header for OTP */
		if (stm32prog_otp_write(data, cursor,
					data->buffer, &size))
			result = ABORT_BYTE;
		goto end;
	}

	if (operation == PHASE_PMIC) {
		size = data->cursor - cursor;
		/* no header for PMIC */
		if (stm32prog_pmic_write(data, cursor,
					 data->buffer, &size))
			result = ABORT_BYTE;
		goto end;
	}

	if (cursor < BL_HEADER_SIZE) {
		/* size = portion of header in this chunck */
		if (data->cursor >= BL_HEADER_SIZE)
			size = BL_HEADER_SIZE - cursor;
		else
			size = data->cursor - cursor;
		memcpy((void *)((u32)(data->header_data) + cursor),
		       data->buffer, size);
		cursor += size;

		if (cursor == BL_HEADER_SIZE) {
			/* Check and Write the header */
			if (stm32prog_header(data)) {
				result = ABORT_BYTE;
				goto end;
			}
		} else {
			goto end;
		}
	}
	/*
	 * pr_debug("packet_number = 0x%x\n", packet_number);
	 * pr_debug("cursor = 0x%x\n", data->cursor);
	 * pr_debug("image_length = 0x%x\n", image_header->image_length);
	 * pr_debug("codesize = 0x%x\n", codesize);
	 */
	if (image_header->present) {
		if (data->cursor <= BL_HEADER_SIZE)
			goto end;
		/* compute checksum on payload */
		for (i = size; i < codesize; i++)
			data->checksum += data->buffer[i];

		if (data->cursor >
		    image_header->image_length + BL_HEADER_SIZE) {
			pr_err("expected size exceeded\n");
			result = ABORT_BYTE;
			goto end;
		}

		/* write data (payload) */
		ret = stm32prog_write(data,
				      &data->buffer[size],
				      codesize - size);
	} else {
		/* write all */
		ret = stm32prog_write(data,
				      data->buffer,
				      codesize);
	}
	if (ret)
		result = ABORT_BYTE;

end:
	/*pr_debug("%s : result = 0x%x\n", __func__, result);*/
	stm32prog_serial_result(result);
}

/*
 * Function Name  : read_partition
 * Description    : read data from Flash
 * Input          : None
 * Output         : None
 * Return         : Result
 */
static void read_partition_command(struct stm32prog_data *data)
{
	u32 part_id, codesize, offset = 0, rcv_data;
	long size;
	u8 tmp_xor;
	int i, res;
	u8 buffer[256];

	part_id = stm32prog_serial_getc();
	tmp_xor = part_id;

	offset = get_address(&tmp_xor);

	rcv_data = stm32prog_serial_getc();
	if (rcv_data != tmp_xor) {
		pr_debug("1st checksum received = %x, computed %x\n",
			 rcv_data, tmp_xor);
		goto error;
	}
	stm32prog_serial_putc(ACK_BYTE);

	/* NbrOfData to read = data + 1 */
	rcv_data = stm32prog_serial_getc();
	codesize = rcv_data + 0x01;
	tmp_xor = rcv_data;

	rcv_data = stm32prog_serial_getc();
	if ((rcv_data ^ tmp_xor) != 0xFF) {
		pr_debug("2nd checksum received = %x, computed %x\n",
			 rcv_data, tmp_xor);
		goto error;
	}

	pr_debug("%s : %x\n", __func__, part_id);
	switch (part_id) {
	case PHASE_OTP:
		res = 0;
		size = codesize;
		if (!stm32prog_otp_read(data, offset, buffer, &size))
			res = size;
		break;
	case PHASE_PMIC:
		res = 0;
		size = codesize;
		if (!stm32prog_pmic_read(data, offset, buffer, &size))
			res = size;
		break;
	default:
		res = stm32prog_read(data, part_id, offset,
				     buffer, codesize);
		break;
	}
	if (res > 0) {
		stm32prog_serial_putc(ACK_BYTE);
		/*----------- Send data to the host -----------*/
		for (i = 0; i < res; i++)
			stm32prog_serial_putc(buffer[i]);
		/*----------- Send filler to the host -----------*/
		for (; i < codesize; i++)
			stm32prog_serial_putc(0x0);
		return;
	}
	stm32prog_serial_result(ABORT_BYTE);
	return;

error:
	stm32prog_serial_result(NACK_BYTE);
}

/** SERIAL LOOP ****************************************************/

/*
 * Function Name  : stm32prog_serial_loop
 * Description    : USART bootloader Loop routine
 * Input          : data
 * Output         : None
 * Return         : None
 */
bool stm32prog_serial_loop(struct stm32prog_data *data)
{
	u32 counter = 0x0;
	u8 command = 0x0;
	u8 found;
	int phase = data->phase;

	/* element of cmd_func need to aligned with cmd_id[]*/
	void (*cmd_func[NB_CMD])(struct stm32prog_data *) = {
		/* GET_CMD_COMMAND */	get_cmd_command,
		/* GET_VER_COMMAND */	get_version_command,
		/* GET_ID_COMMAND */	get_id_command,
		/* GET_PHASE_COMMAND */	get_phase_command,
		/* RM_COMMAND */	read_memory_command,
		/* READ_PART_COMMAND */	read_partition_command,
		/* START_COMMAND */	start_command,
		/* DOWNLOAD_COMMAND */	download_command
	};

	/* flush and NACK pending command received during u-boot init
	 * request command reemit
	 */
	stm32prog_serial_result(NACK_BYTE);

	clear_ctrlc(); /* forget any previous Control C */
	while (!had_ctrlc()) {
		phase = data->phase;

		if (phase == PHASE_DO_RESET)
			return true;

		/* Get the user command: read first byte */
		command = stm32prog_serial_getc();

		if (command == INIT_BYTE) {
			puts("\nConnected\n");
			stm32prog_serial_result(ACK_BYTE);
			continue;
		}

		found = 0;
		for (counter = 0; counter < NB_CMD; counter++)
			if (cmd_id[counter] == command) {
				found = 1;
				break;
			}
		if (found)
			if ((command ^ stm32prog_serial_getc()) != 0xFF)
				found = 0;
		if (!found) {
			/* wait to be sure that all data are received
			 * in the FIFO before flush (CMD and XOR)
			 */
			mdelay(2);
			stm32prog_serial_result(NACK_BYTE);
		} else {
			/*pr_debug("+ cmd %x\n", counter);*/
			stm32prog_serial_result(ACK_BYTE);
			cmd_func[counter](data);
		}
		WATCHDOG_RESET();
	}

	/* clean device */
	if (gd->cur_serial_dev == down_serial_dev) {
		/* restore console on uart */
		gd->flags &= ~(GD_FLG_DISABLE_CONSOLE | GD_FLG_SILENT);
	}
	down_serial_dev = NULL;

	return false; /* no reset after ctrlc */
}
