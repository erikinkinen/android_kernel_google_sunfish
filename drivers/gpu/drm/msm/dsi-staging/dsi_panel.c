/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"msm-dsi-panel:[%s:%d] " fmt, __func__, __LINE__
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <video/mipi_display.h>
#include <video/display_timing.h>

#include "dsi_panel.h"
#include "dsi_ctrl_hw.h"
#include "dsi_parser.h"
#include "sde_trace.h"

/**
 * topology is currently defined by a set of following 3 values:
 * 1. num of layer mixers
 * 2. num of compression encoders
 * 3. num of interfaces
 */
#define TOPOLOGY_SET_LEN 3
#define MAX_TOPOLOGY 5

#define DSI_PANEL_DEFAULT_LABEL  "Default dsi panel"
#define DSI_PANEL_VENDOR_DEFAULT_LABEL "Undefined vendor"

#define DEFAULT_MDP_TRANSFER_TIME 14000
#define HBM_SV_MAX_MS (10 * 60 * 1000) /* 10 min */
#define DEFAULT_PANEL_JITTER_NUMERATOR		2
#define DEFAULT_PANEL_JITTER_DENOMINATOR	1
#define DEFAULT_PANEL_JITTER_ARRAY_SIZE		2
#define MAX_PANEL_JITTER		10
#define DEFAULT_PANEL_PREFILL_LINES	25
#define TICKS_IN_MICRO_SECOND		1000000

enum dsi_dsc_ratio_type {
	DSC_8BPC_8BPP,
	DSC_10BPC_8BPP,
	DSC_12BPC_8BPP,
	DSC_RATIO_TYPE_MAX
};

static u32 dsi_dsc_rc_buf_thresh[] = {0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54,
		0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e};

/*
 * DSC 1.1
 * Rate control - Min QP values for each ratio type in dsi_dsc_ratio_type
 */
static char dsi_dsc_rc_range_min_qp_1_1[][15] = {
	{0, 0, 1, 1, 3, 3, 3, 3, 3, 3, 5, 5, 5, 7, 13},
	{0, 4, 5, 5, 7, 7, 7, 7, 7, 7, 9, 9, 9, 11, 17},
	{0, 4, 9, 9, 11, 11, 11, 11, 11, 11, 13, 13, 13, 15, 21},
	};

/*
 * DSC 1.1 SCR
 * Rate control - Min QP values for each ratio type in dsi_dsc_ratio_type
 */
static char dsi_dsc_rc_range_min_qp_1_1_scr1[][15] = {
	{0, 0, 1, 1, 3, 3, 3, 3, 3, 3, 5, 5, 5, 9, 12},
	{0, 4, 5, 5, 7, 7, 7, 7, 7, 7, 9, 9, 9, 13, 16},
	{0, 4, 9, 9, 11, 11, 11, 11, 11, 11, 13, 13, 13, 17, 20},
	};

/*
 * DSC 1.1
 * Rate control - Max QP values for each ratio type in dsi_dsc_ratio_type
 */
static char dsi_dsc_rc_range_max_qp_1_1[][15] = {
	{4, 4, 5, 6, 7, 7, 7, 8, 9, 10, 11, 12, 13, 13, 15},
	{4, 8, 9, 10, 11, 11, 11, 12, 13, 14, 15, 16, 17, 17, 19},
	{12, 12, 13, 14, 15, 15, 15, 16, 17, 18, 19, 20, 21, 21, 23},
	};

/*
 * DSC 1.1 SCR
 * Rate control - Max QP values for each ratio type in dsi_dsc_ratio_type
 */
static char dsi_dsc_rc_range_max_qp_1_1_scr1[][15] = {
	{4, 4, 5, 6, 7, 7, 7, 8, 9, 10, 10, 11, 11, 12, 13},
	{8, 8, 9, 10, 11, 11, 11, 12, 13, 14, 14, 15, 15, 16, 17},
	{12, 12, 13, 14, 15, 15, 15, 16, 17, 18, 18, 19, 19, 20, 23},
	};

/*
 * DSC 1.1 and DSC 1.1 SCR
 * Rate control - bpg offset values
 */
static char dsi_dsc_rc_range_bpg_offset[] = {2, 0, 0, -2, -4, -6, -8, -8,
		-8, -10, -10, -12, -12, -12, -12};

/*
 * VR utility function.  Forward-declared so that other commands
 * can clear VR mode if necessary.
 */

static int dsi_panel_clear_vr_locked(struct dsi_panel *panel);
static int dsi_panel_update_hbm_locked(struct dsi_panel *panel,
					enum hbm_mode_type hbm_mode);

int dsi_dsc_create_pps_buf_cmd(struct msm_display_dsc_info *dsc, char *buf,
				int pps_id)
{
	char *bp;
	char data;
	int i, bpp;
	char *dbgbp;

	dbgbp = buf;
	bp = buf;
	/* First 7 bytes are cmd header */
	*bp++ = 0x0A;
	*bp++ = 1;
	*bp++ = 0;
	*bp++ = 0;
	*bp++ = 10;
	*bp++ = 0;
	*bp++ = 128;

	*bp++ = (dsc->version & 0xff);		/* pps0 */
	*bp++ = (pps_id & 0xff);		/* pps1 */
	bp++;					/* pps2, reserved */

	data = dsc->line_buf_depth & 0x0f;
	data |= ((dsc->bpc & 0xf) << 4);
	*bp++ = data;				/* pps3 */

	bpp = dsc->bpp;
	bpp <<= 4;				/* 4 fraction bits */
	data = (bpp >> 8);
	data &= 0x03;				/* upper two bits */
	data |= ((dsc->block_pred_enable & 0x1) << 5);
	data |= ((dsc->convert_rgb & 0x1) << 4);
	data |= ((dsc->enable_422 & 0x1) << 3);
	data |= ((dsc->vbr_enable & 0x1) << 2);
	*bp++ = data;				/* pps4 */
	*bp++ = (bpp & 0xff);			/* pps5 */

	*bp++ = ((dsc->pic_height >> 8) & 0xff); /* pps6 */
	*bp++ = (dsc->pic_height & 0x0ff);	/* pps7 */
	*bp++ = ((dsc->pic_width >> 8) & 0xff);	/* pps8 */
	*bp++ = (dsc->pic_width & 0x0ff);	/* pps9 */

	*bp++ = ((dsc->slice_height >> 8) & 0xff);/* pps10 */
	*bp++ = (dsc->slice_height & 0x0ff);	/* pps11 */
	*bp++ = ((dsc->slice_width >> 8) & 0xff); /* pps12 */
	*bp++ = (dsc->slice_width & 0x0ff);	/* pps13 */

	*bp++ = ((dsc->chunk_size >> 8) & 0xff);/* pps14 */
	*bp++ = (dsc->chunk_size & 0x0ff);	/* pps15 */

	*bp++ = (dsc->initial_xmit_delay >> 8) & 0x3; /* pps16, bit 0, 1 */
	*bp++ = (dsc->initial_xmit_delay & 0xff);/* pps17 */

	*bp++ = ((dsc->initial_dec_delay >> 8) & 0xff); /* pps18 */
	*bp++ = (dsc->initial_dec_delay & 0xff);/* pps19 */

	bp++;					/* pps20, reserved */

	*bp++ = (dsc->initial_scale_value & 0x3f); /* pps21 */

	*bp++ = ((dsc->scale_increment_interval >> 8) & 0xff); /* pps22 */
	*bp++ = (dsc->scale_increment_interval & 0xff); /* pps23 */

	*bp++ = ((dsc->scale_decrement_interval >> 8) & 0xf); /* pps24 */
	*bp++ = (dsc->scale_decrement_interval & 0x0ff);/* pps25 */

	bp++;					/* pps26, reserved */

	*bp++ = (dsc->first_line_bpg_offset & 0x1f);/* pps27 */

	*bp++ = ((dsc->nfl_bpg_offset >> 8) & 0xff);/* pps28 */
	*bp++ = (dsc->nfl_bpg_offset & 0x0ff);	/* pps29 */
	*bp++ = ((dsc->slice_bpg_offset >> 8) & 0xff);/* pps30 */
	*bp++ = (dsc->slice_bpg_offset & 0x0ff);/* pps31 */

	*bp++ = ((dsc->initial_offset >> 8) & 0xff);/* pps32 */
	*bp++ = (dsc->initial_offset & 0x0ff);	/* pps33 */

	*bp++ = ((dsc->final_offset >> 8) & 0xff);/* pps34 */
	*bp++ = (dsc->final_offset & 0x0ff);	/* pps35 */

	*bp++ = (dsc->min_qp_flatness & 0x1f);	/* pps36 */
	*bp++ = (dsc->max_qp_flatness & 0x1f);	/* pps37 */

	*bp++ = ((dsc->rc_model_size >> 8) & 0xff);/* pps38 */
	*bp++ = (dsc->rc_model_size & 0x0ff);	/* pps39 */

	*bp++ = (dsc->edge_factor & 0x0f);	/* pps40 */

	*bp++ = (dsc->quant_incr_limit0 & 0x1f);	/* pps41 */
	*bp++ = (dsc->quant_incr_limit1 & 0x1f);	/* pps42 */

	data = ((dsc->tgt_offset_hi & 0xf) << 4);
	data |= (dsc->tgt_offset_lo & 0x0f);
	*bp++ = data;				/* pps43 */

	for (i = 0; i < 14; i++)
		*bp++ = (dsc->buf_thresh[i] & 0xff); /* pps44 - pps57 */

	for (i = 0; i < 15; i++) {		/* pps58 - pps87 */
		data = (dsc->range_min_qp[i] & 0x1f);
		data <<= 3;
		data |= ((dsc->range_max_qp[i] >> 2) & 0x07);
		*bp++ = data;
		data = (dsc->range_max_qp[i] & 0x03);
		data <<= 6;
		data |= (dsc->range_bpg_offset[i] & 0x3f);
		*bp++ = data;
	}

	return 128;
}

static int dsi_panel_vreg_get(struct dsi_panel *panel)
{
	int rc = 0;
	int i;
	struct regulator *vreg = NULL;

	for (i = 0; i < panel->power_info.count; i++) {
		vreg = devm_regulator_get(panel->parent,
					  panel->power_info.vregs[i].vreg_name);
		rc = PTR_RET(vreg);
		if (rc) {
			pr_err("failed to get %s regulator\n",
			       panel->power_info.vregs[i].vreg_name);
			goto error_put;
		}
		panel->power_info.vregs[i].vreg = vreg;
	}

	return rc;
error_put:
	for (i = i - 1; i >= 0; i--) {
		devm_regulator_put(panel->power_info.vregs[i].vreg);
		panel->power_info.vregs[i].vreg = NULL;
	}
	return rc;
}

static int dsi_panel_vreg_put(struct dsi_panel *panel)
{
	int rc = 0;
	int i;

	for (i = panel->power_info.count - 1; i >= 0; i--)
		devm_regulator_put(panel->power_info.vregs[i].vreg);

	return rc;
}

static int dsi_panel_gpio_request(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_reset_config *r_config = &panel->reset_config;

	if (gpio_is_valid(r_config->reset_gpio)) {
		rc = gpio_request(r_config->reset_gpio, "reset_gpio");
		if (rc) {
			pr_err("request for reset_gpio failed, rc=%d\n", rc);
			goto error;
		}
	}

	if (gpio_is_valid(r_config->disp_en_gpio)) {
		rc = gpio_request(r_config->disp_en_gpio, "disp_en_gpio");
		if (rc) {
			pr_err("request for disp_en_gpio failed, rc=%d\n", rc);
			goto error_release_reset;
		}
	}

	if (gpio_is_valid(panel->bl_config.en_gpio)) {
		rc = gpio_request(panel->bl_config.en_gpio, "bklt_en_gpio");
		if (rc) {
			pr_err("request for bklt_en_gpio failed, rc=%d\n", rc);
			goto error_release_disp_en;
		}
	}

	if (gpio_is_valid(r_config->lcd_mode_sel_gpio)) {
		rc = gpio_request(r_config->lcd_mode_sel_gpio, "mode_gpio");
		if (rc) {
			pr_err("request for mode_gpio failed, rc=%d\n", rc);
			goto error_release_mode_sel;
		}
	}

	goto error;
error_release_mode_sel:
	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_free(panel->bl_config.en_gpio);
error_release_disp_en:
	if (gpio_is_valid(r_config->disp_en_gpio))
		gpio_free(r_config->disp_en_gpio);
error_release_reset:
	if (gpio_is_valid(r_config->reset_gpio))
		gpio_free(r_config->reset_gpio);
error:
	return rc;
}

static int dsi_panel_gpio_release(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_reset_config *r_config = &panel->reset_config;

	if (gpio_is_valid(r_config->reset_gpio))
		gpio_free(r_config->reset_gpio);

	if (gpio_is_valid(r_config->disp_en_gpio))
		gpio_free(r_config->disp_en_gpio);

	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_free(panel->bl_config.en_gpio);

	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		gpio_free(panel->reset_config.lcd_mode_sel_gpio);

	return rc;
}

int dsi_panel_trigger_esd_attack(struct dsi_panel *panel)
{
	struct dsi_panel_reset_config *r_config;

	if (!panel) {
		pr_err("Invalid panel param\n");
		return -EINVAL;
	}

	r_config = &panel->reset_config;
	if (!r_config) {
		pr_err("Invalid panel reset configuration\n");
		return -EINVAL;
	}

	if (gpio_is_valid(r_config->reset_gpio)) {
		gpio_set_value(r_config->reset_gpio, 0);
		pr_info("GPIO pulled low to simulate ESD\n");
		return 0;
	}
	pr_err("failed to pull down gpio\n");
	return -EINVAL;
}

static int dsi_panel_reset(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_reset_config *r_config = &panel->reset_config;
	int i;

	if (gpio_is_valid(panel->reset_config.disp_en_gpio)) {
		rc = gpio_direction_output(panel->reset_config.disp_en_gpio, 1);
		if (rc) {
			pr_err("unable to set dir for disp gpio rc=%d\n", rc);
			goto exit;
		}
	}

	if (r_config->count) {
		rc = gpio_direction_output(r_config->reset_gpio,
			r_config->sequence[0].level);
		if (rc) {
			pr_err("unable to set dir for rst gpio rc=%d\n", rc);
			goto exit;
		}
	}

	for (i = 0; i < r_config->count; i++) {
		gpio_set_value(r_config->reset_gpio,
			       r_config->sequence[i].level);


		if (r_config->sequence[i].sleep_ms)
			usleep_range(r_config->sequence[i].sleep_ms * 1000,
				(r_config->sequence[i].sleep_ms * 1000) + 100);
	}

	if (gpio_is_valid(panel->bl_config.en_gpio)) {
		rc = gpio_direction_output(panel->bl_config.en_gpio, 1);
		if (rc)
			pr_err("unable to set dir for bklt gpio rc=%d\n", rc);
	}

	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio)) {
		bool out = true;

		if ((panel->reset_config.mode_sel_state == MODE_SEL_DUAL_PORT)
				|| (panel->reset_config.mode_sel_state
					== MODE_GPIO_LOW))
			out = false;
		else if ((panel->reset_config.mode_sel_state
				== MODE_SEL_SINGLE_PORT) ||
				(panel->reset_config.mode_sel_state
				 == MODE_GPIO_HIGH))
			out = true;

		rc = gpio_direction_output(
			panel->reset_config.lcd_mode_sel_gpio, out);
		if (rc)
			pr_err("unable to set dir for mode gpio rc=%d\n", rc);
	}
exit:
	return rc;
}

static int dsi_panel_set_pinctrl_state(struct dsi_panel *panel, bool enable)
{
	int rc = 0;
	struct pinctrl_state *state;

	if (panel->host_config.ext_bridge_num)
		return 0;

	if (enable)
		state = panel->pinctrl.active;
	else
		state = panel->pinctrl.suspend;

	rc = pinctrl_select_state(panel->pinctrl.pinctrl, state);
	if (rc)
		pr_err("[%s] failed to set pin state, rc=%d\n", panel->name,
		       rc);

	return rc;
}


static int dsi_panel_power_on(struct dsi_panel *panel)
{
	int rc = 0;

	rc = dsi_panel_set_pinctrl_state(panel, true);
	if (rc) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", panel->name, rc);
		goto error_disable_vregs;
	}

	rc = dsi_panel_reset(panel);
	if (rc) {
		pr_err("[%s] failed to reset panel, rc=%d\n", panel->name, rc);
		goto error_disable_gpio;
	}

	goto exit;

error_disable_gpio:
	if (gpio_is_valid(panel->reset_config.disp_en_gpio))
		gpio_set_value(panel->reset_config.disp_en_gpio, 0);

	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_set_value(panel->bl_config.en_gpio, 0);

	(void)dsi_panel_set_pinctrl_state(panel, false);

error_disable_vregs:
	(void)dsi_pwr_enable_regulator(&panel->power_info, false);

exit:
	return rc;
}

static int dsi_panel_power_off(struct dsi_panel *panel)
{
	int rc = 0;

	if (gpio_is_valid(panel->reset_config.disp_en_gpio))
		gpio_set_value(panel->reset_config.disp_en_gpio, 0);

	if (gpio_is_valid(panel->reset_config.reset_gpio))
		gpio_set_value(panel->reset_config.reset_gpio, 0);

	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		gpio_set_value(panel->reset_config.lcd_mode_sel_gpio, 0);

	rc = dsi_panel_set_pinctrl_state(panel, false);
	if (rc) {
		pr_err("[%s] failed set pinctrl state, rc=%d\n", panel->name,
		       rc);
	}

	rc = dsi_pwr_enable_regulator(&panel->power_info, false);
	if (rc)
		pr_err("[%s] failed to enable vregs, rc=%d\n", panel->name, rc);

	return rc;
}

int dsi_panel_cmd_set_transfer(struct dsi_panel *panel,
			       struct dsi_panel_cmd_set *cmd)
{
	int rc = 0, i = 0;
	ssize_t len;
	struct dsi_cmd_desc *cmds;
	const struct mipi_dsi_host_ops *ops = panel->host->ops;

	cmds = cmd->cmds;

	if (cmd->count == 0) {
		pr_debug("[%s] No commands to be sent\n", panel->name);
		goto error;
	}

	for (i = 0; i < cmd->count; i++) {
		if (cmd->state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;

		if (cmds->last_command)
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

		len = ops->transfer(panel->host, &cmds->msg);
		if (len < 0) {
			rc = len;
			pr_err("failed to set cmds, rc=%d\n", rc);
			goto error;
		}
		if (cmds->post_wait_ms)
			usleep_range(cmds->post_wait_ms*1000,
					((cmds->post_wait_ms*1000)+10));
		cmds++;
	}
error:
	return rc;
}

static int dsi_panel_tx_cmd_set(struct dsi_panel *panel,
				enum dsi_cmd_set_type type)
{
	struct dsi_display_mode *mode;

