// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016
 *
 * Michael Kurz, <michi.kurz@gmail.com>
 *
 * STM32 QSPI driver
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <reset.h>
#include <spi.h>
#include <spi_flash.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <asm/arch/stm32.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>

struct stm32_qspi_regs {
	u32 cr;		/* 0x00 */
	u32 dcr;	/* 0x04 */
	u32 sr;		/* 0x08 */
	u32 fcr;	/* 0x0C */
	u32 dlr;	/* 0x10 */
	u32 ccr;	/* 0x14 */
	u32 ar;		/* 0x18 */
	u32 abr;	/* 0x1C */
	u32 dr;		/* 0x20 */
	u32 psmkr;	/* 0x24 */
	u32 psmar;	/* 0x28 */
	u32 pir;	/* 0x2C */
	u32 lptr;	/* 0x30 */
};

/*
 * QUADSPI control register
 */
#define STM32_QSPI_CR_EN		BIT(0)
#define STM32_QSPI_CR_ABORT		BIT(1)
#define STM32_QSPI_CR_DMAEN		BIT(2)
#define STM32_QSPI_CR_TCEN		BIT(3)
#define STM32_QSPI_CR_SSHIFT		BIT(4)
#define STM32_QSPI_CR_DFM		BIT(6)
#define STM32_QSPI_CR_FSEL		BIT(7)
#define STM32_QSPI_CR_FTHRES_SHIFT	(8)
#define STM32_QSPI_CR_TEIE		BIT(16)
#define STM32_QSPI_CR_TCIE		BIT(17)
#define STM32_QSPI_CR_FTIE		BIT(18)
#define STM32_QSPI_CR_SMIE		BIT(19)
#define STM32_QSPI_CR_TOIE		BIT(20)
#define STM32_QSPI_CR_APMS		BIT(22)
#define STM32_QSPI_CR_PMM		BIT(23)
#define STM32_QSPI_CR_PRESCALER_MASK	GENMASK(7, 0)
#define STM32_QSPI_CR_PRESCALER_SHIFT	(24)

/*
 * QUADSPI device configuration register
 */
#define STM32_QSPI_DCR_CKMODE		BIT(0)
#define STM32_QSPI_DCR_CSHT_MASK	GENMASK(2, 0)
#define STM32_QSPI_DCR_CSHT_SHIFT	(8)
#define STM32_QSPI_DCR_FSIZE_MASK	GENMASK(4, 0)
#define STM32_QSPI_DCR_FSIZE_SHIFT	(16)

/*
 * QUADSPI status register
 */
#define STM32_QSPI_SR_TEF		BIT(0)
#define STM32_QSPI_SR_TCF		BIT(1)
#define STM32_QSPI_SR_FTF		BIT(2)
#define STM32_QSPI_SR_SMF		BIT(3)
#define STM32_QSPI_SR_TOF		BIT(4)
#define STM32_QSPI_SR_BUSY		BIT(5)
#define STM32_QSPI_SR_FLEVEL_MASK	GENMASK(5, 0)
#define STM32_QSPI_SR_FLEVEL_SHIFT	(8)

/*
 * QUADSPI flag clear register
 */
#define STM32_QSPI_FCR_CTEF		BIT(0)
#define STM32_QSPI_FCR_CTCF		BIT(1)
#define STM32_QSPI_FCR_CSMF		BIT(3)
#define STM32_QSPI_FCR_CTOF		BIT(4)

/*
 * QUADSPI communication configuration register
 */
#define STM32_QSPI_CCR_DDRM		BIT(31)
#define STM32_QSPI_CCR_DHHC		BIT(30)
#define STM32_QSPI_CCR_SIOO		BIT(28)
#define STM32_QSPI_CCR_FMODE_SHIFT	(26)
#define STM32_QSPI_CCR_DMODE_SHIFT	(24)
#define STM32_QSPI_CCR_DCYC_SHIFT	(18)
#define STM32_QSPI_CCR_DCYC_MASK	GENMASK(4, 0)
#define STM32_QSPI_CCR_ABSIZE_SHIFT	(16)
#define STM32_QSPI_CCR_ABMODE_SHIFT	(14)
#define STM32_QSPI_CCR_ADSIZE_SHIFT	(12)
#define STM32_QSPI_CCR_ADMODE_SHIFT	(10)
#define STM32_QSPI_CCR_IMODE_SHIFT	(8)
#define STM32_QSPI_CCR_INSTRUCTION_MASK	GENMASK(7, 0)

