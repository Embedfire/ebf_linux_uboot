// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#include <common.h>
#include <console.h>
#include <dfu.h>
#include <malloc.h>
#include <mmc.h>
#include <nand.h>
#include <part.h>
#include <spi.h>
#include <asm/arch/stm32mp1_smc.h>
#include <dm/uclass.h>
#include <jffs2/load_kernel.h>
#include <linux/libfdt.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/mtd/mtd.h>
#include <linux/sizes.h>
#include <power/stpmic1.h>

#include "stm32prog.h"

/* Primary GPT header size for 128 entries : 17kB = 34 LBA of 512B */
#define GPT_HEADER_SZ	34

#define OPT_SELECT	BIT(0)
#define OPT_EMPTY	BIT(1)
#define OPT_DELETE	BIT(2)

#define IS_SELECT(part)	(part->option & OPT_SELECT)
#define IS_EMPTY(part)	(part->option & OPT_EMPTY)
#define IS_DELETE(part)	(part->option & OPT_DELETE)

#define ROOTFS_MMC0_UUID \
	EFI_GUID(0xE91C4E10, 0x16E6, 0x4C0E, \
		 0xBD, 0x0E, 0x77, 0xBE, 0xCF, 0x4A, 0x35, 0x82)

#define ROOTFS_MMC1_UUID \
	EFI_GUID(0x491F6117, 0x415D, 0x4F53, \
		 0x88, 0xC9, 0x6E, 0x0D, 0xE5, 0x4D, 0xEA, 0xC6)

#define ROOTFS_MMC2_UUID \
	EFI_GUID(0xFD58F1C7, 0xBE0D, 0x4338, \
		 0x88, 0xE9, 0xAD, 0x8F, 0x05, 0x0A, 0xEB, 0x18)

/*
 * unique partition guid (uuid) for partition named "rootfs"
 * on each MMC instance = SD Card or eMMC
 * allow fixed kernel bootcmd: "rootf=PARTUID=e91c4e10-..."
 */
const static efi_guid_t uuid_mmc[3] = {
	ROOTFS_MMC0_UUID,
	ROOTFS_MMC1_UUID,
	ROOTFS_MMC2_UUID
};

DECLARE_GLOBAL_DATA_PTR;
#define ENV_BUF_LEN			SZ_1K

/* order of column in flash layout file */
enum stm32prog_col_t {
	COL_OPTION,
	COL_ID,
	COL_NAME,
	COL_TYPE,
	COL_IP,
	COL_OFFSET,
	COL_NB_STM32
};

/* partition handling routines : CONFIG_CMD_MTDPARTS */
int mtdparts_init(void);
int find_dev_and_part(const char *id, struct mtd_device **dev,
		      u8 *part_num, struct part_info **part);

char *stm32prog_get_error(struct stm32prog_data *data)
{
	const char error_msg[] = "Unspecified";

	if (strlen(data->error) == 0)
		strcpy(data->error, error_msg);

	return data->error;
}

u8 stm32prog_header_check(struct raw_header_s *raw_header,
			  struct image_header_s *header)
{
	int i;

	header->present = 0;
	header->image_checksum = 0x0;
	header->image_length = 0x0;

	/*pr_debug("%s entry\n", __func__);*/
	if (!raw_header || !header) {
		pr_debug("%s:no header data\n", __func__);
		return -1;
	}
	if (raw_header->magic_number !=
		(('S' << 0) | ('T' << 8) | ('M' << 16) | (0x32 << 24))) {
		pr_debug("%s:invalid magic number : 0x%x\n",
			 __func__, raw_header->magic_number);
		return -2;
	}
	/* only header v1.0 supported */
	if (raw_header->header_version != 0x00010000) {
		pr_debug("%s:invalid header version : 0x%x\n",
			 __func__, raw_header->header_version);
		return -3;
	}
	if (raw_header->reserved1 != 0x0 || raw_header->reserved2) {
		pr_debug("%s:invalid reserved field\n", __func__);
		return -4;
	}
	for (i = 0; i < (sizeof(raw_header->padding) / 4); i++) {
		if (raw_header->padding[i] != 0) {
			pr_debug("%s:invalid padding field\n", __func__);
			return -5;
		}
	}
	header->present = 1;
	header->image_checksum = le32_to_cpu(raw_header->image_checksum);
	header->image_length = le32_to_cpu(raw_header->image_length);

	/*pr_debug("%s exit\n", __func__);*/

	return 0;
}

static u32 stm32prog_header_checksum(u32 addr, struct image_header_s *header)
{
	u32 i, checksum;
	u8 *payload;

	/* compute checksum on payload */
	payload = (u8 *)addr;
	checksum = 0;
	for (i = header->image_length; i > 0; i--)
		checksum += *(payload++);

	return checksum;
}

/* FLASHLAYOUT PARSING *****************************************/
static int parse_option(struct stm32prog_data *data,
			char *p, struct stm32prog_part_t *part)
{
	int result = 0;
	char *c = p;

	part->option = 0;
	if (!strcmp(p, "-"))
		return 0;

	while (*c) {
		switch (*c) {
		case 'P':
			part->option |= OPT_SELECT;
			break;
		case 'E':
			part->option |= OPT_EMPTY;
			break;
		case 'D':
			part->option |= OPT_DELETE;
			break;
		default:
			result = -EINVAL;
			stm32prog_err("Layout: invalid option '%c' in %s)",
				      *c, p);
			return -EINVAL;
		}
		c++;
	}
	if (!(part->option & OPT_SELECT)) {
		stm32prog_err("Layout: missing 'P' in option %s", p);
		return -EINVAL;
	}

	/* pr_debug("option : %x\n", part->option); */

	return result;
}

static int parse_id(struct stm32prog_data *data,
		    char *p, struct stm32prog_part_t *part)
{
	int result = 0;
	unsigned long value;

	result = strict_strtoul(p, 0, &value);
	part->id = value;
	if (result || value > PHASE_LAST_USER) {
		stm32prog_err("Layout: invalid phase value = %s", p);
		result = -EINVAL;
	}
	/* pr_debug("phase : %x\n", part->id); */

	return result;
}