	if (!panel || !panel->cur_mode)
		return -EINVAL;

	mode = panel->cur_mode;

	return dsi_panel_cmd_set_transfer(panel,
					  &mode->priv_info->cmd_sets[type]);
}

static int dsi_panel_pinctrl_deinit(struct dsi_panel *panel)
{
	int rc = 0;

	if (panel->host_config.ext_bridge_num)
		return 0;

	devm_pinctrl_put(panel->pinctrl.pinctrl);

	return rc;
}

static int dsi_panel_pinctrl_init(struct dsi_panel *panel)
{
	int rc = 0;

	if (panel->host_config.ext_bridge_num)
		return 0;

	/* TODO:  pinctrl is defined in dsi dt node */
	panel->pinctrl.pinctrl = devm_pinctrl_get(panel->parent);
	if (IS_ERR_OR_NULL(panel->pinctrl.pinctrl)) {
		rc = PTR_ERR(panel->pinctrl.pinctrl);
		pr_err("failed to get pinctrl, rc=%d\n", rc);
		goto error;
	}

	panel->pinctrl.active = pinctrl_lookup_state(panel->pinctrl.pinctrl,
						       "panel_active");
	if (IS_ERR_OR_NULL(panel->pinctrl.active)) {
		rc = PTR_ERR(panel->pinctrl.active);
		pr_err("failed to get pinctrl active state, rc=%d\n", rc);
		goto error;
	}

	panel->pinctrl.suspend =
		pinctrl_lookup_state(panel->pinctrl.pinctrl, "panel_suspend");

	if (IS_ERR_OR_NULL(panel->pinctrl.suspend)) {
		rc = PTR_ERR(panel->pinctrl.suspend);
		pr_err("failed to get pinctrl suspend state, rc=%d\n", rc);
		goto error;
	}

error:
	return rc;
}

static int dsi_panel_parse_timing(struct dsi_mode_info *mode,
				  struct dsi_parser_utils *utils)
{
	int rc = 0;
	u64 tmp64 = 0;
	u32 val = 0;
	struct dsi_display_mode *display_mode;
	struct dsi_display_mode_priv_info *priv_info;

	display_mode = container_of(mode, struct dsi_display_mode, timing);

	priv_info = display_mode->priv_info;

	rc = utils->read_u64(utils->data,
			"qcom,mdss-dsi-panel-clockrate", &tmp64);
	if (rc == -EOVERFLOW) {
		tmp64 = 0;
		rc = utils->read_u32(utils->data,
			"qcom,mdss-dsi-panel-clockrate", (u32 *)&tmp64);
	}

	mode->clk_rate_hz = !rc ? tmp64 : 0;
	display_mode->priv_info->clk_rate_hz = mode->clk_rate_hz;

	rc = utils->read_u32(utils->data, "qcom,mdss-mdp-transfer-time-us",
			&mode->mdp_transfer_time_us);
	if (rc) {
		pr_debug("fallback to default mdp-transfer-time-us\n");
		mode->mdp_transfer_time_us = DEFAULT_MDP_TRANSFER_TIME;
	}
	display_mode->priv_info->mdp_transfer_time_us =
					mode->mdp_transfer_time_us;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-overlap-pixels",
				  &val);
	priv_info->overlap_pixels = rc ? 0 : val;

	rc = utils->read_u32(utils->data,
				"qcom,mdss-dsi-panel-framerate",
				&mode->refresh_rate);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-panel-framerate, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-panel-width",
				  &mode->h_active);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-panel-width, rc=%d\n", rc);
		goto error;
	}

	rc = utils->read_u32(utils->data,
				"qcom,mdss-dsi-h-front-porch",
				  &mode->h_front_porch);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-h-front-porch, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data,
				"qcom,mdss-dsi-h-back-porch",
				  &mode->h_back_porch);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-h-back-porch, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data,
				"qcom,mdss-dsi-h-pulse-width",
				  &mode->h_sync_width);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-h-pulse-width, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-h-sync-skew",
				  &mode->h_skew);
	if (rc)
		pr_err("qcom,mdss-dsi-h-sync-skew is not defined, rc=%d\n", rc);

	pr_debug("panel horz active:%d front_portch:%d back_porch:%d sync_skew:%d\n",
		mode->h_active, mode->h_front_porch, mode->h_back_porch,
		mode->h_sync_width);

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-panel-height",
				  &mode->v_active);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-panel-height, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-v-back-porch",
				  &mode->v_back_porch);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-v-back-porch, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-v-front-porch",
				  &mode->v_front_porch);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-v-back-porch, rc=%d\n",
		       rc);
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-v-pulse-width",
				  &mode->v_sync_width);
	if (rc) {
		pr_err("failed to read qcom,mdss-dsi-v-pulse-width, rc=%d\n",
		       rc);
		goto error;
	}
	pr_debug("panel vert active:%d front_portch:%d back_porch:%d pulse_width:%d\n",
		mode->v_active, mode->v_front_porch, mode->v_back_porch,
		mode->v_sync_width);

error:
	return rc;
}

static int dsi_panel_parse_pixel_format(struct dsi_host_common_cfg *host,
					struct dsi_parser_utils *utils,
					const char *name)
{
	int rc = 0;
	u32 bpp = 0;
	enum dsi_pixel_format fmt;
	const char *packing;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-bpp", &bpp);
	if (rc) {
		pr_err("[%s] failed to read qcom,mdss-dsi-bpp, rc=%d\n",
		       name, rc);
		return rc;
	}

	switch (bpp) {
	case 3:
		fmt = DSI_PIXEL_FORMAT_RGB111;
		break;
	case 8:
		fmt = DSI_PIXEL_FORMAT_RGB332;
		break;
	case 12:
		fmt = DSI_PIXEL_FORMAT_RGB444;
		break;
	case 16:
		fmt = DSI_PIXEL_FORMAT_RGB565;
		break;
	case 18:
		fmt = DSI_PIXEL_FORMAT_RGB666;
		break;
	case 24:
	default:
		fmt = DSI_PIXEL_FORMAT_RGB888;
		break;
	}

	if (fmt == DSI_PIXEL_FORMAT_RGB666) {
		packing = utils->get_property(utils->data,
					  "qcom,mdss-dsi-pixel-packing",
					  NULL);
		if (packing && !strcmp(packing, "loose"))
			fmt = DSI_PIXEL_FORMAT_RGB666_LOOSE;
	}

	host->dst_format = fmt;
	return rc;
}

static int dsi_panel_parse_lane_states(struct dsi_host_common_cfg *host,
				       struct dsi_parser_utils *utils,
				       const char *name)
{
	int rc = 0;
	bool lane_enabled;

	lane_enabled = utils->read_bool(utils->data,
					    "qcom,mdss-dsi-lane-0-state");
	host->data_lanes |= (lane_enabled ? DSI_DATA_LANE_0 : 0);

	lane_enabled = utils->read_bool(utils->data,
					     "qcom,mdss-dsi-lane-1-state");
	host->data_lanes |= (lane_enabled ? DSI_DATA_LANE_1 : 0);

	lane_enabled = utils->read_bool(utils->data,
					    "qcom,mdss-dsi-lane-2-state");
	host->data_lanes |= (lane_enabled ? DSI_DATA_LANE_2 : 0);

	lane_enabled = utils->read_bool(utils->data,
					     "qcom,mdss-dsi-lane-3-state");
	host->data_lanes |= (lane_enabled ? DSI_DATA_LANE_3 : 0);

	if (host->data_lanes == 0) {
		pr_err("[%s] No data lanes are enabled, rc=%d\n", name, rc);
		rc = -EINVAL;
	}

	return rc;
}

static int dsi_panel_parse_color_swap(struct dsi_host_common_cfg *host,
				      struct dsi_parser_utils *utils,
				      const char *name)
{
	int rc = 0;
	const char *swap_mode;

	swap_mode = utils->get_property(utils->data,
			"qcom,mdss-dsi-color-order", NULL);
	if (swap_mode) {
		if (!strcmp(swap_mode, "rgb_swap_rgb")) {
			host->swap_mode = DSI_COLOR_SWAP_RGB;
		} else if (!strcmp(swap_mode, "rgb_swap_rbg")) {
			host->swap_mode = DSI_COLOR_SWAP_RBG;
		} else if (!strcmp(swap_mode, "rgb_swap_brg")) {
			host->swap_mode = DSI_COLOR_SWAP_BRG;
		} else if (!strcmp(swap_mode, "rgb_swap_grb")) {
			host->swap_mode = DSI_COLOR_SWAP_GRB;
		} else if (!strcmp(swap_mode, "rgb_swap_gbr")) {
			host->swap_mode = DSI_COLOR_SWAP_GBR;
		} else {
			pr_err("[%s] Unrecognized color order-%s\n",
			       name, swap_mode);
			rc = -EINVAL;
		}
	} else {
		pr_debug("[%s] Falling back to default color order\n", name);
		host->swap_mode = DSI_COLOR_SWAP_RGB;
	}

	/* bit swap on color channel is not defined in dt */
	host->bit_swap_red = false;
	host->bit_swap_green = false;
	host->bit_swap_blue = false;
	return rc;
}

static int dsi_panel_parse_triggers(struct dsi_host_common_cfg *host,
				    struct dsi_parser_utils *utils,
				    const char *name)
{
	const char *trig;
	int rc = 0;

	trig = utils->get_property(utils->data,
			"qcom,mdss-dsi-mdp-trigger", NULL);
	if (trig) {
		if (!strcmp(trig, "none")) {
			host->mdp_cmd_trigger = DSI_TRIGGER_NONE;
		} else if (!strcmp(trig, "trigger_te")) {
			host->mdp_cmd_trigger = DSI_TRIGGER_TE;
		} else if (!strcmp(trig, "trigger_sw")) {
			host->mdp_cmd_trigger = DSI_TRIGGER_SW;
		} else if (!strcmp(trig, "trigger_sw_te")) {
			host->mdp_cmd_trigger = DSI_TRIGGER_SW_TE;
		} else {
			pr_err("[%s] Unrecognized mdp trigger type (%s)\n",
			       name, trig);
			rc = -EINVAL;
		}

	} else {
		pr_debug("[%s] Falling back to default MDP trigger\n",
			 name);
		host->mdp_cmd_trigger = DSI_TRIGGER_SW;
	}

	trig = utils->get_property(utils->data,
			"qcom,mdss-dsi-dma-trigger", NULL);
	if (trig) {
		if (!strcmp(trig, "none")) {
			host->dma_cmd_trigger = DSI_TRIGGER_NONE;
		} else if (!strcmp(trig, "trigger_te")) {
			host->dma_cmd_trigger = DSI_TRIGGER_TE;
		} else if (!strcmp(trig, "trigger_sw")) {
			host->dma_cmd_trigger = DSI_TRIGGER_SW;
		} else if (!strcmp(trig, "trigger_sw_seof")) {
			host->dma_cmd_trigger = DSI_TRIGGER_SW_SEOF;
		} else if (!strcmp(trig, "trigger_sw_te")) {
			host->dma_cmd_trigger = DSI_TRIGGER_SW_TE;
		} else {
			pr_err("[%s] Unrecognized mdp trigger type (%s)\n",
			       name, trig);
			rc = -EINVAL;
		}

	} else {
		pr_debug("[%s] Falling back to default MDP trigger\n", name);
		host->dma_cmd_trigger = DSI_TRIGGER_SW;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-te-pin-select",
			&host->te_mode);
	if (rc) {
		pr_warn("[%s] fallback to default te-pin-select\n", name);
		host->te_mode = 1;
		rc = 0;
	}

	return rc;
}

static int dsi_panel_parse_ext_bridge_config(struct dsi_host_common_cfg *host,
					    struct dsi_parser_utils *utils,
					    const char *name)
{
	u32 len = 0, i = 0;
	int rc = 0;

	host->ext_bridge_num = 0;

	len = utils->count_u32_elems(utils->data, "qcom,mdss-dsi-ext-bridge");

	if (len > MAX_DSI_CTRLS_PER_DISPLAY) {
		pr_debug("[%s] Invalid ext bridge count set\n", name);
		return -EINVAL;
	}

	if (len == 0) {
		pr_debug("[%s] It's a DSI panel, not bridge\n", name);
		return rc;
	}

	rc = utils->read_u32_array(utils->data, "qcom,mdss-dsi-ext-bridge",
			host->ext_bridge_map,
			len);

	if (rc) {
		pr_debug("[%s] Did not get ext bridge set\n", name);
		return rc;
	}

	for (i = 0; i < len; i++) {
		if (host->ext_bridge_map[i] >= MAX_EXT_BRIDGE_PORT_CONFIG) {
			pr_debug("[%s] Invalid bridge port value %d\n",
				name, host->ext_bridge_map[i]);
			return -EINVAL;
		}
	}

	host->ext_bridge_num = len;

	pr_debug("[%s] ext bridge count is %d\n", name, host->ext_bridge_num);

	return rc;
}

static int dsi_panel_parse_misc_host_config(struct dsi_host_common_cfg *host,
					    struct dsi_parser_utils *utils,
					    const char *name)
{
	u32 val = 0;
	int rc = 0;
	bool panel_cphy_mode = false;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-t-clk-post", &val);
	if (!rc) {
		host->t_clk_post = val;
		pr_debug("[%s] t_clk_post = %d\n", name, val);
	}

	val = 0;
	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-t-clk-pre", &val);
	if (!rc) {
		host->t_clk_pre = val;
		pr_debug("[%s] t_clk_pre = %d\n", name, val);
	}

	host->ignore_rx_eot = utils->read_bool(utils->data,
						"qcom,mdss-dsi-rx-eot-ignore");

	host->append_tx_eot = utils->read_bool(utils->data,
						"qcom,mdss-dsi-tx-eot-append");

	host->force_hs_clk_lane = utils->read_bool(utils->data,
					"qcom,mdss-dsi-force-clock-lane-hs");
	panel_cphy_mode = utils->read_bool(utils->data,
					"qcom,panel-cphy-mode");
	host->phy_type = panel_cphy_mode ? DSI_PHY_TYPE_CPHY
						: DSI_PHY_TYPE_DPHY;

	return 0;
}

static int dsi_panel_create_sn_buf(struct dsi_panel *panel)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;
	struct dsi_panel_sn_location *const location = &vendor_info->location;
	ssize_t rc = 0;
	u32 sn_str_size;
	u8 *tmp_sn_buf;

	if (!panel)
		return -EINVAL;

	if  (!location->addr || !location->sn_length) {
		pr_err("[%s] invalid location\n", __func__);
		return -EINVAL;
	}

	// prepare buffer for bin2hex()
	// e.g. buf[0] = 0x01 -> sn[0] = '0' and sn[1] = '1'
	sn_str_size = location->sn_length * 2;
	tmp_sn_buf = kmalloc(sn_str_size + 1, GFP_KERNEL);
	if (!tmp_sn_buf)
		return -ENOMEM;

	mutex_lock(&panel->panel_lock);
	if (!vendor_info->sn) {
		vendor_info->is_sn = false;
		vendor_info->sn = tmp_sn_buf;
	} else
		kfree(tmp_sn_buf);

	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_panel_release_sn_buf(struct dsi_panel *panel)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;
	ssize_t rc = 0;

	if (!panel)
		return -EINVAL;

	mutex_lock(&panel->panel_lock);
	kfree(vendor_info->sn);
	vendor_info->sn = NULL;
	vendor_info->is_sn = false;
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_get_sn(struct dsi_panel *panel)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;
	struct dsi_panel_sn_location *const location = &vendor_info->location;
	ssize_t rc = 0;
	u32 read_size, read_back_size, sn_str_size;
	u8 *buf;

	if (!panel || !panel->panel_initialized) {
		pr_err("panel is not ready\n");
		return -EINVAL;
	}

	read_size = location->start_byte + location->sn_length;
	buf = kmalloc(read_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&panel->panel_lock);

	if (!panel->vendor_info.sn) {
		rc = -EINVAL;
		goto out_mutex;
	}

	read_back_size = mipi_dsi_dcs_read(&panel->mipi_device, location->addr,
					   buf, read_size);
	if (read_back_size == read_size) {
		bin2hex(vendor_info->sn, &buf[location->start_byte],
			location->sn_length);
		// e.g. buf[0] = 0x01 -> sn[0] = '0' and sn[1] = '1'
		sn_str_size = location->sn_length * 2;
		vendor_info->sn[sn_str_size] = '\0';
		vendor_info->is_sn = true;
	} else {
		rc = -EINVAL;
		pr_err("failed to read: addr=0x%X, read_back_size=%d, read_size=%d\n",
		      location->addr, read_back_size, read_size);
	}

	kfree(buf);
