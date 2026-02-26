/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub header for DKMS build on non-Rockchip kernels
 * rockchip_ipa.h - Intelligent Power Allocation (not in vanilla)
 */
#ifndef __SOC_ROCKCHIP_IPA_H
#define __SOC_ROCKCHIP_IPA_H

struct ipa_power_model_data;

static inline unsigned long
rockchip_ipa_get_static_power(struct ipa_power_model_data *data,
                              unsigned long voltage)
{
    return 0;
}

static inline int rockchip_ipa_power_model_init(struct device *dev,
                                                const char *name)
{
    return -ENOTSUPP;
}

#endif /* __SOC_ROCKCHIP_IPA_H */
