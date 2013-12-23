/*
  USB Driver for Sierra Wireless

  Copyright (C) 2006, 2007, 2008  Kevin Lloyd <klloyd@sierrawireless.com>

  IMPORTANT DISCLAIMER: This driver is not commercially supported by
  Sierra Wireless. Use at your own risk.

  This driver is free software; you can redistribute it and/or modify
  it under the terms of Version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  Portions based on the option driver by Matthias Urlichs <smurf@smurf.noris.de>
  Whom based his on the Keyspan driver by Hugh Blemings <hugh@blemings.org>
*/

#define DRIVER_VERSION "v.1.3.3"
#define DRIVER_AUTHOR "Kevin Lloyd <klloyd@sierrawireless.com>"
#define DRIVER_DESC "USB Driver for Sierra Wireless USB modems"

#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/usb/ch9.h>

#define SWIMS_USB_REQUEST_SetPower	0x00
#define SWIMS_USB_REQUEST_SetNmea	0x07

/* per port private data */
#define N_IN_URB	4
#define N_OUT_URB	4
#define IN_BUFLEN	4096

static int debug;
static int nmea;

static int sierra_set_power_state(struct usb_device *udev, __u16 swiState)
{
	int result;
	dev_dbg(&udev->dev, "%s", __func__);
	result = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			SWIMS_USB_REQUEST_SetPower,	/* __u8 request      */
			USB_TYPE_VENDOR,		/* __u8 request type */
			swiState,			/* __u16 value       */
			0,				/* __u16 index       */
			NULL,				/* void *data        */
			0,				/* __u16 size 	     */
			USB_CTRL_SET_TIMEOUT);		/* int timeout 	     */
	return result;
}

static int sierra_vsc_set_nmea(struct usb_device *udev, __u16 enable)
{
	int result;
	dev_dbg(&udev->dev, "%s", __func__);
	result = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			SWIMS_USB_REQUEST_SetNmea,	/* __u8 request      */
			USB_TYPE_VENDOR,		/* __u8 request type */
			enable,				/* __u16 value       */
			0x0000,				/* __u16 index       */
			NULL,				/* void *data        */
			0,				/* __u16 size 	     */
			USB_CTRL_SET_TIMEOUT);		/* int timeout       */
	return result;
}

static int sierra_calc_num_ports(struct usb_serial *serial)
{
	int result;
	int *num_ports = usb_get_serial_data(serial);
	dev_dbg(&serial->dev->dev, "%s", __func__);

	result = *num_ports;

	if (result) {
		kfree(num_ports);
		usb_set_serial_data(serial, NULL);
	}

	return result;
}

static int sierra_calc_interface(struct usb_serial *serial)
{
	int interface;
	struct usb_interface *p_interface;
	struct usb_host_interface *p_host_interface;
	dev_dbg(&serial->dev->dev, "%s", __func__);

	/* Get the interface structure pointer from the serial struct */
	p_interface = serial->interface;

	/* Get a pointer to the host interface structure */
	p_host_interface = p_interface->cur_altsetting;

	/* read the interface descriptor for this active altsetting
	 * to find out the interface number we are on
	*/
	interface = p_host_interface->desc.bInterfaceNumber;

	return interface;
}

