/*
 * include/linux/exi.h
 *
 * Nintendo GameCube EXpansion Interface definitions
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004 Arthur Othieno <a.othieno@bluewin.ch>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __EXI_H
#define __EXI_H

#include <linux/device.h>
#include <asm/io.h>


extern struct bus_type exi_bus_type;
struct exi_channel;

/*
 *
 */
struct exi_device_id {
	unsigned int		channel;
#define	EXI_CHANNEL_ANY	(~0)

	unsigned int		device;
#define	EXI_DEVICE_ANY	(~0)
	
	u32			id;
#define	EXI_ID_INVALID	(~0)
#define	EXI_ID_NONE	(EXI_ID_INVALID-1)
};

/*
 *
 */
struct exi_device {
	struct exi_channel	*exi_channel;

	struct exi_device_id	eid;
	int			frequency;

	unsigned long		flags;

	struct device		dev;
};

#define to_exi_device(n) container_of(n,struct exi_device,dev)

struct exi_device *exi_get_exi_device(struct exi_channel *exi_channel,
				      int device);

/*
 *
 */
struct exi_driver {
	char			*name;
	struct exi_device_id	*eid_table;
	int			frequency;
	
	int  (*probe)  (struct exi_device *dev);
	void (*remove) (struct exi_device *dev);

	struct device_driver driver;
};

#define to_exi_driver(n) container_of(n,struct exi_driver,driver)


/*
 * EXpansion Interface devices and drivers.
 *
 */
extern struct exi_device *exi_device_get(struct exi_device *exi_device);
extern void exi_device_put(struct exi_device *exi_device);

extern int  exi_driver_register(struct exi_driver *exi_driver);
extern void exi_driver_unregister(struct exi_driver *exi_driver);

static inline void *exi_get_drvdata(struct exi_device *exi_dev)
{
	return dev_get_drvdata(&exi_dev->dev);
}
                                                                                
static inline void exi_set_drvdata(struct exi_device *exi_dev, void *data)
{
	dev_set_drvdata(&exi_dev->dev, data);
}


/*
 * EXpansion Interface channels.
 *
 */

extern struct exi_channel *to_exi_channel(unsigned int channel);
extern unsigned int to_channel(struct exi_channel *exi_channel);

static inline struct exi_channel *exi_get_exi_channel(struct exi_device *dev)
{
	return dev->exi_channel;
}

#define EXI_EVENT_IRQ     0
#define EXI_EVENT_INSERT  1
#define EXI_EVENT_TC      2
                                                                                
typedef int (*exi_event_handler_t)(struct exi_channel *exi_channel,
				   unsigned int event_id, void *data);

extern int exi_event_register(struct exi_channel *exi_channel,
			      unsigned int event_id,
			      exi_event_handler_t handler, void *data,
			      unsigned int channel_mask);
extern int exi_event_unregister(struct exi_channel *exi_channel,
				unsigned int event_id);


/*
 * Commands.
 *
 *
 */
struct exi_command {
	int			opcode;
#define EXI_OP_READ      (0x00<<2) /* same as in EXIxCR */
#define EXI_OP_WRITE     (0x01<<2) /* same as in EXIxCR */
#define EXI_OP_READWRITE (0x02<<2) /* same as in EXIxCR */

#define EXI_OP_SELECT    0x0100
#define EXI_OP_DESELECT  0x0200
#define EXI_OP_NOP       -1

	unsigned long		flags;
#define EXI_NODMA (1<<0)

	void			*data;
	size_t			len;

	dma_addr_t		dma_addr;
	size_t			dma_len;

	void			*done_data;
	void			(*done)(struct exi_command *cmd);

	struct exi_channel	*exi_channel;
};

static inline void exi_op_basic(struct exi_command *cmd,
				struct exi_channel *exi_channel)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->exi_channel = exi_channel;
}

static inline void exi_op_nop(struct exi_command *cmd,
			      struct exi_channel *exi_channel)
{
	exi_op_basic(cmd, exi_channel);
	cmd->opcode = EXI_OP_NOP;
}

static inline void exi_op_select(struct exi_command *cmd,
				 struct exi_device *exi_device)
{
	exi_op_basic(cmd, exi_device->exi_channel);
	cmd->opcode = EXI_OP_SELECT;
	cmd->data = exi_device;
}

static inline void exi_op_deselect(struct exi_command *cmd,
				   struct exi_channel *exi_channel)
{
	exi_op_basic(cmd, exi_channel);
	cmd->opcode = EXI_OP_DESELECT;
}

