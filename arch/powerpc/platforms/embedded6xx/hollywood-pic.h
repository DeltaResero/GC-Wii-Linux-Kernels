/*
 * arch/powerpc/platforms/embedded6xx/hollywood-pic.h
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

#ifndef __HOLLYWOOD_PIC_H
#define __HOLLYWOOD_PIC_H

extern unsigned int hollywood_pic_get_irq(void);
extern void hollywood_pic_probe(void);
extern void hollywood_quiesce(void);

#endif
