/*
 *  USB HID support for Linux
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2006 Jiri Kosina
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/unaligned.h>
#include <asm/byteorder.h>
#include <linux/input.h>
#include <linux/wait.h>

#undef DEBUG
#undef DEBUG_DATA

#include <linux/usb.h>

#include <linux/hid.h>
#include <linux/hiddev.h>
#include "usbhid.h"

/*
 * Version Information
 */

#define DRIVER_VERSION "v2.6"
#define DRIVER_AUTHOR "Andreas Gal, Vojtech Pavlik"
#define DRIVER_DESC "USB HID core driver"
#define DRIVER_LICENSE "GPL"

static char *hid_types[] = {"Device", "Pointer", "Mouse", "Device", "Joystick",
				"Gamepad", "Keyboard", "Keypad", "Multi-Axis Controller"};
/*
 * Module parameters.
 */

static unsigned int hid_mousepoll_interval;
module_param_named(mousepoll, hid_mousepoll_interval, uint, 0644);
MODULE_PARM_DESC(mousepoll, "Polling interval of mice");

/*
 * Input submission and I/O error handler.
 */

static void hid_io_error(struct hid_device *hid);

/* Start up the input URB */
static int hid_start_in(struct hid_device *hid)
{
	unsigned long flags;
	int rc = 0;
	struct usbhid_device *usbhid = hid->driver_data;

	spin_lock_irqsave(&usbhid->inlock, flags);
	if (hid->open > 0 && !test_bit(HID_SUSPENDED, &usbhid->iofl) &&
			!test_and_set_bit(HID_IN_RUNNING, &usbhid->iofl)) {
		rc = usb_submit_urb(usbhid->urbin, GFP_ATOMIC);
		if (rc != 0)
			clear_bit(HID_IN_RUNNING, &usbhid->iofl);
	}
	spin_unlock_irqrestore(&usbhid->inlock, flags);
	return rc;
}

/* I/O retry timer routine */
static void hid_retry_timeout(unsigned long _hid)
{
	struct hid_device *hid = (struct hid_device *) _hid;
	struct usbhid_device *usbhid = hid->driver_data;

	dev_dbg(&usbhid->intf->dev, "retrying intr urb\n");
	if (hid_start_in(hid))
		hid_io_error(hid);
}

/* Workqueue routine to reset the device or clear a halt */
static void hid_reset(struct work_struct *work)
{
	struct usbhid_device *usbhid =
		container_of(work, struct usbhid_device, reset_work);
	struct hid_device *hid = usbhid->hid;
	int rc_lock, rc = 0;

	if (test_bit(HID_CLEAR_HALT, &usbhid->iofl)) {
		dev_dbg(&usbhid->intf->dev, "clear halt\n");
		rc = usb_clear_halt(hid_to_usb_dev(hid), usbhid->urbin->pipe);
		clear_bit(HID_CLEAR_HALT, &usbhid->iofl);
		hid_start_in(hid);
	}

	else if (test_bit(HID_RESET_PENDING, &usbhid->iofl)) {
		dev_dbg(&usbhid->intf->dev, "resetting device\n");
		rc = rc_lock = usb_lock_device_for_reset(hid_to_usb_dev(hid), usbhid->intf);
		if (rc_lock >= 0) {
			rc = usb_reset_composite_device(hid_to_usb_dev(hid), usbhid->intf);
			if (rc_lock)
				usb_unlock_device(hid_to_usb_dev(hid));
		}
		clear_bit(HID_RESET_PENDING, &usbhid->iofl);
	}

	switch (rc) {
	case 0:
		if (!test_bit(HID_IN_RUNNING, &usbhid->iofl))
			hid_io_error(hid);
		break;
	default:
		err("can't reset device, %s-%s/input%d, status %d",
				hid_to_usb_dev(hid)->bus->bus_name,
				hid_to_usb_dev(hid)->devpath,
				usbhid->ifnum, rc);
		/* FALLTHROUGH */
	case -EHOSTUNREACH:
	case -ENODEV:
	case -EINTR:
		break;
	}
}

/* Main I/O error handler */
static void hid_io_error(struct hid_device *hid)
{
	unsigned long flags;
	struct usbhid_device *usbhid = hid->driver_data;

	spin_lock_irqsave(&usbhid->inlock, flags);

	/* Stop when disconnected */
	if (usb_get_intfdata(usbhid->intf) == NULL)
		goto done;

	/* When an error occurs, retry at increasing intervals */
	if (usbhid->retry_delay == 0) {
		usbhid->retry_delay = 13;	/* Then 26, 52, 104, 104, ... */
		usbhid->stop_retry = jiffies + msecs_to_jiffies(1000);
	} else if (usbhid->retry_delay < 100)
		usbhid->retry_delay *= 2;

	if (time_after(jiffies, usbhid->stop_retry)) {

		/* Retries failed, so do a port reset */
		if (!test_and_set_bit(HID_RESET_PENDING, &usbhid->iofl)) {
			schedule_work(&usbhid->reset_work);
			goto done;
		}
	}

	mod_timer(&usbhid->io_retry,
			jiffies + msecs_to_jiffies(usbhid->retry_delay));
done:
	spin_unlock_irqrestore(&usbhid->inlock, flags);
}

/*
 * Input interrupt completion handler.
 */

static void hid_irq_in(struct urb *urb)
{
	struct hid_device	*hid = urb->context;
	struct usbhid_device 	*usbhid = hid->driver_data;
	int			status;

	switch (urb->status) {
		case 0:			/* success */
			usbhid->retry_delay = 0;
			hid_input_report(urb->context, HID_INPUT_REPORT,
					 urb->transfer_buffer,
					 urb->actual_length, 1);
			break;
		case -EPIPE:		/* stall */
			clear_bit(HID_IN_RUNNING, &usbhid->iofl);
			set_bit(HID_CLEAR_HALT, &usbhid->iofl);
			schedule_work(&usbhid->reset_work);
			return;
		case -ECONNRESET:	/* unlink */
		case -ENOENT:
		case -ESHUTDOWN:	/* unplug */
			clear_bit(HID_IN_RUNNING, &usbhid->iofl);
			return;
		case -EILSEQ:		/* protocol error or unplug */
		case -EPROTO:		/* protocol error or unplug */
		case -ETIME:		/* protocol error or unplug */
		case -ETIMEDOUT:	/* Should never happen, but... */
			clear_bit(HID_IN_RUNNING, &usbhid->iofl);
			hid_io_error(hid);
			return;
		default:		/* error */
			warn("input irq status %d received", urb->status);
	}

	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status) {
		clear_bit(HID_IN_RUNNING, &usbhid->iofl);
		if (status != -EPERM) {
			err("can't resubmit intr, %s-%s/input%d, status %d",
					hid_to_usb_dev(hid)->bus->bus_name,
					hid_to_usb_dev(hid)->devpath,
					usbhid->ifnum, status);
			hid_io_error(hid);
		}
	}
}

/*
 * Find a report field with a specified HID usage.
 */