static int sierra_probe(struct usb_serial *serial,
			const struct usb_device_id *id)
{
	int result = 0;
	struct usb_device *udev;
	int *num_ports;
	u8 ifnum;
	u8 numendpoints;

	dev_dbg(&serial->dev->dev, "%s", __func__);

	num_ports = kmalloc(sizeof(*num_ports), GFP_KERNEL);
	if (!num_ports)
		return -ENOMEM;

	ifnum = serial->interface->cur_altsetting->desc.bInterfaceNumber;
	numendpoints = serial->interface->cur_altsetting->desc.bNumEndpoints;
	udev = serial->dev;

	/* Figure out the interface number from the serial structure */
	ifnum = sierra_calc_interface(serial);

	/*
	 * If this interface supports more than 1 alternate
	 * select the 2nd one
	 */
	if (serial->interface->num_altsetting == 2) {
		dev_dbg(&udev->dev, "Selecting alt setting for interface %d\n",
			ifnum);
		/* We know the alternate setting is 1 for the MC8785 */
		usb_set_interface(udev, ifnum, 1);
	}

	/* Dummy interface present on some SKUs should be ignored */
	if (ifnum == 0x99)
		*num_ports = 0;
	else if (numendpoints <= 3)
		*num_ports = 1;
	else
		*num_ports = (numendpoints-1)/2;

	/*
	 * save off our num_ports info so that we can use it in the
	 * calc_num_ports callback
	 */
	usb_set_serial_data(serial, (void *)num_ports);

	return result;
}

static struct usb_device_id id_table [] = {
	{ USB_DEVICE(0x1199, 0x0017) },	/* Sierra Wireless EM5625 */
	{ USB_DEVICE(0x1199, 0x0018) },	/* Sierra Wireless MC5720 */
	{ USB_DEVICE(0x1199, 0x0218) },	/* Sierra Wireless MC5720 */
	{ USB_DEVICE(0x03f0, 0x1b1d) }, /* HP ev2200 a.k.a MC5720 */
	{ USB_DEVICE(0x1199, 0x0020) },	/* Sierra Wireless MC5725 */
	{ USB_DEVICE(0x1199, 0x0024) },	/* Sierra Wireless MC5727 */
	{ USB_DEVICE(0x1199, 0x0220) },	/* Sierra Wireless MC5725 */
	{ USB_DEVICE(0x1199, 0x0019) },	/* Sierra Wireless AirCard 595 */
	{ USB_DEVICE(0x1199, 0x0021) },	/* Sierra Wireless AirCard 597E */
	{ USB_DEVICE(0x1199, 0x0120) },	/* Sierra Wireless USB Dongle 595U */
	 /* Sierra Wireless C597 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x0023, 0xFF, 0xFF, 0xFF) },
	 /* Sierra Wireless Device */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x0025, 0xFF, 0xFF, 0xFF) },
	{ USB_DEVICE(0x1199, 0x0026) }, /* Sierra Wireless Device */
	{ USB_DEVICE(0x1199, 0x0027) }, /* Sierra Wireless Device */
	{ USB_DEVICE(0x1199, 0x0028) }, /* Sierra Wireless Device */

	{ USB_DEVICE(0x1199, 0x6802) },	/* Sierra Wireless MC8755 */
	{ USB_DEVICE(0x1199, 0x6804) },	/* Sierra Wireless MC8755 */
	{ USB_DEVICE(0x1199, 0x6803) },	/* Sierra Wireless MC8765 */
	{ USB_DEVICE(0x1199, 0x6812) },	/* Sierra Wireless MC8775 & AC 875U */
	{ USB_DEVICE(0x1199, 0x6813) },	/* Sierra Wireless MC8775 (Lenovo) */
	{ USB_DEVICE(0x1199, 0x6815) },	/* Sierra Wireless MC8775 */
	{ USB_DEVICE(0x03f0, 0x1e1d) },	/* HP hs2300 a.k.a MC8775 */
	{ USB_DEVICE(0x1199, 0x6820) },	/* Sierra Wireless AirCard 875 */
	{ USB_DEVICE(0x1199, 0x6821) },	/* Sierra Wireless AirCard 875U */
	{ USB_DEVICE(0x1199, 0x6832) },	/* Sierra Wireless MC8780 */
	{ USB_DEVICE(0x1199, 0x6833) },	/* Sierra Wireless MC8781 */
	{ USB_DEVICE(0x1199, 0x683A) },	/* Sierra Wireless MC8785 */
	{ USB_DEVICE(0x1199, 0x683B) },	/* Sierra Wireless MC8785 Composite */
	{ USB_DEVICE(0x1199, 0x683C) },	/* Sierra Wireless MC8790 */
	{ USB_DEVICE(0x1199, 0x683D) },	/* Sierra Wireless MC8790 */
	{ USB_DEVICE(0x1199, 0x683E) },	/* Sierra Wireless MC8790 */
	{ USB_DEVICE(0x1199, 0x6850) },	/* Sierra Wireless AirCard 880 */
	{ USB_DEVICE(0x1199, 0x6851) },	/* Sierra Wireless AirCard 881 */
	{ USB_DEVICE(0x1199, 0x6852) },	/* Sierra Wireless AirCard 880 E */
	{ USB_DEVICE(0x1199, 0x6853) },	/* Sierra Wireless AirCard 881 E */
	{ USB_DEVICE(0x1199, 0x6855) },	/* Sierra Wireless AirCard 880 U */
	{ USB_DEVICE(0x1199, 0x6856) },	/* Sierra Wireless AirCard 881 U */
	{ USB_DEVICE(0x1199, 0x6859) },	/* Sierra Wireless AirCard 885 E */
	{ USB_DEVICE(0x1199, 0x685A) },	/* Sierra Wireless AirCard 885 E */
	/* Sierra Wireless C885 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x6880, 0xFF, 0xFF, 0xFF)},
	/* Sierra Wireless Device */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x6890, 0xFF, 0xFF, 0xFF)},
	/* Sierra Wireless Device */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x6891, 0xFF, 0xFF, 0xFF)},
	/* Sierra Wireless Device */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x1199, 0x6892, 0xFF, 0xFF, 0xFF)},

	{ USB_DEVICE(0x1199, 0x0112) }, /* Sierra Wireless AirCard 580 */
	{ USB_DEVICE(0x0F3D, 0x0112) }, /* Airprime/Sierra PC 5220 */

	{ }
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver sierra_driver = {
	.name       = "sierra",
	.probe      = usb_serial_probe,
	.disconnect = usb_serial_disconnect,
	.id_table   = id_table,
	.no_dynamic_id = 	1,
};