enum STM32_QSPI_CCR_IMODE {
	STM32_QSPI_CCR_IMODE_NONE = 0,
	STM32_QSPI_CCR_IMODE_ONE_LINE = 1,
	STM32_QSPI_CCR_IMODE_TWO_LINE = 2,
	STM32_QSPI_CCR_IMODE_FOUR_LINE = 3,
};

enum STM32_QSPI_CCR_ADMODE {
	STM32_QSPI_CCR_ADMODE_NONE = 0,
	STM32_QSPI_CCR_ADMODE_ONE_LINE = 1,
	STM32_QSPI_CCR_ADMODE_TWO_LINE = 2,
	STM32_QSPI_CCR_ADMODE_FOUR_LINE = 3,
};

enum STM32_QSPI_CCR_ADSIZE {
	STM32_QSPI_CCR_ADSIZE_8BIT = 0,
	STM32_QSPI_CCR_ADSIZE_16BIT = 1,
	STM32_QSPI_CCR_ADSIZE_24BIT = 2,
	STM32_QSPI_CCR_ADSIZE_32BIT = 3,
};

enum STM32_QSPI_CCR_ABMODE {
	STM32_QSPI_CCR_ABMODE_NONE = 0,
	STM32_QSPI_CCR_ABMODE_ONE_LINE = 1,
	STM32_QSPI_CCR_ABMODE_TWO_LINE = 2,
	STM32_QSPI_CCR_ABMODE_FOUR_LINE = 3,
};

enum STM32_QSPI_CCR_ABSIZE {
	STM32_QSPI_CCR_ABSIZE_8BIT = 0,
	STM32_QSPI_CCR_ABSIZE_16BIT = 1,
	STM32_QSPI_CCR_ABSIZE_24BIT = 2,
	STM32_QSPI_CCR_ABSIZE_32BIT = 3,
};

enum STM32_QSPI_CCR_DMODE {
	STM32_QSPI_CCR_DMODE_NONE = 0,
	STM32_QSPI_CCR_DMODE_ONE_LINE = 1,
	STM32_QSPI_CCR_DMODE_TWO_LINE = 2,
	STM32_QSPI_CCR_DMODE_FOUR_LINE = 3,
};

enum STM32_QSPI_CCR_FMODE {
	STM32_QSPI_CCR_IND_WRITE = 0,
	STM32_QSPI_CCR_IND_READ = 1,
	STM32_QSPI_CCR_AUTO_POLL = 2,
	STM32_QSPI_CCR_MEM_MAP = 3,
};

/* default SCK frequency, unit: HZ */
#define STM32_QSPI_DEFAULT_SCK_FREQ 108000000

#define STM32_QSPI_MAX_CHIP 2

#define STM32_QSPI_FIFO_TIMEOUT_US 30000
#define STM32_QSPI_CMD_TIMEOUT_US 1000000
#define STM32_BUSY_TIMEOUT_US 100000
#define STM32_ABT_TIMEOUT_US 100000

struct stm32_qspi_platdata {
	u32 base;
	u32 memory_map;
	u32 max_hz;
};

struct stm32_qspi_flash {
	u32 cr;
	u32 dcr;
	u32 mode;
	bool initialized;
};

struct stm32_qspi_priv {
	struct stm32_qspi_regs *regs;
	struct stm32_qspi_flash flash[STM32_QSPI_MAX_CHIP];
	ulong clock_rate;
	u32 max_hz;
	u32 mode;
	int cs_used;

	u32 command;
	u32 address;
	u32 dummycycles;
#define CMD_HAS_ADR	BIT(24)
#define CMD_HAS_DUMMY	BIT(25)
#define CMD_HAS_DATA	BIT(26)
};

static int _stm32_qspi_wait_for_not_busy(struct stm32_qspi_priv *priv)
{
	u32 sr;
	int ret;

	ret = readl_poll_timeout(&priv->regs->sr, sr,
				 !(sr & STM32_QSPI_SR_BUSY),
				 STM32_BUSY_TIMEOUT_US);
	if (ret)
		pr_err("busy timeout (stat:%#x)\n", sr);

	return ret;
}

