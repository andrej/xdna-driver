/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2024, Advanced Micro Devices, Inc.
 */

#ifndef _AMDXDNA_SYSFS_H_
#define _AMDXDNA_SYSFS_H_

#include "amdxdna_drm.h"
#include "amdxdna_ctx.h"

int amdxdna_sysfs_init(struct amdxdna_dev *xdna);
void amdxdna_sysfs_fini(struct amdxdna_dev *xdna);

int amdxdna_sysfs_create_forever_ctx(struct amdxdna_dev *xdna, struct amdxdna_ctx *ctx);
void amdxdna_sysfs_remove_forever_ctx(struct amdxdna_ctx *ctx);

#endif /* _AMDXDNA_SYSFS_H_ */
