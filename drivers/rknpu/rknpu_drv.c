// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mm.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/of_address.h>

#include <soc/rockchip/rockchip_iommu.h>

#include "rknpu_ioctl.h"
#include "rknpu_reset.h"
#include "rknpu_fence.h"
#include "rknpu_drv.h"
#include "rknpu_gem.h"
#include "rknpu_devfreq.h"
#include "rknpu_iommu.h"
#include "rknpu_debugfs_ctrl.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
#include <drm/drm_device.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include "rknpu_gem.h"
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
#include <linux/rk-dma-heap.h>
#include "rknpu_mem.h"

#endif

/* DKMS misc device support - works without rk-dma-heap */
#ifdef RKNPU_DKMS_MISCDEV_ENABLED
#include <linux/miscdevice.h>
#include "rknpu_mem.h"
#endif

/* Stub for missing Rockchip nvmem function */
static inline int rockchip_nvmem_cell_read_u8(struct device_node *np, const char *cell_id, u8 *val)
{
    *val = 0;
    return 0;
}

#define POWER_DOWN_FREQ 200000000
#define NPU_MMU_DISABLED_POLL_PERIOD_US 1000
#define NPU_MMU_DISABLED_POLL_TIMEOUT_US 20000

static int bypass_irq_handler;
module_param(bypass_irq_handler, int, 0644);
MODULE_PARM_DESC(bypass_irq_handler,
		 "bypass RKNPU irq handler if set it to 1, disabled by default");

static int bypass_soft_reset;
module_param(bypass_soft_reset, int, 0644);
MODULE_PARM_DESC(bypass_soft_reset,
		 "bypass RKNPU soft reset if set it to 1, disabled by default");

static unsigned long power_put_delay_ms = 500;
module_param(power_put_delay_ms, ulong, 0644);
MODULE_PARM_DESC(power_put_delay_ms,
		 "delay in ms before powering off NPU after last use (default 500)");

unsigned int max_freq_mhz = 1000;
module_param(max_freq_mhz, uint, 0644);
MODULE_PARM_DESC(max_freq_mhz,
		 "maximum NPU frequency in MHz (default 1000)");

static const struct rknpu_irqs_data rknpu_irqs[] = {
	{ "npu_irq", rknpu_core0_irq_handler }
};

static const struct rknpu_amount_data rknpu_old_top_amount = {
	.offset_clr_all = 0x8010,
	.offset_dt_wr = 0x8034,
	.offset_dt_rd = 0x8038,
	.offset_wt_rd = 0x803c,
};

static void rknpu_state_init(struct rknpu_device *rknpu_dev)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];

	dev_dbg(rknpu_dev->dev, "RKNPU state_init: writing init sequence to base %px", rknpu_core_base);

	writel(0x1, rknpu_core_base + 0x10);
	writel(0, rknpu_core_base + 0x1004);
	writel(0x80000000, rknpu_core_base + 0x1024);
	writel(1, rknpu_core_base + 0x1004);
	writel(0x80000000, rknpu_core_base + 0x1024);
	writel(0x1e, rknpu_core_base + 0x1004);
}

static const struct rknpu_config rk356x_rknpu_config = {
	.bw_priority_addr = 0xfe180008,
	.bw_priority_length = 0x10,
	.dma_mask = DMA_BIT_MASK(32),
	.pc_data_amount_scale = 1,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.pc_dma_ctrl = 0,
	.irqs = rknpu_irqs,
	.num_irqs = ARRAY_SIZE(rknpu_irqs),
	.nbuf_phyaddr = 0,
	.nbuf_size = 0,
	.max_submit_number = (1 << 12) - 1,
	.core_mask = 0x1,
	.amount_top = &rknpu_old_top_amount,
	.amount_core = NULL,
	.state_init = rknpu_state_init,
	.cache_sgt_init = NULL,
};