#if 0
struct hid_field *hid_find_field_by_usage(struct hid_device *hid, __u32 wanted_usage, int type)
{
	struct hid_report *report;
	int i;

	list_for_each_entry(report, &hid->report_enum[type].report_list, list)
		for (i = 0; i < report->maxfield; i++)
			if (report->field[i]->logical == wanted_usage)
				return report->field[i];
	return NULL;
}
#endif  /*  0  */

static int hid_submit_out(struct hid_device *hid)
{
	struct hid_report *report;
	struct usbhid_device *usbhid = hid->driver_data;

	report = usbhid->out[usbhid->outtail];

	hid_output_report(report, usbhid->outbuf);
	usbhid->urbout->transfer_buffer_length = ((report->size - 1) >> 3) + 1 + (report->id > 0);
	usbhid->urbout->dev = hid_to_usb_dev(hid);

	dbg("submitting out urb");

	if (usb_submit_urb(usbhid->urbout, GFP_ATOMIC)) {
		err("usb_submit_urb(out) failed");
		return -1;
	}

	return 0;
}

static int hid_submit_ctrl(struct hid_device *hid)
{
	struct hid_report *report;
	unsigned char dir;
	int len;
	struct usbhid_device *usbhid = hid->driver_data;

	report = usbhid->ctrl[usbhid->ctrltail].report;
	dir = usbhid->ctrl[usbhid->ctrltail].dir;

	len = ((report->size - 1) >> 3) + 1 + (report->id > 0);
	if (dir == USB_DIR_OUT) {
		hid_output_report(report, usbhid->ctrlbuf);
		usbhid->urbctrl->pipe = usb_sndctrlpipe(hid_to_usb_dev(hid), 0);
		usbhid->urbctrl->transfer_buffer_length = len;
	} else {
		int maxpacket, padlen;

		usbhid->urbctrl->pipe = usb_rcvctrlpipe(hid_to_usb_dev(hid), 0);
		maxpacket = usb_maxpacket(hid_to_usb_dev(hid), usbhid->urbctrl->pipe, 0);
		if (maxpacket > 0) {
			padlen = (len + maxpacket - 1) / maxpacket;
			padlen *= maxpacket;
			if (padlen > usbhid->bufsize)
				padlen = usbhid->bufsize;
		} else
			padlen = 0;
		usbhid->urbctrl->transfer_buffer_length = padlen;
	}
	usbhid->urbctrl->dev = hid_to_usb_dev(hid);

	usbhid->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE | dir;
	usbhid->cr->bRequest = (dir == USB_DIR_OUT) ? HID_REQ_SET_REPORT : HID_REQ_GET_REPORT;
	usbhid->cr->wValue = cpu_to_le16(((report->type + 1) << 8) | report->id);
	usbhid->cr->wIndex = cpu_to_le16(usbhid->ifnum);
	usbhid->cr->wLength = cpu_to_le16(len);

	dbg("submitting ctrl urb: %s wValue=0x%04x wIndex=0x%04x wLength=%u",
		usbhid->cr->bRequest == HID_REQ_SET_REPORT ? "Set_Report" : "Get_Report",
		usbhid->cr->wValue, usbhid->cr->wIndex, usbhid->cr->wLength);

	if (usb_submit_urb(usbhid->urbctrl, GFP_ATOMIC)) {
		err("usb_submit_urb(ctrl) failed");
		return -1;
	}

	return 0;
}

/*
 * Output interrupt completion handler.
 */

static void hid_irq_out(struct urb *urb)
{
	struct hid_device *hid = urb->context;
	struct usbhid_device *usbhid = hid->driver_data;
	unsigned long flags;
	int unplug = 0;

	switch (urb->status) {
		case 0:			/* success */
			break;
		case -ESHUTDOWN:	/* unplug */
			unplug = 1;
		case -EILSEQ:		/* protocol error or unplug */
		case -EPROTO:		/* protocol error or unplug */
		case -ECONNRESET:	/* unlink */
		case -ENOENT:
			break;
		default:		/* error */
			warn("output irq status %d received", urb->status);
	}

	spin_lock_irqsave(&usbhid->outlock, flags);

	if (unplug)
		usbhid->outtail = usbhid->outhead;
	else
		usbhid->outtail = (usbhid->outtail + 1) & (HID_OUTPUT_FIFO_SIZE - 1);

	if (usbhid->outhead != usbhid->outtail) {
		if (hid_submit_out(hid)) {
			clear_bit(HID_OUT_RUNNING, &usbhid->iofl);
			wake_up(&hid->wait);
		}
		spin_unlock_irqrestore(&usbhid->outlock, flags);
		return;
	}

	clear_bit(HID_OUT_RUNNING, &usbhid->iofl);
	spin_unlock_irqrestore(&usbhid->outlock, flags);
	wake_up(&hid->wait);
}

/*
 * Control pipe completion handler.
 */

static void hid_ctrl(struct urb *urb)
{
	struct hid_device *hid = urb->context;
	struct usbhid_device *usbhid = hid->driver_data;
	unsigned long flags;
	int unplug = 0;

	spin_lock_irqsave(&usbhid->ctrllock, flags);

	switch (urb->status) {
		case 0:			/* success */
			if (usbhid->ctrl[usbhid->ctrltail].dir == USB_DIR_IN)
				hid_input_report(urb->context, usbhid->ctrl[usbhid->ctrltail].report->type,
						urb->transfer_buffer, urb->actual_length, 0);
			break;
		case -ESHUTDOWN:	/* unplug */
			unplug = 1;
		case -EILSEQ:		/* protocol error or unplug */
		case -EPROTO:		/* protocol error or unplug */
		case -ECONNRESET:	/* unlink */
		case -ENOENT:
		case -EPIPE:		/* report not available */
			break;
		default:		/* error */
			warn("ctrl urb status %d received", urb->status);
	}

	if (unplug)
		usbhid->ctrltail = usbhid->ctrlhead;
	else
		usbhid->ctrltail = (usbhid->ctrltail + 1) & (HID_CONTROL_FIFO_SIZE - 1);

	if (usbhid->ctrlhead != usbhid->ctrltail) {
		if (hid_submit_ctrl(hid)) {
			clear_bit(HID_CTRL_RUNNING, &usbhid->iofl);
			wake_up(&hid->wait);
		}
		spin_unlock_irqrestore(&usbhid->ctrllock, flags);
		return;
	}

	clear_bit(HID_CTRL_RUNNING, &usbhid->iofl);
	spin_unlock_irqrestore(&usbhid->ctrllock, flags);
	wake_up(&hid->wait);
}

