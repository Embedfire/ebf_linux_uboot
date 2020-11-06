/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * Configuration settings for the Freescale i.MX6UL 14x14 EVK board.
 */
#ifndef __MX6ULLFIRE_CONFIG_H
#define __MX6ULLFIRE_CONFIG_H


#include <asm/arch/imx-regs.h>
#include <linux/sizes.h>
#include <linux/stringify.h>
#include "mx6_common.h"
#include <asm/mach-imx/gpio.h>

#define PHYS_SDRAM_SIZE	SZ_512M

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(16 * SZ_1M)

#define CONFIG_MXC_UART_BASE		UART1_BASE

/* MMC Configs */
#ifdef CONFIG_FSL_USDHC
#define CONFIG_SYS_FSL_ESDHC_ADDR	USDHC2_BASE_ADDR
#endif

/* NAND stuff */
#ifdef CONFIG_NAND_MXS
#define CONFIG_SYS_MAX_NAND_DEVICE	1
#define CONFIG_SYS_NAND_BASE		0x40000000
#define CONFIG_SYS_NAND_5_ADDR_CYCLE
#define CONFIG_SYS_NAND_ONFI_DETECTION
/* DMA stuff, needed for GPMI/MXS NAND support */
#endif

/* I2C configs */
#ifdef CONFIG_CMD_I2C
#define CONFIG_SYS_I2C_MXC
#define CONFIG_SYS_I2C_MXC_I2C1		/* enable I2C bus 1 */
#define CONFIG_SYS_I2C_MXC_I2C2		/* enable I2C bus 2 */
#define CONFIG_SYS_I2C_SPEED		100000
#endif

#define CONFIG_SYS_MMC_IMG_LOAD_PART	1

#if defined(CONFIG_SYS_BOOT_NAND)
#define CONFIG_EXTRA_ENV_SETTINGS \
	"ethaddr=00:05:9f:05:d2:36\0" \
	"eth1addr=00:05:9f:05:d2:37\0" \
	"panel=TFT50AB\0" \
	"splashimage=0x82000000\0" \
	"fdt_addr=0x83000000\0" \
	"fdt_high=0xffffffff\0"	  \
	"console=ttymxc0\0" \
	"console=ttymxc0\0" \
	"loaduEnv=" \
		"load ramblock 0:1 ${loadaddr} /uEnv.txt;\0" \
	"importbootenv=echo Importing environment from ${devtype} ...; " \
		"env import -t ${loadaddr} ${filesize}\0" \
	"bootargs=console=ttymxc0 bootargs=console=ttymxc0,115200 ubi.mtd=1 "  \
		"root=ubi0:rootfs rw rootfstype=ubifs "		     \
		"mtdparts=gpmi-nand:8m(uboot),-(rootfs)coherent_pool=1M "\
		"net.ifnames=0 vt.global_cursor_default=0 quiet ${cmdline}\0"\
	"bootcmd=ubi part rootfs;ubifsmount ubi0;"\
		"ubifsload ${ramblock_addr} /lib/fireware/fatboot.img;"\
		"echo loading /uEnv.txt ...; "\
		"if run loaduEnv; then " \
			"run importbootenv;" \
			"if test -n ${flash_firmware}; then "  \
					"echo setting flash firmware...;"  \
					"mmc list;"  \
					"setenv cmdline ${storage_media};"  \
			"fi;" \
			"echo loading vmlinuz-${uname_r} ...; "\
			"load ramblock 0:1 0x80800000 /vmlinuz-${uname_r};"\
			"echo loading ${dtb} ...; "\
			"load ramblock 0:1 0x83000000 /dtbs/${uname_r}/${dtb};"\
			"dtfile 0x83000000 0x87000000  /uEnv.txt ${loadaddr};"   \
			"load ramblock 0:1 0x88000000 /initrd.img-${uname_r};"\
			"echo debug: [${bootargs}] ... ;" \
			"echo debug: [bootz] ...  ;" \
			"bootz 0x80800000 0x88000000:${filesize} 0x83000000;"	\
		"fi;\0"
