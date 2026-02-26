// SPDX-License-Identifier: GPL-2.0
/*
 * DKMS Stub for rknpu_devfreq - provides no-op implementations
 * since Rockchip devfreq APIs are not available in vanilla/Armbian kernels
 */

#include <linux/device.h>
#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
    /* Stub - devfreq not available in DKMS builds */
}

void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
    /* Stub - devfreq not available in DKMS builds */
}

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
    /* Stub - return success, devfreq not available */
    return 0;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
    /* Stub - nothing to remove */
}

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
    /* Stub - return success */
    return 0;
}

int rknpu_devfreq_runtime_resume(struct device *dev)
{
    /* Stub - return success */
    return 0;
}