void usbhid_submit_report(struct hid_device *hid, struct hid_report *report, unsigned char dir)
{
	int head;
	unsigned long flags;
	struct usbhid_device *usbhid = hid->driver_data;

	if ((hid->quirks & HID_QUIRK_NOGET) && dir == USB_DIR_IN)
		return;

	if (usbhid->urbout && dir == USB_DIR_OUT && report->type == HID_OUTPUT_REPORT) {

		spin_lock_irqsave(&usbhid->outlock, flags);

		if ((head = (usbhid->outhead + 1) & (HID_OUTPUT_FIFO_SIZE - 1)) == usbhid->outtail) {
			spin_unlock_irqrestore(&usbhid->outlock, flags);
			warn("output queue full");
			return;
		}

		usbhid->out[usbhid->outhead] = report;
		usbhid->outhead = head;

		if (!test_and_set_bit(HID_OUT_RUNNING, &usbhid->iofl))
			if (hid_submit_out(hid))
				clear_bit(HID_OUT_RUNNING, &usbhid->iofl);

		spin_unlock_irqrestore(&usbhid->outlock, flags);
		return;
	}

	spin_lock_irqsave(&usbhid->ctrllock, flags);

	if ((head = (usbhid->ctrlhead + 1) & (HID_CONTROL_FIFO_SIZE - 1)) == usbhid->ctrltail) {
		spin_unlock_irqrestore(&usbhid->ctrllock, flags);
		warn("control queue full");
		return;
	}

	usbhid->ctrl[usbhid->ctrlhead].report = report;
	usbhid->ctrl[usbhid->ctrlhead].dir = dir;
	usbhid->ctrlhead = head;

	if (!test_and_set_bit(HID_CTRL_RUNNING, &usbhid->iofl))
		if (hid_submit_ctrl(hid))
			clear_bit(HID_CTRL_RUNNING, &usbhid->iofl);

	spin_unlock_irqrestore(&usbhid->ctrllock, flags);
}

static int usb_hidinput_input_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct hid_device *hid = dev->private;
	struct hid_field *field;
	int offset;

	if (type == EV_FF)
		return input_ff_event(dev, type, code, value);

	if (type != EV_LED)
		return -1;

	if ((offset = hidinput_find_field(hid, type, code, &field)) == -1) {
		warn("event field not found");
		return -1;
	}

	hid_set_field(field, offset, value);
	usbhid_submit_report(hid, field->report, USB_DIR_OUT);

	return 0;
}

int usbhid_wait_io(struct hid_device *hid)
{
	struct usbhid_device *usbhid = hid->driver_data;

	if (!wait_event_timeout(hid->wait, (!test_bit(HID_CTRL_RUNNING, &usbhid->iofl) &&
					!test_bit(HID_OUT_RUNNING, &usbhid->iofl)),
					10*HZ)) {
		dbg("timeout waiting for ctrl or out queue to clear");
		return -1;
	}

	return 0;
}

static int hid_set_idle(struct usb_device *dev, int ifnum, int report, int idle)
{
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
		HID_REQ_SET_IDLE, USB_TYPE_CLASS | USB_RECIP_INTERFACE, (idle << 8) | report,
		ifnum, NULL, 0, USB_CTRL_SET_TIMEOUT);
}

static int hid_get_class_descriptor(struct usb_device *dev, int ifnum,
		unsigned char type, void *buf, int size)
{
	int result, retries = 4;

	memset(buf,0,size);	// Make sure we parse really received data

	do {
		result = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
				USB_REQ_GET_DESCRIPTOR, USB_RECIP_INTERFACE | USB_DIR_IN,
				(type << 8), ifnum, buf, size, USB_CTRL_GET_TIMEOUT);
		retries--;
	} while (result < size && retries);
	return result;
}

int usbhid_open(struct hid_device *hid)
{
	++hid->open;
	if (hid_start_in(hid))
		hid_io_error(hid);
	return 0;
}

void usbhid_close(struct hid_device *hid)
{
	struct usbhid_device *usbhid = hid->driver_data;

	if (!--hid->open)
		usb_kill_urb(usbhid->urbin);
}

static int hidinput_open(struct input_dev *dev)
{
	struct hid_device *hid = dev->private;
	return usbhid_open(hid);
}

static void hidinput_close(struct input_dev *dev)
{
	struct hid_device *hid = dev->private;
	usbhid_close(hid);
}

#define USB_VENDOR_ID_PANJIT		0x134c

#define USB_VENDOR_ID_TURBOX		0x062a
#define USB_DEVICE_ID_TURBOX_KEYBOARD	0x0201

/*
 * Initialize all reports
 */

void usbhid_init_reports(struct hid_device *hid)
{
	struct hid_report *report;
	struct usbhid_device *usbhid = hid->driver_data;
	int err, ret;

	list_for_each_entry(report, &hid->report_enum[HID_INPUT_REPORT].report_list, list)
		usbhid_submit_report(hid, report, USB_DIR_IN);

	list_for_each_entry(report, &hid->report_enum[HID_FEATURE_REPORT].report_list, list)
		usbhid_submit_report(hid, report, USB_DIR_IN);

	err = 0;
	ret = usbhid_wait_io(hid);
	while (ret) {
		err |= ret;
		if (test_bit(HID_CTRL_RUNNING, &usbhid->iofl))
			usb_kill_urb(usbhid->urbctrl);
		if (test_bit(HID_OUT_RUNNING, &usbhid->iofl))
			usb_kill_urb(usbhid->urbout);
		ret = usbhid_wait_io(hid);
	}

	if (err)
		warn("timeout initializing reports");
}

#define USB_VENDOR_ID_GTCO		0x078c
#define USB_VENDOR_ID_GTCO_IPANEL_2     0x5543
#define USB_DEVICE_ID_GTCO_90		0x0090
#define USB_DEVICE_ID_GTCO_100		0x0100
#define USB_DEVICE_ID_GTCO_101		0x0101
#define USB_DEVICE_ID_GTCO_103		0x0103
#define USB_DEVICE_ID_GTCO_104		0x0104
#define USB_DEVICE_ID_GTCO_105		0x0105
#define USB_DEVICE_ID_GTCO_106		0x0106
#define USB_DEVICE_ID_GTCO_107		0x0107
#define USB_DEVICE_ID_GTCO_108		0x0108
#define USB_DEVICE_ID_GTCO_200		0x0200
#define USB_DEVICE_ID_GTCO_201		0x0201
#define USB_DEVICE_ID_GTCO_202		0x0202
#define USB_DEVICE_ID_GTCO_203		0x0203
#define USB_DEVICE_ID_GTCO_204		0x0204
#define USB_DEVICE_ID_GTCO_205		0x0205
#define USB_DEVICE_ID_GTCO_206		0x0206
#define USB_DEVICE_ID_GTCO_207		0x0207
#define USB_DEVICE_ID_GTCO_300		0x0300
#define USB_DEVICE_ID_GTCO_301		0x0301
#define USB_DEVICE_ID_GTCO_302		0x0302
#define USB_DEVICE_ID_GTCO_303		0x0303
#define USB_DEVICE_ID_GTCO_304		0x0304
#define USB_DEVICE_ID_GTCO_305		0x0305
#define USB_DEVICE_ID_GTCO_306		0x0306
#define USB_DEVICE_ID_GTCO_307		0x0307
#define USB_DEVICE_ID_GTCO_308		0x0308
#define USB_DEVICE_ID_GTCO_309		0x0309
#define USB_DEVICE_ID_GTCO_400		0x0400
#define USB_DEVICE_ID_GTCO_401		0x0401
#define USB_DEVICE_ID_GTCO_402		0x0402
#define USB_DEVICE_ID_GTCO_403		0x0403
#define USB_DEVICE_ID_GTCO_404		0x0404
#define USB_DEVICE_ID_GTCO_405		0x0405
#define USB_DEVICE_ID_GTCO_500		0x0500
#define USB_DEVICE_ID_GTCO_501		0x0501
#define USB_DEVICE_ID_GTCO_502		0x0502
#define USB_DEVICE_ID_GTCO_503		0x0503
#define USB_DEVICE_ID_GTCO_504		0x0504
#define USB_DEVICE_ID_GTCO_1000		0x1000
#define USB_DEVICE_ID_GTCO_1001		0x1001
#define USB_DEVICE_ID_GTCO_1002		0x1002
#define USB_DEVICE_ID_GTCO_1003		0x1003
#define USB_DEVICE_ID_GTCO_1004		0x1004
#define USB_DEVICE_ID_GTCO_1005		0x1005
#define USB_DEVICE_ID_GTCO_1006		0x1006
#define USB_DEVICE_ID_GTCO_8		0x0008
#define USB_DEVICE_ID_GTCO_d            0x000d

