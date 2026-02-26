// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sync_file.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/moduleparam.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/list.h>

#include "rknpu_ioctl.h"
#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_fence.h"
#include "rknpu_iommu.h"
#include "rknpu_job.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
#include "rknpu_gem.h"
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
#include "rknpu_mem.h"
#endif

#ifdef RKNPU_DKMS_MISCDEV
#include "rknpu_mem.h"
#endif

#ifdef RKNPU_DKMS
static bool allow_unsafe_no_power_domains;
module_param_named(allow_unsafe_no_power_domains,
		  allow_unsafe_no_power_domains, bool, 0644);
MODULE_PARM_DESC(
	allow_unsafe_no_power_domains,
	"allow job submit even if DT lacks power-domains in non-IOMMU mode (DANGEROUS; may SError)"
);

static int dkms_pc_addr_mode;
module_param(dkms_pc_addr_mode, int, 0644);
MODULE_PARM_DESC(
	dkms_pc_addr_mode,
	"DKMS: PC_DATA_ADDR/PC_DMA_BASE_ADDR mode when task_base_addr==0 (0=auto, 1=absolute regcmd_addr, 2=base+offset from containing GEM)"
);

static bool dkms_pulse_pc_op_en = true;
module_param(dkms_pulse_pc_op_en, bool, 0644);
MODULE_PARM_DESC(dkms_pulse_pc_op_en,
		 "DKMS: pulse PC_OP_EN (write 1 then 0) like upstream driver");

static bool dkms_clear_int_all;
module_param(dkms_clear_int_all, bool, 0644);
MODULE_PARM_DESC(dkms_clear_int_all,
		 "DKMS: clear all interrupt bits before submit (instead of first_task->int_mask)");

static bool dkms_force_int_mask_bit16;
module_param(dkms_force_int_mask_bit16, bool, 0644);

static bool dkms_write_enable_mask;
module_param(dkms_write_enable_mask, bool, 0644);
MODULE_PARM_DESC(dkms_write_enable_mask,
		 "DKMS: write RKNPU_OFFSET_ENABLE_MASK from first_task->enable_mask");

static bool dkms_pc_use_iommu_phys;
module_param(dkms_pc_use_iommu_phys, bool, 0644);
MODULE_PARM_DESC(
	dkms_pc_use_iommu_phys,
	"DKMS: program PC addresses using iommu_iova_to_phys() result (debug; tests whether NPU is actually behind IOMMU)"
);

static bool dkms_pc_use_cmd_sg_phys;
module_param(dkms_pc_use_cmd_sg_phys, bool, 0644);
MODULE_PARM_DESC(
	dkms_pc_use_cmd_sg_phys,
	"DKMS: program PC_DATA_ADDR using cmd GEM physical address (sg_phys/pages) + offset; useful in non-IOMMU mode where dma_addr may be bus/IOVA"
);

static bool rknpu_dkms_cmd_phys_from_off(struct rknpu_gem_object *cmd_gem,
 					 dma_addr_t off, phys_addr_t *phys_out)
 {
 	struct scatterlist *sg;
 	phys_addr_t phys;
 	size_t remain;
	
 	if (!cmd_gem || !phys_out)
 		return false;
 	if (!cmd_gem->sgt || !cmd_gem->sgt->sgl)
 		return false;

 	remain = (size_t)off;
 	for (sg = cmd_gem->sgt->sgl; sg; sg = sg_next(sg)) {
 		size_t seglen = (size_t)sg->length;
 		if (remain < seglen) {
 			phys = sg_phys(sg) + remain;
 			*phys_out = phys;
 			return true;
 		}
 		remain -= seglen;
 	}

	return false;
 }

static bool rknpu_dkms_gem_phys_from_off(struct rknpu_gem_object *obj,
					 dma_addr_t off, phys_addr_t *phys_out)
{
	struct scatterlist *sg;
	phys_addr_t phys;
	size_t remain;

	if (!obj || !phys_out)
		return false;
	if (!obj->sgt || !obj->sgt->sgl)
		return false;

	remain = (size_t)off;
	for (sg = obj->sgt->sgl; sg; sg = sg_next(sg)) {
		size_t seglen = (size_t)sg->length;
		if (remain < seglen) {
			phys = sg_phys(sg) + remain;
			*phys_out = phys;
			return true;
		}
		remain -= seglen;
	}

	return false;
}

static bool dkms_patch_cmd_iova_to_phys;
module_param(dkms_patch_cmd_iova_to_phys, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_iova_to_phys,
	"DKMS: patch command buffer by translating embedded IOVA addresses to physical via iommu_iova_to_phys()"
);

static uint dkms_patch_cmd_scan_bytes = 0x4000;
module_param(dkms_patch_cmd_scan_bytes, uint, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_scan_bytes,
	"DKMS: bytes to scan in command buffer when dkms_patch_cmd_iova_to_phys=1"
);

static uint dkms_patch_cmd_mode;
module_param(dkms_patch_cmd_mode, uint, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_mode,
	"DKMS: cmd patch mode: 0=only values within tracked GEM ranges, 1=any value with iommu_iova_to_phys()!=0"
);

static bool dkms_patch_cmd_start_from_zero;
module_param(dkms_patch_cmd_start_from_zero, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_start_from_zero,
	"DKMS: scan command buffer from start of cmd GEM (instead of from regcmd offset)"
);

static bool dkms_patch_cmd_only_cmd_gem;
module_param(dkms_patch_cmd_only_cmd_gem, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_only_cmd_gem,
	"DKMS: only patch values that fall within the cmd GEM range (avoid patching external buffer addresses)"
);

static uint dkms_patch_cmd_align_mask = 0xfff;
module_param(dkms_patch_cmd_align_mask, uint, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_align_mask,
	"DKMS: alignment mask for patch candidates (default 0xfff requires 4K-page aligned)"
);

static uint dkms_patch_cmd_align_value;
module_param(dkms_patch_cmd_align_value, uint, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_align_value,
	"DKMS: required alignment value for patch candidates (default 0)"
);

static bool dkms_patch_cmd_dry_run;
module_param(dkms_patch_cmd_dry_run, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_dry_run,
	"DKMS: scan/log cmd buffer patch candidates but do not modify the cmd buffer"
);

static bool dkms_dump_regcmd_words;
module_param(dkms_dump_regcmd_words, bool, 0644);
MODULE_PARM_DESC(
	dkms_dump_regcmd_words,
	"DKMS: dump regcmd stream words (u32/u64) near regcmd offset for format inspection"
);

static bool dkms_force_cmd_dma_sync;
module_param(dkms_force_cmd_dma_sync, bool, 0644);
MODULE_PARM_DESC(
	dkms_force_cmd_dma_sync,
	"DKMS: always dma_sync cmd GEM to device before starting PC (even when not patching)"
);

static bool dkms_timeout_dump_ext;
module_param(dkms_timeout_dump_ext, bool, 0644);
MODULE_PARM_DESC(
	dkms_timeout_dump_ext,
	"DKMS: on timeout, dump additional register windows (0x1000/0x3000) and ENABLE_MASK (0xf008)"
);

static bool dkms_timeout_dump_iommu;
module_param(dkms_timeout_dump_iommu, bool, 0644);
MODULE_PARM_DESC(
	dkms_timeout_dump_iommu,
	"DKMS: on timeout, dump Rockchip IOMMU MMIO status regs (page fault/bus error)"
);

static bool dkms_commit_dump_iommu;
module_param(dkms_commit_dump_iommu, bool, 0644);
MODULE_PARM_DESC(
	dkms_commit_dump_iommu,
	"DKMS: on commit, dump Rockchip IOMMU MMIO status regs (page fault/bus error)"
);

static bool dkms_commit_set_iommu_autogating_bit31;
module_param(dkms_commit_set_iommu_autogating_bit31, bool, 0644);
MODULE_PARM_DESC(
	dkms_commit_set_iommu_autogating_bit31,
	"DKMS: on commit, set IOMMU AUTO_GATING BIT(31) (Rockchip workaround: DISABLE_FETCH_DTE_TIME_LIMIT)"
);

static bool dkms_commit_force_iommu_attach;
module_param(dkms_commit_force_iommu_attach, bool, 0644);
MODULE_PARM_DESC(
	dkms_commit_force_iommu_attach,
	"DKMS: on commit, force pm_runtime_get_sync(iommu) + detach/attach (domain,npu) to trigger rk_iommu_enable()"
);

static bool dkms_regcmd_pair_scan;
module_param(dkms_regcmd_pair_scan, bool, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_scan,
	"DKMS: parse regcmd stream as (addr,value) u32 pairs and log values within tracked GEM ranges"
);

static bool dkms_regcmd_pair_patch;
module_param(dkms_regcmd_pair_patch, bool, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_patch,
	"DKMS: translate regcmd pair values via iommu_iova_to_phys() and patch in-place (only when IOMMU is enabled)"
);

static bool dkms_regcmd_pair_strict_objref;
module_param(dkms_regcmd_pair_strict_objref, bool, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_strict_objref,
	"DKMS: for regcmd pair patching, only patch tracked GEM values when sg-derived phys matches iommu_iova_to_phys()"
);

static uint dkms_regcmd_pair_mode;
module_param(dkms_regcmd_pair_mode, uint, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_mode,
	"DKMS: regcmd pair mode: 0=only values within tracked GEM ranges, 1=any value with iommu_iova_to_phys()!=0"
);

static bool dkms_regcmd_pair_start_from_zero;
module_param(dkms_regcmd_pair_start_from_zero, bool, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_start_from_zero,
	"DKMS: parse regcmd pairs from start of cmd GEM (instead of from regcmd offset)"
);

static uint dkms_regcmd_pair_log_limit = 16;
module_param(dkms_regcmd_pair_log_limit, uint, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_log_limit,
	"DKMS: max number of regcmd pair candidates to log"
);

static uint dkms_regcmd_pair_log_candidate_limit = 16;
module_param(dkms_regcmd_pair_log_candidate_limit, uint, 0644);
MODULE_PARM_DESC(
	dkms_regcmd_pair_log_candidate_limit,
	"DKMS: max number of regcmd pair candidate hits to log (phys_ok or tracked GEM)"
);

static bool dkms_patch_cmd_try_u64;
module_param(dkms_patch_cmd_try_u64, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_try_u64,
	"DKMS: also scan u64 words in command buffer and translate low32 IOVA when high32 is 0 or 0xffffffff"
);

static bool dkms_pc_dma_base_from_mmio;
module_param(dkms_pc_dma_base_from_mmio, bool, 0644);
MODULE_PARM_DESC(
	dkms_pc_dma_base_from_mmio,
	"DKMS: when task_base_addr==0, program PC_DMA_BASE_ADDR from platform MMIO resource start (tests regcmd offset semantics)"
);

static uint dkms_pc_task_mode = 6;
module_param(dkms_pc_task_mode, uint, 0644);
MODULE_PARM_DESC(
	dkms_pc_task_mode,
	"DKMS: PC_TASK_CONTROL mode bits (default 6; value is placed in bits [pc_task_number_bits+?])"
);

static bool dkms_patch_cmd_log_untracked;
module_param(dkms_patch_cmd_log_untracked, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_log_untracked,
	"DKMS: log a small sample of aligned IOVA values in cmd buffer that are IOMMU-translatable but not tracked as GEM objects (debug)"
);

static bool dkms_patch_cmd_strict_selfref;
module_param(dkms_patch_cmd_strict_selfref, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_strict_selfref,
	"DKMS: when patching cmd buffer, only patch values that are proven cmd-GEM self-references (IOMMU phys matches cmd GEM sg phys at same offset)"
);

static bool dkms_patch_cmd_strict_objref;
module_param(dkms_patch_cmd_strict_objref, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_strict_objref,
	"DKMS: when patching cmd buffer, only patch values within tracked GEM ranges when sg-derived phys matches iommu_iova_to_phys() (reduces false positives)"
);