static int parse_name(struct stm32prog_data *data,
		      char *p, struct stm32prog_part_t *part)
{
	int result = 0;

	if (strlen(p) < sizeof(part->name)) {
		strcpy(part->name, p);
	} else {
		stm32prog_err("Layout: partition name too long [%d]  : %s",
			      strlen(p), p);
		result = -EINVAL;
	}
	/* pr_debug("name : %s\n", part->name); */

	return result;
}

static int parse_type(struct stm32prog_data *data,
		      char *p, struct stm32prog_part_t *part)
{
	int result = 0;
	int len = 0;

	part->bin_nb = 0;
	if (!strncmp(p, "Binary", 6)) {
		part->part_type = PART_BINARY;

		/* search for Binary(X) case */
		len = strlen(p);
		part->bin_nb = 1;
		if (len > 6) {
			if (len < 8 ||
			    (p[6] != '(') ||
			    (p[len - 1] != ')'))
				result = -EINVAL;
			else
				part->bin_nb =
					simple_strtoul(&p[7], NULL, 10);
		}
	} else if (!strcmp(p, "System")) {
		part->part_type = PART_SYSTEM;
	} else if (!strcmp(p, "FileSystem")) {
		part->part_type = PART_FILESYSTEM;
	} else if (!strcmp(p, "RawImage")) {
		part->part_type = RAW_IMAGE;
	} else {
		result = -EINVAL;
	}
	if (result)
		stm32prog_err("Layout: type parsing error : '%s'", p);
	/* pr_debug("type : %d\n", part->part_type); */

	return result;
}

static int parse_ip(struct stm32prog_data *data,
		    char *p, struct stm32prog_part_t *part)
{
	int result = 0;
	int len = 0;

	part->dev_id = 0;
	if (!strcmp(p, "none")) {
		part->dev_type = DFU_DEV_VIRT;
	} else if (!strncmp(p, "mmc", 3)) {
		part->dev_type = DFU_DEV_MMC;
		len = 3;
	} else if (!strncmp(p, "nor", 3)) {
		part->dev_type = DFU_DEV_SF;
		len = 3;
	} else if (!strncmp(p, "nand", 4)) {
		part->dev_type = DFU_DEV_NAND;
		len = 4;
	} else {
		result = -EINVAL;
	}
	if (len) {
		/* only one digit allowed for device id */
		if (strlen(p) != len + 1) {
			result = -EINVAL;
		} else {
			part->dev_id = p[len] - '0';
			if (part->dev_id > 9)
				result = -EINVAL;
		}
	}
	if (result)
		stm32prog_err("Layout: ip parsing error : '%s'", p);
	/* pr_debug("dev : %d\n", part->dev_id); */

	return result;
}

static int parse_offset(struct stm32prog_data *data,
			char *p, struct stm32prog_part_t *part)
{
	int result = 0;
	char *tail;

	part->part_id = 0;
	part->addr = 0;
	part->size = 0;
	/* eMMC boot parttion */
	if (!strncmp(p, "boot", 4)) {
		if (p[4] == '1') {
			part->part_id = -1;
		} else if (p[4] == '2') {
			part->part_id = -2;
		} else {
			stm32prog_err("Layout: invalid part '%s'", p);
			result = -EINVAL;
		}
	} else {
		part->addr = simple_strtoull(p, &tail, 0);
		if (tail == p || *tail != '\0') {
			stm32prog_err("Layout: invalid offset '%s'", p);
			result = -EINVAL;
		}
	}
	/* pr_debug("addr : 0x%llx, part_id : %d\n", part->addr,
	 *       part->part_id);
	 */

	return result;
}

static
int (* const parse[COL_NB_STM32])(struct stm32prog_data *data, char *p,
				  struct stm32prog_part_t *part) = {
	[COL_OPTION] = parse_option,
	[COL_ID] = parse_id,
	[COL_NAME] =  parse_name,
	[COL_TYPE] = parse_type,
	[COL_IP] = parse_ip,
	[COL_OFFSET] = parse_offset,
};

static int parse_flash_layout(struct stm32prog_data *data,
			      ulong addr,
			      ulong size)
{
	int column = 0, part_nb = 0, ret;
	bool end_of_line, eof;
	char *p, *start, *last, *col;
	struct stm32prog_part_t *part;
	int part_list_size;
	bool stm32image = false;

	data->part_nb = 0;

	/* check if STM32image is detected */
	if (!stm32prog_header_check((struct raw_header_s *)addr,
				    &data->header)) {
		u32 checksum;

		addr = addr + BL_HEADER_SIZE;
		size = data->header.image_length;
		stm32image = true;

		checksum = stm32prog_header_checksum(addr, &data->header);
		if (checksum != data->header.image_checksum) {
			stm32prog_err("Layout: invalid checksum : 0x%x expected 0x%x",
				      checksum, data->header.image_checksum);
			return -EIO;
		}
	}
	if (!size)
		return -EINVAL;

	start = (char *)addr;
	last = start + size;

	*last = 0x0; /* force null terminated string */
	pr_debug("flash layout =\n%s\n", start);

	/* calculate expected number of partitions */
	part_list_size = 1;
	p = start;
	while (*p && (p < last)) {
		if (*p++ == '\n') {
			part_list_size++;
			if (p < last && *p == '#')
				part_list_size--;
		}
	}
	if (part_list_size > PHASE_LAST_USER) {
		stm32prog_err("Layout: too many line");
		return -1;
	}
	part = calloc(sizeof(struct stm32prog_part_t), part_list_size);
	if (!part) {
		stm32prog_err("Layout: alloc failed");
		return -ENOMEM;
	}
	data->part_array = part;

	/* main parsing loop */
	eof = false;
	p = start;
	col = start; /* 1st column */
	while (!eof) {
		end_of_line = false;
		switch (*p) {
		/* CR is ignored and replaced by NULL chararc*/
		case '\r':
			*p = '\0';
			p++;
			continue;
		/* end of column detected */
		case '\0':
			end_of_line = true;
			eof = true;
			break;
		case '\n':
			end_of_line = true;
			break;
		case '\t':
			break;
		case '#':
			/* comment line is skipped */
			if (column == 0 && p == col) {
				while ((p < last) && *p)
					if (*p++ == '\n')
						break;
				col = p;
				if (p >= last || !*p)
					eof = true;
				continue;
			}
			/* no break */
		/* by default continue with the next character */
		default:
			p++;
			continue;
		}

		/* replace by \0 to allow string parsing */
		*p = '\0';
		p++;
		if (p >= last) {
			eof = true;
			end_of_line = true;
		}
		/*pr_debug("%d:%d = '%s' => ", part_nb, column, col);*/
		if (strlen(col) == 0) {
			col = p;
			/* skip empty line */
			if (column == 0 && end_of_line)
				continue;
			/* multiple TAB allowed in tsv file */
			if (!stm32image)
				continue;
			stm32prog_err("empty field for line %d", part_nb);
			return -1;
		}
		if (column < COL_NB_STM32) {
			ret = parse[column](data, col, part);
			if (ret)
				return ret;
		}

		/* save the beginning of the next column */
		column++;
		col = p;

		if (!end_of_line)
			continue;

		/* end of the line detected */
		if (column < COL_NB_STM32) {
			stm32prog_err("Layout: no enought column for line %d",
				      part_nb);
			return -EINVAL;
		}
		column = 0;
		part_nb++;
		part++;
		if (part_nb >= part_list_size) {
			part = NULL;
			if (!eof) {
				stm32prog_err("Layout: no enought memory for %d part",
					      part_nb);
				return -EINVAL;
			}
		}
	}
	data->part_nb = part_nb;
	if (data->part_nb == 0) {
		stm32prog_err("Layout: no partition found");
		return -ENODEV;
	}

	return 0;
}