#define USB_VENDOR_ID_WACOM		0x056a

#define USB_VENDOR_ID_ACECAD		0x0460
#define USB_DEVICE_ID_ACECAD_FLAIR	0x0004
#define USB_DEVICE_ID_ACECAD_302	0x0008

#define USB_VENDOR_ID_KBGEAR		0x084e
#define USB_DEVICE_ID_KBGEAR_JAMSTUDIO	0x1001

#define USB_VENDOR_ID_AIPTEK		0x08ca
#define USB_DEVICE_ID_AIPTEK_01		0x0001
#define USB_DEVICE_ID_AIPTEK_10		0x0010
#define USB_DEVICE_ID_AIPTEK_20		0x0020
#define USB_DEVICE_ID_AIPTEK_21		0x0021
#define USB_DEVICE_ID_AIPTEK_22		0x0022
#define USB_DEVICE_ID_AIPTEK_23		0x0023
#define USB_DEVICE_ID_AIPTEK_24		0x0024

#define USB_VENDOR_ID_GRIFFIN		0x077d
#define USB_DEVICE_ID_POWERMATE		0x0410
#define USB_DEVICE_ID_SOUNDKNOB		0x04AA

#define USB_VENDOR_ID_ATEN		0x0557
#define USB_DEVICE_ID_ATEN_UC100KM	0x2004
#define USB_DEVICE_ID_ATEN_CS124U	0x2202
#define USB_DEVICE_ID_ATEN_2PORTKVM	0x2204
#define USB_DEVICE_ID_ATEN_4PORTKVM	0x2205
#define USB_DEVICE_ID_ATEN_4PORTKVMC	0x2208

#define USB_VENDOR_ID_TOPMAX		0x0663
#define USB_DEVICE_ID_TOPMAX_COBRAPAD	0x0103

#define USB_VENDOR_ID_HAPP		0x078b
#define USB_DEVICE_ID_UGCI_DRIVING	0x0010
#define USB_DEVICE_ID_UGCI_FLYING	0x0020
#define USB_DEVICE_ID_UGCI_FIGHTING	0x0030

#define USB_VENDOR_ID_MGE		0x0463
#define USB_DEVICE_ID_MGE_UPS		0xffff
#define USB_DEVICE_ID_MGE_UPS1		0x0001

#define USB_VENDOR_ID_ONTRAK		0x0a07
#define USB_DEVICE_ID_ONTRAK_ADU100	0x0064

#define USB_VENDOR_ID_ESSENTIAL_REALITY	0x0d7f
#define USB_DEVICE_ID_ESSENTIAL_REALITY_P5 0x0100

#define USB_VENDOR_ID_A4TECH		0x09da
#define USB_DEVICE_ID_A4TECH_WCP32PU	0x0006

#define USB_VENDOR_ID_AASHIMA		0x06d6
#define USB_DEVICE_ID_AASHIMA_GAMEPAD	0x0025
#define USB_DEVICE_ID_AASHIMA_PREDATOR	0x0026

#define USB_VENDOR_ID_CYPRESS		0x04b4
#define USB_DEVICE_ID_CYPRESS_MOUSE	0x0001
#define USB_DEVICE_ID_CYPRESS_HIDCOM	0x5500
#define USB_DEVICE_ID_CYPRESS_ULTRAMOUSE	0x7417

#define USB_VENDOR_ID_BERKSHIRE		0x0c98
#define USB_DEVICE_ID_BERKSHIRE_PCWD	0x1140

#define USB_VENDOR_ID_ALPS		0x0433
#define USB_DEVICE_ID_IBM_GAMEPAD	0x1101

#define USB_VENDOR_ID_SAITEK		0x06a3
#define USB_DEVICE_ID_SAITEK_RUMBLEPAD	0xff17

#define USB_VENDOR_ID_NEC		0x073e
#define USB_DEVICE_ID_NEC_USB_GAME_PAD	0x0301

#define USB_VENDOR_ID_CHIC		0x05fe
#define USB_DEVICE_ID_CHIC_GAMEPAD	0x0014

#define USB_VENDOR_ID_GLAB		0x06c2
#define USB_DEVICE_ID_4_PHIDGETSERVO_30	0x0038
#define USB_DEVICE_ID_1_PHIDGETSERVO_30	0x0039
#define USB_DEVICE_ID_0_0_4_IF_KIT	0x0040
#define USB_DEVICE_ID_0_16_16_IF_KIT	0x0044
#define USB_DEVICE_ID_8_8_8_IF_KIT	0x0045
#define USB_DEVICE_ID_0_8_7_IF_KIT	0x0051
#define USB_DEVICE_ID_0_8_8_IF_KIT	0x0053
#define USB_DEVICE_ID_PHIDGET_MOTORCONTROL	0x0058

#define USB_VENDOR_ID_WISEGROUP		0x0925
#define USB_DEVICE_ID_1_PHIDGETSERVO_20	0x8101
#define USB_DEVICE_ID_4_PHIDGETSERVO_20	0x8104
#define USB_DEVICE_ID_8_8_4_IF_KIT	0x8201
#define USB_DEVICE_ID_DUAL_USB_JOYPAD   0x8866

#define USB_VENDOR_ID_WISEGROUP_LTD	0x6677
#define USB_DEVICE_ID_SMARTJOY_DUAL_PLUS 0x8802

#define USB_VENDOR_ID_CODEMERCS		0x07c0
#define USB_DEVICE_ID_CODEMERCS_IOW40	0x1500
#define USB_DEVICE_ID_CODEMERCS_IOW24	0x1501
#define USB_DEVICE_ID_CODEMERCS_IOW48	0x1502
#define USB_DEVICE_ID_CODEMERCS_IOW28	0x1503

