/*
 * arch/ppc/platforms/gcn-rsw.c
 *
 * Nintendo GameCube reset switch driver
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004 Stefan Esser
 * Copyright (C) 2004,2005 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/reboot.h>

#ifdef CONFIG_KEXEC
#include <linux/kexec.h>
#endif

#define RSW_IRQ 1

#define RSW_NORMAL_TIMEOUT      3	/* seconds */
#define RSW_EMERGENCY_PUSHES   10

typedef enum {
	IDLE = 0,		/* nothing to do */
	NORMAL_RESET,		/* reboot requested */
	EMERGENCY_RESET,	/* try emergency reboot */
} gcn_rsw_state_t;

struct gcn_rsw_private {
	gcn_rsw_state_t state;
	struct timer_list timer;
	unsigned long jiffies;
	int pushes;
	int timeout;
	spinlock_t lock;
};


#define DRV_MODULE_NAME      "gcn-rsw"
#define DRV_DESCRIPTION      "Nintendo GameCube reset switch driver"
#define DRV_AUTHOR           "Stefan Esser <se@nopiracy.de>"
                                                                                
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
                                                                                
#define PFX DRV_MODULE_NAME ": "
#define rsw_printk(level, format, arg...) \
	printk(level PFX format , ## arg)
                                                                                

/* from kernel/sys.c */
extern void ctrl_alt_del(void);

static struct gcn_rsw_private gcn_rsw_private = {
	.state = IDLE,
	.timeout = RSW_NORMAL_TIMEOUT,
};


/**
 *
 */
static void gcn_rsw_normal_reset(unsigned long dummy)
{
	ctrl_alt_del();
}

/**
 *
 */
static void gcn_rsw_emergency_reset(void)
{
#ifdef CONFIG_KEXEC
	struct kimage *image;
	image = xchg(&kexec_image, 0);
	if (image) {
		machine_kexec(image);
	}
#endif
	machine_restart(NULL);
}

/**
 *
 */
static irqreturn_t gcn_rsw_handler(int this_irq, void *data,
				     struct pt_regs *regs)
{
	struct gcn_rsw_private *priv = (struct gcn_rsw_private *)data;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	/* someone pushed the reset button */
	switch (priv->state) {
	case IDLE:
		priv->state = NORMAL_RESET;
		printk(KERN_EMERG "Rebooting in %d seconds...\n",
		       priv->timeout);
		printk(KERN_WARNING
		       "Push the Reset button again to cancel reboot!\n");

		/* schedule a reboot in a few seconds */
		init_timer(&priv->timer);
		priv->timer.expires = jiffies + priv->timeout * HZ;
		priv->timer.function =
		    (void (*)(unsigned long))gcn_rsw_normal_reset;
		add_timer(&priv->timer);
		priv->jiffies = jiffies;
		break;
	case NORMAL_RESET:
		if (time_before(jiffies, priv->jiffies + priv->timeout * HZ)) {
			/* the reset button was hit again before deadline */
			del_timer(&priv->timer);
			priv->state = IDLE;
			printk(KERN_EMERG "Reboot cancelled!\n");
		} else {
			/*
			 * Time expired. System should be now restarting.
			 * Go to emergency mode in case something goes bad.
			 */
			priv->state = EMERGENCY_RESET;
			priv->pushes = 0;
			printk(KERN_WARNING
			       "SWITCHED TO EMERGENCY RESET MODE!\n"
			       "Push %d times the Reset button to force"
			       " a hard reset!\n"
			       "NOTE THAT THIS COULD CAUSE DATA LOSS!\n",
			       RSW_EMERGENCY_PUSHES);
		}
		break;
	case EMERGENCY_RESET:
		/* force a hard reset if the user insists ... */
		if (++priv->pushes >= RSW_EMERGENCY_PUSHES) {
			spin_unlock_irqrestore(&priv->lock, flags);
			gcn_rsw_emergency_reset();
			return IRQ_HANDLED;
		} else {
			printk(KERN_INFO
			       "%d/%d\n",
			       priv->pushes,
			       RSW_EMERGENCY_PUSHES);
		}
		break;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return IRQ_HANDLED;
}

/**
 *
 */
static int gcn_rsw_init(void)
{
	int err;

	spin_lock_init(&gcn_rsw_private.lock);

	err = request_irq(RSW_IRQ, gcn_rsw_handler, 0,
			  "Nintendo GameCube reset switch",
			  (void *)&gcn_rsw_private);
	if (err) {
		rsw_printk(KERN_ERR, "request of irq%d failed\n", RSW_IRQ);
	}

	return err;
}

/**
 *
 */
static void gcn_rsw_exit(void)
{
	free_irq(RSW_IRQ, &gcn_rsw_private);
}

module_init(gcn_rsw_init);
module_exit(gcn_rsw_exit);

