// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2015
 * Texas Instruments Incorporated - http://www.ti.com/
 */
#define pr_fmt(fmt) "%s: " fmt, __func__
#include <common.h>
#include <elf.h>
#include <errno.h>
#include <fdtdec.h>
#include <malloc.h>
#include <remoteproc.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm.h>
#include <dm/uclass.h>
#include <dm/uclass-internal.h>

DECLARE_GLOBAL_DATA_PTR;

/**
 * struct resource_table - firmware resource table header
 * @ver: version number
 * @num: number of resource entries
 * @reserved: reserved (must be zero)
 * @offset: array of offsets pointing at the various resource entries
 *
 * A resource table is essentially a list of system resources required
 * by the remote processor. It may also include configuration entries.
 * If needed, the remote processor firmware should contain this table
 * as a dedicated ".resource_table" ELF section.
 *
 * Some resources entries are mere announcements, where the host is informed
 * of specific remoteproc configuration. Other entries require the host to
 * do something (e.g. allocate a system resource). Sometimes a negotiation
 * is expected, where the firmware requests a resource, and once allocated,
 * the host should provide back its details (e.g. address of an allocated
 * memory region).
 *
 * The header of the resource table, as expressed by this structure,
 * contains a version number (should we need to change this format in the
 * future), the number of available resource entries, and their offsets
 * in the table.
 *
 * Immediately following this header are the resource entries themselves,
 * each of which begins with a resource entry header (as described below).
 */
struct resource_table {
	u32 ver;
	u32 num;
	u32 reserved[2];
	u32 offset[0];
} __packed;

/**
 * for_each_remoteproc_device() - iterate through the list of rproc devices
 * @fn: check function to call per match, if this function returns fail,
 *	iteration is aborted with the resultant error value
 * @skip_dev:	Device to skip calling the callback about.
 * @data:	Data to pass to the callback function
 *
 * Return: 0 if none of the callback returned a non 0 result, else returns the
 * result from the callback function
 */
