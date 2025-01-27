/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/iopoll.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>

#include "mdss_fb.h"
#include "mdss_dsi.h"
#include "mdss_panel.h"
#include "mdss_mdp.h"

#ifdef CONFIG_LCDKIT_DRIVER
#include "lcdkit_dsi_status.h"
#endif

#define STATUS_CHECK_INTERVAL_MS 5000
#define STATUS_CHECK_INTERVAL_MIN_MS 50
#ifdef CONFIG_LCDKIT_DRIVER
#define DSI_STATUS_CHECK_INIT 0
#else
#define DSI_STATUS_CHECK_INIT -1
#endif

#define DSI_STATUS_CHECK_DISABLE 1

static uint32_t interval = STATUS_CHECK_INTERVAL_MS;
static int32_t dsi_status_disable = DSI_STATUS_CHECK_INIT;
struct dsi_status_data *pstatus_data;
static DEFINE_SPINLOCK(pstatus_init_lock);

/*
 * check_dsi_ctrl_status() - Reads MFD structure and
 * calls platform specific DSI ctrl Status function.
 * @work  : dsi controller status data
 */
static void check_dsi_ctrl_status(struct work_struct *work)
{
	struct dsi_status_data *pdsi_status = NULL;

	pdsi_status = container_of(to_delayed_work(work),
		struct dsi_status_data, check_status);

	if (!pdsi_status) {
		pr_err("%s: DSI status data not available\n", __func__);
		return;
	}

	if (!pdsi_status->mfd) {
		pr_err("%s: FB data not available\n", __func__);
		return;
	}

	if (mdss_panel_is_power_off(pdsi_status->mfd->panel_power_state) ||
			pdsi_status->mfd->shutdown_pending) {
		pr_debug("%s: panel off\n", __func__);
		return;
	}

	pdsi_status->mfd->mdp.check_dsi_status(work, interval);
}

/*
 * hw_vsync_handler() - Interrupt handler for HW VSYNC signal.
 * @irq		: irq line number
 * @data	: Pointer to the device structure.
 *
 * This function is called whenever a HW vsync signal is received from the
 * panel. This resets the timer of ESD delayed workqueue back to initial
 * value.
 */
irqreturn_t hw_vsync_handler(int irq, void *data)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata =
			(struct mdss_dsi_ctrl_pdata *)data;
	struct dsi_status_data *ps_data;
	unsigned long flags;

	if (!ctrl_pdata) {
		pr_err("%s: DSI ctrl not available\n", __func__);
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&pstatus_init_lock, flags);
	ps_data = pstatus_data;
	spin_unlock_irqrestore(&pstatus_init_lock, flags);

	if (ps_data)
		mod_delayed_work(system_wq, &ps_data->check_status,
			msecs_to_jiffies(interval));
	else
		pr_err("Pstatus data is NULL\n");

	if (!atomic_read(&ctrl_pdata->te_irq_ready)) {
		complete_all(&ctrl_pdata->te_irq_comp);
		atomic_inc(&ctrl_pdata->te_irq_ready);
	}

	return IRQ_HANDLED;
}

/*
 * disable_esd_thread() - Cancels work item for the esd check.
 */
void disable_esd_thread(void)
{
	if (pstatus_data &&
		cancel_delayed_work_sync(&pstatus_data->check_status))
			pr_debug("esd thread killed\n");
}

static int param_dsi_status_disable(const char *val, struct kernel_param *kp)
{
	int ret = 0;
	int int_val;

	ret = kstrtos32(val, 0, &int_val);
	if (ret)
		return ret;

	pr_info("%s: Set DSI status disable to %d\n",
			__func__, int_val);
	*((int *)kp->arg) = int_val;
	return ret;
}

static int param_set_interval(const char *val, struct kernel_param *kp)
{
	int ret = 0;
	int int_val;

	ret = kstrtos32(val, 0, &int_val);
	if (ret)
		return ret;
	if (int_val < STATUS_CHECK_INTERVAL_MIN_MS) {
		pr_err("%s: Invalid value %d used, ignoring\n",
						__func__, int_val);
		ret = -EINVAL;
	} else {
		pr_info("%s: Set check interval to %d msecs\n",
						__func__, int_val);
		*((int *)kp->arg) = int_val;
	}
	return ret;
}

int __init mdss_dsi_status_init(void)
{
	struct dsi_status_data *ps_data;
	unsigned long flags;
	int rc = 0;
	struct mdss_util_intf *util = mdss_get_util_intf();

	if (!util) {
		pr_err("%s: Failed to get utility functions\n", __func__);
		return -ENODEV;
	}

	if (util->display_disabled) {
		pr_info("Display is disabled, not progressing with dsi_init\n");
		return -ENOTSUPP;
	}

	ps_data = kzalloc(sizeof(struct dsi_status_data), GFP_KERNEL);
	if (!ps_data) {
		pr_err("%s: can't allocate memory\n", __func__);
		return -ENOMEM;
	}

	ps_data->fb_notifier.notifier_call = fb_notifier_callback;

	rc = fb_register_client(&ps_data->fb_notifier);
	if (rc < 0) {
		pr_err("%s: fb_register_client failed, returned with rc=%d\n",
								__func__, rc);
		kfree(ps_data);
		return -EPERM;
	}

	pr_info("%s: DSI status check interval:%d\n", __func__,	interval);

	INIT_DELAYED_WORK(&ps_data->check_status, check_dsi_ctrl_status);

	spin_lock_irqsave(&pstatus_init_lock, flags);
	pstatus_data = ps_data;
	spin_unlock_irqrestore(&pstatus_init_lock, flags);

	pr_debug("%s: DSI ctrl status work queue initialized\n", __func__);

	return rc;
}

void __exit mdss_dsi_status_exit(void)
{
	cancel_delayed_work_sync(&pstatus_data->check_status);
	kfree(pstatus_data);
	pr_debug("%s: DSI ctrl status work queue removed\n", __func__);
}

#ifdef CONFIG_LCDKIT_DRIVER
#include "lcdkit_dsi_status.c"
#endif

module_param_call(interval, param_set_interval, param_get_uint,
						&interval, 0644);
MODULE_PARM_DESC(interval,
		"Duration in milliseconds to send BTA command for checking"
		"DSI status periodically");

module_param_call(dsi_status_disable, param_dsi_status_disable, param_get_uint,
						&dsi_status_disable, 0644);
MODULE_PARM_DESC(dsi_status_disable,
		"Disable DSI status check");

module_init(mdss_dsi_status_init);
module_exit(mdss_dsi_status_exit);

MODULE_LICENSE("GPL v2");