static int __init part_cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct stm32prog_part_t *parta, *partb;

	parta = container_of(a, struct stm32prog_part_t, list);
	partb = container_of(b, struct stm32prog_part_t, list);

	if (parta->part_id != partb->part_id)
		return parta->part_id - partb->part_id;
	else
		return parta->addr > partb->addr ? 1 : -1;
}

static int init_device(struct stm32prog_data *data,
		       struct stm32prog_dev_t *dev)
{
	struct mmc *mmc = NULL;
#ifdef CONFIG_MTD_PARTITIONS
	struct blk_desc *block_dev = NULL;
	struct mtd_info *mtd = NULL;
	char mtd_id[16];
	char cmdbuf[40];
#endif
	int part_id;
	int ret;
	u64 first_addr = 0, last_addr = 0;
	struct stm32prog_part_t *part, *next_part;
	u64 part_addr, part_size;

	dev->lba_blk_size = MMC_MAX_BLOCK_LEN;
	switch (dev->dev_type) {
	case DFU_DEV_MMC:
		mmc = find_mmc_device(dev->dev_id);
		if (mmc_init(mmc)) {
			stm32prog_err("mmc device %d not found", dev->dev_id);
			return -ENODEV;
		}
		block_dev = mmc_get_blk_desc(mmc);
		if (!block_dev) {
			stm32prog_err("mmc device %d not probed", dev->dev_id);
			return -ENODEV;
		}
		dev->lba_blk_size = mmc->read_bl_len;
		dev->erase_size = mmc->erase_grp_size * block_dev->blksz;

		/* reserve a full erase group for each GTP headers */
		if (mmc->erase_grp_size > GPT_HEADER_SZ) {
			first_addr = dev->erase_size;
			last_addr = (u64)(block_dev->lba -
					  mmc->erase_grp_size) *
				    block_dev->blksz;
		} else {
			first_addr = (u64)GPT_HEADER_SZ * block_dev->blksz;
			last_addr = (u64)(block_dev->lba - GPT_HEADER_SZ - 1) *
				    block_dev->blksz;
		}
		pr_debug("MMC %d: lba=%ld blksz=%ld\n", dev->dev_id,
			 block_dev->lba, block_dev->blksz);
		pr_debug(" available address = 0x%llx..0x%llx\n",
			 first_addr, last_addr);
		break;
#ifdef CONFIG_MTD_PARTITIONS
	case DFU_DEV_SF:
#ifdef CONFIG_SPI_FLASH
		sprintf(cmdbuf, "sf probe %d", dev->dev_id);
		if (run_command(cmdbuf, 0)) {
			stm32prog_err("invalid device : %s", cmdbuf);
			return -ENODEV;
		}
		sprintf(mtd_id, "nor%d", dev->dev_id);
		pr_debug("%s\n", mtd_id);
		break;
#else
		stm32prog_err("device SF nor supported");
		return -ENODEV;
#endif
	case DFU_DEV_NAND:
		sprintf(cmdbuf, "nand device %d", dev->dev_id);
		if (run_command(cmdbuf, 0)) {
			stm32prog_err("invalid device : %s", cmdbuf);
			return -ENODEV;
		}
		sprintf(mtd_id, "nand%d", dev->dev_id);
		pr_debug("%s\n", mtd_id);
		break;
#endif
	default:
		stm32prog_err("unknown device type = %d", dev->dev_type);
		return -ENODEV;
	}
#ifdef CONFIG_MTD_PARTITIONS
	if (dev->dev_type == DFU_DEV_SF ||
	    dev->dev_type == DFU_DEV_NAND) {
#ifdef CONFIG_SPI_FLASH
		if (dev->dev_type == DFU_DEV_NAND) {
			/* sf probe is needed for mtdparts
			 * because mtdids can use nor0 and nor driver
			 * is not probed by default as nand
			 */
			sprintf(cmdbuf, "sf probe %d", dev->dev_id);
			run_command(cmdbuf, 0);
		}
#endif
		mtdparts_init();
		mtd = get_mtd_device_nm(mtd_id);
		if (IS_ERR(mtd)) {
			stm32prog_err("MTD device %s not found", mtd_id);
			return -ENODEV;
		}
		first_addr = 0;
		last_addr = mtd->size;
		dev->erase_size = mtd->erasesize;
		pr_debug("MTD device %s : size=%lld erasesize=%d\n",
			 mtd_id, mtd->size, mtd->erasesize);
		pr_debug(" available address = 0x%llx..0x%llx\n",
			 first_addr, last_addr);
	}
	dev->mtd = mtd;
#endif
	pr_debug(" erase size = 0x%x\n", dev->erase_size);
	dev->block_dev = block_dev;

