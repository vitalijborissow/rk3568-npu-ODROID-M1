// SPDX-License-Identifier: GPL-2.0
/*
 * RKNPU Devfreq Implementation for Kernel 6.18+ (v3 - Hybrid Clock)
 *
 * This replaces the Rockchip vendor-specific devfreq code with standard
 * kernel APIs for DKMS compatibility on Armbian/mainline kernels.
 *
 * HYBRID CLOCK APPROACH:
 * - CRU clk_npu for ≤600 MHz: Hardware divider based
 * - SCMI clk_scmi_npu for 700+ MHz: Firmware controlled (required for high range)
 *
 * SAFETY: Max frequency capped at 1000 MHz (firmware limit, 1188 MHz caused IOMMU crash).
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
#define RKNPU_MAX_FREQ      600000000UL   /* 600 MHz - CRU aclk_npu max (SCMI doesn't work) */
#define RKNPU_DEFAULT_FREQ  600000000UL   /* 600 MHz */

/* Note: SCMI clock doesn't control actual NPU hardware frequency!
 * We use aclk (CRU) which is limited to ~600 MHz via PLL dividers.
 */
#define RKNPU_CRU_SCMI_THRESHOLD 600000000UL

/* SCMI clock name for NPU DVFS (info only, not used for actual DVFS) */
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
	{ 1000000000UL, 1050000 },  /* 1 GHz @ 1050mV - MAX */
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

/* Clock indices in DTB clock-names: "scmi clk aclk hclk"
 * - clks[0] = scmi (SCMI firmware clock, doesn't affect hardware directly)
 * - clks[1] = clk (clk_npu)
 * - clks[2] = aclk (aclk_npu - THE ACTUAL NPU HARDWARE CLOCK)
 * - clks[3] = hclk (hclk_npu - APB clock)
 */
#define RKNPU_ACLK_INDEX 2  /* aclk_npu - controls actual NPU frequency */

/* Select appropriate clock based on target frequency
 * ALWAYS use aclk (hardware clock) since SCMI doesn't control NPU frequency!
 */
static struct clk *rknpu_select_clock(struct rknpu_device *rknpu_dev,
				      unsigned long target_freq,
				      bool *using_scmi)
{
	/* Use aclk (clks[2]) for hardware frequency control */
	if (rknpu_dev->num_clks > RKNPU_ACLK_INDEX && 
	    rknpu_dev->clks[RKNPU_ACLK_INDEX].clk) {
		*using_scmi = false;
		return rknpu_dev->clks[RKNPU_ACLK_INDEX].clk;
	}

	/* Fallback to clks[0] if aclk not available */
	if (rknpu_dev->num_clks > 0 && rknpu_dev->clks[0].clk) {
		*using_scmi = false;
		return rknpu_dev->clks[0].clk;
	}

	/* Last resort: SCMI if nothing else works */
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

		/* Log transition with actual hardware values */
		dev_info(dev, "RKNPU freq: %lu -> %lu MHz [%s] (requested %lu MHz)\n",
			 old_actual_freq / 1000000, new_actual_freq / 1000000,
			 using_scmi ? "SCMI" : "CRU", *freq / 1000000);
	}

	return 0;
}

static int rknpu_devfreq_get_dev_status(struct device *dev,
					struct devfreq_dev_status *stat)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	stat->current_frequency = rknpu_dev->current_freq;
	stat->busy_time = 0;
	stat->total_time = 1;

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
	struct dev_pm_opp *opp;
	unsigned long freq;
	int ret;

	/* Try to get SCMI clock for high frequency range (700-1100 MHz)
	 * The SCMI clock bypasses CRU divider limitations and controls
	 * NPU frequency via ARM SCMI firmware protocol.
	 */
	rknpu_dev->scmi_clk = NULL;

	/* Method 1: via device clock-names property (preferred) */
	rknpu_dev->scmi_clk = devm_clk_get(dev, "scmi");
	if (IS_ERR(rknpu_dev->scmi_clk)) {
		dev_info(dev, "SCMI clock 'scmi' not in device (%ld)\n",
			 PTR_ERR(rknpu_dev->scmi_clk));
		rknpu_dev->scmi_clk = NULL;

		/* Method 2: via clkdev lookup */
		rknpu_dev->scmi_clk = clk_get_sys(NULL, RKNPU_SCMI_CLK_NAME);
		if (IS_ERR(rknpu_dev->scmi_clk)) {
			rknpu_dev->scmi_clk = NULL;
		}
	}

	if (!rknpu_dev->scmi_clk) {
		dev_info(dev, "SCMI clock not available\n");
	} else {
		unsigned long cur_rate = clk_get_rate(rknpu_dev->scmi_clk);
		dev_info(dev, "RKNPU SCMI clock found: %lu MHz (info only, using aclk for DVFS)\n",
			 cur_rate / 1000000);
	}

	/* Initialize OPP table from device tree */
	ret = devm_pm_opp_of_add_table(dev);
	if (ret) {
		dev_warn(dev, "OPP table not found in DT, using defaults: %d\n", ret);
		/* Continue without OPP - will use fixed frequency */
	}

	/* Get initial frequency from aclk (THE ACTUAL HARDWARE CLOCK) */
	if (rknpu_dev->num_clks > RKNPU_ACLK_INDEX && 
	    rknpu_dev->clks[RKNPU_ACLK_INDEX].clk) {
		rknpu_dev->current_freq = clk_get_rate(rknpu_dev->clks[RKNPU_ACLK_INDEX].clk);
		dev_info(dev, "RKNPU: Using aclk (clks[%d]) for DVFS\n", RKNPU_ACLK_INDEX);
	} else if (rknpu_dev->num_clks > 0 && rknpu_dev->clks[0].clk) {
		rknpu_dev->current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
		dev_warn(dev, "RKNPU: aclk not found, falling back to clks[0]\n");
	} else if (rknpu_dev->scmi_clk) {
		rknpu_dev->current_freq = clk_get_rate(rknpu_dev->scmi_clk);
		dev_warn(dev, "RKNPU: No CRU clocks, using SCMI\n");
	} else {
		rknpu_dev->current_freq = RKNPU_DEFAULT_FREQ;
	}

	dev_info(dev, "RKNPU initial frequency: %lu MHz\n",
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

	/* Register devfreq device with simple_ondemand governor */
	rknpu_dev->devfreq = devm_devfreq_add_device(dev, &rknpu_devfreq_profile,
						     DEVFREQ_GOV_SIMPLE_ONDEMAND,
						     NULL);
	if (IS_ERR(rknpu_dev->devfreq)) {
		ret = PTR_ERR(rknpu_dev->devfreq);
		dev_warn(dev, "devfreq registration failed: %d (continuing without DVFS)\n", ret);
		rknpu_dev->devfreq = NULL;
		return 0; /* Non-fatal */
	}

	dev_info(dev, "RKNPU devfreq registered: /sys/class/devfreq/%s\n",
		 dev_name(dev));

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
