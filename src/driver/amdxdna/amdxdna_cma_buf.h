/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#ifndef _AMDXDNA_CMA_BUF_H_
#define _AMDXDNA_CMA_BUF_H_

#include <drm/drm_device.h>
#include <linux/bitops.h>

#define AMDXDNA_BO_FLAGS_CACHEABLE	BIT(24)

bool amdxdna_use_cma(void);
struct dma_buf *amdxdna_get_cma_buf_with_fallback(struct device *const *region_devs,
						  int max_regions,
						  struct device *fallback_dev,
						  size_t size, u64 flags);
struct dma_buf *amdxdna_get_fixed_addr_buf(struct device *dev, phys_addr_t phys,
					   size_t size);

#endif /* _AMDXDNA_CMA_BUF_H */