#else
#define CONFIG_EXTRA_ENV_SETTINGS \
	"script=boot.scr\0" \
	"image=zImage\0" \
	"console=ttymxc0\0" \
	"fdt_high=0xffffffff\0" \
	"initrd_high=0xffffffff\0" \
	"fdt_file=undefined\0" \
	"fdt_addr=0x83000000\0" \
	"boot_fdt=try\0" \
	"ip_dyn=yes\0" \
	"ethaddr=00:05:9f:05:d2:36\0" \
	"eth1addr=00:05:9f:05:d2:37\0" \
	"videomode=video=ctfb:x:480,y:272,depth:24,pclk:108695,le:8,ri:4,up:2,lo:4,hs:41,vs:10,sync:0,vmode:0\0" \
	"mmcdev="__stringify(CONFIG_SYS_MMC_ENV_DEV)"\0" \
	"mmcpart=" __stringify(CONFIG_SYS_MMC_IMG_LOAD_PART) "\0" \
	"mmcroot=" CONFIG_MMCROOT " rootwait rw\0" \
	"mmcautodetect=yes\0" \
	"mmcargs=setenv bootargs console=${console},${baudrate} " \
		"root=${mmcroot}\0" \
	"loadbootscript=" \
		"load mmc ${mmcdev}:${mmcpart} ${loadaddr} ${script};\0" \
	"loaduEnv=" \
		"load ${devtype} ${bootpart} ${loadaddr} /uEnv.txt;\0" \
	"importbootenv=echo Importing environment from ${devtype} ...; " \
		"env import -t ${loadaddr} ${filesize}\0" \
	"bootscript=echo Running bootscript from mmc ...; " \
		"source\0" \
	"loadimage=load mmc ${mmcdev}:${mmcpart} ${loadaddr} ${image};\0" \
	"loadfdt=load mmc ${mmcdev}:${mmcpart} ${fdt_addr} ${fdt_file};\0" \
	"mmcboot=echo Booting from mmc ...; " \
		"run mmcargs; " \
		"if test ${boot_fdt} = yes || test ${boot_fdt} = try; then " \
			"if run loadfdt; then " \
				"bootz ${loadaddr} - ${fdt_addr}; " \
			"else " \
				"if test ${boot_fdt} = try; then " \
					"bootz; " \
				"else " \
					"echo WARN: Cannot load the DT; " \
				"fi; " \
			"fi; " \
		"else " \
			"bootz; " \
		"fi;\0" \
		"findfdt="\
			"if test $fdt_file = undefined; then " \
				"if test $board_name = ULZ-EVK && test $board_rev = 14X14; then " \
					"setenv fdt_file imx6ulz-14x14-evk.dtb; fi; " \
				"if test $board_name = EVK && test $board_rev = 14X14; then " \
					"setenv fdt_file imx6ull-14x14-evk.dtb; fi; " \
				"if test $fdt_file = undefined; then " \
					"echo WARNING: Could not determine dtb to use; " \
				"fi; " \
			"fi;\0" \
	"netargs=setenv bootargs console=${console},${baudrate} " \
		"root=/dev/nfs " \
	"ip=dhcp nfsroot=${serverip}:${nfsroot},v3,tcp\0" \
		"netboot=echo Booting from net ...; " \
		"run netargs; " \
		"if test ${ip_dyn} = yes; then " \
			"setenv get_cmd dhcp; " \
		"else " \
			"setenv get_cmd tftp; " \
		"fi; " \
		"${get_cmd} ${image}; " \
		"if test ${boot_fdt} = yes || test ${boot_fdt} = try; then " \
			"if ${get_cmd} ${fdt_addr} ${fdt_file}; then " \
				"bootz ${loadaddr} - ${fdt_addr}; " \
			"else " \
				"if test ${boot_fdt} = try; then " \
					"bootz; " \
				"else " \
					"echo WARN: Cannot load the DT; " \
				"fi; " \
			"fi; " \
		"else " \
			"bootz; " \
		"fi;\0" \
	"args_mmc_old=setenv bootargs console=ttymxc0 " \
		"root=/dev/mmcblk${mmcdev}p2 rw " \
		"rootfstype=ext4 " \
		"rootwait coherent_pool=1M net.ifnames=0 vt.global_cursor_default=0 quiet ${cmdline}\0" \
	"boot=${devtype} dev ${mmcdev};mmc rescan; " \
		"echo loading [${devtype} ${bootpart}] /uEnv.txt ...; "\
		"if run loaduEnv; then " \
			"run importbootenv;" \
			"if test -n ${flash_firmware}; then "  \
					"echo setting flash firmware...;"  \
					"mmc list;"  \
					"setenv cmdline ${storage_media};"  \
			"fi;" \
			"run args_mmc_old;" \
			"echo loading vmlinuz-${uname_r} ...; "\
			"load ${devtype} ${bootpart} 0x80800000 /vmlinuz-${uname_r};"\
			"echo loading ${dtb} ...; "\
			"load ${devtype} ${bootpart} 0x83000000 /dtbs/${uname_r}/${dtb};"\
			"dtfile 0x83000000 0x87000000  /uEnv.txt ${loadaddr};"   \
			"load ${devtype} ${bootpart} 0x88000000 /initrd.img-${uname_r};"\
			"echo debug: [${bootargs}] ... ;" \
			"echo debug: [bootz] ...  ;" \
			"bootz 0x80800000 0x88000000:${filesize} 0x83000000;"	\
		"fi;\0" \
	BOOTENV
