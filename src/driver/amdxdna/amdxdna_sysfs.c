// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024, Advanced Micro Devices, Inc.
 */
#include "amdxdna_sysfs.h"
#include "amdxdna_ctx.h"
#include "ve2_of.h"

static ssize_t vbnv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);

	if (!xdna->vbnv)
		return sprintf(buf, "\n");

	return sprintf(buf, "%s\n", xdna->vbnv);
}
static DEVICE_ATTR_RO(vbnv);

static ssize_t device_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", xdna->dev_info->device_type);
}
static DEVICE_ATTR_RO(device_type);

static ssize_t fw_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);

	return sprintf(buf, "%d.%d.%d.%d\n", xdna->fw_ver.major,
		       xdna->fw_ver.minor, xdna->fw_ver.sub,
		       xdna->fw_ver.build);
}
static DEVICE_ATTR_RO(fw_version);

/*
 * Forever mode default - enables forever mode for all new contexts
 * Write before starting application to auto-enable forever mode
 */
static ssize_t forever_mode_default_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", atomic_read(&xdna->forever_mode_default));
}

static ssize_t forever_mode_default_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);
	unsigned int enabled;
	int ret;

	ret = kstrtouint(buf, 0, &enabled);
	if (ret) {
		XDNA_ERR(xdna, "Invalid value. Usage: echo 0|1 > forever_mode_default");
		return ret;
	}

	if (enabled > 1) {
		XDNA_ERR(xdna, "Invalid value %u. Use 0 (disable) or 1 (enable)", enabled);
		return -EINVAL;
	}

	atomic_set(&xdna->forever_mode_default, enabled);
	XDNA_INFO(xdna, "Forever mode default %s - will apply to all new contexts",
		  enabled ? "enabled" : "disabled");

	return count;
}
static DEVICE_ATTR_RW(forever_mode_default);

/*
 * Forever mode sysfs interface for debugging
 * Format: "ctx_id value"
 * Example: echo "0 1" > /sys/class/accel/accel0/device/forever_mode_enable
 */
static ssize_t forever_mode_enable_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);
	struct amdxdna_client *client;
	struct amdxdna_ctx *ctx;
	unsigned int ctx_id, enabled;
	int ret, idx;

	ret = sscanf(buf, "%u %u", &ctx_id, &enabled);
	if (ret != 2) {
		XDNA_ERR(xdna, "Invalid format. Usage: echo 'ctx_id enable' > forever_mode_enable");
		return -EINVAL;
	}

	if (enabled > 1) {
		XDNA_ERR(xdna, "Invalid value %u. Use 0 (disable) or 1 (enable)", enabled);
		return -EINVAL;
	}

	/* Find the first client - for debugging, we assume single client */
	mutex_lock(&xdna->dev_lock);
	client = list_first_entry_or_null(&xdna->client_list,
					  struct amdxdna_client, node);
	if (!client) {
		mutex_unlock(&xdna->dev_lock);
		XDNA_ERR(xdna, "No client found");
		return -ENODEV;
	}

	idx = srcu_read_lock(&client->ctx_srcu);
	ctx = xa_load(&client->ctx_xa, ctx_id);
	if (!ctx) {
		srcu_read_unlock(&client->ctx_srcu, idx);
		mutex_unlock(&xdna->dev_lock);
		XDNA_ERR(xdna, "Context %u not found", ctx_id);
		return -EINVAL;
	}

	ret = ve2_hwctx_config_forever_mode(ctx, enabled);
	srcu_read_unlock(&client->ctx_srcu, idx);
	mutex_unlock(&xdna->dev_lock);

	if (ret) {
		XDNA_ERR(xdna, "Failed to %s forever mode for ctx %u: %d",
			 enabled ? "enable" : "disable", ctx_id, ret);
		return ret;
	}

	XDNA_INFO(xdna, "Forever mode %s for ctx %u",
		  enabled ? "enabled" : "disabled", ctx_id);

	return count;
}
static DEVICE_ATTR_WO(forever_mode_enable);

