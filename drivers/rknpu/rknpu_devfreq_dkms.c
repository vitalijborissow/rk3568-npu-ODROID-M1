// SPDX-License-Identifier: GPL-2.0
/*
 * RKNPU Devfreq Implementation for Kernel 6.18+ (v4 - Full Governor Support)
 *
 * This replaces the Rockchip vendor-specific devfreq code with standard
 * kernel APIs for DKMS compatibility on Armbian/mainline kernels.
 *
 * FEATURES:
 * - Full governor support: ondemand, simple_ondemand, performance, powersave, userspace
 * - Load-based frequency scaling using NPU job busy time tracking
 * - Hybrid clock approach (CRU for ≤600MHz, SCMI for higher)
 * - Proper transition statistics
 *
 * HYBRID CLOCK APPROACH:
 * - CRU clk_npu for ≤600 MHz: Hardware divider based
 * - SCMI clk_scmi_npu for 700+ MHz: Firmware controlled (required for high range)
 *
 * SAFETY: Max frequency capped at 1200 MHz (firmware limit).
 *
 * Copyright (C) 2026 NPU2 Project
 */

/* DKMS misc device support - define early for struct compatibility */
#if defined(RKNPU_DKMS_MISCDEV) && !defined(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP)
#define RKNPU_DKMS_MISCDEV_ENABLED 1
#endif

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/of_clk.h>

#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

#define RKNPU_DEVFREQ_POLLING_MS 50
#define RKNPU_MIN_FREQ      100000000UL   /* 100 MHz */
#define RKNPU_MAX_FREQ     1200000000UL   /* 1200 MHz - Extended for testing */
#define RKNPU_DEFAULT_FREQ  600000000UL   /* 600 MHz */

/* Threshold for CRU vs SCMI clock selection:
 * <= 600 MHz: Use CRU clk_npu (hardware divider)
 * >= 700 MHz: Use SCMI clk_scmi_npu (firmware DVFS)
 */
#define RKNPU_CRU_SCMI_THRESHOLD 600000000UL

/* SCMI clock name for NPU DVFS */
#define RKNPU_SCMI_CLK_NAME "clk_scmi_npu"

/* Voltage table for RK3568 NPU (from OPP table)
 * Extended to 1100 MHz with appropriate voltages
 */
static const struct {
	unsigned long freq;
	unsigned long volt_uv;
} rknpu_volt_table[] = {
	{ 100000000UL,  825000 },
	{ 200000000UL,  825000 },
	{ 300000000UL,  825000 },
	{ 400000000UL,  825000 },
	{ 500000000UL,  825000 },
	{ 600000000UL,  825000 },
	{ 700000000UL,  900000 },
	{ 800000000UL,  950000 },
	{ 900000000UL, 1000000 },
	{ 1000000000UL, 1050000 },  /* 1.0 GHz @ 1050mV */
	{ 1100000000UL, 1100000 },  /* 1.1 GHz @ 1100mV */
	{ 1200000000UL, 1150000 },  /* 1.2 GHz @ 1150mV - EXPERIMENTAL */
};

static unsigned long rknpu_get_voltage_for_freq(unsigned long freq)
{
	int i;
	unsigned long volt = rknpu_volt_table[0].volt_uv;

	for (i = 0; i < ARRAY_SIZE(rknpu_volt_table); i++) {
		if (freq >= rknpu_volt_table[i].freq)
			volt = rknpu_volt_table[i].volt_uv;
	}

	return volt;
}

/* Select appropriate clock based on target frequency
 * CRU for ≤600 MHz (hardware dividers), SCMI for 700+ MHz (firmware DVFS)
 */
static struct clk *rknpu_select_clock(struct rknpu_device *rknpu_dev,
				      unsigned long target_freq,
				      bool *using_scmi)
{
	/* For high frequencies (700+ MHz), must use SCMI */
	if (target_freq > RKNPU_CRU_SCMI_THRESHOLD && rknpu_dev->scmi_clk) {
		*using_scmi = true;
		return rknpu_dev->scmi_clk;
	}

	/* For low frequencies (≤600 MHz), prefer CRU clock */
	if (rknpu_dev->num_clks > 0 && rknpu_dev->clks[0].clk) {
		*using_scmi = false;
		return rknpu_dev->clks[0].clk;
	}

	/* Fallback to SCMI if CRU not available */
	if (rknpu_dev->scmi_clk) {
		*using_scmi = true;
		return rknpu_dev->scmi_clk;
	}

	*using_scmi = false;
	return NULL;
}

