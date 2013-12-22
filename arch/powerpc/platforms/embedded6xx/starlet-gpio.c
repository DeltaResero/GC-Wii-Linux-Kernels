/*
 * arch/powerpc/platforms/embedded6xx/starlet-gpio.c
 *
 * Nintendo Wii starlet GPIO driver
 * Copyright (C) 2008 The GameCube Linux Team
 * Copyright (C) 2008 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>

struct stgpio_chip {
	struct of_mm_gpio_chip mmchip;
	spinlock_t lock;
};

struct stgpio_regs {
	__be32 data, dir;
};


static inline struct stgpio_chip *
to_stgpio_chip(struct of_mm_gpio_chip *mm_gc)
{
	return container_of(mm_gc, struct stgpio_chip, mmchip);
}

static int stgpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct stgpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);
	unsigned int val;

	val = !!(in_be32(&regs->data) & pin_mask);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);

	return val;
}

static void stgpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct stgpio_chip *st_gc = to_stgpio_chip(mm_gc);
	struct stgpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);
	u32 data;
	unsigned long flags;

	spin_lock_irqsave(&st_gc->lock, flags);
	data = in_be32(&regs->data) & ~pin_mask;
	if (val)
		data |= pin_mask;
	out_be32(&regs->data, data);
	spin_unlock_irqrestore(&st_gc->lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);
}

static int stgpio_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct stgpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);

	clrbits32(&regs->dir, pin_mask);

	return 0;
}

static int stgpio_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct stgpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);

	setbits32(&regs->dir, pin_mask);
	stgpio_set(gc, gpio, val);

	return 0;
}

int stgpio_add32(struct device_node *np)
{
	struct of_mm_gpio_chip *mm_gc;
	struct of_gpio_chip *of_gc;
	struct gpio_chip *gc;
	struct stgpio_chip *st_gc;

	st_gc = kzalloc(sizeof(*st_gc), GFP_KERNEL);
	if (!st_gc)
		return -ENOMEM;

	spin_lock_init(&st_gc->lock);
	mm_gc = &st_gc->mmchip;
	of_gc = &mm_gc->of_gc;
	gc = &of_gc->gc;

	of_gc->gpio_cells = 1;

	gc->ngpio = 32;
	gc->direction_input = stgpio_dir_in;
	gc->direction_output = stgpio_dir_out;
	gc->get = stgpio_get;
	gc->set = stgpio_set;

	return of_mm_gpiochip_add(np, mm_gc);
}

static int stgpio_init(void)
{
	struct device_node *np;
	int error;

	for_each_compatible_node(np, NULL, "nintendo,starlet-gpio") {
		error = stgpio_add32(np);
		if (error < 0)
			printk(KERN_ERR "starlet-gpio: error %d adding gpios"
					" for %s\n", error, np->full_name);
	}
	return 0; /* whatever */
}
arch_initcall(stgpio_init);