/* driver probe and init */
static const struct of_device_id rknpu_of_match[] = {
	{
		.compatible = "rockchip,rk3568-rknpu",
		.data = &rk356x_rknpu_config,
	},
	{},
};

MODULE_DEVICE_TABLE(of, rknpu_of_match);

static int rknpu_get_drv_version(uint32_t *version)
{
	*version = RKNPU_GET_DRV_VERSION_CODE(DRIVER_MAJOR, DRIVER_MINOR,
					      DRIVER_PATCHLEVEL);
	return 0;
}

static int rknpu_power_on(struct rknpu_device *rknpu_dev);
static int rknpu_power_off(struct rknpu_device *rknpu_dev);

static void rknpu_power_off_delay_work(struct work_struct *power_off_work)
{
	int ret = 0;
	struct rknpu_device *rknpu_dev =
		container_of(to_delayed_work(power_off_work),
			     struct rknpu_device, power_off_work);
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);
}

int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_inc_return(&rknpu_dev->power_refcount) == 1) {
		ret = rknpu_power_on(rknpu_dev);
		if (!ret) {
			dev_dbg(rknpu_dev->dev, "RKNPU: power on (0->1)\n");
			rknpu_dev->devfreq_last_busy = ktime_get();
		}
	}
	mutex_unlock(&rknpu_dev->power_lock);

	return ret;
}

int rknpu_power_put(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ktime_t now = ktime_get();
		s64 busy_ns = ktime_to_ns(ktime_sub(now, rknpu_dev->devfreq_last_busy));

		if (busy_ns > 0)
			rknpu_dev->devfreq_busy_ns += busy_ns;

		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
		else
			dev_dbg(rknpu_dev->dev, "RKNPU: power off (1->0)\n");
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);

	return ret;
}

int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->power_put_delay == 0)
		return rknpu_power_put(rknpu_dev);

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) == 1)
		queue_delayed_work(
			rknpu_dev->power_off_wq, &rknpu_dev->power_off_work,
			msecs_to_jiffies(rknpu_dev->power_put_delay));
	else
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);

	return 0;
}

static int rknpu_action(struct rknpu_device *rknpu_dev,
			struct rknpu_action *args)
{
	int ret = -EINVAL;
	int need_power = 0;