#define USB_VENDOR_ID_DELORME		0x1163
#define USB_DEVICE_ID_DELORME_EARTHMATE 0x0100
#define USB_DEVICE_ID_DELORME_EM_LT20	0x0200

#define USB_VENDOR_ID_MCC		0x09db
#define USB_DEVICE_ID_MCC_PMD1024LS	0x0076
#define USB_DEVICE_ID_MCC_PMD1208LS	0x007a

#define USB_VENDOR_ID_VERNIER		0x08f7
#define USB_DEVICE_ID_VERNIER_LABPRO	0x0001
#define USB_DEVICE_ID_VERNIER_GOTEMP	0x0002
#define USB_DEVICE_ID_VERNIER_SKIP	0x0003
#define USB_DEVICE_ID_VERNIER_CYCLOPS	0x0004

#define USB_VENDOR_ID_LD		0x0f11
#define USB_DEVICE_ID_LD_CASSY		0x1000
#define USB_DEVICE_ID_LD_POCKETCASSY	0x1010
#define USB_DEVICE_ID_LD_MOBILECASSY	0x1020
#define USB_DEVICE_ID_LD_JWM		0x1080
#define USB_DEVICE_ID_LD_DMMP		0x1081
#define USB_DEVICE_ID_LD_UMIP		0x1090
#define USB_DEVICE_ID_LD_XRAY1		0x1100
#define USB_DEVICE_ID_LD_XRAY2		0x1101
#define USB_DEVICE_ID_LD_VIDEOCOM	0x1200
#define USB_DEVICE_ID_LD_COM3LAB	0x2000
#define USB_DEVICE_ID_LD_TELEPORT	0x2010
#define USB_DEVICE_ID_LD_NETWORKANALYSER 0x2020
#define USB_DEVICE_ID_LD_POWERCONTROL	0x2030
#define USB_DEVICE_ID_LD_MACHINETEST	0x2040

#define USB_VENDOR_ID_APPLE		0x05ac
#define USB_DEVICE_ID_APPLE_MIGHTYMOUSE	0x0304
#define USB_DEVICE_ID_APPLE_FOUNTAIN_ANSI	0x020e
#define USB_DEVICE_ID_APPLE_FOUNTAIN_ISO	0x020f
#define USB_DEVICE_ID_APPLE_GEYSER_ANSI	0x0214
#define USB_DEVICE_ID_APPLE_GEYSER_ISO	0x0215
#define USB_DEVICE_ID_APPLE_GEYSER_JIS	0x0216
#define USB_DEVICE_ID_APPLE_GEYSER3_ANSI	0x0217
#define USB_DEVICE_ID_APPLE_GEYSER3_ISO	0x0218
#define USB_DEVICE_ID_APPLE_GEYSER3_JIS	0x0219
#define USB_DEVICE_ID_APPLE_GEYSER4_ANSI	0x021a
#define USB_DEVICE_ID_APPLE_GEYSER4_ISO	0x021b
#define USB_DEVICE_ID_APPLE_GEYSER4_JIS	0x021c
#define USB_DEVICE_ID_APPLE_FOUNTAIN_TP_ONLY	0x030a
#define USB_DEVICE_ID_APPLE_GEYSER1_TP_ONLY	0x030b

#define USB_VENDOR_ID_CHERRY		0x046a
#define USB_DEVICE_ID_CHERRY_CYMOTION	0x0023

#define USB_VENDOR_ID_YEALINK		0x6993
#define USB_DEVICE_ID_YEALINK_P1K_P4K_B2K	0xb001

#define USB_VENDOR_ID_ALCOR		0x058f
#define USB_DEVICE_ID_ALCOR_USBRS232	0x9720

#define USB_VENDOR_ID_SUN		0x0430
#define USB_DEVICE_ID_RARITAN_KVM_DONGLE	0xcdab

#define USB_VENDOR_ID_AIRCABLE		0x16CA
#define USB_DEVICE_ID_AIRCABLE1		0x1502

#define USB_VENDOR_ID_LOGITECH		0x046d
#define USB_DEVICE_ID_LOGITECH_USB_RECEIVER	0xc101

#define USB_VENDOR_ID_IMATION		0x0718
#define USB_DEVICE_ID_DISC_STAKKA	0xd000

/*
 * Alphabetically sorted blacklist by quirk type.
 */

