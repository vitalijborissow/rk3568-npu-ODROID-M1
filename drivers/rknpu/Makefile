# SPDX-License-Identifier: GPL-2.0
# DKMS Makefile for RKNPU driver - Kernel 6.18+ edition
obj-m += rknpu.o

ccflags-y += -I$(src)/include
ccflags-y += -I$(src)

# DKMS-specific flags
ccflags-y += -DRKNPU_DKMS
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_FENCE
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_DRM_GEM
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_PROC_FS
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_DEBUG_FS
ccflags-y += -DCONFIG_PM_DEVFREQ
# Misc device (/dev/rknpu) support - runs in parallel with DRM path
ccflags-y += -DRKNPU_DKMS_MISCDEV_ENABLED -DRKNPU_DKMS_MISCDEV
# SRAM support - requires DT rockchip,sram phandle (we patch out CONFIG_NO_GKI check)
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_SRAM -DRKNPU_DKMS_SRAM_ENABLED

# Core driver files
rknpu-y += rknpu_drv.o
rknpu-y += rknpu_reset.o
rknpu-y += rknpu_job.o
rknpu-y += rknpu_debugger.o
rknpu-y += rknpu_iommu.o
rknpu-y += rknpu_fence.o
rknpu-y += rknpu_gem.o
rknpu-y += rknpu_mem.o
# Full devfreq with DVFS support for kernel 6.18+
rknpu-y += rknpu_devfreq_dkms.o
rknpu-y += rknpu_debugfs_ctrl.o
# SRAM memory manager
rknpu-y += rknpu_mm.o