	/* order partition list in offset order */
	list_sort(NULL, &dev->part_list, &part_cmp);
	part_id = 1;
	pr_debug("id : Opt Phase     Name  type.n dev.n addr     size     part_off part_size\n");
	list_for_each_entry(part, &dev->part_list, list) {
		if (part->bin_nb > 1) {
			if (dev->dev_type != DFU_DEV_NAND ||
			    part->id >= PHASE_FIRST_USER ||
			    strncmp(part->name, "fsbl", 4)) {
				stm32prog_err("%s: multiple binary %d not supported for phase %d",
					      part->name, part->bin_nb,
					      part->id);
				return -EINVAL;
			}
		}
		if (part->part_type == RAW_IMAGE) {
			part->part_id = 0x0;
			part->addr = 0x0;
			if (block_dev)
				part->size = block_dev->lba * block_dev->blksz;
			else
				part->size = last_addr;
			pr_debug("-- : %1d %02x %14s %02d.%d %02d.%02d %08llx %08llx\n",
				 part->option, part->id, part->name,
				 part->part_type, part->bin_nb, part->dev_type,
				 part->dev_id, part->addr, part->size);
			continue;
		}
		if (part->part_id < 0) { /* boot hw partition for eMMC */
			if (mmc) {
				part->size = mmc->capacity_boot;
			} else {
				stm32prog_err("%s: hw partition not expected : %d",
					      part->name, part->part_id);
				return -ENODEV;
			}
		} else {
			part->part_id = part_id++;

			/* last partition : size to the end of the device */
			if (part->list.next != &dev->part_list) {
				next_part =
					container_of(part->list.next,
						     struct stm32prog_part_t,
						     list);
				if (part->addr < next_part->addr) {
					part->size = next_part->addr -
						     part->addr;
				} else {
					stm32prog_err("%s: invalid address : 0x%llx >= 0x%llx",
						      part->name, part->addr,
						      next_part->addr);
					return -EINVAL;
				}
			} else {
				if (part->addr <= last_addr) {
					part->size = last_addr - part->addr;
				} else {
					stm32prog_err("%s: invalid address 0x%llx (max=0x%llx)",
						      part->name, part->addr,
						      last_addr);
					return -EINVAL;
				}
			}
			if (part->addr < first_addr) {
				stm32prog_err("%s: invalid address 0x%llx (min=0x%llx)",
					      part->name, part->addr,
					      first_addr);
				return -EINVAL;
			}
		}
		if ((part->addr & ((u64)part->dev->erase_size - 1)) != 0) {
			stm32prog_err("%s: not aligned address : 0x%llx on erase size 0x%x",
				      part->name, part->addr,
				      part->dev->erase_size);
			return -EINVAL;
		}
		pr_debug("%02d : %1d %02x %14s %02d.%d %02d.%02d %08llx %08llx",
			 part->part_id, part->option, part->id, part->name,
			 part->part_type, part->bin_nb, part->dev_type,
			 part->dev_id, part->addr, part->size);

		part_addr = 0;
		part_size = 0;
		/* check coherency with existing partition */
		if (block_dev) {
			disk_partition_t partinfo;

			/* only check partition size for partial update */
			if (data->full_update || part->part_id < 0) {
				pr_debug("\n");
				continue;
			}

			ret = part_get_info(block_dev, part->part_id,
					    &partinfo);

			if (ret) {
				stm32prog_err("Couldn't find part %d on device mmc %d",
					      part_id, part->dev_id);
				return -ENODEV;
			}
			part_addr = (u64)partinfo.start * partinfo.blksz;
			part_size = (u64)partinfo.size * partinfo.blksz;
		}

#ifdef CONFIG_MTD_PARTITIONS
		if (mtd) {
			char mtd_part_id[32];
			struct part_info *mtd_part;
			struct mtd_device *mtd_dev;
			u8 part_num;

			sprintf(mtd_part_id, "%s,%d", mtd_id,
				part->part_id - 1);
			ret = find_dev_and_part(mtd_part_id, &mtd_dev,
						&part_num, &mtd_part);
			if (ret != 0) {
				stm32prog_err("Invalid partition %s",
					      mtd_part_id);
				return -ENODEV;
			}
			part_addr = mtd_part->offset;
			part_size = mtd_part->size;
		}
#endif
		pr_debug(" %08llx %08llx\n", part_addr, part_size);

		if (part->addr != part_addr) {
			stm32prog_err("%s: Bad address requested for partition %d = 0x%llx <> 0x%llx",
				      part->name, part->part_id, part->addr,
				      part_addr);
			return -ENODEV;
		}
		if (part->size != part_size) {
			stm32prog_err("%s: Bad size requested for partition %d = 0x%llx <> 0x%llx",
				      part->name, part->part_id, part->size,
				      part_size);
			return -ENODEV;
		}
	}
	return 0;
}

static int treat_partition_list(struct stm32prog_data *data)
{
	int i, j;
	struct stm32prog_part_t *part;

	for (j = 0; j < STM32PROG_MAX_DEV; j++) {
		data->dev[j].dev_type = -1;
		INIT_LIST_HEAD(&data->dev[j].part_list);
	}

	data->full_update = 1;
	/*pr_debug("id : S Phase  Name       type  dev.n  addr  id\n");*/
	for (i = 0; i < data->part_nb; i++) {
		part = &data->part_array[i];
		part->alt_id = -1;

		/* skip partition with IP="none" */
		if (part->dev_type == DFU_DEV_VIRT) {
			if (IS_SELECT(part)) {
				stm32prog_err("Layout: selected none phase = 0x%x",
					      part->id);
				return -EINVAL;
			}
			continue;
		}

		/*
		 * pr_debug("%02d : %1d %02x %14s %02d %02d.%02d 0x%08llx %d\n",
		 *	 i, part->option, part->id, part->name,
		 *	 part->part_type, part->dev_id_type, part->dev_id,
		 *	 part->addr, part->part_id);
		 */
		if (!IS_SELECT(part) && part->part_type != RAW_IMAGE)
			data->full_update = 0;

		if (part->id == PHASE_FLASHLAYOUT ||
		    part->id > PHASE_LAST_USER) {
			stm32prog_err("Layout: invalid phase = 0x%x",
				      part->id);
			return -EINVAL;
		}
		for (j = i + 1; j < data->part_nb; j++) {
			if (part->id == data->part_array[j].id) {
				stm32prog_err("Layout: duplicated phase %d at line %d and %d",
					      part->id, i, j);
				return -EINVAL;
			}
		}
		for (j = 0; j < STM32PROG_MAX_DEV; j++) {
			if (data->dev[j].dev_type == -1) {
				/* new device found */
				data->dev[j].dev_type = part->dev_type;
				data->dev[j].dev_id = part->dev_id;
				data->dev_nb++;
				break;
			} else if ((part->dev_type == data->dev[j].dev_type) &&
				   (part->dev_id == data->dev[j].dev_id)) {
				break;
			}
		}
		if (j == STM32PROG_MAX_DEV) {
			stm32prog_err("Layout: too many device");
			return -EINVAL;
		}
		part->dev = &data->dev[j];
		list_add_tail(&part->list, &data->dev[j].part_list);
	}

	return 0;
}

