/*
 * Copyright (C) 2018 SwtcR <swtcr0@gmail.com>
 * Copyright (C) 2023-2024 Azkali <a.ffcc7@gmail.com>
 *
 * Based on Sharp ls043t1le01 panel driver by Werner Johansson <werner.johansson@sonymobile.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>

/*! MIPI DCS Panel Private CMDs. */
#define MIPI_DCS_PRIV_SM_SET_COLOR_MODE      ((u8)0xA0)
#define MIPI_DCS_PRIV_SM_SET_REG_OFFSET      ((u8)0xB0)
#define MIPI_DCS_PRIV_SM_SET_ELVSS           ((u8)0xB1) /* OLED backlight tuning. Byte7: PWM transition time in frames. */
#define MIPI_DCS_PRIV_SET_POWER_CONTROL      ((u8)0xB1)
#define MIPI_DCS_PRIV_SET_EXTC               ((u8)0xB9) /* Enable extended commands. */
#define MIPI_DCS_PRIV_UNK_BD                 ((u8)0xBD)
#define MIPI_DCS_PRIV_UNK_D5                 ((u8)0xD5)
#define MIPI_DCS_PRIV_UNK_D6                 ((u8)0xD6)
#define MIPI_DCS_PRIV_UNK_D8                 ((u8)0xD8)
#define MIPI_DCS_PRIV_UNK_D9                 ((u8)0xD9)

#define MIPI_DCS_PRIV_SM_SET_REGS_LOCK       ((u8)0xE2)

/* BL Control */
#define DCS_CONTROL_DISPLAY_SM_FLASHLIGHT    ((u8)BIT(2))
#define DCS_CONTROL_DISPLAY_BACKLIGHT_CTRL   ((u8)BIT(2))
#define DCS_CONTROL_DISPLAY_DIMMING_CTRL     ((u8)BIT(3))
#define DCS_CONTROL_DISPLAY_BRIGHTNESS_CTRL  ((u8)BIT(5))

/* OLED Panels color mode */
#define DCS_SM_COLOR_MODE_SATURATED          ((u8)0x00) /* Disabled. Similar to vivid but over-saturated. Wide gamut? */
#define DCS_SM_COLOR_MODE_WASHED             ((u8)0x45)
#define DCS_SM_COLOR_MODE_BASIC              ((u8)0x03)
#define DCS_SM_COLOR_MODE_POR_RESET          ((u8)0x20) /* Reset value on power on. */
#define DCS_SM_COLOR_MODE_NATURAL            ((u8)0x23) /* Not actually natural.. */
#define DCS_SM_COLOR_MODE_VIVID              ((u8)0x65)
#define DCS_SM_COLOR_MODE_NIGHT0             ((u8)0x43) /* Based on washed out. */
#define DCS_SM_COLOR_MODE_NIGHT1             ((u8)0x15) /* Based on basic. */
#define DCS_SM_COLOR_MODE_NIGHT2             ((u8)0x35) /* Based on natural. */
#define DCS_SM_COLOR_MODE_NIGHT3             ((u8)0x75) /* Based on vivid. */

#define DCS_SM_COLOR_MODE_ENABLE             ((u8)BIT(0))

#define PANEL_SM_BL_CANDELA_MAX              2047

/* Retro Remake panels report their own maker byte, 0xE1 when driven at 1080p. */
#define PANEL_RR_MAKER                       ((u8)0xE0)
#define PANEL_RR_MAKER_HD                    ((u8)0xE1)