static unsigned int _stm32_qspi_gen_ccr(struct stm32_qspi_priv *priv, u8 fmode)
{
	unsigned int ccr_reg = 0;
	u8 imode, admode, dmode;
	u32 mode = priv->mode;
	u32 cmd = (priv->command & STM32_QSPI_CCR_INSTRUCTION_MASK);

	imode = STM32_QSPI_CCR_IMODE_ONE_LINE;
	admode = STM32_QSPI_CCR_ADMODE_ONE_LINE;
	dmode = STM32_QSPI_CCR_DMODE_ONE_LINE;

	if ((priv->command & CMD_HAS_ADR) && (priv->command & CMD_HAS_DATA)) {
		if (fmode == STM32_QSPI_CCR_IND_WRITE) {
			if (mode & SPI_TX_QUAD)
				dmode = STM32_QSPI_CCR_DMODE_FOUR_LINE;
			else if (mode & SPI_TX_DUAL)
				dmode = STM32_QSPI_CCR_DMODE_TWO_LINE;
		} else if ((fmode == STM32_QSPI_CCR_MEM_MAP) ||
			 (fmode == STM32_QSPI_CCR_IND_READ)) {
			if (mode & SPI_RX_QUAD)
				dmode = STM32_QSPI_CCR_DMODE_FOUR_LINE;
			else if (mode & SPI_RX_DUAL)
				dmode = STM32_QSPI_CCR_DMODE_TWO_LINE;
		}
	}

	if (priv->command & CMD_HAS_DATA)
		ccr_reg |= (dmode << STM32_QSPI_CCR_DMODE_SHIFT);

	if (priv->command & CMD_HAS_DUMMY)
		ccr_reg |= ((priv->dummycycles & STM32_QSPI_CCR_DCYC_MASK)
				<< STM32_QSPI_CCR_DCYC_SHIFT);

	if (priv->command & CMD_HAS_ADR) {
		ccr_reg |= (STM32_QSPI_CCR_ADSIZE_24BIT
				<< STM32_QSPI_CCR_ADSIZE_SHIFT);
		ccr_reg |= (admode << STM32_QSPI_CCR_ADMODE_SHIFT);
	}

	ccr_reg |= (fmode << STM32_QSPI_CCR_FMODE_SHIFT);
	ccr_reg |= (imode << STM32_QSPI_CCR_IMODE_SHIFT);
	ccr_reg |= cmd;

	return ccr_reg;
}

static void _stm32_qspi_enable_mmap(struct stm32_qspi_priv *priv,
				    struct spi_flash *flash)
{
	unsigned int ccr_reg;

	priv->command = flash->read_cmd | CMD_HAS_ADR | CMD_HAS_DATA
			| CMD_HAS_DUMMY;
	priv->dummycycles = flash->dummy_byte * 8;

	ccr_reg = _stm32_qspi_gen_ccr(priv, STM32_QSPI_CCR_MEM_MAP);

	_stm32_qspi_wait_for_not_busy(priv);

	writel(ccr_reg, &priv->regs->ccr);

	priv->dummycycles = 0;
}

static void _stm32_qspi_disable_mmap(struct stm32_qspi_priv *priv)
{
	setbits_le32(&priv->regs->cr, STM32_QSPI_CR_ABORT);
}

static void _stm32_qspi_set_xfer_length(struct stm32_qspi_priv *priv,
					u32 length)
{
	writel(length - 1, &priv->regs->dlr);
}

static void _stm32_qspi_start_xfer(struct stm32_qspi_priv *priv, u32 cr_reg)
{
	writel(cr_reg, &priv->regs->ccr);

	if (priv->command & CMD_HAS_ADR)
		writel(priv->address, &priv->regs->ar);
}

static int _stm32_qspi_wait_cmd(struct stm32_qspi_priv *priv, bool wait_nobusy)
{
	u32 sr;
	int ret;

	if (wait_nobusy)
		return _stm32_qspi_wait_for_not_busy(priv);

	ret = readl_poll_timeout(&priv->regs->sr, sr,
				 sr & STM32_QSPI_SR_TCF,
				 STM32_QSPI_CMD_TIMEOUT_US);
	if (ret) {
		pr_err("cmd timeout (stat:%#x)\n", sr);
	} else if (readl(&priv->regs->sr) & STM32_QSPI_SR_TEF) {
		pr_err("transfer error (stat:%#x)\n", sr);
		ret = -EIO;
	}

	/* clear flags */
	writel(STM32_QSPI_FCR_CTCF | STM32_QSPI_FCR_CTEF, &priv->regs->fcr);

	return ret;
}

