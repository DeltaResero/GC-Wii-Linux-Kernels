/*
 * arch/ppc/platforms/gcn-con.c
 *
 * Nintendo GameCube early debug console
 * Copyright (C) 2004-2005 The GameCube Linux Team
 *
 * Based on console.c by tmbinc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/string.h>
#include <linux/console.h>
#include <linux/font.h>
#include <linux/cache.h>
#include <asm/cacheflush.h>

#include "gamecube.h"

#define FONT_XSIZE  8
#define FONT_YSIZE  16
#define FONT_XFACTOR 1
#define FONT_YFACTOR 1
#define FONT_XGAP   2
#define FONT_YGAP   0

#define COLOR_WHITE 0xFF80FF80
#define COLOR_BLACK 0x00800080

struct console_data_s {
	unsigned char *framebuffer;
	int xres, yres, stride;

	const unsigned char *font;

	int cursor_x, cursor_y;
	int foreground, background;

	int border_left, border_right, border_top, border_bottom;

	int scrolled_lines;
};

static struct console_data_s *default_console;

#if 0
static int console_set_color(int background, int foreground)
{
	default_console->foreground = foreground;
	default_console->background = background;
	return 0;
}
#endif

static void console_drawc(struct console_data_s *con, int x, int y,
			  unsigned char c)
{
	x >>= 1;
	int ax, ay;
	unsigned long *ptr =
	    (unsigned long *)(con->framebuffer + con->stride * y + x * 4);
	for (ay = 0; ay < FONT_YSIZE; ay++) {
#if FONT_XFACTOR == 2
		for (ax = 0; ax < 8; ax++) {
			unsigned long color;
			if ((con->font[c * FONT_YSIZE + ay] << ax) & 0x80)
				color = con->foreground;
			else
				color = con->background;
#if FONT_YFACTOR == 2
			// pixel doubling: we write u32
			ptr[ay * 2 * con->stride / 4 + ax] = color;
			// line doubling
			ptr[(ay * 2 + 1) * con->stride / 4 + ax] = color;
#else
			ptr[ay * con->stride / 4 + ax] = color;
#endif
		}
#else
		for (ax = 0; ax < 4; ax++) {
			unsigned long color[2];
			int bits = (con->font[c * FONT_YSIZE + ay] << (ax * 2));
			if (bits & 0x80)
				color[0] = con->foreground;
			else
				color[0] = con->background;
			if (bits & 0x40)
				color[1] = con->foreground;
			else
				color[1] = con->background;
			ptr[ay * con->stride / 4 + ax] =
			    (color[0] & 0xFFFF00FF) | (color[1] & 0x0000FF00);
		}
#endif
	}
}

static void console_putc(struct console_data_s *con, char c)
{
	switch (c) {
	case '\n':
		con->cursor_y += FONT_YSIZE * FONT_YFACTOR + FONT_YGAP;
		con->cursor_x = con->border_left;
		break;
	default:
		console_drawc(con, con->cursor_x, con->cursor_y, c);
		con->cursor_x += FONT_XSIZE * FONT_XFACTOR + FONT_XGAP;
		if ((con->cursor_x + (FONT_XSIZE * FONT_XFACTOR)) >
		    con->border_right) {
			con->cursor_y += FONT_YSIZE * FONT_YFACTOR + FONT_YGAP;
			con->cursor_x = con->border_left;
		}
	}
	if ((con->cursor_y + FONT_YSIZE * FONT_YFACTOR) >= con->border_bottom) {
		memcpy(con->framebuffer,
		       con->framebuffer +
		       con->stride * (FONT_YSIZE * FONT_YFACTOR + FONT_YGAP),
		       con->stride * con->yres - FONT_YSIZE);
		int cnt =
		    (con->stride * (FONT_YSIZE * FONT_YFACTOR + FONT_YGAP)) / 4;
		unsigned long *ptr =
		    (unsigned long *)(con->framebuffer +
				      con->stride * (con->yres - FONT_YSIZE));
		while (cnt--)
			*ptr++ = con->background;
		con->cursor_y -= FONT_YSIZE * FONT_YFACTOR + FONT_YGAP;
	}

	flush_dcache_range((unsigned long)con->framebuffer,
			   (unsigned long)(con->framebuffer +
					   con->stride * con->yres));
}

static void console_puts(struct console_data_s *con, const char *string)
{
	while (*string)
		console_putc(con, *string++);
}

static void console_init(struct console_data_s *con, void *framebuffer,
			 int xres, int yres, int stride)
{
	int c;
	unsigned long *p;

	con->framebuffer = framebuffer;
	con->xres = xres;
	con->yres = yres;
	con->border_left = 0;
	con->border_top = 0;
	con->border_right = con->xres;
	con->border_bottom = con->yres;
	con->stride = stride;
	con->cursor_x = con->cursor_y = 0;

	con->font = font_vga_8x16.data;

	con->foreground = COLOR_WHITE;
	con->background = COLOR_BLACK;

	con->scrolled_lines = 0;

	c = con->xres * con->yres / 2;
	p = (unsigned long *)con->framebuffer;
	while (c--)
		*p++ = con->background;

	default_console = con;
}



static struct console_data_s gcn_con_data;

static struct console gcn_con = {
	.name = "gcn-con",
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

/**
 *
 */
void gcn_con_puts(const char *s)
{
	if (!default_console)
		gcn_con_init();
	console_puts(default_console, s);
}

/**
 *
 */
void gcn_con_putc(char c)
{
	if (!default_console)
		gcn_con_init();
	console_putc(default_console, c);
}

/**
 *
 */
static void gcn_con_write(struct console *co, const char *b,
			     unsigned int count)
{
	while (count--)
		console_putc(default_console, *b++);
}

/**
 *
 */
void gcn_con_init(void)
{
	console_init(&gcn_con_data, (void *)(0xd0000000 | GCN_XFB_START),
		     640, GCN_VIDEO_LINES, 640 * 2);
	gcn_con_puts("gcn-con: console initialized.\n");

	gcn_con.write = gcn_con_write;
	register_console(&gcn_con);
}