static int for_each_remoteproc_device(int (*fn) (struct udevice *dev,
					struct dm_rproc_uclass_pdata *uc_pdata,
					const void *data),
				      struct udevice *skip_dev,
				      const void *data)
{
	struct udevice *dev;
	struct dm_rproc_uclass_pdata *uc_pdata;
	int ret;

	for (ret = uclass_find_first_device(UCLASS_REMOTEPROC, &dev); dev;
	     ret = uclass_find_next_device(&dev)) {
		if (ret || dev == skip_dev)
			continue;
		uc_pdata = dev_get_uclass_platdata(dev);
		ret = fn(dev, uc_pdata, data);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * _rproc_name_is_unique() - iteration helper to check if rproc name is unique
 * @dev:	device that we are checking name for
 * @uc_pdata:	uclass platform data
 * @data:	compare data (this is the name we want to ensure is unique)
 *
 * Return: 0 is there is no match(is unique); if there is a match(we dont
 * have a unique name), return -EINVAL.
 */
static int _rproc_name_is_unique(struct udevice *dev,
				 struct dm_rproc_uclass_pdata *uc_pdata,
				 const void *data)
{
	const char *check_name = data;

	/* devices not yet populated with data - so skip them */
	if (!uc_pdata->name || !check_name)
		return 0;

	/* Return 0 to search further if we dont match */
	if (strlen(uc_pdata->name) != strlen(check_name))
		return 0;

	if (!strcmp(uc_pdata->name, check_name))
		return -EINVAL;

	return 0;
}

/**
 * rproc_name_is_unique() - Check if the rproc name is unique
 * @check_dev:	Device we are attempting to ensure is unique
 * @check_name:	Name we are trying to ensure is unique.
 *
 * Return: true if we have a unique name, false if name is not unique.
 */
static bool rproc_name_is_unique(struct udevice *check_dev,
				 const char *check_name)
{
	int ret;

	ret = for_each_remoteproc_device(_rproc_name_is_unique,
					 check_dev, check_name);
	return ret ? false : true;
}

/**
 * rproc_pre_probe() - Pre probe accessor for the uclass
 * @dev:	device for which we are preprobing
 *
 * Parses and fills up the uclass pdata for use as needed by core and
 * remote proc drivers.
 *
 * Return: 0 if all wernt ok, else appropriate error value.
 */
static int rproc_pre_probe(struct udevice *dev)
{
	struct dm_rproc_uclass_pdata *uc_pdata;
	const struct dm_rproc_ops *ops;

	uc_pdata = dev_get_uclass_platdata(dev);

	/* See if we need to populate via fdt */

	if (!dev->platdata) {
#if CONFIG_IS_ENABLED(OF_CONTROL)
		int node = dev_of_offset(dev);
		const void *blob = gd->fdt_blob;
		bool tmp;
		if (!blob) {
			debug("'%s' no dt?\n", dev->name);
			return -EINVAL;
		}
		debug("'%s': using fdt\n", dev->name);
		uc_pdata->name = fdt_getprop(blob, node,
					     "remoteproc-name", NULL);

		/* Default is internal memory mapped */
		uc_pdata->mem_type = RPROC_INTERNAL_MEMORY_MAPPED;
		tmp = fdtdec_get_bool(blob, node,
				      "remoteproc-internal-memory-mapped");
		if (tmp)
			uc_pdata->mem_type = RPROC_INTERNAL_MEMORY_MAPPED;
#else
		/* Nothing much we can do about this, can we? */
		return -EINVAL;
#endif

	} else {
		struct dm_rproc_uclass_pdata *pdata = dev->platdata;

		debug("'%s': using legacy data\n", dev->name);
		if (pdata->name)
			uc_pdata->name = pdata->name;
		uc_pdata->mem_type = pdata->mem_type;
		uc_pdata->driver_plat_data = pdata->driver_plat_data;
	}

	/* Else try using device Name */
	if (!uc_pdata->name)
		uc_pdata->name = dev->name;
	if (!uc_pdata->name) {
		debug("Unnamed device!");
		return -EINVAL;
	}

	if (!rproc_name_is_unique(dev, uc_pdata->name)) {
		debug("%s duplicate name '%s'\n", dev->name, uc_pdata->name);
		return -EINVAL;
	}

	ops = rproc_get_ops(dev);
	if (!ops) {
		debug("%s driver has no ops?\n", dev->name);
		return -EINVAL;
	}

	if (!ops->load || !ops->start) {
		debug("%s driver has missing mandatory ops?\n", dev->name);
		return -EINVAL;
	}

	return 0;
}

/**
 * rproc_post_probe() - post probe accessor for the uclass
 * @dev:	deivce we finished probing
 *
 * initiate init function after the probe is completed. This allows
 * the remote processor drivers to split up the initializations between
 * probe and init as needed.
 *
 * Return: if the remote proc driver has a init routine, invokes it and
 * hands over the return value. overall, 0 if all went well, else appropriate
 * error value.
 */
static int rproc_post_probe(struct udevice *dev)
{
	const struct dm_rproc_ops *ops;

	ops = rproc_get_ops(dev);
	if (!ops) {
		debug("%s driver has no ops?\n", dev->name);
		return -EINVAL;
	}

	if (ops->init)
		return ops->init(dev);

	return 0;
}

UCLASS_DRIVER(rproc) = {
	.id = UCLASS_REMOTEPROC,
	.name = "remoteproc",
	.flags = DM_UC_FLAG_SEQ_ALIAS,
	.pre_probe = rproc_pre_probe,
	.post_probe = rproc_post_probe,
	.per_device_platdata_auto_alloc_size =
		sizeof(struct dm_rproc_uclass_pdata),
};

/* Remoteproc subsystem access functions */
/**
 * _rproc_probe_dev() - iteration helper to probe a rproc device
 * @dev:	device to probe
 * @uc_pdata:	uclass data allocated for the device
 * @data:	unused
 *
 * Return: 0 if all ok, else appropriate error value.
 */
static int _rproc_probe_dev(struct udevice *dev,
			    struct dm_rproc_uclass_pdata *uc_pdata,
			    const void *data)
{
	int ret;

	ret = device_probe(dev);

	if (ret)
		debug("%s: Failed to initialize - %d\n", dev->name, ret);
	return ret;
}

/**
 * _rproc_dev_is_probed() - check if the device has been probed
 * @dev:	device to check
 * @uc_pdata:	unused
 * @data:	unused
 *
 * Return: -EAGAIN if not probed else return 0
 */
static int _rproc_dev_is_probed(struct udevice *dev,
			    struct dm_rproc_uclass_pdata *uc_pdata,
			    const void *data)
{
	if (dev->flags & DM_FLAG_ACTIVATED)
		return 0;

	return -EAGAIN;
}

bool rproc_is_initialized(void)
{
	int ret = for_each_remoteproc_device(_rproc_dev_is_probed, NULL, NULL);
	return ret ? false : true;
}

int rproc_init(void)
{
	int ret;

	if (rproc_is_initialized()) {
		debug("Already initialized\n");
		return -EINVAL;
	}

	ret = for_each_remoteproc_device(_rproc_probe_dev, NULL, NULL);
	return ret;
}

int rproc_dev_init(int id)
{
	struct udevice *dev = NULL;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_REMOTEPROC, id, &dev);
	if (ret) {
		debug("Unknown remote processor id '%d' requested(%d)\n",
		      id, ret);
		return ret;
	}

	ret = device_probe(dev);
	if (ret)
		debug("%s: Failed to initialize - %d\n", dev->name, ret);

	return ret;
}

/*
 * Determine if a valid ELF image exists at the given memory location.
 * First look at the ELF header magic field, then make sure that it is
 * executable.
 */
static bool is_valid_elf_image(unsigned long addr, int size)
{
	Elf32_Ehdr *ehdr; /* Elf header structure pointer */

	ehdr = (Elf32_Ehdr *)addr;

	if (!IS_ELF(*ehdr) || size <= sizeof(Elf32_Ehdr)) {
		pr_err("No elf image at address 0x%08lx\n", addr);
		return false;
	}

	if (ehdr->e_type != ET_EXEC) {
		pr_err("Not a 32-bit elf image at address 0x%08lx\n", addr);
		return false;
	}

	return true;
}


/* Basic function to verify ELF image format */
static int
rproc_elf_sanity_check(struct udevice *dev, ulong addr, ulong size)
{
	Elf32_Ehdr *ehdr;
	char class;

	if (!addr) {
		dev_err(dev, "Invalid fw address?\n");
		return -EINVAL;
	}

	if (size < sizeof(Elf32_Ehdr)) {
		dev_err(dev, "Image is too small\n");
		return -EINVAL;
	}

	ehdr = (Elf32_Ehdr *)addr;

	/* We only support ELF32 at this point */
	class = ehdr->e_ident[EI_CLASS];
	if (class != ELFCLASS32) {
		dev_err(dev, "Unsupported class: %d\n", class);
		return -EINVAL;
	}

	/* We assume the firmware has the same endianness as the host */
# ifdef __LITTLE_ENDIAN
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
# else /* BIG ENDIAN */
	if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB) {
# endif
		dev_err(dev, "Unsupported firmware endianness\n");
		return -EINVAL;
	}

	if (size < ehdr->e_shoff + sizeof(Elf32_Shdr)) {
		dev_err(dev, "Image is too small\n");
		return -EINVAL;
	}

	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) {
		dev_err(dev, "Image is corrupted (bad magic)\n");
		return -EINVAL;
	}

	if (ehdr->e_phnum == 0) {
		dev_err(dev, "No loadable segments\n");
		return -EINVAL;
	}

	if (ehdr->e_phoff > size) {
		dev_err(dev, "Firmware size is too small\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * A very simple elf loader, assumes the image is valid, returns the
 * entry point address.
 */
static int rproc_load_elf_image(struct udevice *dev, unsigned long addr,
				unsigned long *entry)
{
	Elf32_Ehdr *ehdr; /* Elf header structure pointer */
	Elf32_Phdr *phdr; /* Program header structure pointer */
	const struct dm_rproc_ops *ops;
	unsigned int i;

	ehdr = (Elf32_Ehdr *)addr;
	phdr = (Elf32_Phdr *)(addr + ehdr->e_phoff);

	ops = rproc_get_ops(dev);
	if (!ops) {
		dev_dbg(dev, "Driver has no ops?\n");
		return -EINVAL;
	}

	/* Load each program header */
	for (i = 0; i < ehdr->e_phnum; ++i) {
		void *dst = (void *)(uintptr_t)phdr->p_paddr;
		void *src = (void *)addr + phdr->p_offset;

		if (phdr->p_type != PT_LOAD)
			continue;

		if (ops->da_to_pa)
			dst = (void *)ops->da_to_pa(dev, (ulong)dst);

		dev_dbg(dev, "Loading phdr %i to 0x%p (%i bytes)\n",
			i, dst, phdr->p_filesz);
		if (phdr->p_filesz)
			memcpy(dst, src, phdr->p_filesz);
		if (phdr->p_filesz != phdr->p_memsz)
			memset(dst + phdr->p_filesz, 0x00,
			       phdr->p_memsz - phdr->p_filesz);
		flush_cache(rounddown((int)dst, ARCH_DMA_MINALIGN),
			    roundup((int)dst + phdr->p_filesz,
				    ARCH_DMA_MINALIGN) -
			    rounddown((int)dst, ARCH_DMA_MINALIGN));
		++phdr;
	}

	*entry = ehdr->e_entry;

	return 0;
}

/* Helper to find resource table in an ELF image */
static Elf32_Shdr *find_rsc_table(struct udevice *dev, Elf32_Ehdr *ehdr,
				  size_t fw_size)
{
	Elf32_Shdr *shdr;
	int i;
	const char *name_table;
	struct resource_table *table = NULL;
	const u8 *elf_data = (void *)ehdr;

	/* look for the resource table and handle it */
	shdr = (Elf32_Shdr *)(elf_data + ehdr->e_shoff);
	name_table = (const char *)(elf_data +
				    shdr[ehdr->e_shstrndx].sh_offset);

	for (i = 0; i < ehdr->e_shnum; i++, shdr++) {
		u32 size = shdr->sh_size;
		u32 offset = shdr->sh_offset;

		if (strcmp(name_table + shdr->sh_name, ".resource_table"))
			continue;

		table = (struct resource_table *)(elf_data + offset);

		/* make sure we have the entire table */
		if (offset + size > fw_size || offset + size < size) {
			dev_err(dev, "resource table truncated\n");
			return NULL;
		}

		/* make sure table has at least the header */
		if (sizeof(struct resource_table) > size) {
			dev_err(dev, "header-less resource table\n");
			return NULL;
		}

		/* we don't support any version beyond the first */
		if (table->ver != 1) {
			dev_err(dev, "unsupported fw ver: %d\n", table->ver);
			return NULL;
		}

		/* make sure reserved bytes are zeroes */
		if (table->reserved[0] || table->reserved[1]) {
			dev_err(dev, "non zero reserved bytes\n");
			return NULL;
		}

		/* make sure the offsets array isn't truncated */
		if (table->num * sizeof(table->offset[0]) +
				sizeof(struct resource_table) > size) {
			dev_err(dev, "resource table incomplete\n");
			return NULL;
		}

		return shdr;
	}

	return NULL;
}

/**
 * rproc_elf_find_load_rsc_table() - find the loaded resource table
 * @dev: the rproc device
 * @fw_addr: the ELF firmware image address
 * @fw_size: the ELF firmware image size
 * @rsc_addr: resource table address
 * @rsc_size: resource table size
 *
 * This function finds the resource table, load it and return its address.
 *
 * Return: 0 if all ok, else appropriate error value.
 */
static int rproc_elf_find_load_rsc_table(struct udevice *dev,
					 ulong fw_addr, ulong fw_size,
					 ulong *rsc_addr,
					 unsigned int *rsc_size)
{
	const struct dm_rproc_ops *ops;
	Elf32_Ehdr *ehdr = (Elf32_Ehdr *)fw_addr;
	Elf32_Shdr *shdr;
	void *src;
	void *dst;

	shdr = find_rsc_table(dev, ehdr, fw_size);
	if (!shdr)
		return -ENODATA;

	*rsc_addr = (ulong)shdr->sh_addr;
	*rsc_size = (unsigned int)shdr->sh_size;

	ops = rproc_get_ops(dev);
	if (ops->da_to_pa)
		dst = (void *)ops->da_to_pa(dev, (ulong)shdr->sh_addr);
	else
		dst = (void *)shdr->sh_addr;

	dev_dbg(dev, "Loading resource table to 0x%8lx (%i bytes)\n",
		(ulong)dst, shdr->sh_size);

	src = (void *)fw_addr + shdr->sh_offset;

	memcpy(dst, src, shdr->sh_size);
	flush_cache(rounddown((int)dst, ARCH_DMA_MINALIGN),
		    roundup((int)dst + shdr->sh_size,
			    ARCH_DMA_MINALIGN) -
		    rounddown((int)dst, ARCH_DMA_MINALIGN));

	return 0;
}

int rproc_load(int id, ulong addr, ulong size)
{
	struct udevice *dev = NULL;
	struct dm_rproc_uclass_pdata *uc_pdata;
	const struct dm_rproc_ops *ops;
	unsigned long entry;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_REMOTEPROC, id, &dev);
	if (ret) {
		debug("Unknown remote processor id '%d' requested(%d)\n",
		      id, ret);
		return ret;
	}

	uc_pdata = dev_get_uclass_platdata(dev);

	ops = rproc_get_ops(dev);
	if (!ops) {
		debug("%s driver has no ops?\n", dev->name);
		return -EINVAL;
	}

	debug("Loading to '%s' from address 0x%08lX size of %lu bytes\n",
	      uc_pdata->name, addr, size);

	if (is_valid_elf_image(addr, size)) {
		if (rproc_elf_sanity_check(dev, addr, size))
			return -EINVAL;

		/* Elf file load */
		if (ops->reset)
			ops->reset(dev);

		return rproc_load_elf_image(dev, addr, &entry);

	} else {
		/* Binary file load */
		if (ops->load)
			return ops->load(dev, addr, size);
	}

	debug("%s: data corruption?? mandatory function is missing!\n",
	      dev->name);

	return -EINVAL;
};

int rproc_load_rsc_table(int id, ulong addr, ulong size, ulong *rsc_addr,
			 unsigned int *rsc_size)
{
	struct udevice *dev = NULL;
	const struct dm_rproc_ops *ops;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_REMOTEPROC, id, &dev);
	if (ret) {
		debug("Unknown remote processor id '%d' requested(%d)\n",
		      id, ret);
		return -EINVAL;
	}

	ops = rproc_get_ops(dev);
	if (!ops) {
		dev_dbg(dev, "Driver has no ops?\n");
		return -EINVAL;
	}

	dev_dbg(dev, "Loocking for resource table from address 0x%08lX size of %lu bytes\n",
		addr, size);

	if (!rproc_elf_sanity_check(dev, addr, size)) {
		/* load elf image */
		ret = rproc_elf_find_load_rsc_table(dev, addr, size, rsc_addr,
						    rsc_size);
		if (ret) {
			dev_dbg(dev, "No resource table found\n");
			return -ENODATA;
		}
		dev_dbg(dev, "Resource table at 0x%08lx, size 0x%x!\n",
			*rsc_addr, *rsc_size);
		return 0;
	}

	return -ENODATA;
};

