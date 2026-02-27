// SPDX-License-Identifier: GPL-2.0
/*
 * RKNPU Devfreq Implementation for Kernel 6.18+ (v5 - SCMI-only)
 *
 * This replaces the Rockchip vendor-specific devfreq code with standard
 * kernel APIs for DKMS compatibility on Armbian/mainline kernels.
 *
 * FEATURES:
 * - Full governor support: simple_ondemand, performance, powersave, userspace
 * - Load-based frequency scaling using NPU power reference count
 * - SCMI-only clocking (all frequencies via ARM SCMI firmware)
 * - Proper transition statistics and thermal throttling
 *
 * SCMI provides: 198, 297, 396, 594, 600, 700, 800, 900, 1000 MHz
 * Safe max: 1000 MHz (1100+ MHz crashes or misreports)
 *
 * Copyright (C) 2026 NPU2 Project
 */

/* DKMS misc device support - define early for struct compatibility */
#if defined(RKNPU_DKMS_MISCDEV) && !defined(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP)
#define RKNPU_DKMS_MISCDEV_ENABLED 1
#endif

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/of_clk.h>

#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

#define RKNPU_DEVFREQ_POLLING_MS 50
#define RKNPU_MAX_FREQ     1000000000UL   /* 1000 MHz - safe SCMI max */
#define RKNPU_DEFAULT_FREQ  600000000UL   /* 600 MHz */

static int rknpu_devfreq_target(struct device *dev, unsigned long *freq,
				u32 flags)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long target_freq = *freq;
	unsigned long old_freq, new_freq, new_volt;
	int ret;

	if (!rknpu_dev->scmi_clk) {
		dev_err(dev, "no SCMI clock for DVFS\n");
		return -ENODEV;
	}

	if (target_freq > RKNPU_MAX_FREQ)
		target_freq = RKNPU_MAX_FREQ;

	old_freq = clk_get_rate(rknpu_dev->scmi_clk);

	opp = devfreq_recommended_opp(dev, &target_freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	new_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	/* Scale voltage up before freq increase */
	if (target_freq > old_freq && rknpu_dev->vdd) {
		ret = regulator_set_voltage(rknpu_dev->vdd, new_volt, new_volt + 50000);
		if (ret) {
			dev_err(dev, "voltage set %lu uV failed: %d\n", new_volt, ret);
			return ret;
		}
	}

	ret = clk_set_rate(rknpu_dev->scmi_clk, target_freq);
	if (ret) {
		dev_err(dev, "SCMI clk_set_rate %lu Hz failed: %d\n", target_freq, ret);
		return ret;
	}

	new_freq = clk_get_rate(rknpu_dev->scmi_clk);

	/* Scale voltage down after freq decrease */
	if (target_freq < old_freq && rknpu_dev->vdd)
		regulator_set_voltage(rknpu_dev->vdd, new_volt, new_volt + 50000);

	*freq = target_freq;

	if (new_freq != old_freq) {
		rknpu_dev->current_freq = new_freq;
		rknpu_dev->current_volt = new_volt;
		dev_dbg(dev, "RKNPU freq: %lu -> %lu MHz\n",
			old_freq / 1000000, new_freq / 1000000);
	}

	return 0;
}

static int rknpu_devfreq_get_dev_status(struct device *dev,
					struct devfreq_dev_status *stat)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	int power_refs;

	stat->current_frequency = rknpu_dev->current_freq;

	/* Use power reference count as load indicator
	 * When NPU is powered on (power_refcount > 0), it's processing jobs.
	 * This is more reliable than busy_time since the hrtimer may not
	 * sample fast enough between quick power cycles.
	 */
	power_refs = atomic_read(&rknpu_dev->power_refcount);

	if (power_refs > 0) {
		/* NPU is active - report high load to scale up */
		stat->total_time = 100;
		stat->busy_time = 95;  /* 95% load - will trigger scale-up */
	} else {
		/* NPU is idle - report low load to scale down */
		stat->total_time = 100;
		stat->busy_time = 0;
	}

	return 0;
}

static int rknpu_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	*freq = rknpu_dev->current_freq;

	return 0;
}

static struct devfreq_dev_profile rknpu_devfreq_profile = {
	.polling_ms = RKNPU_DEVFREQ_POLLING_MS,
	.target = rknpu_devfreq_target,
	.get_dev_status = rknpu_devfreq_get_dev_status,
	.get_cur_freq = rknpu_devfreq_get_cur_freq,
};

void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq)
		mutex_lock(&rknpu_dev->devfreq->lock);
}

