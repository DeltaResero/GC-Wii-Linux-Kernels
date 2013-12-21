/*
 * Since there are many different boards and no standard configuration,
 * we have a unique include file for each.  Rather than change every
 * file that has to include 6xx/7xx/74xx configuration, they all include
 * this one and the configuration switching is done here.
 */
#ifdef __KERNEL__
#ifndef __ASM_PPC_PPC6xx_H__
#define __ASM_PPC_PPC6xx_H__

#include <linux/config.h>

#ifdef CONFIG_6xx

#ifdef CONFIG_GAMECUBE
#include <platforms/gamecube.h>
#endif

#ifndef __ASSEMBLY__
/* The "residual" data board information structure the boot loader
 * hands to us.
 */
extern unsigned char __res[];
#endif

#endif	/* CONFIG_6xx */
#endif	/* !__ASM_PPC_PPC6xx_H__ */
#endif /* __KERNEL__ */
