/*
 * arch/powerpc/platforms/embedded6xx/hollywood-pic.c
 *
 * Nintendo Wii "hollywood" interrupt controller support.
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <asm/io.h>
#include <asm/starlet-mini.h>

#include "hollywood-pic.h"


#define DRV_MODULE_NAME "hollywood-pic"

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)


#define HOLLYWOOD_NR_IRQS	32

/*
 * Each interrupt has a corresponding bit in both
 * the Interrupt Cause (ICR) and Interrupt Mask (IMR) registers.
 *
 * Enabling/disabling an interrupt line involves asserting/clearing
 * the corresponding bit in IMR. ACK'ing a request simply involves
 * asserting the corresponding bit in ICR.
 */
#define HW_BROADWAY_ICR		0x00
#define HW_BROADWAY_IMR		0x04


/*
 * IRQ chip hooks.
 *
 */

static void hollywood_pic_mask_and_ack(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	mipc_clear_bit(irq, io_base + HW_BROADWAY_IMR);
	mipc_set_bit(irq, io_base + HW_BROADWAY_ICR);
	mipc_wmb();
}

static void hollywood_pic_ack(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	mipc_set_bit(irq, io_base + HW_BROADWAY_ICR);
	mipc_wmb();
}

static void hollywood_pic_mask(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	mipc_clear_bit(irq, io_base + HW_BROADWAY_IMR);
	mipc_wmb();
}

static void hollywood_pic_unmask(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	mipc_set_bit(irq, io_base + HW_BROADWAY_IMR);
	mipc_wmb();
}


static struct irq_chip hollywood_pic = {
	.typename	= "hollywood-pic",
	.ack		= hollywood_pic_ack,
	.mask_ack	= hollywood_pic_mask_and_ack,
	.mask		= hollywood_pic_mask,
	.unmask		= hollywood_pic_unmask,
};

/*
 * IRQ host hooks.
 *
 */

static struct irq_host *hollywood_irq_host;

static int hollywood_pic_map(struct irq_host *h, unsigned int virq,
			   irq_hw_number_t hwirq)
{
	set_irq_chip_data(virq, h->host_data);
	set_irq_chip_and_handler(virq, &hollywood_pic, handle_level_irq);
	return 0;
}

static void hollywood_pic_unmap(struct irq_host *h, unsigned int irq)
{
	set_irq_chip_data(irq, NULL);
	set_irq_chip(irq, NULL);
}

static struct irq_host_ops hollywood_irq_host_ops = {
	.map = hollywood_pic_map,
	.unmap = hollywood_pic_unmap,
};

static unsigned int __hollywood_pic_get_irq(struct irq_host *h)
{
	void __iomem *io_base = h->host_data;
	int irq;
	u32 irq_status;

	irq_status = mipc_in_be32(io_base + HW_BROADWAY_ICR) &
		     mipc_in_be32(io_base + HW_BROADWAY_IMR);
	if (irq_status == 0)
		return NO_IRQ_IGNORE;	/* no more IRQs pending */

	__asm__ __volatile__("cntlzw %0,%1" : "=r"(irq) : "r"(irq_status));
	return irq_linear_revmap(h, 31 - irq);
}

static void hollywood_pic_irq_cascade(unsigned int cascade_virq,
				      struct irq_desc *desc)
{
	struct irq_host *irq_host = get_irq_data(cascade_virq);
	unsigned int virq;

	spin_lock(&desc->lock);
	desc->chip->mask(cascade_virq); /* IRQ_LEVEL */
	spin_unlock(&desc->lock);

	virq = __hollywood_pic_get_irq(irq_host);
	if (virq != NO_IRQ_IGNORE)
		generic_handle_irq(virq);
	else
		drv_printk(KERN_ERR, "spurious interrupt!\n");

	spin_lock(&desc->lock);
	desc->chip->ack(cascade_virq); /* IRQ_LEVEL */
	if (!(desc->status & IRQ_DISABLED) && desc->chip->unmask)
		desc->chip->unmask(cascade_virq);
	spin_unlock(&desc->lock);
}

/*
 * Platform hooks.
 *
 */

static void __hollywood_quiesce(void __iomem *io_base)
{
	/* mask and ack all IRQs */
	mipc_out_be32(io_base + HW_BROADWAY_IMR, 0);
	mipc_out_be32(io_base + HW_BROADWAY_ICR, ~0);
	mipc_wmb();
}

struct irq_host *hollywood_pic_init(struct device_node *np)
{
	struct irq_host *irq_host;
	struct resource res;
	void __iomem *io_base;
	int retval;

	retval = of_address_to_resource(np, 0, &res);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return NULL;
	}
	io_base = mipc_ioremap(res.start, resource_size(&res));
	if (!io_base) {
		drv_printk(KERN_ERR, "ioremap failed\n");
		return NULL;
	}

	drv_printk(KERN_INFO, "controller at 0x%p\n", io_base);

	__hollywood_quiesce(io_base);

	irq_host = irq_alloc_host(np, IRQ_HOST_MAP_LINEAR, HOLLYWOOD_NR_IRQS,
				  &hollywood_irq_host_ops, NO_IRQ_IGNORE);
	if (!irq_host) {
		drv_printk(KERN_ERR, "failed to allocate irq_host\n");
		return NULL;
	}
	irq_host->host_data = io_base;

	return irq_host;
}

unsigned int hollywood_pic_get_irq(void)
{
	return __hollywood_pic_get_irq(hollywood_irq_host);
}

/*
 * Probe function.
 *
 */

void hollywood_pic_probe(void)
{
	struct irq_host *host;
	struct device_node *np;
	const u32 *interrupts;
	int cascade_virq;

	for_each_compatible_node(np, NULL, "nintendo,hollywood-pic") {
		interrupts = of_get_property(np, "interrupts", NULL);
		if (interrupts) {
			host = hollywood_pic_init(np);
			BUG_ON(!host);
			cascade_virq = irq_of_parse_and_map(np, 0);
			set_irq_data(cascade_virq, host);
			set_irq_chained_handler(cascade_virq,
						hollywood_pic_irq_cascade);
		}
	}
}

/**
 * hollywood_quiesce() - quiesce hollywood irq controller
 *
 * Mask and ack all interrupt sources.
 *
 */
void hollywood_quiesce(void)
{
	void __iomem *io_base = hollywood_irq_host->host_data;

	__hollywood_quiesce(io_base);
}