static bool dkms_patch_cmd_patch_other_obj;
module_param(dkms_patch_cmd_patch_other_obj, bool, 0644);
MODULE_PARM_DESC(
	dkms_patch_cmd_patch_other_obj,
	"DKMS: when dkms_patch_cmd_only_cmd_gem=1, optionally also patch values that fall into other tracked GEM objects (debug)"
);

#endif /* RKNPU_DKMS */

#ifdef RKNPU_DKMS

struct rknpu_dkms_rk_iommu_dbg {
	struct device *dev;
	void __iomem **bases;
	int num_mmu;
	int num_irq;
	struct clk_bulk_data *clocks;
	int num_clocks;
	bool reset_disabled;
	u8 __pad[3];
	struct iommu_device iommu;
	struct list_head node;
	struct iommu_domain *domain;
};

struct rknpu_dkms_rk_iommu_domain_dbg {
	struct list_head iommus;
	u32 *dt;
	dma_addr_t dt_dma;
	spinlock_t iommus_lock;
	spinlock_t dt_lock;
	struct iommu_domain domain;
};

static u32 rknpu_dkms_rk_mk_dte_v2(dma_addr_t pt_dma)
{
	u64 v = (u64)pt_dma;
	u64 lo = v & 0xFFFFFFF0ULL;
	u64 hi1 = (v & 0xF00000000ULL) >> 24;
	u64 hi2 = (v & 0xF000000000ULL) >> 32;
	u64 enc = (lo | hi1 | hi2) & 0xFFFFFFF0ULL;

	return (u32)enc | 0x1;
}

static void rknpu_dkms_dump_iommu(struct rknpu_device *rknpu_dev,
				 const char *prefix)
{
	struct device_node *iommu_np;
	struct platform_device *iommu_pdev;
	struct rknpu_dkms_rk_iommu_dbg *rk_iommu;
	struct iommu_domain *domain;
	int idx;

	if (!prefix)
		prefix = "";
	if (!rknpu_dev || !rknpu_dev->dev || !rknpu_dev->dev->of_node) {
		LOG_ERROR("%siommu: device/of_node missing\n", prefix);
		return;
	}

	iommu_np = of_parse_phandle(rknpu_dev->dev->of_node, "iommus", 0);
	if (!iommu_np) {
		LOG_ERROR("%siommu: no iommus phandle\n", prefix);
		return;
	}

	iommu_pdev = of_find_device_by_node(iommu_np);
	if (!iommu_pdev) {
		LOG_ERROR("%siommu_pm: of_find_device_by_node failed\n", prefix);
	} else {
		struct device *iommu_dev = &iommu_pdev->dev;
		rk_iommu = platform_get_drvdata(iommu_pdev);
		LOG_ERROR(
			"%siommu_pm: runtime_status=%d active=%d usage_count=%d\n",
			prefix, (int)iommu_dev->power.runtime_status,
			pm_runtime_active(iommu_dev) ? 1 : 0,
			atomic_read(&iommu_dev->power.usage_count));
		LOG_ERROR("%siommu_pm: iommu_dev=%px rk_iommu=%px\n", prefix,
			  iommu_dev, rk_iommu);
		if (rk_iommu) {
			void __iomem *b0 = NULL;
			LOG_ERROR(
				"%siommu_pm: rk_iommu->dev=%px rk_iommu->domain=%px reset_disabled=%d num_clocks=%d\n",
				prefix, rk_iommu->dev, rk_iommu->domain,
				rk_iommu->reset_disabled ? 1 : 0, rk_iommu->num_clocks);
			LOG_ERROR("%siommu_pm: num_mmu=%d bases=%px\n", prefix,
				  rk_iommu->num_mmu, rk_iommu->bases);
			if (rk_iommu->bases && rk_iommu->num_mmu > 0)
				b0 = rk_iommu->bases[0];
			LOG_ERROR("%siommu_pm: base0=%px\n", prefix, b0);
			if (b0) {
				u32 status = readl(b0 + 0x04);
				LOG_ERROR(
					"%siommu_base0: DTE_ADDR=%#x STATUS=%#x COMMAND=%#x PF_ADDR=%#x\n",
					prefix, readl(b0 + 0x00), status, readl(b0 + 0x08),
					readl(b0 + 0x0c));
				LOG_ERROR(
					"%siommu_base0: INT_RAW=%#x INT_STATUS=%#x INT_MASK=%#x INT_CLEAR=%#x AUTO_GATING=%#x\n",
					prefix, readl(b0 + 0x14), readl(b0 + 0x20), readl(b0 + 0x1c),
					readl(b0 + 0x18), readl(b0 + 0x24));
				LOG_ERROR(
					"%siommu_base0: STATUS{paging=%d pf_active=%d stall=%d idle=%d is_write=%d}\n",
					prefix, (status & BIT(0)) ? 1 : 0,
					(status & BIT(1)) ? 1 : 0,
					(status & BIT(2)) ? 1 : 0,
					(status & BIT(3)) ? 1 : 0,
					(status & BIT(5)) ? 1 : 0);
			}
		}

		domain = iommu_get_domain_for_dev(rknpu_dev->dev);
		LOG_ERROR("%siommu_domain: npu_dev=%px npu_domain=%px\n", prefix,
			  rknpu_dev->dev, domain);
		if (domain) {
			if (domain->type != IOMMU_DOMAIN_IDENTITY && domain->ops &&
			    domain->ops->map_pages) {
				struct rknpu_dkms_rk_iommu_domain_dbg *rk_dom;
				rk_dom = container_of(
					domain,
					struct rknpu_dkms_rk_iommu_domain_dbg,
					domain);
				LOG_ERROR(
					"%siommu_domain: rk_dom=%px dt=%px dt_dma=%pad expected_dte_v2=%#x\n",
					prefix, rk_dom, rk_dom->dt, &rk_dom->dt_dma,
					rknpu_dkms_rk_mk_dte_v2(rk_dom->dt_dma));
			} else {
				LOG_ERROR(
					"%siommu_domain: skipping rk_dom decode (domain_type=%d ops=%p)\n",
					prefix, (int)domain->type, domain->ops);
			}
		}
		put_device(iommu_dev);
	}

	for (idx = 0; idx < 4; idx++) {
		struct resource res;
		phys_addr_t start;
		phys_addr_t size;
		void __iomem *b;
		u32 status;

		if (of_address_to_resource(iommu_np, idx, &res))
			break;

		start = (phys_addr_t)res.start;
		size = (phys_addr_t)resource_size(&res);
		b = ioremap(start, (size_t)size);
		if (!b) {
			LOG_ERROR(
				"%siommu[%d] ioremap failed start=%pa size=%pa\n",
				prefix, idx, &start, &size);
			continue;
		}

		status = readl(b + 0x04);
		LOG_ERROR("%siommu[%d] start=%pa size=%pa base=%p\n", prefix, idx,
			  &start, &size, b);
		LOG_ERROR(
			"%siommu[%d] DTE_ADDR=%#x STATUS=%#x COMMAND=%#x PF_ADDR=%#x\n",
			prefix, idx, readl(b + 0x00), status, readl(b + 0x08),
			readl(b + 0x0c));
		LOG_ERROR(
			"%siommu[%d] INT_RAW=%#x INT_STATUS=%#x INT_MASK=%#x INT_CLEAR=%#x AUTO_GATING=%#x\n",
			prefix, idx, readl(b + 0x14), readl(b + 0x20), readl(b + 0x1c),
			readl(b + 0x18), readl(b + 0x24));
		LOG_ERROR(
			"%siommu[%d] STATUS{paging=%d pf_active=%d stall=%d idle=%d is_write=%d}\n",
			prefix, idx, (status & BIT(0)) ? 1 : 0,
			(status & BIT(1)) ? 1 : 0, (status & BIT(2)) ? 1 : 0,
			(status & BIT(3)) ? 1 : 0, (status & BIT(5)) ? 1 : 0);
		iounmap(b);
	}

	of_node_put(iommu_np);
}

static void rknpu_dkms_force_iommu_attach(struct rknpu_device *rknpu_dev,
					 const char *prefix)
{
	struct device_node *iommu_np;
	struct platform_device *iommu_pdev;
	struct device *iommu_dev;
	struct iommu_domain *domain;
	int pret;
	int ret;

	if (!prefix)
		prefix = "";
	if (!rknpu_dev || !rknpu_dev->dev || !rknpu_dev->dev->of_node) {
		LOG_ERROR("%siommu_force: device/of_node missing\n", prefix);
		return;
	}

	iommu_np = of_parse_phandle(rknpu_dev->dev->of_node, "iommus", 0);
	if (!iommu_np) {
		LOG_ERROR("%siommu_force: no iommus phandle\n", prefix);
		return;
	}

	iommu_pdev = of_find_device_by_node(iommu_np);
	of_node_put(iommu_np);
	if (!iommu_pdev) {
		LOG_ERROR("%siommu_force: of_find_device_by_node failed\n", prefix);
		return;
	}
	iommu_dev = &iommu_pdev->dev;

	domain = iommu_get_domain_for_dev(rknpu_dev->dev);
	if (!domain) {
		LOG_ERROR("%siommu_force: iommu_get_domain_for_dev(npu) returned NULL\n",
			  prefix);
		put_device(iommu_dev);
		return;
	}

	pret = pm_runtime_get_sync(iommu_dev);
	LOG_ERROR(
		"%siommu_force: pm_runtime_get_sync ret=%d runtime_status=%d usage_count=%d\n",
		prefix, pret, (int)iommu_dev->power.runtime_status,
		atomic_read(&iommu_dev->power.usage_count));

	iommu_detach_device(domain, rknpu_dev->dev);
	ret = iommu_attach_device(domain, rknpu_dev->dev);
	LOG_ERROR("%siommu_force: iommu_attach_device ret=%d\n", prefix, ret);

	pm_runtime_put_sync(iommu_dev);
	put_device(iommu_dev);
}

static void rknpu_dkms_set_iommu_autogating_bit31(struct rknpu_device *rknpu_dev,
					  const char *prefix)
{
	struct device_node *iommu_np;
	struct resource res;
	phys_addr_t start;
	phys_addr_t size;
	void __iomem *b;
	u32 before;
	u32 after;

	if (!prefix)
		prefix = "";
	if (!rknpu_dev || !rknpu_dev->dev || !rknpu_dev->dev->of_node) {
		LOG_ERROR("%siommu: device/of_node missing\n", prefix);
		return;
	}

	iommu_np = of_parse_phandle(rknpu_dev->dev->of_node, "iommus", 0);
	if (!iommu_np) {
		LOG_ERROR("%siommu: no iommus phandle\n", prefix);
		return;
	}

	if (of_address_to_resource(iommu_np, 0, &res)) {
		LOG_ERROR("%siommu: of_address_to_resource(idx=0) failed\n", prefix);
		of_node_put(iommu_np);
		return;
	}

	start = (phys_addr_t)res.start;
	size = (phys_addr_t)resource_size(&res);
	b = ioremap(start, (size_t)size);
	if (!b) {
		LOG_ERROR("%siommu: ioremap failed start=%pa size=%pa\n", prefix,
			  &start, &size);
		of_node_put(iommu_np);
		return;
	}

	before = readl(b + 0x24);
	writel(before | BIT(31), b + 0x24);
	after = readl(b + 0x24);
	LOG_ERROR("%siommu: AUTO_GATING before=%#x after=%#x\n", prefix, before,
		  after);

	iounmap(b);
	of_node_put(iommu_np);
}

