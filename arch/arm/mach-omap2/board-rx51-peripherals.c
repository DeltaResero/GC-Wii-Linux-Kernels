/*
 * linux/arch/arm/mach-omap2/board-rx51-peripherals.c
 *
 * Copyright (C) 2008-2009 Nokia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/spi/spi.h>
#include <linux/i2c.h>
#include <linux/i2c/twl4030.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/regulator/machine.h>
#include <linux/gpio.h>
#include <linux/mmc/host.h>

#include <mach/mcspi.h>
#include <mach/mux.h>
#include <mach/board.h>
#include <mach/common.h>
#include <mach/dma.h>
#include <mach/gpmc.h>
#include <mach/onenand.h>
#include <mach/gpmc-smc91x.h>

#include "mmc-twl4030.h"

#define SYSTEM_REV_B_USES_VAUX3	0x1699
#define SYSTEM_REV_S_USES_VAUX3 0x8

static int board_keymap[] = {
	/*
	 * Note that KEY(x, 8, KEY_XXX) entries represent "entrire row
	 * connected to the ground" matrix state.
	 */
	KEY(0, 0, KEY_Q),
	KEY(0, 1, KEY_O),
	KEY(0, 2, KEY_P),
	KEY(0, 3, KEY_COMMA),
	KEY(0, 4, KEY_BACKSPACE),
	KEY(0, 6, KEY_A),
	KEY(0, 7, KEY_S),

	KEY(1, 0, KEY_W),
	KEY(1, 1, KEY_D),
	KEY(1, 2, KEY_F),
	KEY(1, 3, KEY_G),
	KEY(1, 4, KEY_H),
	KEY(1, 5, KEY_J),
	KEY(1, 6, KEY_K),
	KEY(1, 7, KEY_L),

	KEY(2, 0, KEY_E),
	KEY(2, 1, KEY_DOT),
	KEY(2, 2, KEY_UP),
	KEY(2, 3, KEY_ENTER),
	KEY(2, 5, KEY_Z),
	KEY(2, 6, KEY_X),
	KEY(2, 7, KEY_C),
	KEY(2, 8, KEY_F9),

	KEY(3, 0, KEY_R),
	KEY(3, 1, KEY_V),
	KEY(3, 2, KEY_B),
	KEY(3, 3, KEY_N),
	KEY(3, 4, KEY_M),
	KEY(3, 5, KEY_SPACE),
	KEY(3, 6, KEY_SPACE),
	KEY(3, 7, KEY_LEFT),

	KEY(4, 0, KEY_T),
	KEY(4, 1, KEY_DOWN),
	KEY(4, 2, KEY_RIGHT),
	KEY(4, 4, KEY_LEFTCTRL),
	KEY(4, 5, KEY_RIGHTALT),
	KEY(4, 6, KEY_LEFTSHIFT),
	KEY(4, 8, KEY_F10),

	KEY(5, 0, KEY_Y),
	KEY(5, 8, KEY_F11),

	KEY(6, 0, KEY_U),

	KEY(7, 0, KEY_I),
	KEY(7, 1, KEY_F7),
	KEY(7, 2, KEY_F8),
};

static struct matrix_keymap_data board_map_data = {
	.keymap			= board_keymap,
	.keymap_size		= ARRAY_SIZE(board_keymap),
};

static struct twl4030_keypad_data rx51_kp_data = {
	.keymap_data	= &board_map_data,
	.rows		= 8,
	.cols		= 8,
	.rep		= 1,
};

static struct twl4030_madc_platform_data rx51_madc_data = {
	.irq_line		= 1,
};

static struct twl4030_hsmmc_info mmc[] = {
	{
		.name		= "external",
		.mmc		= 1,
		.wires		= 4,
		.cover_only	= true,
		.gpio_cd	= 160,
		.gpio_wp	= -EINVAL,
		.power_saving	= true,
	},
	{
		.name		= "internal",
		.mmc		= 2,
		.wires		= 8,
		.gpio_cd	= -EINVAL,
		.gpio_wp	= -EINVAL,
		.nonremovable	= true,
		.power_saving	= true,
	},
	{}	/* Terminator */
};

static struct regulator_consumer_supply rx51_vmmc1_supply = {
	.supply			= "vmmc",
};

static struct regulator_consumer_supply rx51_vmmc2_supply = {
	.supply			= "vmmc",
};

static struct regulator_consumer_supply rx51_vsim_supply = {
	.supply			= "vmmc_aux",
};

