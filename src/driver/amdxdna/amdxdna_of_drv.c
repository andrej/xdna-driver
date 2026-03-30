// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <drm/drm_managed.h>

#include "amdxdna_devel.h"
#include "amdxdna_of_drv.h"
#include "amdxdna_sysfs.h"

static const struct of_device_id amdxdna_of_table[] = {
	{ .compatible = "amdxdna,ve2", .data = &dev_ve2_info },
	{ .compatible = "xlnx,aiarm", .data = &dev_ve2_info },
	{ /* end of table */ }
};

MODULE_DEVICE_TABLE(of, amdxdna_of_table);

static int amdxdna_of_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *id;
	struct amdxdna_dev *xdna;
	int ret;

	xdna = devm_drm_dev_alloc(dev, &amdxdna_drm_drv, typeof(*xdna), ddev);
	if (IS_ERR(xdna))
		return PTR_ERR(xdna);

	id = of_match_node(amdxdna_of_table, dev->of_node);
	if (!id) {
		XDNA_ERR(xdna, "Match device ID not found");
		return -EINVAL;
	}

	xdna->dev_info = (struct amdxdna_dev_info *)id->data;
	if (!xdna->dev_info)
		return -ENODEV;

	drmm_mutex_init(&xdna->ddev, &xdna->dev_lock);
	INIT_LIST_HEAD(&xdna->client_list);
	INIT_LIST_HEAD(&xdna->detached_forever_ctxs);
	mutex_init(&xdna->detached_lock);
	xdna->next_detached_id = AMDXDNA_MAX_CTX_ID + 1;
	platform_set_drvdata(pdev, xdna);

	if (!xdna->dev_info->ops->init || !xdna->dev_info->ops->fini)
		return -EOPNOTSUPP;

	mutex_lock(&xdna->dev_lock);
	ret = xdna->dev_info->ops->init(xdna);
	mutex_unlock(&xdna->dev_lock);
	if (ret) {
		XDNA_ERR(xdna, "Hardware init failed, ret %d", ret);
		return ret;
	}

	/* Set vbnv to default - OF devices don't query revision from firmware */
	xdna->vbnv = xdna->dev_info->default_vbnv;

	ret = amdxdna_sysfs_init(xdna);
	if (ret) {
		XDNA_ERR(xdna, "Create amdxdna sysfs attrs failed: %d", ret);
		goto failed_dev_fini;
	}

	ret = drm_dev_register(&xdna->ddev, 0);
	if (ret) {
		XDNA_ERR(xdna, "DRM register failed, ret %d", ret);
		goto failed_sysfs_fini;
	}

	if (!xdna->dev_handle) {
		XDNA_ERR(xdna, "amdxdna device handle is null");
		ret = -EINVAL;
		goto failed_drm_unreg;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			XDNA_ERR(xdna, "DMA configuration failed: 0x%x\n", ret);
			goto failed_drm_unreg;
		}

		XDNA_WARN(xdna, "DMA configuration downgraded to 32bit Mask\n");
	}

	if (xdna->dev_info->ops->debugfs)
		xdna->dev_info->ops->debugfs(xdna);

	iommu_mode = AMDXDNA_IOMMU_NO_PASID;

	XDNA_DBG(xdna, "pdev %p", pdev);

	return 0;

failed_drm_unreg:
	drm_dev_put(&xdna->ddev);
failed_sysfs_fini:
	amdxdna_sysfs_fini(xdna);
failed_dev_fini:
	mutex_lock(&xdna->dev_lock);
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
	return ret;
}

static void amdxdna_of_remove(struct platform_device *pdev)
{
	struct amdxdna_dev *xdna = platform_get_drvdata(pdev);
	struct amdxdna_ctx *ctx;

	drm_dev_unplug(&xdna->ddev);

	/* Force-stop and reclaim any detached forever mode contexts */
	mutex_lock(&xdna->detached_lock);
	while (!list_empty(&xdna->detached_forever_ctxs)) {
		ctx = list_first_entry(&xdna->detached_forever_ctxs,
				       struct amdxdna_ctx, detached_list_node);

		XDNA_WARN(xdna,
			  "Driver unload: force-stopping detached forever context %s (ctx_id=%u)",
			  ctx->name, ctx->id);

		/* Stop forever mode - this polls until firmware stops */
		ve2_hwctx_forever_stop(xdna, ctx);

		/* Reclaim all resources (removes from list) */
		ve2_hwctx_reclaim_detached(xdna, ctx);

		/* Free the context structure itself */
		kfree(ctx->name);
		kfree(ctx);
	}
	mutex_unlock(&xdna->detached_lock);
	mutex_destroy(&xdna->detached_lock);

	amdxdna_sysfs_fini(xdna);

	mutex_lock(&xdna->dev_lock);
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
}

static struct platform_driver amdxdna_of_plat_driver = {
	.probe		= amdxdna_of_probe,
	.remove_new	= amdxdna_of_remove,
	.driver		= {
		.name	= KBUILD_MODNAME,
		.of_match_table = amdxdna_of_table,
	},
};

module_platform_driver(amdxdna_of_plat_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_VERSION(MODULE_VER_STR);
MODULE_DESCRIPTION("amdxdna_of driver");
