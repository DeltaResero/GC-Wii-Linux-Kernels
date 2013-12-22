
/*
 * Copyright (C) 2006		Red Hat <alan@redhat.com>
 *
 *  May be copied or modified under the terms of the GNU General Public License
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/init.h>

#include <asm/io.h>

typedef enum {
	PORT_PATA0 = 0,
	PORT_PATA1 = 1,
	PORT_SATA = 2,
} port_type;

/**
 *	jmicron_ratemask	-	Compute available modes
 *	@drive: IDE drive
 *
 *	Compute the available speeds for the devices on the interface. This
 *	is all modes to ATA133 clipped by drive cable setup.
 */

static u8 jmicron_ratemask(ide_drive_t *drive)
{
	u8 mode	= 4;
	if (!eighty_ninty_three(drive))
		mode = min(mode, (u8)1);
	return mode;
}

/**
 *	ata66_jmicron		-	Cable check
 *	@hwif: IDE port
 *
 *	Return 1 if the cable is 80pin
 */

static int __devinit ata66_jmicron(ide_hwif_t *hwif)
{
	struct pci_dev *pdev = hwif->pci_dev;

	u32 control;
	u32 control5;

	int port = hwif->channel;
	port_type port_map[2];

	pci_read_config_dword(pdev, 0x40, &control);

	/* There are two basic mappings. One has the two SATA ports merged
	   as master/slave and the secondary as PATA, the other has only the
	   SATA port mapped */
	if (control & (1 << 23)) {
		port_map[0] = PORT_SATA;
		port_map[1] = PORT_PATA0;
	} else {
		port_map[0] = PORT_SATA;
		port_map[1] = PORT_SATA;
	}

	/* The 365/366 may have this bit set to map the second PATA port
	   as the internal primary channel */
	pci_read_config_dword(pdev, 0x80, &control5);
	if (control5 & (1<<24))
		port_map[0] = PORT_PATA1;

	/* The two ports may then be logically swapped by the firmware */
	if (control & (1 << 22))
		port = port ^ 1;

	/*
	 *	Now we know which physical port we are talking about we can
	 *	actually do our cable checking etc. Thankfully we don't need
	 *	to do the plumbing for other cases.
	 */
	switch (port_map[port])
	{
	case PORT_PATA0:
		if (control & (1 << 3))	/* 40/80 pin primary */
			return 0;
		return 1;
	case PORT_PATA1:
		if (control5 & (1 << 19))	/* 40/80 pin secondary */
			return 0;
		return 1;
	case PORT_SATA:
		break;
	}
	return 1; /* Avoid bogus "control reaches end of non-void function" */
}

static void jmicron_tuneproc (ide_drive_t *drive, byte mode_wanted)
{
	return;
}

/**
 *	config_jmicron_chipset_for_pio	-	set drive timings
 *	@drive: drive to tune
 *	@speed we want
 *
 */

static void config_jmicron_chipset_for_pio (ide_drive_t *drive, byte set_speed)
{
	u8 speed = XFER_PIO_0 + ide_get_best_pio_mode(drive, 255, 5, NULL);
	if (set_speed)
		(void) ide_config_drive_speed(drive, speed);
}

/**
 *	jmicron_tune_chipset	-	set controller timings
 *	@drive: Drive to set up
 *	@xferspeed: speed we want to achieve
 *
 *	As the JMicron snoops for timings all we actually need to do is
 *	make sure we don't set an invalid mode. We do need to honour
 *	the cable detect here.
 */

static int jmicron_tune_chipset (ide_drive_t *drive, byte xferspeed)
{

	u8 speed		= ide_rate_filter(jmicron_ratemask(drive), xferspeed);

	return ide_config_drive_speed(drive, speed);
}

/**
 *	config_chipset_for_dma	-	configure for DMA
 *	@drive: drive to configure
 *
 *	As the JMicron snoops for timings all we actually need to do is
 *	make sure we don't set an invalid mode.
 */

