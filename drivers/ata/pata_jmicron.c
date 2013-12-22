/*
 *    pata_jmicron.c - JMicron ATA driver for non AHCI mode. This drives the
 *			PATA port of the controller. The SATA ports are
 *			driven by AHCI in the usual configuration although
 *			this driver can handle other setups if we need it.
 *
 *	(c) 2006 Red Hat  <alan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>
#include <linux/ata.h>

#define DRV_NAME	"pata_jmicron"
#define DRV_VERSION	"0.1.4"

typedef enum {
	PORT_PATA0 = 0,
	PORT_PATA1 = 1,
	PORT_SATA = 2,
} port_type;

/**
 *	jmicron_pre_reset	-	check for 40/80 pin
 *	@ap: Port
 *
 *	Perform the PATA port setup we need.

 *	On the Jmicron 361/363 there is a single PATA port that can be mapped
 *	either as primary or secondary (or neither). We don't do any policy
 *	and setup here. We assume that has been done by init_one and the
 *	BIOS.
 */

static int jmicron_pre_reset(struct ata_port *ap)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u32 control;
	u32 control5;
	int port_mask = 1<< (4 * ap->port_no);
	int port = ap->port_no;
	port_type port_map[2];

	/* Check if our port is enabled */
	pci_read_config_dword(pdev, 0x40, &control);
	if ((control & port_mask) == 0)
		return -ENOENT;

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
		if (control & (1 << 5))
			return 0;
		if (control & (1 << 3))	/* 40/80 pin primary */
			ap->cbl = ATA_CBL_PATA40;
		else
			ap->cbl = ATA_CBL_PATA80;
		break;
	case PORT_PATA1:
		/* Bit 21 is set if the port is enabled */
		if ((control5 & (1 << 21)) == 0)
			return 0;
		if (control5 & (1 << 19))	/* 40/80 pin secondary */
			ap->cbl = ATA_CBL_PATA40;
		else
			ap->cbl = ATA_CBL_PATA80;
		break;
	case PORT_SATA:
		ap->cbl = ATA_CBL_SATA;
		break;
	}
	return ata_std_prereset(ap);
}

/**
 *	jmicron_error_handler - Setup and error handler
 *	@ap: Port to handle
 *
 *	LOCKING:
 *	None (inherited from caller).
 */

static void jmicron_error_handler(struct ata_port *ap)
{
	return ata_bmdma_drive_eh(ap, jmicron_pre_reset, ata_std_softreset, NULL, ata_std_postreset);
}

/* No PIO or DMA methods needed for this device */

static struct scsi_host_template jmicron_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.slave_destroy		= ata_scsi_slave_destroy,
	/* Use standard CHS mapping rules */
	.bios_param		= ata_std_bios_param,
#ifdef CONFIG_PM
	.suspend		= ata_scsi_device_suspend,
	.resume			= ata_scsi_device_resume,
#endif
};

static const struct ata_port_operations jmicron_ops = {
	.port_disable		= ata_port_disable,

	/* Task file is PCI ATA format, use helpers */
	.tf_load		= ata_tf_load,
	.tf_read		= ata_tf_read,
	.check_status		= ata_check_status,
	.exec_command		= ata_exec_command,
	.dev_select		= ata_std_dev_select,

	.freeze			= ata_bmdma_freeze,
	.thaw			= ata_bmdma_thaw,
	.error_handler		= jmicron_error_handler,
	.post_internal_cmd	= ata_bmdma_post_internal_cmd,

	/* BMDMA handling is PCI ATA format, use helpers */
	.bmdma_setup		= ata_bmdma_setup,
	.bmdma_start		= ata_bmdma_start,
	.bmdma_stop		= ata_bmdma_stop,
	.bmdma_status		= ata_bmdma_status,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.data_xfer		= ata_pio_data_xfer,

	/* IRQ-related hooks */
	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,

	/* Generic PATA PCI ATA helpers */
	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
	.host_stop		= ata_host_stop,
};