struct sierra_port_private {
	spinlock_t lock;	/* lock the structure */
	int outstanding_urbs;	/* number of out urbs in flight */

	/* Input endpoints and buffers for this port */
	struct urb *in_urbs[N_IN_URB];
	char *in_buffer[N_IN_URB];

	/* Settings for the port */
	int rts_state;	/* Handshaking pins (outputs) */
	int dtr_state;
	int cts_state;	/* Handshaking pins (inputs) */
	int dsr_state;
	int dcd_state;
	int ri_state;
};

static int sierra_send_setup(struct tty_struct *tty,
						struct usb_serial_port *port)
{
	struct usb_serial *serial = port->serial;
	struct sierra_port_private *portdata;
	__u16 interface = 0;

	dev_dbg(&port->dev, "%s", __func__);

	portdata = usb_get_serial_port_data(port);

	if (tty) {
		int val = 0;
		if (portdata->dtr_state)
			val |= 0x01;
		if (portdata->rts_state)
			val |= 0x02;

		/* If composite device then properly report interface */
		if (serial->num_ports == 1) {
			interface = sierra_calc_interface(serial);

			/* Control message is sent only to interfaces with
			 * interrupt_in endpoints
			 */
			if (port->interrupt_in_urb) {
				/* send control message */
				return usb_control_msg(serial->dev,
					usb_rcvctrlpipe(serial->dev, 0),
					0x22, 0x21, val, interface,
					NULL, 0, USB_CTRL_SET_TIMEOUT);
			}
		}

		/* Otherwise the need to do non-composite mapping */
		else {
			if (port->bulk_out_endpointAddress == 2)
				interface = 0;
			else if (port->bulk_out_endpointAddress == 4)
				interface = 1;
			else if (port->bulk_out_endpointAddress == 5)
				interface = 2;

			return usb_control_msg(serial->dev,
				usb_rcvctrlpipe(serial->dev, 0),
				0x22, 0x21, val, interface,
				NULL, 0, USB_CTRL_SET_TIMEOUT);

		}
	}