static const struct hid_blacklist {
	__u16 idVendor;
	__u16 idProduct;
	unsigned quirks;
} hid_blacklist[] = {

	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_01, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_10, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_21, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_22, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_23, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_24, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIRCABLE, USB_DEVICE_ID_AIRCABLE1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ALCOR, USB_DEVICE_ID_ALCOR_USBRS232, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_BERKSHIRE, USB_DEVICE_ID_BERKSHIRE_PCWD, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CODEMERCS, USB_DEVICE_ID_CODEMERCS_IOW40, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CODEMERCS, USB_DEVICE_ID_CODEMERCS_IOW24, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CODEMERCS, USB_DEVICE_ID_CODEMERCS_IOW48, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CODEMERCS, USB_DEVICE_ID_CODEMERCS_IOW28, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_HIDCOM, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_ULTRAMOUSE, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_DELORME, USB_DEVICE_ID_DELORME_EARTHMATE, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_DELORME, USB_DEVICE_ID_DELORME_EM_LT20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ESSENTIAL_REALITY, USB_DEVICE_ID_ESSENTIAL_REALITY_P5, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_4_PHIDGETSERVO_30, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_1_PHIDGETSERVO_30, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_0_4_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_16_16_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_8_8_8_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_8_7_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_8_8_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_PHIDGET_MOTORCONTROL, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_POWERMATE, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_SOUNDKNOB, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_90, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_100, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_101, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_103, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_104, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_105, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_106, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_107, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_108, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_200, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_201, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_202, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_203, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_204, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_205, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_206, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_207, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_300, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_301, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_302, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_303, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_304, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_305, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_306, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_307, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_308, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_309, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_400, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_401, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_402, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_403, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_404, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_405, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_500, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_501, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_502, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_503, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_504, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1000, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1001, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1002, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1003, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1004, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1005, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1006, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO_IPANEL_2, USB_DEVICE_ID_GTCO_8, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GTCO_IPANEL_2, USB_DEVICE_ID_GTCO_d, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_IMATION, USB_DEVICE_ID_DISC_STAKKA, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_KBGEAR, USB_DEVICE_ID_KBGEAR_JAMSTUDIO, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_CASSY, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_POCKETCASSY, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOBILECASSY, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_JWM, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_DMMP, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_UMIP, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_XRAY1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_XRAY2, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_VIDEOCOM, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_COM3LAB, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_TELEPORT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_NETWORKANALYSER, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_POWERCONTROL, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MACHINETEST, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MCC, USB_DEVICE_ID_MCC_PMD1024LS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MCC, USB_DEVICE_ID_MCC_PMD1208LS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 30, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 100, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 108, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 118, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 200, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 300, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 400, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 500, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_LABPRO, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_GOTEMP, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_SKIP, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_CYCLOPS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_4_PHIDGETSERVO_20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_1_PHIDGETSERVO_20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_8_8_4_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_YEALINK, USB_DEVICE_ID_YEALINK_P1K_P4K_B2K, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_ACECAD, USB_DEVICE_ID_ACECAD_FLAIR, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ACECAD, USB_DEVICE_ID_ACECAD_302, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_UC100KM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_CS124U, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_2PORTKVM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_4PORTKVM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_4PORTKVMC, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_SUN, USB_DEVICE_ID_RARITAN_KVM_DONGLE, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_DUAL_USB_JOYPAD, HID_QUIRK_NOGET | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_WISEGROUP_LTD, USB_DEVICE_ID_SMARTJOY_DUAL_PLUS, HID_QUIRK_NOGET | HID_QUIRK_MULTI_INPUT },

	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MIGHTYMOUSE, HID_QUIRK_MIGHTYMOUSE | HID_QUIRK_INVERT_HWHEEL },
	{ USB_VENDOR_ID_A4TECH, USB_DEVICE_ID_A4TECH_WCP32PU, HID_QUIRK_2WHEEL_MOUSE_HACK_7 },
	{ USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_MOUSE, HID_QUIRK_2WHEEL_MOUSE_HACK_5 },

	{ USB_VENDOR_ID_AASHIMA, USB_DEVICE_ID_AASHIMA_GAMEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_AASHIMA, USB_DEVICE_ID_AASHIMA_PREDATOR, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_ALPS, USB_DEVICE_ID_IBM_GAMEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_CHIC, USB_DEVICE_ID_CHIC_GAMEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_DRIVING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_FLYING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_FIGHTING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_NEC, USB_DEVICE_ID_NEC_USB_GAME_PAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_SAITEK, USB_DEVICE_ID_SAITEK_RUMBLEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_TOPMAX, USB_DEVICE_ID_TOPMAX_COBRAPAD, HID_QUIRK_BADPAD },

	{ USB_VENDOR_ID_CHERRY, USB_DEVICE_ID_CHERRY_CYMOTION, HID_QUIRK_CYMOTION },

	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ANSI, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ISO, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ANSI, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ISO, HID_QUIRK_POWERBOOK_HAS_FN | HID_QUIRK_POWERBOOK_ISO_KEYBOARD},
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_JIS, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ANSI, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ISO, HID_QUIRK_POWERBOOK_HAS_FN | HID_QUIRK_POWERBOOK_ISO_KEYBOARD},
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_JIS, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ANSI, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ISO, HID_QUIRK_POWERBOOK_HAS_FN | HID_QUIRK_POWERBOOK_ISO_KEYBOARD},
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_JIS, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_TP_ONLY, HID_QUIRK_POWERBOOK_HAS_FN },
	{ USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER1_TP_ONLY, HID_QUIRK_POWERBOOK_HAS_FN },

	{ USB_VENDOR_ID_PANJIT, 0x0001, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_PANJIT, 0x0002, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_PANJIT, 0x0003, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_PANJIT, 0x0004, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_TURBOX, USB_DEVICE_ID_TURBOX_KEYBOARD, HID_QUIRK_NOGET },

	{ USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_USB_RECEIVER, HID_QUIRK_BAD_RELATIVE_KEYS },

	{ 0, 0 }
};

/*
 * Traverse the supplied list of reports and find the longest
 */
static void hid_find_max_report(struct hid_device *hid, unsigned int type, int *max)
{
	struct hid_report *report;
	int size;

	list_for_each_entry(report, &hid->report_enum[type].report_list, list) {
		size = ((report->size - 1) >> 3) + 1;
		if (type == HID_INPUT_REPORT && hid->report_enum[type].numbered)
			size++;
		if (*max < size)
			*max = size;
	}
}

static int hid_alloc_buffers(struct usb_device *dev, struct hid_device *hid)
{
	struct usbhid_device *usbhid = hid->driver_data;

	if (!(usbhid->inbuf = usb_buffer_alloc(dev, usbhid->bufsize, GFP_ATOMIC, &usbhid->inbuf_dma)))
		return -1;
	if (!(usbhid->outbuf = usb_buffer_alloc(dev, usbhid->bufsize, GFP_ATOMIC, &usbhid->outbuf_dma)))
		return -1;
	if (!(usbhid->cr = usb_buffer_alloc(dev, sizeof(*(usbhid->cr)), GFP_ATOMIC, &usbhid->cr_dma)))
		return -1;
	if (!(usbhid->ctrlbuf = usb_buffer_alloc(dev, usbhid->bufsize, GFP_ATOMIC, &usbhid->ctrlbuf_dma)))
		return -1;

	return 0;
}

static void hid_free_buffers(struct usb_device *dev, struct hid_device *hid)
{
	struct usbhid_device *usbhid = hid->driver_data;

	if (usbhid->inbuf)
		usb_buffer_free(dev, usbhid->bufsize, usbhid->inbuf, usbhid->inbuf_dma);
	if (usbhid->outbuf)
		usb_buffer_free(dev, usbhid->bufsize, usbhid->outbuf, usbhid->outbuf_dma);
	if (usbhid->cr)
		usb_buffer_free(dev, sizeof(*(usbhid->cr)), usbhid->cr, usbhid->cr_dma);
	if (usbhid->ctrlbuf)
		usb_buffer_free(dev, usbhid->bufsize, usbhid->ctrlbuf, usbhid->ctrlbuf_dma);
}

/*
 * Cherry Cymotion keyboard have an invalid HID report descriptor,
 * that needs fixing before we can parse it.
 */

static void hid_fixup_cymotion_descriptor(char *rdesc, int rsize)
{
	if (rsize >= 17 && rdesc[11] == 0x3c && rdesc[12] == 0x02) {
		info("Fixing up Cherry Cymotion report descriptor");
		rdesc[11] = rdesc[16] = 0xff;
		rdesc[12] = rdesc[17] = 0x03;
	}
}

static struct hid_device *usb_hid_configure(struct usb_interface *intf)
{
	struct usb_host_interface *interface = intf->cur_altsetting;
	struct usb_device *dev = interface_to_usbdev (intf);
	struct hid_descriptor *hdesc;
	struct hid_device *hid;
	unsigned quirks = 0, rsize = 0;
	char *rdesc;
	int n, len, insize = 0;
	struct usbhid_device *usbhid;

        /* Ignore all Wacom devices */
        if (le16_to_cpu(dev->descriptor.idVendor) == USB_VENDOR_ID_WACOM)
                return NULL;

	for (n = 0; hid_blacklist[n].idVendor; n++)
		if ((hid_blacklist[n].idVendor == le16_to_cpu(dev->descriptor.idVendor)) &&
			(hid_blacklist[n].idProduct == le16_to_cpu(dev->descriptor.idProduct)))
				quirks = hid_blacklist[n].quirks;