static void rknpu_dkms_patch_cmd_buf_iova_to_phys(struct rknpu_device *rknpu_dev,
						  struct rknpu_gem_object *cmd_gem,
						  dma_addr_t cmd_gem_base,
						  dma_addr_t regcmd_addr,
						  dma_addr_t scan_off,
						  size_t scan_len)
{
	struct iommu_domain *domain;
	size_t i;
	u32 replaced = 0;
	u32 candidates = 0;
	u32 translatable = 0;
	u32 logged = 0;
	u32 logged_other_obj = 0;
	u32 logged_untracked = 0;
	u32 untracked_checked = 0;
	u32 skipped_self_nomap = 0;
	u32 skipped_self_mismatch = 0;
	u32 logged_self_mismatch = 0;
	u32 skipped_align = 0;
	u32 skipped_other_obj = 0;
	u32 candidates64 = 0;
	u32 translatable64 = 0;
	u32 replaced64 = 0;

	if (!dkms_patch_cmd_iova_to_phys && !dkms_patch_cmd_dry_run)
		return;
	if (!rknpu_dev || !cmd_gem)
		return;
	if (!rknpu_dev->iommu_en)
		return;

	domain = iommu_get_domain_for_dev(rknpu_dev->dev);
	if (!domain)
		return;

	if (!scan_len)
		return;

	LOG_ERROR(
		"DKMS: patch_cmd_iova_to_phys base=%#llx regcmd=%#llx off=%#llx len=%zu\n",
		(unsigned long long)cmd_gem_base,
		(unsigned long long)regcmd_addr,
		(unsigned long long)scan_off,
		scan_len);

	if (cmd_gem->kv_addr) {
		u32 *w = (u32 *)((u8 *)cmd_gem->kv_addr + scan_off);
		for (i = 0; i + sizeof(u32) <= scan_len; i += sizeof(u32)) {
			u32 v = READ_ONCE(w[i / 4]);
			dma_addr_t base = 0;
			struct rknpu_gem_object *obj;
			phys_addr_t phys;

			if ((v & dkms_patch_cmd_align_mask) !=
			    (dkms_patch_cmd_align_value & dkms_patch_cmd_align_mask)) {
				skipped_align++;
				continue;
			}

			obj = rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)v, &base);
			if (!obj && dkms_patch_cmd_mode == 0) {
				if (dkms_patch_cmd_log_untracked &&
				    logged_untracked < 8 &&
				    untracked_checked < 8192) {
					untracked_checked++;
					phys = iommu_iova_to_phys(domain, (dma_addr_t)v);
					if (phys && (phys >> 32) == 0) {
						LOG_ERROR(
							"DKMS: patch_cmd untracked translatable v=%#x phys=%#llx\n",
							v, (unsigned long long)phys);
						logged_untracked++;
					}
				}
				continue;
			}
			if (dkms_patch_cmd_only_cmd_gem && obj && obj != cmd_gem &&
			    !dkms_patch_cmd_patch_other_obj) {
				skipped_other_obj++;
				if (logged_other_obj < 8) {
					phys_addr_t phys_other = 0;
					if (domain)
						phys_other = iommu_iova_to_phys(domain, (dma_addr_t)v);
					LOG_ERROR(
						"DKMS: patch_cmd skip other_obj off=%#llx v=%#x phys=%#llx obj=%p base=%#llx\n",
						(unsigned long long)(scan_off + i),
						v, (unsigned long long)phys_other, obj,
						(unsigned long long)base);
					logged_other_obj++;
				}
				rknpu_gem_object_put(&obj->base);
				continue;
			}
			candidates++;

			phys = iommu_iova_to_phys(domain, (dma_addr_t)v);
			if (!phys || (phys >> 32) != 0) {
				if (obj)
					rknpu_gem_object_put(&obj->base);
				continue;
			}
			translatable++;

			if (dkms_patch_cmd_strict_objref && obj) {
				dma_addr_t off;
				phys_addr_t expected = 0;

				if ((dma_addr_t)v < base) {
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				off = (dma_addr_t)v - base;
				if (!rknpu_dkms_gem_phys_from_off(obj, off, &expected)) {
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				if ((u32)expected != (u32)phys) {
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				phys = expected;
			}

			if (dkms_patch_cmd_strict_selfref && obj == cmd_gem) {
				dma_addr_t off;
				phys_addr_t expected = 0;

				if ((dma_addr_t)v < cmd_gem_base) {
					skipped_self_nomap++;
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				off = (dma_addr_t)v - cmd_gem_base;
				if (!rknpu_dkms_cmd_phys_from_off(cmd_gem, off, &expected)) {
					skipped_self_nomap++;
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				if ((u32)expected != (u32)phys) {
					skipped_self_mismatch++;
					if (logged_self_mismatch < 8) {
						LOG_ERROR(
							"DKMS: patch_cmd strict mismatch v=%#x off=%#llx phys=%#llx expected=%#llx\n",
							v,
							(unsigned long long)off,
							(unsigned long long)phys,
							(unsigned long long)expected);
						logged_self_mismatch++;
					}
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
			}

			if (logged < 8) {
				LOG_ERROR(
					"DKMS: patch_cmd candidate off=%#llx v=%#x phys=%#llx obj=%p base=%#llx\n",
					(unsigned long long)(scan_off + i),
					v, (unsigned long long)phys, obj,
					(unsigned long long)base);
				logged++;
			}

			if (!dkms_patch_cmd_dry_run) {
				WRITE_ONCE(w[i / 4], (u32)phys);
				replaced++;
			}
			if (obj)
				rknpu_gem_object_put(&obj->base);
		}

		if (dkms_patch_cmd_try_u64) {
			u64 *w64 = (u64 *)((u8 *)cmd_gem->kv_addr + scan_off);
			size_t n64 = scan_len / sizeof(u64);
			size_t k;

			for (k = 0; k < n64; k++) {
				u64 vv = READ_ONCE(w64[k]);
				dma_addr_t base = 0;
				struct rknpu_gem_object *obj =
					rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)vv, &base);
				phys_addr_t phys;

				if ((vv & dkms_patch_cmd_align_mask) !=
				    (dkms_patch_cmd_align_value &
				     dkms_patch_cmd_align_mask))
					continue;

				obj = rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)vv, &base);
				if (!obj && dkms_patch_cmd_mode == 0) {
					if (dkms_patch_cmd_log_untracked &&
					    logged_untracked < 8 &&
					    untracked_checked < 8192) {
						untracked_checked++;
						phys = iommu_iova_to_phys(domain, (dma_addr_t)vv);
						if (phys && (phys >> 32) == 0) {
							LOG_ERROR(
								"DKMS: patch_cmd untracked translatable vv=%#llx phys=%#llx\n",
								(unsigned long long)vv,
								(unsigned long long)phys);
							logged_untracked++;
						}
					}
					continue;
				}
				if (dkms_patch_cmd_only_cmd_gem && obj && obj != cmd_gem &&
				    !dkms_patch_cmd_patch_other_obj) {
					skipped_other_obj++;
					if (logged_other_obj < 8) {
						phys_addr_t phys_other = 0;
						if (domain)
							phys_other = iommu_iova_to_phys(domain, (dma_addr_t)vv);
						LOG_ERROR(
							"DKMS: patch_cmd skip other_obj off=%#llx vv=%#llx phys=%#llx obj=%p base=%#llx\n",
							(unsigned long long)(scan_off + k * sizeof(u64)),
							(unsigned long long)vv,
							(unsigned long long)phys_other, obj,
							(unsigned long long)base);
						logged_other_obj++;
					}
					rknpu_gem_object_put(&obj->base);
					continue;
				}

				candidates64++;
				phys = iommu_iova_to_phys(domain, (dma_addr_t)vv);
				if (!phys || (phys >> 32) != 0) {
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}
				translatable64++;

				if (dkms_patch_cmd_strict_objref && obj) {
					dma_addr_t off;
					phys_addr_t expected = 0;
					dma_addr_t vvv = (dma_addr_t)vv;

					if (vvv < base) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					off = vvv - base;
					if (!rknpu_dkms_gem_phys_from_off(obj, off, &expected)) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					if ((u32)expected != (u32)phys) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					phys = expected;
				}

				if (dkms_patch_cmd_strict_selfref && obj == cmd_gem) {
					dma_addr_t off;
					phys_addr_t expected = 0;
					dma_addr_t vvv = (dma_addr_t)vv;

					if (vvv < cmd_gem_base) {
						skipped_self_nomap++;
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					off = vvv - cmd_gem_base;
					if (!rknpu_dkms_cmd_phys_from_off(cmd_gem, off, &expected)) {
						skipped_self_nomap++;
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					if ((u32)expected != (u32)phys) {
						skipped_self_mismatch++;
						if (logged_self_mismatch < 8) {
							LOG_ERROR(
								"DKMS: patch_cmd strict mismatch vv=%#llx off=%#llx phys=%#llx expected=%#llx\n",
								(unsigned long long)vv,
								(unsigned long long)off,
								(unsigned long long)phys,
								(unsigned long long)expected);
							logged_self_mismatch++;
						}
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
				}
				if (!dkms_patch_cmd_dry_run) {
					WRITE_ONCE(w64[k], (u64)(u32)phys);
					replaced64++;
				}
				if (obj)
					rknpu_gem_object_put(&obj->base);
			}
		}
	} else if (cmd_gem->pages) {
		size_t remaining = scan_len;
		dma_addr_t cur_off = scan_off;
		unsigned long page_index = (unsigned long)(cur_off >> PAGE_SHIFT);
		unsigned long page_off = (unsigned long)(cur_off & (PAGE_SIZE - 1));

		while (remaining && page_index < cmd_gem->num_pages) {
			void *vaddr = kmap_local_page(cmd_gem->pages[page_index]);
			size_t chunk = PAGE_SIZE - page_off;
			size_t j;

			if (chunk > remaining)
				chunk = remaining;

			for (j = 0; j + sizeof(u32) <= chunk; j += sizeof(u32)) {
				u32 *p = (u32 *)((u8 *)vaddr + page_off + j);
				u32 vv = READ_ONCE(*p);
				dma_addr_t base = 0;
				struct rknpu_gem_object *obj;
				phys_addr_t phys;

				if ((vv & dkms_patch_cmd_align_mask) !=
				    (dkms_patch_cmd_align_value & dkms_patch_cmd_align_mask)) {
					skipped_align++;
					continue;
				}

				obj = rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)vv, &base);
				if (!obj && dkms_patch_cmd_mode == 0) {
					if (dkms_patch_cmd_log_untracked &&
					    logged_untracked < 8 &&
					    untracked_checked < 8192) {
						untracked_checked++;
						phys = iommu_iova_to_phys(domain, (dma_addr_t)vv);
						if (phys && (phys >> 32) == 0) {
							LOG_ERROR(
								"DKMS: patch_cmd untracked translatable v=%#x phys=%#llx\n",
								vv, (unsigned long long)phys);
							logged_untracked++;
						}
					}
					continue;
				}
				if (dkms_patch_cmd_only_cmd_gem && obj && obj != cmd_gem &&
				    !dkms_patch_cmd_patch_other_obj) {
					skipped_other_obj++;
					if (logged_other_obj < 8) {
						phys_addr_t phys_other = 0;
						if (domain)
							phys_other = iommu_iova_to_phys(domain, (dma_addr_t)vv);
						LOG_ERROR(
							"DKMS: patch_cmd skip other_obj off=%#llx vv=%#x phys=%#llx obj=%p base=%#llx\n",
							(unsigned long long)(((dma_addr_t)page_index << PAGE_SHIFT) + page_off + j),
							vv, (unsigned long long)phys_other, obj,
							(unsigned long long)base);
						logged_other_obj++;
					}
					rknpu_gem_object_put(&obj->base);
					continue;
				}
				candidates++;
				phys = iommu_iova_to_phys(domain, (dma_addr_t)vv);
				if (!phys || (phys >> 32) != 0) {
					rknpu_gem_object_put(&obj->base);
					continue;
				}
				translatable++;

				if (dkms_patch_cmd_strict_selfref && obj == cmd_gem) {
					dma_addr_t off;
					phys_addr_t expected = 0;

					if ((dma_addr_t)vv < cmd_gem_base) {
						skipped_self_nomap++;
						rknpu_gem_object_put(&obj->base);
						continue;
					}
					off = (dma_addr_t)vv - cmd_gem_base;
					if (!rknpu_dkms_cmd_phys_from_off(cmd_gem, off, &expected)) {
						skipped_self_nomap++;
						rknpu_gem_object_put(&obj->base);
						continue;
					}
					if ((u32)expected != (u32)phys) {
						skipped_self_mismatch++;
						if (logged_self_mismatch < 8) {
							LOG_ERROR(
								"DKMS: patch_cmd strict mismatch v=%#x off=%#llx phys=%#llx expected=%#llx\n",
								vv,
								(unsigned long long)off,
								(unsigned long long)phys,
								(unsigned long long)expected);
							logged_self_mismatch++;
						}
						rknpu_gem_object_put(&obj->base);
						continue;
					}
				}

				if (logged < 8) {
					LOG_ERROR(
						"DKMS: patch_cmd candidate off=%#llx v=%#x phys=%#llx obj=%p base=%#llx\n",
						(unsigned long long)(((dma_addr_t)page_index << PAGE_SHIFT) + page_off + j),
						vv, (unsigned long long)phys, obj,
						(unsigned long long)base);
					logged++;
				}
				if (!dkms_patch_cmd_dry_run) {
					WRITE_ONCE(*p, (u32)phys);
					replaced++;
				}
				if (obj)
					rknpu_gem_object_put(&obj->base);
			}

			kunmap_local(vaddr);
			remaining -= chunk;
			page_index++;
			page_off = 0;
		}
	} else {
		LOG_ERROR(
			"DKMS: patch_cmd_iova_to_phys skipped: cmd GEM has no CPU mapping (kv_addr=NULL, pages=NULL)\n");
	}

	LOG_ERROR(
		"DKMS: patch_cmd_iova_to_phys summary candidates=%u translatable=%u replaced=%u skipped_align=%u skipped_other_obj=%u skipped_self_nomap=%u skipped_self_mismatch=%u\n",
		candidates, translatable, replaced, skipped_align,
		skipped_other_obj,
		skipped_self_nomap,
		skipped_self_mismatch);
	if (dkms_patch_cmd_try_u64)
		LOG_ERROR(
			"DKMS: patch_cmd_iova_to_phys u64 summary candidates=%u translatable=%u replaced=%u\n",
			candidates64, translatable64, replaced64);
}

static u32 rknpu_dkms_scan_regcmd_pairs(struct rknpu_device *rknpu_dev,
					struct rknpu_gem_object *cmd_gem,
					dma_addr_t cmd_gem_base,
					dma_addr_t regcmd_addr,
					dma_addr_t scan_off,
					size_t scan_len)
{
	struct iommu_domain *domain = NULL;
	u32 pairs = 0;
	u32 candidates = 0;
	u32 translatable = 0;
	u32 patched = 0;
	u32 logged = 0;
	u32 logged_candidate = 0;
	bool have_addr = false;
	u32 cur_addr = 0;
	size_t i;

	if (!dkms_regcmd_pair_scan && !dkms_regcmd_pair_patch)
		return 0;
	if (!rknpu_dev || !cmd_gem)
		return 0;
	if (!scan_len)
		return 0;

	if (rknpu_dev->iommu_en)
		domain = iommu_get_domain_for_dev(rknpu_dev->dev);

	LOG_ERROR(
		"DKMS: regcmd_pair scan base=%#llx regcmd=%#llx off=%#llx len=%zu\n",
		(unsigned long long)cmd_gem_base,
		(unsigned long long)regcmd_addr,
		(unsigned long long)scan_off,
		scan_len);

	if (cmd_gem->kv_addr) {
		u32 *w = (u32 *)((u8 *)cmd_gem->kv_addr + scan_off);
		size_t n = scan_len / sizeof(u32);

		for (i = 0; i < n; i++) {
			u32 v = READ_ONCE(w[i]);

			if (!have_addr) {
				cur_addr = v;
				have_addr = true;
				continue;
			}
			have_addr = false;
			pairs++;

			{
				dma_addr_t base = 0;
				struct rknpu_gem_object *obj =
					rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)v, &base);
				phys_addr_t phys = 0;
				bool want_phys = dkms_regcmd_pair_patch || dkms_regcmd_pair_mode == 1;
				bool phys_ok = false;
				bool is_candidate = false;

				if (want_phys && domain && rknpu_dev->iommu_en) {
					phys = iommu_iova_to_phys(domain, (dma_addr_t)v);
					phys_ok = phys && ((phys >> 32) == 0);
				}

				if (dkms_regcmd_pair_strict_objref && obj && phys_ok) {
					dma_addr_t off;
					phys_addr_t expected = 0;
					if ((dma_addr_t)v < base) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					off = (dma_addr_t)v - base;
					if (!rknpu_dkms_gem_phys_from_off(obj, off, &expected)) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					if ((u32)expected != (u32)phys) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}
					phys = expected;
				}

				if (dkms_regcmd_pair_scan &&
				    logged < dkms_regcmd_pair_log_limit) {
					LOG_ERROR(
						"DKMS: regcmd_pair addr=%#x value=%#x phys=%#llx obj=%p base=%#llx\n",
						cur_addr,
						v,
						(unsigned long long)phys,
						obj,
						(unsigned long long)base);
					logged++;
				}

				if (dkms_regcmd_pair_strict_objref)
					is_candidate = !!obj;
				else
					is_candidate = !!obj || (dkms_regcmd_pair_mode == 1 && phys_ok);
				if (!is_candidate) {
					if (obj)
						rknpu_gem_object_put(&obj->base);
					continue;
				}

				if (dkms_regcmd_pair_scan &&
				    logged_candidate < dkms_regcmd_pair_log_candidate_limit) {
					LOG_ERROR(
						"DKMS: regcmd_pair CAND pair=%u off_addr=%#llx off_val=%#llx addr=%#x value=%#x phys=%#llx obj=%p base=%#llx\n",
						pairs,
						(unsigned long long)(scan_off + (i - 1) * sizeof(u32)),
						(unsigned long long)(scan_off + i * sizeof(u32)),
						cur_addr,
						v,
						(unsigned long long)phys,
						obj,
						(unsigned long long)base);
					logged_candidate++;
				}

				candidates++;

				if (dkms_regcmd_pair_patch && domain && rknpu_dev->iommu_en) {
					if (phys_ok) {
						translatable++;
						WRITE_ONCE(w[i], (u32)phys);
						patched++;
					}
				}
				if (obj)
					rknpu_gem_object_put(&obj->base);
			}
		}
	} else if (cmd_gem->pages) {
		size_t remaining = scan_len;
		dma_addr_t cur_off = scan_off;
		unsigned long page_index = (unsigned long)(cur_off >> PAGE_SHIFT);
		unsigned long page_off = (unsigned long)(cur_off & (PAGE_SIZE - 1));

		while (remaining && page_index < cmd_gem->num_pages) {
			void *vaddr = kmap_local_page(cmd_gem->pages[page_index]);
			size_t chunk = PAGE_SIZE - page_off;
			size_t j;

			if (chunk > remaining)
				chunk = remaining;
			chunk &= ~(sizeof(u32) - 1);

			for (j = 0; j + sizeof(u32) <= chunk; j += sizeof(u32)) {
				u32 *p = (u32 *)((u8 *)vaddr + page_off + j);
				u32 v = READ_ONCE(*p);

				if (!have_addr) {
					cur_addr = v;
					have_addr = true;
					continue;
				}
				have_addr = false;
				pairs++;

				{
					dma_addr_t base = 0;
					struct rknpu_gem_object *obj =
						rknpu_dkms_find_gem_obj_by_addr((dma_addr_t)v, &base);
					phys_addr_t phys = 0;
					bool want_phys = dkms_regcmd_pair_patch || dkms_regcmd_pair_mode == 1;
					bool phys_ok = false;
					bool is_candidate = false;

					if (want_phys && domain && rknpu_dev->iommu_en) {
						phys = iommu_iova_to_phys(domain, (dma_addr_t)v);
						phys_ok = phys && ((phys >> 32) == 0);
					}

					if (dkms_regcmd_pair_scan &&
					    logged < dkms_regcmd_pair_log_limit) {
						LOG_ERROR(
							"DKMS: regcmd_pair addr=%#x value=%#x phys=%#llx obj=%p base=%#llx\n",
							cur_addr,
							v,
							(unsigned long long)phys,
							obj,
							(unsigned long long)base);
						logged++;
					}

					if (dkms_regcmd_pair_strict_objref)
						is_candidate = !!obj;
					else
						is_candidate = !!obj || (dkms_regcmd_pair_mode == 1 && phys_ok);
					if (!is_candidate) {
						if (obj)
							rknpu_gem_object_put(&obj->base);
						continue;
					}

					if (dkms_regcmd_pair_scan &&
					    logged_candidate < dkms_regcmd_pair_log_candidate_limit) {
						LOG_ERROR(
							"DKMS: regcmd_pair CAND pair=%u off_addr=%#llx off_val=%#llx addr=%#x value=%#x phys=%#llx obj=%p base=%#llx\n",
							pairs,
							(unsigned long long)(scan_off + ((size_t)cur_off + j - sizeof(u32))),
							(unsigned long long)(scan_off + ((size_t)cur_off + j)),
							cur_addr,
							v,
							(unsigned long long)phys,
							obj,
							(unsigned long long)base);
						logged_candidate++;
					}

					candidates++;

					if (dkms_regcmd_pair_patch && domain && rknpu_dev->iommu_en) {
						if (phys_ok) {
							translatable++;
							WRITE_ONCE(*p, (u32)phys);
							patched++;
						}
					}
					if (obj)
						rknpu_gem_object_put(&obj->base);
				}
			}

			kunmap_local(vaddr);
			remaining -= chunk;
			page_index++;
			page_off = 0;
		}
	} else {
		LOG_ERROR(
			"DKMS: regcmd_pair scan skipped: cmd GEM has no CPU mapping (kv_addr=NULL, pages=NULL)\n");
	}

	LOG_ERROR(
		"DKMS: regcmd_pair summary pairs=%u candidates=%u translatable=%u patched=%u\n",
		pairs,
		candidates,
		translatable,
		patched);

	return patched;
}

