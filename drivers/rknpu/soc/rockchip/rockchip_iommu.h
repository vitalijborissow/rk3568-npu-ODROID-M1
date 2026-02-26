/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub header for DKMS build on non-Rockchip kernels
 * These functions don't exist in vanilla/Armbian kernels
 */
#ifndef __SOC_ROCKCHIP_IOMMU_H
#define __SOC_ROCKCHIP_IOMMU_H

#include <linux/device.h>

/* Stub: always return 0 (IOMMU not enabled in Rockchip sense) */
static inline int rockchip_iommu_is_enabled(struct device *dev)
{
    return 0;
}

#endif /* __SOC_ROCKCHIP_IOMMU_H */