	/* Many keyboards and mice don't like to be polled for reports,
	 * so we will always set the HID_QUIRK_NOGET flag for them. */
	if (interface->desc.bInterfaceSubClass == USB_INTERFACE_SUBCLASS_BOOT) {
		if (interface->desc.bInterfaceProtocol == USB_INTERFACE_PROTOCOL_KEYBOARD ||
			interface->desc.bInterfaceProtocol == USB_INTERFACE_PROTOCOL_MOUSE)
				quirks |= HID_QUIRK_NOGET;
	}

	if (quirks & HID_QUIRK_IGNORE)
		return NULL;

	if (usb_get_extra_descriptor(interface, HID_DT_HID, &hdesc) &&
	    (!interface->desc.bNumEndpoints ||
	     usb_get_extra_descriptor(&interface->endpoint[0], HID_DT_HID, &hdesc))) {
		dbg("class descriptor not present\n");
		return NULL;
	}

	for (n = 0; n < hdesc->bNumDescriptors; n++)
		if (hdesc->desc[n].bDescriptorType == HID_DT_REPORT)
			rsize = le16_to_cpu(hdesc->desc[n].wDescriptorLength);

	if (!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE) {
		dbg("weird size of report descriptor (%u)", rsize);
		return NULL;
	}

	if (!(rdesc = kmalloc(rsize, GFP_KERNEL))) {
		dbg("couldn't allocate rdesc memory");
		return NULL;
	}

	hid_set_idle(dev, interface->desc.bInterfaceNumber, 0, 0);

	if ((n = hid_get_class_descriptor(dev, interface->desc.bInterfaceNumber, HID_DT_REPORT, rdesc, rsize)) < 0) {
		dbg("reading report descriptor failed");
		kfree(rdesc);
		return NULL;
	}

	if ((quirks & HID_QUIRK_CYMOTION))
		hid_fixup_cymotion_descriptor(rdesc, rsize);

#ifdef DEBUG_DATA
	printk(KERN_DEBUG __FILE__ ": report descriptor (size %u, read %d) = ", rsize, n);
	for (n = 0; n < rsize; n++)
		printk(" %02x", (unsigned char) rdesc[n]);
	printk("\n");
#endif

	if (!(hid = hid_parse_report(rdesc, n))) {
		dbg("parsing report descriptor failed");
		kfree(rdesc);
		return NULL;
	}

	kfree(rdesc);
	hid->quirks = quirks;

	if (!(usbhid = kzalloc(sizeof(struct usbhid_device), GFP_KERNEL)))
		goto fail;

	hid->driver_data = usbhid;
	usbhid->hid = hid;

	usbhid->bufsize = HID_MIN_BUFFER_SIZE;
	hid_find_max_report(hid, HID_INPUT_REPORT, &usbhid->bufsize);
	hid_find_max_report(hid, HID_OUTPUT_REPORT, &usbhid->bufsize);
	hid_find_max_report(hid, HID_FEATURE_REPORT, &usbhid->bufsize);

	if (usbhid->bufsize > HID_MAX_BUFFER_SIZE)
		usbhid->bufsize = HID_MAX_BUFFER_SIZE;

	hid_find_max_report(hid, HID_INPUT_REPORT, &insize);

	if (insize > HID_MAX_BUFFER_SIZE)
		insize = HID_MAX_BUFFER_SIZE;

	if (hid_alloc_buffers(dev, hid)) {
		hid_free_buffers(dev, hid);
		goto fail;
	}

	for (n = 0; n < interface->desc.bNumEndpoints; n++) {

		struct usb_endpoint_descriptor *endpoint;
		int pipe;
		int interval;

		endpoint = &interface->endpoint[n].desc;
		if ((endpoint->bmAttributes & 3) != 3)		/* Not an interrupt endpoint */
			continue;

		interval = endpoint->bInterval;

		/* Change the polling interval of mice. */
		if (hid->collection->usage == HID_GD_MOUSE && hid_mousepoll_interval > 0)
			interval = hid_mousepoll_interval;

		if (usb_endpoint_dir_in(endpoint)) {
			if (usbhid->urbin)
				continue;
			if (!(usbhid->urbin = usb_alloc_urb(0, GFP_KERNEL)))
				goto fail;
			pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
			usb_fill_int_urb(usbhid->urbin, dev, pipe, usbhid->inbuf, insize,
					 hid_irq_in, hid, interval);
			usbhid->urbin->transfer_dma = usbhid->inbuf_dma;
			usbhid->urbin->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		} else {
			if (usbhid->urbout)
				continue;
			if (!(usbhid->urbout = usb_alloc_urb(0, GFP_KERNEL)))
				goto fail;
			pipe = usb_sndintpipe(dev, endpoint->bEndpointAddress);
			usb_fill_int_urb(usbhid->urbout, dev, pipe, usbhid->outbuf, 0,
					 hid_irq_out, hid, interval);
			usbhid->urbout->transfer_dma = usbhid->outbuf_dma;
			usbhid->urbout->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		}
	}

	if (!usbhid->urbin) {
		err("couldn't find an input interrupt endpoint");
		goto fail;
	}

	init_waitqueue_head(&hid->wait);

	INIT_WORK(&usbhid->reset_work, hid_reset);
	setup_timer(&usbhid->io_retry, hid_retry_timeout, (unsigned long) hid);

	spin_lock_init(&usbhid->inlock);
	spin_lock_init(&usbhid->outlock);
	spin_lock_init(&usbhid->ctrllock);

	hid->version = le16_to_cpu(hdesc->bcdHID);
	hid->country = hdesc->bCountryCode;
	hid->dev = &intf->dev;
	usbhid->intf = intf;
	usbhid->ifnum = interface->desc.bInterfaceNumber;

	hid->name[0] = 0;

	if (dev->manufacturer)
		strlcpy(hid->name, dev->manufacturer, sizeof(hid->name));

	if (dev->product) {
		if (dev->manufacturer)
			strlcat(hid->name, " ", sizeof(hid->name));
		strlcat(hid->name, dev->product, sizeof(hid->name));
	}

	if (!strlen(hid->name))
		snprintf(hid->name, sizeof(hid->name), "HID %04x:%04x",
			 le16_to_cpu(dev->descriptor.idVendor),
			 le16_to_cpu(dev->descriptor.idProduct));

	hid->bus = BUS_USB;
	hid->vendor = le16_to_cpu(dev->descriptor.idVendor);
	hid->product = le16_to_cpu(dev->descriptor.idProduct);

	usb_make_path(dev, hid->phys, sizeof(hid->phys));
	strlcat(hid->phys, "/input", sizeof(hid->phys));
	len = strlen(hid->phys);
	if (len < sizeof(hid->phys) - 1)
		snprintf(hid->phys + len, sizeof(hid->phys) - len,
			 "%d", intf->altsetting[0].desc.bInterfaceNumber);

	if (usb_string(dev, dev->descriptor.iSerialNumber, hid->uniq, 64) <= 0)
		hid->uniq[0] = 0;

