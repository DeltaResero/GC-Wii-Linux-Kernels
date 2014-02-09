#ifndef _MIPS_SETUP_H
#define _MIPS_SETUP_H

#define COMMAND_LINE_SIZE	CONFIG_COMMAND_LINE_SIZE

#ifdef  __KERNEL__
extern void setup_early_printk(void);
#endif /* __KERNEL__ */

#endif /* __SETUP_H */
