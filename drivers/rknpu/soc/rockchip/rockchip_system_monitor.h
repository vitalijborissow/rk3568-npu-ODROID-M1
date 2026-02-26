/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub header for DKMS build on non-Rockchip kernels
 * rockchip_system_monitor.h - System monitor functions (not in vanilla)
 */
#ifndef __SOC_ROCKCHIP_SYSTEM_MONITOR_H
#define __SOC_ROCKCHIP_SYSTEM_MONITOR_H

struct monitor_dev_info;
struct monitor_dev_profile;

static inline struct monitor_dev_info *
rockchip_system_monitor_register(struct device *dev,
                                 struct monitor_dev_profile *devp)
{
    return NULL;
}

static inline void rockchip_system_monitor_unregister(struct monitor_dev_info *info) {}

#endif /* __SOC_ROCKCHIP_SYSTEM_MONITOR_H */