/**
 *	jmicron_init_one - Register Jmicron ATA PCI device with kernel services
 *	@pdev: PCI device to register
 *	@ent: Entry in jmicron_pci_tbl matching with @pdev
 *
 *	Called from kernel PCI layer.
 *
 *	LOCKING:
 *	Inherited from PCI layer (may sleep).
 *
 *	RETURNS:
 *	Zero on success, or -ERRNO value.
 */

static int jmicron_init_one (struct pci_dev *pdev, const struct pci_device_id *id)
{
	static struct ata_port_info info = {
		.sht		= &jmicron_sht,
		.flags	= ATA_FLAG_SLAVE_POSS | ATA_FLAG_SRST,

		.pio_mask	= 0x1f,
		.mwdma_mask	= 0x07,
		.udma_mask 	= 0x3f,

		.port_ops	= &jmicron_ops,
	};
	struct ata_port_info *port_info[2] = { &info, &info };

	u32 reg;

	/* PATA controller is fn 1, AHCI is fn 0 */
	if (id->driver_data != 368 && PCI_FUNC(pdev->devfn) != 1)
		return -ENODEV;

	/* The 365/66 have two PATA channels, redirect the second */
	if (id->driver_data == 365 || id->driver_data == 366) {
		pci_read_config_dword(pdev, 0x80, &reg);
		reg |= (1 << 24);	/* IDE1 to PATA IDE secondary */
		pci_write_config_dword(pdev, 0x80, reg);
	}

	return ata_pci_init_one(pdev, port_info, 2);
}

#ifdef CONFIG_PM
static int jmicron_reinit_one(struct pci_dev *pdev)
{
	u32 reg;
	
	switch(pdev->device) {
		case PCI_DEVICE_ID_JMICRON_JMB368:
			break;
		case PCI_DEVICE_ID_JMICRON_JMB365:
		case PCI_DEVICE_ID_JMICRON_JMB366:
			/* Restore mapping or disks swap and boy does it get ugly */
			pci_read_config_dword(pdev, 0x80, &reg);
			reg |= (1 << 24);	/* IDE1 to PATA IDE secondary */
			pci_write_config_dword(pdev, 0x80, reg);
			/* Fall through */
		default:
			/* Make sure AHCI is turned back on */
			pci_write_config_byte(pdev, 0x41, 0xa1);
	}
	return ata_pci_device_resume(pdev);
}
#endif

static const struct pci_device_id jmicron_pci_tbl[] = {
	{ PCI_VDEVICE(JMICRON, PCI_DEVICE_ID_JMICRON_JMB361), 361},
	{ PCI_VDEVICE(JMICRON, PCI_DEVICE_ID_JMICRON_JMB363), 363},
	{ PCI_VDEVICE(JMICRON, PCI_DEVICE_ID_JMICRON_JMB365), 365},
	{ PCI_VDEVICE(JMICRON, PCI_DEVICE_ID_JMICRON_JMB366), 366},
	{ PCI_VDEVICE(JMICRON, PCI_DEVICE_ID_JMICRON_JMB368), 368},

	{ }	/* terminate list */
};

static struct pci_driver jmicron_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= jmicron_pci_tbl,
	.probe			= jmicron_init_one,
	.remove			= ata_pci_remove_one,
#ifdef CONFIG_PM
	.suspend		= ata_pci_device_suspend,
	.resume			= jmicron_reinit_one,
#endif
};

static int __init jmicron_init(void)
{
	return pci_register_driver(&jmicron_pci_driver);
}

static void __exit jmicron_exit(void)
{
	pci_unregister_driver(&jmicron_pci_driver);
}

module_init(jmicron_init);
module_exit(jmicron_exit);

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("SCSI low-level driver for Jmicron PATA ports");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, jmicron_pci_tbl);
MODULE_VERSION(DRV_VERSION);