	/* Operations that access hardware registers need power */
	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
	case RKNPU_ACT_RESET:
	case RKNPU_GET_BW_PRIORITY:
	case RKNPU_SET_BW_PRIORITY:
	case RKNPU_GET_BW_EXPECT:
	case RKNPU_SET_BW_EXPECT:
	case RKNPU_GET_BW_TW:
	case RKNPU_SET_BW_TW:
	case RKNPU_GET_TOTAL_RW_AMOUNT:
		need_power = 1;
		break;
	default:
		break;
	}

	if (need_power) {
		ret = rknpu_power_get(rknpu_dev);
		if (ret) {
			pr_err("RKNPU: failed to power on for action %d\n", args->flags);
			return ret;
		}
	}

	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
		ret = rknpu_get_hw_version(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_DRV_VERSION:
		ret = rknpu_get_drv_version(&args->value);
		break;
	case RKNPU_GET_FREQ:
		args->value = clk_get_rate(rknpu_dev->clks[0].clk);
		ret = 0;
		break;
	case RKNPU_SET_FREQ:
		break;
	case RKNPU_GET_VOLT:
		args->value = regulator_get_voltage(rknpu_dev->vdd);
		ret = 0;
		break;
	case RKNPU_SET_VOLT:
		break;
	case RKNPU_ACT_RESET:
		ret = rknpu_soft_reset(rknpu_dev);
		break;
	case RKNPU_GET_BW_PRIORITY:
		ret = rknpu_get_bw_priority(rknpu_dev, &args->value, NULL,
					    NULL);
		break;
	case RKNPU_SET_BW_PRIORITY:
		ret = rknpu_set_bw_priority(rknpu_dev, args->value, 0, 0);
		break;
	case RKNPU_GET_BW_EXPECT:
		ret = rknpu_get_bw_priority(rknpu_dev, NULL, &args->value,
					    NULL);
		break;
	case RKNPU_SET_BW_EXPECT:
		ret = rknpu_set_bw_priority(rknpu_dev, 0, args->value, 0);
		break;
	case RKNPU_GET_BW_TW:
		ret = rknpu_get_bw_priority(rknpu_dev, NULL, NULL,
					    &args->value);
		break;
	case RKNPU_SET_BW_TW:
		ret = rknpu_set_bw_priority(rknpu_dev, 0, 0, args->value);
		break;
	case RKNPU_ACT_CLR_TOTAL_RW_AMOUNT:
		ret = rknpu_clear_rw_amount(rknpu_dev);
		break;
	case RKNPU_GET_DT_WR_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, &args->value, NULL, NULL);
		break;
	case RKNPU_GET_DT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, &args->value, NULL);
		break;
	case RKNPU_GET_WT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, NULL, &args->value);
		break;
	case RKNPU_GET_TOTAL_RW_AMOUNT:
		ret = rknpu_get_total_rw_amount(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_IOMMU_EN:
		args->value = rknpu_dev->iommu_en;
		ret = 0;
		break;
	case RKNPU_SET_PROC_NICE:
		set_user_nice(current, *(int32_t *)&args->value);
		ret = 0;
		break;
	case RKNPU_GET_TOTAL_SRAM_SIZE:
		if (rknpu_dev->sram_mm)
			args->value = rknpu_dev->sram_mm->total_chunks *
				      rknpu_dev->sram_mm->chunk_size;
		else
			args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_FREE_SRAM_SIZE:
		if (rknpu_dev->sram_mm)
			args->value = rknpu_dev->sram_mm->free_chunks *
				      rknpu_dev->sram_mm->chunk_size;
		else
			args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_IOMMU_DOMAIN_ID:
		args->value = rknpu_dev->iommu_domain_id;
		ret = 0;
		break;
	case RKNPU_SET_IOMMU_DOMAIN_ID: {
		ret = rknpu_iommu_domain_get_and_switch(
			rknpu_dev, *(int32_t *)&args->value);
		if (ret)
			break;
		rknpu_iommu_domain_put(rknpu_dev);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	if (need_power)
		rknpu_power_put(rknpu_dev);

	return ret;
}

#if defined(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP) || defined(RKNPU_DKMS_MISCDEV_ENABLED)
static int rknpu_open(struct inode *inode, struct file *file)
{
	struct rknpu_device *rknpu_dev =
		container_of(file->private_data, struct rknpu_device, miscdev);
	struct rknpu_session *session = NULL;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		LOG_ERROR("rknpu session alloc failed\n");
		return -ENOMEM;
	}

	session->rknpu_dev = rknpu_dev;
	INIT_LIST_HEAD(&session->list);

	file->private_data = (void *)session;

	return nonseekable_open(inode, file);
}

static int rknpu_release(struct inode *inode, struct file *file)
{
	struct rknpu_mem_object *entry;
	struct rknpu_session *session = file->private_data;
	struct rknpu_device *rknpu_dev = session->rknpu_dev;
	LIST_HEAD(local_list);

	spin_lock(&rknpu_dev->lock);
	list_replace_init(&session->list, &local_list);
	file->private_data = NULL;
	spin_unlock(&rknpu_dev->lock);

	while (!list_empty(&local_list)) {
		entry = list_first_entry(&local_list, struct rknpu_mem_object,
					 head);

		LOG_DEBUG(
			"Fd close free rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
			(__u64)(uintptr_t)entry, (__u64)entry->dma_addr);

		if (entry->kv_addr) {
			struct iosys_map map =
				IOSYS_MAP_INIT_VADDR(entry->kv_addr);
			dma_buf_vunmap(entry->dmabuf, &map);
			entry->kv_addr = NULL;
		}

		dma_buf_unmap_attachment(entry->attachment, entry->sgt,
					 DMA_BIDIRECTIONAL);
		dma_buf_detach(entry->dmabuf, entry->attachment);

		if (!entry->owner)
			dma_buf_put(entry->dmabuf);

		list_del(&entry->head);
		kfree(entry);
	}

	kfree(session);

	return 0;
}