static int create_partitions(struct stm32prog_data *data)
{
	int offset = 0;
	char cmdbuf[32];
	char buf[ENV_BUF_LEN];
	char uuid[UUID_STR_LEN + 1];
	unsigned char *uuid_bin;
	int i, mmc_id;
	bool rootfs_found;
	struct stm32prog_part_t *part;

	puts("partitions : ");
	/* initialize the selected device */
	for (i = 0; i < data->dev_nb; i++) {
		/* gpt support only for MMC */
		if (data->dev[i].dev_type != DFU_DEV_MMC)
			continue;

		offset = 0;
		rootfs_found = false;
		memset(buf, 0, sizeof(buf));

		list_for_each_entry(part, &data->dev[i].part_list, list) {
			/* skip eMMC boot partitions */
			if (part->part_id < 0)
				continue;

			/* skip Raw Image */
			if (part->part_type == RAW_IMAGE)
				continue;

			offset += snprintf(buf + offset, ENV_BUF_LEN - offset,
					   "name=%s,start=0x%llx,size=0x%llx",
					   part->name,
					   part->addr,
					   part->size);

			if (part->part_type == PART_BINARY)
				offset += snprintf(buf + offset,
						   ENV_BUF_LEN - offset,
						   ",type=data");
			else
				offset += snprintf(buf + offset,
						   ENV_BUF_LEN - offset,
						   ",type=linux");

			if (part->part_type == PART_SYSTEM)
				offset += snprintf(buf + offset,
						   ENV_BUF_LEN - offset,
						   ",bootable");

			if (!rootfs_found && !strcmp(part->name, "rootfs")) {
				mmc_id = part->dev_id;
				rootfs_found = true;
				if (mmc_id < ARRAY_SIZE(uuid_mmc)) {
					uuid_bin =
					  (unsigned char *)uuid_mmc[mmc_id].b;
					uuid_bin_to_str(uuid_bin, uuid,
							UUID_STR_FORMAT_GUID);
					offset += snprintf(buf + offset,
							   ENV_BUF_LEN - offset,
							   ",uuid=%s", uuid);
				}
			}

			offset += snprintf(buf + offset,
					   ENV_BUF_LEN - offset,
					   ";");
		}

		if (offset) {
			sprintf(cmdbuf, "gpt write mmc %d \"%s\"",
				data->dev[i].dev_id, buf);
			pr_debug("cmd: %s\n", cmdbuf);
			if (run_command(cmdbuf, 0)) {
				stm32prog_err("partitionning fail : %s",
					      cmdbuf);
				return -1;
			}
		}

		if (data->dev[i].block_dev)
			part_init(data->dev[i].block_dev);

#ifdef DEBUG
		sprintf(cmdbuf, "gpt verify mmc %d",
			data->dev[i].dev_id);
		pr_debug("cmd: %s ", cmdbuf);
		if (run_command(cmdbuf, 0))
			printf("fail !\n");
		else
			printf("OK\n");

		/* TEMP : for debug, display partition */
		sprintf(cmdbuf, "part list mmc %d",
			data->dev[i].dev_id);
		run_command(cmdbuf, 0);
#endif
	}
	puts("done\n");
	return 0;
}

static int stm32prog_alt_add(struct stm32prog_data *data,
			     struct dfu_entity *dfu,
			     struct stm32prog_part_t *part)
{
	int ret = 0;
	int offset = 0;
	char devstr[4];
	char dfustr[10];
	char buf[ENV_BUF_LEN];
	u32 size;
	char multiplier,  type;

	/* max 3 digit for sector size */
	if (part->size > SZ_1M) {
		size = (u32)(part->size / SZ_1M);
		multiplier = 'M';
	} else if (part->size > SZ_1K) {
		size = (u32)(part->size / SZ_1K);
		multiplier = 'K';
	} else {
		size = (u32)part->size;
		multiplier = 'B';
	}
	if (IS_SELECT(part) && !IS_EMPTY(part))
		type = 'e'; /*Readable and Writeable*/
	else
		type = 'a';/*Readable*/

	memset(buf, 0, sizeof(buf));
	offset = snprintf(buf, ENV_BUF_LEN - offset,
			  "@%s/0x%02x/1*%d%c%c ",
			  part->name, part->id,
			  size, multiplier, type);

