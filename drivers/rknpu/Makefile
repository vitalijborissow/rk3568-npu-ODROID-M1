# SPDX-License-Identifier: GPL-2.0
obj-m += rknpu.o

ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/soc
ccflags-y += -DRKNPU_DKMS
ccflags-y += -DRKNPU_DKMS_MISCDEV
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_FENCE
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_DRM_GEM
ccflags-y += -DCONFIG_ROCKCHIP_RKNPU_PROC_FS

rknpu-y += rknpu_drv.o
rknpu-y += rknpu_reset.o
rknpu-y += rknpu_job.o
rknpu-y += rknpu_debugger.o
rknpu-y += rknpu_iommu.o
rknpu-y += rknpu_fence.o
rknpu-y += rknpu_gem.o
rknpu-y += rknpu_mem.o
rknpu-y += rknpu_devfreq_dkms.o