static int rknpu_miscdev_action_ioctl(struct rknpu_device *rknpu_dev,
			      unsigned long data)
{
	struct rknpu_action args;
	int ret = -EINVAL;

	if (unlikely(copy_from_user(&args, (struct rknpu_action *)data,
				    sizeof(struct rknpu_action)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	ret = rknpu_action(rknpu_dev, &args);

	if (unlikely(copy_to_user((struct rknpu_action *)data, &args,
				  sizeof(struct rknpu_action)))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	return ret;
}

static long rknpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -ENOTTY;
	struct rknpu_device *rknpu_dev = NULL;

	if (!file->private_data)
		return -EINVAL;

	rknpu_dev = ((struct rknpu_session *)file->private_data)->rknpu_dev;

	rknpu_power_get(rknpu_dev);

	switch (_IOC_NR(cmd)) {
	case RKNPU_ACTION:
		ret = rknpu_miscdev_action_ioctl(rknpu_dev, arg);
		break;
	case RKNPU_SUBMIT:
		ret = rknpu_miscdev_submit_ioctl(rknpu_dev, arg);
		break;
	case RKNPU_MEM_CREATE:
		ret = rknpu_mem_create_ioctl(rknpu_dev, file, cmd, arg);
		break;
	case RKNPU_MEM_MAP:
		break;
	case RKNPU_MEM_DESTROY:
		ret = rknpu_mem_destroy_ioctl(rknpu_dev, file, arg);
		break;
	case RKNPU_MEM_SYNC:
		ret = rknpu_mem_sync_ioctl(rknpu_dev, file, arg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	rknpu_power_put_delay(rknpu_dev);

	return ret;
}
const struct file_operations rknpu_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_open,
	.release = rknpu_release,
	.unlocked_ioctl = rknpu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = rknpu_ioctl,
#endif
};
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM

static int rknpu_action_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev->dev);

	return rknpu_action(rknpu_dev, (struct rknpu_action *)data);
}

#define RKNPU_IOCTL(func)                                                   \
	static int __##func(struct drm_device *dev, void *data,             \
			    struct drm_file *file_priv)                     \
	{                                                                   \
		struct rknpu_device *rknpu_dev = dev_get_drvdata(dev->dev); \
		int ret = -EINVAL;                                          \
		rknpu_power_get(rknpu_dev);                                 \
		ret = func(dev, data, file_priv);                           \
		rknpu_power_put_delay(rknpu_dev);                           \
		return ret;                                                 \
	}

RKNPU_IOCTL(rknpu_action_ioctl);
RKNPU_IOCTL(rknpu_submit_ioctl);
RKNPU_IOCTL(rknpu_gem_create_ioctl);
RKNPU_IOCTL(rknpu_gem_map_ioctl);
RKNPU_IOCTL(rknpu_gem_destroy_ioctl);
RKNPU_IOCTL(rknpu_gem_sync_ioctl);

static const struct drm_ioctl_desc rknpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(RKNPU_ACTION, __rknpu_action_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT, __rknpu_submit_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_CREATE, __rknpu_gem_create_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_MAP, __rknpu_gem_map_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_DESTROY, __rknpu_gem_destroy_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_SYNC, __rknpu_gem_sync_ioctl,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(rknpu_drm_driver_fops);

static struct drm_driver rknpu_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.dumb_create = rknpu_gem_dumb_create,
	.dumb_map_offset = drm_gem_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = rknpu_gem_prime_import,
	.gem_prime_import_sg_table = rknpu_gem_prime_import_sg_table,
	.ioctls = rknpu_ioctls,
	.num_ioctls = ARRAY_SIZE(rknpu_ioctls),
	.fops = &rknpu_drm_driver_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

#endif

static enum hrtimer_restart hrtimer_handler(struct hrtimer *timer)
{
	struct rknpu_device *rknpu_dev =
		container_of(timer, struct rknpu_device, timer);
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	ktime_t now;
	unsigned long flags;
	int i;

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		subcore_data = &rknpu_dev->subcore_datas[i];

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

		job = subcore_data->job;
		if (job) {
			now = ktime_get();
			subcore_data->timer.busy_time +=
				ktime_sub(now, job->hw_recoder_time);
			job->hw_recoder_time = now;
		}

		subcore_data->timer.total_busy_time =
			subcore_data->timer.busy_time;
		subcore_data->timer.busy_time = 0;

		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	}

	hrtimer_forward_now(timer, rknpu_dev->kt);
	return HRTIMER_RESTART;
}

static void rknpu_init_timer(struct rknpu_device *rknpu_dev)
{
	rknpu_dev->kt = ktime_set(0, RKNPU_LOAD_INTERVAL);
	hrtimer_setup(&rknpu_dev->timer, hrtimer_handler, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&rknpu_dev->timer, rknpu_dev->kt, HRTIMER_MODE_REL);
}

static void rknpu_cancel_timer(struct rknpu_device *rknpu_dev)
{
	hrtimer_cancel(&rknpu_dev->timer);
}

static bool rknpu_is_iommu_enable(struct device *dev)
{
	struct device_node *iommu = NULL;

	iommu = of_parse_phandle(dev->of_node, "iommus", 0);
	if (!iommu) {
		dev_dbg(dev,
			"rknpu iommu device-tree entry not found, using non-iommu mode\n");
		return false;
	}

	if (!of_device_is_available(iommu)) {
		dev_dbg(dev,
			"rknpu iommu is disabled, using non-iommu mode\n");
		of_node_put(iommu);
		return false;
	}
	of_node_put(iommu);

	return true;
}

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
static int drm_fake_dev_register(struct rknpu_device *rknpu_dev)
{
	const struct platform_device_info rknpu_dev_info = {
		.name = "rknpu_dev",
		.id = PLATFORM_DEVID_AUTO,
		.dma_mask = rknpu_dev->config->dma_mask,
	};
	struct platform_device *pdev = NULL;
	int ret = -EINVAL;

	pdev = platform_device_register_full(&rknpu_dev_info);
	if (pdev) {
		ret = of_dma_configure(&pdev->dev, NULL, true);
		if (ret) {
			platform_device_unregister(pdev);
			pdev = NULL;
		}
	}

	rknpu_dev->fake_dev = pdev ? &pdev->dev : NULL;

	return ret;
}

static void drm_fake_dev_unregister(struct rknpu_device *rknpu_dev)
{
	struct platform_device *pdev = NULL;

	if (!rknpu_dev->fake_dev)
		return;

	pdev = to_platform_device(rknpu_dev->fake_dev);

	platform_device_unregister(pdev);
}

static int rknpu_drm_probe(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	struct drm_device *drm_dev = NULL;
	int ret = -EINVAL;

	drm_dev = drm_dev_alloc(&rknpu_drm_driver, dev);
	if (IS_ERR(drm_dev))
		return PTR_ERR(drm_dev);

	/* register the DRM device */
	ret = drm_dev_register(drm_dev, 0);
	if (ret < 0)
		goto err_free_drm;

	drm_dev->dev_private = rknpu_dev;
	rknpu_dev->drm_dev = drm_dev;

	drm_fake_dev_register(rknpu_dev);

	return 0;

err_free_drm:
	drm_dev_put(drm_dev);

	return ret;
}

static void rknpu_drm_remove(struct rknpu_device *rknpu_dev)
{
	struct drm_device *drm_dev = rknpu_dev->drm_dev;

	drm_fake_dev_unregister(rknpu_dev);

	drm_dev_unregister(drm_dev);

	drm_dev_put(drm_dev);
}
#endif

static int rknpu_power_on(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	int ret = -EINVAL;

	if (rknpu_dev->vdd) {
		ret = regulator_enable(rknpu_dev->vdd);
		if (ret) {
			dev_err(dev, "failed to enable vdd regulator: %d\n", ret);
			return ret;
		}
	}

	if (rknpu_dev->mem) {
		ret = regulator_enable(rknpu_dev->mem);
		if (ret) {
			dev_err(dev, "failed to enable mem regulator: %d\n", ret);
			if (rknpu_dev->vdd)
				regulator_disable(rknpu_dev->vdd);
			return ret;
		}
	}

	ret = clk_bulk_prepare_enable(rknpu_dev->num_clks, rknpu_dev->clks);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to enable clk for rknpu, ret: %d\n",
			      ret);
		return ret;
	}

	/* Clock rates logged at dev_dbg level to avoid spam */
	dev_dbg(dev, "RKNPU: clocks enabled (%d clks)\n", rknpu_dev->num_clks);

	rknpu_devfreq_lock(rknpu_dev);

	if (rknpu_dev->multiple_domains) {
		if (rknpu_dev->genpd_dev_npu0) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu0);
			if (ret < 0) {
				LOG_DEV_ERROR(
					dev,
					"failed to get pm runtime for npu0, ret: %d\n",
					ret);
				goto out;
			}
		}
		if (rknpu_dev->genpd_dev_npu1) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu1);
			if (ret < 0) {
				LOG_DEV_ERROR(
					dev,
					"failed to get pm runtime for npu1, ret: %d\n",
					ret);
				goto out;
			}
		}
		if (rknpu_dev->genpd_dev_npu2) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu2);
			if (ret < 0) {
				LOG_DEV_ERROR(
					dev,
					"failed to get pm runtime for npu2, ret: %d\n",
					ret);
				goto out;
			}
		}
	}
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		LOG_DEV_ERROR(dev,
			      "failed to get pm runtime for rknpu, ret: %d\n",
			      ret);
	}

	if (rknpu_dev->config && rknpu_dev->config->state_init != NULL)
		rknpu_dev->config->state_init(rknpu_dev);

