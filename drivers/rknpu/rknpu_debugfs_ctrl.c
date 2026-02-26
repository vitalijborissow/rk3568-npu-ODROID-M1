// SPDX-License-Identifier: GPL-2.0
/*
 * RKNPU Debugfs Control Interface
 *
 * Copyright (C) 2026 Vitalij Borissow <250549977+vitalijborissow@users.noreply.github.com>
 *
 * Provides direct control over NPU frequency and voltage bypassing OPP restrictions.
 * Exports: /sys/kernel/debug/rknpu/
 *   - freq_hz          (rw) - Set/get frequency in Hz
 *   - freq_mhz         (rw) - Set/get frequency in MHz  
 *   - voltage_uv       (rw) - Set/get voltage in uV
 *   - voltage_mv       (rw) - Set/get voltage in mV
 *   - clock_source     (ro) - Show clock source being used
 *   - opp_bypass       (rw) - Enable/disable OPP table bypass
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include "rknpu_drv.h"

static struct dentry *rknpu_debugfs_root;
static bool opp_bypass_enabled = true;

/* Frequency control */
static int freq_hz_get(void *data, u64 *val)
{
    struct rknpu_device *rknpu_dev = data;
    struct clk *clk = rknpu_dev->scmi_clk ? rknpu_dev->scmi_clk :
                      (rknpu_dev->num_clks > 0 ? rknpu_dev->clks[0].clk : NULL);
    if (clk)
        *val = clk_get_rate(clk);
    else
        *val = 0;
    return 0;
}

static int freq_hz_set(void *data, u64 val)
{
    struct rknpu_device *rknpu_dev = data;
    struct clk *clk = rknpu_dev->scmi_clk ? rknpu_dev->scmi_clk :
                      (rknpu_dev->num_clks > 0 ? rknpu_dev->clks[0].clk : NULL);
    int ret;
    
    if (clk == NULL)
        return -ENODEV;
    
    ret = clk_set_rate(clk, val);
    if (ret)
        pr_err("rknpu: failed to set freq to %llu Hz: %d\n", val, ret);
    return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(freq_hz_fops, freq_hz_get, freq_hz_set, "%llu\n");

static int freq_mhz_get(void *data, u64 *val)
{
    u64 hz;
    freq_hz_get(data, &hz);
    *val = hz / 1000000;
    return 0;
}

static int freq_mhz_set(void *data, u64 val)
{
    return freq_hz_set(data, val * 1000000);
}
DEFINE_DEBUGFS_ATTRIBUTE(freq_mhz_fops, freq_mhz_get, freq_mhz_set, "%llu\n");

/* Voltage control */
static int voltage_uv_get(void *data, u64 *val)
{
    struct rknpu_device *rknpu_dev = data;
    if (rknpu_dev->vdd && !IS_ERR(rknpu_dev->vdd))
        *val = regulator_get_voltage(rknpu_dev->vdd);
    else
        *val = 0;
    return 0;
}

static int voltage_uv_set(void *data, u64 val)
{
    struct rknpu_device *rknpu_dev = data;
    int ret;
    
    if (rknpu_dev->vdd == NULL || IS_ERR(rknpu_dev->vdd)) {
        pr_err("rknpu: no VDD regulator available\n");
        return -ENODEV;
    }
    
    ret = regulator_set_voltage(rknpu_dev->vdd, val, val + 50000);
    if (ret)
        pr_err("rknpu: failed to set voltage to %llu uV: %d\n", val, ret);
    return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(voltage_uv_fops, voltage_uv_get, voltage_uv_set, "%llu\n");

static int voltage_mv_get(void *data, u64 *val)
{
    u64 uv;
    voltage_uv_get(data, &uv);
    *val = uv / 1000;
    return 0;
}

static int voltage_mv_set(void *data, u64 val)
{
    return voltage_uv_set(data, val * 1000);
}
DEFINE_DEBUGFS_ATTRIBUTE(voltage_mv_fops, voltage_mv_get, voltage_mv_set, "%llu\n");

/* Clock source info */
static int clock_source_show(struct seq_file *s, void *data)
{
    struct rknpu_device *rknpu_dev = s->private;
    int i;
    
    seq_printf(s, "SCMI clock: %s\n", 
               rknpu_dev->scmi_clk ? "available" : "not available");
    
    if (rknpu_dev->scmi_clk)
        seq_printf(s, "  clk_scmi_npu rate: %lu Hz\n", 
                   clk_get_rate(rknpu_dev->scmi_clk));
    
    seq_printf(s, "Device clocks: %d\n", rknpu_dev->num_clks);
    for (i = 0; i < rknpu_dev->num_clks && i < 4; i++) {
        if (rknpu_dev->clks[i].clk)
            seq_printf(s, "  clk[%d] rate: %lu Hz\n", i,
                       clk_get_rate(rknpu_dev->clks[i].clk));
    }
    
    seq_printf(s, "VDD regulator: %s\n",
               (rknpu_dev->vdd && !IS_ERR(rknpu_dev->vdd)) ? "available" : "not available");
    
    if (rknpu_dev->vdd && !IS_ERR(rknpu_dev->vdd))
        seq_printf(s, "  voltage: %d uV\n", 
                   regulator_get_voltage(rknpu_dev->vdd));
    
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(clock_source);

/* OPP bypass control */
static int opp_bypass_get(void *data, u64 *val)
{
    *val = opp_bypass_enabled ? 1 : 0;
    return 0;
}

static int opp_bypass_set(void *data, u64 val)
{
    opp_bypass_enabled = val ? true : false;
    return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(opp_bypass_fops, opp_bypass_get, opp_bypass_set, "%llu\n");

bool rknpu_opp_bypass_enabled(void)
{
    return opp_bypass_enabled;
}
EXPORT_SYMBOL(rknpu_opp_bypass_enabled);

int rknpu_debugfs_ctrl_init(struct rknpu_device *rknpu_dev)
{
    /* Check if debugfs directory already exists (created by rknpu_debugger) */
    rknpu_debugfs_root = debugfs_lookup("rknpu", NULL);
    if (!rknpu_debugfs_root) {
        /* Create new directory if it doesn't exist */
        rknpu_debugfs_root = debugfs_create_dir("rknpu", NULL);
        if (IS_ERR_OR_NULL(rknpu_debugfs_root)) {
            pr_warn("rknpu: failed to create debugfs directory\n");
            return IS_ERR(rknpu_debugfs_root) ? PTR_ERR(rknpu_debugfs_root) : -ENOENT;
        }
    }
    
    debugfs_create_file("freq_hz", 0644, rknpu_debugfs_root, 
                        rknpu_dev, &freq_hz_fops);
    debugfs_create_file("freq_mhz", 0644, rknpu_debugfs_root,
                        rknpu_dev, &freq_mhz_fops);
    debugfs_create_file("voltage_uv", 0644, rknpu_debugfs_root,
                        rknpu_dev, &voltage_uv_fops);
    debugfs_create_file("voltage_mv", 0644, rknpu_debugfs_root,
                        rknpu_dev, &voltage_mv_fops);
    debugfs_create_file("clock_source", 0444, rknpu_debugfs_root,
                        rknpu_dev, &clock_source_fops);
    debugfs_create_file("opp_bypass", 0644, rknpu_debugfs_root,
                        rknpu_dev, &opp_bypass_fops);
    
    return 0;
}

void rknpu_debugfs_ctrl_remove(void)
{
    debugfs_remove_recursive(rknpu_debugfs_root);
    rknpu_debugfs_root = NULL;
}