	return 0;
}

static void sierra_set_termios(struct tty_struct *tty,
		struct usb_serial_port *port, struct ktermios *old_termios)
{
	dev_dbg(&port->dev, "%s", __func__);
	tty_termios_copy_hw(tty->termios, old_termios);
	sierra_send_setup(tty, port);
}

static int sierra_tiocmget(struct tty_struct *tty, struct file *file)
{
	struct usb_serial_port *port = tty->driver_data;
	unsigned int value;
	struct sierra_port_private *portdata;

	dev_dbg(&port->dev, "%s", __func__);
	portdata = usb_get_serial_port_data(port);

	value = ((portdata->rts_state) ? TIOCM_RTS : 0) |
		((portdata->dtr_state) ? TIOCM_DTR : 0) |
		((portdata->cts_state) ? TIOCM_CTS : 0) |
		((portdata->dsr_state) ? TIOCM_DSR : 0) |
		((portdata->dcd_state) ? TIOCM_CAR : 0) |
		((portdata->ri_state) ? TIOCM_RNG : 0);

	return value;
}

static int sierra_tiocmset(struct tty_struct *tty, struct file *file,
			unsigned int set, unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;
	struct sierra_port_private *portdata;

	portdata = usb_get_serial_port_data(port);

	if (set & TIOCM_RTS)
		portdata->rts_state = 1;
	if (set & TIOCM_DTR)
		portdata->dtr_state = 1;

	if (clear & TIOCM_RTS)
		portdata->rts_state = 0;
	if (clear & TIOCM_DTR)
		portdata->dtr_state = 0;
	return sierra_send_setup(tty, port);
}

static void sierra_outdat_callback(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	struct sierra_port_private *portdata = usb_get_serial_port_data(port);
	int status = urb->status;
	unsigned long flags;

	dev_dbg(&port->dev, "%s - port %d", __func__, port->number);

	/* free up the transfer buffer, as usb_free_urb() does not do this */
	kfree(urb->transfer_buffer);

	if (status)
		dev_dbg(&port->dev, "%s - nonzero write bulk status "
		    "received: %d", __func__, status);

	spin_lock_irqsave(&portdata->lock, flags);
	--portdata->outstanding_urbs;
	spin_unlock_irqrestore(&portdata->lock, flags);

	usb_serial_port_softint(port);
}

/* Write */
static int sierra_write(struct tty_struct *tty, struct usb_serial_port *port,
					const unsigned char *buf, int count)
{
	struct sierra_port_private *portdata = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	unsigned long flags;
	unsigned char *buffer;
	struct urb *urb;
	int status;

	portdata = usb_get_serial_port_data(port);

	dev_dbg(&port->dev, "%s: write (%d chars)", __func__, count);

	spin_lock_irqsave(&portdata->lock, flags);
	if (portdata->outstanding_urbs > N_OUT_URB) {
		spin_unlock_irqrestore(&portdata->lock, flags);
		dev_dbg(&port->dev, "%s - write limit hit\n", __func__);
		return 0;
	}
	portdata->outstanding_urbs++;
	spin_unlock_irqrestore(&portdata->lock, flags);

	buffer = kmalloc(count, GFP_ATOMIC);
	if (!buffer) {
		dev_err(&port->dev, "out of memory\n");
		count = -ENOMEM;
		goto error_no_buffer;
	}

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		dev_err(&port->dev, "no more free urbs\n");
		count = -ENOMEM;
		goto error_no_urb;
	}

	memcpy(buffer, buf, count);

	usb_serial_debug_data(debug, &port->dev, __func__, count, buffer);

	usb_fill_bulk_urb(urb, serial->dev,
			  usb_sndbulkpipe(serial->dev,
					  port->bulk_out_endpointAddress),
			  buffer, count, sierra_outdat_callback, port);

	/* send it down the pipe */
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status) {
		dev_err(&port->dev, "%s - usb_submit_urb(write bulk) failed "
			"with status = %d\n", __func__, status);
		count = status;
		goto error;
	}

	/* we are done with this urb, so let the host driver
	 * really free it when it is finished with it */
	usb_free_urb(urb);

	return count;