static void _stm32_qspi_read_fifo(u8 *val, void __iomem *addr)
{
	*val = readb(addr);
}

static void _stm32_qspi_write_fifo(u8 *val, void __iomem *addr)
{
	writeb(*val, addr);
}

static int _stm32_qspi_poll(struct stm32_qspi_priv *priv,
			    const u8 *dout, u8 *din, u32 len)
{
	void (*fifo)(u8 *val, void __iomem *addr);
	u32 sr;
	u8 *buf;
	int ret;

	if (din) {
		fifo = _stm32_qspi_read_fifo;
		buf = din;

	} else {
		fifo = _stm32_qspi_write_fifo;
		buf = (u8 *)dout;
	}

	while (len--) {
		ret = readl_poll_timeout(&priv->regs->sr, sr,
					 sr & STM32_QSPI_SR_FTF,
					 STM32_QSPI_FIFO_TIMEOUT_US);
		if (ret) {
			pr_err("fifo timeout (len:%d stat:%#x)\n", len, sr);
			return ret;
		}

		fifo(buf++, &priv->regs->dr);
	}

	return 0;
}

static int _stm32_qspi_xfer(struct stm32_qspi_priv *priv,
			    struct spi_flash *flash, unsigned int bitlen,
			    const u8 *dout, u8 *din, unsigned long flags)
{
	unsigned int words = bitlen / 8;
	u32 ccr_reg;
	int ret;

	if (flags & SPI_XFER_MMAP) {
		_stm32_qspi_enable_mmap(priv, flash);
		return 0;
	} else if (flags & SPI_XFER_MMAP_END) {
		_stm32_qspi_disable_mmap(priv);
		return 0;
	}

	if (bitlen == 0)
		return -1;

	if (bitlen % 8) {
		debug("spi_xfer: Non byte aligned SPI transfer\n");
		return -1;
	}

	if (dout && din) {
		debug("spi_xfer: QSPI cannot have data in and data out set\n");
		return -1;
	}

	if (!dout && (flags & SPI_XFER_BEGIN)) {
		debug("spi_xfer: QSPI transfer must begin with command\n");
		return -1;
	}

	if (dout) {
		if (flags & SPI_XFER_BEGIN) {
			/* data is command */
			priv->command = dout[0] | CMD_HAS_DATA;
			if (words >= 4) {
				/* address is here too */
				priv->address = (dout[1] << 16) |
						(dout[2] << 8) | dout[3];
				priv->command |= CMD_HAS_ADR;
			}

			if (words > 4) {
				/* rest is dummy bytes */
				priv->dummycycles = (words - 4) * 8;
				priv->command |= CMD_HAS_DUMMY;
			}

			if (flags & SPI_XFER_END) {
				/* command without data */
				priv->command &= ~(CMD_HAS_DATA);
			}
		}

		if (flags & SPI_XFER_END) {
			bool cmd_has_data = priv->command & CMD_HAS_DATA;

			ccr_reg = _stm32_qspi_gen_ccr(priv,
						      STM32_QSPI_CCR_IND_WRITE);

			ret = _stm32_qspi_wait_for_not_busy(priv);
			if (ret)
				return ret;

			if (cmd_has_data)
				_stm32_qspi_set_xfer_length(priv, words);

			_stm32_qspi_start_xfer(priv, ccr_reg);

			debug("%s: write: ccr:0x%08x adr:0x%08x\n",
			      __func__, priv->regs->ccr, priv->regs->ar);

			if (cmd_has_data) {
				ret = _stm32_qspi_poll(priv, dout, NULL, words);
				if (ret)
					return ret;
			}

			ret = _stm32_qspi_wait_cmd(priv, !cmd_has_data);
			if (ret)
				return ret;
		}
	} else if (din) {
		ccr_reg = _stm32_qspi_gen_ccr(priv, STM32_QSPI_CCR_IND_READ);

		ret = _stm32_qspi_wait_for_not_busy(priv);
		if (ret)
			return ret;

		_stm32_qspi_set_xfer_length(priv, words);

		_stm32_qspi_start_xfer(priv, ccr_reg);

		debug("%s: read: ccr:0x%08x adr:0x%08x len:%d\n", __func__,
		      priv->regs->ccr, priv->regs->ar, priv->regs->dlr);

		ret = _stm32_qspi_poll(priv, NULL, din, words);
		if (ret)
			return ret;

		ret = _stm32_qspi_wait_cmd(priv, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int _stm32_qspi_tx(struct stm32_qspi_priv *priv,
			  const struct spi_mem_op *op)
{
	if (!op->data.nbytes)
		return 0;

	if (op->data.dir == SPI_MEM_DATA_IN)
		return _stm32_qspi_poll(priv, NULL, op->data.buf.in,
					op->data.nbytes);

	return _stm32_qspi_poll(priv, op->data.buf.out, NULL, op->data.nbytes);
}

static int _stm32_qspi_get_mode(u8 buswidth)
{
	if (buswidth == 4)
		return 3;

	return buswidth;
}

static int stm32_qspi_exec_op(struct spi_slave *slave,
			      const struct spi_mem_op *op)
{
	struct stm32_qspi_priv *priv = dev_get_priv(slave->dev->parent);
	u32 cr, ccr;
	int timeout, ret;

	debug("%s: cmd:%#x mode:%d.%d.%d.%d addr:%#llx len:%#x\n",
	      __func__, op->cmd.opcode, op->cmd.buswidth, op->addr.buswidth,
	      op->dummy.buswidth, op->data.buswidth,
	      op->addr.val, op->data.nbytes);

	ret = _stm32_qspi_wait_for_not_busy(priv);
	if (ret)
		return ret;

	ccr = (STM32_QSPI_CCR_IND_WRITE << STM32_QSPI_CCR_FMODE_SHIFT);
	if (op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes)
		ccr = (STM32_QSPI_CCR_IND_READ << STM32_QSPI_CCR_FMODE_SHIFT);

	if (op->data.nbytes)
		_stm32_qspi_set_xfer_length(priv, op->data.nbytes);

	ccr |= op->cmd.opcode;
	ccr |= (_stm32_qspi_get_mode(op->cmd.buswidth)
		<< STM32_QSPI_CCR_IMODE_SHIFT);

	if (op->addr.nbytes) {
		ccr |= ((op->addr.nbytes - 1) << STM32_QSPI_CCR_ADSIZE_SHIFT);
		ccr |= (_stm32_qspi_get_mode(op->addr.buswidth)
			<< STM32_QSPI_CCR_ADMODE_SHIFT);
	}

	if (op->dummy.buswidth && op->dummy.nbytes)
		ccr |= (op->dummy.nbytes * 8 / op->dummy.buswidth
			<< STM32_QSPI_CCR_DCYC_SHIFT);

	if (op->data.nbytes)
		ccr |= (_stm32_qspi_get_mode(op->data.buswidth)
			<< STM32_QSPI_CCR_DMODE_SHIFT);

	writel(ccr, &priv->regs->ccr);

	if (op->addr.nbytes)
		writel(op->addr.val, &priv->regs->ar);

	ret = _stm32_qspi_tx(priv, op);
	if (ret)
		goto abort;

	/* wait end of tx in indirect mode */
	ret = _stm32_qspi_wait_cmd(priv, !op->data.nbytes);
	if (ret)
		goto abort;

	return 0;

abort:
	setbits_le32(&priv->regs->cr, STM32_QSPI_CR_ABORT);

	/* wait clear of abort bit by hw */
	timeout = readl_poll_timeout(&priv->regs->cr, cr,
				     !(cr & STM32_QSPI_CR_ABORT),
				     STM32_ABT_TIMEOUT_US);

	writel(STM32_QSPI_FCR_CTCF, &priv->regs->fcr);

	if (ret || timeout)
		debug("%s ret:%d abort timeout:%d\n", __func__, ret, timeout);

	return ret;
}

static int stm32_qspi_ofdata_to_platdata(struct udevice *bus)
{
	struct resource res_regs, res_mem;
	struct stm32_qspi_platdata *plat = bus->platdata;
	int ret;

	ret = dev_read_resource_byname(bus, "qspi", &res_regs);
	if (ret) {
		debug("Error: can't get regs base addresses(ret = %d)!\n", ret);
		return -ENOMEM;
	}
	ret = dev_read_resource_byname(bus, "qspi_mm", &res_mem);
	if (ret) {
		debug("Error: can't get mmap base address(ret = %d)!\n", ret);
		return -ENOMEM;
	}

	plat->max_hz = dev_read_u32_default(bus, "spi-max-frequency",
					    STM32_QSPI_DEFAULT_SCK_FREQ);

	plat->base = res_regs.start;
	plat->memory_map = res_mem.start;

	debug("%s: regs=<0x%x> mapped=<0x%x>, max-frequency=%d\n",
	      __func__,
	      plat->base,
	      plat->memory_map,
	      plat->max_hz
	      );

	return 0;
}

static int stm32_qspi_probe(struct udevice *bus)
{
	struct stm32_qspi_platdata *plat = dev_get_platdata(bus);
	struct stm32_qspi_priv *priv = dev_get_priv(bus);
	struct dm_spi_bus *dm_spi_bus = bus->uclass_priv;
	struct clk clk;
	struct reset_ctl reset_ctl;
	int ret;

	dm_spi_bus->max_hz = plat->max_hz;

	priv->regs = (struct stm32_qspi_regs *)(uintptr_t)plat->base;
	priv->cs_used = -1;
	priv->max_hz = plat->max_hz;

	ret = clk_get_by_index(bus, 0, &clk);
	if (ret < 0)
		return ret;

	ret = clk_enable(&clk);

	if (ret) {
		dev_err(bus, "failed to enable clock\n");
		return ret;
	}

	priv->clock_rate = clk_get_rate(&clk);
	if (!priv->clock_rate) {
		clk_disable(&clk);
		return -EINVAL;
	}

	ret = reset_get_by_index(bus, 0, &reset_ctl);
	if (ret) {
		if (ret != -ENOENT) {
			dev_err(bus, "failed to get reset\n");
			clk_disable(&clk);
			return ret;
		}
	} else {
		/* Reset QSPI controller */
		reset_assert(&reset_ctl);
		udelay(2);
		reset_deassert(&reset_ctl);
	}

	setbits_le32(&priv->regs->cr, STM32_QSPI_CR_SSHIFT);

	/* Set dcr fsize to max address */
	setbits_le32(&priv->regs->dcr,
		     STM32_QSPI_DCR_FSIZE_MASK << STM32_QSPI_DCR_FSIZE_SHIFT);

	return 0;
}

static int stm32_qspi_remove(struct udevice *bus)
{
	return 0;
}

static int stm32_qspi_claim_bus(struct udevice *dev)
{
	struct stm32_qspi_priv *priv = dev_get_priv(dev->parent);
	struct dm_spi_slave_platdata *slave_plat = dev_get_parent_platdata(dev);
	int slave_cs = slave_plat->cs;

	if (slave_cs >= STM32_QSPI_MAX_CHIP)
		return -ENODEV;

	if (priv->cs_used != slave_cs) {
		struct stm32_qspi_flash *flash = &priv->flash[slave_cs];

		priv->cs_used = slave_cs;

		if (flash->initialized) {
			/* Set the configuration: speed + mode + cs */
			writel(flash->cr, &priv->regs->cr);
			writel(flash->dcr, &priv->regs->dcr);
			priv->mode = flash->mode;
		} else {
			/* Set chip select */
			clrsetbits_le32(&priv->regs->cr, STM32_QSPI_CR_FSEL,
					priv->cs_used ? STM32_QSPI_CR_FSEL : 0);

			/* Save the configuration: speed + mode + cs */
			flash->cr = readl(&priv->regs->cr);
			flash->dcr = readl(&priv->regs->dcr);
			flash->mode = priv->mode;

			flash->initialized = true;
		}
	}

	setbits_le32(&priv->regs->cr, STM32_QSPI_CR_EN);

	return 0;
}

static int stm32_qspi_release_bus(struct udevice *dev)
{
	struct stm32_qspi_priv *priv = dev_get_priv(dev->parent);

	clrbits_le32(&priv->regs->cr, STM32_QSPI_CR_EN);

	return 0;
}

static int stm32_qspi_xfer(struct udevice *dev, unsigned int bitlen,
			   const void *dout, void *din, unsigned long flags)
{
	struct stm32_qspi_priv *priv = dev_get_priv(dev->parent);
	struct spi_flash *flash = dev_get_uclass_priv(dev);

	return _stm32_qspi_xfer(priv, flash, bitlen, (const u8 *)dout,
				(u8 *)din, flags);
}

static int stm32_qspi_set_speed(struct udevice *bus, uint speed)
{
	struct stm32_qspi_platdata *plat = bus->platdata;
	struct stm32_qspi_priv *priv = dev_get_priv(bus);
	u32 qspi_clk = priv->clock_rate;
	u32 prescaler = 255;
	u32 csht;
	int ret;

	if (speed > plat->max_hz)
		speed = plat->max_hz;

	if (speed > 0) {
		prescaler = 0;
		if (qspi_clk) {
			prescaler = DIV_ROUND_UP(qspi_clk, speed) - 1;
			if (prescaler > 255)
				prescaler = 255;
		}
	}

	csht = DIV_ROUND_UP((5 * qspi_clk) / (prescaler + 1), 100000000);
	csht = (csht - 1) & STM32_QSPI_DCR_CSHT_MASK;

	ret = _stm32_qspi_wait_for_not_busy(priv);
	if (ret)
		return ret;

	clrsetbits_le32(&priv->regs->cr,
			STM32_QSPI_CR_PRESCALER_MASK <<
			STM32_QSPI_CR_PRESCALER_SHIFT,
			prescaler << STM32_QSPI_CR_PRESCALER_SHIFT);

	clrsetbits_le32(&priv->regs->dcr,
			STM32_QSPI_DCR_CSHT_MASK << STM32_QSPI_DCR_CSHT_SHIFT,
			csht << STM32_QSPI_DCR_CSHT_SHIFT);

	debug("%s: regs=%p, speed=%d\n", __func__, priv->regs,
	      (qspi_clk / (prescaler + 1)));

	return 0;
}

static int stm32_qspi_set_mode(struct udevice *bus, uint mode)
{
	struct stm32_qspi_priv *priv = dev_get_priv(bus);
	int ret;

	ret = _stm32_qspi_wait_for_not_busy(priv);
	if (ret)
		return ret;

	if ((mode & SPI_CPHA) && (mode & SPI_CPOL))
		setbits_le32(&priv->regs->dcr, STM32_QSPI_DCR_CKMODE);
	else if (!(mode & SPI_CPHA) && !(mode & SPI_CPOL))
		clrbits_le32(&priv->regs->dcr, STM32_QSPI_DCR_CKMODE);
	else
		return -ENODEV;

	if (mode & SPI_CS_HIGH)
		return -ENODEV;

	if (mode & SPI_RX_QUAD)
		priv->mode |= SPI_RX_QUAD;
	else if (mode & SPI_RX_DUAL)
		priv->mode |= SPI_RX_DUAL;
	else
		priv->mode &= ~(SPI_RX_QUAD | SPI_RX_DUAL);

	if (mode & SPI_TX_QUAD)
		priv->mode |= SPI_TX_QUAD;
	else if (mode & SPI_TX_DUAL)
		priv->mode |= SPI_TX_DUAL;
	else
		priv->mode &= ~(SPI_TX_QUAD | SPI_TX_DUAL);

	debug("%s: regs=%p, mode=%d rx: ", __func__, priv->regs, mode);

	if (mode & SPI_RX_QUAD)
		debug("quad, tx: ");
	else if (mode & SPI_RX_DUAL)
		debug("dual, tx: ");
	else
		debug("single, tx: ");

	if (mode & SPI_TX_QUAD)
		debug("quad\n");
	else if (mode & SPI_TX_DUAL)
		debug("dual\n");
	else
		debug("single\n");

	return 0;
}

static const struct spi_controller_mem_ops stm32_qspi_mem_ops = {
	.exec_op = stm32_qspi_exec_op,
};

static const struct dm_spi_ops stm32_qspi_ops = {
	.claim_bus	= stm32_qspi_claim_bus,
	.release_bus	= stm32_qspi_release_bus,
	.xfer		= stm32_qspi_xfer,
	.set_speed	= stm32_qspi_set_speed,
	.set_mode	= stm32_qspi_set_mode,
	.mem_ops	= &stm32_qspi_mem_ops,
};

static const struct udevice_id stm32_qspi_ids[] = {
	{ .compatible = "st,stm32-qspi" },
	{ .compatible = "st,stm32f469-qspi" },
	{ }
};

U_BOOT_DRIVER(stm32_qspi) = {
	.name	= "stm32_qspi",
	.id	= UCLASS_SPI,
	.of_match = stm32_qspi_ids,
	.ops	= &stm32_qspi_ops,
	.ofdata_to_platdata = stm32_qspi_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct stm32_qspi_platdata),
	.priv_auto_alloc_size = sizeof(struct stm32_qspi_priv),
	.probe	= stm32_qspi_probe,
	.remove = stm32_qspi_remove,
};
