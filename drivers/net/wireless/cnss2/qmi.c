/* Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
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

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/qmi_encdec.h>
#include <soc/qcom/msm_qmi_interface.h>

#include "main.h"
#include "debug.h"
#include "qmi.h"

#define WLFW_SERVICE_INS_ID_V01		1
#define WLFW_CLIENT_ID			0x4b4e454c
#define MAX_BDF_FILE_NAME		11
#define DEFAULT_BDF_FILE_NAME		"bdwlan.bin"
#define BDF_FILE_NAME_PREFIX		"bdwlan.b"

#ifdef CONFIG_CNSS2_DEBUG
static unsigned int qmi_timeout = 10000;
module_param(qmi_timeout, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(qmi_timeout, "Timeout for QMI message in milliseconds");

#define QMI_WLFW_TIMEOUT_MS		qmi_timeout
#else
#define QMI_WLFW_TIMEOUT_MS		10000
#endif

static bool daemon_support;
module_param(daemon_support, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(daemon_support, "User space has cnss-daemon support or not");

static bool bdf_bypass = true;
#ifdef CONFIG_CNSS2_DEBUG
module_param(bdf_bypass, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(bdf_bypass, "If BDF is not found, send dummy BDF to FW");
#endif

static void cnss_wlfw_clnt_notifier_work(struct work_struct *work)
{
	struct cnss_plat_data *plat_priv =
		container_of(work, struct cnss_plat_data, qmi_recv_msg_work);
	int ret = 0;

	cnss_pr_dbg("Receiving QMI WLFW event in work queue context\n");

	do {
		ret = qmi_recv_msg(plat_priv->qmi_wlfw_clnt);
	} while (ret == 0);

	if (ret != -ENOMSG)
		cnss_pr_err("Error receiving message: %d\n", ret);

	cnss_pr_dbg("Receiving QMI event completed\n");
}

static void cnss_wlfw_clnt_notifier(struct qmi_handle *handle,
				    enum qmi_event_type event,
				    void *notify_priv)
{
	struct cnss_plat_data *plat_priv = notify_priv;

	cnss_pr_dbg("Received QMI WLFW event: %d\n", event);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL!\n");
		return;
	}

	switch (event) {
	case QMI_RECV_MSG:
		schedule_work(&plat_priv->qmi_recv_msg_work);
		break;
	case QMI_SERVER_EXIT:
		break;
	default:
		cnss_pr_dbg("Unhandled QMI event: %d\n", event);
		break;
	}
}

static int cnss_wlfw_clnt_svc_event_notifier(struct notifier_block *nb,
					     unsigned long code, void *_cmd)
{
	struct cnss_plat_data *plat_priv =
		container_of(nb, struct cnss_plat_data, qmi_wlfw_clnt_nb);
	int ret = 0;

	cnss_pr_dbg("Received QMI WLFW service event: %ld\n", code);

	switch (code) {
	case QMI_SERVER_ARRIVE:
		ret = cnss_driver_event_post(plat_priv,
					     CNSS_DRIVER_EVENT_SERVER_ARRIVE,
					     false, NULL);
		break;

	case QMI_SERVER_EXIT:
		ret = cnss_driver_event_post(plat_priv,
					     CNSS_DRIVER_EVENT_SERVER_EXIT,
					     false, NULL);
		break;
	default:
		cnss_pr_dbg("Invalid QMI service event: %ld\n", code);
		break;
	}

	return ret;
}

static int cnss_wlfw_host_cap_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_host_cap_req_msg_v01 req;
	struct wlfw_host_cap_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	cnss_pr_dbg("Sending host capability message, state: 0x%lx\n",
		    plat_priv->driver_state);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.daemon_support_valid = 1;
	req.daemon_support = daemon_support;

	cnss_pr_dbg("daemon_support is %d\n", req.daemon_support);

	req_desc.max_msg_len = WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_HOST_CAP_REQ_V01;
	req_desc.ei_array = wlfw_host_cap_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_HOST_CAP_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_HOST_CAP_RESP_V01;
	resp_desc.ei_array = wlfw_host_cap_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send host capability request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("Host capability request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;
out:
	CNSS_ASSERT(0);
	return ret;
}

static int cnss_wlfw_ind_register_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_ind_register_req_msg_v01 req;
	struct wlfw_ind_register_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	cnss_pr_dbg("Sending indication register message, state: 0x%lx\n",
		    plat_priv->driver_state);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.client_id_valid = 1;
	req.client_id = WLFW_CLIENT_ID;
	req.fw_ready_enable_valid = 1;
	req.fw_ready_enable = 1;
	req.request_mem_enable_valid = 1;
	req.request_mem_enable = 1;
	req.fw_mem_ready_enable_valid = 1;
	req.fw_mem_ready_enable = 1;
	req.cold_boot_cal_done_enable_valid = 1;
	req.cold_boot_cal_done_enable = 1;
	req.pin_connect_result_enable_valid = 1;
	req.pin_connect_result_enable = 1;

	req_desc.max_msg_len = WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_IND_REGISTER_REQ_V01;
	req_desc.ei_array = wlfw_ind_register_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_IND_REGISTER_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_IND_REGISTER_RESP_V01;
	resp_desc.ei_array = wlfw_ind_register_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send indication register request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("Indication register request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;
out:
	CNSS_ASSERT(0);
	return ret;
}

static int cnss_wlfw_request_mem_ind_hdlr(struct cnss_plat_data *plat_priv,
					  void *msg, unsigned int msg_len)
{
	struct msg_desc ind_desc;
	struct wlfw_request_mem_ind_msg_v01 ind_msg;
	struct cnss_fw_mem *fw_mem = &plat_priv->fw_mem;
	int ret = 0;

	ind_desc.msg_id = QMI_WLFW_REQUEST_MEM_IND_V01;
	ind_desc.max_msg_len = WLFW_REQUEST_MEM_IND_MSG_V01_MAX_MSG_LEN;
	ind_desc.ei_array = wlfw_request_mem_ind_msg_v01_ei;

	ret = qmi_kernel_decode(&ind_desc, &ind_msg, msg, msg_len);
	if (ret < 0) {
		cnss_pr_err("Failed to decode request memory indication, msg_len: %u, err = %d\n",
			    ret, msg_len);
		return ret;
	}

	fw_mem->size = ind_msg.size;

	cnss_driver_event_post(plat_priv, CNSS_DRIVER_EVENT_REQUEST_MEM,
			       false, NULL);

	return 0;
}

static int cnss_qmi_pin_result_ind_hdlr(struct cnss_plat_data *plat_priv,
					void *msg, unsigned int msg_len)
{
	struct msg_desc ind_desc;
	struct wlfw_pin_connect_result_ind_msg_v01 ind_msg;
	int ret = 0;

	ind_desc.msg_id = QMI_WLFW_PIN_CONNECT_RESULT_IND_V01;
	ind_desc.max_msg_len = WLFW_PIN_CONNECT_RESULT_IND_MSG_V01_MAX_MSG_LEN;
	ind_desc.ei_array = wlfw_pin_connect_result_ind_msg_v01_ei;

	ret = qmi_kernel_decode(&ind_desc, &ind_msg, msg, msg_len);
	if (ret < 0) {
		cnss_pr_err("Failed to decode pin connect result indication, msg_len: %u, err = %d\n",
			    msg_len, ret);
		return ret;
	}
	if (ind_msg.pwr_pin_result_valid)
		plat_priv->pin_result.fw_pwr_pin_result =
		    ind_msg.pwr_pin_result;
	if (ind_msg.phy_io_pin_result_valid)
		plat_priv->pin_result.fw_phy_io_pin_result =
		    ind_msg.phy_io_pin_result;
	if (ind_msg.rf_pin_result_valid)
		plat_priv->pin_result.fw_rf_pin_result = ind_msg.rf_pin_result;

	cnss_pr_dbg("Pin connect Result: pwr_pin: 0x%x phy_io_pin: 0x%x rf_io_pin: 0x%x\n",
		    ind_msg.pwr_pin_result, ind_msg.phy_io_pin_result,
		    ind_msg.rf_pin_result);
	return ret;
}

int cnss_wlfw_respond_mem_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_respond_mem_req_msg_v01 req;
	struct wlfw_respond_mem_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	struct cnss_fw_mem *fw_mem = &plat_priv->fw_mem;
	int ret = 0;

	cnss_pr_dbg("Sending respond memory message, state: 0x%lx\n",
		    plat_priv->driver_state);

	if (!fw_mem->pa || !fw_mem->size) {
		cnss_pr_err("Memory for FW is not available!\n");
		ret = -ENOMEM;
		goto out;
	}

	cnss_pr_dbg("Memory for FW, va: 0x%pK, pa: %pa, size: 0x%zx\n",
		    fw_mem->va, &fw_mem->pa, fw_mem->size);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.addr = fw_mem->pa;
	req.size = fw_mem->size;

	req_desc.max_msg_len = WLFW_RESPOND_MEM_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_RESPOND_MEM_REQ_V01;
	req_desc.ei_array = wlfw_respond_mem_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_RESPOND_MEM_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_RESPOND_MEM_RESP_V01;
	resp_desc.ei_array = wlfw_respond_mem_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send respond memory request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("Respond memory request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;
out:
	CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_tgt_cap_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_cap_req_msg_v01 req;
	struct wlfw_cap_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	cnss_pr_dbg("Sending target capability message, state: 0x%lx\n",
		    plat_priv->driver_state);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req_desc.max_msg_len = WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_CAP_REQ_V01;
	req_desc.ei_array = wlfw_cap_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_CAP_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_CAP_RESP_V01;
	resp_desc.ei_array = wlfw_cap_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send target capability request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("Target capability request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	if (resp.chip_info_valid)
		plat_priv->chip_info = resp.chip_info;
	if (resp.board_info_valid)
		plat_priv->board_info = resp.board_info;
	else
		plat_priv->board_info.board_id = 0xFF;
	if (resp.soc_info_valid)
		plat_priv->soc_info = resp.soc_info;
	if (resp.fw_version_info_valid)
		plat_priv->fw_version_info = resp.fw_version_info;

	cnss_pr_dbg("Target capability: chip_id: 0x%x, chip_family: 0x%x, board_id: 0x%x, soc_id: 0x%x, fw_version: 0x%x, fw_build_timestamp: %s",
		    plat_priv->chip_info.chip_id,
		    plat_priv->chip_info.chip_family,
		    plat_priv->board_info.board_id, plat_priv->soc_info.soc_id,
		    plat_priv->fw_version_info.fw_version,
		    plat_priv->fw_version_info.fw_build_timestamp);

	return 0;
out:
	CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_bdf_dnld_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_bdf_download_req_msg_v01 *req;
	struct wlfw_bdf_download_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	char filename[MAX_BDF_FILE_NAME];
	const struct firmware *fw_entry;
	const u8 *temp;
	unsigned int remaining;
	int ret = 0;

	cnss_pr_dbg("Sending BDF download message, state: 0x%lx\n",
		    plat_priv->driver_state);

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}

	if (0xFF == plat_priv->board_info.board_id)
		snprintf(filename, sizeof(filename), DEFAULT_BDF_FILE_NAME);
	else
		snprintf(filename, sizeof(filename),
			 BDF_FILE_NAME_PREFIX "%02x",
			 plat_priv->board_info.board_id);

	ret = request_firmware(&fw_entry, filename, &plat_priv->plat_dev->dev);
	if (ret) {
		cnss_pr_err("Failed to load BDF: %s\n", filename);
		if (bdf_bypass) {
			cnss_pr_info("bdf_bypass is enabled, sending dummy BDF\n");
			temp = filename;
			remaining = MAX_BDF_FILE_NAME;
			goto bypass_bdf;
		} else {
			goto err_req_fw;
		}
	}

	temp = fw_entry->data;
	remaining = fw_entry->size;

bypass_bdf:
	cnss_pr_dbg("Downloading BDF: %s, size: %u\n", filename, remaining);

	memset(&resp, 0, sizeof(resp));

	req_desc.max_msg_len = WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_BDF_DOWNLOAD_REQ_V01;
	req_desc.ei_array = wlfw_bdf_download_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_BDF_DOWNLOAD_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_BDF_DOWNLOAD_RESP_V01;
	resp_desc.ei_array = wlfw_bdf_download_resp_msg_v01_ei;

	while (remaining) {
		req->valid = 1;
		req->file_id_valid = 1;
		req->file_id = plat_priv->board_info.board_id;
		req->total_size_valid = 1;
		req->total_size = remaining;
		req->seg_id_valid = 1;
		req->data_valid = 1;
		req->end_valid = 1;

		if (remaining > QMI_WLFW_MAX_DATA_SIZE_V01) {
			req->data_len = QMI_WLFW_MAX_DATA_SIZE_V01;
		} else {
			req->data_len = remaining;
			req->end = 1;
		}

		memcpy(req->data, temp, req->data_len);

		ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc,
					req, sizeof(*req), &resp_desc, &resp,
					sizeof(resp), QMI_WLFW_TIMEOUT_MS);
		if (ret < 0) {
			cnss_pr_err("Failed to send BDF download request, err = %d\n",
				    ret);
			goto err_send;
		}

		if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
			cnss_pr_err("BDF download request failed, result: %d, err: %d\n",
				    resp.resp.result, resp.resp.error);
			ret = resp.resp.result;
			goto err_send;
		}

		remaining -= req->data_len;
		temp += req->data_len;
		req->seg_id++;
	}

err_send:
	if (!bdf_bypass)
		release_firmware(fw_entry);
err_req_fw:
	kfree(req);
out:
	if (ret)
		CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_m3_dnld_send_sync(struct cnss_plat_data *plat_priv)
{
	struct wlfw_m3_info_req_msg_v01 req;
	struct wlfw_m3_info_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	struct cnss_fw_mem *m3_mem = &plat_priv->m3_mem;
	int ret = 0;

	cnss_pr_dbg("Sending M3 information message, state: 0x%lx\n",
		    plat_priv->driver_state);

	if (!m3_mem->pa || !m3_mem->size) {
		cnss_pr_err("Memory for M3 is not available!\n");
		ret = -ENOMEM;
		goto out;
	}

	cnss_pr_dbg("M3 memory, va: 0x%pK, pa: %pa, size: 0x%zx\n",
		    m3_mem->va, &m3_mem->pa, m3_mem->size);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.addr = plat_priv->m3_mem.pa;
	req.size = plat_priv->m3_mem.size;

	req_desc.max_msg_len = WLFW_M3_INFO_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_M3_INFO_REQ_V01;
	req_desc.ei_array = wlfw_m3_info_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_M3_INFO_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_M3_INFO_RESP_V01;
	resp_desc.ei_array = wlfw_m3_info_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send M3 information request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("M3 information request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;

out:
	CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_wlan_mode_send_sync(struct cnss_plat_data *plat_priv,
				  enum wlfw_driver_mode_enum_v01 mode)
{
	struct wlfw_wlan_mode_req_msg_v01 req;
	struct wlfw_wlan_mode_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	if (!plat_priv)
		return -ENODEV;

	cnss_pr_dbg("Sending mode message, state: 0x%lx, mode: %d\n",
		    plat_priv->driver_state, mode);

	if (mode == QMI_WLFW_OFF_V01 &&
	    test_bit(CNSS_DRIVER_RECOVERY, &plat_priv->driver_state)) {
		cnss_pr_dbg("Recovery is in progress, ignore mode off request.\n");
		return 0;
	}

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.mode = mode;
	req.hw_debug_valid = 1;
	req.hw_debug = 0;

	req_desc.max_msg_len = WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_WLAN_MODE_REQ_V01;
	req_desc.ei_array = wlfw_wlan_mode_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_WLAN_MODE_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_WLAN_MODE_RESP_V01;
	resp_desc.ei_array = wlfw_wlan_mode_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		if (mode == QMI_WLFW_OFF_V01 && ret == -ENETRESET) {
			cnss_pr_dbg("WLFW service is disconnected while sending mode off request.\n");
			return 0;
		}
		cnss_pr_err("Failed to send mode request, mode: %d, err = %d\n",
			    mode, ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("Mode request failed, mode: %d, result: %d err: %d\n",
			    mode, resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;
out:
	if (mode != QMI_WLFW_OFF_V01)
		CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_wlan_cfg_send_sync(struct cnss_plat_data *plat_priv,
				 struct wlfw_wlan_cfg_req_msg_v01 *data)
{
	struct wlfw_wlan_cfg_req_msg_v01 req;
	struct wlfw_wlan_cfg_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	cnss_pr_dbg("Sending WLAN config message, state: 0x%lx\n",
		    plat_priv->driver_state);

	if (!plat_priv)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	memcpy(&req, data, sizeof(req));

	req_desc.max_msg_len = WLFW_WLAN_CFG_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_WLAN_CFG_REQ_V01;
	req_desc.ei_array = wlfw_wlan_cfg_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_WLAN_CFG_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_WLAN_CFG_RESP_V01;
	resp_desc.ei_array = wlfw_wlan_cfg_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send WLAN config request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("WLAN config request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;
out:
	CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_athdiag_read_send_sync(struct cnss_plat_data *plat_priv,
				     uint32_t offset, uint32_t mem_type,
				     uint32_t data_len, uint8_t *data)
{
	struct wlfw_athdiag_read_req_msg_v01 req;
	struct wlfw_athdiag_read_resp_msg_v01 *resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	if (!plat_priv)
		return -ENODEV;

	if (!plat_priv->qmi_wlfw_clnt)
		return -EINVAL;

	cnss_pr_dbg("athdiag read: state 0x%lx, offset %x, mem_type %x, data_len %u\n",
		    plat_priv->driver_state, offset, mem_type, data_len);

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	memset(&req, 0, sizeof(req));

	req.offset = offset;
	req.mem_type = mem_type;
	req.data_len = data_len;

	req_desc.max_msg_len = WLFW_ATHDIAG_READ_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_ATHDIAG_READ_REQ_V01;
	req_desc.ei_array = wlfw_athdiag_read_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_ATHDIAG_READ_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_ATHDIAG_READ_RESP_V01;
	resp_desc.ei_array = wlfw_athdiag_read_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, &req,
				sizeof(req), &resp_desc, resp, sizeof(*resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send athdiag read request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("athdiag read request failed, result: %d, err: %d\n",
			    resp->resp.result, resp->resp.error);
		ret = resp->resp.result;
		goto out;
	}

	if (!resp->data_valid || resp->data_len != data_len) {
		cnss_pr_err("athdiag read data is invalid, data_valid = %u, data_len = %u\n",
			    resp->data_valid, resp->data_len);
		ret = -EINVAL;
		goto out;
	}

	memcpy(data, resp->data, resp->data_len);

out:
	kfree(resp);
	return ret;
}

int cnss_wlfw_athdiag_write_send_sync(struct cnss_plat_data *plat_priv,
				      uint32_t offset, uint32_t mem_type,
				      uint32_t data_len, uint8_t *data)
{
	struct wlfw_athdiag_write_req_msg_v01 *req;
	struct wlfw_athdiag_write_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;
	int ret = 0;

	if (!plat_priv)
		return -ENODEV;

	if (!plat_priv->qmi_wlfw_clnt)
		return -EINVAL;

	cnss_pr_dbg("athdiag write: state 0x%lx, offset %x, mem_type %x, data_len %u, data %p\n",
		    plat_priv->driver_state, offset, mem_type, data_len, data);

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	memset(&resp, 0, sizeof(resp));

	req->offset = offset;
	req->mem_type = mem_type;
	req->data_len = data_len;
	memcpy(req->data, data, data_len);

	req_desc.max_msg_len = WLFW_ATHDIAG_WRITE_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_ATHDIAG_WRITE_REQ_V01;
	req_desc.ei_array = wlfw_athdiag_write_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_ATHDIAG_WRITE_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_ATHDIAG_WRITE_RESP_V01;
	resp_desc.ei_array = wlfw_athdiag_write_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt, &req_desc, req,
				sizeof(*req), &resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Failed to send athdiag write request, err = %d\n",
			    ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("athdiag write request failed, result: %d, err: %d\n",
			    resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

out:
	kfree(req);
	return ret;
}

int cnss_wlfw_ini_send_sync(struct cnss_plat_data *plat_priv,
			    uint8_t fw_log_mode)
{
	int ret;
	struct wlfw_ini_req_msg_v01 req;
	struct wlfw_ini_resp_msg_v01 resp;
	struct msg_desc req_desc, resp_desc;

	if (!plat_priv)
		return -ENODEV;

	cnss_pr_dbg("Sending ini sync request, state: 0x%lx, fw_log_mode: %d\n",
		    plat_priv->driver_state, fw_log_mode);

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	req.enablefwlog_valid = 1;
	req.enablefwlog = fw_log_mode;

	req_desc.max_msg_len = WLFW_INI_REQ_MSG_V01_MAX_MSG_LEN;
	req_desc.msg_id = QMI_WLFW_INI_REQ_V01;
	req_desc.ei_array = wlfw_ini_req_msg_v01_ei;

	resp_desc.max_msg_len = WLFW_INI_RESP_MSG_V01_MAX_MSG_LEN;
	resp_desc.msg_id = QMI_WLFW_INI_RESP_V01;
	resp_desc.ei_array = wlfw_ini_resp_msg_v01_ei;

	ret = qmi_send_req_wait(plat_priv->qmi_wlfw_clnt,
				&req_desc, &req, sizeof(req),
				&resp_desc, &resp, sizeof(resp),
				QMI_WLFW_TIMEOUT_MS);
	if (ret < 0) {
		cnss_pr_err("Send INI req failed fw_log_mode: %d, ret: %d\n",
			    fw_log_mode, ret);
		goto out;
	}

	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		cnss_pr_err("QMI INI request rejected, fw_log_mode:%d result:%d error:%d\n",
			    fw_log_mode, resp.resp.result, resp.resp.error);
		ret = resp.resp.result;
		goto out;
	}

	return 0;

out:
	return ret;
}

static void cnss_wlfw_clnt_ind(struct qmi_handle *handle,
			       unsigned int msg_id, void *msg,
			       unsigned int msg_len, void *ind_cb_priv)
{
	struct cnss_plat_data *plat_priv = ind_cb_priv;

	cnss_pr_dbg("Received QMI WLFW indication, msg_id: 0x%x, msg_len: %d\n",
		    msg_id, msg_len);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL!\n");
		return;
	}

	switch (msg_id) {
	case QMI_WLFW_REQUEST_MEM_IND_V01:
		cnss_wlfw_request_mem_ind_hdlr(plat_priv, msg, msg_len);
		break;
	case QMI_WLFW_FW_MEM_READY_IND_V01:
		cnss_driver_event_post(plat_priv,
				       CNSS_DRIVER_EVENT_FW_MEM_READY,
				       false, NULL);
		break;
	case QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01:
		cnss_driver_event_post(plat_priv,
				       CNSS_DRIVER_EVENT_COLD_BOOT_CAL_DONE,
				       false, NULL);
		break;
	case QMI_WLFW_FW_READY_IND_V01:
		cnss_driver_event_post(plat_priv,
				       CNSS_DRIVER_EVENT_FW_READY,
				       false, NULL);
		break;
	case QMI_WLFW_PIN_CONNECT_RESULT_IND_V01:
		cnss_qmi_pin_result_ind_hdlr(plat_priv, msg, msg_len);
		break;
	default:
		cnss_pr_err("Invalid QMI WLFW indication, msg_id: 0x%x\n",
			    msg_id);
		break;
	}
}

unsigned int cnss_get_qmi_timeout(void)
{
	cnss_pr_dbg("QMI timeout is %u ms\n", QMI_WLFW_TIMEOUT_MS);

	return QMI_WLFW_TIMEOUT_MS;
}
EXPORT_SYMBOL(cnss_get_qmi_timeout);

int cnss_wlfw_server_arrive(struct cnss_plat_data *plat_priv)
{
	int ret = 0;

	if (!plat_priv)
		return -ENODEV;

	plat_priv->qmi_wlfw_clnt =
		qmi_handle_create(cnss_wlfw_clnt_notifier, plat_priv);
	if (!plat_priv->qmi_wlfw_clnt) {
		cnss_pr_err("Failed to create QMI client handle!\n");
		ret = -ENOMEM;
		goto err_create_handle;
	}

	ret = qmi_connect_to_service(plat_priv->qmi_wlfw_clnt,
				     WLFW_SERVICE_ID_V01,
				     WLFW_SERVICE_VERS_V01,
				     WLFW_SERVICE_INS_ID_V01);
	if (ret < 0) {
		cnss_pr_err("Failed to connect to QMI WLFW service, err = %d\n",
			    ret);
		goto out;
	}

	ret = qmi_register_ind_cb(plat_priv->qmi_wlfw_clnt,
				  cnss_wlfw_clnt_ind, plat_priv);
	if (ret < 0) {
		cnss_pr_err("Failed to register QMI WLFW service indication callback, err = %d\n",
			    ret);
		goto out;
	}

	set_bit(CNSS_QMI_WLFW_CONNECTED, &plat_priv->driver_state);

	cnss_pr_info("QMI WLFW service connected, state: 0x%lx\n",
		     plat_priv->driver_state);

	ret = cnss_wlfw_host_cap_send_sync(plat_priv);
	if (ret < 0)
		goto out;

	ret = cnss_wlfw_ind_register_send_sync(plat_priv);
	if (ret < 0)
		goto out;

	return 0;
out:
	qmi_handle_destroy(plat_priv->qmi_wlfw_clnt);
	plat_priv->qmi_wlfw_clnt = NULL;
err_create_handle:
	CNSS_ASSERT(0);
	return ret;
}

int cnss_wlfw_server_exit(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	qmi_handle_destroy(plat_priv->qmi_wlfw_clnt);
	plat_priv->qmi_wlfw_clnt = NULL;

	clear_bit(CNSS_FW_READY, &plat_priv->driver_state);
	clear_bit(CNSS_FW_MEM_READY, &plat_priv->driver_state);
	clear_bit(CNSS_QMI_WLFW_CONNECTED, &plat_priv->driver_state);

	cnss_pr_info("QMI WLFW service disconnected, state: 0x%lx\n",
		     plat_priv->driver_state);

	return 0;
}

int cnss_qmi_init(struct cnss_plat_data *plat_priv)
{
	int ret = 0;

	INIT_WORK(&plat_priv->qmi_recv_msg_work,
		  cnss_wlfw_clnt_notifier_work);

	plat_priv->qmi_wlfw_clnt_nb.notifier_call =
		cnss_wlfw_clnt_svc_event_notifier;

	ret = qmi_svc_event_notifier_register(WLFW_SERVICE_ID_V01,
					      WLFW_SERVICE_VERS_V01,
					      WLFW_SERVICE_INS_ID_V01,
					      &plat_priv->qmi_wlfw_clnt_nb);
	if (ret < 0)
		cnss_pr_err("Failed to register QMI event notifier, err = %d\n",
			    ret);

	return ret;
}

void cnss_qmi_deinit(struct cnss_plat_data *plat_priv)
{
	qmi_svc_event_notifier_unregister(WLFW_SERVICE_ID_V01,
					  WLFW_SERVICE_VERS_V01,
					  WLFW_SERVICE_INS_ID_V01,
					  &plat_priv->qmi_wlfw_clnt_nb);
}
