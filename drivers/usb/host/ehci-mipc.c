/*
 * drivers/usb/host/ehci-mipc.c
 *
 * Nintendo Wii USB Enhanced Host Controller Interface via 'mini' IPC (mipc).
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Based on ehci-ppc-of.c
 *
 * EHCI HCD (Host Controller Driver) for USB.
 *
 * Bus Glue for PPC On-Chip EHCI driver on the of_platform bus
 * Tested on AMCC PPC 440EPx
 *
 * Valentine Barshak <vbarshak@ru.mvista.com>
 *
 * Based on "ehci-ppc-soc.c" by Stefan Roese <sr@denx.de>
 * and "ohci-ppc-of.c" by Sylvain Munaut <tnt@246tNt.com>
 *
 * This file is licenced under the GPL.
 */

#include <linux/signal.h>

#include <linux/of.h>
#include <linux/of_platform.h>
#include <asm/starlet.h>

#define DRV_MODULE_NAME "ehci-mipc"
#define DRV_DESCRIPTION "USB Enhanced Host Controller Interface via MINI"
#define DRV_AUTHOR      "Albert Herranz"

#define HOLLYWOOD_EHCI_CTL	0x0d0400cc
#define HOLLYWOOD_EHCI_CTL_INTE		(1<<15)


/* called during probe() after chip reset completes */
static int ehci_mipc_reset(struct usb_hcd *hcd)
{
	struct ehci_hcd	*ehci = hcd_to_ehci(hcd);
	void __iomem *ehci_ctl = (void __iomem *)HOLLYWOOD_EHCI_CTL;
	int		retval;

	retval = ehci_halt(ehci);
	if (retval)
		return retval;

	retval = ehci_init(hcd);
	if (retval)
		return retval;

	/* enable notification of EHCI interrupts */
	mipc_out_be32(ehci_ctl, mipc_in_be32(ehci_ctl) |
						HOLLYWOOD_EHCI_CTL_INTE);

	ehci->sbrn = 0x20;
	return ehci_reset(ehci);
}

static const struct hc_driver ehci_mipc_hc_driver = {
	.description		= hcd_name,
	.product_desc		= "Nintendo Wii EHCI Host Controller",
	.hcd_priv_size		= sizeof(struct ehci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq			= ehci_irq,
	.flags			= HCD_USB2 | HCD_BOUNCE_DMA_MEM,

	/*
	 * basic lifecycle operations
	 */
	.reset			= ehci_mipc_reset,
	.start			= ehci_run,
	.stop			= ehci_stop,
	.shutdown		= ehci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue		= ehci_urb_enqueue,
	.urb_dequeue		= ehci_urb_dequeue,
	.endpoint_disable	= ehci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number	= ehci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data	= ehci_hub_status_data,
	.hub_control		= ehci_hub_control,
#ifdef	CONFIG_PM
	.bus_suspend		= ehci_bus_suspend,
	.bus_resume		= ehci_bus_resume,
#endif
	.relinquish_port	= ehci_relinquish_port,
	.port_handed_over	= ehci_port_handed_over,
};


static int __devinit
ehci_hcd_mipc_probe(struct of_device *op, const struct of_device_id *match)
{
	struct device_node *dn = op->node;
	struct usb_hcd *hcd;
	struct ehci_hcd	*ehci = NULL;
	struct resource res;
	dma_addr_t coherent_mem_addr;
	size_t coherent_mem_size;
	int irq;
	int error = -ENODEV;

	if (usb_disabled())
		goto out;

	if (starlet_get_ipc_flavour() != STARLET_IPC_MINI)
		goto out;

	dev_dbg(&op->dev, "initializing " DRV_MODULE_NAME " USB Controller\n");

	error = of_address_to_resource(dn, 0, &res);
	if (error)
		goto out;

	hcd = usb_create_hcd(&ehci_mipc_hc_driver, &op->dev, DRV_MODULE_NAME);
	if (!hcd) {
		error = -ENOMEM;
		goto out;
	}

	hcd->rsrc_start = res.start;
	hcd->rsrc_len = res.end - res.start + 1;

	error = of_address_to_resource(dn, 1, &res);
	if (error) {
		/* satisfy coherent memory allocations from mem1 or mem2 */
		dev_warn(&op->dev, "using normal memory\n");
	} else {
		coherent_mem_addr = res.start;
		coherent_mem_size = res.end - res.start + 1;
		if (!dma_declare_coherent_memory(&op->dev, coherent_mem_addr,
						 coherent_mem_addr,
						 coherent_mem_size,
						 DMA_MEMORY_MAP |
						 DMA_MEMORY_EXCLUSIVE)) {
			dev_err(&op->dev, "error declaring %u bytes of"
				" coherent memory at 0x%p\n",
				coherent_mem_size, (void *)coherent_mem_addr);
			error = -EBUSY;
			goto err_decl_coherent;
		}
	}

	irq = irq_of_parse_and_map(dn, 0);
	if (irq == NO_IRQ) {
		printk(KERN_ERR __FILE__ ": irq_of_parse_and_map failed\n");
		error = -EBUSY;
		goto err_irq;
	}

	hcd->regs = (void __iomem *)(unsigned long)hcd->rsrc_start;

	ehci = hcd_to_ehci(hcd);
	ehci->caps = hcd->regs;
	ehci->regs = hcd->regs +
			HC_LENGTH(ehci_readl(ehci, &ehci->caps->hc_capbase));

	/* cache this readonly data; minimize chip reads */
	ehci->hcs_params = ehci_readl(ehci, &ehci->caps->hcs_params);

	error = usb_add_hcd(hcd, irq, 0);
	if (error == 0)
		return 0;

	irq_dispose_mapping(irq);
err_irq:
	dma_release_declared_memory(&op->dev);
err_decl_coherent:
	usb_put_hcd(hcd);
out:
	return error;
}


static int ehci_hcd_mipc_remove(struct of_device *op)
{
	struct usb_hcd *hcd = dev_get_drvdata(&op->dev);

	dev_set_drvdata(&op->dev, NULL);

	dev_dbg(&op->dev, "stopping " DRV_MODULE_NAME " USB Controller\n");

	usb_remove_hcd(hcd);
	irq_dispose_mapping(hcd->irq);
	dma_release_declared_memory(&op->dev);
	usb_put_hcd(hcd);

	return 0;
}


static int ehci_hcd_mipc_shutdown(struct of_device *op)
{
	struct usb_hcd *hcd = dev_get_drvdata(&op->dev);

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);

	return 0;
}


static struct of_device_id ehci_hcd_mipc_match[] = {
	{
		.compatible = "nintendo,hollywood-ehci",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ehci_hcd_mipc_match);

static struct of_platform_driver ehci_hcd_mipc_driver = {
	.name		= DRV_MODULE_NAME,
	.match_table	= ehci_hcd_mipc_match,
	.probe		= ehci_hcd_mipc_probe,
	.remove		= ehci_hcd_mipc_remove,
	.shutdown	= ehci_hcd_mipc_shutdown,
	.driver		= {
		.name	= DRV_MODULE_NAME,
		.owner	= THIS_MODULE,
	},
};