out_mutex:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_panel_parse_sn_location(struct dsi_panel *panel,
				       struct device_node *of_node)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;
	int rc = 0, len = 0;
	u32 array[3] = {0};

	if (!panel)
		return -EINVAL;

	len = of_property_count_u32_elems(of_node,
			"google,mdss-dsi-panel-sn-location");
	if (len != 3) {
		pr_err("[%s] invalid format\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	rc = of_property_read_u32_array(of_node,
			"google,mdss-dsi-panel-sn-location", array, len);
	if (rc || !array[0] || !array[2]) {
		pr_err("[%s] invalid format\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	vendor_info->location.addr = array[0];
	vendor_info->location.start_byte = array[1];
	vendor_info->location.sn_length = array[2];

	pr_debug("addr=0x%x, start=%d, length=%d",
		vendor_info->location.addr, vendor_info->location.start_byte,
		vendor_info->location.sn_length);
error:
	return rc;
}

int dsi_panel_get_vendor_extinfo(struct dsi_panel *panel)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;
	char buffer[128];
	size_t bytes = 0;
	u32 read_addr, read_start, read_len, dsi_read_len;
	int i, rc = 0;

	mutex_lock(&panel->panel_lock);

	/* Transmit the commands as specified. */
	for (i = 0; i < vendor_info->extinfo_loc_length; i += 3) {
		read_addr  = vendor_info->extinfo_loc[i];
		read_start = vendor_info->extinfo_loc[i + 1];
		read_len   = vendor_info->extinfo_loc[i + 2];

		/* Compute size of entire DSI read, starting from byte zero. */
		dsi_read_len = read_start + read_len;

		if (dsi_read_len > sizeof(buffer)) {
			pr_err("Read is too large for buffer.\n");
			rc = -EINVAL;
			goto error;
		} else if (read_len > vendor_info->extinfo_length - bytes) {
			pr_err("Ran out of space reading extinfo data.\n");
			rc = -EINVAL;
			goto error;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device,
				       read_addr,
				       buffer,
				       dsi_read_len);
		if (rc < 0) {
			pr_err("mipi_dsi_dcs_read failed with rc=%d.\n", rc);
			goto error;
		}

		/* Copy only the the specified portion of the DSI read, which is
		 * read_len bytes starting at read_start.
		 */
		memcpy(&vendor_info->extinfo[bytes], &buffer[read_start],
		       read_len);
		bytes += read_len;
	}

	vendor_info->extinfo_read = bytes;
	rc = 0;

error:

	if (rc < 0) {
		pr_err("Failed to read vendor extinfo. Freeing extinfo memory.\n");

		vendor_info->extinfo_loc_length = 0;
		vendor_info->extinfo_length = 0;
		vendor_info->extinfo_read = 0;

		kfree(vendor_info->extinfo_loc);
		vendor_info->extinfo_loc = NULL;

		kfree(vendor_info->extinfo);
		vendor_info->extinfo = NULL;
	}

	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_panel_release_vendor_extinfo(struct dsi_panel *panel)
{
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;

	mutex_lock(&panel->panel_lock);

	vendor_info->extinfo_loc_length = 0;
	vendor_info->extinfo_length = 0;
	vendor_info->extinfo_read = 0;

	kfree(vendor_info->extinfo_loc);
	vendor_info->extinfo_loc = NULL;

	kfree(vendor_info->extinfo);
	vendor_info->extinfo = NULL;

	mutex_unlock(&panel->panel_lock);
	return 0;
}

static int dsi_panel_parse_vendor_extinfo_location(struct dsi_panel *panel,
						   struct device_node *of_node)
{
	u32 extinfo_location[24];
	int size_bytes, loc_length;
	int extinfo_length = 0;
	int i, rc = 0;

	if (!of_property_read_bool(
			of_node, "google,mdss-dsi-panel-vendor-extinfo-loc")) {
		pr_info("Could not find optional vendor extinfo.\n");
		rc = 0;
		goto error;
	}

	if (IS_ERR(of_get_property(of_node,
				   "google,mdss-dsi-panel-vendor-extinfo-loc",
				   &size_bytes))) {
		pr_err("Failed to read google,mdss-dsi-panel-vendor-extinfo-loc.\n");
		goto error;
	}
	loc_length = size_bytes / 4;

	if (size_bytes > sizeof(extinfo_location)) {
		pr_err("Length (%d) is larger than allocated (%d).\n",
		       size_bytes, sizeof(extinfo_location));
		rc = -EINVAL;
		goto error;
	} else if (size_bytes % 4 != 0 || loc_length % 3 != 0) {
		pr_err("Size (%d) is not 32-bit aligned and/or the length (%d) is not divisible by 3.\n",
		       size_bytes, loc_length);
		rc = -EINVAL;
		goto error;
	}

	rc = of_property_read_u32_array(
			of_node, "google,mdss-dsi-panel-vendor-extinfo-loc",
			extinfo_location, loc_length);
	if (rc) {
		pr_err("Error (%d) reading extinfo location.\n", rc);
		goto error;
	}

	/* Compute expected length of extinfo */
	for (i = 0; i < loc_length; i += 3) {
		/* Each register read consists of three u32 values specifying
		 * the register address, start byte of the target range, and the
		 * length of the target range. Length is the 3rd u32.
		 */
		extinfo_length += extinfo_location[i + 2];
	}

	panel->vendor_info.extinfo_loc = kzalloc(size_bytes, GFP_KERNEL);
	panel->vendor_info.extinfo = kzalloc(extinfo_length, GFP_KERNEL);
	if (!panel->vendor_info.extinfo_loc || !panel->vendor_info.extinfo) {
		pr_err("Failed to allocate extinfo buffer memory.\n");

		kfree(panel->vendor_info.extinfo_loc);
		panel->vendor_info.extinfo_loc = NULL;

		kfree(panel->vendor_info.extinfo);
		panel->vendor_info.extinfo = NULL;

		rc = -ENOMEM;
		goto error;
	}
	memcpy(panel->vendor_info.extinfo_loc, extinfo_location, size_bytes);
	panel->vendor_info.extinfo_loc_length = loc_length;
	panel->vendor_info.extinfo_length = extinfo_length;
	rc = 0;

error:
	return rc;
}

/*
 * Copy the display panel extended info to a buffer provided by the caller.
 *
 * panel  - pointer to a drm_panel
 * buffer - destination for extinfo data
 * len    - size of destination buffer
 *
 * If there is no extinfo to read, returns 0
 * If provided buffer is NULL, returns the size of the extinfo in bytes
 * If len is less than the extinfo length, returns -ENOSPC
 * If there is extinfo pending read, returns -EBUSY
 * Otherwise, copies extinfo to buffer and returns the number of bytes copied
 */
int dsi_panel_read_vendor_extinfo(struct drm_panel *panel, char *buffer,
				  size_t len)
{
	struct dsi_panel *p = container_of(panel, struct dsi_panel, drm_panel);
	struct dsi_panel_vendor_info *const vendor_info = &p->vendor_info;

	if (!panel)
		return -EINVAL;

	/* Return zero if there is no extinfo to read */
	if (vendor_info->extinfo_loc_length == 0)
		return 0;

	/* If buffer is null, this is a request for the length of extinfo */
	if (!buffer)
		return vendor_info->extinfo_length;

	/* Verify adequate buffer space */
	if (len < vendor_info->extinfo_length)
		return -ENOSPC;

	/* Return -EBUSY if there is extinfo to read, but read is pending */
	if (vendor_info->extinfo_read == 0)
		return -EBUSY;

	memcpy(buffer, vendor_info->extinfo, vendor_info->extinfo_length);
	return vendor_info->extinfo_length;
}
EXPORT_SYMBOL(dsi_panel_read_vendor_extinfo);

static int dsi_panel_parse_vendor_info(struct dsi_panel *panel,
				       struct device_node *of_node)
{
	int rc = 0;
	struct dsi_panel_vendor_info *const vendor_info = &panel->vendor_info;

	if (!panel)
		return -EINVAL;

	vendor_info->name = of_get_property(of_node,
				"google,mdss-dsi-panel-vendor", NULL);

	if (!vendor_info->name) {
		vendor_info->name = DSI_PANEL_VENDOR_DEFAULT_LABEL;
		rc = -EINVAL;
		goto error;
	}
	rc = dsi_panel_parse_sn_location(panel, of_node);
	if (rc) {
		pr_err("[%s] failed to parse the parameter for SN, rc=%d\n",
			panel->name, rc);
	}
	rc = dsi_panel_parse_vendor_extinfo_location(panel, of_node);
	if (rc) {
		pr_err("[%s] failed to parse the extended panel info, rc=%d\n",
			panel->name, rc);
	}
error:
	return rc;
}

static void dsi_panel_parse_split_link_config(struct dsi_host_common_cfg *host,
					struct dsi_parser_utils *utils,
					const char *name)
{
	int rc = 0;
	u32 val = 0;
	bool supported = false;
	struct dsi_split_link_config *split_link = &host->split_link;

	supported = utils->read_bool(utils->data, "qcom,split-link-enabled");

	if (!supported) {
		pr_debug("[%s] Split link is not supported\n", name);
		split_link->split_link_enabled = false;
		return;
	}

	rc = utils->read_u32(utils->data, "qcom,sublinks-count", &val);
	if (rc || val < 1) {
		pr_debug("[%s] Using default sublinks count\n", name);
		split_link->num_sublinks = 2;
	} else {
		split_link->num_sublinks = val;
	}

	rc = utils->read_u32(utils->data, "qcom,lanes-per-sublink", &val);
	if (rc || val < 1) {
		pr_debug("[%s] Using default lanes per sublink\n", name);
		split_link->lanes_per_sublink = 2;
	} else {
		split_link->lanes_per_sublink = val;
	}

	pr_debug("[%s] Split link is supported %d-%d\n", name,
		split_link->num_sublinks, split_link->lanes_per_sublink);
	split_link->split_link_enabled = true;
}

static int dsi_panel_parse_host_config(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_parser_utils *utils = &panel->utils;

	rc = dsi_panel_parse_pixel_format(&panel->host_config, utils,
					  panel->name);
	if (rc) {
		pr_err("[%s] failed to get pixel format, rc=%d\n",
		panel->name, rc);
		goto error;
	}

	rc = dsi_panel_parse_lane_states(&panel->host_config, utils,
					 panel->name);
	if (rc) {
		pr_err("[%s] failed to parse lane states, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

	rc = dsi_panel_parse_color_swap(&panel->host_config, utils,
					panel->name);
	if (rc) {
		pr_err("[%s] failed to parse color swap config, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

	rc = dsi_panel_parse_triggers(&panel->host_config, utils,
				      panel->name);
	if (rc) {
		pr_err("[%s] failed to parse triggers, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

	rc = dsi_panel_parse_misc_host_config(&panel->host_config, utils,
					      panel->name);
	if (rc) {
		pr_err("[%s] failed to parse misc host config, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

	dsi_panel_parse_ext_bridge_config(&panel->host_config, utils,
					      panel->name);
	if (rc) {
		pr_err("[%s] failed to parse ext bridge config, rc=%d\n",
		       panel->name, rc);
	}

	dsi_panel_parse_split_link_config(&panel->host_config, utils,
						panel->name);

error:
	return rc;
}

static int dsi_panel_parse_qsync_caps(struct dsi_panel *panel,
				     struct device_node *of_node)
{
	int rc = 0;
	u32 val = 0;

	rc = of_property_read_u32(of_node,
				  "qcom,mdss-dsi-qsync-min-refresh-rate",
				  &val);
	if (rc)
		pr_err("[%s] qsync min fps not defined rc:%d\n",
			panel->name, rc);

	panel->qsync_min_fps = val;

	return rc;
}

static int dsi_panel_parse_dyn_clk_caps(struct dsi_panel *panel)
{
	int rc = 0;
	bool supported = false;
	struct dsi_dyn_clk_caps *dyn_clk_caps = &panel->dyn_clk_caps;
	struct dsi_parser_utils *utils = &panel->utils;
	const char *name = panel->name;

	supported = utils->read_bool(utils->data, "qcom,dsi-dyn-clk-enable");

	if (!supported) {
		dyn_clk_caps->dyn_clk_support = false;
		return rc;
	}

	dyn_clk_caps->bit_clk_list_len = utils->count_u32_elems(utils->data,
			"qcom,dsi-dyn-clk-list");

	if (dyn_clk_caps->bit_clk_list_len < 1) {
		pr_err("[%s] failed to get supported bit clk list\n", name);
		return -EINVAL;
	}

	dyn_clk_caps->bit_clk_list = kcalloc(dyn_clk_caps->bit_clk_list_len,
			sizeof(u32), GFP_KERNEL);
	if (!dyn_clk_caps->bit_clk_list)
		return -ENOMEM;

	rc = utils->read_u32_array(utils->data, "qcom,dsi-dyn-clk-list",
			dyn_clk_caps->bit_clk_list,
			dyn_clk_caps->bit_clk_list_len);

	if (rc) {
		pr_err("[%s] failed to parse supported bit clk list\n", name);
		return -EINVAL;
	}

	dyn_clk_caps->dyn_clk_support = true;

	return 0;
}

static int dsi_panel_parse_dfps_caps(struct dsi_panel *panel)
{
	int rc = 0;
	bool supported = false;
	struct dsi_dfps_capabilities *dfps_caps = &panel->dfps_caps;
	struct dsi_parser_utils *utils = &panel->utils;
	const char *name = panel->name;
	const char *type;
	u32 i;

	supported = utils->read_bool(utils->data,
			"qcom,mdss-dsi-pan-enable-dynamic-fps");

	if (!supported) {
		pr_debug("[%s] DFPS is not supported\n", name);
		dfps_caps->dfps_support = false;
		return rc;
	}

	type = utils->get_property(utils->data,
			"qcom,mdss-dsi-pan-fps-update", NULL);
	if (!type) {
		pr_err("[%s] dfps type not defined\n", name);
		rc = -EINVAL;
		goto error;
	} else if (!strcmp(type, "dfps_suspend_resume_mode")) {
		dfps_caps->type = DSI_DFPS_SUSPEND_RESUME;
	} else if (!strcmp(type, "dfps_immediate_clk_mode")) {
		dfps_caps->type = DSI_DFPS_IMMEDIATE_CLK;
	} else if (!strcmp(type, "dfps_immediate_porch_mode_hfp")) {
		dfps_caps->type = DSI_DFPS_IMMEDIATE_HFP;
	} else if (!strcmp(type, "dfps_immediate_porch_mode_vfp")) {
		dfps_caps->type = DSI_DFPS_IMMEDIATE_VFP;
	} else {
		pr_err("[%s] dfps type is not recognized\n", name);
		rc = -EINVAL;
		goto error;
	}

	dfps_caps->dfps_list_len = utils->count_u32_elems(utils->data,
				  "qcom,dsi-supported-dfps-list");
	if (dfps_caps->dfps_list_len < 1) {
		pr_err("[%s] dfps refresh list not present\n", name);
		rc = -EINVAL;
		goto error;
	}

	dfps_caps->dfps_list = kcalloc(dfps_caps->dfps_list_len, sizeof(u32),
			GFP_KERNEL);
	if (!dfps_caps->dfps_list) {
		rc = -ENOMEM;
		goto error;
	}

	rc = utils->read_u32_array(utils->data,
			"qcom,dsi-supported-dfps-list",
			dfps_caps->dfps_list,
			dfps_caps->dfps_list_len);
	if (rc) {
		pr_err("[%s] dfps refresh rate list parse failed\n", name);
		rc = -EINVAL;
		goto error;
	}
	dfps_caps->dfps_support = true;

	/* calculate max and min fps */
	dfps_caps->max_refresh_rate = dfps_caps->dfps_list[0];
	dfps_caps->min_refresh_rate = dfps_caps->dfps_list[0];

	for (i = 1; i < dfps_caps->dfps_list_len; i++) {
		if (dfps_caps->dfps_list[i] < dfps_caps->min_refresh_rate)
			dfps_caps->min_refresh_rate = dfps_caps->dfps_list[i];
		else if (dfps_caps->dfps_list[i] > dfps_caps->max_refresh_rate)
			dfps_caps->max_refresh_rate = dfps_caps->dfps_list[i];
	}

error:
	return rc;
}

static int dsi_panel_parse_video_host_config(struct dsi_video_engine_cfg *cfg,
					     struct dsi_parser_utils *utils,
					     const char *name)
{
	int rc = 0;
	const char *traffic_mode;
	u32 vc_id = 0;
	u32 val = 0;
	u32 line_no = 0;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-h-sync-pulse", &val);
	if (rc) {
		pr_debug("[%s] fallback to default h-sync-pulse\n", name);
		cfg->pulse_mode_hsa_he = false;
	} else if (val == 1) {
		cfg->pulse_mode_hsa_he = true;
	} else if (val == 0) {
		cfg->pulse_mode_hsa_he = false;
	} else {
		pr_err("[%s] Unrecognized value for mdss-dsi-h-sync-pulse\n",
		       name);
		rc = -EINVAL;
		goto error;
	}

	cfg->hfp_lp11_en = utils->read_bool(utils->data,
						"qcom,mdss-dsi-hfp-power-mode");

	cfg->hbp_lp11_en = utils->read_bool(utils->data,
						"qcom,mdss-dsi-hbp-power-mode");

	cfg->hsa_lp11_en = utils->read_bool(utils->data,
						"qcom,mdss-dsi-hsa-power-mode");

	cfg->last_line_interleave_en = utils->read_bool(utils->data,
					"qcom,mdss-dsi-last-line-interleave");

	cfg->eof_bllp_lp11_en = utils->read_bool(utils->data,
					"qcom,mdss-dsi-bllp-eof-power-mode");

	cfg->bllp_lp11_en = utils->read_bool(utils->data,
					"qcom,mdss-dsi-bllp-power-mode");

	traffic_mode = utils->get_property(utils->data,
				       "qcom,mdss-dsi-traffic-mode",
				       NULL);
	if (!traffic_mode) {
		pr_debug("[%s] Falling back to default traffic mode\n", name);
		cfg->traffic_mode = DSI_VIDEO_TRAFFIC_SYNC_PULSES;
	} else if (!strcmp(traffic_mode, "non_burst_sync_pulse")) {
		cfg->traffic_mode = DSI_VIDEO_TRAFFIC_SYNC_PULSES;
	} else if (!strcmp(traffic_mode, "non_burst_sync_event")) {
		cfg->traffic_mode = DSI_VIDEO_TRAFFIC_SYNC_START_EVENTS;
	} else if (!strcmp(traffic_mode, "burst_mode")) {
		cfg->traffic_mode = DSI_VIDEO_TRAFFIC_BURST_MODE;
	} else {
		pr_err("[%s] Unrecognized traffic mode-%s\n", name,
		       traffic_mode);
		rc = -EINVAL;
		goto error;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-virtual-channel-id",
				  &vc_id);
	if (rc) {
		pr_debug("[%s] Fallback to default vc id\n", name);
		cfg->vc_id = 0;
	} else {
		cfg->vc_id = vc_id;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-dma-schedule-line",
				  &line_no);
	if (rc) {
		pr_debug("[%s] set default dma scheduling line no\n", name);
		cfg->dma_sched_line = 0x1;
		/* do not fail since we have default value */
		rc = 0;
	} else {
		cfg->dma_sched_line = line_no;
	}

error:
	return rc;
}

static int dsi_panel_parse_cmd_host_config(struct dsi_cmd_engine_cfg *cfg,
					   struct dsi_parser_utils *utils,
					   const char *name)
{
	u32 val = 0;
	int rc = 0;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-wr-mem-start", &val);
	if (rc) {
		pr_debug("[%s] Fallback to default wr-mem-start\n", name);
		cfg->wr_mem_start = 0x2C;
	} else {
		cfg->wr_mem_start = val;
	}

	val = 0;
	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-wr-mem-continue",
				  &val);
	if (rc) {
		pr_debug("[%s] Fallback to default wr-mem-continue\n", name);
		cfg->wr_mem_continue = 0x3C;
	} else {
		cfg->wr_mem_continue = val;
	}

	/* TODO:  fix following */
	cfg->max_cmd_packets_interleave = 0;

	val = 0;
	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-te-dcs-command",
				  &val);
	if (rc) {
		pr_debug("[%s] fallback to default te-dcs-cmd\n", name);
		cfg->insert_dcs_command = true;
	} else if (val == 1) {
		cfg->insert_dcs_command = true;
	} else if (val == 0) {
		cfg->insert_dcs_command = false;
	} else {
		pr_err("[%s] Unrecognized value for mdss-dsi-te-dcs-command\n",
		       name);
		rc = -EINVAL;
		goto error;
	}

error:
	return rc;
}

static int dsi_panel_parse_panel_mode(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_parser_utils *utils = &panel->utils;
	enum dsi_op_mode panel_mode;
	const char *mode;

	mode = utils->get_property(utils->data,
			"qcom,mdss-dsi-panel-type", NULL);
	if (!mode) {
		pr_debug("[%s] Fallback to default panel mode\n", panel->name);
		panel_mode = DSI_OP_VIDEO_MODE;
	} else if (!strcmp(mode, "dsi_video_mode")) {
		panel_mode = DSI_OP_VIDEO_MODE;
	} else if (!strcmp(mode, "dsi_cmd_mode")) {
		panel_mode = DSI_OP_CMD_MODE;
	} else {
		pr_err("[%s] Unrecognized panel type-%s\n", panel->name, mode);
		rc = -EINVAL;
		goto error;
	}

	if (panel_mode == DSI_OP_VIDEO_MODE) {
		rc = dsi_panel_parse_video_host_config(&panel->video_config,
						       utils,
						       panel->name);
		if (rc) {
			pr_err("[%s] Failed to parse video host cfg, rc=%d\n",
			       panel->name, rc);
			goto error;
		}
	}

	if (panel_mode == DSI_OP_CMD_MODE) {
		rc = dsi_panel_parse_cmd_host_config(&panel->cmd_config,
						     utils,
						     panel->name);
		if (rc) {
			pr_err("[%s] Failed to parse cmd host config, rc=%d\n",
			       panel->name, rc);
			goto error;
		}
	}

	panel->panel_mode = panel_mode;
error:
	return rc;
}

static int dsi_panel_parse_phy_props(struct dsi_panel *panel)
{
	int rc = 0;
	u32 val = 0;
	const char *str;
	struct dsi_panel_phy_props *props = &panel->phy_props;
	struct dsi_parser_utils *utils = &panel->utils;
	const char *name = panel->name;

	rc = utils->read_u32(utils->data,
		  "qcom,mdss-pan-physical-width-dimension", &val);
	if (rc) {
		pr_debug("[%s] Physical panel width is not defined\n", name);
		props->panel_width_mm = 0;
		rc = 0;
	} else {
		props->panel_width_mm = val;
	}

	rc = utils->read_u32(utils->data,
				  "qcom,mdss-pan-physical-height-dimension",
				  &val);
	if (rc) {
		pr_debug("[%s] Physical panel height is not defined\n", name);
		props->panel_height_mm = 0;
		rc = 0;
	} else {
		props->panel_height_mm = val;
	}

	str = utils->get_property(utils->data,
			"qcom,mdss-dsi-panel-orientation", NULL);
	if (!str) {
		props->rotation = DSI_PANEL_ROTATE_NONE;
	} else if (!strcmp(str, "180")) {
		props->rotation = DSI_PANEL_ROTATE_HV_FLIP;
	} else if (!strcmp(str, "hflip")) {
		props->rotation = DSI_PANEL_ROTATE_H_FLIP;
	} else if (!strcmp(str, "vflip")) {
		props->rotation = DSI_PANEL_ROTATE_V_FLIP;
	} else {
		pr_err("[%s] Unrecognized panel rotation-%s\n", name, str);
		rc = -EINVAL;
		goto error;
	}
error:
	return rc;
}
const char *cmd_set_prop_map[DSI_CMD_SET_MAX] = {
	"qcom,mdss-dsi-pre-on-command",
	"qcom,mdss-dsi-on-command",
	"qcom,mdss-dsi-post-panel-on-command",
	"qcom,mdss-dsi-pre-off-command",
	"qcom,mdss-dsi-off-command",
	"qcom,mdss-dsi-post-off-command",
	"qcom,mdss-dsi-pre-res-switch",
	"qcom,mdss-dsi-res-switch",
	"qcom,mdss-dsi-post-res-switch",
	"qcom,cmd-to-video-mode-switch-commands",
	"qcom,cmd-to-video-mode-post-switch-commands",
	"qcom,video-to-cmd-mode-switch-commands",
	"qcom,video-to-cmd-mode-post-switch-commands",
	"qcom,mdss-dsi-panel-status-command",
	"qcom,mdss-dsi-lp1-command",
	"qcom,mdss-dsi-lp2-command",
	"qcom,mdss-dsi-nolp-command",
	"qcom,mdss-dsi-post-nolp-command",
	"qcom,mdss-dsi-vr-command",
	"qcom,mdss-dsi-novr-command",
	"PPS not parsed from DTSI, generated dynamically",
	"ROI not parsed from DTSI, generated dynamically",
	"qcom,mdss-dsi-timing-switch-command",
	"qcom,mdss-dsi-post-mode-switch-on-command",
	"qcom,mdss-dsi-qsync-on-commands",
	"qcom,mdss-dsi-qsync-off-commands",
};

const char *cmd_set_state_map[DSI_CMD_SET_MAX] = {
	"qcom,mdss-dsi-pre-on-command-state",
	"qcom,mdss-dsi-on-command-state",
	"qcom,mdss-dsi-post-on-command-state",
	"qcom,mdss-dsi-pre-off-command-state",
	"qcom,mdss-dsi-off-command-state",
	"qcom,mdss-dsi-post-off-command-state",
	"qcom,mdss-dsi-pre-res-switch-state",
	"qcom,mdss-dsi-res-switch-state",
	"qcom,mdss-dsi-post-res-switch-state",
	"qcom,cmd-to-video-mode-switch-commands-state",
	"qcom,cmd-to-video-mode-post-switch-commands-state",
	"qcom,video-to-cmd-mode-switch-commands-state",
	"qcom,video-to-cmd-mode-post-switch-commands-state",
	"qcom,mdss-dsi-panel-status-command-state",
	"qcom,mdss-dsi-lp1-command-state",
	"qcom,mdss-dsi-lp2-command-state",
	"qcom,mdss-dsi-nolp-command-state",
	"qcom,mdss-dsi-post-nolp-command-state",
	"qcom,mdss-dsi-vr-command-state",
	"qcom,mdss-dsi-novr-command-state",
	"PPS not parsed from DTSI, generated dynamically",
	"ROI not parsed from DTSI, generated dynamically",
	"qcom,mdss-dsi-timing-switch-command-state",
	"qcom,mdss-dsi-post-mode-switch-on-command-state",
	"qcom,mdss-dsi-qsync-on-commands-state",
	"qcom,mdss-dsi-qsync-off-commands-state",
};

static int dsi_panel_get_cmd_pkt_count(const char *data, u32 length, u32 *cnt)
{
	const u32 cmd_set_min_size = 7;
	u32 count = 0;
	u32 packet_length;
	u32 tmp;

	while (length >= cmd_set_min_size) {
		packet_length = cmd_set_min_size;
		tmp = ((data[5] << 8) | (data[6]));
		packet_length += tmp;
		if (packet_length > length) {
			pr_err("format error\n");
			return -EINVAL;
		}
		length -= packet_length;
		data += packet_length;
		count++;
	};

	*cnt = count;
	return 0;
}

static int dsi_panel_create_cmd_packets(const char *data,
					u32 length,
					u32 count,
					struct dsi_cmd_desc *cmd)
{
	int rc = 0;
	int i, j;
	u8 *payload;

	for (i = 0; i < count; i++) {
		u32 size;

		cmd[i].msg.type = data[0];
		cmd[i].last_command = (data[1] == 1 ? true : false);
		cmd[i].msg.channel = data[2];
		cmd[i].msg.flags |= (data[3] == 1 ? MIPI_DSI_MSG_REQ_ACK : 0);
		cmd[i].msg.ctrl = 0;
		cmd[i].post_wait_ms = cmd[i].msg.wait_ms = data[4];
		cmd[i].msg.tx_len = ((data[5] << 8) | (data[6]));

		size = cmd[i].msg.tx_len * sizeof(u8);

		payload = kzalloc(size, GFP_KERNEL);
		if (!payload) {
			rc = -ENOMEM;
			goto error_free_payloads;
		}

		for (j = 0; j < cmd[i].msg.tx_len; j++)
			payload[j] = data[7 + j];

		cmd[i].msg.tx_buf = payload;
		data += (7 + cmd[i].msg.tx_len);
	}

	return rc;
error_free_payloads:
	for (i = i - 1; i >= 0; i--) {
		kfree(cmd[i].msg.tx_buf);
		cmd[i].msg.tx_buf = NULL;
	}

	return rc;
}


static void dsi_panel_destroy_cmds_packets_buf(struct dsi_panel_cmd_set *set)
{
	u32 i = 0;
	struct dsi_cmd_desc *cmd;

	for (i = 0; i < set->count; i++) {
		cmd = &set->cmds[i];
		kfree(cmd->msg.tx_buf);
		cmd->msg.tx_buf = NULL;
	}
}

void dsi_panel_destroy_cmd_packets(struct dsi_panel_cmd_set *set)
{

	dsi_panel_destroy_cmds_packets_buf(set);
	kfree(set->cmds);
}

static int dsi_panel_alloc_cmd_packets(struct dsi_panel_cmd_set *cmd,
					u32 packet_count)
{
	u32 size;

	size = packet_count * sizeof(*cmd->cmds);
	cmd->cmds = kzalloc(size, GFP_KERNEL);
	if (!cmd->cmds)
		return -ENOMEM;

	cmd->count = packet_count;
	return 0;
}

static int dsi_panel_parse_cmd_sets_sub(struct dsi_panel_cmd_set *cmd,
					const char *data,
					size_t length)
{
	int rc = 0;
	u32 packet_count = 0;

	rc = dsi_panel_get_cmd_pkt_count(data, length, &packet_count);
	if (rc) {
		pr_err("commands failed, rc=%d\n", rc);
		goto error;
	}
	pr_debug("packet-count=%d, %d\n", packet_count, length);

	rc = dsi_panel_alloc_cmd_packets(cmd, packet_count);
	if (rc) {
		pr_err("failed to allocate cmd packets, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_create_cmd_packets(data, length, packet_count,
					  cmd->cmds);
	if (rc) {
		pr_err("failed to create cmd packets, rc=%d\n", rc);
		goto error_free_mem;
	}

	return rc;
error_free_mem:
	kfree(cmd->cmds);
	cmd->cmds = NULL;
error:
	return rc;
}

int dsi_panel_parse_dt_cmd_set(struct device_node *of_node,
			       const char *cmd_str,
			       const char *cmd_state_str,
			       struct dsi_panel_cmd_set *cmd)
{
	const char *data;
	const char *state;
	enum dsi_cmd_set_state st;
	u32 length = 0;
	int rc;

	data = of_get_property(of_node, cmd_str, &length);
	if (!data) {
		pr_debug("%s commands not defined\n", cmd_str);
		return -ENOTSUPP;
	}

	pr_debug("name=%s, length=%d\n", cmd_str, length);

	print_hex_dump_debug("", DUMP_PREFIX_NONE,
		8, 1, data, length, false);

	state = of_get_property(of_node, cmd_state_str, NULL);
	if (!state || !strcmp(state, "dsi_lp_mode")) {
		st = DSI_CMD_SET_STATE_LP;
	} else if (!strcmp(state, "dsi_hs_mode")) {
		st = DSI_CMD_SET_STATE_HS;
	} else {
		pr_err("[%s] command state unrecognized-%s\n",
		       cmd_state_str, state);
		return -ENOTSUPP;
	}

	rc = dsi_panel_parse_cmd_sets_sub(cmd, data, length);
	if (rc)
		return rc;

	cmd->state = st;

	return 0;
}

static int dsi_panel_parse_cmd_sets_dt(struct dsi_panel_cmd_set *cmd,
				       enum dsi_cmd_set_type type,
				       struct dsi_parser_utils *utils)
{
	return dsi_panel_parse_dt_cmd_set(utils->data, cmd_set_prop_map[type],
					  cmd_set_state_map[type], cmd);
}

static int dsi_panel_parse_cmd_sets(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils)
{
	int rc = 0;
	struct dsi_panel_cmd_set *set;
	u32 i;

	if (!priv_info) {
		pr_err("invalid mode priv info\n");
		return -EINVAL;
	}

	for (i = DSI_CMD_SET_PRE_ON; i < DSI_CMD_SET_MAX; i++) {
		set = &priv_info->cmd_sets[i];
		set->type = i;
		set->count = 0;

		if (i == DSI_CMD_SET_PPS) {
			rc = dsi_panel_alloc_cmd_packets(set, 1);
			if (rc)
				pr_err("failed to allocate cmd set %d, rc = %d\n",
					i, rc);
			set->state = DSI_CMD_SET_STATE_LP;
		} else {
			rc = dsi_panel_parse_cmd_sets_dt(set, i, utils);
			if (rc)
				pr_debug("failed to parse set %d\n", i);
		}
	}

	rc = 0;
	return rc;
}

static int dsi_panel_parse_reset_sequence(struct dsi_panel *panel)
{
	int rc = 0;
	int i;
	u32 length = 0;
	u32 count = 0;
	u32 size = 0;
	u32 *arr_32 = NULL;
	const u32 *arr;
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_reset_seq *seq;

	if (panel->host_config.ext_bridge_num)
		return 0;

	arr = utils->get_property(utils->data,
			"qcom,mdss-dsi-reset-sequence", &length);
	if (!arr) {
		pr_err("[%s] dsi-reset-sequence not found\n", panel->name);
		rc = -EINVAL;
		goto error;
	}
	if (length & 0x1) {
		pr_err("[%s] syntax error for dsi-reset-sequence\n",
		       panel->name);
		rc = -EINVAL;
		goto error;
	}

	pr_err("RESET SEQ LENGTH = %d\n", length);
	length = length / sizeof(u32);

	size = length * sizeof(u32);

	arr_32 = kzalloc(size, GFP_KERNEL);
	if (!arr_32) {
		rc = -ENOMEM;
		goto error;
	}

	rc = utils->read_u32_array(utils->data, "qcom,mdss-dsi-reset-sequence",
					arr_32, length);
	if (rc) {
		pr_err("[%s] cannot read qcom,mdss-dsi-reset-sequence\n",
		       panel->name);
		goto error_free_arr_32;
	}

	count = length / 2;
	size = count * sizeof(*seq);
	seq = kzalloc(size, GFP_KERNEL);
	if (!seq) {
		rc = -ENOMEM;
		goto error_free_arr_32;
	}

	panel->reset_config.sequence = seq;
	panel->reset_config.count = count;

	for (i = 0; i < length; i += 2) {
		seq->level = arr_32[i];
		seq->sleep_ms = arr_32[i + 1];
		seq++;
	}


error_free_arr_32:
	kfree(arr_32);
error:
	return rc;
}

static int dsi_panel_parse_misc_features(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	u32 val;

	panel->ulps_feature_enabled =
		utils->read_bool(utils->data, "qcom,ulps-enabled");

	pr_info("%s: ulps feature %s\n", __func__,
		(panel->ulps_feature_enabled ? "enabled" : "disabled"));

	panel->ulps_suspend_enabled =
		utils->read_bool(utils->data, "qcom,suspend-ulps-enabled");

	pr_info("%s: ulps during suspend feature %s", __func__,
		(panel->ulps_suspend_enabled ? "enabled" : "disabled"));

	panel->te_using_watchdog_timer = utils->read_bool(utils->data,
					"qcom,mdss-dsi-te-using-wd");

	panel->sync_broadcast_en = utils->read_bool(utils->data,
			"qcom,cmd-sync-wait-broadcast");

	panel->lp11_init = utils->read_bool(utils->data,
			"qcom,mdss-dsi-lp11-init");
	if (!utils->read_u32(utils->data,
				  "qcom,mdss-dsi-init-delay-us",
				  &val)) {
		pr_debug("Panel init delay specified: %d\n", val);
		panel->init_delay_us = val;
	} else {
		panel->init_delay_us = 0;
	}

	return 0;
}

static int dsi_panel_parse_jitter_config(
				struct dsi_display_mode *mode,
				struct dsi_parser_utils *utils)
{
	int rc;
	struct dsi_display_mode_priv_info *priv_info;
	u32 jitter[DEFAULT_PANEL_JITTER_ARRAY_SIZE] = {0, 0};
	u64 jitter_val = 0;

	priv_info = mode->priv_info;

	rc = utils->read_u32_array(utils->data, "qcom,mdss-dsi-panel-jitter",
				jitter, DEFAULT_PANEL_JITTER_ARRAY_SIZE);
	if (rc) {
		pr_debug("panel jitter not defined rc=%d\n", rc);
	} else {
		jitter_val = jitter[0];
		jitter_val = div_u64(jitter_val, jitter[1]);
	}

	if (rc || !jitter_val || (jitter_val > MAX_PANEL_JITTER)) {
		priv_info->panel_jitter_numer = DEFAULT_PANEL_JITTER_NUMERATOR;
		priv_info->panel_jitter_denom =
					DEFAULT_PANEL_JITTER_DENOMINATOR;
	} else {
		priv_info->panel_jitter_numer = jitter[0];
		priv_info->panel_jitter_denom = jitter[1];
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-panel-prefill-lines",
				  &priv_info->panel_prefill_lines);
	if (rc) {
		pr_debug("panel prefill lines are not defined rc=%d\n", rc);
		priv_info->panel_prefill_lines = mode->timing.v_back_porch +
			mode->timing.v_sync_width + mode->timing.v_front_porch;
	} else if (priv_info->panel_prefill_lines >=
					DSI_V_TOTAL(&mode->timing)) {
		pr_debug("invalid prefill lines config=%d setting to:%d\n",
		priv_info->panel_prefill_lines, DEFAULT_PANEL_PREFILL_LINES);

		priv_info->panel_prefill_lines = DEFAULT_PANEL_PREFILL_LINES;
	}

	return 0;
}

static int dsi_panel_parse_power_cfg(struct dsi_panel *panel)
{
	int rc = 0;
	char *supply_name;

	if (panel->host_config.ext_bridge_num)
		return 0;

	if (!strcmp(panel->type, "primary"))
		supply_name = "qcom,panel-supply-entries";
	else
		supply_name = "qcom,panel-sec-supply-entries";

	rc = dsi_pwr_of_get_vreg_data(&panel->utils,
			&panel->power_info, supply_name);
	if (rc) {
		pr_err("[%s] failed to parse vregs\n", panel->name);
		goto error;
	}

error:
	return rc;
}

static int dsi_panel_parse_gpios(struct dsi_panel *panel)
{
	int rc = 0;
	const char *data;
	struct dsi_parser_utils *utils = &panel->utils;
	char *reset_gpio_name, *mode_set_gpio_name;

	if (!strcmp(panel->type, "primary")) {
		reset_gpio_name = "qcom,platform-reset-gpio";
		mode_set_gpio_name = "qcom,panel-mode-gpio";
	} else {
		reset_gpio_name = "qcom,platform-sec-reset-gpio";
		mode_set_gpio_name = "qcom,panel-sec-mode-gpio";
	}

	panel->reset_config.reset_gpio = utils->get_named_gpio(utils->data,
					      reset_gpio_name, 0);
	if (!gpio_is_valid(panel->reset_config.reset_gpio) &&
		!panel->host_config.ext_bridge_num) {
		rc = panel->reset_config.reset_gpio;
		pr_err("[%s] failed get reset gpio, rc=%d\n", panel->name, rc);
		goto error;
	}

	panel->reset_config.disp_en_gpio = utils->get_named_gpio(utils->data,
						"qcom,5v-boost-gpio",
						0);
	if (!gpio_is_valid(panel->reset_config.disp_en_gpio)) {
		pr_debug("[%s] 5v-boot-gpio is not set, rc=%d\n",
			 panel->name, rc);
		panel->reset_config.disp_en_gpio =
				utils->get_named_gpio(utils->data,
					"qcom,platform-en-gpio", 0);
		if (!gpio_is_valid(panel->reset_config.disp_en_gpio)) {
			pr_debug("[%s] platform-en-gpio is not set, rc=%d\n",
				 panel->name, rc);
		}
	}

	panel->reset_config.lcd_mode_sel_gpio = utils->get_named_gpio(
		utils->data, mode_set_gpio_name, 0);
	if (!gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		pr_debug("%s:%d mode gpio not specified\n", __func__, __LINE__);

	pr_debug("mode gpio=%d\n", panel->reset_config.lcd_mode_sel_gpio);

	data = utils->get_property(utils->data,
		"qcom,mdss-dsi-mode-sel-gpio-state", NULL);
	if (data) {
		if (!strcmp(data, "single_port"))
			panel->reset_config.mode_sel_state =
				MODE_SEL_SINGLE_PORT;
		else if (!strcmp(data, "dual_port"))
			panel->reset_config.mode_sel_state =
				MODE_SEL_DUAL_PORT;
		else if (!strcmp(data, "high"))
			panel->reset_config.mode_sel_state =
				MODE_GPIO_HIGH;
		else if (!strcmp(data, "low"))
			panel->reset_config.mode_sel_state =
				MODE_GPIO_LOW;
	} else {
		/* Set default mode as SPLIT mode */
		panel->reset_config.mode_sel_state = MODE_SEL_DUAL_PORT;
	}

	/* TODO:  release memory */
	rc = dsi_panel_parse_reset_sequence(panel);
	if (rc) {
		pr_err("[%s] failed to parse reset sequence, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

error:
	return rc;
}

void dsi_dsc_pclk_param_calc(struct msm_display_dsc_info *dsc, int intf_width)
{
	int slice_per_pkt, slice_per_intf;
	int bytes_in_slice, total_bytes_per_intf;

	if (!dsc || !dsc->slice_width || !dsc->slice_per_pkt ||
	    (intf_width < dsc->slice_width)) {
		pr_err("invalid input, intf_width=%d slice_width=%d\n",
			intf_width, dsc ? dsc->slice_width : -1);
		return;
	}

	slice_per_pkt = dsc->slice_per_pkt;
	slice_per_intf = DIV_ROUND_UP(intf_width, dsc->slice_width);

	/*
	 * If slice_per_pkt is greater than slice_per_intf then default to 1.
	 * This can happen during partial update.
	 */
	if (slice_per_pkt > slice_per_intf)
		slice_per_pkt = 1;

	bytes_in_slice = DIV_ROUND_UP(dsc->slice_width * dsc->bpp, 8);
	total_bytes_per_intf = bytes_in_slice * slice_per_intf;

	dsc->eol_byte_num = total_bytes_per_intf % 3;
	dsc->pclk_per_line =  DIV_ROUND_UP(total_bytes_per_intf, 3);
	dsc->bytes_in_slice = bytes_in_slice;
	dsc->bytes_per_pkt = bytes_in_slice * slice_per_pkt;
	dsc->pkt_per_line = slice_per_intf / slice_per_pkt;
}


int dsi_dsc_populate_static_param(struct msm_display_dsc_info *dsc)
{
	int bpp, bpc;
	int mux_words_size;
	int groups_per_line, groups_total;
	int min_rate_buffer_size;
	int hrd_delay;
	int pre_num_extra_mux_bits, num_extra_mux_bits;
	int slice_bits;
	int data;
	int final_value, final_scale;
	int ratio_index, mod_offset;

	dsc->rc_model_size = 8192;

	if (dsc->version == 0x11 && dsc->scr_rev == 0x1)
		dsc->first_line_bpg_offset = 15;
	else
		dsc->first_line_bpg_offset = 12;

	dsc->edge_factor = 6;
	dsc->tgt_offset_hi = 3;
	dsc->tgt_offset_lo = 3;
	dsc->enable_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	dsc->buf_thresh = dsi_dsc_rc_buf_thresh;

	bpp = dsc->bpp;
	bpc = dsc->bpc;

	if (bpc == 12)
		ratio_index = DSC_12BPC_8BPP;
	else if (bpc == 10)
		ratio_index = DSC_10BPC_8BPP;
	else
		ratio_index = DSC_8BPC_8BPP;

	if (dsc->version == 0x11 && dsc->scr_rev == 0x1) {
		dsc->range_min_qp =
			dsi_dsc_rc_range_min_qp_1_1_scr1[ratio_index];
		dsc->range_max_qp =
			dsi_dsc_rc_range_max_qp_1_1_scr1[ratio_index];
	} else {
		dsc->range_min_qp = dsi_dsc_rc_range_min_qp_1_1[ratio_index];
		dsc->range_max_qp = dsi_dsc_rc_range_max_qp_1_1[ratio_index];
	}
	dsc->range_bpg_offset = dsi_dsc_rc_range_bpg_offset;

	if (bpp <= 10)
		dsc->initial_offset = 6144;
	else
		dsc->initial_offset = 2048;	/* bpp = 12 */

	if (bpc == 12)
		mux_words_size = 64;
	else
		mux_words_size = 48;		/* bpc == 8/10 */

	dsc->line_buf_depth = bpc + 1;

	if (bpc == 8) {
		dsc->input_10_bits = 0;
		dsc->min_qp_flatness = 3;
		dsc->max_qp_flatness = 12;
		dsc->quant_incr_limit0 = 11;
		dsc->quant_incr_limit1 = 11;
	} else if (bpc == 10) { /* 10bpc */
		dsc->input_10_bits = 1;
		dsc->min_qp_flatness = 7;
		dsc->max_qp_flatness = 16;
		dsc->quant_incr_limit0 = 15;
		dsc->quant_incr_limit1 = 15;
	} else { /* 12 bpc */
		dsc->input_10_bits = 0;
		dsc->min_qp_flatness = 11;
		dsc->max_qp_flatness = 20;
		dsc->quant_incr_limit0 = 19;
		dsc->quant_incr_limit1 = 19;
	}

	mod_offset = dsc->slice_width % 3;
	switch (mod_offset) {
	case 0:
		dsc->slice_last_group_size = 2;
		break;
	case 1:
		dsc->slice_last_group_size = 0;
		break;
	case 2:
		dsc->slice_last_group_size = 1;
		break;
	default:
		break;
	}

	dsc->det_thresh_flatness = 2 << (bpc - 8);

	dsc->initial_xmit_delay = dsc->rc_model_size / (2 * bpp);

	groups_per_line = DIV_ROUND_UP(dsc->slice_width, 3);

	dsc->chunk_size = dsc->slice_width * bpp / 8;
	if ((dsc->slice_width * bpp) % 8)
		dsc->chunk_size++;

	/* rbs-min */
	min_rate_buffer_size =  dsc->rc_model_size - dsc->initial_offset +
			dsc->initial_xmit_delay * bpp +
			groups_per_line * dsc->first_line_bpg_offset;

	hrd_delay = DIV_ROUND_UP(min_rate_buffer_size, bpp);

	dsc->initial_dec_delay = hrd_delay - dsc->initial_xmit_delay;

	dsc->initial_scale_value = 8 * dsc->rc_model_size /
			(dsc->rc_model_size - dsc->initial_offset);

	slice_bits = 8 * dsc->chunk_size * dsc->slice_height;

	groups_total = groups_per_line * dsc->slice_height;

	data = dsc->first_line_bpg_offset * 2048;

	dsc->nfl_bpg_offset = DIV_ROUND_UP(data, (dsc->slice_height - 1));

	pre_num_extra_mux_bits = 3 * (mux_words_size + (4 * bpc + 4) - 2);

	num_extra_mux_bits = pre_num_extra_mux_bits - (mux_words_size -
		((slice_bits - pre_num_extra_mux_bits) % mux_words_size));

	data = 2048 * (dsc->rc_model_size - dsc->initial_offset
		+ num_extra_mux_bits);
	dsc->slice_bpg_offset = DIV_ROUND_UP(data, groups_total);

	data = dsc->initial_xmit_delay * bpp;
	final_value =  dsc->rc_model_size - data + num_extra_mux_bits;

	final_scale = 8 * dsc->rc_model_size /
		(dsc->rc_model_size - final_value);

	dsc->final_offset = final_value;

	data = (final_scale - 9) * (dsc->nfl_bpg_offset +
		dsc->slice_bpg_offset);
	dsc->scale_increment_interval = (2048 * dsc->final_offset) / data;

	dsc->scale_decrement_interval = groups_per_line /
		(dsc->initial_scale_value - 8);

	return 0;
}


static int dsi_panel_parse_phy_timing(struct dsi_display_mode *mode,
		struct dsi_parser_utils *utils, enum dsi_op_mode panel_mode)
{
	const char *data;
	u32 len, i;
	int rc = 0;
	struct dsi_display_mode_priv_info *priv_info;
	u64 h_period, v_period;
	u64 refresh_rate = TICKS_IN_MICRO_SECOND;
	struct dsi_mode_info *timing = NULL;
	u64 pixel_clk_khz;

	if (!mode || !mode->priv_info)
		return -EINVAL;

	priv_info = mode->priv_info;

	data = utils->get_property(utils->data,
			"qcom,mdss-dsi-panel-phy-timings", &len);
	if (!data) {
		pr_debug("Unable to read Phy timing settings");
	} else {
		priv_info->phy_timing_val =
			kzalloc((sizeof(u32) * len), GFP_KERNEL);
		if (!priv_info->phy_timing_val)
			return -EINVAL;

		for (i = 0; i < len; i++)
			priv_info->phy_timing_val[i] = data[i];

		priv_info->phy_timing_len = len;
	};

	timing = &mode->timing;

	if (panel_mode == DSI_OP_CMD_MODE) {
		h_period = DSI_H_ACTIVE_DSC(timing);
		v_period = timing->v_active;
		do_div(refresh_rate, priv_info->mdp_transfer_time_us);
	} else {
		h_period = DSI_H_TOTAL_DSC(timing);
		v_period = DSI_V_TOTAL(timing);
		refresh_rate = timing->refresh_rate;
	}

	pixel_clk_khz = h_period * v_period * refresh_rate;
	do_div(pixel_clk_khz, 1000);
	mode->pixel_clk_khz = pixel_clk_khz;

	return rc;
}

static int dsi_panel_parse_dsc_params(struct dsi_display_mode *mode,
				struct dsi_parser_utils *utils)
{
	u32 data;
	int rc = -EINVAL;
	int intf_width;
	const char *compression;
	struct dsi_display_mode_priv_info *priv_info;

	if (!mode || !mode->priv_info)
		return -EINVAL;

	priv_info = mode->priv_info;

	priv_info->dsc_enabled = false;
	mode->timing.dsc_enabled = false;
	compression = utils->get_property(utils->data,
			"qcom,compression-mode", NULL);
	if (compression && !strcmp(compression, "dsc")) {
		priv_info->dsc_enabled = true;
		mode->timing.dsc_enabled = true;
	}

	if (!priv_info->dsc_enabled) {
		pr_debug("dsc compression is not enabled for the mode");
		return 0;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-version", &data);
	if (rc) {
		priv_info->dsc.version = 0x11;
	} else {
		priv_info->dsc.version = data & 0xff;
		/* only support DSC 1.1 rev */
		if (priv_info->dsc.version != 0x11) {
			pr_err("%s: DSC version:%d not supported\n", __func__,
					priv_info->dsc.version);
			rc = -EINVAL;
			goto error;
		}
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-scr-version", &data);
	if (rc) {
		priv_info->dsc.scr_rev = 0x0;
	} else {
		priv_info->dsc.scr_rev = data & 0xff;
		/* only one scr rev supported */
		if (priv_info->dsc.scr_rev > 0x1) {
			pr_err("%s: DSC scr version:%d not supported\n",
					__func__, priv_info->dsc.scr_rev);
			rc = -EINVAL;
			goto error;
		}
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-slice-height", &data);
	if (rc) {
		pr_err("failed to parse qcom,mdss-dsc-slice-height\n");
		goto error;
	}
	priv_info->dsc.slice_height = data;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-slice-width", &data);
	if (rc) {
		pr_err("failed to parse qcom,mdss-dsc-slice-width\n");
		goto error;
	}
	priv_info->dsc.slice_width = data;

	intf_width = mode->timing.h_active;
	if (intf_width % priv_info->dsc.slice_width) {
		pr_err("invalid slice width for the intf width:%d slice width:%d\n",
			intf_width, priv_info->dsc.slice_width);
		rc = -EINVAL;
		goto error;
	}

	priv_info->dsc.pic_width = mode->timing.h_active;
	priv_info->dsc.pic_height = mode->timing.v_active;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-slice-per-pkt", &data);
	if (rc) {
		pr_err("failed to parse qcom,mdss-dsc-slice-per-pkt\n");
		goto error;
	} else if (!data || (data > 2)) {
		pr_err("invalid dsc slice-per-pkt:%d\n", data);
		goto error;
	}
	priv_info->dsc.slice_per_pkt = data;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-bit-per-component",
		&data);
	if (rc) {
		pr_err("failed to parse qcom,mdss-dsc-bit-per-component\n");
		goto error;
	}
	priv_info->dsc.bpc = data;

	rc = utils->read_u32(utils->data, "qcom,mdss-dsc-bit-per-pixel",
			&data);
	if (rc) {
		pr_err("failed to parse qcom,mdss-dsc-bit-per-pixel\n");
		goto error;
	}
	priv_info->dsc.bpp = data;

	priv_info->dsc.block_pred_enable = utils->read_bool(utils->data,
		"qcom,mdss-dsc-block-prediction-enable");

	priv_info->dsc.full_frame_slices = DIV_ROUND_UP(intf_width,
		priv_info->dsc.slice_width);

	dsi_dsc_populate_static_param(&priv_info->dsc);
	dsi_dsc_pclk_param_calc(&priv_info->dsc, intf_width);

	mode->timing.dsc = &priv_info->dsc;

	return 0;

error:
	priv_info->dsc_enabled = false;
	mode->timing.dsc_enabled = false;
	return rc;
}

static int dsi_panel_parse_hdr_config(struct dsi_panel *panel)
{
	int rc = 0;
	struct drm_panel_hdr_properties *hdr_prop;
	struct dsi_parser_utils *utils = &panel->utils;

	hdr_prop = &panel->hdr_props;
	hdr_prop->hdr_enabled = utils->read_bool(utils->data,
		"qcom,mdss-dsi-panel-hdr-enabled");

	if (hdr_prop->hdr_enabled) {
		rc = utils->read_u32_array(utils->data,
				"qcom,mdss-dsi-panel-hdr-color-primaries",
				hdr_prop->display_primaries,
				DISPLAY_PRIMARIES_MAX);
		if (rc) {
			pr_err("%s:%d, Unable to read color primaries,rc:%u",
					__func__, __LINE__, rc);
			hdr_prop->hdr_enabled = false;
			return rc;
		}

		rc = utils->read_u32(utils->data,
			"qcom,mdss-dsi-panel-peak-brightness",
			&(hdr_prop->peak_brightness));
		if (rc) {
			pr_err("%s:%d, Unable to read hdr brightness, rc:%u",
				__func__, __LINE__, rc);
			hdr_prop->hdr_enabled = false;
			return rc;
		}

		rc = utils->read_u32(utils->data,
			"qcom,mdss-dsi-panel-blackness-level",
			&(hdr_prop->blackness_level));
		if (rc) {
			pr_err("%s:%d, Unable to read hdr brightness, rc:%u",
				__func__, __LINE__, rc);
			hdr_prop->hdr_enabled = false;
			return rc;
		}
	}
	return 0;
}

static int dsi_panel_parse_topology(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils,
		int topology_override)
{
	struct msm_display_topology *topology;
	u32 top_count, top_sel, *array = NULL;
	int i, len = 0;
	int rc = -EINVAL;

	len = utils->count_u32_elems(utils->data, "qcom,display-topology");
	if (len <= 0 || len % TOPOLOGY_SET_LEN ||
			len > (TOPOLOGY_SET_LEN * MAX_TOPOLOGY)) {
		pr_err("invalid topology list for the panel, rc = %d\n", rc);
		return rc;
	}

	top_count = len / TOPOLOGY_SET_LEN;

	array = kcalloc(len, sizeof(u32), GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	rc = utils->read_u32_array(utils->data,
			"qcom,display-topology", array, len);
	if (rc) {
		pr_err("unable to read the display topologies, rc = %d\n", rc);
		goto read_fail;
	}

	topology = kcalloc(top_count, sizeof(*topology), GFP_KERNEL);
	if (!topology) {
		rc = -ENOMEM;
		goto read_fail;
	}

	for (i = 0; i < top_count; i++) {
		struct msm_display_topology *top = &topology[i];

		top->num_lm = array[i * TOPOLOGY_SET_LEN];
		top->num_enc = array[i * TOPOLOGY_SET_LEN + 1];
		top->num_intf = array[i * TOPOLOGY_SET_LEN + 2];
	};

	if (topology_override >= 0 && topology_override < top_count) {
		pr_info("override topology: cfg:%d lm:%d comp_enc:%d intf:%d\n",
			topology_override,
			topology[topology_override].num_lm,
			topology[topology_override].num_enc,
			topology[topology_override].num_intf);
		top_sel = topology_override;
		goto parse_done;
	}

	rc = utils->read_u32(utils->data,
			"qcom,default-topology-index", &top_sel);
	if (rc) {
		pr_err("no default topology selected, rc = %d\n", rc);
		goto parse_fail;
	}

	if (top_sel >= top_count) {
		rc = -EINVAL;
		pr_err("default topology is specified is not valid, rc = %d\n",
			rc);
		goto parse_fail;
	}

	pr_info("default topology: lm: %d comp_enc:%d intf: %d\n",
		topology[top_sel].num_lm,
		topology[top_sel].num_enc,
		topology[top_sel].num_intf);

parse_done:
	memcpy(&priv_info->topology, &topology[top_sel],
		sizeof(struct msm_display_topology));
parse_fail:
	kfree(topology);
read_fail:
	kfree(array);

	return rc;
}

static int dsi_panel_parse_roi_alignment(struct dsi_parser_utils *utils,
					 struct msm_roi_alignment *align)
{
	int len = 0, rc = 0;
	u32 value[6];
	struct property *data;

	if (!align)
		return -EINVAL;

	memset(align, 0, sizeof(*align));

	data = utils->find_property(utils->data,
			"qcom,panel-roi-alignment", &len);
	len /= sizeof(u32);
	if (!data) {
		pr_err("panel roi alignment not found\n");
		rc = -EINVAL;
	} else if (len != 6) {
		pr_err("incorrect roi alignment len %d\n", len);
		rc = -EINVAL;
	} else {
		rc = utils->read_u32_array(utils->data,
				"qcom,panel-roi-alignment", value, len);
		if (rc)
			pr_debug("error reading panel roi alignment values\n");
		else {
			align->xstart_pix_align = value[0];
			align->ystart_pix_align = value[1];
			align->width_pix_align = value[2];
			align->height_pix_align = value[3];
			align->min_width = value[4];
			align->min_height = value[5];
		}

		pr_info("roi alignment: [%d, %d, %d, %d, %d, %d]\n",
			align->xstart_pix_align,
			align->width_pix_align,
			align->ystart_pix_align,
			align->height_pix_align,
			align->min_width,
			align->min_height);
	}

	return rc;
}

static int dsi_panel_parse_partial_update_caps(struct dsi_display_mode *mode,
				struct dsi_parser_utils *utils)
{
	struct msm_roi_caps *roi_caps = NULL;
	const char *data;
	int rc = 0;

	if (!mode || !mode->priv_info) {
		pr_err("invalid arguments\n");
		return -EINVAL;
	}

	roi_caps = &mode->priv_info->roi_caps;

	memset(roi_caps, 0, sizeof(*roi_caps));

	data = utils->get_property(utils->data,
		"qcom,partial-update-enabled", NULL);
	if (data) {
		if (!strcmp(data, "dual_roi"))
			roi_caps->num_roi = 2;
		else if (!strcmp(data, "single_roi"))
			roi_caps->num_roi = 1;
		else {
			pr_info(
			"invalid value for qcom,partial-update-enabled: %s\n",
			data);
			return 0;
		}
	} else {
		pr_info("partial update disabled as the property is not set\n");
		return 0;
	}

	roi_caps->merge_rois = utils->read_bool(utils->data,
			"qcom,partial-update-roi-merge");

	roi_caps->enabled = roi_caps->num_roi > 0;

	pr_info("partial update num_rois=%d enabled=%d\n", roi_caps->num_roi,
			roi_caps->enabled);

	if (roi_caps->enabled)
		rc = dsi_panel_parse_roi_alignment(utils,
				&roi_caps->align);

	if (rc)
		memset(roi_caps, 0, sizeof(*roi_caps));

	return rc;
}

static int dsi_panel_parse_dms_info(struct dsi_panel *panel)
{
	int dms_enabled;
	const char *data;
	struct dsi_parser_utils *utils = &panel->utils;

	panel->dms_mode = DSI_DMS_MODE_DISABLED;
	dms_enabled = utils->read_bool(utils->data,
		"qcom,dynamic-mode-switch-enabled");
	if (!dms_enabled)
		return 0;

	data = utils->get_property(utils->data,
			"qcom,dynamic-mode-switch-type", NULL);
	if (data && !strcmp(data, "dynamic-resolution-switch-immediate")) {
		panel->dms_mode = DSI_DMS_MODE_RES_SWITCH_IMMEDIATE;
	} else {
		pr_err("[%s] unsupported dynamic switch mode: %s\n",
							panel->name, data);
		return -EINVAL;
	}

	return 0;
};

/*
 * The length of all the valid values to be checked should not be greater
 * than the length of returned data from read command.
 */
static bool
dsi_panel_parse_esd_check_valid_params(struct dsi_panel *panel, u32 count)
{
	int i;
	struct drm_panel_esd_config *config = &panel->esd_config;

	for (i = 0; i < count; ++i) {
		if (config->status_valid_params[i] >
				config->status_cmds_rlen[i]) {
			pr_debug("ignore valid params\n");
			return false;
		}
	}

	return true;
}

static bool dsi_panel_parse_esd_status_len(struct dsi_parser_utils *utils,
	char *prop_key, u32 **target, u32 cmd_cnt)
{
	int tmp;

	if (!utils->find_property(utils->data, prop_key, &tmp))
		return false;

	tmp /= sizeof(u32);
	if (tmp != cmd_cnt) {
		pr_err("request property(%d) do not match cmd count(%d)\n",
				tmp, cmd_cnt);
		return false;
	}

	*target = kcalloc(tmp, sizeof(u32), GFP_KERNEL);
	if (IS_ERR_OR_NULL(*target)) {
		pr_err("Error allocating memory for property\n");
		return false;
	}

	if (utils->read_u32_array(utils->data, prop_key, *target, tmp)) {
		pr_err("cannot get values from dts\n");
		kfree(*target);
		*target = NULL;
		return false;
	}

	return true;
}

static void dsi_panel_esd_config_deinit(struct drm_panel_esd_config *esd_config)
{
	kfree(esd_config->status_buf);
	kfree(esd_config->return_buf);
	kfree(esd_config->status_value);
	kfree(esd_config->status_valid_params);
	kfree(esd_config->status_cmds_rlen);
	kfree(esd_config->status_cmd.cmds);
}

int dsi_panel_parse_esd_reg_read_configs(struct dsi_panel *panel)
{
	struct drm_panel_esd_config *esd_config;
	int rc = 0;
	u32 tmp;
	u32 i, status_len, *lenp;
	struct property *data;
	struct dsi_parser_utils *utils = &panel->utils;

	if (!panel) {
		pr_err("Invalid Params\n");
		return -EINVAL;
	}

	esd_config = &panel->esd_config;
	if (!esd_config)
		return -EINVAL;

	dsi_panel_parse_cmd_sets_dt(&esd_config->status_cmd,
				DSI_CMD_SET_PANEL_STATUS, utils);
	if (!esd_config->status_cmd.count) {
		pr_err("panel status command parsing failed\n");
		rc = -EINVAL;
		goto error;
	}

	if (!dsi_panel_parse_esd_status_len(utils,
		"qcom,mdss-dsi-panel-status-read-length",
			&panel->esd_config.status_cmds_rlen,
				esd_config->status_cmd.count)) {
		pr_err("Invalid status read length\n");
		rc = -EINVAL;
		goto error1;
	}

	if (dsi_panel_parse_esd_status_len(utils,
		"qcom,mdss-dsi-panel-status-valid-params",
			&panel->esd_config.status_valid_params,
				esd_config->status_cmd.count)) {
		if (!dsi_panel_parse_esd_check_valid_params(panel,
					esd_config->status_cmd.count)) {
			rc = -EINVAL;
			goto error2;
		}
	}

	status_len = 0;
	lenp = esd_config->status_valid_params ?: esd_config->status_cmds_rlen;
	for (i = 0; i < esd_config->status_cmd.count; ++i)
		status_len += lenp[i];

	if (!status_len) {
		rc = -EINVAL;
		goto error2;
	}

	/*
	 * Some panel may need multiple read commands to properly
	 * check panel status. Do a sanity check for proper status
	 * value which will be compared with the value read by dsi
	 * controller during ESD check. Also check if multiple read
	 * commands are there then, there should be corresponding
	 * status check values for each read command.
	 */
	data = utils->find_property(utils->data,
			"qcom,mdss-dsi-panel-status-value", &tmp);
	tmp /= sizeof(u32);
	if (!IS_ERR_OR_NULL(data) && tmp != 0 && (tmp % status_len) == 0) {
		esd_config->groups = tmp / status_len;
	} else {
		pr_err("error parse panel-status-value\n");
		rc = -EINVAL;
		goto error2;
	}

	esd_config->status_value =
		kzalloc(sizeof(u32) * status_len * esd_config->groups,
			GFP_KERNEL);
	if (!esd_config->status_value) {
		rc = -ENOMEM;
		goto error2;
	}

	esd_config->return_buf = kcalloc(status_len * esd_config->groups,
			sizeof(unsigned char), GFP_KERNEL);
	if (!esd_config->return_buf) {
		rc = -ENOMEM;
		goto error3;
	}

	esd_config->status_buf = kzalloc(SZ_4K, GFP_KERNEL);
	if (!esd_config->status_buf) {
		rc = -ENOMEM;
		goto error4;
	}

	rc = utils->read_u32_array(utils->data,
		"qcom,mdss-dsi-panel-status-value",
		esd_config->status_value, esd_config->groups * status_len);
	if (rc) {
		pr_debug("error reading panel status values\n");
		memset(esd_config->status_value, 0,
				esd_config->groups * status_len);
	}

	return 0;

error4:
	kfree(esd_config->return_buf);
error3:
	kfree(esd_config->status_value);
error2:
	kfree(esd_config->status_valid_params);
	kfree(esd_config->status_cmds_rlen);
error1:
	kfree(esd_config->status_cmd.cmds);
error:
	return rc;
}

static int dsi_panel_parse_esd_config(struct dsi_panel *panel)
{
	int rc = 0;
	const char *string;
	struct drm_panel_esd_config *esd_config;
	struct dsi_parser_utils *utils = &panel->utils;
	u8 *esd_mode = NULL;

	esd_config = &panel->esd_config;
	esd_config->status_mode = ESD_MODE_MAX;
	esd_config->esd_enabled = utils->read_bool(utils->data,
		"qcom,esd-check-enabled");

	if (!esd_config->esd_enabled)
		return 0;

	rc = utils->read_string(utils->data,
			"qcom,mdss-dsi-panel-status-check-mode", &string);
	if (!rc) {
		if (!strcmp(string, "bta_check")) {
			esd_config->status_mode = ESD_MODE_SW_BTA;
		} else if (!strcmp(string, "reg_read")) {
			esd_config->status_mode = ESD_MODE_REG_READ;
		} else if (!strcmp(string, "te_signal_check")) {
			if (panel->panel_mode == DSI_OP_CMD_MODE) {
				esd_config->status_mode = ESD_MODE_PANEL_TE;
			} else {
				pr_err("TE-ESD not valid for video mode\n");
				rc = -EINVAL;
				goto error;
			}
		} else {
			pr_err("No valid panel-status-check-mode string\n");
			rc = -EINVAL;
			goto error;
		}
	} else {
		pr_debug("status check method not defined!\n");
		rc = -EINVAL;
		goto error;
	}

	if (panel->esd_config.status_mode == ESD_MODE_REG_READ) {
		rc = dsi_panel_parse_esd_reg_read_configs(panel);
		if (rc) {
			pr_err("failed to parse esd reg read mode params, rc=%d\n",
						rc);
			goto error;
		}
		esd_mode = "register_read";
	} else if (panel->esd_config.status_mode == ESD_MODE_SW_BTA) {
		esd_mode = "bta_trigger";
	} else if (panel->esd_config.status_mode ==  ESD_MODE_PANEL_TE) {
		esd_mode = "te_check";
	}

	pr_info("ESD enabled with mode: %s\n", esd_mode);

	return 0;

error:
	panel->esd_config.esd_enabled = false;
	return rc;
}

static void dsi_panel_update_util(struct dsi_panel *panel,
				  struct device_node *parser_node)
{
	struct dsi_parser_utils *utils = &panel->utils;

	if (parser_node) {
		*utils = *dsi_parser_get_parser_utils();
		utils->data = parser_node;

		pr_debug("switching to parser APIs\n");

		goto end;
	}

	*utils = *dsi_parser_get_of_utils();
	utils->data = panel->panel_of_node;
end:
	utils->node = panel->panel_of_node;
}

static int drm_panel_get_timings(struct drm_panel *panel,
				 unsigned int num_timings,
				 struct display_timing *timings)
{
	struct dsi_panel *p = container_of(panel, struct dsi_panel, drm_panel);
	unsigned int i;
	int rc = 0;

	if (p->num_timing_nodes < num_timings)
		num_timings = p->num_timing_nodes;

	if (timings)
		for (i = 0; i < num_timings; i++) {
			struct display_timing *t = &timings[i];
			struct dsi_display_mode m = {{0}};

			rc = dsi_panel_get_mode(p, i, &m, -1);
			if (rc)
				break;

			t->pixelclock.typ = m.pixel_clk_khz;
			t->hactive.typ = m.timing.h_active;
			t->hfront_porch.typ = m.timing.h_front_porch;
			t->hback_porch.typ = m.timing.h_back_porch;
			t->hsync_len.typ = m.timing.h_sync_width;
			t->vactive.typ = m.timing.v_active;
			t->vfront_porch.typ = m.timing.v_front_porch;
			t->vback_porch.typ = m.timing.v_back_porch;
			t->vsync_len.typ = m.timing.v_sync_width;
			t->flags = (m.timing.h_sync_polarity ?
				   DISPLAY_FLAGS_HSYNC_HIGH :
				   DISPLAY_FLAGS_HSYNC_LOW) |
				   (m.timing.v_sync_polarity ?
				   DISPLAY_FLAGS_VSYNC_HIGH :
				   DISPLAY_FLAGS_VSYNC_LOW);

			t->pixelclock.max = t->pixelclock.typ;
			t->hactive.max = t->hactive.typ;
			t->hfront_porch.max = t->hfront_porch.typ;
			t->hback_porch.max = t->hback_porch.typ;
			t->hsync_len.max = t->hsync_len.typ;
			t->vactive.max = t->vactive.typ;
			t->vfront_porch.max = t->vfront_porch.typ;
			t->vback_porch.max = t->vback_porch.typ;
			t->vsync_len.max = t->vsync_len.typ;

			t->pixelclock.min = t->pixelclock.typ;
			t->hactive.min = t->hactive.typ;
			t->hfront_porch.min = t->hfront_porch.typ;
			t->hback_porch.min = t->hback_porch.typ;
			t->hsync_len.min = t->hsync_len.typ;
			t->vactive.min = t->vactive.typ;
			t->vfront_porch.min = t->vfront_porch.typ;
			t->vback_porch.min = t->vback_porch.typ;
			t->vsync_len.min = t->vsync_len.typ;
		}

	return rc ?: p->num_timing_nodes;
}

static const struct drm_panel_funcs drm_panel_funcs = {
	.get_timings = drm_panel_get_timings,
};

static void dsi_panel_hbmsv_hanghandler_work(struct work_struct *work)
{
	struct dsi_panel *panel =
		    container_of(work, struct dsi_panel, hanghandler_work.work);

	pr_warn("hbmsv hang handler\n");
	panel->hbm_sv_enabled = false;
	dsi_panel_update_hbm(panel, HBM_MODE_OFF);
}

struct dsi_panel *dsi_panel_get(struct device *parent,
				struct device_node *of_node,
				struct device_node *parser_node,
				const char *type,
				int topology_override)
{
	struct dsi_panel *panel;
	struct dsi_parser_utils *utils;
	const char *panel_physical_type;
	int rc = 0;

	panel = kzalloc(sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return ERR_PTR(-ENOMEM);

	panel->panel_of_node = of_node;
	panel->parent = parent;
	panel->type = type;

	dsi_panel_update_util(panel, parser_node);
	utils = &panel->utils;

	panel->name = utils->get_property(utils->data,
				"qcom,mdss-dsi-panel-name", NULL);
	if (!panel->name)
		panel->name = DSI_PANEL_DEFAULT_LABEL;

	/*
	 * Set panel type to LCD as default.
	 */
	panel->panel_type = DSI_DISPLAY_PANEL_TYPE_LCD;
	panel_physical_type  = utils->get_property(utils->data,
				"qcom,mdss-dsi-panel-physical-type", NULL);
	if (panel_physical_type && !strcmp(panel_physical_type, "oled"))
		panel->panel_type = DSI_DISPLAY_PANEL_TYPE_OLED;

	rc = dsi_panel_parse_host_config(panel);
	if (rc) {
		pr_err("failed to parse host configuration, rc=%d\n", rc);
		goto error;
	}
	rc = dsi_panel_parse_vendor_info(panel, of_node);
	if (rc)
		pr_err("failed to parse vendor information, rc=%d\n", rc);

	rc = dsi_panel_parse_panel_mode(panel);
	if (rc) {
		pr_err("failed to parse panel mode configuration, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_parse_dfps_caps(panel);
	if (rc)
		pr_err("failed to parse dfps configuration, rc=%d\n", rc);

	rc = dsi_panel_parse_qsync_caps(panel, of_node);
	if (rc)
		pr_err("failed to parse qsync features, rc=%d\n", rc);

	/* allow qsync support only if DFPS is with VFP approach */
	if ((panel->dfps_caps.dfps_support) &&
	    !(panel->dfps_caps.type == DSI_DFPS_IMMEDIATE_VFP))
		panel->qsync_min_fps = 0;

	rc = dsi_panel_parse_dyn_clk_caps(panel);
	if (rc)
		pr_err("failed to parse dynamic clk config, rc=%d\n", rc);

	rc = dsi_panel_parse_phy_props(panel);
	if (rc) {
		pr_err("failed to parse panel physical dimension, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_parse_gpios(panel);
	if (rc) {
		pr_err("failed to parse panel gpios, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_parse_power_cfg(panel);
	if (rc)
		pr_err("failed to parse power config, rc=%d\n", rc);

	rc = dsi_panel_bl_parse_config(parent, &panel->bl_config);
	if (rc)
		pr_err("failed to parse backlight config, rc=%d\n", rc);

	rc = dsi_panel_parse_misc_features(panel);
	if (rc)
		pr_err("failed to parse misc features, rc=%d\n", rc);

	rc = dsi_panel_parse_hdr_config(panel);
	if (rc)
		pr_err("failed to parse hdr config, rc=%d\n", rc);

	rc = dsi_panel_get_mode_count(panel);
	if (rc) {
		pr_err("failed to get mode count, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_parse_dms_info(panel);
	if (rc)
		pr_debug("failed to get dms info, rc=%d\n", rc);

	rc = dsi_panel_parse_esd_config(panel);
	if (rc)
		pr_debug("failed to parse esd config, rc=%d\n", rc);

	panel->power_mode = SDE_MODE_DPMS_OFF;
	drm_panel_init(&panel->drm_panel);
	panel->drm_panel.dev = &panel->mipi_device.dev;
	panel->mipi_device.dev.of_node = of_node;
	panel->drm_panel.funcs = &drm_panel_funcs;

	rc = drm_panel_add(&panel->drm_panel);
	if (rc)
		goto error;

	mutex_init(&panel->panel_lock);

	rc = dsi_panel_create_sn_buf(panel);
	if (rc)
		pr_err("failed to create buffer for SN, rc=%d\n", rc);

	panel->hbm_sv_enabled = true;
	INIT_DELAYED_WORK(&panel->hanghandler_work,
			dsi_panel_hbmsv_hanghandler_work);

	return panel;
error:
	kfree(panel);
	return ERR_PTR(rc);
}

void dsi_panel_put(struct dsi_panel *panel)
{
	cancel_delayed_work_sync(&panel->hanghandler_work);

	dsi_panel_release_sn_buf(panel);

	dsi_panel_release_vendor_extinfo(panel);

	drm_panel_remove(&panel->drm_panel);

	/* free resources allocated for ESD check */
	dsi_panel_esd_config_deinit(&panel->esd_config);

	kfree(panel);
}

static ssize_t parse_byte_buf(u8 *out, size_t len, char *src)
{
	const char *skip = "\n ";
	size_t i = 0;
	int rc = 0;
	char *s;

	while (src && !rc && i < len) {
		s = strsep(&src, skip);
		if (*s != '\0') {
			rc = kstrtou8(s, 16, out + i);
			i++;
		}
	}

	return rc ? : i;
}

ssize_t dsi_panel_debugfs_write_reg(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct dsi_panel *panel = seq->private;
	char *buf;
	char *payload;
	size_t len;
	int rc = 0;

	if (!panel || !panel->panel_initialized)
		return -EPERM;

	/* calculate length for worst case (1 digit per byte + whitespace) */
	len = (count + 1) / 2;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	payload = kmalloc(len, GFP_KERNEL);
	if (!payload) {
		kfree(buf);
		return -ENOMEM;
	}

	if (copy_from_user(buf, user_buf, count)) {
		rc = -EFAULT;
		goto done;
	}

	buf[count] = 0; /* terminate end of string */

	rc = parse_byte_buf(payload, len, buf);
	if (rc <= 0) {
		rc = -EINVAL;
		goto done;
	}
	len = rc;

	pr_debug("writing cmd=%x len=%zu\n", payload[0], len);

	mutex_lock(&panel->panel_lock);
	rc = mipi_dsi_dcs_write_buffer(&panel->mipi_device, payload, len);
	mutex_unlock(&panel->panel_lock);

done:
	kfree(buf);
	kfree(payload);

	return rc ? : count;
}

int dsi_panel_debugfs_read_reg(struct seq_file *seq, void *data)
{
	struct dsi_panel *panel = seq->private;
	char *buf;
	ssize_t rc;
	size_t len;
	u8 cmd;

	if (!panel || !panel->panel_initialized)
		return -EPERM;

	len = panel->debug.reg_read_len;
	cmd = panel->debug.reg_read_cmd;

	if (len == 0)
		return -EINVAL;

	buf = kmalloc(max(PAGE_SIZE, len), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&panel->panel_lock);
	rc = mipi_dsi_dcs_read(&panel->mipi_device, cmd, buf, len);
	mutex_unlock(&panel->panel_lock);

	if (rc > 0) {
		int i;

		pr_debug("got %zd bytes back, first: 0x%x\n", rc, buf[0]);
		for (i = 0; i < rc; i++) {
			if ((i % 8) > 0)
				seq_puts(seq, " ");
			else if (i)
				seq_puts(seq, "\n");
			seq_printf(seq, "%02X", buf[i]);
		}
		seq_puts(seq, "\n");
		rc = 0;
	} else if (rc == 0) {
		pr_debug("no response back\n");
	}
	kfree(buf);

	return rc;
}

static int dsi_panel_debugfs_open_reg(struct inode *inode, struct file *f)
{
	return single_open(f, dsi_panel_debugfs_read_reg, inode->i_private);
}

static const struct file_operations panel_reg_fops = {
	.owner =	THIS_MODULE,
	.open =		dsi_panel_debugfs_open_reg,
	.write =	dsi_panel_debugfs_write_reg,
	.read =		seq_read,
	.llseek =	seq_lseek,
	.release =	single_release,
};

struct debugfs_cmdset_entry {
	struct dsi_panel *panel;
	enum dsi_cmd_set_type type;
};

struct {
	const char *label;
	enum dsi_cmd_set_type type;
} cmdset_list[] = {
	{ "pre_on",		DSI_CMD_SET_PRE_ON },
	{ "on",			DSI_CMD_SET_ON },
	{ "post_on",		DSI_CMD_SET_POST_ON },
	{ "pre_off",		DSI_CMD_SET_PRE_OFF },
	{ "off",		DSI_CMD_SET_OFF },
	{ "post_off",		DSI_CMD_SET_POST_OFF },
	{ "panel_status",	DSI_CMD_SET_PANEL_STATUS },
	{ "pps",		DSI_CMD_SET_PPS },
	{ "lp1",		DSI_CMD_SET_LP1 },
	{ "lp2",		DSI_CMD_SET_LP2 },
	{ "no_lp",		DSI_CMD_SET_NOLP },
	{ "post_nolp",		DSI_CMD_SET_POST_NOLP },
	{ "vr",			DSI_CMD_SET_VR },
	{ "novr",		DSI_CMD_SET_NOVR },
	{ "switch",		DSI_CMD_SET_TIMING_SWITCH },
	{ "post_switch",	DSI_CMD_SET_POST_TIMING_SWITCH },
};

static inline ssize_t parse_cmdset(struct dsi_panel_cmd_set *set, char *buf,
				   size_t count)
{
	char *tmp;
	size_t len;
	ssize_t rc;

	/* calculate length for worst case (1 digit per byte + whitespace) */
	len = (count + 1) / 2;
	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	rc = parse_byte_buf(tmp, len, buf);
	if (rc <= 0) {
		rc = -EINVAL;
		goto done;
	}

	rc = dsi_panel_parse_cmd_sets_sub(set, tmp, rc);
done:
	kfree(tmp);

	return rc;
}

ssize_t dsi_panel_debugfs_write_cmdset(struct file *file,
				       const char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct debugfs_cmdset_entry *entry = seq->private;
	struct dsi_panel *panel = entry->panel;
	struct dsi_panel_cmd_set tmp_set;
	struct dsi_panel_cmd_set *set;
	char *buf;
	int rc = 0;

	mutex_lock(&panel->panel_lock);
	if (!panel->cur_mode || !panel->cur_mode->priv_info) {
		pr_err("Invalid mode for panel [%s]\n", panel->name);
		mutex_unlock(&panel->panel_lock);
		return -EINVAL;
	}

	set = &panel->cur_mode->priv_info->cmd_sets[entry->type];
	tmp_set = *set;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf) {
		rc = -ENOMEM;
		goto done;
	}

	if (copy_from_user(buf, user_buf, count)) {
		rc = -EFAULT;
		goto done;
	}
	buf[count] = '\0';

	rc = parse_cmdset(&tmp_set, buf, count);
	if (rc)
		goto done;

	dsi_panel_destroy_cmd_packets(set);
	*set = tmp_set;

done:
	kfree(buf);
	mutex_unlock(&panel->panel_lock);

	return rc ? : count;
}

int dsi_panel_debugfs_read_cmdset(struct seq_file *seq, void *data)
{
	struct debugfs_cmdset_entry *entry = seq->private;
	struct dsi_panel *panel = entry->panel;
	struct dsi_panel_cmd_set *set;
	int i, j;

	mutex_lock(&panel->panel_lock);
	if (!panel->cur_mode || !panel->cur_mode->priv_info) {
		pr_err("Invalid mode for panel [%s]\n", panel->name);
		mutex_unlock(&panel->panel_lock);
		return -EINVAL;
	}
	set = &panel->cur_mode->priv_info->cmd_sets[entry->type];

	for (i = 0; i < set->count; i++) {
		struct dsi_cmd_desc *cmd = set->cmds + i;
		const char *txbuf = cmd->msg.tx_buf;

		seq_printf(seq, "%02X %02X %02X %02X %02X %02zX %02zX",
			   cmd->msg.type, cmd->last_command, cmd->msg.channel,
			   cmd->msg.flags & MIPI_DSI_MSG_REQ_ACK ? 1 : 0,
			   cmd->post_wait_ms,
			   cmd->msg.tx_len >> 8, cmd->msg.tx_len & 0xff);

		for (j = 0; j < cmd->msg.tx_len; j++)
			seq_printf(seq, " %02X", txbuf[j]);
		seq_puts(seq, "\n");
	}
	mutex_unlock(&panel->panel_lock);

	return 0;
}

static int dsi_panel_debugfs_open_cmdset(struct inode *inode, struct file *f)
{
	return single_open(f, dsi_panel_debugfs_read_cmdset, inode->i_private);
}

static const struct file_operations panel_cmdset_fops = {
	.owner =	THIS_MODULE,
	.open =		dsi_panel_debugfs_open_cmdset,
	.write =	dsi_panel_debugfs_write_cmdset,
	.read =		seq_read,
	.llseek =	seq_lseek,
	.release =	single_release,
};

static void dsi_panel_debugfs_create_cmdsets(struct dentry *parent,
					     struct dsi_panel *panel)
{
	struct device *dev = panel->parent;
	struct debugfs_cmdset_entry *entry;
	const size_t cmds_size = ARRAY_SIZE(cmdset_list);
	struct dentry *r;
	int i;

	r = debugfs_create_dir("cmd_sets", parent);
	if (IS_ERR(r))
		return;

	entry = devm_kzalloc(dev, cmds_size * sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return;

	for (i = 0; i < cmds_size; i++, entry++) {
		const char *name = cmdset_list[i].label;

		entry->type = cmdset_list[i].type;
		entry->panel = panel;

		debugfs_create_file(name, 0600, r, entry, &panel_cmdset_fops);
	}
}

void dsi_panel_debugfs_init(struct dsi_panel *panel, struct dentry *dir)
{
	struct dentry *r;
	struct dsi_panel_debug *pdbg = &panel->debug;
	r = debugfs_create_dir("panel_reg", dir);
	if (IS_ERR(r))
		return;

	/* default read of 2 bytes */
	pdbg->reg_read_len = 2;
	debugfs_create_u8("addr", 0600, r, &pdbg->reg_read_cmd);
	debugfs_create_size_t("len", 0600, r, &pdbg->reg_read_len);
	debugfs_create_file("payload", 0600, r, panel, &panel_reg_fops);

	dsi_panel_debugfs_create_cmdsets(dir, panel);
}

int dsi_panel_drv_init(struct dsi_panel *panel,
		       struct mipi_dsi_host *host)
{
	int rc = 0;
	struct mipi_dsi_device *dev;

	if (!panel || !host) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	dev = &panel->mipi_device;

	dev->host = host;
	/*
	 * We dont have device structure since panel is not a device node.
	 * When using drm panel framework, the device is probed when the host is
	 * create.
	 */
	dev->channel = 0;
	dev->lanes = 4;

	panel->host = host;
	rc = dsi_panel_vreg_get(panel);
	if (rc) {
		pr_err("[%s] failed to get panel regulators, rc=%d\n",
		       panel->name, rc);
		goto exit;
	}

	rc = dsi_panel_pinctrl_init(panel);
	if (rc) {
		pr_err("[%s] failed to init pinctrl, rc=%d\n", panel->name, rc);
		goto error_vreg_put;
	}

	rc = dsi_panel_gpio_request(panel);
	if (rc) {
		pr_err("[%s] failed to request gpios, rc=%d\n", panel->name,
		       rc);
		goto error_pinctrl_deinit;
	}

	rc = dsi_panel_bl_register(panel);
	if (rc) {
		if (rc != -EPROBE_DEFER)
			pr_err("[%s] failed to register backlight, rc=%d\n",
			       panel->name, rc);
		goto error_gpio_release;
	}

	rc = dsi_panel_switch_init(panel);

	goto exit;

error_gpio_release:
	(void)dsi_panel_gpio_release(panel);
error_pinctrl_deinit:
	(void)dsi_panel_pinctrl_deinit(panel);
error_vreg_put:
	(void)dsi_panel_vreg_put(panel);
exit:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_drv_deinit(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	dsi_panel_switch_destroy(panel);

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_bl_unregister(panel);
	if (rc)
		pr_err("[%s] failed to unregister backlight, rc=%d\n",
		       panel->name, rc);

	rc = dsi_panel_gpio_release(panel);
	if (rc)
		pr_err("[%s] failed to release gpios, rc=%d\n", panel->name,
		       rc);

	rc = dsi_panel_pinctrl_deinit(panel);
	if (rc)
		pr_err("[%s] failed to deinit gpios, rc=%d\n", panel->name,
		       rc);

	rc = dsi_panel_vreg_put(panel);
	if (rc)
		pr_err("[%s] failed to put regs, rc=%d\n", panel->name, rc);

	panel->host = NULL;
	memset(&panel->mipi_device, 0x0, sizeof(panel->mipi_device));

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_validate_mode(struct dsi_panel *panel,
			    struct dsi_display_mode *mode)
{
	return 0;
}

int dsi_panel_get_mode_count(struct dsi_panel *panel)
{
	const u32 SINGLE_MODE_SUPPORT = 1;
	struct dsi_parser_utils *utils;
	struct device_node *timings_np;
	int count, rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	utils = &panel->utils;

	panel->num_timing_nodes = 0;

	timings_np = utils->get_child_by_name(utils->data,
			"qcom,mdss-dsi-display-timings");
	if (!timings_np && !panel->host_config.ext_bridge_num) {
		pr_err("no display timing nodes defined\n");
		rc = -EINVAL;
		goto error;
	}

	count = utils->get_child_count(timings_np);
	if ((!count && !panel->host_config.ext_bridge_num) ||
		count > DSI_MODE_MAX) {
		pr_err("invalid count of timing nodes: %d\n", count);
		rc = -EINVAL;
		goto error;
	}

	/* No multiresolution support is available for video mode panels */
	if (panel->panel_mode != DSI_OP_CMD_MODE &&
		!panel->host_config.ext_bridge_num)
		count = SINGLE_MODE_SUPPORT;

	panel->num_timing_nodes = count;

error:
	return rc;
}

int dsi_panel_get_phy_props(struct dsi_panel *panel,
			    struct dsi_panel_phy_props *phy_props)
{
	int rc = 0;

	if (!panel || !phy_props) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	memcpy(phy_props, &panel->phy_props, sizeof(*phy_props));
	return rc;
}

int dsi_panel_get_dfps_caps(struct dsi_panel *panel,
			    struct dsi_dfps_capabilities *dfps_caps)
{
	int rc = 0;

	if (!panel || !dfps_caps) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	memcpy(dfps_caps, &panel->dfps_caps, sizeof(*dfps_caps));
	return rc;
}

void dsi_panel_put_mode(struct dsi_display_mode *mode)
{
	int i;

	if (!mode->priv_info)
		return;

	dsi_panel_switch_put_mode(mode);

	for (i = 0; i < DSI_CMD_SET_MAX; i++)
		dsi_panel_destroy_cmd_packets(&mode->priv_info->cmd_sets[i]);

	kfree(mode->priv_info->phy_timing_val);
	kfree(mode->priv_info);
	mode->priv_info = NULL;
}

int dsi_panel_get_mode(struct dsi_panel *panel,
			u32 index, struct dsi_display_mode *mode,
			int topology_override)
{
	struct device_node *timings_np, *child_np;
	struct dsi_parser_utils *utils;
	struct dsi_display_mode_priv_info *prv_info;
	u32 child_idx = 0;
	int rc = 0, num_timings;
	void *utils_data = NULL;

	if (!panel || !mode) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	utils = &panel->utils;

	mode->priv_info = kzalloc(sizeof(*mode->priv_info), GFP_KERNEL);
	if (!mode->priv_info) {
		rc = -ENOMEM;
		goto done;
	}

	prv_info = mode->priv_info;

	timings_np = utils->get_child_by_name(utils->data,
		"qcom,mdss-dsi-display-timings");
	if (!timings_np) {
		pr_err("no display timing nodes defined\n");
		rc = -EINVAL;
		goto parse_fail;
	}

	num_timings = utils->get_child_count(timings_np);
	if (!num_timings || num_timings > DSI_MODE_MAX) {
		pr_err("invalid count of timing nodes: %d\n", num_timings);
		rc = -EINVAL;
		goto parse_fail;
	}

	utils_data = utils->data;

	dsi_for_each_child_node(timings_np, child_np) {
		if (index != child_idx++)
			continue;

		utils->data = child_np;

		rc = dsi_panel_parse_timing(&mode->timing, utils);
		if (rc) {
			pr_err("failed to parse panel timing, rc=%d\n", rc);
			goto parse_fail;
		}

		if (panel->panel_mode == DSI_OP_VIDEO_MODE)
			mode->priv_info->mdp_transfer_time_us = 0;

		rc = dsi_panel_parse_dsc_params(mode, utils);
		if (rc) {
			pr_err("failed to parse dsc params, rc=%d\n", rc);
			goto parse_fail;
		}

		rc = dsi_panel_parse_topology(prv_info, utils,
				topology_override);
		if (rc) {
			pr_err("failed to parse panel topology, rc=%d\n", rc);
			goto parse_fail;
		}

		rc = dsi_panel_parse_cmd_sets(prv_info, utils);
		if (rc) {
			pr_err("failed to parse command sets, rc=%d\n", rc);
			goto parse_fail;
		}

		rc = dsi_panel_parse_jitter_config(mode, utils);
		if (rc)
			pr_err(
			"failed to parse panel jitter config, rc=%d\n", rc);

		rc = dsi_panel_parse_phy_timing(mode, utils, panel->panel_mode);
		if (rc) {
			pr_err(
			"failed to parse panel phy timings, rc=%d\n", rc);
			goto parse_fail;
		}

		rc = dsi_panel_parse_partial_update_caps(mode, utils);
		if (rc)
			pr_err("failed to partial update caps, rc=%d\n", rc);

		/*
		 * No support for pixel overlap in DSC enabled or Partial
		 * update enabled cases.
		 */
		if (prv_info->dsc_enabled || prv_info->roi_caps.enabled)
			prv_info->overlap_pixels = 0;
	}
	goto done;

parse_fail:
	kfree(mode->priv_info);
	mode->priv_info = NULL;
done:
	utils->data = utils_data;
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_get_host_cfg_for_mode(struct dsi_panel *panel,
				    struct dsi_display_mode *mode,
				    struct dsi_host_config *config)
{
	int rc = 0;
	struct dsi_dyn_clk_caps *dyn_clk_caps = &panel->dyn_clk_caps;

	if (!panel || !mode || !config) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	config->panel_mode = panel->panel_mode;
	memcpy(&config->common_config, &panel->host_config,
	       sizeof(config->common_config));

	if (panel->panel_mode == DSI_OP_VIDEO_MODE) {
		memcpy(&config->u.video_engine, &panel->video_config,
		       sizeof(config->u.video_engine));
	} else {
		memcpy(&config->u.cmd_engine, &panel->cmd_config,
		       sizeof(config->u.cmd_engine));
	}

	memcpy(&config->video_timing, &mode->timing,
	       sizeof(config->video_timing));
	config->video_timing.mdp_transfer_time_us =
			mode->priv_info->mdp_transfer_time_us;
	config->video_timing.dsc_enabled = mode->priv_info->dsc_enabled;
	config->video_timing.dsc = &mode->priv_info->dsc;
	config->video_timing.overlap_pixels = mode->priv_info->overlap_pixels;

	if (dyn_clk_caps->dyn_clk_support)
		config->bit_clk_rate_hz_override = mode->timing.clk_rate_hz;
	else
		config->bit_clk_rate_hz_override = mode->priv_info->clk_rate_hz;

	config->esc_clk_rate_hz = 19200000;
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_pre_prepare(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_pwr_enable_regulator(&panel->power_info, true);
	if (rc) {
		pr_err("[%s] failed to enable vregs, rc=%d\n", panel->name, rc);
		goto error;
	}

	/* If LP11_INIT is set, panel will be powered up during prepare() */
	if (panel->lp11_init)
		goto error;

	rc = dsi_panel_power_on(panel);
	if (rc) {
		pr_err("[%s] panel power on failed, rc=%d\n", panel->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_update_pps(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_cmd_set *set = NULL;
	struct dsi_display_mode_priv_info *priv_info = NULL;

	if (!panel || !panel->cur_mode) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	priv_info = panel->cur_mode->priv_info;

	set = &priv_info->cmd_sets[DSI_CMD_SET_PPS];

	dsi_dsc_create_pps_buf_cmd(&priv_info->dsc, panel->dsc_pps_cmd, 0);
	rc = dsi_panel_create_cmd_packets(panel->dsc_pps_cmd,
					  DSI_CMD_PPS_SIZE, 1, set->cmds);
	if (rc) {
		pr_err("failed to create cmd packets, rc=%d\n", rc);
		goto error;
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_PPS);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_PPS cmds, rc=%d\n",
			panel->name, rc);

	dsi_panel_destroy_cmds_packets_buf(set);
error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_set_lp1(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	if (panel->funcs && panel->funcs->pre_lp1)
		panel->funcs->pre_lp1(panel);

	dsi_backlight_early_dpms(&panel->bl_config, SDE_MODE_DPMS_LP1);

	mutex_lock(&panel->panel_lock);

	if (!panel->panel_initialized)
		goto exit;

	if (panel->vr_mode) {
		rc = dsi_panel_clear_vr_locked(panel);
		if (rc) {
			pr_err("[%s] couldn't disable VR mode for LP1 transition\n",
				panel->name);
			mutex_unlock(&panel->panel_lock);
			return rc;
		}
	}

	rc = dsi_panel_update_hbm_locked(panel, HBM_MODE_OFF);
	if (rc) {
		pr_err("[%s] couldn't disable HBM mode for LP1 transition\n",
			panel->name);
		mutex_unlock(&panel->panel_lock);
		return rc;
	}
	dsi_backlight_hbm_dimming_stop(&panel->bl_config);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP1);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_LP1 cmd, rc=%d\n",
		       panel->name, rc);
exit:
	mutex_unlock(&panel->panel_lock);

	if (!rc)
		rc = dsi_backlight_late_dpms(&panel->bl_config,
					       SDE_MODE_DPMS_LP1);

	return rc;
}

int dsi_panel_set_lp2(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	dsi_backlight_early_dpms(&panel->bl_config, SDE_MODE_DPMS_LP2);

	mutex_lock(&panel->panel_lock);

	if (!panel->panel_initialized)
		goto exit;

	if (panel->vr_mode) {
		rc = dsi_panel_clear_vr_locked(panel);
		if (rc) {
			pr_err("[%s] couldn't disable VR mode for LP2 transition\n",
				panel->name);
			mutex_unlock(&panel->panel_lock);
			return rc;
		}
	}

	rc = dsi_panel_update_hbm_locked(panel, HBM_MODE_OFF);
	if (rc) {
		pr_err("[%s] couldn't disable HBM mode for LP2 transition\n",
			panel->name);
		mutex_unlock(&panel->panel_lock);
		return rc;
	}
	dsi_backlight_hbm_dimming_stop(&panel->bl_config);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP2);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_LP2 cmd, rc=%d\n",
		       panel->name, rc);
exit:
	mutex_unlock(&panel->panel_lock);

	if (!rc)
		rc = dsi_backlight_late_dpms(&panel->bl_config,
					       SDE_MODE_DPMS_LP2);

	return rc;
}

int dsi_panel_set_nolp(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	dsi_backlight_early_dpms(&panel->bl_config, SDE_MODE_DPMS_ON);

	mutex_lock(&panel->panel_lock);
	if (!panel->panel_initialized)
		goto exit;

	if (!panel->funcs || !panel->funcs->send_nolp)
		goto exit;

	rc = panel->funcs->send_nolp(panel);
	if (rc == -EOPNOTSUPP)
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);

	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\n",
		       panel->name, rc);
exit:
	mutex_unlock(&panel->panel_lock);

	if (!rc)
		rc = dsi_backlight_late_dpms(&panel->bl_config,
					       SDE_MODE_DPMS_ON);
	return rc;
}

static int dsi_panel_set_vr_locked(struct dsi_panel *panel)
{
	int rc = 0;

	if (dsi_backlight_get_dpms(&panel->bl_config) != SDE_MODE_DPMS_ON) {
		pr_err("[%s] DSI_CMD_SET_VR wasn't allowed\n", panel->name);
		return -EINVAL;
	}

	rc = dsi_panel_update_hbm_locked(panel, HBM_MODE_OFF);
	if (rc) {
		pr_err("[%s] couldn't disable HBM mode before VR entry, aborting VR entry\n",
			panel->name);
		return -EINVAL;
	}
	dsi_backlight_hbm_dimming_stop(&panel->bl_config);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_VR);

	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_VR cmd, rc=%d\n",
			panel->name, rc);
	else
		panel->vr_mode = true;

	return rc;
}

static int dsi_panel_clear_vr_locked(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel->vr_mode) {
		pr_err("[%s] tried to exit, but not in VR mode\n", panel->name);
		return -EINVAL;
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOVR);

	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_NOVR cmd, rc=%d\n",
		       panel->name, rc);
	else
		panel->vr_mode = false;

	return rc;
}

int dsi_panel_set_vr(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_set_vr_locked(panel);
	mutex_unlock(&panel->panel_lock);

	return rc;
}

int dsi_panel_clear_vr(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_clear_vr_locked(panel);
	mutex_unlock(&panel->panel_lock);

	return rc;
}

int dsi_panel_update_vr_mode(struct dsi_panel *panel, bool enable)
{
	if (enable)
		return dsi_panel_set_vr(panel);
	else
		return dsi_panel_clear_vr(panel);
}

bool dsi_panel_get_vr_mode(struct dsi_panel *panel)
{
	bool vr_mode;

	if (!panel) {
		pr_err("invalid params\n");
		return false;
	}

	return vr_mode = panel->vr_mode;
}

static int dsi_panel_update_hbm_locked(struct dsi_panel *panel,
					enum hbm_mode_type hbm_mode)
{
	struct dsi_backlight_config *bl = &panel->bl_config;
	struct hbm_data *hbm = bl->hbm;

	if (!hbm)
		return 0;

	if (hbm_mode < 0 || hbm_mode >= HBM_MODE_MAX)
		return -EINVAL;
	if (hbm_mode == panel->hbm_mode)
		return 0;

	if (hbm_mode == HBM_MODE_SV && !panel->hbm_sv_enabled) {
		pr_warn("hbmsv is disabled\n");
		return -EINVAL;
	}

	if ((dsi_backlight_get_dpms(bl) != SDE_MODE_DPMS_ON) ||
		panel->vr_mode) {
		pr_err("[%s] Backlight in incompatible state, HBM changes not allowed\n",
			panel->name);
		return -EINVAL;
	}

	if (hbm_mode == HBM_MODE_SV)
		mod_delayed_work(system_wq, &panel->hanghandler_work,
				      msecs_to_jiffies(HBM_SV_MAX_MS));
	else
		cancel_delayed_work(&panel->hanghandler_work);

	panel->hbm_pending_irc_on =
		(panel->hbm_mode == HBM_MODE_SV && hbm_mode == HBM_MODE_OFF);

	hbm->cur_range = HBM_RANGE_MAX;

	if (hbm_mode == HBM_MODE_SV) {
		int rc = dsi_panel_bl_update_irc(bl, false);

		if (rc != 0 && rc != -EOPNOTSUPP)
			pr_err("[%s] failed to disable IRC, rc=%d\n",
				panel->name, rc);
	} else if (hbm_mode == HBM_MODE_ON && panel->hbm_mode == HBM_MODE_SV) {
		int rc = dsi_panel_bl_update_irc(bl, true);

		if (rc != 0 && rc != -EOPNOTSUPP)
			pr_err("[%s] failed to enable IRC, rc=%d\n",
				panel->name, rc);
	}

	panel->hbm_mode = hbm_mode;

	/* When HBM exit is requested, send HBM exit commands
	 * immediately to avoid conflict with subsequent backlight ops.
	 */
	if (hbm_mode == HBM_MODE_OFF) {
		int rc;

		dsi_backlight_hbm_dimming_start(bl,
			hbm->exit_num_dimming_frames,
			&hbm->exit_dimming_stop_cmd);

		rc = panel->funcs->update_hbm(panel);
		if (rc == -EOPNOTSUPP)
			rc = dsi_panel_cmd_set_transfer(panel, &hbm->exit_cmd);
		if (rc)
			pr_err("[%s] failed to send HBM exit cmd, rc=%d\n",
				panel->name, rc);
	}

	if (bl->bl_device)
		sysfs_notify(&bl->bl_device->dev.kobj, NULL,
					"state");

	return 0;
}

int dsi_panel_update_hbm(struct dsi_panel *panel, enum hbm_mode_type hbm_mode)
{
	int rc = 0;

	if (!panel)
		return -EINVAL;

	if (!panel->bl_config.hbm)
		return 0;

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_update_hbm_locked(panel, hbm_mode);
	mutex_unlock(&panel->panel_lock);
	if (rc)
		return rc;

	return backlight_update_status(panel->bl_config.bl_device);
}

enum hbm_mode_type dsi_panel_get_hbm(struct dsi_panel *panel)
{
	if (!panel) {
		pr_err("invalid params\n");
		return false;
	}

	return panel->hbm_mode;
}

int dsi_panel_prepare(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	dsi_backlight_early_dpms(&panel->bl_config, SDE_MODE_DPMS_ON);

	mutex_lock(&panel->panel_lock);

	if (panel->init_delay_us)
		usleep_range(panel->init_delay_us, panel->init_delay_us);

	if (panel->lp11_init) {
		rc = dsi_panel_power_on(panel);
		if (rc) {
			pr_err("[%s] panel power on failed, rc=%d\n",
			       panel->name, rc);
			goto error;
		}
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_PRE_ON);
	if (rc) {
		pr_err("[%s] failed to send DSI_CMD_SET_PRE_ON cmds, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_panel_roi_prepare_dcs_cmds(struct dsi_panel_cmd_set *set,
		struct dsi_rect *roi, int ctrl_idx, int unicast)
{
	static const int ROI_CMD_LEN = 5;

	int rc = 0;

	/* DTYPE_DCS_LWRITE */
	static char *caset, *paset;

	set->cmds = NULL;

	caset = kzalloc(ROI_CMD_LEN, GFP_KERNEL);
	if (!caset) {
		rc = -ENOMEM;
		goto exit;
	}
	caset[0] = 0x2a;
	caset[1] = (roi->x & 0xFF00) >> 8;
	caset[2] = roi->x & 0xFF;
	caset[3] = ((roi->x - 1 + roi->w) & 0xFF00) >> 8;
	caset[4] = (roi->x - 1 + roi->w) & 0xFF;

	paset = kzalloc(ROI_CMD_LEN, GFP_KERNEL);
	if (!paset) {
		rc = -ENOMEM;
		goto error_free_mem;
	}
	paset[0] = 0x2b;
	paset[1] = (roi->y & 0xFF00) >> 8;
	paset[2] = roi->y & 0xFF;
	paset[3] = ((roi->y - 1 + roi->h) & 0xFF00) >> 8;
	paset[4] = (roi->y - 1 + roi->h) & 0xFF;

	set->type = DSI_CMD_SET_ROI;
	set->state = DSI_CMD_SET_STATE_LP;
	set->count = 2; /* send caset + paset together */
	set->cmds = kcalloc(set->count, sizeof(*set->cmds), GFP_KERNEL);
	if (!set->cmds) {
		rc = -ENOMEM;
		goto error_free_mem;
	}
	set->cmds[0].msg.channel = 0;
	set->cmds[0].msg.type = MIPI_DSI_DCS_LONG_WRITE;
	set->cmds[0].msg.flags = unicast ? MIPI_DSI_MSG_UNICAST : 0;
	set->cmds[0].msg.ctrl = unicast ? ctrl_idx : 0;
	set->cmds[0].msg.tx_len = ROI_CMD_LEN;
	set->cmds[0].msg.tx_buf = caset;
	set->cmds[0].msg.rx_len = 0;
	set->cmds[0].msg.rx_buf = 0;
	set->cmds[0].msg.wait_ms = 0;
	set->cmds[0].last_command = 0;
	set->cmds[0].post_wait_ms = 0;

	set->cmds[1].msg.channel = 0;
	set->cmds[1].msg.type = MIPI_DSI_DCS_LONG_WRITE;
	set->cmds[1].msg.flags = unicast ? MIPI_DSI_MSG_UNICAST : 0;
	set->cmds[1].msg.ctrl = unicast ? ctrl_idx : 0;
	set->cmds[1].msg.tx_len = ROI_CMD_LEN;
	set->cmds[1].msg.tx_buf = paset;
	set->cmds[1].msg.rx_len = 0;
	set->cmds[1].msg.rx_buf = 0;
	set->cmds[1].msg.wait_ms = 0;
	set->cmds[1].last_command = 1;
	set->cmds[1].post_wait_ms = 0;

	goto exit;

error_free_mem:
	kfree(caset);
	kfree(paset);
	kfree(set->cmds);

exit:
	return rc;
}

int dsi_panel_send_qsync_on_dcs(struct dsi_panel *panel,
		int ctrl_idx)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	pr_debug("ctrl:%d qsync on\n", ctrl_idx);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_QSYNC_ON cmds rc=%d\n",
		       panel->name, rc);

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_send_qsync_off_dcs(struct dsi_panel *panel,
		int ctrl_idx)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	pr_debug("ctrl:%d qsync off\n", ctrl_idx);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_QSYNC_OFF cmds rc=%d\n",
		       panel->name, rc);

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_send_roi_dcs(struct dsi_panel *panel, int ctrl_idx,
		struct dsi_rect *roi)
{
	int rc = 0;
	struct dsi_panel_cmd_set *set;
	struct dsi_display_mode_priv_info *priv_info;

	if (!panel || !panel->cur_mode) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	priv_info = panel->cur_mode->priv_info;
	set = &priv_info->cmd_sets[DSI_CMD_SET_ROI];

	rc = dsi_panel_roi_prepare_dcs_cmds(set, roi, ctrl_idx, true);
	if (rc) {
		pr_err("[%s] failed to prepare DSI_CMD_SET_ROI cmds, rc=%d\n",
				panel->name, rc);
		return rc;
	}
	pr_debug("[%s] send roi x %d y %d w %d h %d\n", panel->name,
			roi->x, roi->y, roi->w, roi->h);

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_ROI);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_ROI cmds, rc=%d\n",
				panel->name, rc);

	mutex_unlock(&panel->panel_lock);

	dsi_panel_destroy_cmd_packets(set);

	return rc;
}

int dsi_panel_switch(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (panel->funcs && panel->funcs->mode_switch) {
		rc = panel->funcs->mode_switch(panel);
		return rc;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_TIMING_SWITCH cmds, rc=%d\n",
		       panel->name, rc);

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_post_switch(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_POST_TIMING_SWITCH);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_POST_TIMING_SWITCH cmds, rc=%d\n",
		       panel->name, rc);

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_enable(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_ON);
	if (rc)
		pr_err("[%s] failed to send DSI_CMD_SET_ON cmds, rc=%d\n",
		       panel->name, rc);
	else
		panel->panel_initialized = true;
	mutex_unlock(&panel->panel_lock);

	if (!rc)
		rc = dsi_backlight_late_dpms(&panel->bl_config,
					       SDE_MODE_DPMS_ON);

	return rc;
}

int dsi_panel_post_enable(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	if (panel->funcs && panel->funcs->post_enable)
		return panel->funcs->post_enable(panel);

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_POST_ON);
	if (rc) {
		pr_err("[%s] failed to send DSI_CMD_SET_POST_ON cmds, rc=%d\n",
		       panel->name, rc);
		goto error;
	}
error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_pre_disable(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	dsi_backlight_early_dpms(&panel->bl_config, SDE_MODE_DPMS_OFF);

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_PRE_OFF);
	if (rc) {
		pr_err("[%s] failed to send DSI_CMD_SET_PRE_OFF cmds, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);

	if (!rc && panel->funcs && panel->funcs->pre_disable)
		rc = panel->funcs->pre_disable(panel);

	return rc;
}

int dsi_panel_disable(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	if (panel->vr_mode) {
		rc = dsi_panel_clear_vr_locked(panel);
		if (rc) {
			pr_warn("[%s] couldn't disable VR mode to unprepare display\n",
				panel->name);
		}
	}

	rc = dsi_panel_update_hbm_locked(panel, HBM_MODE_OFF);
	if (rc)
		pr_warn("[%s] couldn't disable HBM mode to unprepare display\n",
			panel->name);
	dsi_backlight_hbm_dimming_stop(&panel->bl_config);

	/* Avoid sending panel off commands when ESD recovery is underway */
	if (!atomic_read(&panel->esd_recovery_pending)) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_OFF);
		if (rc) {
			/*
			 * Sending panel off commands may fail when  DSI
			 * controller is in a bad state. These failures can be
			 * ignored since controller will go for full reset on
			 * subsequent display enable anyway.
			 */
			pr_warn_ratelimited("[%s] failed to send DSI_CMD_SET_OFF cmds, rc=%d\n",
					panel->name, rc);
			rc = 0;
		}
	}
	panel->panel_initialized = false;

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_unprepare(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_POST_OFF);
	if (rc) {
		pr_err("[%s] failed to send DSI_CMD_SET_POST_OFF cmds, rc=%d\n",
		       panel->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);

	if (!rc)
		rc = dsi_backlight_late_dpms(&panel->bl_config,
					       SDE_MODE_DPMS_OFF);

	if (panel->bl_config.bl_update == BL_UPDATE_DELAY_UNTIL_FIRST_FRAME)
		panel->bl_config.allow_bl_update = false;

	return rc;
}

int dsi_panel_post_unprepare(struct dsi_panel *panel)
{
	int rc = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = dsi_panel_power_off(panel);
	if (rc) {
		pr_err("[%s] panel power_Off failed, rc=%d\n",
		       panel->name, rc);
		goto error;
	}
error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_idle(struct dsi_panel *panel)
{
	if (unlikely(!panel))
		return -EINVAL;

	if (panel->funcs && panel->funcs->idle)
		return panel->funcs->idle(panel);

	return 0;
}

int dsi_panel_wakeup(struct dsi_panel *panel)
{
	if (unlikely(!panel))
		return -EINVAL;

	if (panel->funcs && panel->funcs->wakeup)
		return panel->funcs->wakeup(panel);

	return 0;
}