/*
#define CONFIG_BOOTCOMMAND \
	   "run findfdt;" \
	   "mmc dev ${mmcdev};" \
	   "mmc dev ${mmcdev}; if mmc rescan; then " \
		   "if run loadbootscript; then " \
			   "run bootscript; " \
		   "else " \
			   "if run loadimage; then " \
				   "run mmcboot; " \
			   "else run netboot; " \
			   "fi; " \
		   "fi; " \
	   "else run netboot; fi"
*/
#endif

/* Miscellaneous configurable options */

#define CONFIG_SYS_LOAD_ADDR		CONFIG_LOADADDR
#define CONFIG_SYS_HZ			1000

/* Physical Memory Map */
#define PHYS_SDRAM			MMDC0_ARB_BASE_ADDR

#define CONFIG_SYS_SDRAM_BASE		PHYS_SDRAM
#define CONFIG_SYS_INIT_RAM_ADDR	IRAM_BASE_ADDR
#define CONFIG_SYS_INIT_RAM_SIZE	IRAM_SIZE

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

/* environment organization */
#define CONFIG_MMCROOT			"/dev/mmcblk1p2"  /* USDHC2 */

#define CONFIG_IMX_THERMAL

#define CONFIG_IOMUX_LPSR

#define CONFIG_SOFT_SPI

#ifdef CONFIG_CMD_NET
#define CONFIG_FEC_ENET_DEV		1
#if (CONFIG_FEC_ENET_DEV == 0)
#define CONFIG_ETHPRIME			"eth0"
#elif (CONFIG_FEC_ENET_DEV == 1)
#define CONFIG_ETHPRIME			"eth1"
#endif
#endif

#define BOOTENV_DEV_LEGACY_MMC(devtypeu, devtypel, instance) \
	"bootcmd_" #devtypel #instance "=" \
	"setenv devtype mmc; " \
	"setenv mmcdev " #instance"; "\
	"setenv bootpart " #instance":1 ; "\
	"run boot\0"

#define BOOTENV_DEV_NAME_LEGACY_MMC(devtypeu, devtypel, instance) \
	#devtypel #instance " "

#define BOOT_TARGET_DEVICES(func) \
	func(LEGACY_MMC, mmc, 0) \
	func(LEGACY_MMC, mmc, 1) \

#include <config_distro_bootcmd.h>

#endif