static int config_chipset_for_dma (ide_drive_t *drive)
{
	u8 speed	= ide_dma_speed(drive, jmicron_ratemask(drive));

	config_jmicron_chipset_for_pio(drive, !speed);
	jmicron_tune_chipset(drive, speed);
	return ide_dma_enable(drive);
}

/**
 *	jmicron_configure_drive_for_dma	-	set up for DMA transfers
 *	@drive: drive we are going to set up
 *
 *	As the JMicron snoops for timings all we actually need to do is
 *	make sure we don't set an invalid mode.
 */

static int jmicron_config_drive_for_dma (ide_drive_t *drive)
{
	ide_hwif_t *hwif	= drive->hwif;

	if (ide_use_dma(drive)) {
		if (config_chipset_for_dma(drive))
			return hwif->ide_dma_on(drive);
	}
	config_jmicron_chipset_for_pio(drive, 1);
	return hwif->ide_dma_off_quietly(drive);
}

/**
 *	init_hwif_jmicron	-	set up hwif structs
 *	@hwif: interface to set up
 *
 *	Minimal set up is required for the Jmicron hardware.
 */

static void __devinit init_hwif_jmicron(ide_hwif_t *hwif)
{
	hwif->speedproc = &jmicron_tune_chipset;
	hwif->tuneproc	= &jmicron_tuneproc;

	hwif->drives[0].autotune = 1;
	hwif->drives[1].autotune = 1;

	if (!hwif->dma_base)
		goto fallback;

	hwif->atapi_dma = 1;
	hwif->ultra_mask = 0x7f;
	hwif->mwdma_mask = 0x07;

	hwif->ide_dma_check = &jmicron_config_drive_for_dma;
	if (!(hwif->udma_four))
		hwif->udma_four = ata66_jmicron(hwif);

	hwif->autodma = 1;
	hwif->drives[0].autodma = hwif->autodma;
	hwif->drives[1].autodma = hwif->autodma;
	return;
fallback:
	hwif->autodma = 0;
	return;
}

#define DECLARE_JMB_DEV(name_str)			\
	{						\
		.name		= name_str,		\
		.init_hwif	= init_hwif_jmicron,	\
		.channels	= 2,			\
		.autodma	= AUTODMA,		\
		.bootable	= ON_BOARD,		\
		.enablebits	= { {0x40, 1, 1}, {0x40, 0x10, 0x10} }, \
	}

static ide_pci_device_t jmicron_chipsets[] __devinitdata = {
	/* 0 */ DECLARE_JMB_DEV("JMB361"),
	/* 1 */ DECLARE_JMB_DEV("JMB363"),
	/* 2 */ DECLARE_JMB_DEV("JMB365"),
	/* 3 */ DECLARE_JMB_DEV("JMB366"),
	/* 4 */ DECLARE_JMB_DEV("JMB368"),
};

/**
 *	jmicron_init_one	-	pci layer discovery entry
 *	@dev: PCI device
 *	@id: ident table entry
 *
 *	Called by the PCI code when it finds a Jmicron controller.
 *	We then use the IDE PCI generic helper to do most of the work.
 */

static int __devinit jmicron_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	ide_setup_pci_device(dev, &jmicron_chipsets[id->driver_data]);
	return 0;
}

static struct pci_device_id jmicron_pci_tbl[] = {
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB361, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB363, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 1},
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB365, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 2},
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB366, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 3},
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB368, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 4},
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, jmicron_pci_tbl);

static struct pci_driver driver = {
	.name		= "JMicron IDE",
	.id_table	= jmicron_pci_tbl,
	.probe		= jmicron_init_one,
};

static int __init jmicron_ide_init(void)
{
	return ide_pci_register_driver(&driver);
}

module_init(jmicron_ide_init);

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("PCI driver module for the JMicron in legacy modes");
MODULE_LICENSE("GPL");