enum
{
	PANEL_JDI_XXX062M     = 0x10,
	PANEL_JDI_LAM062M109A = 0x0910,
	PANEL_JDI_LPM062M326A = 0x2610,
	PANEL_INL_P062CCA_AZ1 = 0x0F20,
	PANEL_AUO_A062TAN01   = 0x0F30,
	PANEL_INL_2J055IA_27A = 0x1020,
	PANEL_AUO_A055TAN01   = 0x1030,
	PANEL_SHP_LQ055T1SW10 = 0x1040,
	PANEL_SAM_AMS699VC01  = 0x2050,
	PANEL_RR_SUPER5_OLED_V1  = 0x10E0,
	PANEL_RR_SUPER5_OLED_HD_V1  = 0x10E1,
	PANEL_RR_SUPER7_IPS_V1  = 0x0FE0,
	PANEL_RR_SUPER7_IPS_HD_V1  = 0x0FE1,
	PANEL_RR_SUPER7_OLED_7_V1  = 0x20E0,
	PANEL_RR_SUPER7_OLED_HD_7_V1  = 0x20E1,

	// Found on 6/2" clones. Unknown markings. Quality seems JDI like. Has bad low backlight scaling. ID: [83] 94 [0F].
	PANEL_OEM_CLONE_6_2   = 0x0F83,
	// Found on 5.5" clones with AUO A055TAN02 (59.05A30.001) fake markings.
	PANEL_OEM_CLONE_5_5   = 0x00B3,
	// Found on 5.5" clones with AUO A055TAN02 (59.05A30.001) fake markings.
	PANEL_OEM_CLONE       = 0x0000
};

struct init_cmd {
	u8 cmd;
	int length;
	u8 data[64];
};

struct nx_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;
	struct regulator *supply1;
	struct regulator *supply2;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;

	struct init_cmd *init_cmds;
	struct init_cmd *suspend_cmds;

	bool dcs_backlight;

	u16 display_id;
};

