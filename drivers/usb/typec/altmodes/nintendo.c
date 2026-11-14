// SPDX-License-Identifier: GPL-2.0
/*
 * Nintendo Switch Dock USB Type-C Alternate Mode driver
 *
 * The official Nintendo Switch Dock advertises an alternate mode at
 * SVID 0x057E (Nintendo's USB Implementers Forum vendor identifier).
 * Once a host has negotiated a PD contract with the dock and entered
 * this alternate mode, the dock accepts a small set of vendor commands
 * delivered as Unstructured VDMs that control the front LED, the
 * built-in USB hub, and the DP signal forwarded over the USB-C cable.
 *
 * The chip-level USB-PD broker (e.g. drivers/usb/typec/bm92txx.c on the
 * Switch's BM92T36) registers the partner altmode with the typec class
 * during Discover Mode; this driver binds to that altmode by SVID and
 * owns the dock-specific VDM protocol from then on.  Outgoing dock
 * commands go out through typec_altmode_vdm(), which the framework
 * routes back to the broker's send path; incoming replies arrive in
 * .vdm.
 *
 * Wire format reverse-engineered by CTCaer for the downstream Linux
 * port of the Switch:
 *
 *   VDM header  : [REQST/REPLY | 0x00 | SVID_LO 0x7E | SVID_HI 0x05]
 *                 (bit 15 = 0 => Unstructured VDM)
 *
 *   VDO[0]      : [R/W flag    | SOURCE (HOST=1, DOCK=2) | CMD | 0x00]
 *
 *   VDO[1+]     : command-specific payload
 *                 (LED: { fade, time_off, time_on, duty })
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/usb/pd_vdo.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>

#define NINTENDO_SVID		0x057E

/* Header low-byte tag distinguishing direction. */
#define NIN_HDR_REQST		0x00
#define NIN_HDR_REPLY		0x40

/* VDO[0] field encoding. */
#define NIN_RW_READ		0
#define NIN_RW_WRITE		1
#define NIN_SRC_HOST		1
#define NIN_SRC_DOCK		2

/* Dock command codes (VDO[0] byte 2). */
#define NIN_CMD_LED_CONTROL		0x01
#define NIN_CMD_DEVICE_STATE		0x16
#define NIN_CMD_DP_SIGNAL_DISABLE	0x1C
#define NIN_CMD_HUB_RESET		0x1E
#define NIN_CMD_HUB_CONTROL		0x20

struct nintendo_altmode {
	struct typec_altmode	*alt;
	struct mutex		lock;	/* serialises sysfs writes vs. VDM RX */
	bool			entered;

	/* Last LED parameters programmed.  Held so a userspace read of
	 * brightness reflects what we last asked for; the dock does not
	 * report current LED state back synchronously.
	 */
	u8			led_duty;
	u8			led_time_on;
	u8			led_time_off;
	u8			led_fade;
};

/* Build the Unstructured VDM header used by every dock command. */
static u32 nintendo_make_header(u8 tag)
{
	return ((u32)NINTENDO_SVID << 16) | tag;
}

/* Build VDO[0] from the command fields. */
static u32 nintendo_make_cmd_vdo(u8 rw, u8 src, u8 cmd)
{
	return ((u32)rw) | ((u32)src << 8) | ((u32)cmd << 16);
}

static int nintendo_send_cmd(struct nintendo_altmode *nin, u8 rw, u8 cmd,
			     const u8 *payload, unsigned int plen)
{
	u32 vdo[2] = { 0, 0 };
	unsigned int nvdos = 1;
	u32 hdr;

	vdo[0] = nintendo_make_cmd_vdo(rw, NIN_SRC_HOST, cmd);

	if (plen) {
		if (plen > sizeof(u32))
			return -EINVAL;
		memcpy(&vdo[1], payload, plen);
		nvdos = 2;
	}

	hdr = nintendo_make_header(NIN_HDR_REQST);
	return typec_altmode_vdm(nin->alt, hdr, vdo, nvdos);
}

static int nintendo_program_led(struct nintendo_altmode *nin)
{
	u8 payload[4] = {
		nin->led_fade,
		nin->led_time_off,
		nin->led_time_on,
		nin->led_duty,
	};

	return nintendo_send_cmd(nin, NIN_RW_WRITE, NIN_CMD_LED_CONTROL,
				 payload, sizeof(payload));
}

/* -------------------------------- sysfs --------------------------------- */

static ssize_t brightness_show(struct device *dev,
			       struct device_attribute *a, char *buf)
{
	struct nintendo_altmode *nin = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", nin->led_duty);
}

static ssize_t brightness_store(struct device *dev,
				struct device_attribute *a,
				const char *buf, size_t n)
{
	struct nintendo_altmode *nin = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;

	guard(mutex)(&nin->lock);

	nin->led_duty		= val;
	nin->led_time_on	= 0xff;
	nin->led_time_off	= 0;
	nin->led_fade		= 0;

	ret = nintendo_program_led(nin);
	return ret ? ret : n;
}
static DEVICE_ATTR_RW(brightness);