out:
	rknpu_devfreq_unlock(rknpu_dev);

	return ret;
}

static int rknpu_power_off(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	int ret;
	bool val;

	rknpu_devfreq_lock(rknpu_dev);

	pm_runtime_put_sync(dev);

	if (rknpu_dev->multiple_domains) {
		/*
		 * Because IOMMU's runtime suspend callback is asynchronous,
		 * it may be executed after the NPU is turned off after PD/CLK/VD,
		 * and the runtime suspend callback has a register access.
		 * If the PD/VD/CLK is closed, the register access will crash.
		 * As a workaround, it's safe to close pd stuff until iommu disabled.
		 */
		ret = readx_poll_timeout(rockchip_iommu_is_enabled, dev, val,
					 !val, NPU_MMU_DISABLED_POLL_PERIOD_US,
					 NPU_MMU_DISABLED_POLL_TIMEOUT_US);
		if (ret) {
			LOG_DEV_ERROR(dev, "iommu still enabled\n");
			pm_runtime_get_sync(dev);
			rknpu_devfreq_unlock(rknpu_dev);
			return ret;
		}
		if (rknpu_dev->genpd_dev_npu2)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu2);
		if (rknpu_dev->genpd_dev_npu1)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu1);
		if (rknpu_dev->genpd_dev_npu0)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu0);
	}

	rknpu_devfreq_unlock(rknpu_dev);

	clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);

	if (rknpu_dev->mem && !IS_ERR(rknpu_dev->mem))
		regulator_disable(rknpu_dev->mem);

	if (rknpu_dev->vdd && !IS_ERR(rknpu_dev->vdd))
		regulator_disable(rknpu_dev->vdd);

	return 0;
}