#endif /* RKNPU_DKMS */

#define _REG_READ(base, offset) readl(base + (offset))
#define _REG_WRITE(base, value, offset) writel(value, base + (offset))

#define REG_READ(offset) _REG_READ(rknpu_core_base, offset)
#define REG_WRITE(value, offset) _REG_WRITE(rknpu_core_base, value, offset)

static int rknpu_wait_core_index(int core_mask)
{
	int index = 0;

	switch (core_mask) {
	case RKNPU_CORE0_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		index = 0;
		break;
	case RKNPU_CORE1_MASK:
		index = 1;
		break;
	case RKNPU_CORE2_MASK:
		index = 2;
		break;
	default:
		break;
	}

	return index;
}

static int rknpu_core_mask(int core_index)
{
	int core_mask = RKNPU_CORE_AUTO_MASK;

	switch (core_index) {
	case 0:
		core_mask = RKNPU_CORE0_MASK;
		break;
	case 1:
		core_mask = RKNPU_CORE1_MASK;
		break;
	case 2:
		core_mask = RKNPU_CORE2_MASK;
		break;
	default:
		break;
	}

	return core_mask;
}

static int rknpu_get_task_number(struct rknpu_job *job, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	int task_num = job->args->task_number;

	if (core_index >= RKNPU_MAX_CORES || core_index < 0) {
		LOG_ERROR("invalid rknpu core index: %d", core_index);
		return 0;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		if (job->use_core_num == 1 || job->use_core_num == 2)
			task_num =
				job->args->subcore_task[core_index].task_number;
		else if (job->use_core_num == 3)
			task_num = job->args->subcore_task[core_index + 2]
					   .task_number;
	}

	return task_num;
}