static ssize_t hub_reset_store(struct device *dev,
			       struct device_attribute *a,
			       const char *buf, size_t n)
{
	struct nintendo_altmode *nin = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "0"))
		return n;

	guard(mutex)(&nin->lock);
	ret = nintendo_send_cmd(nin, NIN_RW_READ, NIN_CMD_HUB_RESET, NULL, 0);
	return ret ? ret : n;
}
static DEVICE_ATTR_WO(hub_reset);

static struct attribute *nintendo_dock_attrs[] = {
	&dev_attr_brightness.attr,
	&dev_attr_hub_reset.attr,
	NULL,
};

static const struct attribute_group nintendo_dock_group = {
	.name	= "nintendo_dock",
	.attrs	= nintendo_dock_attrs,
};

/* --------------------------- altmode framework -------------------------- */

static int nintendo_altmode_vdm(struct typec_altmode *alt,
				const u32 hdr, const u32 *vdo, int count)
{
	struct nintendo_altmode *nin = typec_altmode_get_drvdata(alt);

	if (PD_VDO_SVDM(hdr)) {
		/* Structured VDM: Enter / Exit ACK or NAK. */
		u8 cmdt = PD_VDO_CMDT(hdr);
		u8 cmd  = PD_VDO_CMD(hdr);

		if (cmdt == CMDT_RSP_ACK && cmd == CMD_ENTER_MODE) {
			scoped_guard(mutex, &nin->lock)
				nin->entered = true;
			typec_altmode_update_active(alt, true);
			nintendo_program_led(nin);
		} else if (cmd == CMD_EXIT_MODE) {
			scoped_guard(mutex, &nin->lock)
				nin->entered = false;
			typec_altmode_update_active(alt, false);
		}
		return 0;
	}

	/*
	 * Unstructured VDM from the dock.  These are replies to the host's
	 * dock-protocol commands; for now they are accepted and traced.  A
	 * future revision could correlate them with outstanding requests
	 * for synchronous sysfs writes.
	 */
	dev_dbg(&alt->dev, "dock VDM rx hdr=0x%08x cnt=%d vdo0=0x%08x\n",
		hdr, count, count ? vdo[0] : 0);
	return 0;
}

static int nintendo_altmode_activate(struct typec_altmode *alt, int activate)
{
	return activate ? typec_altmode_enter(alt, NULL)
			: typec_altmode_exit(alt);
}

static const struct typec_altmode_ops nintendo_altmode_ops = {
	.vdm		= nintendo_altmode_vdm,
	.activate	= nintendo_altmode_activate,
};

/* ------------------------------ driver core ----------------------------- */

static int nintendo_altmode_probe(struct typec_altmode *alt)
{
	struct nintendo_altmode *nin;
	int ret;

	nin = devm_kzalloc(&alt->dev, sizeof(*nin), GFP_KERNEL);
	if (!nin)
		return -ENOMEM;

	mutex_init(&nin->lock);
	nin->alt	   = alt;
	nin->led_duty	   = 0xff;
	nin->led_time_on   = 0xff;
	nin->led_time_off  = 0;
	nin->led_fade	   = 0;

	typec_altmode_set_drvdata(alt, nin);
	typec_altmode_set_ops(alt, &nintendo_altmode_ops);

	ret = sysfs_create_group(&alt->dev.kobj, &nintendo_dock_group);
	if (ret)
		return ret;

	ret = typec_altmode_enter(alt, NULL);
	if (ret) {
		sysfs_remove_group(&alt->dev.kobj, &nintendo_dock_group);
		return ret;
	}

	return 0;
}

static void nintendo_altmode_remove(struct typec_altmode *alt)
{
	sysfs_remove_group(&alt->dev.kobj, &nintendo_dock_group);
}

static const struct typec_device_id nintendo_typec_id[] = {
	{ NINTENDO_SVID },
	{ },
};
MODULE_DEVICE_TABLE(typec, nintendo_typec_id);

static struct typec_altmode_driver nintendo_altmode_driver = {
	.id_table	= nintendo_typec_id,
	.probe		= nintendo_altmode_probe,
	.remove		= nintendo_altmode_remove,
	.driver = {
		.name	= "typec-nintendo",
	},
};
module_typec_altmode_driver(nintendo_altmode_driver);

MODULE_AUTHOR("Alexandre Hamamdjian <azkali.limited@gmail.com>");
MODULE_AUTHOR("CTCaer <ctcaer@gmail.com>");
MODULE_DESCRIPTION("Nintendo Switch Dock USB Type-C Alternate Mode driver");
MODULE_LICENSE("GPL");