error:
	usb_free_urb(urb);
error_no_urb:
	kfree(buffer);
error_no_buffer:
	spin_lock_irqsave(&portdata->lock, flags);
	--portdata->outstanding_urbs;
	spin_unlock_irqrestore(&portdata->lock, flags);
	return count;
}

static void sierra_indat_callback(struct urb *urb)
{
	int err;
	int endpoint;
	struct usb_serial_port *port;
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	int status = urb->status;

	dbg("%s: %p", __func__, urb);

	endpoint = usb_pipeendpoint(urb->pipe);
	port =  urb->context;

	if (status) {
		dev_dbg(&port->dev, "%s: nonzero status: %d on"
		    " endpoint %02x.", __func__, status, endpoint);
	} else {
		if (urb->actual_length) {
			tty = tty_port_tty_get(&port->port);
			tty_buffer_request_room(tty, urb->actual_length);
			tty_insert_flip_string(tty, data, urb->actual_length);
			tty_flip_buffer_push(tty);
			tty_kref_put(tty);
		} else
			dev_dbg(&port->dev, "%s: empty read urb"
				" received", __func__);

		/* Resubmit urb so we continue receiving */
		if (port->port.count && status != -ESHUTDOWN) {
			err = usb_submit_urb(urb, GFP_ATOMIC);
			if (err)
				dev_err(&port->dev, "resubmit read urb failed."
					"(%d)\n", err);
		}
	}
	return;
}

static void sierra_instat_callback(struct urb *urb)
{
	int err;
	int status = urb->status;
	struct usb_serial_port *port =  urb->context;
	struct sierra_port_private *portdata = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;

	dev_dbg(&port->dev, "%s", __func__);
	dev_dbg(&port->dev, "%s: urb %p port %p has data %p", __func__,
		urb, port, portdata);

	if (status == 0) {
		struct usb_ctrlrequest *req_pkt =
				(struct usb_ctrlrequest *)urb->transfer_buffer;

		if (!req_pkt) {
			dev_dbg(&port->dev, "%s: NULL req_pkt\n",
				__func__);
			return;
		}
		if ((req_pkt->bRequestType == 0xA1) &&
				(req_pkt->bRequest == 0x20)) {
			int old_dcd_state;
			unsigned char signals = *((unsigned char *)
					urb->transfer_buffer +
					sizeof(struct usb_ctrlrequest));
			struct tty_struct *tty;

			dev_dbg(&port->dev, "%s: signal x%x", __func__,
				signals);

			old_dcd_state = portdata->dcd_state;
			portdata->cts_state = 1;
			portdata->dcd_state = ((signals & 0x01) ? 1 : 0);
			portdata->dsr_state = ((signals & 0x02) ? 1 : 0);
			portdata->ri_state = ((signals & 0x08) ? 1 : 0);

			tty = tty_port_tty_get(&port->port);
			if (tty && !C_CLOCAL(tty) &&
					old_dcd_state && !portdata->dcd_state)
				tty_hangup(tty);
			tty_kref_put(tty);
		} else {
			dev_dbg(&port->dev, "%s: type %x req %x",
				__func__, req_pkt->bRequestType,
				req_pkt->bRequest);
		}
	} else
		dev_dbg(&port->dev, "%s: error %d", __func__, status);

	/* Resubmit urb so we continue receiving IRQ data */
	if (status != -ESHUTDOWN) {
		urb->dev = serial->dev;
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err)
			dev_dbg(&port->dev, "%s: resubmit intr urb "
				"failed. (%d)",	__func__, err);
	}
}