	if (part->part_type == RAW_IMAGE) {
		u64 dfu_size;

		if (part->dev->dev_type == DFU_DEV_MMC)
			dfu_size = part->size / part->dev->lba_blk_size;
		else
			dfu_size = part->size;
		offset += snprintf(buf + offset, ENV_BUF_LEN - offset,
				   "raw 0x0 0x%llx", dfu_size);
	} else if (part->part_id < 0) {
		u64 nb_blk = part->size / part->dev->lba_blk_size;

		/* lba_blk_size, mmc->read_bl_len */
		offset += snprintf(buf + offset, ENV_BUF_LEN - offset,
				   "raw 0x%llx 0x%llx",
				   part->addr, nb_blk);
		offset += snprintf(buf + offset, ENV_BUF_LEN - offset,
				   " mmcpart %d;", -(part->part_id));
	} else {
		if (part->part_type == PART_SYSTEM &&
		    (part->dev_type == DFU_DEV_NAND ||
		     part->dev_type == DFU_DEV_SF))
			offset += snprintf(buf + offset,
					   ENV_BUF_LEN - offset,
					   "partubi");
		else
			offset += snprintf(buf + offset,
					   ENV_BUF_LEN - offset,
					   "part");
		offset += snprintf(buf + offset, ENV_BUF_LEN - offset,
				   " %d %d;",
				   part->dev_id,
				   part->part_id);
	}
	switch (part->dev_type) {
	case DFU_DEV_MMC:
		sprintf(dfustr, "mmc");
		sprintf(devstr, "%d", part->dev_id);
		break;
	case DFU_DEV_SF:
		sprintf(dfustr, "sf");
		sprintf(devstr, "0:%d", part->dev_id);
		break;
	case DFU_DEV_NAND:
		sprintf(dfustr, "nand");
		sprintf(devstr, "%d", part->dev_id);
		break;
	default:
		stm32prog_err("invalid dev_type: %d", part->dev_type);
		return -ENODEV;
	}
	ret = dfu_alt_add(dfu, dfustr, devstr, buf);
	pr_debug("dfu_alt_add(%s,%s,%s) result %d\n",
		 dfustr, devstr, buf, ret);

	return ret;
}

static int stm32prog_alt_add_virt(struct dfu_entity *dfu,
				  char *name, int phase, int size)
{
	int ret = 0;
	char devstr[4];
	char buf[ENV_BUF_LEN];

	sprintf(devstr, "%d", phase);
	sprintf(buf, "@%s/0x%02x/1*%dBe", name, phase, size);
	ret = dfu_alt_add(dfu, "virt", devstr, buf);
	pr_debug("dfu_alt_add(virt,%s,%s) result %d\n", devstr, buf, ret);

	return ret;
}

static int dfu_init_entities(struct stm32prog_data *data)
{
	int ret = 0;
	int phase, i, alt_id;
	struct stm32prog_part_t *part;
	struct dfu_entity *dfu;
	int alt_nb;

	/* nb of alternate = nb part not virtual or 1 for FlashLayout
	 * + 3 virtual for CMD and OTP and PMIC
	 */
	if (data->part_nb == 0) {
		alt_nb = 4;
	} else {
		alt_nb = 3;
		for (i = 0; i < data->part_nb; i++) {
			if (data->part_array[i].dev_type != DFU_DEV_VIRT)
				alt_nb++;
		}
	}

	if (dfu_alt_init(alt_nb, &dfu))
		return -ENODEV;

	puts("DFU alt info setting: ");
	if (data->part_nb) {
		alt_id = 0;
		for (phase = 1;
		     (phase <= PHASE_LAST_USER) &&
		     (alt_id < alt_nb) && !ret;
		     phase++) {
			/* ordering alt setting by phase id */
			part = NULL;
			for (i = 0; i < data->part_nb; i++) {
				if (phase == data->part_array[i].id) {
					part = &data->part_array[i];
					break;
				}
			}
			if (!part)
				continue;
			if (part->dev_type == DFU_DEV_VIRT)
				continue;
			part->alt_id = alt_id;
			alt_id++;

			ret = stm32prog_alt_add(data, dfu, part);
		}
	} else {
		char buf[ENV_BUF_LEN];

		sprintf(buf, "@FlashLayout/0x%02x/1*256Ke ram %x 40000",
			PHASE_FLASHLAYOUT, STM32_DDR_BASE);
		ret = dfu_alt_add(dfu, "ram", NULL, buf);
		pr_debug("dfu_alt_add(ram, NULL,%s) result %d\n", buf, ret);
	}

	if (!ret)
		ret = stm32prog_alt_add_virt(dfu, "virtual", PHASE_CMD, 512);

	if (!ret)
		ret = stm32prog_alt_add_virt(dfu, "OTP", PHASE_OTP, 512);

#ifdef CONFIG_DM_PMIC
	if (!ret)
		ret = stm32prog_alt_add_virt(dfu, "PMIC", PHASE_PMIC, 8);
#endif

	if (ret)
		stm32prog_err("dfu init failed: %d", ret);
	puts("done\n");
#ifdef DEBUG
	/* TEMP : for debug */
	dfu_show_entities();
#endif
	return ret;
}

int stm32prog_otp_write(struct stm32prog_data *data, u32 offset, u8 *buffer,
			long *size)
{
	pr_debug("%s : %x %lx\n", __func__, offset, *size);

	if (!data->otp_part) {
		data->otp_part = memalign(CONFIG_SYS_CACHELINE_SIZE, OTP_SIZE);
		if (!data->otp_part)
			return -ENOMEM;
	}

	if (!offset)
		memset(data->otp_part, 0, OTP_SIZE);

	if (offset + *size > OTP_SIZE)
		*size = OTP_SIZE - offset;

	memcpy((void *)((u32)data->otp_part + offset), buffer, *size);
	return 0;
}

int stm32prog_otp_read(struct stm32prog_data *data, u32 offset, u8 *buffer,
		       long *size)
{
#ifndef CONFIG_ARM_SMCCC
	stm32prog_err("OTP update not supported");
	return -1;
#else
	int result = 0;

	pr_debug("%s : %x %lx\n", __func__, offset, *size);
	/* alway read for first packet */
	if (!offset) {
		if (!data->otp_part)
			data->otp_part =
				memalign(CONFIG_SYS_CACHELINE_SIZE, OTP_SIZE);

		if (!data->otp_part) {
			result = -ENOMEM;
			goto end_otp_read;
		}

		/* init struct with 0 */
		memset(data->otp_part, 0, OTP_SIZE);

		/* call the service */
		result = stm32_smc_exec(STM32_SMC_BSEC, STM32_SMC_READ_ALL,
					(u32)data->otp_part, 0);
		if (result)
			goto end_otp_read;
	}

	if (!data->otp_part) {
		result = -ENOMEM;
		goto end_otp_read;
	}

	if (offset + *size > OTP_SIZE)
		*size = OTP_SIZE - offset;
	memcpy(buffer, (void *)((u32)data->otp_part + offset), *size);

end_otp_read:
	pr_debug("%s : result %i\n", __func__, result);
	return result;
#endif
}