static struct regulator_init_data rx51_vaux1 = {
	.constraints = {
		.name			= "V28",
		.min_uV			= 2800000,
		.max_uV			= 2800000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
};

static struct regulator_init_data rx51_vaux2 = {
	.constraints = {
		.name			= "VCSI",
		.min_uV			= 1800000,
		.max_uV			= 1800000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
};

/* VAUX3 - adds more power to VIO_18 rail */
static struct regulator_init_data rx51_vaux3_cam = {
	.constraints = {
		.name			= "VCAM_DIG_18",
		.min_uV			= 1800000,
		.max_uV			= 1800000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
};

static struct regulator_init_data rx51_vaux3_mmc = {
	.constraints = {
		.name			= "VMMC2_30",
		.min_uV			= 2800000,
		.max_uV			= 3000000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= 1,
	.consumer_supplies	= &rx51_vmmc2_supply,
};

static struct regulator_init_data rx51_vaux4 = {
	.constraints = {
		.name			= "VCAM_ANA_28",
		.min_uV			= 2800000,
		.max_uV			= 2800000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
};

static struct regulator_init_data rx51_vmmc1 = {
	.constraints = {
		.min_uV			= 1850000,
		.max_uV			= 3150000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= 1,
	.consumer_supplies	= &rx51_vmmc1_supply,
};

static struct regulator_init_data rx51_vmmc2 = {
	.constraints = {
		.name			= "VMMC2_30",
		.min_uV			= 1850000,
		.max_uV			= 3150000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= 1,
	.consumer_supplies	= &rx51_vmmc2_supply,
};

static struct regulator_init_data rx51_vsim = {
	.constraints = {
		.name			= "VMMC2_IO_18",
		.min_uV			= 1800000,
		.max_uV			= 1800000,
		.apply_uV		= true,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies	= 1,
	.consumer_supplies	= &rx51_vsim_supply,
};

static struct regulator_init_data rx51_vdac = {
	.constraints = {
		.min_uV			= 1800000,
		.max_uV			= 1800000,
		.valid_modes_mask	= REGULATOR_MODE_NORMAL
					| REGULATOR_MODE_STANDBY,
		.valid_ops_mask		= REGULATOR_CHANGE_VOLTAGE
					| REGULATOR_CHANGE_MODE
					| REGULATOR_CHANGE_STATUS,
	},
};

static int rx51_twlgpio_setup(struct device *dev, unsigned gpio, unsigned n)
{
	/* FIXME this gpio setup is just a placeholder for now */
	gpio_request(gpio + 6, "backlight_pwm");
	gpio_direction_output(gpio + 6, 0);
	gpio_request(gpio + 7, "speaker_en");
	gpio_direction_output(gpio + 7, 1);

	/* set up MMC adapters, linking their regulators to them */
	twl4030_mmc_init(mmc);
	rx51_vmmc1_supply.dev = mmc[0].dev;
	rx51_vmmc2_supply.dev = mmc[1].dev;
	rx51_vsim_supply.dev = mmc[1].dev;

	return 0;
}

static struct twl4030_gpio_platform_data rx51_gpio_data = {
	.gpio_base		= OMAP_MAX_GPIO_LINES,
	.irq_base		= TWL4030_GPIO_IRQ_BASE,
	.irq_end		= TWL4030_GPIO_IRQ_END,
	.pulldowns		= BIT(0) | BIT(1) | BIT(2) | BIT(3)
				| BIT(4) | BIT(5)
				| BIT(8) | BIT(9) | BIT(10) | BIT(11)
				| BIT(12) | BIT(13) | BIT(14) | BIT(15)
				| BIT(16) | BIT(17) ,
	.setup			= rx51_twlgpio_setup,
};

static struct twl4030_usb_data rx51_usb_data = {
	.usb_mode		= T2_USB_MODE_ULPI,
};

static struct twl4030_ins sleep_on_seq[] __initdata = {
/*
 * Turn off VDD1 and VDD2.
 */
	{MSG_SINGULAR(DEV_GRP_P1, 0xf, RES_STATE_OFF), 4},
	{MSG_SINGULAR(DEV_GRP_P1, 0x10, RES_STATE_OFF), 2},
/*
 * And also turn off the OMAP3 PLLs and the sysclk output.
 */
	{MSG_SINGULAR(DEV_GRP_P1, 0x7, RES_STATE_OFF), 3},
	{MSG_SINGULAR(DEV_GRP_P1, 0x17, RES_STATE_OFF), 3},
};

static struct twl4030_script sleep_on_script __initdata = {
	.script = sleep_on_seq,
	.size   = ARRAY_SIZE(sleep_on_seq),
	.flags  = TWL4030_SLEEP_SCRIPT,
};

static struct twl4030_ins wakeup_seq[] __initdata = {
/*
 * Reenable the OMAP3 PLLs.
 * Wakeup VDD1 and VDD2.
 * Reenable sysclk output.
 */
	{MSG_SINGULAR(DEV_GRP_P1, 0x7, RES_STATE_ACTIVE), 0x30},
	{MSG_SINGULAR(DEV_GRP_P1, 0xf, RES_STATE_ACTIVE), 0x30},
	{MSG_SINGULAR(DEV_GRP_P1, 0x10, RES_STATE_ACTIVE), 0x37},
	{MSG_SINGULAR(DEV_GRP_P1, 0x19, RES_STATE_ACTIVE), 3},
};

static struct twl4030_script wakeup_script __initdata = {
	.script	= wakeup_seq,
	.size	= ARRAY_SIZE(wakeup_seq),
	.flags	= TWL4030_WAKEUP12_SCRIPT,
};

static struct twl4030_ins wakeup_p3_seq[] __initdata = {
/*
 * Wakeup VDD1 (dummy to be able to insert a delay)
 * Enable CLKEN
 */
	{MSG_SINGULAR(DEV_GRP_P1, 0x17, RES_STATE_ACTIVE), 3},
};

static struct twl4030_script wakeup_p3_script __initdata = {
	.script	= wakeup_p3_seq,
	.size	= ARRAY_SIZE(wakeup_p3_seq),
	.flags	= TWL4030_WAKEUP3_SCRIPT,
};

static struct twl4030_ins wrst_seq[] __initdata = {
/*
 * Reset twl4030.
 * Reset VDD1 regulator.
 * Reset VDD2 regulator.
 * Reset VPLL1 regulator.
 * Enable sysclk output.
 * Reenable twl4030.
 */
	{MSG_SINGULAR(DEV_GRP_NULL, RES_RESET, RES_STATE_OFF), 2},
	{MSG_BROADCAST(DEV_GRP_NULL, RES_GRP_ALL, 0, 1, RES_STATE_ACTIVE),
		0x13},
	{MSG_BROADCAST(DEV_GRP_NULL, RES_GRP_PP, 0, 2, RES_STATE_WRST), 0x13},
	{MSG_BROADCAST(DEV_GRP_NULL, RES_GRP_PP, 0, 3, RES_STATE_OFF), 0x13},
	{MSG_SINGULAR(DEV_GRP_NULL, RES_VDD1, RES_STATE_WRST), 0x13},
	{MSG_SINGULAR(DEV_GRP_NULL, RES_VDD2, RES_STATE_WRST), 0x13},
	{MSG_SINGULAR(DEV_GRP_NULL, RES_VPLL1, RES_STATE_WRST), 0x35},
	{MSG_SINGULAR(DEV_GRP_P1, RES_HFCLKOUT, RES_STATE_ACTIVE), 2},
	{MSG_SINGULAR(DEV_GRP_NULL, RES_RESET, RES_STATE_ACTIVE), 2},
};

static struct twl4030_script wrst_script __initdata = {
	.script = wrst_seq,
	.size   = ARRAY_SIZE(wrst_seq),
	.flags  = TWL4030_WRST_SCRIPT,
};

static struct twl4030_script *twl4030_scripts[] __initdata = {
	/* wakeup12 script should be loaded before sleep script, otherwise a
	   board might hit retention before loading of wakeup script is
	   completed. This can cause boot failures depending on timing issues.
	*/
	&wakeup_script,
	&sleep_on_script,
	&wakeup_p3_script,
	&wrst_script,
};

static struct twl4030_resconfig twl4030_rconfig[] __initdata = {
	{ .resource = RES_VINTANA1, .devgroup = -1, .type = -1, .type2 = 1 },
	{ .resource = RES_VINTANA2, .devgroup = -1, .type = -1, .type2 = 1 },
	{ .resource = RES_VINTDIG, .devgroup = -1, .type = -1, .type2 = 1 },
	{ .resource = RES_VMMC1, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VMMC2, .devgroup = DEV_GRP_NULL, .type = -1,
	  .type2 = 3},
	{ .resource = RES_VAUX1, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VAUX2, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VAUX3, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VAUX4, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VPLL2, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VDAC, .devgroup = -1, .type = -1, .type2 = 3},
	{ .resource = RES_VSIM, .devgroup = DEV_GRP_NULL, .type = -1,
	  .type2 = 3},
	{ .resource = RES_CLKEN, .devgroup = DEV_GRP_P3, .type = -1,
		.type2 = 1 },
	{ 0, 0},
};

static struct twl4030_power_data rx51_t2scripts_data __initdata = {
	.scripts        = twl4030_scripts,
	.num = ARRAY_SIZE(twl4030_scripts),
	.resource_config = twl4030_rconfig,
};



static struct twl4030_platform_data rx51_twldata __initdata = {
	.irq_base		= TWL4030_IRQ_BASE,
	.irq_end		= TWL4030_IRQ_END,