static void rknpu_job_free(struct rknpu_job *job)
{
#if defined(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) && !defined(RKNPU_DKMS_MISCDEV)
	struct rknpu_gem_object *task_obj = NULL;

	task_obj =
		(struct rknpu_gem_object *)(uintptr_t)job->args->task_obj_addr;
	if (task_obj)
		rknpu_gem_object_put(&task_obj->base);
#endif
	/* Note: RKNPU_DKMS_MISCDEV uses rknpu_mem_object which has no refcount.
	 * Memory lifetime is tied to the session.
	 */

	if (job->fence)
		dma_fence_put(job->fence);

	if (job->args_owner)
		kfree(job->args);

	kfree(job);
}

static int rknpu_job_cleanup(struct rknpu_job *job)
{
	rknpu_job_free(job);

	return 0;
}

static void rknpu_job_cleanup_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(work, struct rknpu_job, cleanup_work);

	rknpu_job_cleanup(job);
}

static inline struct rknpu_job *rknpu_job_alloc(struct rknpu_device *rknpu_dev,
						struct rknpu_submit *args)
{
	struct rknpu_job *job = NULL;
	int i = 0;
#if defined(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) && !defined(RKNPU_DKMS_MISCDEV)
	struct rknpu_gem_object *task_obj = NULL;
#endif

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return NULL;

	job->timestamp = ktime_get();
	job->rknpu_dev = rknpu_dev;
	job->use_core_num = (args->core_mask & RKNPU_CORE0_MASK) +
			    ((args->core_mask & RKNPU_CORE1_MASK) >> 1) +
			    ((args->core_mask & RKNPU_CORE2_MASK) >> 2);
	atomic_set(&job->run_count, job->use_core_num);
	atomic_set(&job->interrupt_count, job->use_core_num);
	job->iommu_domain_id = args->iommu_domain_id;
	for (i = 0; i < rknpu_dev->config->num_irqs; i++)
		atomic_set(&job->submit_count[i], 0);
#if defined(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) && !defined(RKNPU_DKMS_MISCDEV)
	task_obj = (struct rknpu_gem_object *)(uintptr_t)args->task_obj_addr;
	if (task_obj)
		rknpu_gem_object_get(&task_obj->base);
#endif
	/* Note: RKNPU_DKMS_MISCDEV uses rknpu_mem_object which has no refcount. */

	if (!(args->flags & RKNPU_JOB_NONBLOCK)) {
		job->args = args;
		job->args_owner = false;
		return job;
	}

	job->args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!job->args) {
		kfree(job);
		return NULL;
	}
	*job->args = *args;
	job->args_owner = true;

	INIT_WORK(&job->cleanup_work, rknpu_job_cleanup_work);

	return job;
}

static inline int rknpu_job_wait(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_task *last_task = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *entry, *q;
	void __iomem *rknpu_core_base = NULL;
	int core_index = rknpu_wait_core_index(job->args->core_mask);
	unsigned long flags;
	int wait_count = 0;
	bool continue_wait = false;
	int ret = -EINVAL;
	int i = 0;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	do {
		ret = wait_event_timeout(subcore_data->job_done_wq,
					 job->flags & RKNPU_JOB_DONE ||
						 rknpu_dev->soft_reseting,
					 msecs_to_jiffies(args->timeout));

		if (++wait_count >= 3)
			break;

		if (ret == 0) {
			int64_t elapse_time_us = 0;
			spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
			elapse_time_us = ktime_us_delta(ktime_get(),
							job->hw_commit_time);
			continue_wait =
				job->hw_commit_time == 0 ?
					true :
					(elapse_time_us < args->timeout * 1000);
			spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
			LOG_ERROR(
				"job: %p, mask: %#x, job iommu domain id: %d, dev iommu domain id: %d, wait_count: %d, continue wait: %d, commit elapse time: %lldus, wait time: %lldus, timeout: %uus\n",
				job, args->core_mask, job->iommu_domain_id,
				rknpu_dev->iommu_domain_id, wait_count,
				continue_wait,
				(job->hw_commit_time == 0 ? 0 : elapse_time_us),
				ktime_us_delta(ktime_get(), job->timestamp),
				args->timeout * 1000);
		}
	} while (ret == 0 && continue_wait);

	last_task = job->last_task;
	if (!last_task) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		for (i = 0; i < job->use_core_num; i++) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			list_for_each_entry_safe(
				entry, q, &subcore_data->todo_list, head[i]) {
				if (entry == job) {
					list_del(&job->head[i]);
					break;
				}
			}
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		LOG_ERROR("job commit failed\n");
		return ret < 0 ? ret : -EINVAL;
	}

	last_task->int_status = job->int_status[core_index];

	if (ret <= 0) {
		args->task_counter = 0;
		rknpu_core_base = rknpu_dev->base[core_index];
		if (args->flags & RKNPU_JOB_PC) {
			uint32_t task_status = REG_READ(
				rknpu_dev->config->pc_task_status_offset);
			args->task_counter =
				(task_status &
				 rknpu_dev->config->pc_task_number_mask);
		}

		LOG_ERROR(
			"failed to wait job, task counter: %d, flags: %#x, ret = %d, elapsed time: %lldus\n",
			args->task_counter, args->flags, ret,
			ktime_us_delta(ktime_get(), job->timestamp));

		return ret < 0 ? ret : -ETIMEDOUT;
	}

	if (!(job->flags & RKNPU_JOB_DONE))
		return -EINVAL;

	args->task_counter = args->task_number;
	args->hw_elapse_time = job->hw_elapse_time;

	return 0;
}

static inline int rknpu_job_subcore_commit_pc(struct rknpu_job *job,
					      int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	void __iomem *task_obj = NULL;
	u32 pc_dma_base_addr = (u32)args->task_base_addr;
#ifdef RKNPU_DKMS
	u64 task_iova_start = 0;
	u64 task_iova_end = 0;
#endif

#ifdef RKNPU_DKMS_MISCDEV
	/* DKMS MISCDEV path: task_obj_addr is an rknpu_mem_object pointer */
	{
		struct rknpu_mem_object *mem_obj =
			(struct rknpu_mem_object *)(uintptr_t)args->task_obj_addr;
		if (mem_obj) {
			task_obj = mem_obj->kv_addr;
			task_iova_start = (u64)mem_obj->dma_addr;
			task_iova_end = task_iova_start + (u64)mem_obj->size;
			LOG_ERROR(
				"DKMS: task MEM: kv_addr=%p dma_addr=%#llx size=%#lx\n",
				mem_obj->kv_addr,
				(unsigned long long)mem_obj->dma_addr,
				(unsigned long)mem_obj->size);
		}
	}
#elif defined(CONFIG_ROCKCHIP_RKNPU_DRM_GEM)
	{
		struct rknpu_gem_object *gem_obj =
			(struct rknpu_gem_object *)(uintptr_t)args->task_obj_addr;
		if (gem_obj) {
			task_obj = gem_obj->kv_addr;

#ifdef RKNPU_DKMS
			task_iova_start = (u64)gem_obj->dma_addr;
			if (gem_obj->iova_size)
				task_iova_end = task_iova_start + (u64)gem_obj->iova_size;
			else
				task_iova_end = task_iova_start + (u64)gem_obj->size;
			LOG_ERROR(
				"DKMS: task GEM: kv_addr=%p dma_addr=%#llx iova_start=%#llx iova_size=%#lx size=%#lx\n",
				gem_obj->kv_addr,
				(unsigned long long)gem_obj->dma_addr,
				(unsigned long long)gem_obj->iova_start,
				(unsigned long)gem_obj->iova_size,
				(unsigned long)gem_obj->size);
#endif
		}
	}
#elif defined(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP)
	{
		struct rknpu_mem_object *mem_obj =
			(struct rknpu_mem_object *)(uintptr_t)args->task_obj_addr;
		if (mem_obj)
			task_obj = mem_obj->kv_addr;
	}
#endif
	struct rknpu_task *task_base = NULL;
	struct rknpu_task *first_task = NULL;
	struct rknpu_task *last_task = NULL;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	u32 pc_data_addr = 0;
#ifdef RKNPU_DKMS
	bool dkms_pc_data_is_offset = false;
	struct rknpu_gem_object *cmd_gem = NULL;
	dma_addr_t cmd_gem_base = 0;
	u32 regcmd_patched = 0;
#endif
	int task_start = args->task_start;
	int task_end;
	int task_number = args->task_number;
	int task_pp_en = args->flags & RKNPU_JOB_PINGPONG ? 1 : 0;
	int pc_data_amount_scale = rknpu_dev->config->pc_data_amount_scale;
	int pc_task_number_bits = rknpu_dev->config->pc_task_number_bits;
	int i = 0;
	int submit_index = atomic_read(&job->submit_count[core_index]);
	int max_submit_number = rknpu_dev->config->max_submit_number;
	uint32_t pc_data_amount_reg = 0;
	uint32_t pc_task_control = 0;
	unsigned long flags;

	if (!task_obj) {
		job->ret = -EINVAL;
		return job->ret;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (i == core_index) {
				REG_WRITE((0xe + 0x10000000 * i), 0x1004);
				REG_WRITE((0xe + 0x10000000 * i), 0x3004);
			}
		}

		switch (job->use_core_num) {
		case 1:
		case 2:
			task_start = args->subcore_task[core_index].task_start;
			task_number =
				args->subcore_task[core_index].task_number;
			break;
		case 3:
			task_start =
				args->subcore_task[core_index + 2].task_start;
			task_number =
				args->subcore_task[core_index + 2].task_number;
			break;
		default:
			LOG_ERROR("Unknown use core num %d\n",
				  job->use_core_num);
			break;
		}
	}

	task_start = task_start + submit_index * max_submit_number;
	task_number = task_number - submit_index * max_submit_number;
	task_number = task_number > max_submit_number ? max_submit_number :
							task_number;
	task_end = task_start + task_number - 1;

	task_base = (struct rknpu_task *)task_obj;

	first_task = &task_base[task_start];
	last_task = &task_base[task_end];
	pc_data_addr = (u32)first_task->regcmd_addr;

