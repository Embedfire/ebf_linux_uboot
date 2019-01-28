# SPDX-License-Identifier: GPL-2.0+
#
# Copyright (C) 2018, STMicroelectronics - All Rights Reserved
#

obj-y += cpu.o
obj-y += dram_init.o
obj-y += syscon.o

ifdef CONFIG_SPL_BUILD
obj-y += spl.o
obj-$(CONFIG_STM32MP1_RESET_HALT_WORKAROUND) += stm32mp1_helper_dgb.o
else
obj-$(CONFIG_CMD_STM32PROG) += cmd_stm32prog/
obj-$(CONFIG_CMD_STM32KEY) += cmd_stm32key.o

obj-$(CONFIG_ARMV7_PSCI) += psci.o
obj-y += bsec.o
obj-y += cmd_poweroff.o
endif

obj-$(CONFIG_$(SPL_)DM_REGULATOR) += pwr_regulator.o
obj-$(CONFIG_STM32_ETZPC) += stm32-etzpc.o