int stm32prog_otp_start(struct stm32prog_data *data)
{
#ifndef CONFIG_ARM_SMCCC
	stm32prog_err("OTP update not supported");
	return -1;
#else
	int result = 0;
	struct arm_smccc_res res;

	if (!data->otp_part) {
		stm32prog_err("start OTP without data");
		return -1;
	}

	arm_smccc_smc(STM32_SMC_BSEC, STM32_SMC_WRITE_ALL,
		      (u32)data->otp_part, 0, 0, 0, 0, 0, &res);

	if (!res.a0) {
		switch (res.a1) {
		case 0:
			result = 0;
			break;
		case 1:
			stm32prog_err("Provisioning");
			result = 0;
			break;
		default:
			pr_err("%s: OTP incorrect value (err = %ld)\n",
			       __func__, res.a1);
			result = -EINVAL;
			break;
		}
	} else {
		pr_err("%s: Failed to exec in secure mode (err = %ld)\n",
		       __func__, res.a0);
		result = -EINVAL;
	}

	free(data->otp_part);
	data->otp_part = NULL;
	pr_debug("%s : result %i\n", __func__, result);
	return result;
#endif
}

int stm32prog_pmic_write(struct stm32prog_data *data, u32 offset, u8 *buffer,
			 long *size)
{
	pr_debug("%s : %x %lx\n", __func__, offset, *size);

	if (!offset)
		memset(data->pmic_part, 0, PMIC_SIZE);

	if (offset + *size > PMIC_SIZE)
		*size = PMIC_SIZE - offset;

	memcpy(&data->pmic_part[offset], buffer, *size);

	return 0;
}

int stm32prog_pmic_read(struct stm32prog_data *data, u32 offset, u8 *buffer,
			long *size)
{
#ifndef CONFIG_DM_PMIC
	stm32prog_err("PMIC update not supported");
	return -EOPNOTSUPP;
#else /* CONFIG_DM_PMIC */
	int result = 0;

	pr_debug("%s : %x %lx\n", __func__, offset, *size);

	/* alway request PMIC for first packet */
	if (!offset) {
		/* init struct with 0 */
		memset(data->pmic_part, 0, PMIC_SIZE);

		result = stpmic1_nvm_read_all(data->pmic_part, PMIC_SIZE);
		if (result < 0)
			goto end_pmic_read;
	}

	if (offset + *size > PMIC_SIZE)
		*size = PMIC_SIZE - offset;

	memcpy(buffer, &data->pmic_part[offset], *size);

end_pmic_read:
	pr_debug("%s : result %i\n", __func__, result);
	return result;
#endif /* CONFIG_DM_PMIC */
}

int stm32prog_pmic_start(struct stm32prog_data *data)
{
#ifndef CONFIG_DM_PMIC
	stm32prog_err("PMIC update not supported");
	return -EOPNOTSUPP;
#else /* CONFIG_DM_PMIC */
	return stpmic1_nvm_write_all(data->pmic_part, PMIC_SIZE);
#endif /* CONFIG_DM_PMIC */
}

/* copy FSBL on NAND to improve reliability on NAND */
static int stm32prog_copy_fsbl(struct stm32prog_part_t *part)
{
#ifndef CONFIG_CMD_NAND
	return -1;
#else
	loff_t start, lim;
	size_t count, actual = 0;
	int ret, i;
	void *fsbl;
	nand_erase_options_t opts;
	struct image_header_s header;
	struct raw_header_s raw_header;

	if (part->dev_type != DFU_DEV_NAND)
		return -1;

	start = part->addr;
	lim = part->size;
	count = BL_HEADER_SIZE;

	ret = nand_read_skip_bad(part->dev->mtd, start, &count, &actual, lim,
				 (void *)&raw_header);
	if (ret)
		return ret;
	if (stm32prog_header_check(&raw_header, &header))
		return -1;

	count = header.image_length + BL_HEADER_SIZE;
	fsbl = calloc(1, count);
	if (!fsbl)
		return -ENOMEM;
	ret = nand_read_skip_bad(part->dev->mtd, start, &count, &actual, lim,
				 fsbl);
	if (ret)
		goto error;

	memset(&opts, 0, sizeof(opts));
	opts.length = count;
	opts.spread = 1;
#ifndef DEBUG
	opts.quiet = 1;
#endif

	for (i = part->bin_nb - 1; i > 0; i--) {
		size_t block_offset;

		/* copy to next block */
		start += actual;
		block_offset = start & (part->dev->mtd->erasesize - 1);
		if (block_offset != 0)
			start += part->dev->mtd->erasesize - block_offset;

		lim = part->size - (start - part->addr);

		/* first erase */
		opts.offset = start;
		opts.lim = lim;
		ret = nand_erase_opts(part->dev->mtd, &opts);
		if (ret)
			goto error;

		/* then write */
		ret = nand_write_skip_bad(part->dev->mtd,
					  start, &count, &actual,
					  lim, fsbl, WITH_WR_VERIFY);
		if (ret)
			goto error;
	}

error:
	free(fsbl);
	/* pr_debug("%s exit ret=%d\n", __func__, ret); */
	return ret;
#endif
}

void stm32prog_end_phase(struct stm32prog_data *data)
{
	if (data->phase == PHASE_FLASHLAYOUT) {
		if (parse_flash_layout(data, STM32_DDR_BASE, 0))
			stm32prog_err("Layout: invalid FlashLayout");
		return;
	}

	if (!data->cur_part)
		return;

	if (data->cur_part->part_id < 0) {
		char cmdbuf[60];

		sprintf(cmdbuf, "mmc bootbus %d 0 0 0; mmc partconf %d 1 %d 0",
			data->cur_part->dev_id, data->cur_part->dev_id,
			-(data->cur_part->part_id));
		if (run_command(cmdbuf, 0)) {
			stm32prog_err("commands %s have failed", cmdbuf);
			return;
		}
	}

	if (data->cur_part->bin_nb > 1) {
		if (stm32prog_copy_fsbl(data->cur_part)) {
			stm32prog_err("copy of fsbl failed");
			return;
		}
	}
}

void stm32prog_do_reset(struct stm32prog_data *data)
{
	if (data->phase == PHASE_RESET) {
		data->phase = PHASE_DO_RESET;
		puts("Reset requested\n");
	}
}