static int sierra_write_room(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct sierra_port_private *portdata = usb_get_serial_port_data(port);
	unsigned long flags;

	dev_dbg(&port->dev, "%s - port %d", __func__, port->number);

	/* try to give a good number back based on if we have any free urbs at
	 * this point in time */
	spin_lock_irqsave(&portdata->lock, flags);
	if (portdata->outstanding_urbs > N_OUT_URB * 2 / 3) {
		spin_unlock_irqrestore(&portdata->lock, flags);
		dev_dbg(&port->dev, "%s - write limit hit\n", __func__);
		return 0;
	}
	spin_unlock_irqrestore(&portdata->lock, flags);

	return 2048;
}

static int sierra_open(struct tty_struct *tty,
			struct usb_serial_port *port, struct file *filp)
{
	struct sierra_port_private *portdata;
	struct usb_serial *serial = port->serial;
	int i;
	struct urb *urb;
	int result;

	portdata = usb_get_serial_port_data(port);

	dev_dbg(&port->dev, "%s", __func__);

	/* Set some sane defaults */
	portdata->rts_state = 1;
	portdata->dtr_state = 1;

	/* Reset low level data toggle and start reading from endpoints */
	for (i = 0; i < N_IN_URB; i++) {
		urb = portdata->in_urbs[i];
		if (!urb)
			continue;
		if (urb->dev != serial->dev) {
			dev_dbg(&port->dev, "%s: dev %p != %p",
				 __func__, urb->dev, serial->dev);
			continue;
		}

		/*
		 * make sure endpoint data toggle is synchronized with the
		 * device
		 */
		usb_clear_halt(urb->dev, urb->pipe);

		result = usb_submit_urb(urb, GFP_KERNEL);
		if (result) {
			dev_err(&port->dev, "submit urb %d failed (%d) %d\n",
				i, result, urb->transfer_buffer_length);
		}
	}

	sierra_send_setup(tty, port);

	/* start up the interrupt endpoint if we have one */
	if (port->interrupt_in_urb) {
		result = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
		if (result)
			dev_err(&port->dev, "submit irq_in urb failed %d\n",
				result);
	}
	return 0;
}

static void sierra_close(struct tty_struct *tty,
			struct usb_serial_port *port, struct file *filp)
{
	int i;
	struct usb_serial *serial = port->serial;
	struct sierra_port_private *portdata;

	dev_dbg(&port->dev, "%s", __func__);
	portdata = usb_get_serial_port_data(port);

	portdata->rts_state = 0;
	portdata->dtr_state = 0;

	if (serial->dev) {
		mutex_lock(&serial->disc_mutex);
		if (!serial->disconnected)
			sierra_send_setup(tty, port);
		mutex_unlock(&serial->disc_mutex);

		/* Stop reading/writing urbs */
		for (i = 0; i < N_IN_URB; i++)
			usb_kill_urb(portdata->in_urbs[i]);
	}

	usb_kill_urb(port->interrupt_in_urb);
	tty_port_tty_set(&port->port, NULL);
}