static int rknpu_devfreq_target(struct device *dev, unsigned long *freq,
				u32 flags)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	struct clk *target_clk;
	bool using_scmi = false;
	unsigned long target_freq = *freq;
	unsigned long old_actual_freq;
	unsigned long new_actual_freq;
	unsigned long new_volt, old_volt;
	int ret = 0;

	/* SAFETY CAP: Reject frequencies above max to prevent instability */
	if (target_freq > RKNPU_MAX_FREQ) {
		dev_warn(dev, "RKNPU: Requested %lu MHz exceeds safe maximum (%lu MHz), capping\n",
			 target_freq / 1000000, RKNPU_MAX_FREQ / 1000000);
		target_freq = RKNPU_MAX_FREQ;
		*freq = RKNPU_MAX_FREQ;
	}

	/* Choose clock based on target frequency (hybrid approach) */
	target_clk = rknpu_select_clock(rknpu_dev, target_freq, &using_scmi);

	if (!target_clk) {
		dev_err(dev, "no clock available for DVFS\n");
		return -ENODEV;
	}

	/* Get ACTUAL current rate from hardware (not cached value) */
	old_actual_freq = clk_get_rate(target_clk);

	/* Find closest OPP */
	opp = devfreq_recommended_opp(dev, &target_freq, flags);
	if (IS_ERR(opp)) {
		dev_err(dev, "failed to find OPP for %lu Hz\n", *freq);
		return PTR_ERR(opp);
	}

	new_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	/* Apply safety cap again after OPP lookup */
	if (target_freq > RKNPU_MAX_FREQ)
		target_freq = RKNPU_MAX_FREQ;

	old_volt = rknpu_get_voltage_for_freq(old_actual_freq);

	dev_dbg(dev, "DVFS: %lu MHz -> %lu MHz (volt: %lu -> %lu uV) [%s]\n",
		old_actual_freq / 1000000, target_freq / 1000000,
		old_volt, new_volt, using_scmi ? "SCMI" : "CRU");

	/* If scaling up, increase voltage first */
	if (target_freq > old_actual_freq && new_volt > old_volt && rknpu_dev->vdd) {
		ret = regulator_set_voltage(rknpu_dev->vdd, new_volt, new_volt + 50000);
		if (ret) {
			dev_err(dev, "failed to set voltage to %lu uV: %d\n",
				new_volt, ret);
			return ret;
		}
		dev_dbg(dev, "DVFS: voltage raised to %lu uV\n", new_volt);
	}

	/* Set the clock rate via selected clock (CRU or SCMI) */
	ret = clk_set_rate(target_clk, target_freq);
	if (ret) {
		dev_err(dev, "failed to set clock to %lu Hz: %d\n",
			target_freq, ret);
		/* Restore voltage if clock change failed and we raised it */
		if (target_freq > old_actual_freq && new_volt > old_volt && rknpu_dev->vdd)
			regulator_set_voltage(rknpu_dev->vdd, old_volt, old_volt + 50000);
		return ret;
	}

	/* Get ACTUAL frequency achieved from hardware */
	new_actual_freq = clk_get_rate(target_clk);

	/* If scaling down, decrease voltage after clock change */
	if (target_freq < old_actual_freq && new_volt < old_volt && rknpu_dev->vdd) {
		ret = regulator_set_voltage(rknpu_dev->vdd, new_volt, new_volt + 50000);
		if (ret)
			dev_warn(dev, "failed to lower voltage to %lu uV: %d\n",
				 new_volt, ret);
	}

	/* Return OPP target_freq to devfreq (for stats tracking),
	 * but store actual rate internally for our use.
	 * This prevents "Couldn't update frequency transition information"
	 * warnings from devfreq core when SCMI returns slightly different rates.
	 */
	*freq = target_freq;

	/* Only log and update if actual frequency changed */
	if (new_actual_freq != old_actual_freq) {
		rknpu_dev->current_freq = new_actual_freq;
		rknpu_dev->current_volt = new_volt;

		/* Log transition - use info level for visibility during debug */
		dev_info(dev, "RKNPU freq: %lu -> %lu MHz [%s] (requested %lu MHz)\n",
			old_actual_freq / 1000000, new_actual_freq / 1000000,
			using_scmi ? "SCMI" : "CRU", target_freq / 1000000);
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
	struct dev_pm_opp *opp;
	unsigned long freq;
	int ret;
	int i;

	/* DEBUG: Log all available clocks */
	dev_info(dev, "RKNPU DEBUG: num_clks=%d\n", rknpu_dev->num_clks);
	for (i = 0; i < rknpu_dev->num_clks && i < 4; i++) {
		if (rknpu_dev->clks[i].clk) {
			unsigned long rate = clk_get_rate(rknpu_dev->clks[i].clk);
			dev_info(dev, "RKNPU DEBUG: clks[%d] '%s' = %lu Hz\n",
				 i, rknpu_dev->clks[i].id ? rknpu_dev->clks[i].id : "?", rate);
		}
	}

	/* DEBUG: Log regulator status */
	if (rknpu_dev->vdd) {
		int volt = regulator_get_voltage(rknpu_dev->vdd);
		dev_info(dev, "RKNPU DEBUG: vdd_npu regulator = %d uV, enabled=%d\n",
			 volt, regulator_is_enabled(rknpu_dev->vdd));
	} else {
		dev_warn(dev, "RKNPU DEBUG: vdd_npu regulator NOT FOUND!\n");
	}

	/* Try to get SCMI clock for high frequency range (700-1100 MHz)
	 * The SCMI clock bypasses CRU divider limitations and controls
	 * NPU frequency via ARM SCMI firmware protocol.
	 */
	rknpu_dev->scmi_clk = NULL;

	/* Get SCMI clock by DT clock-names entry "scmi_clk" */
	rknpu_dev->scmi_clk = devm_clk_get(dev, "scmi_clk");
	if (IS_ERR(rknpu_dev->scmi_clk)) {
		dev_dbg(dev, "SCMI clock 'scmi_clk' not found (%ld)\n",
			PTR_ERR(rknpu_dev->scmi_clk));
		rknpu_dev->scmi_clk = NULL;
	}

	if (!rknpu_dev->scmi_clk) {
		dev_info(dev, "RKNPU DEBUG: SCMI clock not available -> CRU only mode, max 600MHz\n");
	} else {
		dev_info(dev, "RKNPU DEBUG: SCMI clock found, rate=%lu Hz\n",
			 clk_get_rate(rknpu_dev->scmi_clk));
	}

	/* Initialize OPP table from device tree */
	ret = devm_pm_opp_of_add_table(dev);
	if (ret) {
		dev_warn(dev, "OPP table not found in DT, using defaults: %d\n", ret);
		/* Continue without OPP - will use fixed frequency */
	}

	/* Get initial frequency from CRU clock (preferred for startup) */
	if (rknpu_dev->num_clks > 0 && rknpu_dev->clks[0].clk) {
		rknpu_dev->current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	} else if (rknpu_dev->scmi_clk) {
		rknpu_dev->current_freq = clk_get_rate(rknpu_dev->scmi_clk);
	} else {
		rknpu_dev->current_freq = RKNPU_DEFAULT_FREQ;
	}

	dev_info(dev, "RKNPU: devfreq init %lu MHz\n",
		 rknpu_dev->current_freq / 1000000);

	/* Set initial values in profile */
	rknpu_devfreq_profile.initial_freq = rknpu_dev->current_freq;

	/* Find frequency range from OPP table */
	freq = 0;
	opp = dev_pm_opp_find_freq_ceil(dev, &freq);
	if (!IS_ERR(opp)) {
		rknpu_devfreq_profile.freq_table = NULL;
		dev_pm_opp_put(opp);
	}

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

	dev_info(dev, "RKNPU: devfreq active with simple_ondemand governor\n");

	return 0;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
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
