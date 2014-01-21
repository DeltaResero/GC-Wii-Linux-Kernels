/*
 * arch/powerpc/include/asm/wii.h
 *
 * Nintendo Wii board-specific definitions
 * Copyright (C) 2010 The GameCube Linux Team
 * Copyright (C) 2010 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef __ASM_POWERPC_WII_H
#define __ASM_POWERPC_WII_H

/*
 * DMA operations for the Nintendo Wii.
 */
extern struct dma_map_ops wii_mem2_dma_ops;

extern int wii_set_mem2_dma_constraints(struct device *dev);
extern void wii_clear_mem2_dma_constraints(struct device *dev);

#endif /* __ASM_POWERPC_WII_H */