#ifdef RKNPU_DKMS
	if (args->task_base_addr == 0) {
		cmd_gem = rknpu_dkms_find_gem_obj_by_addr(
			(dma_addr_t)first_task->regcmd_addr, &cmd_gem_base);
		dma_addr_t inferred = cmd_gem_base;
		phys_addr_t regcmd_phys = 0;
		phys_addr_t inferred_phys = 0;
		if (rknpu_dev->iommu_en) {
			struct iommu_domain *domain =
				iommu_get_domain_for_dev(rknpu_dev->dev);
			phys_addr_t phys = 0;
			phys_addr_t base_phys = 0;

			if (domain) {
				phys = iommu_iova_to_phys(
					domain,
					(dma_addr_t)first_task->regcmd_addr);
				if (inferred)
					base_phys =
						iommu_iova_to_phys(domain, inferred);
			}
			regcmd_phys = phys;
			inferred_phys = base_phys;
			LOG_ERROR(
				"DKMS: iommu_iova_to_phys regcmd=%#llx -> phys=%#llx base=%#llx -> phys=%#llx\n",
				(unsigned long long)first_task->regcmd_addr,
				(unsigned long long)phys,
				(unsigned long long)inferred,
				(unsigned long long)base_phys);
		}
		if (dkms_pc_addr_mode == 2 && inferred) {
			pc_dma_base_addr = (u32)inferred;
			pc_data_addr = (u32)((dma_addr_t)first_task->regcmd_addr - inferred);
			dkms_pc_data_is_offset = true;
			LOG_ERROR(
				"DKMS: pc addr mode=base+offset pc_dma_base_addr=%#x pc_data_addr=%#x from regcmd_addr=%#llx\n",
				pc_dma_base_addr,
				pc_data_addr,
				(unsigned long long)first_task->regcmd_addr);
		} else {
			pc_data_addr = (u32)first_task->regcmd_addr;
			dkms_pc_data_is_offset = false;
			LOG_ERROR(
				"DKMS: pc addr mode=absolute pc_data_addr=%#x from regcmd_addr=%#llx\n",
				pc_data_addr,
				(unsigned long long)first_task->regcmd_addr);
		}

		if (cmd_gem) {
			u8 dump[64];
			size_t dump_len = sizeof(dump);
			size_t copied = 0;
			dma_addr_t off =
				(dma_addr_t)first_task->regcmd_addr - cmd_gem_base;
			unsigned long page_index = (unsigned long)(off >> PAGE_SHIFT);
			unsigned long page_off = (unsigned long)(off & (PAGE_SIZE - 1));

			memset(dump, 0, sizeof(dump));

			if (cmd_gem->kv_addr) {
				u8 *p = (u8 *)cmd_gem->kv_addr + off;
				memcpy(dump, p, dump_len);
				copied = dump_len;
			} else if (cmd_gem->pages && page_index < cmd_gem->num_pages) {
				while (copied < dump_len &&
				       page_index < cmd_gem->num_pages) {
					size_t n = dump_len - copied;
					void *v = kmap_local_page(
						cmd_gem->pages[page_index]);
					if (n > (PAGE_SIZE - page_off))
						n = PAGE_SIZE - page_off;
					memcpy(&dump[copied], (u8 *)v + page_off, n);
					kunmap_local(v);
					copied += n;
					page_index++;
					page_off = 0;
				}
			}

			if (copied) {
				LOG_ERROR(
					"DKMS: cmd hexdump @%#llx (+%#llx): %*phN\n",
					(unsigned long long)first_task->regcmd_addr,
					(unsigned long long)off, 64, dump);
			} else {
				LOG_ERROR(
					"DKMS: cmd hexdump unavailable (no kv_addr and pages missing)\n");
			}

			{
				phys_addr_t phys = 0;
				bool ok = rknpu_dkms_cmd_phys_from_off(cmd_gem, off, &phys);
				if (ok)
					LOG_ERROR(
						"DKMS: cmd phys (from sgt) off=%#llx -> phys=%#llx\n",
						(unsigned long long)off,
						(unsigned long long)phys);

				if (dkms_pc_use_cmd_sg_phys && ok && (phys >> 32) == 0) {
					pc_dma_base_addr = 0;
					pc_data_addr = (u32)phys;
					dkms_pc_data_is_offset = false;
					LOG_ERROR(
						"DKMS: forcing PC addr from cmd phys pc_data_addr=%#x (base cleared)\n",
						pc_data_addr);
				}
			}

			if (dkms_dump_regcmd_words && cmd_gem->kv_addr) {
				u32 *w32 = (u32 *)((u8 *)cmd_gem->kv_addr + off);
				u64 *w64 = (u64 *)((u8 *)cmd_gem->kv_addr + off);
				LOG_ERROR(
					"DKMS: regcmd words (u32 x8): %08x %08x %08x %08x %08x %08x %08x %08x\n",
					w32[0], w32[1], w32[2], w32[3],
					w32[4], w32[5], w32[6], w32[7]);
				LOG_ERROR(
					"DKMS: regcmd words (u64 x4): %016llx %016llx %016llx %016llx\n",
					(unsigned long long)w64[0],
					(unsigned long long)w64[1],
					(unsigned long long)w64[2],
					(unsigned long long)w64[3]);
			}

			if (dkms_regcmd_pair_scan || dkms_regcmd_pair_patch) {
				dma_addr_t pair_off = 0;
				size_t pair_len = dkms_patch_cmd_scan_bytes;

				if (dkms_regcmd_pair_start_from_zero)
					pair_off = 0;
				else
					pair_off = off;

				if (pair_off < cmd_gem->size) {
					size_t rem = cmd_gem->size - pair_off;
					if (pair_len > rem)
						pair_len = rem;
					pair_len &= ~((size_t)8 - 1);
					regcmd_patched = rknpu_dkms_scan_regcmd_pairs(
						rknpu_dev,
						cmd_gem,
						cmd_gem_base,
						(dma_addr_t)first_task->regcmd_addr,
						pair_off,
						pair_len);
				}
			}
		}

		if (dkms_pc_use_iommu_phys && regcmd_phys && (regcmd_phys >> 32) == 0) {
			if (dkms_pc_data_is_offset && inferred_phys &&
			    (inferred_phys >> 32) == 0 && regcmd_phys >= inferred_phys) {
				pc_dma_base_addr = (u32)inferred_phys;
				pc_data_addr = (u32)(regcmd_phys - inferred_phys);
				LOG_ERROR(
					"DKMS: forcing PC addr to iommu phys base=%#x off=%#x\n",
					pc_dma_base_addr, pc_data_addr);
			} else {
				pc_dma_base_addr = 0;
				pc_data_addr = (u32)regcmd_phys;
				dkms_pc_data_is_offset = false;
				LOG_ERROR(
					"DKMS: forcing PC addr to iommu phys pc_data_addr=%#x (base cleared)\n",
					pc_data_addr);
			}
		}

		if (dkms_pc_dma_base_from_mmio) {
			struct platform_device *pdev = to_platform_device(rknpu_dev->dev);
			struct resource *res =
				platform_get_resource(pdev, IORESOURCE_MEM, core_index);
			if (res) {
				pc_dma_base_addr = (u32)res->start;
				LOG_ERROR(
					"DKMS: forcing PC_DMA_BASE_ADDR from MMIO base=%#x\n",
					pc_dma_base_addr);
			}
		}

		if (cmd_gem && cmd_gem_base) {
			dma_addr_t off;
			size_t scan_len = dkms_patch_cmd_scan_bytes;

			if (dkms_patch_cmd_start_from_zero)
				off = 0;
			else
				off = (dma_addr_t)first_task->regcmd_addr - cmd_gem_base;

			if (off < cmd_gem->size) {
				if (scan_len > (cmd_gem->size - off))
					scan_len = cmd_gem->size - off;
				rknpu_dkms_patch_cmd_buf_iova_to_phys(
					rknpu_dev, cmd_gem, cmd_gem_base,
					(dma_addr_t)first_task->regcmd_addr, off,
					scan_len);

				if ((dkms_force_cmd_dma_sync || dkms_patch_cmd_iova_to_phys ||
				     regcmd_patched) && cmd_gem->size) {
					if (!(cmd_gem->flags & RKNPU_MEM_NON_CONTIGUOUS)) {
						dma_sync_single_range_for_device(
							rknpu_dev->dev, cmd_gem->dma_addr,
							0, cmd_gem->size, DMA_TO_DEVICE);
					} else if (cmd_gem->sgt) {
						dma_sync_sg_for_device(
							rknpu_dev->dev, cmd_gem->sgt->sgl,
							cmd_gem->sgt->nents,
							DMA_TO_DEVICE);
					}
				}
			}
		}
	}
#endif

#ifdef RKNPU_DKMS
	LOG_ERROR(
		"DKMS: commit_pc core=%d flags=%#x task_start=%u task_number=%u task_obj_addr=%#llx task_base_addr=%#llx pc_dma_base_addr=%#x first{enable_mask=%#x int_mask=%#x regcfg_amount=%u regcfg_offset=%u regcmd_addr=%#llx} last{int_mask=%#x regcmd_addr=%#llx}\n",
		core_index, args->flags, task_start, task_number,
		(unsigned long long)args->task_obj_addr,
		(unsigned long long)args->task_base_addr, pc_dma_base_addr,
		first_task->enable_mask,
		first_task->int_mask, first_task->regcfg_amount,
		first_task->regcfg_offset,
		(unsigned long long)first_task->regcmd_addr, last_task->int_mask,
		(unsigned long long)last_task->regcmd_addr);
	if (pc_dma_base_addr) {
		LOG_ERROR(
			"DKMS: regcmd deltas: first=%lld last=%lld (regcmd - pc_dma_base_addr)\n",
			(long long)((s64)first_task->regcmd_addr - (s64)pc_dma_base_addr),
			(long long)((s64)last_task->regcmd_addr - (s64)pc_dma_base_addr));
	}
	if (task_iova_start && task_iova_end) {
		bool first_in = ((u64)first_task->regcmd_addr >= task_iova_start) &&
				((u64)first_task->regcmd_addr < task_iova_end);
		bool last_in = ((u64)last_task->regcmd_addr >= task_iova_start) &&
			       ((u64)last_task->regcmd_addr < task_iova_end);
		LOG_ERROR(
			"DKMS: task IOVA range [%#llx..%#llx) regcmd_in_range: first=%d last=%d\n",
			(unsigned long long)task_iova_start,
			(unsigned long long)task_iova_end, first_in, last_in);
	}
	LOG_ERROR(
		"DKMS: pre regs core=%d PC_OP_EN=%#x PC_DATA_ADDR=%#x PC_DATA_AMOUNT=%#x PC_TASK_CONTROL=%#x PC_DMA_BASE_ADDR=%#x INT_MASK=%#x INT_STATUS=%#x INT_RAW_STATUS=%#x\n",
		core_index, REG_READ(RKNPU_OFFSET_PC_OP_EN),
		REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
		REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
		REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
		REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
		REG_READ(RKNPU_OFFSET_INT_MASK),
		REG_READ(RKNPU_OFFSET_INT_STATUS),
		REG_READ(RKNPU_OFFSET_INT_RAW_STATUS));
#endif

	if (rknpu_dev->config->pc_dma_ctrl) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		REG_WRITE(pc_data_addr, RKNPU_OFFSET_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		REG_WRITE(pc_data_addr, RKNPU_OFFSET_PC_DATA_ADDR);
	}

	pc_data_amount_reg =
		(first_task->regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT +
		 pc_data_amount_scale - 1) /
				 pc_data_amount_scale -
			 1;
	REG_WRITE(pc_data_amount_reg, RKNPU_OFFSET_PC_DATA_AMOUNT);

#ifdef RKNPU_DKMS
	LOG_ERROR("DKMS: wrote PC_DATA_AMOUNT=%#x readback=%#x\n",
		  pc_data_amount_reg, REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT));
#endif

	{
		uint32_t int_mask = last_task->int_mask;
		uint32_t int_clear = first_task->int_mask;
		if (dkms_force_int_mask_bit16) {
			int_mask |= BIT(16);
			int_clear |= BIT(16);
		}
		REG_WRITE(int_mask, RKNPU_OFFSET_INT_MASK);

		job->int_mask[core_index] = int_mask;

		#ifdef RKNPU_DKMS
		LOG_ERROR("DKMS: wrote INT_MASK=%#x readback=%#x\n",
			  int_mask, REG_READ(RKNPU_OFFSET_INT_MASK));
		#endif

		#ifdef RKNPU_DKMS
		if (dkms_clear_int_all)
			REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		else
			REG_WRITE(int_clear, RKNPU_OFFSET_INT_CLEAR);
		LOG_ERROR("DKMS: wrote INT_CLEAR=%#x readback=%#x\n",
			  dkms_clear_int_all ? RKNPU_INT_CLEAR : int_clear,
			  REG_READ(RKNPU_OFFSET_INT_CLEAR));
		#else
		REG_WRITE(int_clear, RKNPU_OFFSET_INT_CLEAR);
		#endif
	}

	pc_task_control =
		(((dkms_pc_task_mode | task_pp_en) & 0x7) << pc_task_number_bits) |
		task_number;
	REG_WRITE(pc_dma_base_addr, RKNPU_OFFSET_PC_DMA_BASE_ADDR);
	REG_WRITE(pc_task_control, RKNPU_OFFSET_PC_TASK_CONTROL);

