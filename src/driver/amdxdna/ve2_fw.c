// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include <linux/version.h>

#include "amdxdna_aux_drv.h"
#include "ve2_of.h"
#include "ve2_mgmt.h"
#include "ve2_fw.h"

int ve2_store_firmware_version(struct ve2_firmware_version *c_version, struct device *xaie_dev)
{
	struct ve2_firmware_version *version;
	int ret = 0;

	/*
	 * The CERT firmware lays out the version block as VE2_CERT_VERSION_SIZE
	 * bytes (0x40), and ve2_partition_read() will write exactly that many
	 * bytes into the supplied buffer.  struct ve2_firmware_version itself
	 * is only 54 bytes, so allocating sizeof(*version) and asking the
	 * partition read to write VE2_CERT_VERSION_SIZE bytes overflows the
	 * kmalloc allocation by 10 bytes -- SLUB / KASAN flag it as
	 *   "kmalloc Redzone overwritten ... @offset=3958. First byte 0"
	 *   in ve2_store_firmware_version+0x3c/0x190 [amdxdna]
	 * during module load, and on non-KASAN kernels it silently corrupts
	 * the trailing 10 bytes of the kmalloc-64 bucket.
	 *
	 * Allocate a buffer that matches the on-device layout size and only
	 * pull the fields we care about out of it.
	 */
	BUILD_BUG_ON(sizeof(*version) > VE2_CERT_VERSION_SIZE);
	version = kzalloc(VE2_CERT_VERSION_SIZE, GFP_KERNEL);
	if (!version)
		return -ENOMEM;

	ret = ve2_partition_read(xaie_dev, 0, 0, VE2_PROG_DATA_MEMORY_OFF + VE2_CERT_VERSION_OFF,
				 VE2_CERT_VERSION_SIZE, version);
	if (ret < 0) {
		pr_err("Failed to read firmware version, ret=%d\n", ret);
		kfree(version);
		return ret;
	}

	c_version->major = version->major;
	c_version->minor = version->minor;
	strscpy(c_version->git_hash, version->git_hash, VE2_FW_HASH_STRING_LENGTH);
	c_version->git_hash[VE2_FW_HASH_STRING_LENGTH - 1] = '\0';
	strscpy(c_version->date, version->date, VE2_FW_DATE_STRING_LENGTH);
	c_version->date[VE2_FW_DATE_STRING_LENGTH - 1] = '\0';
	c_version->hotfix = version->hotfix;
	c_version->build = version->build;
	kfree(version);

	return 0;
}

static int get_firmware_status(struct amdxdna_dev *xdna, struct device *aie_dev,
			       u32 lead_col, u32 col)
{
	struct ve2_firmware_status *cs = xdna->dev_handle->fw_slots[lead_col + col];
	struct handshake *hs = NULL;
	int ret = 0;

	hs = kzalloc(sizeof(*hs), GFP_KERNEL);
	if (!hs) {
		XDNA_ERR(xdna, "No memory for handshake.\n");
		return -ENOMEM;
	}

	ret = ve2_partition_read_privileged_mem(aie_dev, col,
						offsetof(struct handshake, mpaie_alive),
						sizeof(struct handshake), (void *)hs);
	if (ret < 0) {
		XDNA_ERR(xdna, "aie_partition_read failed with ret %d\n", ret);
		goto done;
	}

	cs->state = hs->vm.fw_state;
	cs->abs_page_index = hs->vm.abs_page_index;
	cs->ppc = hs->vm.ppc;
	cs->idle_status = hs->cert_idle_status;
	cs->misc_status = hs->misc_status;

	XDNA_INFO(xdna, "Firmware status of col = %u\n", col);
	XDNA_INFO(xdna, "FW state: %u\n", cs->state);
	XDNA_INFO(xdna, "abs_page_index: %u\n", cs->abs_page_index);
	XDNA_INFO(xdna, "ppc: %u\n", cs->ppc);
	XDNA_INFO(xdna, "FW idle_status: %u\n", cs->idle_status);
	XDNA_INFO(xdna, "misc_status: %u\n", cs->misc_status);

done:
	kfree(hs);
	return ret;
}

int ve2_get_firmware_status(struct amdxdna_dev *xdna, struct amdxdna_ctx *hwctx)
{
	struct amdxdna_ctx_priv *priv_ctx = hwctx->priv;
	int ret = 0;

	if (!priv_ctx->aie_dev) {
		XDNA_ERR(xdna, "Partition does not have aie device handle\n");
		return -ENODEV;
	}

	for (u32 col = 0; col < priv_ctx->num_col; col++) {
		ret = get_firmware_status(xdna, priv_ctx->aie_dev, priv_ctx->start_col, col);
		if (ret < 0)
			XDNA_ERR(xdna, "Failed to get cert status for col %d ret = %d\n", col, ret);
	}

	return ret;
}