	/* platform_data for children goes here */
	.gpio			= &rx51_gpio_data,
	.keypad			= &rx51_kp_data,
	.madc			= &rx51_madc_data,
	.usb			= &rx51_usb_data,
	.power			= &rx51_t2scripts_data,

	.vaux1			= &rx51_vaux1,
	.vaux2			= &rx51_vaux2,
	.vaux4			= &rx51_vaux4,
	.vmmc1			= &rx51_vmmc1,
	.vsim			= &rx51_vsim,
	.vdac			= &rx51_vdac,
};

static struct i2c_board_info __initdata rx51_peripherals_i2c_board_info_1[] = {
	{
		I2C_BOARD_INFO("twl5030", 0x48),
		.flags = I2C_CLIENT_WAKE,
		.irq = INT_34XX_SYS_NIRQ,
		.platform_data = &rx51_twldata,
	},
};

static int __init rx51_i2c_init(void)
{
	if ((system_rev >= SYSTEM_REV_S_USES_VAUX3 && system_rev < 0x100) ||
	    system_rev >= SYSTEM_REV_B_USES_VAUX3)
		rx51_twldata.vaux3 = &rx51_vaux3_mmc;
	else {
		rx51_twldata.vaux3 = &rx51_vaux3_cam;
		rx51_twldata.vmmc2 = &rx51_vmmc2;
	}
	omap_register_i2c_bus(1, 2200, rx51_peripherals_i2c_board_info_1,
			ARRAY_SIZE(rx51_peripherals_i2c_board_info_1));
	omap_register_i2c_bus(2, 100, NULL, 0);
	omap_register_i2c_bus(3, 400, NULL, 0);
	return 0;
}

#if defined(CONFIG_MTD_ONENAND_OMAP2) || \
	defined(CONFIG_MTD_ONENAND_OMAP2_MODULE)

static struct mtd_partition onenand_partitions[] = {
	{
		.name           = "bootloader",
		.offset         = 0,
		.size           = 0x20000,
		.mask_flags     = MTD_WRITEABLE,	/* Force read-only */
	},
	{
		.name           = "config",
		.offset         = MTDPART_OFS_APPEND,
		.size           = 0x60000,
	},
	{
		.name           = "log",
		.offset         = MTDPART_OFS_APPEND,
		.size           = 0x40000,
	},
	{
		.name           = "kernel",
		.offset         = MTDPART_OFS_APPEND,
		.size           = 0x200000,
	},
	{
		.name           = "initfs",
		.offset         = MTDPART_OFS_APPEND,
		.size           = 0x200000,
	},
	{
		.name           = "rootfs",
		.offset         = MTDPART_OFS_APPEND,
		.size           = MTDPART_SIZ_FULL,
	},
};

static struct omap_onenand_platform_data board_onenand_data = {
	.cs		= 0,
	.gpio_irq	= 65,
	.parts		= onenand_partitions,
	.nr_parts	= ARRAY_SIZE(onenand_partitions),
	.flags		= ONENAND_SYNC_READWRITE,
};

static void __init board_onenand_init(void)
{
	gpmc_onenand_init(&board_onenand_data);
}

#else

static inline void board_onenand_init(void)
{
}

#endif

#if defined(CONFIG_SMC91X) || defined(CONFIG_SMC91X_MODULE)

static struct omap_smc91x_platform_data board_smc91x_data = {
	.cs		= 1,
	.gpio_irq	= 54,
	.gpio_pwrdwn	= 86,
	.gpio_reset	= 164,
	.flags		= GPMC_TIMINGS_SMC91C96 | IORESOURCE_IRQ_HIGHLEVEL,
};

static void __init board_smc91x_init(void)
{
	omap_cfg_reg(U8_34XX_GPIO54_DOWN);
	omap_cfg_reg(G25_34XX_GPIO86_OUT);
	omap_cfg_reg(H19_34XX_GPIO164_OUT);

	gpmc_smc91x_init(&board_smc91x_data);
}

#else

static inline void board_smc91x_init(void)
{
}

#endif

void __init rx51_peripherals_init(void)
{
	rx51_i2c_init();
	board_onenand_init();
	board_smc91x_init();
}