void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq)
		mutex_unlock(&rknpu_dev->devfreq->lock);
}

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	struct devfreq_simple_ondemand_data *ondemand_data;
	int ret;

	/* Get SCMI clock (sole clock source for all NPU frequencies) */
	rknpu_dev->scmi_clk = devm_clk_get(dev, "scmi_clk");
	if (IS_ERR(rknpu_dev->scmi_clk)) {
		dev_warn(dev, "SCMI clock not found, devfreq disabled\n");
		rknpu_dev->scmi_clk = NULL;
		return 0;
	}

	/* Initialize OPP table from device tree */
	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		dev_warn(dev, "OPP table not found in DT: %d\n", ret);

	rknpu_dev->current_freq = clk_get_rate(rknpu_dev->scmi_clk);
	dev_info(dev, "RKNPU: SCMI clock %lu MHz\n",
		 rknpu_dev->current_freq / 1000000);

	rknpu_devfreq_profile.initial_freq = rknpu_dev->current_freq;

	/* Allocate and configure simple_ondemand governor data
	 * upthreshold: Scale up when load exceeds this (default 90%)
	 * downdifferential: Scale down when load drops below upthreshold - this (default 5%)
	 */
	ondemand_data = devm_kzalloc(dev, sizeof(*ondemand_data), GFP_KERNEL);
	if (ondemand_data) {
		ondemand_data->upthreshold = 70;       /* Scale up at 70% load */
		ondemand_data->downdifferential = 20;  /* Scale down at 50% load */
	}

	/* Register devfreq device with simple_ondemand governor
	 * Governors available: simple_ondemand, performance, powersave, userspace
	 */
	rknpu_dev->devfreq = devm_devfreq_add_device(dev, &rknpu_devfreq_profile,
						     DEVFREQ_GOV_SIMPLE_ONDEMAND,
						     ondemand_data);
	if (IS_ERR(rknpu_dev->devfreq)) {
		ret = PTR_ERR(rknpu_dev->devfreq);
		dev_warn(dev, "devfreq registration failed: %d (continuing without DVFS)\n", ret);
		rknpu_dev->devfreq = NULL;
		return 0; /* Non-fatal */
	}

	/* Initialize transition stats - set previous_freq to avoid warnings */
	rknpu_dev->devfreq->previous_freq = rknpu_dev->current_freq;
	rknpu_dev->devfreq->last_status.current_frequency = rknpu_dev->current_freq;
	rknpu_dev->devfreq->last_status.total_time = 1;
	rknpu_dev->devfreq->last_status.busy_time = 0;

	dev_info(dev, "RKNPU: devfreq active (SCMI-only, OPP 200-1000 MHz)\n");

	/* Register devfreq-cooling so thermal framework can throttle NPU */
	rknpu_dev->devfreq_cooling =
		of_devfreq_cooling_register(dev->of_node, rknpu_dev->devfreq);
	if (IS_ERR(rknpu_dev->devfreq_cooling)) {
		dev_dbg(dev, "devfreq-cooling not registered: %ld\n",
			PTR_ERR(rknpu_dev->devfreq_cooling));
		rknpu_dev->devfreq_cooling = NULL;
	} else {
		dev_info(dev, "RKNPU: thermal throttling enabled\n");
	}

	return 0;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq_cooling) {
		devfreq_cooling_unregister(rknpu_dev->devfreq_cooling);
		rknpu_dev->devfreq_cooling = NULL;
	}
	/* devm handles clock and devfreq cleanup */
	rknpu_dev->scmi_clk = NULL;
	rknpu_dev->devfreq = NULL;
}

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	return 0;
}

int rknpu_devfreq_runtime_resume(struct device *dev)
{
	return 0;
}

/* Sysfs interface for manual frequency control (for testing) */
static ssize_t rknpu_freq_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	return sprintf(buf, "%lu\n", rknpu_dev->current_freq);
}

static ssize_t rknpu_freq_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long freq;
	u32 flags = 0;
	int ret;

	ret = kstrtoul(buf, 10, &freq);
	if (ret)
		return ret;

	/* Apply safety cap before processing */
	if (freq > RKNPU_MAX_FREQ) {
		dev_warn(dev, "RKNPU: Requested %lu MHz exceeds safe max %lu MHz, capping\n",
			 freq / 1000000, RKNPU_MAX_FREQ / 1000000);
		freq = RKNPU_MAX_FREQ;
	}

	ret = rknpu_devfreq_target(dev, &freq, flags);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR(rknpu_freq, 0644, rknpu_freq_show, rknpu_freq_store);

int rknpu_devfreq_create_sysfs(struct rknpu_device *rknpu_dev)
{
	return device_create_file(rknpu_dev->dev, &dev_attr_rknpu_freq);
}

void rknpu_devfreq_remove_sysfs(struct rknpu_device *rknpu_dev)
{
	device_remove_file(rknpu_dev->dev, &dev_attr_rknpu_freq);
}
