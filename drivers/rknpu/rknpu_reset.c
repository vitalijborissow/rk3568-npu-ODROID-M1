// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/delay.h>
#include <linux/iommu.h>

#include "rknpu_reset.h"

static inline struct reset_control *rknpu_reset_control_get(struct device *dev,
							    const char *name)
{
	struct reset_control *rst = NULL;

	rst = devm_reset_control_get(dev, name);
	if (IS_ERR(rst))
		LOG_DEV_ERROR(dev,
			      "failed to get rknpu reset control: %s, %ld\n",
			      name, PTR_ERR(rst));

	return rst;
}

int rknpu_reset_get(struct rknpu_device *rknpu_dev)
{
	int i = 0;
	int num_srsts = 0;

	num_srsts = of_count_phandle_with_args(rknpu_dev->dev->of_node,
					       "resets", "#reset-cells");
	if (num_srsts <= 0) {
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to get rknpu resets from dtb\n");
		return num_srsts;
	}

	rknpu_dev->srsts = devm_kcalloc(rknpu_dev->dev, num_srsts,
					sizeof(*rknpu_dev->srsts), GFP_KERNEL);
	if (!rknpu_dev->srsts)
		return -ENOMEM;

	for (i = 0; i < num_srsts; ++i) {
		rknpu_dev->srsts[i] = devm_reset_control_get_exclusive_by_index(
			rknpu_dev->dev, i);
		if (IS_ERR(rknpu_dev->srsts[i])) {
			rknpu_dev->num_srsts = i;
			return PTR_ERR(rknpu_dev->srsts[i]);
		}
	}

	rknpu_dev->num_srsts = num_srsts;

	return num_srsts;
}

static int rknpu_reset_assert(struct reset_control *rst)
{
	int ret = -EINVAL;

	if (!rst)
		return -EINVAL;

	ret = reset_control_assert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to assert rknpu reset: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rknpu_reset_deassert(struct reset_control *rst)
{
	int ret = -EINVAL;

	if (!rst)
		return -EINVAL;

	ret = reset_control_deassert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to deassert rknpu reset: %d\n", ret);
		return ret;
	}

	return 0;
}

int rknpu_soft_reset(struct rknpu_device *rknpu_dev)
{
	struct iommu_domain *domain = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	int ret = 0, i = 0;

	if (rknpu_dev->bypass_soft_reset) {
		LOG_WARN("bypass soft reset\n");
		return 0;
	}

	/* Skip mutex during init - just proceed with reset */
	
	if (!rknpu_dev->config) {
		LOG_DEV_ERROR(rknpu_dev->dev, "RKNPU: config is NULL, skipping soft_reset\n");
		return 0;
	}

	rknpu_dev->soft_reseting = true;

	/* Wait for pending jobs to drain (poll up to 100ms, skip if idle) */
	{
		bool has_pending = false;
		int j;

		for (j = 0; j < rknpu_dev->config->num_irqs; ++j) {
			if (rknpu_dev->subcore_datas[j].job) {
				has_pending = true;
				break;
			}
		}
		if (has_pending) {
			unsigned long deadline = jiffies + msecs_to_jiffies(100);

			while (time_before(jiffies, deadline)) {
				has_pending = false;
				for (j = 0; j < rknpu_dev->config->num_irqs; ++j) {
					if (rknpu_dev->subcore_datas[j].job) {
						has_pending = true;
						break;
					}
				}
				if (!has_pending)
					break;
				usleep_range(1000, 2000);
			}
		}
	}

	for (i = 0; i < rknpu_dev->config->num_irqs; ++i) {
		subcore_data = &rknpu_dev->subcore_datas[i];
		wake_up(&subcore_data->job_done_wq);
	}

	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_assert(rknpu_dev->srsts[i]);

	udelay(10);

	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_deassert(rknpu_dev->srsts[i]);

	udelay(10);

	if (ret) {
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to soft reset for rknpu: %d\n", ret);
		/* Skip mutex unlock - not using mutex */
		return ret;
	}

	if (rknpu_dev->iommu_en)
		domain = iommu_get_domain_for_dev(rknpu_dev->dev);

	if (domain) {
		iommu_detach_device(domain, rknpu_dev->dev);
		iommu_attach_device(domain, rknpu_dev->dev);
	}

	rknpu_dev->soft_reseting = false;

	if (rknpu_dev->config->state_init != NULL)
		rknpu_dev->config->state_init(rknpu_dev);

	/* Skip mutex unlock - not using mutex */

	return 0;
}