#ifdef RKNPU_DKMS
	{
		uint32_t rb = REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL);
		uint32_t rb_mode = rb >> pc_task_number_bits;
		LOG_ERROR(
			"DKMS: wrote PC_TASK_CONTROL=%#x readback=%#x (mode=%#x task=%u)\n",
			pc_task_control, rb, rb_mode,
			rb & rknpu_dev->config->pc_task_number_mask);

		if (rb == 0 ||
		    ((rb & rknpu_dev->config->pc_task_number_mask) !=
		     (pc_task_control & rknpu_dev->config->pc_task_number_mask))) {
			uint32_t alt = (0x1U << pc_task_number_bits) | task_number;
			REG_WRITE(alt, RKNPU_OFFSET_PC_TASK_CONTROL);
			rb = REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL);
			rb_mode = rb >> pc_task_number_bits;
			LOG_ERROR(
				"DKMS: retry wrote PC_TASK_CONTROL=%#x readback=%#x (mode=%#x task=%u)\n",
				alt, rb, rb_mode,
				rb & rknpu_dev->config->pc_task_number_mask);
		}
	}
#endif

	job->first_task = first_task;
	job->last_task = last_task;

#ifdef RKNPU_DKMS
	LOG_ERROR(
		"DKMS: pre PC_OP_EN readback PC_DATA_ADDR=%#x PC_DMA_BASE_ADDR=%#x\n",
		REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
		REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR));
	if (dkms_commit_force_iommu_attach && core_index == 0)
		rknpu_dkms_force_iommu_attach(rknpu_dev, "\t");
	if (dkms_commit_set_iommu_autogating_bit31 && core_index == 0)
		rknpu_dkms_set_iommu_autogating_bit31(rknpu_dev, "\t");
	if (dkms_commit_dump_iommu && core_index == 0)
		rknpu_dkms_dump_iommu(rknpu_dev, "\t");
#endif

	REG_WRITE(0x1, RKNPU_OFFSET_PC_OP_EN);

#ifdef RKNPU_DKMS
	if (dkms_write_enable_mask) {
		REG_WRITE(first_task->enable_mask, RKNPU_OFFSET_ENABLE_MASK);
		LOG_ERROR("DKMS: wrote ENABLE_MASK=%#x\n", first_task->enable_mask);
	}
#endif

#ifdef RKNPU_DKMS
	LOG_ERROR(
		"DKMS: after PC_OP_EN regs core=%d PC_OP_EN=%#x PC_DATA_ADDR=%#x PC_DATA_AMOUNT=%#x PC_TASK_CONTROL=%#x PC_DMA_BASE_ADDR=%#x INT_MASK=%#x INT_STATUS=%#x INT_RAW_STATUS=%#x\n",
		core_index, REG_READ(RKNPU_OFFSET_PC_OP_EN),
		REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
		REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
		REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
		REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
		REG_READ(RKNPU_OFFSET_INT_MASK),
		REG_READ(RKNPU_OFFSET_INT_STATUS),
		REG_READ(RKNPU_OFFSET_INT_RAW_STATUS));
	if (dkms_pulse_pc_op_en)
		REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);
#endif

#ifndef RKNPU_DKMS
	REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);
#endif

#ifdef RKNPU_DKMS
	LOG_ERROR(
		"DKMS: post regs core=%d PC_OP_EN=%#x PC_DATA_ADDR=%#x PC_DATA_AMOUNT=%#x PC_TASK_CONTROL=%#x PC_DMA_BASE_ADDR=%#x INT_MASK=%#x INT_STATUS=%#x INT_RAW_STATUS=%#x\n",
		core_index, REG_READ(RKNPU_OFFSET_PC_OP_EN),
		REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
		REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
		REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
		REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
		REG_READ(RKNPU_OFFSET_INT_MASK),
		REG_READ(RKNPU_OFFSET_INT_STATUS),
		REG_READ(RKNPU_OFFSET_INT_RAW_STATUS));
	if (cmd_gem)
		rknpu_gem_object_put(&cmd_gem->base);
#endif

	return 0;
}

static inline int rknpu_job_subcore_commit(struct rknpu_job *job,
					   int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	unsigned long flags;

	// switch to slave mode
	if (rknpu_dev->config->pc_dma_ctrl) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);
	}

	if (!(args->flags & RKNPU_JOB_PC)) {
		job->ret = -EINVAL;
		return job->ret;
	}

	return rknpu_job_subcore_commit_pc(job, core_index);
}

static void rknpu_job_commit(struct rknpu_job *job)
{
	switch (job->args->core_mask) {
	case RKNPU_CORE0_MASK:
		rknpu_job_subcore_commit(job, 0);
		break;
	case RKNPU_CORE1_MASK:
		rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE2_MASK:
		rknpu_job_subcore_commit(job, 2);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
		rknpu_job_subcore_commit(job, 0);
		rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		rknpu_job_subcore_commit(job, 0);
		rknpu_job_subcore_commit(job, 1);
		rknpu_job_subcore_commit(job, 2);
		break;
	default:
		LOG_ERROR("Unknown core mask: %d\n", job->args->core_mask);
		break;
	}
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index)
{
	struct rknpu_job *job = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;

	if (rknpu_dev->soft_reseting)
		return;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

	if (subcore_data->job || list_empty(&subcore_data->todo_list)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}

	job = list_first_entry(&subcore_data->todo_list, struct rknpu_job,
			       head[core_index]);

	list_del_init(&job->head[core_index]);
	subcore_data->job = job;
	job->hw_commit_time = ktime_get();
	job->hw_recoder_time = job->hw_commit_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->run_count))
		rknpu_job_commit(job);
}

static void rknpu_job_done(struct rknpu_job *job, int ret, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	ktime_t now;
	unsigned long flags;
	int max_submit_number = rknpu_dev->config->max_submit_number;

	if (atomic_inc_return(&job->submit_count[core_index]) <
	    (rknpu_get_task_number(job, core_index) + max_submit_number - 1) /
		    max_submit_number) {
		rknpu_job_subcore_commit(job, core_index);
		return;
	}

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	subcore_data->job = NULL;
	subcore_data->task_num -= rknpu_get_task_number(job, core_index);
	now = ktime_get();
	job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
	subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->interrupt_count)) {
		int use_core_num = job->use_core_num;

		rknpu_iommu_domain_put(rknpu_dev);

		job->flags |= RKNPU_JOB_DONE;
		job->ret = ret;

		if (job->fence)
			dma_fence_signal(job->fence);

		if (job->flags & RKNPU_JOB_ASYNC)
			schedule_work(&job->cleanup_work);

		if (use_core_num > 1)
			wake_up(&(&rknpu_dev->subcore_datas[0])->job_done_wq);
		else
			wake_up(&subcore_data->job_done_wq);
	}

	rknpu_job_next(rknpu_dev, core_index);
}

static int rknpu_schedule_core_index(struct rknpu_device *rknpu_dev)
{
	int core_num = rknpu_dev->config->num_irqs;
	int task_num = rknpu_dev->subcore_datas[0].task_num;
	int core_index = 0;
	int i = 0;

	for (i = 1; i < core_num; i++) {
		if (task_num > rknpu_dev->subcore_datas[i].task_num) {
			core_index = i;
			task_num = rknpu_dev->subcore_datas[i].task_num;
		}
	}

	return core_index;
}

static void rknpu_job_schedule(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	int i = 0, core_index = 0;
	unsigned long flags;

	if (job->args->core_mask == RKNPU_CORE_AUTO_MASK) {
		core_index = rknpu_schedule_core_index(rknpu_dev);
		job->args->core_mask = rknpu_core_mask(core_index);
		job->use_core_num = 1;
		atomic_set(&job->run_count, job->use_core_num);
		atomic_set(&job->interrupt_count, job->use_core_num);
	}

	if (rknpu_iommu_domain_get_and_switch(rknpu_dev, job->iommu_domain_id)) {
		job->ret = -EINVAL;
		return;
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			list_add_tail(&job->head[i], &subcore_data->todo_list);
			subcore_data->task_num += rknpu_get_task_number(job, i);
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i))
			rknpu_job_next(rknpu_dev, i);
	}
}

static void rknpu_job_abort(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;
	int i = 0;

	rknpu_iommu_domain_put(rknpu_dev);

	msleep(100);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			if (job == subcore_data->job && !job->irq_entry[i]) {
				subcore_data->job = NULL;
				subcore_data->task_num -=
					rknpu_get_task_number(job, i);
			}
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (job->ret == -ETIMEDOUT) {
		LOG_ERROR("job timeout, flags: %#x:\n", job->flags);
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (job->args->core_mask & rknpu_core_mask(i)) {
				void __iomem *rknpu_core_base =
					rknpu_dev->base[i];
				struct rknpu_task *ft = job->first_task;
				struct rknpu_task *lt = job->last_task;
				uint32_t task_status_raw =
					REG_READ(rknpu_dev->config->pc_task_status_offset);
				LOG_ERROR(
					"\tcore %d irq status: %#x, raw status: %#x, require mask: %#x, task counter: %#x, elapsed time: %lldus\n",
					i, REG_READ(RKNPU_OFFSET_INT_STATUS),
					REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
					job->int_mask[i],
					(task_status_raw &
					 rknpu_dev->config->pc_task_number_mask),
					ktime_us_delta(ktime_get(),
						       job->timestamp));

#ifdef RKNPU_DKMS
				LOG_ERROR(
					"\tcore %d pc_task_status_offset=%#x raw=%#x masked_counter=%#x\n",
					i, rknpu_dev->config->pc_task_status_offset,
					task_status_raw,
					task_status_raw &
						rknpu_dev->config->pc_task_number_mask);
				{
					uint32_t off = rknpu_dev->config->pc_task_status_offset;
					uint32_t a;
					for (a = off >= 0x10 ? (off - 0x10) : 0; a <= off + 0x10;
					     a += 4)
						LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, a,
							  REG_READ(a));
					LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, 0x10,
						  REG_READ(0x10));
					LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, 0x1004,
						  REG_READ(0x1004));
					LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, 0x1024,
						  REG_READ(0x1024));
					if (dkms_timeout_dump_ext) {
						LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, 0xf008,
							  REG_READ(0xf008));
						LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, 0x3004,
							  REG_READ(0x3004));
						for (a = 0x1000; a <= 0x1040; a += 4)
							LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, a,
								  REG_READ(a));
						for (a = 0x3000; a <= 0x3040; a += 4)
							LOG_ERROR("\tcore %d reg[%#x]=%#x\n", i, a,
								  REG_READ(a));
					}

					if (dkms_timeout_dump_iommu && i == 0)
						rknpu_dkms_dump_iommu(rknpu_dev, "\t");
				}
#endif
				LOG_ERROR(
					"\tcore %d regs: PC_OP_EN=%#x PC_DATA_ADDR=%#x PC_DATA_AMOUNT=%#x PC_TASK_CONTROL=%#x PC_DMA_BASE_ADDR=%#x INT_MASK=%#x INT_CLEAR=%#x\n",
					i, REG_READ(RKNPU_OFFSET_PC_OP_EN),
					REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
					REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
					REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
					REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
					REG_READ(RKNPU_OFFSET_INT_MASK),
					REG_READ(RKNPU_OFFSET_INT_CLEAR));
				if (ft && lt) {
					LOG_ERROR(
						"\tcore %d tasks: first{enable_mask=%#x int_mask=%#x regcfg_amount=%u regcfg_offset=%u regcmd_addr=%#llx} last{int_mask=%#x regcmd_addr=%#llx}\n",
						i, ft->enable_mask, ft->int_mask,
						ft->regcfg_amount, ft->regcfg_offset,
						(unsigned long long)ft->regcmd_addr,
						lt->int_mask,
						(unsigned long long)lt->regcmd_addr);
				}
			}
		}
		rknpu_soft_reset(rknpu_dev);
	} else {
		LOG_ERROR(
			"job abort, flags: %#x, ret: %d, elapsed time: %lldus\n",
			job->flags, job->ret,
			ktime_us_delta(ktime_get(), job->timestamp));
	}

	rknpu_job_cleanup(job);
}