static int sierra_startup(struct usb_serial *serial)
{
	struct usb_serial_port *port;
	struct sierra_port_private *portdata;
	struct urb *urb;
	int i;
	int j;

	dev_dbg(&serial->dev->dev, "%s", __func__);

	/* Set Device mode to D0 */
	sierra_set_power_state(serial->dev, 0x0000);

	/* Check NMEA and set */
	if (nmea)
		sierra_vsc_set_nmea(serial->dev, 1);

	/* Now setup per port private data */
	for (i = 0; i < serial->num_ports; i++) {
		port = serial->port[i];
		portdata = kzalloc(sizeof(*portdata), GFP_KERNEL);
		if (!portdata) {
			dev_dbg(&port->dev, "%s: kmalloc for "
				"sierra_port_private (%d) failed!.",
				__func__, i);
			return -ENOMEM;
		}
		spin_lock_init(&portdata->lock);
		for (j = 0; j < N_IN_URB; j++) {
			portdata->in_buffer[j] = kmalloc(IN_BUFLEN, GFP_KERNEL);
			if (!portdata->in_buffer[j]) {
				for (--j; j >= 0; j--)
					kfree(portdata->in_buffer[j]);
				kfree(portdata);
				return -ENOMEM;
			}
		}

		usb_set_serial_port_data(port, portdata);

		/* initialize the in urbs */
		for (j = 0; j < N_IN_URB; ++j) {
			urb = usb_alloc_urb(0, GFP_KERNEL);
			if (urb == NULL) {
				dev_dbg(&port->dev, "%s: alloc for in "
					"port failed.", __func__);
				continue;
			}
			/* Fill URB using supplied data. */
			usb_fill_bulk_urb(urb, serial->dev,
					  usb_rcvbulkpipe(serial->dev,
						port->bulk_in_endpointAddress),
					  portdata->in_buffer[j], IN_BUFLEN,
					  sierra_indat_callback, port);
			portdata->in_urbs[j] = urb;
		}
	}

	return 0;
}

static void sierra_disconnect(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct sierra_port_private *portdata;

	dev_dbg(&serial->dev->dev, "%s", __func__);

	for (i = 0; i < serial->num_ports; ++i) {
		port = serial->port[i];
		if (!port)
			continue;
		portdata = usb_get_serial_port_data(port);
		if (!portdata)
			continue;

		for (j = 0; j < N_IN_URB; j++) {
			usb_kill_urb(portdata->in_urbs[j]);
			usb_free_urb(portdata->in_urbs[j]);
		}
	}
}

static void sierra_release(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct sierra_port_private *portdata;

	dev_dbg(&serial->dev->dev, "%s\n", __func__);

	for (i = 0; i < serial->num_ports; ++i) {
		port = serial->port[i];
		if (!port)
			continue;
		portdata = usb_get_serial_port_data(port);
		if (!portdata)
			continue;

		for (j = 0; j < N_IN_URB; j++)
			kfree(portdata->in_buffer[j]);
		kfree(portdata);
	}
}

static struct usb_serial_driver sierra_device = {
	.driver = {
		.owner =	THIS_MODULE,
		.name =		"sierra",
	},
	.description       = "Sierra USB modem",
	.id_table          = id_table,
	.usb_driver        = &sierra_driver,
	.calc_num_ports	   = sierra_calc_num_ports,
	.probe		   = sierra_probe,
	.open              = sierra_open,
	.close             = sierra_close,
	.write             = sierra_write,
	.write_room        = sierra_write_room,
	.set_termios       = sierra_set_termios,
	.tiocmget          = sierra_tiocmget,
	.tiocmset          = sierra_tiocmset,
	.attach            = sierra_startup,
	.disconnect        = sierra_disconnect,
	.release           = sierra_release,
	.read_int_callback = sierra_instat_callback,
};

/* Functions used by new usb-serial code. */
static int __init sierra_init(void)
{
	int retval;
	retval = usb_serial_register(&sierra_device);
	if (retval)
		goto failed_device_register;


	retval = usb_register(&sierra_driver);
	if (retval)
		goto failed_driver_register;

	printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_VERSION ":"
	       DRIVER_DESC "\n");

	return 0;

failed_driver_register:
	usb_serial_deregister(&sierra_device);
failed_device_register:
	return retval;
}

static void __exit sierra_exit(void)
{
	usb_deregister(&sierra_driver);
	usb_serial_deregister(&sierra_device);
}

module_init(sierra_init);
module_exit(sierra_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

module_param(nmea, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(nmea, "NMEA streaming");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug messages");