void stm32prog_next_phase(struct stm32prog_data *data)
{
	int phase, i;
	struct stm32prog_part_t *part;

	/*pr_debug("%s entry\n", __func__);*/

	phase = data->phase;
	switch (phase) {
	case PHASE_RESET:
	case PHASE_END:
	case PHASE_DO_RESET:
		return;
	}

	/* found next selected partition */
	phase++;
	data->cur_part = NULL;
	data->dfu_seq = 0;
	data->phase = PHASE_END;
	while ((phase <= PHASE_LAST_USER) && !data->cur_part) {
		for (i = 0; i < data->part_nb; i++) {
			part = &data->part_array[i];
			if (part->id == phase) {
				if (IS_SELECT(part) && !IS_EMPTY(part)) {
					data->cur_part = part;
					data->phase = phase;
				}
				break;
			}
		}
		phase++;
	}
	if (data->phase == PHASE_END)
		puts("Phase=END\n");

	/*pr_debug("%s exit phase=0x%x\n", __func__, data->phase);*/
}

static int part_delete(struct stm32prog_data *data,
		       struct stm32prog_part_t *part)
{
	int ret = 0;
	unsigned long blks, blks_offset, blks_size;
#ifdef CONFIG_SPI_FLASH
	char cmdbuf[40];
#endif

	printf("Erasing %s ", part->name);
	switch (part->dev_type) {
	case DFU_DEV_MMC:
		printf("on mmc %d: ", part->dev->dev_id);
		blks_offset = lldiv(part->addr, part->dev->lba_blk_size);
		blks_size = lldiv(part->size, part->dev->lba_blk_size);
		/* -1 or -2 : delete boot partition of MMC
		 * need to switch to associated hwpart 1 or 2
		 */
		if (part->part_id < 0)
			if (blk_select_hwpart_devnum(IF_TYPE_MMC,
						     part->dev->dev_id,
						     -part->part_id))
				return -1;
		blks = blk_derase(part->dev->block_dev, blks_offset, blks_size);
		/* return to user partition */
		if (part->part_id < 0)
			blk_select_hwpart_devnum(IF_TYPE_MMC,
						 part->dev->dev_id, 0);
		if (blks != blks_size) {
			ret = -1;
			stm32prog_err("mmc erase failed");
		}
		break;

#ifdef CONFIG_SPI_FLASH
	case DFU_DEV_SF:
		printf("on sf %d: ", part->dev->dev_id);
		sprintf(cmdbuf, "sf erase 0x%llx 0x%llx",
			part->addr, part->size);
		if (run_command(cmdbuf, 0)) {
			ret = -1;
			stm32prog_err("sf erase commands failed (%s)", cmdbuf);
		}
		break;
#endif

#ifdef CONFIG_CMD_NAND
	case DFU_DEV_NAND:
		printf("on nand %d: ", part->dev->dev_id);
		nand_erase_options_t opts;
			memset(&opts, 0, sizeof(opts));
		opts.offset = part->addr;
		opts.length = part->size;
		opts.quiet = 1;
		ret = nand_erase_opts(part->dev->mtd, &opts);
		if (ret)
			stm32prog_err("nand erase failed");
		break;
#endif
	default:
		ret = -1;
		stm32prog_err("erase invalid");
		break;
	}
	if (!ret)
		printf("done\n");

	return ret;
}

void stm32prog_devices_init(struct stm32prog_data *data)
{
	int i;
	int ret;
	struct stm32prog_part_t *part;

	ret = treat_partition_list(data);
	if (ret)
		goto error;

	/* initialize the selected device */
	for (i = 0; i < data->dev_nb; i++) {
		ret = init_device(data, &data->dev[i]);
		if (ret)
			goto error;
	}

	/* delete RAW partition before create partition */
	for (i = 0; i < data->part_nb; i++) {
		part = &data->part_array[i];

		if (part->part_type != RAW_IMAGE)
			continue;

		if (!IS_SELECT(part) || !IS_DELETE(part))
			continue;

		ret = part_delete(data, part);
		if (ret)
			goto error;
	}

	if (data->full_update) {
		ret = create_partitions(data);
		if (ret)
			goto error;
	}

	/* delete partition GPT or MTD */
	for (i = 0; i < data->part_nb; i++) {
		part = &data->part_array[i];

		if (part->part_type == RAW_IMAGE)
			continue;

		if (!IS_SELECT(part) || !IS_DELETE(part))
			continue;

		ret = part_delete(data, part);
		if (ret)
			goto error;
	}

	return;

error:
	data->part_nb = 0;
}

int stm32prog_dfu_init(struct stm32prog_data *data)
{
	/* init device if no error */
	if (data->part_nb)
		stm32prog_devices_init(data);

	if (data->part_nb)
		stm32prog_next_phase(data);

	/* prepare DFU for device read/write */
	dfu_free_entities();
	return dfu_init_entities(data);
}

struct stm32prog_data *stm32prog_init(enum stm32prog_link_t link,
				      int link_dev,
				      ulong addr,
				      ulong size)
{
	struct stm32prog_data *data;

	/*pr_debug("%s entry\n", __func__);*/
	data = (struct stm32prog_data *)malloc(sizeof(*data));

	if (!data) {
		pr_err("alloc failed");
		goto err;
	}
	memset(data, 0x0, sizeof(*data));
	data->read_phase = PHASE_RESET;
	data->phase = PHASE_FLASHLAYOUT;

	parse_flash_layout(data, addr, size);

	/* prepare DFU for device read/write */
	if (stm32prog_dfu_init(data))
		goto err;

	switch (link) {
	case LINK_SERIAL:
		if (stm32prog_serial_init(data, link_dev))
			goto err;
		data->buffer = memalign(CONFIG_SYS_CACHELINE_SIZE,
					USART_RAM_BUFFER_SIZE);
		break;
	case LINK_USB:
		break;
	default:
		break;
	}
	/*pr_debug("%s exit ok\n", __func__);*/
	return data;

err:
	free(data);
	pr_debug("%s exit error\n", __func__);
	return 0;
}

void stm32prog_clean(struct stm32prog_data *data)
{
	/* clean */
	dfu_free_entities();
	free(data->part_array);
	free(data->otp_part);
	free(data->buffer);
	free(data->header_data);
	free(data);
}