static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
	uint32_t fuzz_status = 0;

	if ((status & 0x3) != 0)
		fuzz_status |= 0x3;

	if ((status & 0xc) != 0)
		fuzz_status |= 0xc;

	if ((status & 0x30) != 0)
		fuzz_status |= 0x30;

	if ((status & 0xc0) != 0)
		fuzz_status |= 0xc0;

	if ((status & 0x300) != 0)
		fuzz_status |= 0x300;

	if ((status & 0xc00) != 0)
		fuzz_status |= 0xc00;

#ifdef RKNPU_DKMS
	if (dkms_force_int_mask_bit16 && (status & BIT(16)) != 0)
		fuzz_status |= BIT(16);
#endif

	return fuzz_status;
}

static inline irqreturn_t rknpu_irq_handler(int irq, void *data, int core_index)
{
	struct rknpu_device *rknpu_dev = data;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	uint32_t status = 0;
	unsigned long flags;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	job = subcore_data->job;
	if (!job) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		rknpu_job_next(rknpu_dev, core_index);
		return IRQ_HANDLED;
	}
	job->irq_entry[core_index] = true;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	status = REG_READ(RKNPU_OFFSET_INT_STATUS);

	job->int_status[core_index] = status;

	if (rknpu_fuzz_status(status) != job->int_mask[core_index]) {
		LOG_ERROR(
			"invalid irq status: %#x, raw status: %#x, require mask: %#x, task counter: %#x\n",
			status, REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
			job->int_mask[core_index],
			(REG_READ(rknpu_dev->config->pc_task_status_offset) &
			 rknpu_dev->config->pc_task_number_mask));
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		return IRQ_HANDLED;
	}

	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);

	rknpu_job_done(job, 0, core_index);

	return IRQ_HANDLED;
}

irqreturn_t rknpu_core0_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 0);
}

irqreturn_t rknpu_core1_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 1);
}

irqreturn_t rknpu_core2_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 2);
}

static void rknpu_job_timeout_clean(struct rknpu_device *rknpu_dev,
				    int core_mask)
{
	struct rknpu_job *job = NULL;
	unsigned long flags;
	struct rknpu_subcore_data *subcore_data = NULL;
	int i = 0;

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			job = subcore_data->job;
			if (job &&
			    ktime_us_delta(ktime_get(), job->timestamp) >=
				    job->args->timeout) {
				rknpu_soft_reset(rknpu_dev);

				spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
				subcore_data->job = NULL;
				spin_unlock_irqrestore(&rknpu_dev->irq_lock,
						       flags);

				do {
					schedule_work(&job->cleanup_work);

					spin_lock_irqsave(&rknpu_dev->irq_lock,
							  flags);

					if (!list_empty(
						    &subcore_data->todo_list)) {
						job = list_first_entry(
							&subcore_data->todo_list,
							struct rknpu_job,
							head[i]);
						list_del_init(&job->head[i]);
					} else {
						job = NULL;
					}

					spin_unlock_irqrestore(
						&rknpu_dev->irq_lock, flags);
				} while (job);
			}
		}
	}
}

static int __maybe_unused rknpu_submit(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args)
{
	struct rknpu_job *job = NULL;
	int ret = -EINVAL;

#ifdef RKNPU_DKMS
	if (!allow_unsafe_no_power_domains &&
	    !rknpu_dev->iommu_en &&
	    !of_find_property(rknpu_dev->dev->of_node, "power-domains", NULL)) {
		LOG_DEV_ERROR(
			rknpu_dev->dev,
			"refusing job submission: DT has no power-domains; NPU HW likely not powered in safe-mode (would SError)\n");
		return -EOPNOTSUPP;
	}
#endif

	if (args->task_number == 0) {
		LOG_ERROR("invalid rknpu task number!\n");
		return -EINVAL;
	}

	if (args->core_mask > rknpu_dev->config->core_mask) {
		LOG_ERROR("invalid rknpu core mask: %#x", args->core_mask);
		return -EINVAL;
	}

	job = rknpu_job_alloc(rknpu_dev, args);
	if (!job) {
		LOG_ERROR("failed to allocate rknpu job!\n");
		return -ENOMEM;
	}

	if (args->flags & RKNPU_JOB_FENCE_IN) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
		struct dma_fence *in_fence;

		in_fence = sync_file_get_fence(args->fence_fd);

		if (!in_fence) {
			LOG_ERROR("invalid fence in fd, fd: %d\n",
				  args->fence_fd);
			return -EINVAL;
		}
		args->fence_fd = -1;

		/*
		 * Wait if the fence is from a foreign context, or if the fence
		 * array contains any fence from a foreign context.
		 */
		ret = 0;
		if (!dma_fence_match_context(in_fence,
					     rknpu_dev->fence_ctx->context))
			ret = dma_fence_wait_timeout(in_fence, true,
						     args->timeout);
		dma_fence_put(in_fence);
		if (ret < 0) {
			if (ret != -ERESTARTSYS)
				LOG_ERROR("Error (%d) waiting for fence!\n",
					  ret);

			return ret;
		}
#else
		LOG_ERROR(
			"failed to use rknpu fence, please enable rknpu fence config!\n");
		rknpu_job_free(job);
		return -EINVAL;
#endif
	}

	if (args->flags & RKNPU_JOB_FENCE_OUT) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
		ret = rknpu_fence_alloc(job);
		if (ret) {
			rknpu_job_free(job);
			return ret;
		}
		job->args->fence_fd = rknpu_fence_get_fd(job);
		args->fence_fd = job->args->fence_fd;
#else
		LOG_ERROR(
			"failed to use rknpu fence, please enable rknpu fence config!\n");
		rknpu_job_free(job);
		return -EINVAL;
#endif
	}

	if (args->flags & RKNPU_JOB_NONBLOCK) {
		job->flags |= RKNPU_JOB_ASYNC;
		rknpu_job_timeout_clean(rknpu_dev, job->args->core_mask);
		rknpu_job_schedule(job);
		ret = job->ret;
		if (ret) {
			rknpu_job_abort(job);
			return ret;
		}
	} else {
		rknpu_job_schedule(job);
		if (args->flags & RKNPU_JOB_PC)
			job->ret = rknpu_job_wait(job);

		args->task_counter = job->args->task_counter;
		ret = job->ret;
		if (!ret)
			rknpu_job_cleanup(job);
		else
			rknpu_job_abort(job);
	}

	return ret;
}

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
int rknpu_submit_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev->dev);
	struct rknpu_submit *args = data;

	return rknpu_submit(rknpu_dev, args);
}
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
int rknpu_submit_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	struct rknpu_submit args;
	int ret = -EINVAL;

	if (unlikely(copy_from_user(&args, (struct rknpu_submit *)data,
				    sizeof(struct rknpu_submit)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	ret = rknpu_submit(rknpu_dev, &args);

	if (unlikely(copy_to_user((struct rknpu_submit *)data, &args,
				  sizeof(struct rknpu_submit)))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	return ret;
}
#endif

#ifdef RKNPU_DKMS_MISCDEV
int rknpu_submit_misc_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	struct rknpu_submit args;
	struct rknpu_mem_object *mem_obj = NULL;
	int ret = -EINVAL;

	if (unlikely(copy_from_user(&args, (struct rknpu_submit *)data,
			    sizeof(struct rknpu_submit)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	/* Validate task_obj_addr before passing to rknpu_submit.
	 * Userspace passes back the kernel pointer it received from MEM_CREATE.
	 * We must verify it belongs to this session to prevent arbitrary
	 * kernel pointer dereference.
	 */
	if (args.task_obj_addr) {
		mem_obj = rknpu_mem_find_by_obj_addr(rknpu_dev, file,
						     args.task_obj_addr);
		if (!mem_obj) {
			LOG_ERROR("%s: invalid task_obj_addr %#llx\n",
				  __func__, args.task_obj_addr);
			return -EINVAL;
		}
		/* mem_obj is validated - args.task_obj_addr is safe to use */
	}

	ret = rknpu_submit(rknpu_dev, &args);

	if (unlikely(copy_to_user((struct rknpu_submit *)data, &args,
			  sizeof(struct rknpu_submit)))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	return ret;
}
#endif

int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];

	if (version == NULL)
		return -EINVAL;

#ifdef RKNPU_DKMS
	*version = 0;
	return 0;
#endif

	*version = REG_READ(RKNPU_OFFSET_VERSION) +
		   (REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffff);

	return 0;
}

int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, uint32_t *priority,
			  uint32_t *expect, uint32_t *tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);

	if (priority != NULL)
		*priority = _REG_READ(base, 0x0);

	if (expect != NULL)
		*expect = _REG_READ(base, 0x8);

	if (tw != NULL)
		*tw = _REG_READ(base, 0xc);

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, uint32_t priority,
			  uint32_t expect, uint32_t tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);

	if (priority != 0)
		_REG_WRITE(base, priority, 0x0);

	if (expect != 0)
		_REG_WRITE(base, expect, 0x8);

	if (tw != 0)
		_REG_WRITE(base, tw, 0xc);

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];
	const struct rknpu_config *config = rknpu_dev->config;
	unsigned long flags;

	if (config->amount_top == NULL) {
		LOG_WARN("Clear rw_amount is not supported on this device!\n");
		return 0;
	}

	if (config->pc_dma_ctrl) {
		uint32_t pc_data_addr = 0;

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		pc_data_addr = REG_READ(RKNPU_OFFSET_PC_DATA_ADDR);

		REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);
		REG_WRITE(0x80000101, config->amount_top->offset_clr_all);
		REG_WRITE(0x00000101, config->amount_top->offset_clr_all);
		if (config->amount_core) {
			REG_WRITE(0x80000101,
				  config->amount_core->offset_clr_all);
			REG_WRITE(0x00000101,
				  config->amount_core->offset_clr_all);
		}
		REG_WRITE(pc_data_addr, RKNPU_OFFSET_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		spin_lock(&rknpu_dev->lock);
		REG_WRITE(0x80000101, config->amount_top->offset_clr_all);
		REG_WRITE(0x00000101, config->amount_top->offset_clr_all);
		if (config->amount_core) {
			REG_WRITE(0x80000101,
				  config->amount_core->offset_clr_all);
			REG_WRITE(0x00000101,
				  config->amount_core->offset_clr_all);
		}
		spin_unlock(&rknpu_dev->lock);
	}

	return 0;
}

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr,
			uint32_t *dt_rd, uint32_t *wd_rd)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];
	const struct rknpu_config *config = rknpu_dev->config;
	int amount_scale = config->pc_data_amount_scale;

	if (config->amount_top == NULL) {
		LOG_WARN("Get rw_amount is not supported on this device!\n");
		return 0;
	}

	spin_lock(&rknpu_dev->lock);

	if (dt_wr != NULL) {
		*dt_wr = REG_READ(config->amount_top->offset_dt_wr) *
			 amount_scale;
		if (config->amount_core) {
			*dt_wr += REG_READ(config->amount_core->offset_dt_wr) *
				  amount_scale;
		}
	}

	if (dt_rd != NULL) {
		*dt_rd = REG_READ(config->amount_top->offset_dt_rd) *
			 amount_scale;
		if (config->amount_core) {
			*dt_rd += REG_READ(config->amount_core->offset_dt_rd) *
				  amount_scale;
		}
	}

	if (wd_rd != NULL) {
		*wd_rd = REG_READ(config->amount_top->offset_wt_rd) *
			 amount_scale;
		if (config->amount_core) {
			*wd_rd += REG_READ(config->amount_core->offset_wt_rd) *
				  amount_scale;
		}
	}

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount)
{
	const struct rknpu_config *config = rknpu_dev->config;
	uint32_t dt_wr = 0;
	uint32_t dt_rd = 0;
	uint32_t wd_rd = 0;
	int ret = -EINVAL;

	if (config->amount_top == NULL) {
		LOG_WARN(
			"Get total_rw_amount is not supported on this device!\n");
		return 0;
	}

	ret = rknpu_get_rw_amount(rknpu_dev, &dt_wr, &dt_rd, &wd_rd);

	if (amount != NULL)
		*amount = dt_wr + dt_rd + wd_rd;

	return ret;
}