struct init_cmd init_cmds_default[] = {
	{ MIPI_DCS_EXIT_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		120,
	},
	{ MIPI_DCS_SET_DISPLAY_ON, 0, { 0x00 } },
	{
		0xFF,
		20,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd init_cmds_PANEL_JDI_XXX062M[] = {
	/* SET_EXTC enable: long write, 3 data bytes */
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	/* Page select 0: short write w/ 1 param (length=1) */
	{ MIPI_DCS_PRIV_UNK_BD, 1, { 0x00 } },
	{ MIPI_DCS_PRIV_UNK_D8, 24, { 0xAA, 0xAA, 0xAA, 0xEB, 0xAA, 0xAA,
				      0xAA, 0xAA, 0xAA, 0xEB, 0xAA, 0xAA,
				      0xAA, 0xAA, 0xAA, 0xEB, 0xAA, 0xAA,
				      0xAA, 0xAA, 0xAA, 0xEB, 0xAA, 0xAA } },
	{ MIPI_DCS_PRIV_UNK_BD, 1, { 0x01 } },
	{ MIPI_DCS_PRIV_UNK_D8,
	  38,
	  { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } },
	{ MIPI_DCS_PRIV_UNK_BD, 1, { 0x02 } },
	{ MIPI_DCS_PRIV_UNK_D8,
	  14,
	  { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF } },
	{ MIPI_DCS_PRIV_UNK_BD, 1, { 0x00 } },
	{ MIPI_DCS_PRIV_UNK_D9, 1, { 0x06 } },
	/* SET_EXTC disable */
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0x00, 0x00, 0x00 } },
	/* DCS short writes: no params (length=0) */
	{ MIPI_DCS_EXIT_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		180,
	},
	{ MIPI_DCS_SET_DISPLAY_ON, 0, { 0x00 } },
	{
		0xFF,
		20,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_JDI_XXX062M[] = {
	{ MIPI_DCS_SET_DISPLAY_OFF, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{ MIPI_DCS_PRIV_UNK_D5,
	  32,
	  { 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
	    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
	    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19 } },
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL,
	  10,
	  { 0x41, 0x0F, 0x4F, 0x33, 0xA4, 0x79, 0xF1, 0x81, 0x2D, 0x00 } },
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0x00, 0x00, 0x00 } },
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd init_cmds_PANEL_SAM_AMS699VC01[] = {
	{ MIPI_DCS_EXIT_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		180,
	},

	// Set color mode to basic (natural). Stock is Saturated (0x00). (Reset value is 0x20).
	{ MIPI_DCS_PRIV_SM_SET_COLOR_MODE, 1, { DCS_SM_COLOR_MODE_BASIC } },

	// Enable backlight and smooth PWM.
	{ MIPI_DCS_WRITE_CONTROL_DISPLAY, 1,
	  { DCS_CONTROL_DISPLAY_BRIGHTNESS_CTRL | DCS_CONTROL_DISPLAY_DIMMING_CTRL } },

	// Unlock Level 2 registers.
	{ MIPI_DCS_PRIV_SM_SET_REGS_LOCK, 4, { 0x5A, 0x5A, 0x5A, 0x5A } },

	// Set registers offset and set PWM transition to 6 frames (100ms).
	{ MIPI_DCS_PRIV_SM_SET_REG_OFFSET, 1, { 0x07 } },
	{ MIPI_DCS_PRIV_SM_SET_ELVSS, 1, { 0x06 } },

	// Relock Level 2 registers.
	{ MIPI_DCS_PRIV_SM_SET_REGS_LOCK, 4, { 0x5A, 0x5A, 0xA5, 0xA5 } },

	// MIPI_DCS_SET_BRIGHTNESS 0000: 0%. FF07: 100%.
	{ MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 2, { 0x00, 0x00 } },

	{
		0xFF,
		5,
	},
	{ MIPI_DCS_SET_DISPLAY_ON, 0, { 0x00 } },
	{
		0xFF,
		20,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd init_cmds_PANEL_AUO_A062TAN01[] = {
	{ MIPI_DCS_EXIT_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		180,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL, 6, { 0x48, 0x11, 0x71, 0x09, 0x32, 0x14 } },
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_AUO_A062TAN01[] = {
	{
		0xFF,
		100,
	},
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_AUO_A055TAN01[] = {
	{
		0xFF,
		100,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL, 10, { 0x48, 0x11, 0x71, 0x09, 0x32, 0x14, 0x71, 0x31, 0x4D, 0x11 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd init_cmds_PANEL_INL_P062CCA_AZ1[] = {
	{ MIPI_DCS_EXIT_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		180,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL, 6, { 0x48, 0x15, 0x75, 0x09, 0x32, 0x14 } },

	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_INL_2J055IA_27A[] = {
	{
		0xFF,
		100,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL, 10, { 0x48, 0x15, 0x75, 0x09, 0x32, 0x14, 0x71, 0x31, 0x4D, 0x11 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_SHP_LQ055T1SW10[] = {
	{
		0xFF,
		100,
	},
	{ MIPI_DCS_PRIV_SET_EXTC, 3, { 0xFF, 0x83, 0x94 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_PRIV_SET_POWER_CONTROL, 10, { 0x48, 0x13, 0x73, 0x09, 0x32, 0x24, 0x71, 0x31, 0x4C, 0x00 } },
	{
		0xFF,
		5,
	},
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		50,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

struct init_cmd suspend_cmds_PANEL_SAM_AMS699VC01[] = {
	{
		0xFF,
		100,
	},
	{ MIPI_DCS_ENTER_SLEEP_MODE, 0, { 0x00 } },
	{
		0xFF,
		120,
	},
	{ MIPI_DCS_NOP, -1, { 0x00 } },
};

static inline struct nx_panel *to_nx_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nx_panel, base);
}

static int nx_panel_dcs_backlight_update(struct backlight_device *bl)
{
	struct nx_panel *nx = bl_get_data(bl);
	u16 candela;

	int ret;

	candela = backlight_get_brightness(bl) * PANEL_SM_BL_CANDELA_MAX / 255;

	ret = mipi_dsi_dcs_set_display_brightness_large(nx->dsi, candela);
	if (ret < 0)
		dev_err(&nx->dsi->dev, "failed to set brightness %u: %d\n",
			candela, ret);

	return ret;
}

static const struct backlight_ops nx_panel_dcs_backlight_ops = {
	.update_status = nx_panel_dcs_backlight_update,
};

static int nx_panel_add_dcs_backlight(struct nx_panel *nx)
{
	struct device *dev = &nx->dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 128,
	};

	nx->backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
						       nx,
						       &nx_panel_dcs_backlight_ops,
						       &props);
	if (IS_ERR(nx->backlight)) {
		int err = PTR_ERR(nx->backlight);

		nx->backlight = NULL;
		return err;
	}

	nx->dcs_backlight = true;

	dev_info(dev, "using DCS backlight\n");

	return 0;
}

static void nx_panel_detect(struct nx_panel *nx)
{
	u8 id[3] = { 0 };
	int ret;
	nx->init_cmds = NULL;
	nx->suspend_cmds = NULL;

	printk("nx_panel_detect");

	nx->display_id = 0;

	ret = mipi_dsi_dcs_read(nx->dsi, MIPI_DCS_GET_DISPLAY_ID, id, sizeof(id));
	if (ret < 0) {
		dev_err(&nx->dsi->dev, "failed to read panel ID: %d\n", ret);
	} else {
		nx->display_id = (id[2] << 8) | id[0];
		dev_info(&nx->dsi->dev, "display ID[%d]: %02x %02x %02x -> %04x\n",
			 ret, id[0], id[1], id[2], nx->display_id);
	}

	if ((nx->display_id & 0xFF) == PANEL_JDI_XXX062M)
		nx->display_id = PANEL_JDI_XXX062M;

	if (nx->display_id == PANEL_SAM_AMS699VC01 && !nx->backlight) {
		ret = nx_panel_add_dcs_backlight(nx);
		if (ret < 0)
			dev_err(&nx->dsi->dev,
				"failed to register DCS backlight: %d\n", ret);
	}

	dev_info(&nx->dsi->dev,
		 "setting init sequence for ID %04x\n", nx->display_id);

	switch (nx->display_id) {
	case PANEL_JDI_XXX062M:
	case PANEL_JDI_LAM062M109A:
	case PANEL_JDI_LPM062M326A:
		nx->init_cmds = init_cmds_PANEL_JDI_XXX062M;
		break;
	case PANEL_SAM_AMS699VC01:
		nx->init_cmds = init_cmds_PANEL_SAM_AMS699VC01;
		break;
	case PANEL_INL_P062CCA_AZ1:
		nx->init_cmds = init_cmds_PANEL_INL_P062CCA_AZ1;
		break;
	case PANEL_AUO_A062TAN01:
		nx->init_cmds = init_cmds_PANEL_AUO_A062TAN01;
		break;
	case PANEL_RR_SUPER5_OLED_V1:
	case PANEL_RR_SUPER5_OLED_HD_V1:
		nx->init_cmds = init_cmds_default;
		break;
	case PANEL_INL_2J055IA_27A:
	case PANEL_AUO_A055TAN01:
	case PANEL_SHP_LQ055T1SW10:
	default:
		if ((nx->display_id & 0xFF) == 0x10) {
			dev_info(&nx->dsi->dev,
				 "unknown JDI variant %04x, using JDI XXX062M init sequence\n",
				 nx->display_id);
			nx->init_cmds = init_cmds_PANEL_JDI_XXX062M;
		} else {
			dev_info(&nx->dsi->dev, "using default init sequence\n");
			nx->init_cmds = init_cmds_default;
		}
		break;
	}

	dev_info(&nx->dsi->dev,
		"setting suspend sequence for ID %04x\n", nx->display_id);

	switch (nx->display_id) {
	case PANEL_JDI_XXX062M:
	case PANEL_JDI_LAM062M109A:
	case PANEL_JDI_LPM062M326A:
		nx->suspend_cmds = suspend_cmds_PANEL_JDI_XXX062M;
		break;
	case PANEL_AUO_A062TAN01:
		nx->suspend_cmds = suspend_cmds_PANEL_AUO_A062TAN01;
		break;
	case PANEL_INL_2J055IA_27A:
		nx->suspend_cmds = suspend_cmds_PANEL_INL_2J055IA_27A;
		break;
	case PANEL_AUO_A055TAN01:
		nx->suspend_cmds = suspend_cmds_PANEL_AUO_A055TAN01;
		break;
	case PANEL_SHP_LQ055T1SW10:
		nx->suspend_cmds = suspend_cmds_PANEL_SHP_LQ055T1SW10;
		break;
	case PANEL_SAM_AMS699VC01:
		nx->suspend_cmds = suspend_cmds_PANEL_SAM_AMS699VC01;
		break;
	case PANEL_INL_P062CCA_AZ1:
	default:
		if ((nx->display_id & 0xFF) == 0x10) {
			dev_info(&nx->dsi->dev,
				 "unknown JDI variant %04x, using JDI XXX062M suspend sequence\n",
				 nx->display_id);
			nx->suspend_cmds = suspend_cmds_PANEL_JDI_XXX062M;
		} else {
			dev_info(&nx->dsi->dev, "using default suspend sequence\n");
		}
		break;
	}

	msleep(20);
}

static int nx_mipi_dsi_dcs_cmds(struct init_cmd *cmds, struct nx_panel *nx)
{
	int ret = 0;

	while (cmds && cmds->length != -1) {
		if (cmds->cmd == 0xFF)
			msleep(cmds->length);
		else {
			ret = mipi_dsi_dcs_write(nx->dsi, cmds->cmd,
						 cmds->data, cmds->length);
			if (ret < 0) {
				dev_err(&nx->dsi->dev,
					"failed to write dsi_cmd: %d error: %d\n",
					cmds->cmd, ret);
				return ret;
			}
		}
		cmds++;
	}

	return ret;
}

static int nx_panel_init(struct nx_panel *nx)
{
	struct mipi_dsi_device *dsi = nx->dsi;
	struct device *dev = &nx->dsi->dev;
	int ret;

	printk("nx_panel_init");

	/*
	 * Reads need MIPI_DSI_MSG_USE_LPM (BTA over LP) — DCS reads in HS
	 * never get a turnaround response, so MIPI_DCS_GET_DISPLAY_ID returns
	 * zeros. Set MODE_LPM only for the read path.
	 */
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_set_maximum_return_packet_size(dsi, 3);
	if (ret < 0) {
		dev_err(dev, "failed to set maximum return packet size: %d\n",
			ret);
		return ret;
	}

	nx_panel_detect(nx);

	/*
	 * Send the full init sequence in LP (escape mode). Hekate and the
	 * downstream L4T tegra-dsi driver both push the JDI init array via
	 * the command-mode/escape path, not HS — the panel's command engine
	 * only accepts EXTC/UNK_BD/UNK_D8 register writes from LP. Driving
	 * them in HS leaves the panel without the EXTC unlock, so even after
	 * EXIT_SLEEP_MODE/SET_DISPLAY_ON later in the array the scanout pipe
	 * never opens and the user only sees backlight.
	 */
	ret = nx_mipi_dsi_dcs_cmds(nx->init_cmds, nx);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_column_address(dsi, 0, nx->mode->hdisplay - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set page address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi, 0, nx->mode->vdisplay - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "failed to set vblank tear on: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_pixel_format(dsi, MIPI_DCS_PIXEL_FMT_24BIT);
	if (ret < 0) {
		dev_err(dev, "failed to set pixel format: %d\n", ret);
		return ret;
	}

	return 0;
}

static int nx_panel_enable(struct drm_panel *panel)
{
	struct nx_panel *nx = to_nx_panel(panel);
	int ret;

	if (nx->enabled)
		return 0;

	ret = backlight_enable(nx->backlight);
	if (ret < 0)
		dev_err(&nx->dsi->dev, "failed to enable backlight: %d\n", ret);

	nx->enabled = true;

	return 0;
}

static int nx_panel_disable(struct drm_panel *panel)
{
	struct nx_panel *nx = to_nx_panel(panel);

	if (!nx->enabled)
		return 0;

	backlight_disable(nx->backlight);

	nx->enabled = false;

	return 0;
}

static int nx_panel_unprepare(struct drm_panel *panel)
{
	struct nx_panel *nx = to_nx_panel(panel);
	int ret;

	if (!nx->prepared)
		return 0;

	nx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = nx_mipi_dsi_dcs_cmds(nx->suspend_cmds, nx);
	if (ret < 0)
		dev_err(&nx->dsi->dev, "failed to write suspend cmds: %d\n",
			ret);

	if (nx->reset_gpio)
		gpiod_set_value(nx->reset_gpio, 0);

	msleep(10);
	if (nx->supply2)
		regulator_disable(nx->supply2);
	msleep(10);
	if (nx->supply1)
		regulator_disable(nx->supply1);

	nx->prepared = false;

	return 0;
}

static int nx_panel_prepare(struct drm_panel *panel)
{
	struct nx_panel *nx = to_nx_panel(panel);
	struct device *dev = &nx->dsi->dev;
	int ret;

	printk("nx panel prepare");

	if (nx->prepared)
		return 0;

	if (nx->supply1) {
		ret = regulator_enable(nx->supply1);
		if (ret < 0)
			return ret;
		msleep(10);
	}

	if (nx->supply2) {
		ret = regulator_enable(nx->supply2);
		if (ret < 0)
			goto poweroff;
		msleep(10);
	}

	if (nx->reset_gpio) {
		gpiod_set_value(nx->reset_gpio, 0);
		msleep(10);
		gpiod_set_value(nx->reset_gpio, 1);
		msleep(60);
	}

	ret = nx_panel_init(nx);
	if (ret < 0) {
		dev_err(dev, "failed to init panel: %d\n", ret);
		goto reset;
	}

	nx->prepared = true;

	return 0;

reset:
	if (nx->reset_gpio)
		gpiod_set_value(nx->reset_gpio, 0);
	if (nx->supply2)
		regulator_disable(nx->supply2);

poweroff:
	if (nx->supply1)
		regulator_disable(nx->supply1);
	return ret;
}

static const struct drm_display_mode default_mode = {
	.clock = 78000,
	.hdisplay = 720,
	.hsync_start = 720 + 136,
	.hsync_end = 720 + 136 + 72,
	.htotal = 720 + 136 + 72 + 72,
	.vdisplay = 1280,
	.vsync_start = 1280 + 10,
	.vsync_end = 1280 + 10 + 2,
	.vtotal = 1280 + 10 + 1 + 9,
	.width_mm = 77,
	.height_mm = 137,
};

static const struct drm_display_mode fhd_mode = {
	.clock = 133710,
	.hdisplay = 1080,
	.hsync_start = 1080 + 26,
	.hsync_end = 1080 + 26 + 4,
	.htotal = 1080 + 26 + 4 + 34,
	.vdisplay = 1920,
	.vsync_start = 1920 + 8,
	.vsync_end = 1920 + 8 + 4,
	.vtotal = 1920 + 8 + 4 + 16,
	.width_mm = 68,
	.height_mm = 121,
};

static u16 nx_panel_bootloader_id(struct device *dev)
{
	unsigned int offset;
	struct regmap *regmap;
	unsigned int value;

	regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
						      "nintendo,panel-id", 1,
						      &offset);
	if (IS_ERR(regmap))
		return 0;

	if (regmap_read(regmap, offset, &value) < 0)
		return 0;

	return value & 0xFFFF;
}

static int nx_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct nx_panel *nx = to_nx_panel(panel);
	struct device *dev = &nx->dsi->dev;

	printk("nx panel get_modes");

	mode = drm_mode_duplicate(connector->dev, nx->mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			nx->mode->hdisplay, nx->mode->vdisplay,
			drm_mode_vrefresh(nx->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = nx->mode->width_mm;
	connector->display_info.height_mm = nx->mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs nx_panel_funcs = {
	.prepare = nx_panel_prepare,
	.unprepare = nx_panel_unprepare,
	.enable = nx_panel_enable,
	.disable = nx_panel_disable,
	.get_modes = nx_panel_get_modes,
};

static int nx_panel_add(struct nx_panel *nx)
{
	struct device *dev = &nx->dsi->dev;
	struct device_node *np;

	printk("nx_panel_add");

	nx->mode = &default_mode;

	if ((nx_panel_bootloader_id(dev) & 0xFF) == PANEL_RR_MAKER_HD)
		nx->mode = &fhd_mode;

	nx->supply1 = devm_regulator_get_optional(dev, "vdd1");
	if (IS_ERR(nx->supply1)) {
		if (PTR_ERR(nx->supply1) != -ENODEV)
			return PTR_ERR(nx->supply1);

		nx->supply1 = NULL;
	}

	nx->supply2 = devm_regulator_get_optional(dev, "vdd2");
	if (IS_ERR(nx->supply2)) {
		if (PTR_ERR(nx->supply2) != -ENODEV)
			return PTR_ERR(nx->supply2);

		nx->supply2 = NULL;
	}

	nx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(nx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(nx->reset_gpio));
		nx->reset_gpio = NULL;
	} else {
		gpiod_set_value(nx->reset_gpio, 0);
	}

	printk("backlight");

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (np) {
		nx->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!nx->backlight)
			return -EPROBE_DEFER;
	}

	printk("drm panel add");

	drm_panel_add(&nx->base);

	return 0;
}

static void nx_panel_del(struct nx_panel *nx)
{
	if (nx->base.dev)
		drm_panel_remove(&nx->base);

	if (nx->backlight && !nx->dcs_backlight)
		put_device(&nx->backlight->dev);
}

static int nx_panel_probe(struct mipi_dsi_device *dsi)
{
	struct nx_panel *nx;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_NO_EOT_PACKET;

	printk("nx_panel_probe");

	nx = devm_drm_panel_alloc(&dsi->dev, struct nx_panel, base,
				  &nx_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(nx))
		return PTR_ERR(nx);

	printk("set drvdata");

	mipi_dsi_set_drvdata(dsi, nx);

	nx->dsi = dsi;

	printk("add panel");

	ret = nx_panel_add(nx);
	if (ret < 0)
		return ret;

	printk("dsi attach");
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		nx_panel_del(nx);
		return ret;
	}

	return 0;
}

static void nx_panel_remove(struct mipi_dsi_device *dsi)
{
	struct nx_panel *nx = mipi_dsi_get_drvdata(dsi);
	int ret;

	printk("nx_panel_remove");

	ret = nx_panel_disable(&nx->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	nx_panel_del(nx);
}

static void nx_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct nx_panel *nx = mipi_dsi_get_drvdata(dsi);
	nx_panel_disable(&nx->base);
}

static const struct of_device_id nx_panel_of_match[] = {
	{
		.compatible = "nintendo,nx-dsi",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nx_panel_of_match);

static struct mipi_dsi_driver nx_panel_driver = {
	.driver = {
		.name = "panel-nx-dsi",
		.of_match_table = nx_panel_of_match,
	},
	.probe = nx_panel_probe,
	.remove = nx_panel_remove,
	.shutdown = nx_panel_shutdown,
};
module_mipi_dsi_driver(nx_panel_driver);

MODULE_AUTHOR("SwtcR <swtcr0@gmail.com>");
MODULE_DESCRIPTION("Nintendo Switch DSI (720x1280) panel driver");
MODULE_LICENSE("GPL v2");