	usbhid->urbctrl = usb_alloc_urb(0, GFP_KERNEL);
	if (!usbhid->urbctrl)
		goto fail;

	usb_fill_control_urb(usbhid->urbctrl, dev, 0, (void *) usbhid->cr,
			     usbhid->ctrlbuf, 1, hid_ctrl, hid);
	usbhid->urbctrl->setup_dma = usbhid->cr_dma;
	usbhid->urbctrl->transfer_dma = usbhid->ctrlbuf_dma;
	usbhid->urbctrl->transfer_flags |= (URB_NO_TRANSFER_DMA_MAP | URB_NO_SETUP_DMA_MAP);
	hid->hidinput_input_event = usb_hidinput_input_event;
	hid->hidinput_open = hidinput_open;
	hid->hidinput_close = hidinput_close;
#ifdef CONFIG_USB_HIDDEV
	hid->hiddev_hid_event = hiddev_hid_event;
	hid->hiddev_report_event = hiddev_report_event;
#endif
	return hid;

fail:
	usb_free_urb(usbhid->urbin);
	usb_free_urb(usbhid->urbout);
	usb_free_urb(usbhid->urbctrl);
	hid_free_buffers(dev, hid);
	hid_free_device(hid);

	return NULL;
}

static void hid_disconnect(struct usb_interface *intf)
{
	struct hid_device *hid = usb_get_intfdata (intf);
	struct usbhid_device *usbhid;

	if (!hid)
		return;

	usbhid = hid->driver_data;

	spin_lock_irq(&usbhid->inlock);	/* Sync with error handler */
	usb_set_intfdata(intf, NULL);
	spin_unlock_irq(&usbhid->inlock);
	usb_kill_urb(usbhid->urbin);
	usb_kill_urb(usbhid->urbout);
	usb_kill_urb(usbhid->urbctrl);

	del_timer_sync(&usbhid->io_retry);
	flush_scheduled_work();

	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_disconnect(hid);
	if (hid->claimed & HID_CLAIMED_HIDDEV)
		hiddev_disconnect(hid);

	usb_free_urb(usbhid->urbin);
	usb_free_urb(usbhid->urbctrl);
	usb_free_urb(usbhid->urbout);

	hid_free_buffers(hid_to_usb_dev(hid), hid);
	hid_free_device(hid);
}

static int hid_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct hid_device *hid;
	char path[64];
	int i;
	char *c;

	dbg("HID probe called for ifnum %d",
			intf->altsetting->desc.bInterfaceNumber);

	if (!(hid = usb_hid_configure(intf)))
		return -ENODEV;

	usbhid_init_reports(hid);
	hid_dump_device(hid);

	if (!hidinput_connect(hid))
		hid->claimed |= HID_CLAIMED_INPUT;
	if (!hiddev_connect(hid))
		hid->claimed |= HID_CLAIMED_HIDDEV;

	usb_set_intfdata(intf, hid);

	if (!hid->claimed) {
		printk ("HID device not claimed by input or hiddev\n");
		hid_disconnect(intf);
		return -ENODEV;
	}

	/* This only gets called when we are a single-input (most of the
	 * time). IOW, not a HID_QUIRK_MULTI_INPUT. The hid_ff_init() is
	 * only useful in this case, and not for multi-input quirks. */
	if ((hid->claimed & HID_CLAIMED_INPUT) &&
			!(hid->quirks & HID_QUIRK_MULTI_INPUT))
		hid_ff_init(hid);

	printk(KERN_INFO);

	if (hid->claimed & HID_CLAIMED_INPUT)
		printk("input");
	if (hid->claimed == (HID_CLAIMED_INPUT | HID_CLAIMED_HIDDEV))
		printk(",");
	if (hid->claimed & HID_CLAIMED_HIDDEV)
		printk("hiddev%d", hid->minor);

	c = "Device";
	for (i = 0; i < hid->maxcollection; i++) {
		if (hid->collection[i].type == HID_COLLECTION_APPLICATION &&
		    (hid->collection[i].usage & HID_USAGE_PAGE) == HID_UP_GENDESK &&
		    (hid->collection[i].usage & 0xffff) < ARRAY_SIZE(hid_types)) {
			c = hid_types[hid->collection[i].usage & 0xffff];
			break;
		}
	}

	usb_make_path(interface_to_usbdev(intf), path, 63);

	printk(": USB HID v%x.%02x %s [%s] on %s\n",
		hid->version >> 8, hid->version & 0xff, c, hid->name, path);

	return 0;
}

static int hid_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct hid_device *hid = usb_get_intfdata (intf);
	struct usbhid_device *usbhid = hid->driver_data;

	spin_lock_irq(&usbhid->inlock);	/* Sync with error handler */
	set_bit(HID_SUSPENDED, &usbhid->iofl);
	spin_unlock_irq(&usbhid->inlock);
	del_timer(&usbhid->io_retry);
	usb_kill_urb(usbhid->urbin);
	dev_dbg(&intf->dev, "suspend\n");
	return 0;
}

static int hid_resume(struct usb_interface *intf)
{
	struct hid_device *hid = usb_get_intfdata (intf);
	struct usbhid_device *usbhid = hid->driver_data;
	int status;

	clear_bit(HID_SUSPENDED, &usbhid->iofl);
	usbhid->retry_delay = 0;
	status = hid_start_in(hid);
	dev_dbg(&intf->dev, "resume status %d\n", status);
	return status;
}

/* Treat USB reset pretty much the same as suspend/resume */
static void hid_pre_reset(struct usb_interface *intf)
{
	/* FIXME: What if the interface is already suspended? */
	hid_suspend(intf, PMSG_ON);
}

static void hid_post_reset(struct usb_interface *intf)
{
	struct usb_device *dev = interface_to_usbdev (intf);

	hid_set_idle(dev, intf->cur_altsetting->desc.bInterfaceNumber, 0, 0);
	/* FIXME: Any more reinitialization needed? */

	hid_resume(intf);
}

static struct usb_device_id hid_usb_ids [] = {
	{ .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS,
		.bInterfaceClass = USB_INTERFACE_CLASS_HID },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, hid_usb_ids);

static struct usb_driver hid_driver = {
	.name =		"usbhid",
	.probe =	hid_probe,
	.disconnect =	hid_disconnect,
	.suspend =	hid_suspend,
	.resume =	hid_resume,
	.pre_reset =	hid_pre_reset,
	.post_reset =	hid_post_reset,
	.id_table =	hid_usb_ids,
};

static int __init hid_init(void)
{
	int retval;
	retval = hiddev_init();
	if (retval)
		goto hiddev_init_fail;
	retval = usb_register(&hid_driver);
	if (retval)
		goto usb_register_fail;
	info(DRIVER_VERSION ":" DRIVER_DESC);

	return 0;
usb_register_fail:
	hiddev_exit();
hiddev_init_fail:
	return retval;
}

static void __exit hid_exit(void)
{
	usb_deregister(&hid_driver);
	hiddev_exit();
}

module_init(hid_init);
module_exit(hid_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);