/*
 * Completely internal helper enums..
 * Keeping this isolated helps this code evolve independent of other
 * parts..
 */
enum rproc_ops {
	RPROC_START,
	RPROC_STOP,
	RPROC_RESET,
	RPROC_PING,
	RPROC_RUNNING,
};

/**
 * _rproc_ops_wrapper() - wrapper for invoking remote proc driver callback
 * @id:		id of the remote processor
 * @op:		one of rproc_ops that indicate what operation to invoke
 *
 * Most of the checks and verification for remoteproc operations are more
 * or less same for almost all operations. This allows us to put a wrapper
 * and use the common checks to allow the driver to function appropriately.
 *
 * Return: 0 if all ok, else appropriate error value.
 */
static int _rproc_ops_wrapper(int id, enum rproc_ops op)
{
	struct udevice *dev = NULL;
	struct dm_rproc_uclass_pdata *uc_pdata;
	const struct dm_rproc_ops *ops;
	int (*fn)(struct udevice *dev);
	bool mandatory = false;
	char *op_str;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_REMOTEPROC, id, &dev);
	if (ret) {
		debug("Unknown remote processor id '%d' requested(%d)\n",
		      id, ret);
		return ret;
	}

	uc_pdata = dev_get_uclass_platdata(dev);

	ops = rproc_get_ops(dev);
	if (!ops) {
		debug("%s driver has no ops?\n", dev->name);
		return -EINVAL;
	}
	switch (op) {
	case RPROC_START:
		fn = ops->start;
		mandatory = true;
		op_str = "Starting";
		break;
	case RPROC_STOP:
		fn = ops->stop;
		op_str = "Stopping";
		break;
	case RPROC_RESET:
		fn = ops->reset;
		op_str = "Resetting";
		break;
	case RPROC_RUNNING:
		fn = ops->is_running;
		op_str = "Checking if running:";
		break;
	case RPROC_PING:
		fn = ops->ping;
		op_str = "Pinging";
		break;
	default:
		debug("what is '%d' operation??\n", op);
		return -EINVAL;
	}

	debug("%s %s...\n", op_str, uc_pdata->name);
	if (fn)
		return fn(dev);

	if (mandatory)
		debug("%s: data corruption?? mandatory function is missing!\n",
		      dev->name);

	return -ENOSYS;
}

int rproc_start(int id)
{
	return _rproc_ops_wrapper(id, RPROC_START);
};

int rproc_stop(int id)
{
	return _rproc_ops_wrapper(id, RPROC_STOP);
};

int rproc_reset(int id)
{
	return _rproc_ops_wrapper(id, RPROC_RESET);
};

int rproc_ping(int id)
{
	return _rproc_ops_wrapper(id, RPROC_PING);
};

int rproc_is_running(int id)
{
	return _rproc_ops_wrapper(id, RPROC_RUNNING);
};
