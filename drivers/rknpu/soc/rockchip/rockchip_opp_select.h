/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub header for DKMS build on non-Rockchip kernels
 * rockchip_opp_select.h - OPP selection functions (not available in vanilla)
 */
#ifndef __SOC_ROCKCHIP_OPP_SELECT_H
#define __SOC_ROCKCHIP_OPP_SELECT_H

#include <linux/types.h>
#include <linux/mutex.h>

/* Minimal stub structure - only fields that might be referenced */
struct rockchip_opp_info {
    struct mutex lock;
    /* Add more fields as needed for compilation */
};

/* Stub functions - no-op implementations */
static inline int rockchip_init_opp_table(struct device *dev,
                                          struct rockchip_opp_info *info,
                                          const char *clk_name,
                                          const char *reg_name)
{
    return -ENOTSUPP;
}

static inline void rockchip_opp_dvfs_lock(struct rockchip_opp_info *info) {}
static inline void rockchip_opp_dvfs_unlock(struct rockchip_opp_info *info) {}
static inline bool rockchip_opp_is_use_pvtpll(struct rockchip_opp_info *info) { return false; }

#endif /* __SOC_ROCKCHIP_OPP_SELECT_H */