static inline void exi_op_transfer(struct exi_command *cmd,
				   struct exi_channel *exi_channel,
				   void *data, size_t len, int opcode)
{
	exi_op_basic(cmd, exi_channel);
	cmd->opcode = opcode;
	cmd->data = data;
	cmd->len = len;
}

static inline void exi_op_read(struct exi_command *cmd,
			       struct exi_channel *exi_channel,
			       void *data, size_t len)
{
	exi_op_transfer(cmd, exi_channel, data, len, EXI_OP_READ);
}

static inline void exi_op_write(struct exi_command *cmd,
				struct exi_channel *exi_channel,
				void *data, size_t len)
{
	exi_op_transfer(cmd, exi_channel, data, len, EXI_OP_WRITE);
}


/*
 * EXpansion Interface interfaces.
 *
 */
#include "../drivers/exi/exi-hw.h"

/* 
 * Raw.
 */
extern void exi_select_raw(struct exi_channel *exi_channel,
			   unsigned int device, unsigned int freq);
extern void exi_deselect_raw(struct exi_channel *exi_channel);

#define exi_transfer_u8_raw  __exi_transfer_raw_u8
#define exi_transfer_u16_raw __exi_transfer_raw_u16
#define exi_transfer_u32_raw __exi_transfer_raw_u32

extern void exi_transfer_raw(struct exi_channel *exi_channel,
			     void *data, size_t len, int mode);
extern void exi_dma_transfer_raw(struct exi_channel *channel,
				 dma_addr_t data, size_t len, int mode);

/* 
 * Standard.
 */

extern u32 exi_get_id(struct exi_channel *exi_channel,
		      unsigned int device, unsigned int freq);

int exi_select(struct exi_device *exi_device);
void exi_deselect(struct exi_channel *exi_channel);
void exi_transfer(struct exi_channel *exi_channel,
		  void *data, size_t len, int opcode);

#define exi_dev_select(d) exi_select(d)

static inline void exi_dev_deselect(struct exi_device *exi_device)
{
	return exi_deselect(exi_device->exi_channel);
}

static inline void exi_dev_transfer(struct exi_device *exi_device,
		      void *data, size_t len, int opcode)
{
	return exi_transfer(exi_device->exi_channel, data, len, opcode);
}

static inline void exi_dev_read(struct exi_device *dev, void *data, size_t len)
{
	exi_dev_transfer(dev, data, len, EXI_OP_READ);
}

static inline void exi_dev_write(struct exi_device *dev, void *data, size_t len)
{
	exi_dev_transfer(dev, data, len, EXI_OP_WRITE);
}



/*
 * Compatibility layer with old EXI_LITE.
 */

#ifdef CONFIG_EXI_LITE2_COMPAT

#ifndef EXI_LITE
#define EXI_LITE 2

extern unsigned long exi_running;

static inline int exi_lite_init(void)
{
	int retval = 0;

	if (!test_and_set_bit(1, &exi_running)) {
		retval = exi_hw_init("exi-lite");
	}
	return retval;
}

static inline void exi_lite_exit(void)
{
	exi_hw_exit();
}

static inline int exi_lite_select(int channel, int device, int freq)
{
	struct exi_channel *exi_channel = to_exi_channel(channel);
	struct exi_device *exi_device = exi_get_exi_device(exi_channel, device);
	return exi_select(exi_device);
}

static inline void exi_lite_deselect(int channel)
{
	struct exi_channel *exi_channel = to_exi_channel(channel);
	exi_deselect(exi_channel);
}

static inline void exi_lite_read(int channel, void *data, size_t len)
{
	exi_transfer(to_exi_channel(channel), data, len, EXI_OP_READ);
}

static inline void exi_lite_write(int channel, void *data, size_t len)
{
	exi_transfer(to_exi_channel(channel), data, len, EXI_OP_WRITE);
}

static inline int exi_lite_register_event(int channel, int event_id,
					  exi_event_handler_t handler,
					  void *dev, unsigned int channel_mask)
{
	return exi_event_register(to_exi_channel(channel),
				  (unsigned int)event_id,
				  handler, dev, channel_mask);
}

static inline int exi_lite_unregister_event(int channel, int event_id)
{
	return exi_event_unregister(to_exi_channel(channel),
				    (unsigned int)event_id);
}

#endif /* EXI_LITE */

#endif /* CONFIG_EXI_LITE2_COMPAT */

#endif /* __EXI_H */