static ssize_t forever_mode_stop_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);
	struct amdxdna_client *client;
	struct amdxdna_ctx *ctx;
	unsigned int ctx_id;
	int ret, idx;
	bool was_detached = false;

	ret = sscanf(buf, "%u", &ctx_id);
	if (ret != 1) {
		XDNA_ERR(xdna, "Invalid format. Usage: echo 'ctx_id' > forever_mode_stop");
		return -EINVAL;
	}

	/* First check if this is a detached context */
	mutex_lock(&xdna->detached_lock);
	list_for_each_entry(ctx, &xdna->detached_forever_ctxs, detached_list_node) {
		if (ctx->id == ctx_id) {
			XDNA_INFO(xdna, "Found detached context %u, stopping and reclaiming", ctx_id);
			was_detached = true;

			/* Stop forever mode (polls until firmware stops) */
			ret = ve2_hwctx_forever_stop(xdna, ctx);
			if (ret) {
				mutex_unlock(&xdna->detached_lock);
				XDNA_ERR(xdna, "Failed to stop forever mode for detached ctx %u: %d",
					 ctx_id, ret);
				return ret;
			}

			/* Firmware has stopped, safe to reclaim all resources */
			ve2_hwctx_reclaim_detached(xdna, ctx);

			/* Free the context structure itself */
			kfree(ctx->name);
			kfree(ctx);

			mutex_unlock(&xdna->detached_lock);

			XDNA_INFO(xdna, "Detached context %u stopped and reclaimed", ctx_id);
			return count;
		}
	}
	mutex_unlock(&xdna->detached_lock);

	/* Not a detached context, handle normally */
	mutex_lock(&xdna->dev_lock);
	client = list_first_entry_or_null(&xdna->client_list,
					  struct amdxdna_client, node);
	if (!client) {
		mutex_unlock(&xdna->dev_lock);
		XDNA_ERR(xdna, "No client found");
		return -ENODEV;
	}

	idx = srcu_read_lock(&client->ctx_srcu);
	ctx = xa_load(&client->ctx_xa, ctx_id);
	if (!ctx) {
		srcu_read_unlock(&client->ctx_srcu, idx);
		mutex_unlock(&xdna->dev_lock);
		XDNA_ERR(xdna, "Context %u not found", ctx_id);
		return -EINVAL;
	}

	ret = ve2_hwctx_forever_stop(xdna, ctx);
	srcu_read_unlock(&client->ctx_srcu, idx);
	mutex_unlock(&xdna->dev_lock);

	if (ret) {
		XDNA_ERR(xdna, "Failed to stop forever mode for ctx %u: %d", ctx_id, ret);
		return ret;
	}

	XDNA_INFO(xdna, "Forever mode stopped for ctx %u", ctx_id);

	return count;
}
static DEVICE_ATTR_WO(forever_mode_stop);

static ssize_t forever_mode_status_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct amdxdna_dev *xdna = dev_get_drvdata(dev);
	struct amdxdna_client *client;
	struct amdxdna_ctx *ctx;
	struct amdxdna_hwctx_forever_status status;
	unsigned long ctx_id;
	int ret, idx;
	ssize_t len = 0;

	len += sprintf(buf + len, "ctx_id  enabled  iteration  last_status  state\n");
	len += sprintf(buf + len, "------  -------  ---------  -----------  --------\n");

	/* Show active contexts from clients */
	mutex_lock(&xdna->dev_lock);
	client = list_first_entry_or_null(&xdna->client_list,
					  struct amdxdna_client, node);
	if (client) {
		idx = srcu_read_lock(&client->ctx_srcu);
		xa_for_each(&client->ctx_xa, ctx_id, ctx) {
			memset(&status, 0, sizeof(status));

			ret = ve2_hwctx_query_forever_status(xdna, ctx, &status);
			if (ret == 0) {
				len += sprintf(buf + len, "%-6lu  %-7u  %-9u  0x%08x   active\n",
					       ctx_id, status.enabled, status.iteration,
					       status.last_status);
			}
		}
		srcu_read_unlock(&client->ctx_srcu, idx);
	}
	mutex_unlock(&xdna->dev_lock);

	/* Show detached contexts (running without owning application) */
	mutex_lock(&xdna->detached_lock);
	list_for_each_entry(ctx, &xdna->detached_forever_ctxs, detached_list_node) {
		memset(&status, 0, sizeof(status));

		ret = ve2_hwctx_query_forever_status(xdna, ctx, &status);
		if (ret == 0) {
			len += sprintf(buf + len, "%-6u  %-7u  %-9u  0x%08x   detached\n",
				       ctx->id, status.enabled, status.iteration,
				       status.last_status);
		}
	}
	mutex_unlock(&xdna->detached_lock);

	if (len == sizeof("ctx_id  enabled  iteration  last_status  state\n------  -------  ---------  -----------  --------\n") - 1)
		len += sprintf(buf + len, "No contexts found\n");

	return len;
}
static DEVICE_ATTR_RO(forever_mode_status);

static struct attribute *amdxdna_attrs[] = {
	&dev_attr_device_type.attr,
	&dev_attr_vbnv.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_forever_mode_default.attr,
	&dev_attr_forever_mode_enable.attr,
	&dev_attr_forever_mode_stop.attr,
	&dev_attr_forever_mode_status.attr,
	NULL,
};

static struct attribute_group amdxdna_attr_group = {
	.attrs = amdxdna_attrs,
};

int amdxdna_sysfs_init(struct amdxdna_dev *xdna)
{
	int ret;

	ret = sysfs_create_group(&xdna->ddev.dev->kobj, &amdxdna_attr_group);
	if (ret)
		XDNA_ERR(xdna, "Create attr group failed");

	return ret;
}

void amdxdna_sysfs_fini(struct amdxdna_dev *xdna)
{
	sysfs_remove_group(&xdna->ddev.dev->kobj, &amdxdna_attr_group);
}
