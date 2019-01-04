/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#ifndef _RAM_STM32MP1_TUNING_H_
#define _RAM_STM32MP1_TUNING_H_

#define MAX_DQS_PHASE_IDX _144deg
#define MAX_DQS_UNIT_IDX 7
#define MAX_GSL_IDX 5
#define MAX_GPS_IDX 3

/* Number of bytes used in this SW. ( min 1--> max 4). */
#define NUM_BYTES 4

enum dqs_phase_enum {
	_36deg = 0,
	_54deg = 1,
	_72deg = 2,
	_90deg = 3,
	_108deg = 4,
	_126deg = 5,
	_144deg = 6
};

/* BIST Result struct */
struct BIST_result {
	/* Overall test result:
	 * 0 Fail (any bit failed) ,
	 * 1 Success (All bits success)
	 */
	bool test_result;
	/* 1: true, all fail /  0: False, not all bits fail */
	bool all_bits_fail;
	bool bit_i_test_result[8];  /* 0 fail / 1 success */
};

u8 DQS_phase_index(struct stm32mp1_ddrphy *phy, u8 byte);
u8 DQS_unit_index(struct stm32mp1_ddrphy *phy, u8 byte);
u8 DQ_unit_index(struct stm32mp1_ddrphy *phy, u8 byte, u8 bit);

void apply_deskew_results(struct stm32mp1_ddrphy *phy, u8 byte);
void init_result_struct(struct BIST_result *result);
void BIST_test(struct stm32mp1_ddrphy *phy, u8 byte,
	       struct BIST_result *result);

/* a struct that defines tuning parameters of a byte. */
struct tuning_position {
	u8 phase; /* DQS phase */
	u8 unit; /* DQS unit delay */
	u32 bits_delay; /* Bits deskew in this byte */
};
#endif
