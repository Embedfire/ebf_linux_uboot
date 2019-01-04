/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017-2018, STMicroelectronics - All Rights Reserved
 *
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Philippe Cornu <philippe.cornu@st.com>
 *
 * This generic Synopsys DesignWare MIPI DSI host include is based on
 * the Linux Kernel include from include/drm/bridge/dw_mipi_dsi.h.
 */

#ifndef __DW_MIPI_DSI__
#define __DW_MIPI_DSI__

#include <mipi_display.h>

struct dw_mipi_dsi_phy_ops {
	int (*init)(void *priv_data);
	int (*get_lane_mbps)(void *priv_data, struct display_timing *timings,
			     u32 lanes, u32 format, unsigned int *lane_mbps);
};

struct dw_mipi_dsi_plat_data {
	unsigned int max_data_lanes;
	const struct dw_mipi_dsi_phy_ops *phy_ops;
	struct udevice *panel;
};

int dw_mipi_dsi_init_bridge(struct mipi_dsi_device *device);
void dw_mipi_dsi_bridge_enable(struct mipi_dsi_device *device);

#endif /* __DW_MIPI_DSI__ */
