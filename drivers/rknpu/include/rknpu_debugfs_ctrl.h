/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * RKNPU debugfs control interface header
 */

#ifndef __LINUX_RKNPU_DEBUGFS_CTRL_H
#define __LINUX_RKNPU_DEBUGFS_CTRL_H

#include "rknpu_drv.h"

int rknpu_debugfs_ctrl_init(struct rknpu_device *rknpu_dev);
void rknpu_debugfs_ctrl_remove(void);

#endif