static int rknpu_register_irq(struct platform_device *pdev,
			      struct rknpu_device *rknpu_dev)
{
	const struct rknpu_config *config = rknpu_dev->config;
	struct device *dev = &pdev->dev;
	int i, ret, irq;

	for (i = 0; i < config->num_irqs; i++) {
		irq = platform_get_irq_byname(pdev, config->irqs[i].name);
		if (irq < 0) {
			irq = platform_get_irq(pdev, i);
			if (irq < 0) {
				LOG_DEV_ERROR(dev, "no npu %s in dts\n",
					      config->irqs[i].name);
				return irq;
			}
		}

		ret = devm_request_irq(dev, irq, config->irqs[i].irq_hdl,
				       IRQF_SHARED, dev_name(dev), rknpu_dev);
		if (ret < 0) {
			LOG_DEV_ERROR(dev, "request %s failed: %d\n",
				      config->irqs[i].name, ret);
			return ret;
		}
	}

	return 0;
}

static int rknpu_find_sram_resource(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	struct device_node *sram_node = NULL;
	struct resource sram_res;
	uint32_t sram_size = 0;
	int ret = -EINVAL;

	/* get sram device node */
	sram_node = of_parse_phandle(dev->of_node, "rockchip,sram", 0);
	rknpu_dev->sram_size = 0;
	if (!sram_node)
		return -EINVAL;

	/* get sram start and size */
	ret = of_address_to_resource(sram_node, 0, &sram_res);
	of_node_put(sram_node);
	if (ret)
		return ret;

	/* check sram start and size is PAGE_SIZE align */
	rknpu_dev->sram_start = round_up(sram_res.start, PAGE_SIZE);
	rknpu_dev->sram_end = round_down(
		sram_res.start + resource_size(&sram_res), PAGE_SIZE);
	if (rknpu_dev->sram_end <= rknpu_dev->sram_start) {
		LOG_DEV_WARN(
			dev,
			"invalid sram resource, sram start %pa, sram end %pa\n",
			&rknpu_dev->sram_start, &rknpu_dev->sram_end);
		return -EINVAL;
	}

	sram_size = rknpu_dev->sram_end - rknpu_dev->sram_start;

	rknpu_dev->sram_base_io =
		devm_ioremap(dev, rknpu_dev->sram_start, sram_size);
	if (IS_ERR(rknpu_dev->sram_base_io)) {
		LOG_DEV_ERROR(dev, "failed to remap sram base io!\n");
		rknpu_dev->sram_base_io = NULL;
	}

	rknpu_dev->sram_size = sram_size;

	LOG_DEV_INFO(dev, "sram region: [%pa, %pa), sram size: %#x\n",
		     &rknpu_dev->sram_start, &rknpu_dev->sram_end,
		     rknpu_dev->sram_size);

	return 0;
}

static int rknpu_find_nbuf_resource(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;

	if (rknpu_dev->config->nbuf_size == 0)
		return -EINVAL;

	rknpu_dev->nbuf_start = rknpu_dev->config->nbuf_phyaddr;
	rknpu_dev->nbuf_size = rknpu_dev->config->nbuf_size;
	rknpu_dev->nbuf_base_io =
		devm_ioremap(dev, rknpu_dev->nbuf_start, rknpu_dev->nbuf_size);
	if (IS_ERR(rknpu_dev->nbuf_base_io)) {
		LOG_DEV_ERROR(dev, "failed to remap nbuf base io!\n");
		rknpu_dev->nbuf_base_io = NULL;
	}

	rknpu_dev->nbuf_end = rknpu_dev->nbuf_start + rknpu_dev->nbuf_size;

	LOG_DEV_INFO(dev, "nbuf region: [%pa, %pa), nbuf size: %#x\n",
		     &rknpu_dev->nbuf_start, &rknpu_dev->nbuf_end,
		     rknpu_dev->nbuf_size);

	return 0;
}

