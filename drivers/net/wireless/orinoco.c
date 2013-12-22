/* orinoco.c - (formerly known as dldwd_cs.c and orinoco_cs.c)
 *
 * A driver for Hermes or Prism 2 chipset based PCMCIA wireless
 * adaptors, with Lucent/Agere, Intersil or Symbol firmware.
 *
 * Current maintainers (as of 29 September 2003) are:
 * 	Pavel Roskin <proski AT gnu.org>
 * and	David Gibson <hermes AT gibson.dropbear.id.au>
 *
 * (C) Copyright David Gibson, IBM Corporation 2001-2003.
 * Copyright (C) 2000 David Gibson, Linuxcare Australia.
 *	With some help from :
 * Copyright (C) 2001 Jean Tourrilhes, HP Labs
 * Copyright (C) 2001 Benjamin Herrenschmidt
 *
 * Based on dummy_cs.c 1.27 2000/06/12 21:27:25
 *
 * Portions based on wvlan_cs.c 1.0.6, Copyright Andreas Neuhaus <andy
 * AT fasta.fh-dortmund.de>
 *      http://www.stud.fh-dortmund.de/~andy/wvlan/
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License
 * at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and
 * limitations under the License.
 *
 * The initial developer of the original code is David A. Hinds
 * <dahinds AT users.sourceforge.net>.  Portions created by David
 * A. Hinds are Copyright (C) 1999 David A. Hinds.  All Rights
 * Reserved.
 *
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License version 2 (the "GPL"), in
 * which case the provisions of the GPL are applicable instead of the
 * above.  If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use your
 * version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GPL.  If you do not delete the
 * provisions above, a recipient may use your version of this file
 * under either the MPL or the GPL.  */

/*
 * TODO
 *	o Handle de-encapsulation within network layer, provide 802.11
 *	  headers (patch from Thomas 'Dent' Mirlacher)
 *	o Fix possible races in SPY handling.
 *	o Disconnect wireless extensions from fundamental configuration.
 *	o (maybe) Software WEP support (patch from Stano Meduna).
 *	o (maybe) Use multiple Tx buffers - driver handling queue
 *	  rather than firmware.
 */

/* Locking and synchronization:
 *
 * The basic principle is that everything is serialized through a
 * single spinlock, priv->lock.  The lock is used in user, bh and irq
 * context, so when taken outside hardirq context it should always be
 * taken with interrupts disabled.  The lock protects both the
 * hardware and the struct orinoco_private.
 *
 * Another flag, priv->hw_unavailable indicates that the hardware is
 * unavailable for an extended period of time (e.g. suspended, or in
 * the middle of a hard reset).  This flag is protected by the
 * spinlock.  All code which touches the hardware should check the
 * flag after taking the lock, and if it is set, give up on whatever
 * they are doing and drop the lock again.  The orinoco_lock()
 * function handles this (it unlocks and returns -EBUSY if
 * hw_unavailable is non-zero).
 */

#define DRIVER_NAME "orinoco"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/firmware.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>
#include <net/iw_handler.h>
#include <net/ieee80211.h>

#include <linux/scatterlist.h>
#include <linux/crypto.h>

#include "hermes_rid.h"
#include "hermes_dld.h"
#include "orinoco.h"

/********************************************************************/
/* Module information                                               */
/********************************************************************/

MODULE_AUTHOR("Pavel Roskin <proski@gnu.org> & David Gibson <hermes@gibson.dropbear.id.au>");
MODULE_DESCRIPTION("Driver for Lucent Orinoco, Prism II based and similar wireless cards");
MODULE_LICENSE("Dual MPL/GPL");

/* Level of debugging. Used in the macros in orinoco.h */
#ifdef ORINOCO_DEBUG
int orinoco_debug = ORINOCO_DEBUG;
module_param(orinoco_debug, int, 0644);
MODULE_PARM_DESC(orinoco_debug, "Debug level");
EXPORT_SYMBOL(orinoco_debug);
#endif

static int suppress_linkstatus; /* = 0 */
module_param(suppress_linkstatus, bool, 0644);
MODULE_PARM_DESC(suppress_linkstatus, "Don't log link status changes");
static int ignore_disconnect; /* = 0 */
module_param(ignore_disconnect, int, 0644);
MODULE_PARM_DESC(ignore_disconnect, "Don't report lost link to the network layer");

static int force_monitor; /* = 0 */
module_param(force_monitor, int, 0644);
MODULE_PARM_DESC(force_monitor, "Allow monitor mode for all firmware versions");

/********************************************************************/
/* Compile time configuration and compatibility stuff               */
/********************************************************************/

/* We do this this way to avoid ifdefs in the actual code */
#ifdef WIRELESS_SPY
#define SPY_NUMBER(priv)	(priv->spy_data.spy_number)
#else
#define SPY_NUMBER(priv)	0
#endif /* WIRELESS_SPY */

/********************************************************************/
/* Internal constants                                               */
/********************************************************************/

/* 802.2 LLC/SNAP header used for Ethernet encapsulation over 802.11 */
static const u8 encaps_hdr[] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
#define ENCAPS_OVERHEAD		(sizeof(encaps_hdr) + 2)

#define ORINOCO_MIN_MTU		256
#define ORINOCO_MAX_MTU		(IEEE80211_DATA_LEN - ENCAPS_OVERHEAD)

#define SYMBOL_MAX_VER_LEN	(14)
#define USER_BAP		0
#define IRQ_BAP			1
#define MAX_IRQLOOPS_PER_IRQ	10
#define MAX_IRQLOOPS_PER_JIFFY	(20000/HZ) /* Based on a guestimate of
					    * how many events the
					    * device could
					    * legitimately generate */
#define SMALL_KEY_SIZE		5
#define LARGE_KEY_SIZE		13
#define TX_NICBUF_SIZE_BUG	1585		/* Bug in Symbol firmware */

#define DUMMY_FID		0xFFFF

/*#define MAX_MULTICAST(priv)	(priv->firmware_type == FIRMWARE_TYPE_AGERE ? \
  HERMES_MAX_MULTICAST : 0)*/
#define MAX_MULTICAST(priv)	(HERMES_MAX_MULTICAST)

#define ORINOCO_INTEN	 	(HERMES_EV_RX | HERMES_EV_ALLOC \
				 | HERMES_EV_TX | HERMES_EV_TXEXC \
				 | HERMES_EV_WTERR | HERMES_EV_INFO \
				 | HERMES_EV_INFDROP )

#define MAX_RID_LEN 1024

static const struct iw_handler_def orinoco_handler_def;
static const struct ethtool_ops orinoco_ethtool_ops;

/********************************************************************/
/* Data tables                                                      */
/********************************************************************/

/* The frequency of each channel in MHz */
static const long channel_frequency[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442,
	2447, 2452, 2457, 2462, 2467, 2472, 2484
};
#define NUM_CHANNELS ARRAY_SIZE(channel_frequency)

/* This tables gives the actual meanings of the bitrate IDs returned
 * by the firmware. */
static struct {
	int bitrate; /* in 100s of kilobits */
	int automatic;
	u16 agere_txratectrl;
	u16 intersil_txratectrl;
} bitrate_table[] = {
	{110, 1,  3, 15}, /* Entry 0 is the default */
	{10,  0,  1,  1},
	{10,  1,  1,  1},
	{20,  0,  2,  2},
	{20,  1,  6,  3},
	{55,  0,  4,  4},
	{55,  1,  7,  7},
	{110, 0,  5,  8},
};
#define BITRATE_TABLE_SIZE ARRAY_SIZE(bitrate_table)

/********************************************************************/
/* Data types                                                       */
/********************************************************************/

/* Beginning of the Tx descriptor, used in TxExc handling */
struct hermes_txexc_data {
	struct hermes_tx_descriptor desc;
	__le16 frame_ctl;
	__le16 duration_id;
	u8 addr1[ETH_ALEN];
} __attribute__ ((packed));

/* Rx frame header except compatibility 802.3 header */
struct hermes_rx_descriptor {
	/* Control */
	__le16 status;
	__le32 time;
	u8 silence;
	u8 signal;
	u8 rate;
	u8 rxflow;
	__le32 reserved;

	/* 802.11 header */
	__le16 frame_ctl;
	__le16 duration_id;
	u8 addr1[ETH_ALEN];
	u8 addr2[ETH_ALEN];
	u8 addr3[ETH_ALEN];
	__le16 seq_ctl;
	u8 addr4[ETH_ALEN];

	/* Data length */
	__le16 data_len;
} __attribute__ ((packed));

/********************************************************************/
/* Function prototypes                                              */
/********************************************************************/

static int __orinoco_program_rids(struct net_device *dev);
static void __orinoco_set_multicast_list(struct net_device *dev);

/********************************************************************/
/* Michael MIC crypto setup                                         */
/********************************************************************/
#define MICHAEL_MIC_LEN 8
static int orinoco_mic_init(struct orinoco_private *priv)
{
	priv->tx_tfm_mic = crypto_alloc_hash("michael_mic", 0, 0);
	if (IS_ERR(priv->tx_tfm_mic)) {
		printk(KERN_DEBUG "orinoco_mic_init: could not allocate "
		       "crypto API michael_mic\n");
		priv->tx_tfm_mic = NULL;
		return -ENOMEM;
	}

	priv->rx_tfm_mic = crypto_alloc_hash("michael_mic", 0, 0);
	if (IS_ERR(priv->rx_tfm_mic)) {
		printk(KERN_DEBUG "orinoco_mic_init: could not allocate "
		       "crypto API michael_mic\n");
		priv->rx_tfm_mic = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void orinoco_mic_free(struct orinoco_private *priv)
{
	if (priv->tx_tfm_mic)
		crypto_free_hash(priv->tx_tfm_mic);
	if (priv->rx_tfm_mic)
		crypto_free_hash(priv->rx_tfm_mic);
}

static int michael_mic(struct crypto_hash *tfm_michael, u8 *key,
		       u8 *da, u8 *sa, u8 priority,
		       u8 *data, size_t data_len, u8 *mic)
{
	struct hash_desc desc;
	struct scatterlist sg[2];
	u8 hdr[ETH_HLEN + 2]; /* size of header + padding */

	if (tfm_michael == NULL) {
		printk(KERN_WARNING "michael_mic: tfm_michael == NULL\n");
		return -1;
	}

	/* Copy header into buffer. We need the padding on the end zeroed */
	memcpy(&hdr[0], da, ETH_ALEN);
	memcpy(&hdr[ETH_ALEN], sa, ETH_ALEN);
	hdr[ETH_ALEN*2] = priority;
	hdr[ETH_ALEN*2+1] = 0;
	hdr[ETH_ALEN*2+2] = 0;
	hdr[ETH_ALEN*2+3] = 0;

	/* Use scatter gather to MIC header and data in one go */
	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], hdr, sizeof(hdr));
	sg_set_buf(&sg[1], data, data_len);

	if (crypto_hash_setkey(tfm_michael, key, MIC_KEYLEN))
		return -1;

	desc.tfm = tfm_michael;
	desc.flags = 0;
	return crypto_hash_digest(&desc, sg, data_len + sizeof(hdr),
				  mic);
}

/********************************************************************/
/* Internal helper functions                                        */
/********************************************************************/

static inline void set_port_type(struct orinoco_private *priv)
{
	switch (priv->iw_mode) {
	case IW_MODE_INFRA:
		priv->port_type = 1;
		priv->createibss = 0;
		break;
	case IW_MODE_ADHOC:
		if (priv->prefer_port3) {
			priv->port_type = 3;
			priv->createibss = 0;
		} else {
			priv->port_type = priv->ibss_port;
			priv->createibss = 1;
		}
		break;
	case IW_MODE_MONITOR:
		priv->port_type = 3;
		priv->createibss = 0;
		break;
	default:
		printk(KERN_ERR "%s: Invalid priv->iw_mode in set_port_type()\n",
		       priv->ndev->name);
	}
}

#define ORINOCO_MAX_BSS_COUNT	64
static int orinoco_bss_data_allocate(struct orinoco_private *priv)
{
	if (priv->bss_xbss_data)
		return 0;

	if (priv->has_ext_scan)
		priv->bss_xbss_data = kzalloc(ORINOCO_MAX_BSS_COUNT *
					      sizeof(struct xbss_element),
					      GFP_KERNEL);
	else
		priv->bss_xbss_data = kzalloc(ORINOCO_MAX_BSS_COUNT *
					      sizeof(struct bss_element),
					      GFP_KERNEL);

	if (!priv->bss_xbss_data) {
		printk(KERN_WARNING "Out of memory allocating beacons");
		return -ENOMEM;
	}
	return 0;
}

static void orinoco_bss_data_free(struct orinoco_private *priv)
{
	kfree(priv->bss_xbss_data);
	priv->bss_xbss_data = NULL;
}

#define PRIV_BSS	((struct bss_element *)priv->bss_xbss_data)
#define PRIV_XBSS	((struct xbss_element *)priv->bss_xbss_data)
static void orinoco_bss_data_init(struct orinoco_private *priv)
{
	int i;

	INIT_LIST_HEAD(&priv->bss_free_list);
	INIT_LIST_HEAD(&priv->bss_list);
	if (priv->has_ext_scan)
		for (i = 0; i < ORINOCO_MAX_BSS_COUNT; i++)
			list_add_tail(&(PRIV_XBSS[i].list),
				      &priv->bss_free_list);
	else
		for (i = 0; i < ORINOCO_MAX_BSS_COUNT; i++)
			list_add_tail(&(PRIV_BSS[i].list),
				      &priv->bss_free_list);

}

static inline u8 *orinoco_get_ie(u8 *data, size_t len,
				 enum ieee80211_mfie eid)
{
	u8 *p = data;
	while ((p + 2) < (data + len)) {
		if (p[0] == eid)
			return p;
		p += p[1] + 2;
	}
	return NULL;
}

#define WPA_OUI_TYPE	"\x00\x50\xF2\x01"
#define WPA_SELECTOR_LEN 4
static inline u8 *orinoco_get_wpa_ie(u8 *data, size_t len)
{
	u8 *p = data;
	while ((p + 2 + WPA_SELECTOR_LEN) < (data + len)) {
		if ((p[0] == MFIE_TYPE_GENERIC) &&
		    (memcmp(&p[2], WPA_OUI_TYPE, WPA_SELECTOR_LEN) == 0))
			return p;
		p += p[1] + 2;
	}
	return NULL;
}


/********************************************************************/
/* Download functionality                                           */
/********************************************************************/

struct fw_info {
	char *pri_fw;
	char *sta_fw;
	char *ap_fw;
	u32 pda_addr;
	u16 pda_size;
};

const static struct fw_info orinoco_fw[] = {
	{ "", "agere_sta_fw.bin", "agere_ap_fw.bin", 0x00390000, 1000 },
	{ "", "prism_sta_fw.bin", "prism_ap_fw.bin", 0, 1024 },
	{ "symbol_sp24t_prim_fw", "symbol_sp24t_sec_fw", "", 0x00003100, 512 }
};

/* Structure used to access fields in FW
 * Make sure LE decoding macros are used
 */
struct orinoco_fw_header {
	char hdr_vers[6];       /* ASCII string for header version */
	__le16 headersize;      /* Total length of header */
	__le32 entry_point;     /* NIC entry point */
	__le32 blocks;          /* Number of blocks to program */
	__le32 block_offset;    /* Offset of block data from eof header */
	__le32 pdr_offset;      /* Offset to PDR data from eof header */
	__le32 pri_offset;      /* Offset to primary plug data */
	__le32 compat_offset;   /* Offset to compatibility data*/
	char signature[0];      /* FW signature length headersize-20 */
} __attribute__ ((packed));

/* Download either STA or AP firmware into the card. */
static int
orinoco_dl_firmware(struct orinoco_private *priv,
		    const struct fw_info *fw,
		    int ap)
{
	/* Plug Data Area (PDA) */
	__le16 *pda;

	hermes_t *hw = &priv->hw;
	const struct firmware *fw_entry;
	const struct orinoco_fw_header *hdr;
	const unsigned char *first_block;
	const unsigned char *end;
	const char *firmware;
	struct net_device *dev = priv->ndev;
	int err = 0;

	pda = kzalloc(fw->pda_size, GFP_KERNEL);
	if (!pda)
		return -ENOMEM;

	if (ap)
		firmware = fw->ap_fw;
	else
		firmware = fw->sta_fw;

	printk(KERN_DEBUG "%s: Attempting to download firmware %s\n",
	       dev->name, firmware);

	/* Read current plug data */
	err = hermes_read_pda(hw, pda, fw->pda_addr, fw->pda_size, 0);
	printk(KERN_DEBUG "%s: Read PDA returned %d\n", dev->name, err);
	if (err)
		goto free;

	err = request_firmware(&fw_entry, firmware, priv->dev);
	if (err) {
		printk(KERN_ERR "%s: Cannot find firmware %s\n",
		       dev->name, firmware);
		err = -ENOENT;
		goto free;
	}

	hdr = (const struct orinoco_fw_header *) fw_entry->data;

	/* Enable aux port to allow programming */
	err = hermesi_program_init(hw, le32_to_cpu(hdr->entry_point));
	printk(KERN_DEBUG "%s: Program init returned %d\n", dev->name, err);
	if (err != 0)
		goto abort;

	/* Program data */
	first_block = (fw_entry->data +
		       le16_to_cpu(hdr->headersize) +
		       le32_to_cpu(hdr->block_offset));
	end = fw_entry->data + fw_entry->size;

	err = hermes_program(hw, first_block, end);
	printk(KERN_DEBUG "%s: Program returned %d\n", dev->name, err);
	if (err != 0)
		goto abort;

	/* Update production data */
	first_block = (fw_entry->data +
		       le16_to_cpu(hdr->headersize) +
		       le32_to_cpu(hdr->pdr_offset));

	err = hermes_apply_pda_with_defaults(hw, first_block, pda);
	printk(KERN_DEBUG "%s: Apply PDA returned %d\n", dev->name, err);
	if (err)
		goto abort;

	/* Tell card we've finished */
	err = hermesi_program_end(hw);
	printk(KERN_DEBUG "%s: Program end returned %d\n", dev->name, err);
	if (err != 0)
		goto abort;

	/* Check if we're running */
	printk(KERN_DEBUG "%s: hermes_present returned %d\n",
	       dev->name, hermes_present(hw));

abort:
	release_firmware(fw_entry);

free:
	kfree(pda);
	return err;
}

/* End markers */
#define TEXT_END	0x1A		/* End of text header */

/*
 * Process a firmware image - stop the card, load the firmware, reset
 * the card and make sure it responds.  For the secondary firmware take
 * care of the PDA - read it and then write it on top of the firmware.
 */
static int
symbol_dl_image(struct orinoco_private *priv, const struct fw_info *fw,
		const unsigned char *image, const unsigned char *end,
		int secondary)
{
	hermes_t *hw = &priv->hw;
	int ret = 0;
	const unsigned char *ptr;
	const unsigned char *first_block;

	/* Plug Data Area (PDA) */
	__le16 *pda = NULL;

	/* Binary block begins after the 0x1A marker */
	ptr = image;
	while (*ptr++ != TEXT_END);
	first_block = ptr;

	/* Read the PDA from EEPROM */
	if (secondary) {
		pda = kzalloc(fw->pda_size, GFP_KERNEL);
		if (!pda)
			return -ENOMEM;

		ret = hermes_read_pda(hw, pda, fw->pda_addr, fw->pda_size, 1);
		if (ret)
			goto free;
	}

	/* Stop the firmware, so that it can be safely rewritten */
	if (priv->stop_fw) {
		ret = priv->stop_fw(priv, 1);
		if (ret)
			goto free;
	}

	/* Program the adapter with new firmware */
	ret = hermes_program(hw, first_block, end);
	if (ret)
		goto free;

	/* Write the PDA to the adapter */
	if (secondary) {
		size_t len = hermes_blocks_length(first_block);
		ptr = first_block + len;
		ret = hermes_apply_pda(hw, ptr, pda);
		kfree(pda);
		if (ret)
			return ret;
	}

	/* Run the firmware */
	if (priv->stop_fw) {
		ret = priv->stop_fw(priv, 0);
		if (ret)
			return ret;
	}

	/* Reset hermes chip and make sure it responds */
	ret = hermes_init(hw);

	/* hermes_reset() should return 0 with the secondary firmware */
	if (secondary && ret != 0)
		return -ENODEV;

	/* And this should work with any firmware */
	if (!hermes_present(hw))
		return -ENODEV;

	return 0;

free:
	kfree(pda);
	return ret;
}


/*
 * Download the firmware into the card, this also does a PCMCIA soft
 * reset on the card, to make sure it's in a sane state.
 */
static int
symbol_dl_firmware(struct orinoco_private *priv,
		   const struct fw_info *fw)
{
	struct net_device *dev = priv->ndev;
	int ret;
	const struct firmware *fw_entry;

	if (request_firmware(&fw_entry, fw->pri_fw,
			     priv->dev) != 0) {
		printk(KERN_ERR "%s: Cannot find firmware: %s\n",
		       dev->name, fw->pri_fw);
		return -ENOENT;
	}

	/* Load primary firmware */
	ret = symbol_dl_image(priv, fw, fw_entry->data,
			      fw_entry->data + fw_entry->size, 0);
	release_firmware(fw_entry);
	if (ret) {
		printk(KERN_ERR "%s: Primary firmware download failed\n",
		       dev->name);
		return ret;
	}

	if (request_firmware(&fw_entry, fw->sta_fw,
			     priv->dev) != 0) {
		printk(KERN_ERR "%s: Cannot find firmware: %s\n",
		       dev->name, fw->sta_fw);
		return -ENOENT;
	}

	/* Load secondary firmware */
	ret = symbol_dl_image(priv, fw, fw_entry->data,
			      fw_entry->data + fw_entry->size, 1);
	release_firmware(fw_entry);
	if (ret) {
		printk(KERN_ERR "%s: Secondary firmware download failed\n",
		       dev->name);
	}

	return ret;
}

static int orinoco_download(struct orinoco_private *priv)
{
	int err = 0;
	/* Reload firmware */
	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		/* case FIRMWARE_TYPE_INTERSIL: */
		err = orinoco_dl_firmware(priv,
					  &orinoco_fw[priv->firmware_type], 0);
		break;

	case FIRMWARE_TYPE_SYMBOL:
		err = symbol_dl_firmware(priv,
					 &orinoco_fw[priv->firmware_type]);
		break;
	case FIRMWARE_TYPE_INTERSIL:
		break;
	}
	/* TODO: if we fail we probably need to reinitialise
	 * the driver */

	return err;
}

/********************************************************************/
/* Device methods                                                   */
/********************************************************************/

static int orinoco_open(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;
	int err;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	err = __orinoco_up(dev);

	if (! err)
		priv->open = 1;

	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_stop(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = 0;

	/* We mustn't use orinoco_lock() here, because we need to be
	   able to close the interface even if hw_unavailable is set
	   (e.g. as we're released after a PC Card removal) */
	spin_lock_irq(&priv->lock);

	priv->open = 0;

	err = __orinoco_down(dev);

	spin_unlock_irq(&priv->lock);

	return err;
}

static struct net_device_stats *orinoco_get_stats(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	
	return &priv->stats;
}

static struct iw_statistics *orinoco_get_wireless_stats(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	struct iw_statistics *wstats = &priv->wstats;
	int err;
	unsigned long flags;

	if (! netif_device_present(dev)) {
		printk(KERN_WARNING "%s: get_wireless_stats() called while device not present\n",
		       dev->name);
		return NULL; /* FIXME: Can we do better than this? */
	}

	/* If busy, return the old stats.  Returning NULL may cause
	 * the interface to disappear from /proc/net/wireless */
	if (orinoco_lock(priv, &flags) != 0)
		return wstats;

	/* We can't really wait for the tallies inquiry command to
	 * complete, so we just use the previous results and trigger
	 * a new tallies inquiry command for next time - Jean II */
	/* FIXME: Really we should wait for the inquiry to come back -
	 * as it is the stats we give don't make a whole lot of sense.
	 * Unfortunately, it's not clear how to do that within the
	 * wireless extensions framework: I think we're in user
	 * context, but a lock seems to be held by the time we get in
	 * here so we're not safe to sleep here. */
	hermes_inquire(hw, HERMES_INQ_TALLIES);

	if (priv->iw_mode == IW_MODE_ADHOC) {
		memset(&wstats->qual, 0, sizeof(wstats->qual));
		/* If a spy address is defined, we report stats of the
		 * first spy address - Jean II */
		if (SPY_NUMBER(priv)) {
			wstats->qual.qual = priv->spy_data.spy_stat[0].qual;
			wstats->qual.level = priv->spy_data.spy_stat[0].level;
			wstats->qual.noise = priv->spy_data.spy_stat[0].noise;
			wstats->qual.updated = priv->spy_data.spy_stat[0].updated;
		}
	} else {
		struct {
			__le16 qual, signal, noise, unused;
		} __attribute__ ((packed)) cq;

		err = HERMES_READ_RECORD(hw, USER_BAP,
					 HERMES_RID_COMMSQUALITY, &cq);

		if (!err) {
			wstats->qual.qual = (int)le16_to_cpu(cq.qual);
			wstats->qual.level = (int)le16_to_cpu(cq.signal) - 0x95;
			wstats->qual.noise = (int)le16_to_cpu(cq.noise) - 0x95;
			wstats->qual.updated = 7;
		}
	}

	orinoco_unlock(priv, &flags);
	return wstats;
}

static void orinoco_set_multicast_list(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0) {
		printk(KERN_DEBUG "%s: orinoco_set_multicast_list() "
		       "called when hw_unavailable\n", dev->name);
		return;
	}

	__orinoco_set_multicast_list(dev);
	orinoco_unlock(priv, &flags);
}

static int orinoco_change_mtu(struct net_device *dev, int new_mtu)
{
	struct orinoco_private *priv = netdev_priv(dev);

	if ( (new_mtu < ORINOCO_MIN_MTU) || (new_mtu > ORINOCO_MAX_MTU) )
		return -EINVAL;

	if ( (new_mtu + ENCAPS_OVERHEAD + IEEE80211_HLEN) >
	     (priv->nicbuf_size - ETH_HLEN) )
		return -EINVAL;

	dev->mtu = new_mtu;

	return 0;
}

/********************************************************************/
/* Tx path                                                          */
/********************************************************************/

static int orinoco_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	hermes_t *hw = &priv->hw;
	int err = 0;
	u16 txfid = priv->txfid;
	struct ethhdr *eh;
	int tx_control;
	unsigned long flags;

	if (! netif_running(dev)) {
		printk(KERN_ERR "%s: Tx on stopped device!\n",
		       dev->name);
		return NETDEV_TX_BUSY;
	}
	
	if (netif_queue_stopped(dev)) {
		printk(KERN_DEBUG "%s: Tx while transmitter busy!\n", 
		       dev->name);
		return NETDEV_TX_BUSY;
	}
	
	if (orinoco_lock(priv, &flags) != 0) {
		printk(KERN_ERR "%s: orinoco_xmit() called while hw_unavailable\n",
		       dev->name);
		return NETDEV_TX_BUSY;
	}

	if (! netif_carrier_ok(dev) || (priv->iw_mode == IW_MODE_MONITOR)) {
		/* Oops, the firmware hasn't established a connection,
                   silently drop the packet (this seems to be the
                   safest approach). */
		goto drop;
	}

	/* Check packet length */
	if (skb->len < ETH_HLEN)
		goto drop;

	tx_control = HERMES_TXCTRL_TX_OK | HERMES_TXCTRL_TX_EX;

	if (priv->encode_alg == IW_ENCODE_ALG_TKIP)
		tx_control |= (priv->tx_key << HERMES_MIC_KEY_ID_SHIFT) |
			HERMES_TXCTRL_MIC;

	if (priv->has_alt_txcntl) {
		/* WPA enabled firmwares have tx_cntl at the end of
		 * the 802.11 header.  So write zeroed descriptor and
		 * 802.11 header at the same time
		 */
		char desc[HERMES_802_3_OFFSET];
		__le16 *txcntl = (__le16 *) &desc[HERMES_TXCNTL2_OFFSET];

		memset(&desc, 0, sizeof(desc));

		*txcntl = cpu_to_le16(tx_control);
		err = hermes_bap_pwrite(hw, USER_BAP, &desc, sizeof(desc),
					txfid, 0);
		if (err) {
			if (net_ratelimit())
				printk(KERN_ERR "%s: Error %d writing Tx "
				       "descriptor to BAP\n", dev->name, err);
			goto busy;
		}
	} else {
		struct hermes_tx_descriptor desc;

		memset(&desc, 0, sizeof(desc));

		desc.tx_control = cpu_to_le16(tx_control);
		err = hermes_bap_pwrite(hw, USER_BAP, &desc, sizeof(desc),
					txfid, 0);
		if (err) {
			if (net_ratelimit())
				printk(KERN_ERR "%s: Error %d writing Tx "
				       "descriptor to BAP\n", dev->name, err);
			goto busy;
		}

		/* Clear the 802.11 header and data length fields - some
		 * firmwares (e.g. Lucent/Agere 8.xx) appear to get confused
		 * if this isn't done. */
		hermes_clear_words(hw, HERMES_DATA0,
				   HERMES_802_3_OFFSET - HERMES_802_11_OFFSET);
	}

	eh = (struct ethhdr *)skb->data;

	/* Encapsulate Ethernet-II frames */
	if (ntohs(eh->h_proto) > ETH_DATA_LEN) { /* Ethernet-II frame */
		struct header_struct {
			struct ethhdr eth;	/* 802.3 header */
			u8 encap[6];		/* 802.2 header */
		} __attribute__ ((packed)) hdr;

		/* Strip destination and source from the data */
		skb_pull(skb, 2 * ETH_ALEN);

		/* And move them to a separate header */
		memcpy(&hdr.eth, eh, 2 * ETH_ALEN);
		hdr.eth.h_proto = htons(sizeof(encaps_hdr) + skb->len);
		memcpy(hdr.encap, encaps_hdr, sizeof(encaps_hdr));

		/* Insert the SNAP header */
		if (skb_headroom(skb) < sizeof(hdr)) {
			printk(KERN_ERR
			       "%s: Not enough headroom for 802.2 headers %d\n",
			       dev->name, skb_headroom(skb));
			goto drop;
		}
		eh = (struct ethhdr *) skb_push(skb, sizeof(hdr));
		memcpy(eh, &hdr, sizeof(hdr));
	}

	err = hermes_bap_pwrite(hw, USER_BAP, skb->data, skb->len,
				txfid, HERMES_802_3_OFFSET);
	if (err) {
		printk(KERN_ERR "%s: Error %d writing packet to BAP\n",
		       dev->name, err);
		goto busy;
	}

	/* Calculate Michael MIC */
	if (priv->encode_alg == IW_ENCODE_ALG_TKIP) {
		u8 mic_buf[MICHAEL_MIC_LEN + 1];
		u8 *mic;
		size_t offset;
		size_t len;

		if (skb->len % 2) {
			/* MIC start is on an odd boundary */
			mic_buf[0] = skb->data[skb->len - 1];
			mic = &mic_buf[1];
			offset = skb->len - 1;
			len = MICHAEL_MIC_LEN + 1;
		} else {
			mic = &mic_buf[0];
			offset = skb->len;
			len = MICHAEL_MIC_LEN;
		}

		michael_mic(priv->tx_tfm_mic,
			    priv->tkip_key[priv->tx_key].tx_mic,
			    eh->h_dest, eh->h_source, 0 /* priority */,
			    skb->data + ETH_HLEN, skb->len - ETH_HLEN, mic);

		/* Write the MIC */
		err = hermes_bap_pwrite(hw, USER_BAP, &mic_buf[0], len,
					txfid, HERMES_802_3_OFFSET + offset);
		if (err) {
			printk(KERN_ERR "%s: Error %d writing MIC to BAP\n",
			       dev->name, err);
			goto busy;
		}
	}

	/* Finally, we actually initiate the send */
	netif_stop_queue(dev);

	err = hermes_docmd_wait(hw, HERMES_CMD_TX | HERMES_CMD_RECL,
				txfid, NULL);
	if (err) {
		netif_start_queue(dev);
		if (net_ratelimit())
			printk(KERN_ERR "%s: Error %d transmitting packet\n",
				dev->name, err);
		goto busy;
	}

	dev->trans_start = jiffies;
	stats->tx_bytes += HERMES_802_3_OFFSET + skb->len;
	goto ok;

 drop:
	stats->tx_errors++;
	stats->tx_dropped++;

 ok:
	orinoco_unlock(priv, &flags);
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;

 busy:
	if (err == -EIO)
		schedule_work(&priv->reset_work);
	orinoco_unlock(priv, &flags);
	return NETDEV_TX_BUSY;
}

static void __orinoco_ev_alloc(struct net_device *dev, hermes_t *hw)
{
	struct orinoco_private *priv = netdev_priv(dev);
	u16 fid = hermes_read_regn(hw, ALLOCFID);

	if (fid != priv->txfid) {
		if (fid != DUMMY_FID)
			printk(KERN_WARNING "%s: Allocate event on unexpected fid (%04X)\n",
			       dev->name, fid);
		return;
	}

	hermes_write_regn(hw, ALLOCFID, DUMMY_FID);
}

static void __orinoco_ev_tx(struct net_device *dev, hermes_t *hw)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;

	stats->tx_packets++;

	netif_wake_queue(dev);

	hermes_write_regn(hw, TXCOMPLFID, DUMMY_FID);
}

static void __orinoco_ev_txexc(struct net_device *dev, hermes_t *hw)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	u16 fid = hermes_read_regn(hw, TXCOMPLFID);
	u16 status;
	struct hermes_txexc_data hdr;
	int err = 0;

	if (fid == DUMMY_FID)
		return; /* Nothing's really happened */

	/* Read part of the frame header - we need status and addr1 */
	err = hermes_bap_pread(hw, IRQ_BAP, &hdr,
			       sizeof(struct hermes_txexc_data),
			       fid, 0);

	hermes_write_regn(hw, TXCOMPLFID, DUMMY_FID);
	stats->tx_errors++;

	if (err) {
		printk(KERN_WARNING "%s: Unable to read descriptor on Tx error "
		       "(FID=%04X error %d)\n",
		       dev->name, fid, err);
		return;
	}
	
	DEBUG(1, "%s: Tx error, err %d (FID=%04X)\n", dev->name,
	      err, fid);
    
	/* We produce a TXDROP event only for retry or lifetime
	 * exceeded, because that's the only status that really mean
	 * that this particular node went away.
	 * Other errors means that *we* screwed up. - Jean II */
	status = le16_to_cpu(hdr.desc.status);
	if (status & (HERMES_TXSTAT_RETRYERR | HERMES_TXSTAT_AGEDERR)) {
		union iwreq_data	wrqu;

		/* Copy 802.11 dest address.
		 * We use the 802.11 header because the frame may
		 * not be 802.3 or may be mangled...
		 * In Ad-Hoc mode, it will be the node address.
		 * In managed mode, it will be most likely the AP addr
		 * User space will figure out how to convert it to
		 * whatever it needs (IP address or else).
		 * - Jean II */
		memcpy(wrqu.addr.sa_data, hdr.addr1, ETH_ALEN);
		wrqu.addr.sa_family = ARPHRD_ETHER;

		/* Send event to user space */
		wireless_send_event(dev, IWEVTXDROP, &wrqu, NULL);
	}

	netif_wake_queue(dev);
}

static void orinoco_tx_timeout(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	struct hermes *hw = &priv->hw;

	printk(KERN_WARNING "%s: Tx timeout! "
	       "ALLOCFID=%04x, TXCOMPLFID=%04x, EVSTAT=%04x\n",
	       dev->name, hermes_read_regn(hw, ALLOCFID),
	       hermes_read_regn(hw, TXCOMPLFID), hermes_read_regn(hw, EVSTAT));

	stats->tx_errors++;

	schedule_work(&priv->reset_work);
}

/********************************************************************/
/* Rx path (data frames)                                            */
/********************************************************************/

/* Does the frame have a SNAP header indicating it should be
 * de-encapsulated to Ethernet-II? */
static inline int is_ethersnap(void *_hdr)
{
	u8 *hdr = _hdr;

	/* We de-encapsulate all packets which, a) have SNAP headers
	 * (i.e. SSAP=DSAP=0xaa and CTRL=0x3 in the 802.2 LLC header
	 * and where b) the OUI of the SNAP header is 00:00:00 or
	 * 00:00:f8 - we need both because different APs appear to use
	 * different OUIs for some reason */
	return (memcmp(hdr, &encaps_hdr, 5) == 0)
		&& ( (hdr[5] == 0x00) || (hdr[5] == 0xf8) );
}

static inline void orinoco_spy_gather(struct net_device *dev, u_char *mac,
				      int level, int noise)
{
	struct iw_quality wstats;
	wstats.level = level - 0x95;
	wstats.noise = noise - 0x95;
	wstats.qual = (level > noise) ? (level - noise) : 0;
	wstats.updated = 7;
	/* Update spy records */
	wireless_spy_update(dev, mac, &wstats);
}

static void orinoco_stat_gather(struct net_device *dev,
				struct sk_buff *skb,
				struct hermes_rx_descriptor *desc)
{
	struct orinoco_private *priv = netdev_priv(dev);

	/* Using spy support with lots of Rx packets, like in an
	 * infrastructure (AP), will really slow down everything, because
	 * the MAC address must be compared to each entry of the spy list.
	 * If the user really asks for it (set some address in the
	 * spy list), we do it, but he will pay the price.
	 * Note that to get here, you need both WIRELESS_SPY
	 * compiled in AND some addresses in the list !!!
	 */
	/* Note : gcc will optimise the whole section away if
	 * WIRELESS_SPY is not defined... - Jean II */
	if (SPY_NUMBER(priv)) {
		orinoco_spy_gather(dev, skb_mac_header(skb) + ETH_ALEN,
				   desc->signal, desc->silence);
	}
}

/*
 * orinoco_rx_monitor - handle received monitor frames.
 *
 * Arguments:
 *	dev		network device
 *	rxfid		received FID
 *	desc		rx descriptor of the frame
 *
 * Call context: interrupt
 */
static void orinoco_rx_monitor(struct net_device *dev, u16 rxfid,
			       struct hermes_rx_descriptor *desc)
{
	u32 hdrlen = 30;	/* return full header by default */
	u32 datalen = 0;
	u16 fc;
	int err;
	int len;
	struct sk_buff *skb;
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	hermes_t *hw = &priv->hw;

	len = le16_to_cpu(desc->data_len);

	/* Determine the size of the header and the data */
	fc = le16_to_cpu(desc->frame_ctl);
	switch (fc & IEEE80211_FCTL_FTYPE) {
	case IEEE80211_FTYPE_DATA:
		if ((fc & IEEE80211_FCTL_TODS)
		    && (fc & IEEE80211_FCTL_FROMDS))
			hdrlen = 30;
		else
			hdrlen = 24;
		datalen = len;
		break;
	case IEEE80211_FTYPE_MGMT:
		hdrlen = 24;
		datalen = len;
		break;
	case IEEE80211_FTYPE_CTL:
		switch (fc & IEEE80211_FCTL_STYPE) {
		case IEEE80211_STYPE_PSPOLL:
		case IEEE80211_STYPE_RTS:
		case IEEE80211_STYPE_CFEND:
		case IEEE80211_STYPE_CFENDACK:
			hdrlen = 16;
			break;
		case IEEE80211_STYPE_CTS:
		case IEEE80211_STYPE_ACK:
			hdrlen = 10;
			break;
		}
		break;
	default:
		/* Unknown frame type */
		break;
	}

	/* sanity check the length */
	if (datalen > IEEE80211_DATA_LEN + 12) {
		printk(KERN_DEBUG "%s: oversized monitor frame, "
		       "data length = %d\n", dev->name, datalen);
		stats->rx_length_errors++;
		goto update_stats;
	}

	skb = dev_alloc_skb(hdrlen + datalen);
	if (!skb) {
		printk(KERN_WARNING "%s: Cannot allocate skb for monitor frame\n",
		       dev->name);
		goto update_stats;
	}

	/* Copy the 802.11 header to the skb */
	memcpy(skb_put(skb, hdrlen), &(desc->frame_ctl), hdrlen);
	skb_reset_mac_header(skb);

	/* If any, copy the data from the card to the skb */
	if (datalen > 0) {
		err = hermes_bap_pread(hw, IRQ_BAP, skb_put(skb, datalen),
				       ALIGN(datalen, 2), rxfid,
				       HERMES_802_2_OFFSET);
		if (err) {
			printk(KERN_ERR "%s: error %d reading monitor frame\n",
			       dev->name, err);
			goto drop;
		}
	}

	skb->dev = dev;
	skb->ip_summed = CHECKSUM_NONE;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = __constant_htons(ETH_P_802_2);
	
	dev->last_rx = jiffies;
	stats->rx_packets++;
	stats->rx_bytes += skb->len;

	netif_rx(skb);
	return;

 drop:
	dev_kfree_skb_irq(skb);
 update_stats:
	stats->rx_errors++;
	stats->rx_dropped++;
}

/* Get tsc from the firmware */
static int orinoco_hw_get_tkip_iv(struct orinoco_private *priv, int key,
				  u8 *tsc)
{
	hermes_t *hw = &priv->hw;
	int err = 0;
	u8 tsc_arr[4][IW_ENCODE_SEQ_MAX_SIZE];

	if ((key < 0) || (key > 4))
		return -EINVAL;

	err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_CURRENT_TKIP_IV,
			      sizeof(tsc_arr), NULL, &tsc_arr);
	if (!err)
		memcpy(tsc, &tsc_arr[key][0], sizeof(tsc_arr[0]));

	return err;
}

static void __orinoco_ev_rx(struct net_device *dev, hermes_t *hw)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	struct iw_statistics *wstats = &priv->wstats;
	struct sk_buff *skb = NULL;
	u16 rxfid, status;
	int length;
	struct hermes_rx_descriptor *desc;
	struct orinoco_rx_data *rx_data;
	int err;

	desc = kmalloc(sizeof(*desc), GFP_ATOMIC);
	if (!desc) {
		printk(KERN_WARNING
		       "%s: Can't allocate space for RX descriptor\n",
		       dev->name);
		goto update_stats;
	}

	rxfid = hermes_read_regn(hw, RXFID);

	err = hermes_bap_pread(hw, IRQ_BAP, desc, sizeof(*desc),
			       rxfid, 0);
	if (err) {
		printk(KERN_ERR "%s: error %d reading Rx descriptor. "
		       "Frame dropped.\n", dev->name, err);
		goto update_stats;
	}

	status = le16_to_cpu(desc->status);

	if (status & HERMES_RXSTAT_BADCRC) {
		DEBUG(1, "%s: Bad CRC on Rx. Frame dropped.\n",
		      dev->name);
		stats->rx_crc_errors++;
		goto update_stats;
	}

	/* Handle frames in monitor mode */
	if (priv->iw_mode == IW_MODE_MONITOR) {
		orinoco_rx_monitor(dev, rxfid, desc);
		goto out;
	}

	if (status & HERMES_RXSTAT_UNDECRYPTABLE) {
		DEBUG(1, "%s: Undecryptable frame on Rx. Frame dropped.\n",
		      dev->name);
		wstats->discard.code++;
		goto update_stats;
	}

	length = le16_to_cpu(desc->data_len);

	/* Sanity checks */
	if (length < 3) { /* No for even an 802.2 LLC header */
		/* At least on Symbol firmware with PCF we get quite a
                   lot of these legitimately - Poll frames with no
                   data. */
		goto out;
	}
	if (length > IEEE80211_DATA_LEN) {
		printk(KERN_WARNING "%s: Oversized frame received (%d bytes)\n",
		       dev->name, length);
		stats->rx_length_errors++;
		goto update_stats;
	}

	/* Payload size does not include Michael MIC. Increase payload
	 * size to read it together with the data. */
	if (status & HERMES_RXSTAT_MIC)
		length += MICHAEL_MIC_LEN;

	/* We need space for the packet data itself, plus an ethernet
	   header, plus 2 bytes so we can align the IP header on a
	   32bit boundary, plus 1 byte so we can read in odd length
	   packets from the card, which has an IO granularity of 16
	   bits */  
	skb = dev_alloc_skb(length+ETH_HLEN+2+1);
	if (!skb) {
		printk(KERN_WARNING "%s: Can't allocate skb for Rx\n",
		       dev->name);
		goto update_stats;
	}

	/* We'll prepend the header, so reserve space for it.  The worst
	   case is no decapsulation, when 802.3 header is prepended and
	   nothing is removed.  2 is for aligning the IP header.  */
	skb_reserve(skb, ETH_HLEN + 2);

	err = hermes_bap_pread(hw, IRQ_BAP, skb_put(skb, length),
			       ALIGN(length, 2), rxfid,
			       HERMES_802_2_OFFSET);
	if (err) {
		printk(KERN_ERR "%s: error %d reading frame. "
		       "Frame dropped.\n", dev->name, err);
		goto drop;
	}

	/* Add desc and skb to rx queue */
	rx_data = kzalloc(sizeof(*rx_data), GFP_ATOMIC);
	if (!rx_data) {
		printk(KERN_WARNING "%s: Can't allocate RX packet\n",
			dev->name);
		goto drop;
	}
	rx_data->desc = desc;
	rx_data->skb = skb;
	list_add_tail(&rx_data->list, &priv->rx_list);
	tasklet_schedule(&priv->rx_tasklet);

	return;

drop:
	dev_kfree_skb_irq(skb);
update_stats:
	stats->rx_errors++;
	stats->rx_dropped++;
out:
	kfree(desc);
}

static void orinoco_rx(struct net_device *dev,
		       struct hermes_rx_descriptor *desc,
		       struct sk_buff *skb)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	u16 status, fc;
	int length;
	struct ethhdr *hdr;

	status = le16_to_cpu(desc->status);
	length = le16_to_cpu(desc->data_len);
	fc = le16_to_cpu(desc->frame_ctl);

	/* Calculate and check MIC */
	if (status & HERMES_RXSTAT_MIC) {
		int key_id = ((status & HERMES_RXSTAT_MIC_KEY_ID) >>
			      HERMES_MIC_KEY_ID_SHIFT);
		u8 mic[MICHAEL_MIC_LEN];
		u8 *rxmic;
		u8 *src = (fc & IEEE80211_FCTL_FROMDS) ?
			desc->addr3 : desc->addr2;

		/* Extract Michael MIC from payload */
		rxmic = skb->data + skb->len - MICHAEL_MIC_LEN;

		skb_trim(skb, skb->len - MICHAEL_MIC_LEN);
		length -= MICHAEL_MIC_LEN;

		michael_mic(priv->rx_tfm_mic,
			    priv->tkip_key[key_id].rx_mic,
			    desc->addr1,
			    src,
			    0, /* priority or QoS? */
			    skb->data,
			    skb->len,
			    &mic[0]);

		if (memcmp(mic, rxmic,
			   MICHAEL_MIC_LEN)) {
			union iwreq_data wrqu;
			struct iw_michaelmicfailure wxmic;
			DECLARE_MAC_BUF(mac);

			printk(KERN_WARNING "%s: "
			       "Invalid Michael MIC in data frame from %s, "
			       "using key %i\n",
			       dev->name, print_mac(mac, src), key_id);

			/* TODO: update stats */

			/* Notify userspace */
			memset(&wxmic, 0, sizeof(wxmic));
			wxmic.flags = key_id & IW_MICFAILURE_KEY_ID;
			wxmic.flags |= (desc->addr1[0] & 1) ?
				IW_MICFAILURE_GROUP : IW_MICFAILURE_PAIRWISE;
			wxmic.src_addr.sa_family = ARPHRD_ETHER;
			memcpy(wxmic.src_addr.sa_data, src, ETH_ALEN);

			(void) orinoco_hw_get_tkip_iv(priv, key_id,
						      &wxmic.tsc[0]);

			memset(&wrqu, 0, sizeof(wrqu));
			wrqu.data.length = sizeof(wxmic);
			wireless_send_event(dev, IWEVMICHAELMICFAILURE, &wrqu,
					    (char *) &wxmic);

			goto drop;
		}
	}

	/* Handle decapsulation
	 * In most cases, the firmware tell us about SNAP frames.
	 * For some reason, the SNAP frames sent by LinkSys APs
	 * are not properly recognised by most firmwares.
	 * So, check ourselves */
	if (length >= ENCAPS_OVERHEAD &&
	    (((status & HERMES_RXSTAT_MSGTYPE) == HERMES_RXSTAT_1042) ||
	     ((status & HERMES_RXSTAT_MSGTYPE) == HERMES_RXSTAT_TUNNEL) ||
	     is_ethersnap(skb->data))) {
		/* These indicate a SNAP within 802.2 LLC within
		   802.11 frame which we'll need to de-encapsulate to
		   the original EthernetII frame. */
		hdr = (struct ethhdr *)skb_push(skb, ETH_HLEN - ENCAPS_OVERHEAD);
	} else {
		/* 802.3 frame - prepend 802.3 header as is */
		hdr = (struct ethhdr *)skb_push(skb, ETH_HLEN);
		hdr->h_proto = htons(length);
	}
	memcpy(hdr->h_dest, desc->addr1, ETH_ALEN);
	if (fc & IEEE80211_FCTL_FROMDS)
		memcpy(hdr->h_source, desc->addr3, ETH_ALEN);
	else
		memcpy(hdr->h_source, desc->addr2, ETH_ALEN);

	dev->last_rx = jiffies;
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_NONE;
	if (fc & IEEE80211_FCTL_TODS)
		skb->pkt_type = PACKET_OTHERHOST;
	
	/* Process the wireless stats if needed */
	orinoco_stat_gather(dev, skb, desc);

	/* Pass the packet to the networking stack */
	netif_rx(skb);
	stats->rx_packets++;
	stats->rx_bytes += length;

	return;

 drop:
	dev_kfree_skb(skb);
	stats->rx_errors++;
	stats->rx_dropped++;
}

static void orinoco_rx_isr_tasklet(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	struct orinoco_private *priv = netdev_priv(dev);
	struct orinoco_rx_data *rx_data, *temp;
	struct hermes_rx_descriptor *desc;
	struct sk_buff *skb;

	/* extract desc and skb from queue */
	list_for_each_entry_safe(rx_data, temp, &priv->rx_list, list) {
		desc = rx_data->desc;
		skb = rx_data->skb;
		list_del(&rx_data->list);
		kfree(rx_data);

		orinoco_rx(dev, desc, skb);

		kfree(desc);
	}
}

/********************************************************************/
/* Rx path (info frames)                                            */
/********************************************************************/

static void print_linkstatus(struct net_device *dev, u16 status)
{
	char * s;

	if (suppress_linkstatus)
		return;

	switch (status) {
	case HERMES_LINKSTATUS_NOT_CONNECTED:
		s = "Not Connected";
		break;
	case HERMES_LINKSTATUS_CONNECTED:
		s = "Connected";
		break;
	case HERMES_LINKSTATUS_DISCONNECTED:
		s = "Disconnected";
		break;
	case HERMES_LINKSTATUS_AP_CHANGE:
		s = "AP Changed";
		break;
	case HERMES_LINKSTATUS_AP_OUT_OF_RANGE:
		s = "AP Out of Range";
		break;
	case HERMES_LINKSTATUS_AP_IN_RANGE:
		s = "AP In Range";
		break;
	case HERMES_LINKSTATUS_ASSOC_FAILED:
		s = "Association Failed";
		break;
	default:
		s = "UNKNOWN";
	}
	
	printk(KERN_INFO "%s: New link status: %s (%04x)\n",
	       dev->name, s, status);
}

/* Search scan results for requested BSSID, join it if found */
static void orinoco_join_ap(struct work_struct *work)
{
	struct orinoco_private *priv =
		container_of(work, struct orinoco_private, join_work);
	struct net_device *dev = priv->ndev;
	struct hermes *hw = &priv->hw;
	int err;
	unsigned long flags;
	struct join_req {
		u8 bssid[ETH_ALEN];
		__le16 channel;
	} __attribute__ ((packed)) req;
	const int atom_len = offsetof(struct prism2_scan_apinfo, atim);
	struct prism2_scan_apinfo *atom = NULL;
	int offset = 4;
	int found = 0;
	u8 *buf;
	u16 len;

	/* Allocate buffer for scan results */
	buf = kmalloc(MAX_SCAN_LEN, GFP_KERNEL);
	if (! buf)
		return;

	if (orinoco_lock(priv, &flags) != 0)
		goto fail_lock;

	/* Sanity checks in case user changed something in the meantime */
	if (! priv->bssid_fixed)
		goto out;

	if (strlen(priv->desired_essid) == 0)
		goto out;

	/* Read scan results from the firmware */
	err = hermes_read_ltv(hw, USER_BAP,
			      HERMES_RID_SCANRESULTSTABLE,
			      MAX_SCAN_LEN, &len, buf);
	if (err) {
		printk(KERN_ERR "%s: Cannot read scan results\n",
		       dev->name);
		goto out;
	}

	len = HERMES_RECLEN_TO_BYTES(len);

	/* Go through the scan results looking for the channel of the AP
	 * we were requested to join */
	for (; offset + atom_len <= len; offset += atom_len) {
		atom = (struct prism2_scan_apinfo *) (buf + offset);
		if (memcmp(&atom->bssid, priv->desired_bssid, ETH_ALEN) == 0) {
			found = 1;
			break;
		}
	}

	if (! found) {
		DEBUG(1, "%s: Requested AP not found in scan results\n",
		      dev->name);
		goto out;
	}

	memcpy(req.bssid, priv->desired_bssid, ETH_ALEN);
	req.channel = atom->channel;	/* both are little-endian */
	err = HERMES_WRITE_RECORD(hw, USER_BAP, HERMES_RID_CNFJOINREQUEST,
				  &req);
	if (err)
		printk(KERN_ERR "%s: Error issuing join request\n", dev->name);

 out:
	orinoco_unlock(priv, &flags);

 fail_lock:
	kfree(buf);
}

/* Send new BSSID to userspace */
static void orinoco_send_bssid_wevent(struct orinoco_private *priv)
{
	struct net_device *dev = priv->ndev;
	struct hermes *hw = &priv->hw;
	union iwreq_data wrqu;
	int err;

	err = hermes_read_ltv(hw, IRQ_BAP, HERMES_RID_CURRENTBSSID,
			      ETH_ALEN, NULL, wrqu.ap_addr.sa_data);
	if (err != 0)
		return;

	wrqu.ap_addr.sa_family = ARPHRD_ETHER;

	/* Send event to user space */
	wireless_send_event(dev, SIOCGIWAP, &wrqu, NULL);
}

static void orinoco_send_assocreqie_wevent(struct orinoco_private *priv)
{
	struct net_device *dev = priv->ndev;
	struct hermes *hw = &priv->hw;
	union iwreq_data wrqu;
	int err;
	u8 buf[88];
	u8 *ie;

	if (!priv->has_wpa)
		return;

	err = hermes_read_ltv(hw, IRQ_BAP, HERMES_RID_CURRENT_ASSOC_REQ_INFO,
			      sizeof(buf), NULL, &buf);
	if (err != 0)
		return;

	ie = orinoco_get_wpa_ie(buf, sizeof(buf));
	if (ie) {
		int rem = sizeof(buf) - (ie - &buf[0]);
		wrqu.data.length = ie[1] + 2;
		if (wrqu.data.length > rem)
			wrqu.data.length = rem;

		if (wrqu.data.length)
			/* Send event to user space */
			wireless_send_event(dev, IWEVASSOCREQIE, &wrqu, ie);
	}
}

static void orinoco_send_assocrespie_wevent(struct orinoco_private *priv)
{
	struct net_device *dev = priv->ndev;
	struct hermes *hw = &priv->hw;
	union iwreq_data wrqu;
	int err;
	u8 buf[88]; /* TODO: verify max size or IW_GENERIC_IE_MAX */
	u8 *ie;

	if (!priv->has_wpa)
		return;

	err = hermes_read_ltv(hw, IRQ_BAP, HERMES_RID_CURRENT_ASSOC_RESP_INFO,
			      sizeof(buf), NULL, &buf);
	if (err != 0)
		return;

	ie = orinoco_get_wpa_ie(buf, sizeof(buf));
	if (ie) {
		int rem = sizeof(buf) - (ie - &buf[0]);
		wrqu.data.length = ie[1] + 2;
		if (wrqu.data.length > rem)
			wrqu.data.length = rem;

		if (wrqu.data.length)
			/* Send event to user space */
			wireless_send_event(dev, IWEVASSOCRESPIE, &wrqu, ie);
	}
}

static void orinoco_send_wevents(struct work_struct *work)
{
	struct orinoco_private *priv =
		container_of(work, struct orinoco_private, wevent_work);
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return;

	orinoco_send_assocreqie_wevent(priv);
	orinoco_send_assocrespie_wevent(priv);
	orinoco_send_bssid_wevent(priv);

	orinoco_unlock(priv, &flags);
}

static inline void orinoco_clear_scan_results(struct orinoco_private *priv,
					      unsigned long scan_age)
{
	if (priv->has_ext_scan) {
		struct xbss_element *bss;
		struct xbss_element *tmp_bss;

		/* Blow away current list of scan results */
		list_for_each_entry_safe(bss, tmp_bss, &priv->bss_list, list) {
			if (!scan_age ||
			    time_after(jiffies, bss->last_scanned + scan_age)) {
				list_move_tail(&bss->list,
					       &priv->bss_free_list);
				/* Don't blow away ->list, just BSS data */
				memset(&bss->bss, 0, sizeof(bss->bss));
				bss->last_scanned = 0;
			}
		}
	} else {
		struct bss_element *bss;
		struct bss_element *tmp_bss;

		/* Blow away current list of scan results */
		list_for_each_entry_safe(bss, tmp_bss, &priv->bss_list, list) {
			if (!scan_age ||
			    time_after(jiffies, bss->last_scanned + scan_age)) {
				list_move_tail(&bss->list,
					       &priv->bss_free_list);
				/* Don't blow away ->list, just BSS data */
				memset(&bss->bss, 0, sizeof(bss->bss));
				bss->last_scanned = 0;
			}
		}
	}
}

static void orinoco_add_ext_scan_result(struct orinoco_private *priv,
					struct agere_ext_scan_info *atom)
{
	struct xbss_element *bss = NULL;
	int found = 0;

	/* Try to update an existing bss first */
	list_for_each_entry(bss, &priv->bss_list, list) {
		if (compare_ether_addr(bss->bss.bssid, atom->bssid))
			continue;
		/* ESSID lengths */
		if (bss->bss.data[1] != atom->data[1])
			continue;
		if (memcmp(&bss->bss.data[2], &atom->data[2],
			   atom->data[1]))
			continue;
		found = 1;
		break;
	}

	/* Grab a bss off the free list */
	if (!found && !list_empty(&priv->bss_free_list)) {
		bss = list_entry(priv->bss_free_list.next,
				 struct xbss_element, list);
		list_del(priv->bss_free_list.next);

		list_add_tail(&bss->list, &priv->bss_list);
	}

	if (bss) {
		/* Always update the BSS to get latest beacon info */
		memcpy(&bss->bss, atom, sizeof(bss->bss));
		bss->last_scanned = jiffies;
	}
}

static int orinoco_process_scan_results(struct net_device *dev,
					unsigned char *buf,
					int len)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int			offset;		/* In the scan data */
	union hermes_scan_info *atom;
	int			atom_len;

	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		atom_len = sizeof(struct agere_scan_apinfo);
		offset = 0;
		break;
	case FIRMWARE_TYPE_SYMBOL:
		/* Lack of documentation necessitates this hack.
		 * Different firmwares have 68 or 76 byte long atoms.
		 * We try modulo first.  If the length divides by both,
		 * we check what would be the channel in the second
		 * frame for a 68-byte atom.  76-byte atoms have 0 there.
		 * Valid channel cannot be 0.  */
		if (len % 76)
			atom_len = 68;
		else if (len % 68)
			atom_len = 76;
		else if (len >= 1292 && buf[68] == 0)
			atom_len = 76;
		else
			atom_len = 68;
		offset = 0;
		break;
	case FIRMWARE_TYPE_INTERSIL:
		offset = 4;
		if (priv->has_hostscan) {
			atom_len = le16_to_cpup((__le16 *)buf);
			/* Sanity check for atom_len */
			if (atom_len < sizeof(struct prism2_scan_apinfo)) {
				printk(KERN_ERR "%s: Invalid atom_len in scan "
				       "data: %d\n", dev->name, atom_len);
				return -EIO;
			}
		} else
			atom_len = offsetof(struct prism2_scan_apinfo, atim);
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Check that we got an whole number of atoms */
	if ((len - offset) % atom_len) {
		printk(KERN_ERR "%s: Unexpected scan data length %d, "
		       "atom_len %d, offset %d\n", dev->name, len,
		       atom_len, offset);
		return -EIO;
	}

	orinoco_clear_scan_results(priv, msecs_to_jiffies(15000));

	/* Read the entries one by one */
	for (; offset + atom_len <= len; offset += atom_len) {
		int found = 0;
		struct bss_element *bss = NULL;

		/* Get next atom */
		atom = (union hermes_scan_info *) (buf + offset);

		/* Try to update an existing bss first */
		list_for_each_entry(bss, &priv->bss_list, list) {
			if (compare_ether_addr(bss->bss.a.bssid, atom->a.bssid))
				continue;
			if (le16_to_cpu(bss->bss.a.essid_len) !=
			      le16_to_cpu(atom->a.essid_len))
				continue;
			if (memcmp(bss->bss.a.essid, atom->a.essid,
			      le16_to_cpu(atom->a.essid_len)))
				continue;
			found = 1;
			break;
		}

		/* Grab a bss off the free list */
		if (!found && !list_empty(&priv->bss_free_list)) {
			bss = list_entry(priv->bss_free_list.next,
					 struct bss_element, list);
			list_del(priv->bss_free_list.next);

			list_add_tail(&bss->list, &priv->bss_list);
		}

		if (bss) {
			/* Always update the BSS to get latest beacon info */
			memcpy(&bss->bss, atom, sizeof(bss->bss));
			bss->last_scanned = jiffies;
		}
	}

	return 0;
}

static void __orinoco_ev_info(struct net_device *dev, hermes_t *hw)
{
	struct orinoco_private *priv = netdev_priv(dev);
	u16 infofid;
	struct {
		__le16 len;
		__le16 type;
	} __attribute__ ((packed)) info;
	int len, type;
	int err;

	/* This is an answer to an INQUIRE command that we did earlier,
	 * or an information "event" generated by the card
	 * The controller return to us a pseudo frame containing
	 * the information in question - Jean II */
	infofid = hermes_read_regn(hw, INFOFID);

	/* Read the info frame header - don't try too hard */
	err = hermes_bap_pread(hw, IRQ_BAP, &info, sizeof(info),
			       infofid, 0);
	if (err) {
		printk(KERN_ERR "%s: error %d reading info frame. "
		       "Frame dropped.\n", dev->name, err);
		return;
	}
	
	len = HERMES_RECLEN_TO_BYTES(le16_to_cpu(info.len));
	type = le16_to_cpu(info.type);

	switch (type) {
	case HERMES_INQ_TALLIES: {
		struct hermes_tallies_frame tallies;
		struct iw_statistics *wstats = &priv->wstats;
		
		if (len > sizeof(tallies)) {
			printk(KERN_WARNING "%s: Tallies frame too long (%d bytes)\n",
			       dev->name, len);
			len = sizeof(tallies);
		}
		
		err = hermes_bap_pread(hw, IRQ_BAP, &tallies, len,
				       infofid, sizeof(info));
		if (err)
			break;
		
		/* Increment our various counters */
		/* wstats->discard.nwid - no wrong BSSID stuff */
		wstats->discard.code +=
			le16_to_cpu(tallies.RxWEPUndecryptable);
		if (len == sizeof(tallies))  
			wstats->discard.code +=
				le16_to_cpu(tallies.RxDiscards_WEPICVError) +
				le16_to_cpu(tallies.RxDiscards_WEPExcluded);
		wstats->discard.misc +=
			le16_to_cpu(tallies.TxDiscardsWrongSA);
		wstats->discard.fragment +=
			le16_to_cpu(tallies.RxMsgInBadMsgFragments);
		wstats->discard.retries +=
			le16_to_cpu(tallies.TxRetryLimitExceeded);
		/* wstats->miss.beacon - no match */
	}
	break;
	case HERMES_INQ_LINKSTATUS: {
		struct hermes_linkstatus linkstatus;
		u16 newstatus;
		int connected;

		if (priv->iw_mode == IW_MODE_MONITOR)
			break;

		if (len != sizeof(linkstatus)) {
			printk(KERN_WARNING "%s: Unexpected size for linkstatus frame (%d bytes)\n",
			       dev->name, len);
			break;
		}

		err = hermes_bap_pread(hw, IRQ_BAP, &linkstatus, len,
				       infofid, sizeof(info));
		if (err)
			break;
		newstatus = le16_to_cpu(linkstatus.linkstatus);

		/* Symbol firmware uses "out of range" to signal that
		 * the hostscan frame can be requested.  */
		if (newstatus == HERMES_LINKSTATUS_AP_OUT_OF_RANGE &&
		    priv->firmware_type == FIRMWARE_TYPE_SYMBOL &&
		    priv->has_hostscan && priv->scan_inprogress) {
			hermes_inquire(hw, HERMES_INQ_HOSTSCAN_SYMBOL);
			break;
		}

		connected = (newstatus == HERMES_LINKSTATUS_CONNECTED)
			|| (newstatus == HERMES_LINKSTATUS_AP_CHANGE)
			|| (newstatus == HERMES_LINKSTATUS_AP_IN_RANGE);

		if (connected)
			netif_carrier_on(dev);
		else if (!ignore_disconnect)
			netif_carrier_off(dev);

		if (newstatus != priv->last_linkstatus) {
			priv->last_linkstatus = newstatus;
			print_linkstatus(dev, newstatus);
			/* The info frame contains only one word which is the
			 * status (see hermes.h). The status is pretty boring
			 * in itself, that's why we export the new BSSID...
			 * Jean II */
			schedule_work(&priv->wevent_work);
		}
	}
	break;
	case HERMES_INQ_SCAN:
		if (!priv->scan_inprogress && priv->bssid_fixed &&
		    priv->firmware_type == FIRMWARE_TYPE_INTERSIL) {
			schedule_work(&priv->join_work);
			break;
		}
		/* fall through */
	case HERMES_INQ_HOSTSCAN:
	case HERMES_INQ_HOSTSCAN_SYMBOL: {
		/* Result of a scanning. Contains information about
		 * cells in the vicinity - Jean II */
		union iwreq_data	wrqu;
		unsigned char *buf;

		/* Scan is no longer in progress */
		priv->scan_inprogress = 0;

		/* Sanity check */
		if (len > 4096) {
			printk(KERN_WARNING "%s: Scan results too large (%d bytes)\n",
			       dev->name, len);
			break;
		}

		/* Allocate buffer for results */
		buf = kmalloc(len, GFP_ATOMIC);
		if (buf == NULL)
			/* No memory, so can't printk()... */
			break;

		/* Read scan data */
		err = hermes_bap_pread(hw, IRQ_BAP, (void *) buf, len,
				       infofid, sizeof(info));
		if (err) {
			kfree(buf);
			break;
		}

#ifdef ORINOCO_DEBUG
		{
			int	i;
			printk(KERN_DEBUG "Scan result [%02X", buf[0]);
			for(i = 1; i < (len * 2); i++)
				printk(":%02X", buf[i]);
			printk("]\n");
		}
#endif	/* ORINOCO_DEBUG */

		if (orinoco_process_scan_results(dev, buf, len) == 0) {
			/* Send an empty event to user space.
			 * We don't send the received data on the event because
			 * it would require us to do complex transcoding, and
			 * we want to minimise the work done in the irq handler
			 * Use a request to extract the data - Jean II */
			wrqu.data.length = 0;
			wrqu.data.flags = 0;
			wireless_send_event(dev, SIOCGIWSCAN, &wrqu, NULL);
		}
		kfree(buf);
	}
	break;
	case HERMES_INQ_CHANNELINFO:
	{
		struct agere_ext_scan_info *bss;

		if (!priv->scan_inprogress) {
			printk(KERN_DEBUG "%s: Got chaninfo without scan, "
			       "len=%d\n", dev->name, len);
			break;
		}

		/* An empty result indicates that the scan is complete */
		if (len == 0) {
			union iwreq_data	wrqu;

			/* Scan is no longer in progress */
			priv->scan_inprogress = 0;

			wrqu.data.length = 0;
			wrqu.data.flags = 0;
			wireless_send_event(dev, SIOCGIWSCAN, &wrqu, NULL);
			break;
		}

		/* Sanity check */
		else if (len > sizeof(*bss)) {
			printk(KERN_WARNING
			       "%s: Ext scan results too large (%d bytes). "
			       "Truncating results to %zd bytes.\n",
			       dev->name, len, sizeof(*bss));
			len = sizeof(*bss);
		} else if (len < (offsetof(struct agere_ext_scan_info,
					   data) + 2)) {
			/* Drop this result now so we don't have to
			 * keep checking later */
			printk(KERN_WARNING
			       "%s: Ext scan results too short (%d bytes)\n",
			       dev->name, len);
			break;
		}

		bss = kmalloc(sizeof(*bss), GFP_ATOMIC);
		if (bss == NULL)
			break;

		/* Read scan data */
		err = hermes_bap_pread(hw, IRQ_BAP, (void *) bss, len,
				       infofid, sizeof(info));
		if (err) {
			kfree(bss);
			break;
		}

		orinoco_add_ext_scan_result(priv, bss);

		kfree(bss);
		break;
	}
	case HERMES_INQ_SEC_STAT_AGERE:
		/* Security status (Agere specific) */
		/* Ignore this frame for now */
		if (priv->firmware_type == FIRMWARE_TYPE_AGERE)
			break;
		/* fall through */
	default:
		printk(KERN_DEBUG "%s: Unknown information frame received: "
		       "type 0x%04x, length %d\n", dev->name, type, len);
		/* We don't actually do anything about it */
		break;
	}
}

static void __orinoco_ev_infdrop(struct net_device *dev, hermes_t *hw)
{
	if (net_ratelimit())
		printk(KERN_DEBUG "%s: Information frame lost.\n", dev->name);
}

/********************************************************************/
/* Internal hardware control routines                               */
/********************************************************************/

int __orinoco_up(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct hermes *hw = &priv->hw;
	int err;

	netif_carrier_off(dev); /* just to make sure */

	err = __orinoco_program_rids(dev);
	if (err) {
		printk(KERN_ERR "%s: Error %d configuring card\n",
		       dev->name, err);
		return err;
	}

	/* Fire things up again */
	hermes_set_irqmask(hw, ORINOCO_INTEN);
	err = hermes_enable_port(hw, 0);
	if (err) {
		printk(KERN_ERR "%s: Error %d enabling MAC port\n",
		       dev->name, err);
		return err;
	}

	netif_start_queue(dev);

	return 0;
}

int __orinoco_down(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct hermes *hw = &priv->hw;
	int err;

	netif_stop_queue(dev);

	if (! priv->hw_unavailable) {
		if (! priv->broken_disableport) {
			err = hermes_disable_port(hw, 0);
			if (err) {
				/* Some firmwares (e.g. Intersil 1.3.x) seem
				 * to have problems disabling the port, oh
				 * well, too bad. */
				printk(KERN_WARNING "%s: Error %d disabling MAC port\n",
				       dev->name, err);
				priv->broken_disableport = 1;
			}
		}
		hermes_set_irqmask(hw, 0);
		hermes_write_regn(hw, EVACK, 0xffff);
	}
	
	/* firmware will have to reassociate */
	netif_carrier_off(dev);
	priv->last_linkstatus = 0xffff;

	return 0;
}

static int orinoco_allocate_fid(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct hermes *hw = &priv->hw;
	int err;

	err = hermes_allocate(hw, priv->nicbuf_size, &priv->txfid);
	if (err == -EIO && priv->nicbuf_size > TX_NICBUF_SIZE_BUG) {
		/* Try workaround for old Symbol firmware bug */
		printk(KERN_WARNING "%s: firmware ALLOC bug detected "
		       "(old Symbol firmware?). Trying to work around... ",
		       dev->name);
		
		priv->nicbuf_size = TX_NICBUF_SIZE_BUG;
		err = hermes_allocate(hw, priv->nicbuf_size, &priv->txfid);
		if (err)
			printk("failed!\n");
		else
			printk("ok.\n");
	}

	return err;
}

int orinoco_reinit_firmware(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct hermes *hw = &priv->hw;
	int err;

	err = hermes_init(hw);
	if (!err)
		err = orinoco_allocate_fid(dev);

	return err;
}

static int __orinoco_hw_set_bitrate(struct orinoco_private *priv)
{
	hermes_t *hw = &priv->hw;
	int err = 0;

	if (priv->bitratemode >= BITRATE_TABLE_SIZE) {
		printk(KERN_ERR "%s: BUG: Invalid bitrate mode %d\n",
		       priv->ndev->name, priv->bitratemode);
		return -EINVAL;
	}

	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFTXRATECONTROL,
					   bitrate_table[priv->bitratemode].agere_txratectrl);
		break;
	case FIRMWARE_TYPE_INTERSIL:
	case FIRMWARE_TYPE_SYMBOL:
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFTXRATECONTROL,
					   bitrate_table[priv->bitratemode].intersil_txratectrl);
		break;
	default:
		BUG();
	}

	return err;
}

/* Set fixed AP address */
static int __orinoco_hw_set_wap(struct orinoco_private *priv)
{
	int roaming_flag;
	int err = 0;
	hermes_t *hw = &priv->hw;

	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		/* not supported */
		break;
	case FIRMWARE_TYPE_INTERSIL:
		if (priv->bssid_fixed)
			roaming_flag = 2;
		else
			roaming_flag = 1;

		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFROAMINGMODE,
					   roaming_flag);
		break;
	case FIRMWARE_TYPE_SYMBOL:
		err = HERMES_WRITE_RECORD(hw, USER_BAP,
					  HERMES_RID_CNFMANDATORYBSSID_SYMBOL,
					  &priv->desired_bssid);
		break;
	}
	return err;
}

/* Change the WEP keys and/or the current keys.  Can be called
 * either from __orinoco_hw_setup_enc() or directly from
 * orinoco_ioctl_setiwencode().  In the later case the association
 * with the AP is not broken (if the firmware can handle it),
 * which is needed for 802.1x implementations. */
static int __orinoco_hw_setup_wepkeys(struct orinoco_private *priv)
{
	hermes_t *hw = &priv->hw;
	int err = 0;

	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		err = HERMES_WRITE_RECORD(hw, USER_BAP,
					  HERMES_RID_CNFWEPKEYS_AGERE,
					  &priv->keys);
		if (err)
			return err;
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFTXKEY_AGERE,
					   priv->tx_key);
		if (err)
			return err;
		break;
	case FIRMWARE_TYPE_INTERSIL:
	case FIRMWARE_TYPE_SYMBOL:
		{
			int keylen;
			int i;

			/* Force uniform key length to work around firmware bugs */
			keylen = le16_to_cpu(priv->keys[priv->tx_key].len);
			
			if (keylen > LARGE_KEY_SIZE) {
				printk(KERN_ERR "%s: BUG: Key %d has oversize length %d.\n",
				       priv->ndev->name, priv->tx_key, keylen);
				return -E2BIG;
			}

			/* Write all 4 keys */
			for(i = 0; i < ORINOCO_MAX_KEYS; i++) {
				err = hermes_write_ltv(hw, USER_BAP,
						       HERMES_RID_CNFDEFAULTKEY0 + i,
						       HERMES_BYTES_TO_RECLEN(keylen),
						       priv->keys[i].data);
				if (err)
					return err;
			}

			/* Write the index of the key used in transmission */
			err = hermes_write_wordrec(hw, USER_BAP,
						   HERMES_RID_CNFWEPDEFAULTKEYID,
						   priv->tx_key);
			if (err)
				return err;
		}
		break;
	}

	return 0;
}

static int __orinoco_hw_setup_enc(struct orinoco_private *priv)
{
	hermes_t *hw = &priv->hw;
	int err = 0;
	int master_wep_flag;
	int auth_flag;
	int enc_flag;

	/* Setup WEP keys for WEP and WPA */
	if (priv->encode_alg)
		__orinoco_hw_setup_wepkeys(priv);

	if (priv->wep_restrict)
		auth_flag = HERMES_AUTH_SHARED_KEY;
	else
		auth_flag = HERMES_AUTH_OPEN;

	if (priv->wpa_enabled)
		enc_flag = 2;
	else if (priv->encode_alg == IW_ENCODE_ALG_WEP)
		enc_flag = 1;
	else
		enc_flag = 0;

	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE: /* Agere style WEP */
		if (priv->encode_alg == IW_ENCODE_ALG_WEP) {
			/* Enable the shared-key authentication. */
			err = hermes_write_wordrec(hw, USER_BAP,
						   HERMES_RID_CNFAUTHENTICATION_AGERE,
						   auth_flag);
		}
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFWEPENABLED_AGERE,
					   enc_flag);
		if (err)
			return err;

		if (priv->has_wpa) {
			/* Set WPA key management */
			err = hermes_write_wordrec(hw, USER_BAP,
				  HERMES_RID_CNFSETWPAAUTHMGMTSUITE_AGERE,
				  priv->key_mgmt);
			if (err)
				return err;
		}

		break;

	case FIRMWARE_TYPE_INTERSIL: /* Intersil style WEP */
	case FIRMWARE_TYPE_SYMBOL: /* Symbol style WEP */
		if (priv->encode_alg == IW_ENCODE_ALG_WEP) {
			if (priv->wep_restrict ||
			    (priv->firmware_type == FIRMWARE_TYPE_SYMBOL))
				master_wep_flag = HERMES_WEP_PRIVACY_INVOKED |
						  HERMES_WEP_EXCL_UNENCRYPTED;
			else
				master_wep_flag = HERMES_WEP_PRIVACY_INVOKED;

			err = hermes_write_wordrec(hw, USER_BAP,
						   HERMES_RID_CNFAUTHENTICATION,
						   auth_flag);
			if (err)
				return err;
		} else
			master_wep_flag = 0;

		if (priv->iw_mode == IW_MODE_MONITOR)
			master_wep_flag |= HERMES_WEP_HOST_DECRYPT;

		/* Master WEP setting : on/off */
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFWEPFLAGS_INTERSIL,
					   master_wep_flag);
		if (err)
			return err;	

		break;
	}

	return 0;
}

/* key must be 32 bytes, including the tx and rx MIC keys.
 * rsc must be 8 bytes
 * tsc must be 8 bytes or NULL
 */
static int __orinoco_hw_set_tkip_key(hermes_t *hw, int key_idx, int set_tx,
				     u8 *key, u8 *rsc, u8 *tsc)
{
	struct {
		__le16 idx;
		u8 rsc[IW_ENCODE_SEQ_MAX_SIZE];
		u8 key[TKIP_KEYLEN];
		u8 tx_mic[MIC_KEYLEN];
		u8 rx_mic[MIC_KEYLEN];
		u8 tsc[IW_ENCODE_SEQ_MAX_SIZE];
	} __attribute__ ((packed)) buf;
	int ret;
	int err;
	int k;
	u16 xmitting;

	key_idx &= 0x3;

	if (set_tx)
		key_idx |= 0x8000;

	buf.idx = cpu_to_le16(key_idx);
	memcpy(buf.key, key,
	       sizeof(buf.key) + sizeof(buf.tx_mic) + sizeof(buf.rx_mic));

	if (rsc == NULL)
		memset(buf.rsc, 0, sizeof(buf.rsc));
	else
		memcpy(buf.rsc, rsc, sizeof(buf.rsc));

	if (tsc == NULL) {
		memset(buf.tsc, 0, sizeof(buf.tsc));
		buf.tsc[4] = 0x10;
	} else {
		memcpy(buf.tsc, tsc, sizeof(buf.tsc));
	}

	/* Wait upto 100ms for tx queue to empty */
	k = 100;
	do {
		k--;
		udelay(1000);
		ret = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_TXQUEUEEMPTY,
					  &xmitting);
		if (ret)
			break;
	} while ((k > 0) && xmitting);

	if (k == 0)
		ret = -ETIMEDOUT;

	err = HERMES_WRITE_RECORD(hw, USER_BAP,
				  HERMES_RID_CNFADDDEFAULTTKIPKEY_AGERE,
				  &buf);

	return ret ? ret : err;
}

static int orinoco_clear_tkip_key(struct orinoco_private *priv,
				  int key_idx)
{
	hermes_t *hw = &priv->hw;
	int err;

	memset(&priv->tkip_key[key_idx], 0, sizeof(priv->tkip_key[key_idx]));
	err = hermes_write_wordrec(hw, USER_BAP,
				   HERMES_RID_CNFREMDEFAULTTKIPKEY_AGERE,
				   key_idx);
	if (err)
		printk(KERN_WARNING "%s: Error %d clearing TKIP key %d\n",
		       priv->ndev->name, err, key_idx);
	return err;
}

static int __orinoco_program_rids(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err;
	struct hermes_idstring idbuf;

	/* Set the MAC address */
	err = hermes_write_ltv(hw, USER_BAP, HERMES_RID_CNFOWNMACADDR,
			       HERMES_BYTES_TO_RECLEN(ETH_ALEN), dev->dev_addr);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting MAC address\n",
		       dev->name, err);
		return err;
	}

	/* Set up the link mode */
	err = hermes_write_wordrec(hw, USER_BAP, HERMES_RID_CNFPORTTYPE,
				   priv->port_type);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting port type\n",
		       dev->name, err);
		return err;
	}
	/* Set the channel/frequency */
	if (priv->channel != 0 && priv->iw_mode != IW_MODE_INFRA) {
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFOWNCHANNEL,
					   priv->channel);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting channel %d\n",
			       dev->name, err, priv->channel);
			return err;
		}
	}

	if (priv->has_ibss) {
		u16 createibss;

		if ((strlen(priv->desired_essid) == 0) && (priv->createibss)) {
			printk(KERN_WARNING "%s: This firmware requires an "
			       "ESSID in IBSS-Ad-Hoc mode.\n", dev->name);
			/* With wvlan_cs, in this case, we would crash.
			 * hopefully, this driver will behave better...
			 * Jean II */
			createibss = 0;
		} else {
			createibss = priv->createibss;
		}
		
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFCREATEIBSS,
					   createibss);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting CREATEIBSS\n",
			       dev->name, err);
			return err;
		}
	}

	/* Set the desired BSSID */
	err = __orinoco_hw_set_wap(priv);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting AP address\n",
		       dev->name, err);
		return err;
	}
	/* Set the desired ESSID */
	idbuf.len = cpu_to_le16(strlen(priv->desired_essid));
	memcpy(&idbuf.val, priv->desired_essid, sizeof(idbuf.val));
	/* WinXP wants partner to configure OWNSSID even in IBSS mode. (jimc) */
	err = hermes_write_ltv(hw, USER_BAP, HERMES_RID_CNFOWNSSID,
			       HERMES_BYTES_TO_RECLEN(strlen(priv->desired_essid)+2),
			       &idbuf);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting OWNSSID\n",
		       dev->name, err);
		return err;
	}
	err = hermes_write_ltv(hw, USER_BAP, HERMES_RID_CNFDESIREDSSID,
			       HERMES_BYTES_TO_RECLEN(strlen(priv->desired_essid)+2),
			       &idbuf);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting DESIREDSSID\n",
		       dev->name, err);
		return err;
	}

	/* Set the station name */
	idbuf.len = cpu_to_le16(strlen(priv->nick));
	memcpy(&idbuf.val, priv->nick, sizeof(idbuf.val));
	err = hermes_write_ltv(hw, USER_BAP, HERMES_RID_CNFOWNNAME,
			       HERMES_BYTES_TO_RECLEN(strlen(priv->nick)+2),
			       &idbuf);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting nickname\n",
		       dev->name, err);
		return err;
	}

	/* Set AP density */
	if (priv->has_sensitivity) {
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFSYSTEMSCALE,
					   priv->ap_density);
		if (err) {
			printk(KERN_WARNING "%s: Error %d setting SYSTEMSCALE.  "
			       "Disabling sensitivity control\n",
			       dev->name, err);

			priv->has_sensitivity = 0;
		}
	}

	/* Set RTS threshold */
	err = hermes_write_wordrec(hw, USER_BAP, HERMES_RID_CNFRTSTHRESHOLD,
				   priv->rts_thresh);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting RTS threshold\n",
		       dev->name, err);
		return err;
	}

	/* Set fragmentation threshold or MWO robustness */
	if (priv->has_mwo)
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFMWOROBUST_AGERE,
					   priv->mwo_robust);
	else
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFFRAGMENTATIONTHRESHOLD,
					   priv->frag_thresh);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting fragmentation\n",
		       dev->name, err);
		return err;
	}

	/* Set bitrate */
	err = __orinoco_hw_set_bitrate(priv);
	if (err) {
		printk(KERN_ERR "%s: Error %d setting bitrate\n",
		       dev->name, err);
		return err;
	}

	/* Set power management */
	if (priv->has_pm) {
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFPMENABLED,
					   priv->pm_on);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting up PM\n",
			       dev->name, err);
			return err;
		}

		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFMULTICASTRECEIVE,
					   priv->pm_mcast);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting up PM\n",
			       dev->name, err);
			return err;
		}
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFMAXSLEEPDURATION,
					   priv->pm_period);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting up PM\n",
			       dev->name, err);
			return err;
		}
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFPMHOLDOVERDURATION,
					   priv->pm_timeout);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting up PM\n",
			       dev->name, err);
			return err;
		}
	}

	/* Set preamble - only for Symbol so far... */
	if (priv->has_preamble) {
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFPREAMBLE_SYMBOL,
					   priv->preamble);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting preamble\n",
			       dev->name, err);
			return err;
		}
	}

	/* Set up encryption */
	if (priv->has_wep || priv->has_wpa) {
		err = __orinoco_hw_setup_enc(priv);
		if (err) {
			printk(KERN_ERR "%s: Error %d activating encryption\n",
			       dev->name, err);
			return err;
		}
	}

	if (priv->iw_mode == IW_MODE_MONITOR) {
		/* Enable monitor mode */
		dev->type = ARPHRD_IEEE80211;
		err = hermes_docmd_wait(hw, HERMES_CMD_TEST | 
					    HERMES_TEST_MONITOR, 0, NULL);
	} else {
		/* Disable monitor mode */
		dev->type = ARPHRD_ETHER;
		err = hermes_docmd_wait(hw, HERMES_CMD_TEST |
					    HERMES_TEST_STOP, 0, NULL);
	}
	if (err)
		return err;

	/* Set promiscuity / multicast*/
	priv->promiscuous = 0;
	priv->mc_count = 0;

	/* FIXME: what about netif_tx_lock */
	__orinoco_set_multicast_list(dev);

	return 0;
}

/* FIXME: return int? */
static void
__orinoco_set_multicast_list(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err = 0;
	int promisc, mc_count;

	/* The Hermes doesn't seem to have an allmulti mode, so we go
	 * into promiscuous mode and let the upper levels deal. */
	if ( (dev->flags & IFF_PROMISC) || (dev->flags & IFF_ALLMULTI) ||
	     (dev->mc_count > MAX_MULTICAST(priv)) ) {
		promisc = 1;
		mc_count = 0;
	} else {
		promisc = 0;
		mc_count = dev->mc_count;
	}

	if (promisc != priv->promiscuous) {
		err = hermes_write_wordrec(hw, USER_BAP,
					   HERMES_RID_CNFPROMISCUOUSMODE,
					   promisc);
		if (err) {
			printk(KERN_ERR "%s: Error %d setting PROMISCUOUSMODE to 1.\n",
			       dev->name, err);
		} else 
			priv->promiscuous = promisc;
	}

	/* If we're not in promiscuous mode, then we need to set the
	 * group address if either we want to multicast, or if we were
	 * multicasting and want to stop */
	if (! promisc && (mc_count || priv->mc_count) ) {
		struct dev_mc_list *p = dev->mc_list;
		struct hermes_multicast mclist;
		int i;

		for (i = 0; i < mc_count; i++) {
			/* paranoia: is list shorter than mc_count? */
			BUG_ON(! p);
			/* paranoia: bad address size in list? */
			BUG_ON(p->dmi_addrlen != ETH_ALEN);
			
			memcpy(mclist.addr[i], p->dmi_addr, ETH_ALEN);
			p = p->next;
		}
		
		if (p)
			printk(KERN_WARNING "%s: Multicast list is "
			       "longer than mc_count\n", dev->name);

		err = hermes_write_ltv(hw, USER_BAP,
				   HERMES_RID_CNFGROUPADDRESSES,
				   HERMES_BYTES_TO_RECLEN(mc_count * ETH_ALEN),
				   &mclist);
		if (err)
			printk(KERN_ERR "%s: Error %d setting multicast list.\n",
			       dev->name, err);
		else
			priv->mc_count = mc_count;
	}
}

/* This must be called from user context, without locks held - use
 * schedule_work() */
static void orinoco_reset(struct work_struct *work)
{
	struct orinoco_private *priv =
		container_of(work, struct orinoco_private, reset_work);
	struct net_device *dev = priv->ndev;
	struct hermes *hw = &priv->hw;
	int err;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		/* When the hardware becomes available again, whatever
		 * detects that is responsible for re-initializing
		 * it. So no need for anything further */
		return;

	netif_stop_queue(dev);

	/* Shut off interrupts.  Depending on what state the hardware
	 * is in, this might not work, but we'll try anyway */
	hermes_set_irqmask(hw, 0);
	hermes_write_regn(hw, EVACK, 0xffff);

	priv->hw_unavailable++;
	priv->last_linkstatus = 0xffff; /* firmware will have to reassociate */
	netif_carrier_off(dev);

	orinoco_unlock(priv, &flags);

 	/* Scanning support: Cleanup of driver struct */
	orinoco_clear_scan_results(priv, 0);
	priv->scan_inprogress = 0;

	if (priv->hard_reset) {
		err = (*priv->hard_reset)(priv);
		if (err) {
			printk(KERN_ERR "%s: orinoco_reset: Error %d "
			       "performing hard reset\n", dev->name, err);
			goto disable;
		}
	}

	if (priv->do_fw_download) {
		err = orinoco_download(priv);
		if (err)
			priv->do_fw_download = 0;
	}

	err = orinoco_reinit_firmware(dev);
	if (err) {
		printk(KERN_ERR "%s: orinoco_reset: Error %d re-initializing firmware\n",
		       dev->name, err);
		goto disable;
	}

	spin_lock_irq(&priv->lock); /* This has to be called from user context */

	priv->hw_unavailable--;

	/* priv->open or priv->hw_unavailable might have changed while
	 * we dropped the lock */
	if (priv->open && (! priv->hw_unavailable)) {
		err = __orinoco_up(dev);
		if (err) {
			printk(KERN_ERR "%s: orinoco_reset: Error %d reenabling card\n",
			       dev->name, err);
		} else
			dev->trans_start = jiffies;
	}

	spin_unlock_irq(&priv->lock);

	return;
 disable:
	hermes_set_irqmask(hw, 0);
	netif_device_detach(dev);
	printk(KERN_ERR "%s: Device has been disabled!\n", dev->name);
}

/********************************************************************/
/* Interrupt handler                                                */
/********************************************************************/

static void __orinoco_ev_tick(struct net_device *dev, hermes_t *hw)
{
	printk(KERN_DEBUG "%s: TICK\n", dev->name);
}

static void __orinoco_ev_wterr(struct net_device *dev, hermes_t *hw)
{
	/* This seems to happen a fair bit under load, but ignoring it
	   seems to work fine...*/
	printk(KERN_DEBUG "%s: MAC controller error (WTERR). Ignoring.\n",
	       dev->name);
}

irqreturn_t orinoco_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int count = MAX_IRQLOOPS_PER_IRQ;
	u16 evstat, events;
	/* These are used to detect a runaway interrupt situation */
	/* If we get more than MAX_IRQLOOPS_PER_JIFFY iterations in a jiffy,
	 * we panic and shut down the hardware */
	static int last_irq_jiffy = 0; /* jiffies value the last time
					* we were called */
	static int loops_this_jiffy = 0;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0) {
		/* If hw is unavailable - we don't know if the irq was
		 * for us or not */
		return IRQ_HANDLED;
	}

	evstat = hermes_read_regn(hw, EVSTAT);
	events = evstat & hw->inten;
	if (! events) {
		orinoco_unlock(priv, &flags);
		return IRQ_NONE;
	}
	
	if (jiffies != last_irq_jiffy)
		loops_this_jiffy = 0;
	last_irq_jiffy = jiffies;

	while (events && count--) {
		if (++loops_this_jiffy > MAX_IRQLOOPS_PER_JIFFY) {
			printk(KERN_WARNING "%s: IRQ handler is looping too "
			       "much! Resetting.\n", dev->name);
			/* Disable interrupts for now */
			hermes_set_irqmask(hw, 0);
			schedule_work(&priv->reset_work);
			break;
		}

		/* Check the card hasn't been removed */
		if (! hermes_present(hw)) {
			DEBUG(0, "orinoco_interrupt(): card removed\n");
			break;
		}

		if (events & HERMES_EV_TICK)
			__orinoco_ev_tick(dev, hw);
		if (events & HERMES_EV_WTERR)
			__orinoco_ev_wterr(dev, hw);
		if (events & HERMES_EV_INFDROP)
			__orinoco_ev_infdrop(dev, hw);
		if (events & HERMES_EV_INFO)
			__orinoco_ev_info(dev, hw);
		if (events & HERMES_EV_RX)
			__orinoco_ev_rx(dev, hw);
		if (events & HERMES_EV_TXEXC)
			__orinoco_ev_txexc(dev, hw);
		if (events & HERMES_EV_TX)
			__orinoco_ev_tx(dev, hw);
		if (events & HERMES_EV_ALLOC)
			__orinoco_ev_alloc(dev, hw);
		
		hermes_write_regn(hw, EVACK, evstat);

		evstat = hermes_read_regn(hw, EVSTAT);
		events = evstat & hw->inten;
	};

	orinoco_unlock(priv, &flags);
	return IRQ_HANDLED;
}

/********************************************************************/
/* Initialization                                                   */
/********************************************************************/

struct comp_id {
	u16 id, variant, major, minor;
} __attribute__ ((packed));

static inline fwtype_t determine_firmware_type(struct comp_id *nic_id)
{
	if (nic_id->id < 0x8000)
		return FIRMWARE_TYPE_AGERE;
	else if (nic_id->id == 0x8000 && nic_id->major == 0)
		return FIRMWARE_TYPE_SYMBOL;
	else
		return FIRMWARE_TYPE_INTERSIL;
}

/* Set priv->firmware type, determine firmware properties */
static int determine_firmware(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err;
	struct comp_id nic_id, sta_id;
	unsigned int firmver;
	char tmp[SYMBOL_MAX_VER_LEN+1] __attribute__((aligned(2)));

	/* Get the hardware version */
	err = HERMES_READ_RECORD(hw, USER_BAP, HERMES_RID_NICID, &nic_id);
	if (err) {
		printk(KERN_ERR "%s: Cannot read hardware identity: error %d\n",
		       dev->name, err);
		return err;
	}

	le16_to_cpus(&nic_id.id);
	le16_to_cpus(&nic_id.variant);
	le16_to_cpus(&nic_id.major);
	le16_to_cpus(&nic_id.minor);
	printk(KERN_DEBUG "%s: Hardware identity %04x:%04x:%04x:%04x\n",
	       dev->name, nic_id.id, nic_id.variant,
	       nic_id.major, nic_id.minor);

	priv->firmware_type = determine_firmware_type(&nic_id);

	/* Get the firmware version */
	err = HERMES_READ_RECORD(hw, USER_BAP, HERMES_RID_STAID, &sta_id);
	if (err) {
		printk(KERN_ERR "%s: Cannot read station identity: error %d\n",
		       dev->name, err);
		return err;
	}

	le16_to_cpus(&sta_id.id);
	le16_to_cpus(&sta_id.variant);
	le16_to_cpus(&sta_id.major);
	le16_to_cpus(&sta_id.minor);
	printk(KERN_DEBUG "%s: Station identity  %04x:%04x:%04x:%04x\n",
	       dev->name, sta_id.id, sta_id.variant,
	       sta_id.major, sta_id.minor);

	switch (sta_id.id) {
	case 0x15:
		printk(KERN_ERR "%s: Primary firmware is active\n",
		       dev->name);
		return -ENODEV;
	case 0x14b:
		printk(KERN_ERR "%s: Tertiary firmware is active\n",
		       dev->name);
		return -ENODEV;
	case 0x1f:	/* Intersil, Agere, Symbol Spectrum24 */
	case 0x21:	/* Symbol Spectrum24 Trilogy */
		break;
	default:
		printk(KERN_NOTICE "%s: Unknown station ID, please report\n",
		       dev->name);
		break;
	}

	/* Default capabilities */
	priv->has_sensitivity = 1;
	priv->has_mwo = 0;
	priv->has_preamble = 0;
	priv->has_port3 = 1;
	priv->has_ibss = 1;
	priv->has_wep = 0;
	priv->has_big_wep = 0;
	priv->has_alt_txcntl = 0;
	priv->has_ext_scan = 0;
	priv->has_wpa = 0;
	priv->do_fw_download = 0;

	/* Determine capabilities from the firmware version */
	switch (priv->firmware_type) {
	case FIRMWARE_TYPE_AGERE:
		/* Lucent Wavelan IEEE, Lucent Orinoco, Cabletron RoamAbout,
		   ELSA, Melco, HP, IBM, Dell 1150, Compaq 110/210 */
		snprintf(priv->fw_name, sizeof(priv->fw_name) - 1,
			 "Lucent/Agere %d.%02d", sta_id.major, sta_id.minor);

		firmver = ((unsigned long)sta_id.major << 16) | sta_id.minor;

		priv->has_ibss = (firmver >= 0x60006);
		priv->has_wep = (firmver >= 0x40020);
		priv->has_big_wep = 1; /* FIXME: this is wrong - how do we tell
					  Gold cards from the others? */
		priv->has_mwo = (firmver >= 0x60000);
		priv->has_pm = (firmver >= 0x40020); /* Don't work in 7.52 ? */
		priv->ibss_port = 1;
		priv->has_hostscan = (firmver >= 0x8000a);
		priv->do_fw_download = 1;
		priv->broken_monitor = (firmver >= 0x80000);
		priv->has_alt_txcntl = (firmver >= 0x90000); /* All 9.x ? */
		priv->has_ext_scan = (firmver >= 0x90000); /* All 9.x ? */
		priv->has_wpa = (firmver >= 0x9002a);
		/* Tested with Agere firmware :
		 *	1.16 ; 4.08 ; 4.52 ; 6.04 ; 6.16 ; 7.28 => Jean II
		 * Tested CableTron firmware : 4.32 => Anton */
		break;
	case FIRMWARE_TYPE_SYMBOL:
		/* Symbol , 3Com AirConnect, Intel, Ericsson WLAN */
		/* Intel MAC : 00:02:B3:* */
		/* 3Com MAC : 00:50:DA:* */
		memset(tmp, 0, sizeof(tmp));
		/* Get the Symbol firmware version */
		err = hermes_read_ltv(hw, USER_BAP,
				      HERMES_RID_SECONDARYVERSION_SYMBOL,
				      SYMBOL_MAX_VER_LEN, NULL, &tmp);
		if (err) {
			printk(KERN_WARNING
			       "%s: Error %d reading Symbol firmware info. Wildly guessing capabilities...\n",
			       dev->name, err);
			firmver = 0;
			tmp[0] = '\0';
		} else {
			/* The firmware revision is a string, the format is
			 * something like : "V2.20-01".
			 * Quick and dirty parsing... - Jean II
			 */
			firmver = ((tmp[1] - '0') << 16) | ((tmp[3] - '0') << 12)
				| ((tmp[4] - '0') << 8) | ((tmp[6] - '0') << 4)
				| (tmp[7] - '0');

			tmp[SYMBOL_MAX_VER_LEN] = '\0';
		}

		snprintf(priv->fw_name, sizeof(priv->fw_name) - 1,
			 "Symbol %s", tmp);

		priv->has_ibss = (firmver >= 0x20000);
		priv->has_wep = (firmver >= 0x15012);
		priv->has_big_wep = (firmver >= 0x20000);
		priv->has_pm = (firmver >= 0x20000 && firmver < 0x22000) || 
			       (firmver >= 0x29000 && firmver < 0x30000) ||
			       firmver >= 0x31000;
		priv->has_preamble = (firmver >= 0x20000);
		priv->ibss_port = 4;

		/* Symbol firmware is found on various cards, but
		 * there has been no attempt to check firmware
		 * download on non-spectrum_cs based cards.
		 *
		 * Given that the Agere firmware download works
		 * differently, we should avoid doing a firmware
		 * download with the Symbol algorithm on non-spectrum
		 * cards.
		 *
		 * For now we can identify a spectrum_cs based card
		 * because it has a firmware reset function.
		 */
		priv->do_fw_download = (priv->stop_fw != NULL);

 		priv->broken_disableport = (firmver == 0x25013) ||
 					   (firmver >= 0x30000 && firmver <= 0x31000);
		priv->has_hostscan = (firmver >= 0x31001) ||
				     (firmver >= 0x29057 && firmver < 0x30000);
		/* Tested with Intel firmware : 0x20015 => Jean II */
		/* Tested with 3Com firmware : 0x15012 & 0x22001 => Jean II */
		break;
	case FIRMWARE_TYPE_INTERSIL:
		/* D-Link, Linksys, Adtron, ZoomAir, and many others...
		 * Samsung, Compaq 100/200 and Proxim are slightly
		 * different and less well tested */
		/* D-Link MAC : 00:40:05:* */
		/* Addtron MAC : 00:90:D1:* */
		snprintf(priv->fw_name, sizeof(priv->fw_name) - 1,
			 "Intersil %d.%d.%d", sta_id.major, sta_id.minor,
			 sta_id.variant);

		firmver = ((unsigned long)sta_id.major << 16) |
			((unsigned long)sta_id.minor << 8) | sta_id.variant;

		priv->has_ibss = (firmver >= 0x000700); /* FIXME */
		priv->has_big_wep = priv->has_wep = (firmver >= 0x000800);
		priv->has_pm = (firmver >= 0x000700);
		priv->has_hostscan = (firmver >= 0x010301);

		if (firmver >= 0x000800)
			priv->ibss_port = 0;
		else {
			printk(KERN_NOTICE "%s: Intersil firmware earlier "
			       "than v0.8.x - several features not supported\n",
			       dev->name);
			priv->ibss_port = 1;
		}
		break;
	}
	printk(KERN_DEBUG "%s: Firmware determined as %s\n", dev->name,
	       priv->fw_name);

	return 0;
}

static int orinoco_init(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err = 0;
	struct hermes_idstring nickbuf;
	u16 reclen;
	int len;
	DECLARE_MAC_BUF(mac);

	/* No need to lock, the hw_unavailable flag is already set in
	 * alloc_orinocodev() */
	priv->nicbuf_size = IEEE80211_FRAME_LEN + ETH_HLEN;

	/* Initialize the firmware */
	err = hermes_init(hw);
	if (err != 0) {
		printk(KERN_ERR "%s: failed to initialize firmware (err = %d)\n",
		       dev->name, err);
		goto out;
	}

	err = determine_firmware(dev);
	if (err != 0) {
		printk(KERN_ERR "%s: Incompatible firmware, aborting\n",
		       dev->name);
		goto out;
	}

	if (priv->do_fw_download) {
		err = orinoco_download(priv);
		if (err)
			priv->do_fw_download = 0;

		/* Check firmware version again */
		err = determine_firmware(dev);
		if (err != 0) {
			printk(KERN_ERR "%s: Incompatible firmware, aborting\n",
			       dev->name);
			goto out;
		}
	}

	if (priv->has_port3)
		printk(KERN_DEBUG "%s: Ad-hoc demo mode supported\n", dev->name);
	if (priv->has_ibss)
		printk(KERN_DEBUG "%s: IEEE standard IBSS ad-hoc mode supported\n",
		       dev->name);
	if (priv->has_wep) {
		printk(KERN_DEBUG "%s: WEP supported, ", dev->name);
		if (priv->has_big_wep)
			printk("104-bit key\n");
		else
			printk("40-bit key\n");
	}
	if (priv->has_wpa) {
		printk(KERN_DEBUG "%s: WPA-PSK supported\n", dev->name);
		if (orinoco_mic_init(priv)) {
			printk(KERN_ERR "%s: Failed to setup MIC crypto "
			       "algorithm. Disabling WPA support\n", dev->name);
			priv->has_wpa = 0;
		}
	}

	/* Now we have the firmware capabilities, allocate appropiate
	 * sized scan buffers */
	if (orinoco_bss_data_allocate(priv))
		goto out;
	orinoco_bss_data_init(priv);

	/* Get the MAC address */
	err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_CNFOWNMACADDR,
			      ETH_ALEN, NULL, dev->dev_addr);
	if (err) {
		printk(KERN_WARNING "%s: failed to read MAC address!\n",
		       dev->name);
		goto out;
	}

	printk(KERN_DEBUG "%s: MAC address %s\n",
	       dev->name, print_mac(mac, dev->dev_addr));

	/* Get the station name */
	err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_CNFOWNNAME,
			      sizeof(nickbuf), &reclen, &nickbuf);
	if (err) {
		printk(KERN_ERR "%s: failed to read station name\n",
		       dev->name);
		goto out;
	}
	if (nickbuf.len)
		len = min(IW_ESSID_MAX_SIZE, (int)le16_to_cpu(nickbuf.len));
	else
		len = min(IW_ESSID_MAX_SIZE, 2 * reclen);
	memcpy(priv->nick, &nickbuf.val, len);
	priv->nick[len] = '\0';

	printk(KERN_DEBUG "%s: Station name \"%s\"\n", dev->name, priv->nick);

	err = orinoco_allocate_fid(dev);
	if (err) {
		printk(KERN_ERR "%s: failed to allocate NIC buffer!\n",
		       dev->name);
		goto out;
	}

	/* Get allowed channels */
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CHANNELLIST,
				  &priv->channel_mask);
	if (err) {
		printk(KERN_ERR "%s: failed to read channel list!\n",
		       dev->name);
		goto out;
	}

	/* Get initial AP density */
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFSYSTEMSCALE,
				  &priv->ap_density);
	if (err || priv->ap_density < 1 || priv->ap_density > 3) {
		priv->has_sensitivity = 0;
	}

	/* Get initial RTS threshold */
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFRTSTHRESHOLD,
				  &priv->rts_thresh);
	if (err) {
		printk(KERN_ERR "%s: failed to read RTS threshold!\n",
		       dev->name);
		goto out;
	}

	/* Get initial fragmentation settings */
	if (priv->has_mwo)
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CNFMWOROBUST_AGERE,
					  &priv->mwo_robust);
	else
		err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFFRAGMENTATIONTHRESHOLD,
					  &priv->frag_thresh);
	if (err) {
		printk(KERN_ERR "%s: failed to read fragmentation settings!\n",
		       dev->name);
		goto out;
	}

	/* Power management setup */
	if (priv->has_pm) {
		priv->pm_on = 0;
		priv->pm_mcast = 1;
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CNFMAXSLEEPDURATION,
					  &priv->pm_period);
		if (err) {
			printk(KERN_ERR "%s: failed to read power management period!\n",
			       dev->name);
			goto out;
		}
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CNFPMHOLDOVERDURATION,
					  &priv->pm_timeout);
		if (err) {
			printk(KERN_ERR "%s: failed to read power management timeout!\n",
			       dev->name);
			goto out;
		}
	}

	/* Preamble setup */
	if (priv->has_preamble) {
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CNFPREAMBLE_SYMBOL,
					  &priv->preamble);
		if (err)
			goto out;
	}
		
	/* Set up the default configuration */
	priv->iw_mode = IW_MODE_INFRA;
	/* By default use IEEE/IBSS ad-hoc mode if we have it */
	priv->prefer_port3 = priv->has_port3 && (! priv->has_ibss);
	set_port_type(priv);
	priv->channel = 0; /* use firmware default */

	priv->promiscuous = 0;
	priv->encode_alg = IW_ENCODE_ALG_NONE;
	priv->tx_key = 0;
	priv->wpa_enabled = 0;
	priv->tkip_cm_active = 0;
	priv->key_mgmt = 0;
	priv->wpa_ie_len = 0;
	priv->wpa_ie = NULL;

	/* Make the hardware available, as long as it hasn't been
	 * removed elsewhere (e.g. by PCMCIA hot unplug) */
	spin_lock_irq(&priv->lock);
	priv->hw_unavailable--;
	spin_unlock_irq(&priv->lock);

	printk(KERN_DEBUG "%s: ready\n", dev->name);

 out:
	return err;
}

struct net_device
*alloc_orinocodev(int sizeof_card,
		  struct device *device,
		  int (*hard_reset)(struct orinoco_private *),
		  int (*stop_fw)(struct orinoco_private *, int))
{
	struct net_device *dev;
	struct orinoco_private *priv;

	dev = alloc_etherdev(sizeof(struct orinoco_private) + sizeof_card);
	if (! dev)
		return NULL;
	priv = netdev_priv(dev);
	priv->ndev = dev;
	if (sizeof_card)
		priv->card = (void *)((unsigned long)priv
				      + sizeof(struct orinoco_private));
	else
		priv->card = NULL;
	priv->dev = device;

	/* Setup / override net_device fields */
	dev->init = orinoco_init;
	dev->hard_start_xmit = orinoco_xmit;
	dev->tx_timeout = orinoco_tx_timeout;
	dev->watchdog_timeo = HZ; /* 1 second timeout */
	dev->get_stats = orinoco_get_stats;
	dev->ethtool_ops = &orinoco_ethtool_ops;
	dev->wireless_handlers = (struct iw_handler_def *)&orinoco_handler_def;
#ifdef WIRELESS_SPY
	priv->wireless_data.spy_data = &priv->spy_data;
	dev->wireless_data = &priv->wireless_data;
#endif
	dev->change_mtu = orinoco_change_mtu;
	dev->set_multicast_list = orinoco_set_multicast_list;
	/* we use the default eth_mac_addr for setting the MAC addr */

	/* Reserve space in skb for the SNAP header */
	dev->hard_header_len += ENCAPS_OVERHEAD;

	/* Set up default callbacks */
	dev->open = orinoco_open;
	dev->stop = orinoco_stop;
	priv->hard_reset = hard_reset;
	priv->stop_fw = stop_fw;

	spin_lock_init(&priv->lock);
	priv->open = 0;
	priv->hw_unavailable = 1; /* orinoco_init() must clear this
				   * before anything else touches the
				   * hardware */
	INIT_WORK(&priv->reset_work, orinoco_reset);
	INIT_WORK(&priv->join_work, orinoco_join_ap);
	INIT_WORK(&priv->wevent_work, orinoco_send_wevents);

	INIT_LIST_HEAD(&priv->rx_list);
	tasklet_init(&priv->rx_tasklet, orinoco_rx_isr_tasklet,
		     (unsigned long) dev);

	netif_carrier_off(dev);
	priv->last_linkstatus = 0xffff;

	return dev;
}

void free_orinocodev(struct net_device *dev)
{
	struct orinoco_private *priv = netdev_priv(dev);

	/* No need to empty priv->rx_list: if the tasklet is scheduled
	 * when we call tasklet_kill it will run one final time,
	 * emptying the list */
	tasklet_kill(&priv->rx_tasklet);
	priv->wpa_ie_len = 0;
	kfree(priv->wpa_ie);
	orinoco_mic_free(priv);
	orinoco_bss_data_free(priv);
	free_netdev(dev);
}

/********************************************************************/
/* Wireless extensions                                              */
/********************************************************************/

/* Return : < 0 -> error code ; >= 0 -> length */
static int orinoco_hw_get_essid(struct orinoco_private *priv, int *active,
				char buf[IW_ESSID_MAX_SIZE+1])
{
	hermes_t *hw = &priv->hw;
	int err = 0;
	struct hermes_idstring essidbuf;
	char *p = (char *)(&essidbuf.val);
	int len;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if (strlen(priv->desired_essid) > 0) {
		/* We read the desired SSID from the hardware rather
		   than from priv->desired_essid, just in case the
		   firmware is allowed to change it on us. I'm not
		   sure about this */
		/* My guess is that the OWNSSID should always be whatever
		 * we set to the card, whereas CURRENT_SSID is the one that
		 * may change... - Jean II */
		u16 rid;

		*active = 1;

		rid = (priv->port_type == 3) ? HERMES_RID_CNFOWNSSID :
			HERMES_RID_CNFDESIREDSSID;
		
		err = hermes_read_ltv(hw, USER_BAP, rid, sizeof(essidbuf),
				      NULL, &essidbuf);
		if (err)
			goto fail_unlock;
	} else {
		*active = 0;

		err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_CURRENTSSID,
				      sizeof(essidbuf), NULL, &essidbuf);
		if (err)
			goto fail_unlock;
	}

	len = le16_to_cpu(essidbuf.len);
	BUG_ON(len > IW_ESSID_MAX_SIZE);

	memset(buf, 0, IW_ESSID_MAX_SIZE);
	memcpy(buf, p, len);
	err = len;

 fail_unlock:
	orinoco_unlock(priv, &flags);

	return err;       
}

static long orinoco_hw_get_freq(struct orinoco_private *priv)
{
	
	hermes_t *hw = &priv->hw;
	int err = 0;
	u16 channel;
	long freq = 0;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CURRENTCHANNEL, &channel);
	if (err)
		goto out;

	/* Intersil firmware 1.3.5 returns 0 when the interface is down */
	if (channel == 0) {
		err = -EBUSY;
		goto out;
	}

	if ( (channel < 1) || (channel > NUM_CHANNELS) ) {
		printk(KERN_WARNING "%s: Channel out of range (%d)!\n",
		       priv->ndev->name, channel);
		err = -EBUSY;
		goto out;

	}
	freq = channel_frequency[channel-1] * 100000;

 out:
	orinoco_unlock(priv, &flags);

	if (err > 0)
		err = -EBUSY;
	return err ? err : freq;
}

static int orinoco_hw_get_bitratelist(struct orinoco_private *priv,
				      int *numrates, s32 *rates, int max)
{
	hermes_t *hw = &priv->hw;
	struct hermes_idstring list;
	unsigned char *p = (unsigned char *)&list.val;
	int err = 0;
	int num;
	int i;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_SUPPORTEDDATARATES,
			      sizeof(list), NULL, &list);
	orinoco_unlock(priv, &flags);

	if (err)
		return err;
	
	num = le16_to_cpu(list.len);
	*numrates = num;
	num = min(num, max);

	for (i = 0; i < num; i++) {
		rates[i] = (p[i] & 0x7f) * 500000; /* convert to bps */
	}

	return 0;
}

static int orinoco_ioctl_getname(struct net_device *dev,
				 struct iw_request_info *info,
				 char *name,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int numrates;
	int err;

	err = orinoco_hw_get_bitratelist(priv, &numrates, NULL, 0);

	if (!err && (numrates > 2))
		strcpy(name, "IEEE 802.11b");
	else
		strcpy(name, "IEEE 802.11-DS");

	return 0;
}

static int orinoco_ioctl_setwap(struct net_device *dev,
				struct iw_request_info *info,
				struct sockaddr *ap_addr,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = -EINPROGRESS;		/* Call commit handler */
	unsigned long flags;
	static const u8 off_addr[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const u8 any_addr[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	/* Enable automatic roaming - no sanity checks are needed */
	if (memcmp(&ap_addr->sa_data, off_addr, ETH_ALEN) == 0 ||
	    memcmp(&ap_addr->sa_data, any_addr, ETH_ALEN) == 0) {
		priv->bssid_fixed = 0;
		memset(priv->desired_bssid, 0, ETH_ALEN);

		/* "off" means keep existing connection */
		if (ap_addr->sa_data[0] == 0) {
			__orinoco_hw_set_wap(priv);
			err = 0;
		}
		goto out;
	}

	if (priv->firmware_type == FIRMWARE_TYPE_AGERE) {
		printk(KERN_WARNING "%s: Lucent/Agere firmware doesn't "
		       "support manual roaming\n",
		       dev->name);
		err = -EOPNOTSUPP;
		goto out;
	}

	if (priv->iw_mode != IW_MODE_INFRA) {
		printk(KERN_WARNING "%s: Manual roaming supported only in "
		       "managed mode\n", dev->name);
		err = -EOPNOTSUPP;
		goto out;
	}

	/* Intersil firmware hangs without Desired ESSID */
	if (priv->firmware_type == FIRMWARE_TYPE_INTERSIL &&
	    strlen(priv->desired_essid) == 0) {
		printk(KERN_WARNING "%s: Desired ESSID must be set for "
		       "manual roaming\n", dev->name);
		err = -EOPNOTSUPP;
		goto out;
	}

	/* Finally, enable manual roaming */
	priv->bssid_fixed = 1;
	memcpy(priv->desired_bssid, &ap_addr->sa_data, ETH_ALEN);

 out:
	orinoco_unlock(priv, &flags);
	return err;
}

static int orinoco_ioctl_getwap(struct net_device *dev,
				struct iw_request_info *info,
				struct sockaddr *ap_addr,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);

	hermes_t *hw = &priv->hw;
	int err = 0;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	ap_addr->sa_family = ARPHRD_ETHER;
	err = hermes_read_ltv(hw, USER_BAP, HERMES_RID_CURRENTBSSID,
			      ETH_ALEN, NULL, ap_addr->sa_data);

	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_setmode(struct net_device *dev,
				 struct iw_request_info *info,
				 u32 *mode,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = -EINPROGRESS;		/* Call commit handler */
	unsigned long flags;

	if (priv->iw_mode == *mode)
		return 0;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	switch (*mode) {
	case IW_MODE_ADHOC:
		if (!priv->has_ibss && !priv->has_port3)
			err = -EOPNOTSUPP;
		break;

	case IW_MODE_INFRA:
		break;

	case IW_MODE_MONITOR:
		if (priv->broken_monitor && !force_monitor) {
			printk(KERN_WARNING "%s: Monitor mode support is "
			       "buggy in this firmware, not enabling\n",
			       dev->name);
			err = -EOPNOTSUPP;
		}
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}

	if (err == -EINPROGRESS) {
		priv->iw_mode = *mode;
		set_port_type(priv);
	}

	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getmode(struct net_device *dev,
				 struct iw_request_info *info,
				 u32 *mode,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);

	*mode = priv->iw_mode;
	return 0;
}

static int orinoco_ioctl_getiwrange(struct net_device *dev,
				    struct iw_request_info *info,
				    struct iw_point *rrq,
				    char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = 0;
	struct iw_range *range = (struct iw_range *) extra;
	int numrates;
	int i, k;

	rrq->length = sizeof(struct iw_range);
	memset(range, 0, sizeof(struct iw_range));

	range->we_version_compiled = WIRELESS_EXT;
	range->we_version_source = 22;

	/* Set available channels/frequencies */
	range->num_channels = NUM_CHANNELS;
	k = 0;
	for (i = 0; i < NUM_CHANNELS; i++) {
		if (priv->channel_mask & (1 << i)) {
			range->freq[k].i = i + 1;
			range->freq[k].m = channel_frequency[i] * 100000;
			range->freq[k].e = 1;
			k++;
		}
		
		if (k >= IW_MAX_FREQUENCIES)
			break;
	}
	range->num_frequency = k;
	range->sensitivity = 3;

	if (priv->has_wep) {
		range->max_encoding_tokens = ORINOCO_MAX_KEYS;
		range->encoding_size[0] = SMALL_KEY_SIZE;
		range->num_encoding_sizes = 1;

		if (priv->has_big_wep) {
			range->encoding_size[1] = LARGE_KEY_SIZE;
			range->num_encoding_sizes = 2;
		}
	}

	if (priv->has_wpa)
		range->enc_capa = IW_ENC_CAPA_WPA | IW_ENC_CAPA_CIPHER_TKIP;

	if ((priv->iw_mode == IW_MODE_ADHOC) && (!SPY_NUMBER(priv))){
		/* Quality stats meaningless in ad-hoc mode */
	} else {
		range->max_qual.qual = 0x8b - 0x2f;
		range->max_qual.level = 0x2f - 0x95 - 1;
		range->max_qual.noise = 0x2f - 0x95 - 1;
		/* Need to get better values */
		range->avg_qual.qual = 0x24;
		range->avg_qual.level = 0xC2;
		range->avg_qual.noise = 0x9E;
	}

	err = orinoco_hw_get_bitratelist(priv, &numrates,
					 range->bitrate, IW_MAX_BITRATES);
	if (err)
		return err;
	range->num_bitrates = numrates;

	/* Set an indication of the max TCP throughput in bit/s that we can
	 * expect using this interface. May be use for QoS stuff...
	 * Jean II */
	if (numrates > 2)
		range->throughput = 5 * 1000 * 1000;	/* ~5 Mb/s */
	else
		range->throughput = 1.5 * 1000 * 1000;	/* ~1.5 Mb/s */

	range->min_rts = 0;
	range->max_rts = 2347;
	range->min_frag = 256;
	range->max_frag = 2346;

	range->min_pmp = 0;
	range->max_pmp = 65535000;
	range->min_pmt = 0;
	range->max_pmt = 65535 * 1000;	/* ??? */
	range->pmp_flags = IW_POWER_PERIOD;
	range->pmt_flags = IW_POWER_TIMEOUT;
	range->pm_capa = IW_POWER_PERIOD | IW_POWER_TIMEOUT | IW_POWER_UNICAST_R;

	range->retry_capa = IW_RETRY_LIMIT | IW_RETRY_LIFETIME;
	range->retry_flags = IW_RETRY_LIMIT;
	range->r_time_flags = IW_RETRY_LIFETIME;
	range->min_retry = 0;
	range->max_retry = 65535;	/* ??? */
	range->min_r_time = 0;
	range->max_r_time = 65535 * 1000;	/* ??? */

	if (priv->firmware_type == FIRMWARE_TYPE_AGERE)
		range->scan_capa = IW_SCAN_CAPA_ESSID;
	else
		range->scan_capa = IW_SCAN_CAPA_NONE;

	/* Event capability (kernel) */
	IW_EVENT_CAPA_SET_KERNEL(range->event_capa);
	/* Event capability (driver) */
	IW_EVENT_CAPA_SET(range->event_capa, SIOCGIWTHRSPY);
	IW_EVENT_CAPA_SET(range->event_capa, SIOCGIWAP);
	IW_EVENT_CAPA_SET(range->event_capa, SIOCGIWSCAN);
	IW_EVENT_CAPA_SET(range->event_capa, IWEVTXDROP);

	return 0;
}

static int orinoco_ioctl_setiwencode(struct net_device *dev,
				     struct iw_request_info *info,
				     struct iw_point *erq,
				     char *keybuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int index = (erq->flags & IW_ENCODE_INDEX) - 1;
	int setindex = priv->tx_key;
	int encode_alg = priv->encode_alg;
	int restricted = priv->wep_restrict;
	u16 xlen = 0;
	int err = -EINPROGRESS;		/* Call commit handler */
	unsigned long flags;

	if (! priv->has_wep)
		return -EOPNOTSUPP;

	if (erq->pointer) {
		/* We actually have a key to set - check its length */
		if (erq->length > LARGE_KEY_SIZE)
			return -E2BIG;

		if ( (erq->length > SMALL_KEY_SIZE) && !priv->has_big_wep )
			return -E2BIG;
	}

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	/* Clear any TKIP key we have */
	if ((priv->has_wpa) && (priv->encode_alg == IW_ENCODE_ALG_TKIP))
		(void) orinoco_clear_tkip_key(priv, setindex);

	if (erq->length > 0) {
		if ((index < 0) || (index >= ORINOCO_MAX_KEYS))
			index = priv->tx_key;

		/* Adjust key length to a supported value */
		if (erq->length > SMALL_KEY_SIZE) {
			xlen = LARGE_KEY_SIZE;
		} else if (erq->length > 0) {
			xlen = SMALL_KEY_SIZE;
		} else
			xlen = 0;

		/* Switch on WEP if off */
		if ((encode_alg != IW_ENCODE_ALG_WEP) && (xlen > 0)) {
			setindex = index;
			encode_alg = IW_ENCODE_ALG_WEP;
		}
	} else {
		/* Important note : if the user do "iwconfig eth0 enc off",
		 * we will arrive there with an index of -1. This is valid
		 * but need to be taken care off... Jean II */
		if ((index < 0) || (index >= ORINOCO_MAX_KEYS)) {
			if((index != -1) || (erq->flags == 0)) {
				err = -EINVAL;
				goto out;
			}
		} else {
			/* Set the index : Check that the key is valid */
			if(priv->keys[index].len == 0) {
				err = -EINVAL;
				goto out;
			}
			setindex = index;
		}
	}

	if (erq->flags & IW_ENCODE_DISABLED)
		encode_alg = IW_ENCODE_ALG_NONE;
	if (erq->flags & IW_ENCODE_OPEN)
		restricted = 0;
	if (erq->flags & IW_ENCODE_RESTRICTED)
		restricted = 1;

	if (erq->pointer && erq->length > 0) {
		priv->keys[index].len = cpu_to_le16(xlen);
		memset(priv->keys[index].data, 0,
		       sizeof(priv->keys[index].data));
		memcpy(priv->keys[index].data, keybuf, erq->length);
	}
	priv->tx_key = setindex;

	/* Try fast key change if connected and only keys are changed */
	if ((priv->encode_alg == encode_alg) &&
	    (priv->wep_restrict == restricted) &&
	    netif_carrier_ok(dev)) {
		err = __orinoco_hw_setup_wepkeys(priv);
		/* No need to commit if successful */
		goto out;
	}

	priv->encode_alg = encode_alg;
	priv->wep_restrict = restricted;

 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getiwencode(struct net_device *dev,
				     struct iw_request_info *info,
				     struct iw_point *erq,
				     char *keybuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int index = (erq->flags & IW_ENCODE_INDEX) - 1;
	u16 xlen = 0;
	unsigned long flags;

	if (! priv->has_wep)
		return -EOPNOTSUPP;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if ((index < 0) || (index >= ORINOCO_MAX_KEYS))
		index = priv->tx_key;

	erq->flags = 0;
	if (!priv->encode_alg)
		erq->flags |= IW_ENCODE_DISABLED;
	erq->flags |= index + 1;

	if (priv->wep_restrict)
		erq->flags |= IW_ENCODE_RESTRICTED;
	else
		erq->flags |= IW_ENCODE_OPEN;

	xlen = le16_to_cpu(priv->keys[index].len);

	erq->length = xlen;

	memcpy(keybuf, priv->keys[index].data, ORINOCO_MAX_KEY_SIZE);

	orinoco_unlock(priv, &flags);
	return 0;
}

static int orinoco_ioctl_setessid(struct net_device *dev,
				  struct iw_request_info *info,
				  struct iw_point *erq,
				  char *essidbuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;

	/* Note : ESSID is ignored in Ad-Hoc demo mode, but we can set it
	 * anyway... - Jean II */

	/* Hum... Should not use Wireless Extension constant (may change),
	 * should use our own... - Jean II */
	if (erq->length > IW_ESSID_MAX_SIZE)
		return -E2BIG;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	/* NULL the string (for NULL termination & ESSID = ANY) - Jean II */
	memset(priv->desired_essid, 0, sizeof(priv->desired_essid));

	/* If not ANY, get the new ESSID */
	if (erq->flags) {
		memcpy(priv->desired_essid, essidbuf, erq->length);
	}

	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_getessid(struct net_device *dev,
				  struct iw_request_info *info,
				  struct iw_point *erq,
				  char *essidbuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int active;
	int err = 0;
	unsigned long flags;

	if (netif_running(dev)) {
		err = orinoco_hw_get_essid(priv, &active, essidbuf);
		if (err < 0)
			return err;
		erq->length = err;
	} else {
		if (orinoco_lock(priv, &flags) != 0)
			return -EBUSY;
		memcpy(essidbuf, priv->desired_essid, IW_ESSID_MAX_SIZE);
		erq->length = strlen(priv->desired_essid);
		orinoco_unlock(priv, &flags);
	}

	erq->flags = 1;

	return 0;
}

static int orinoco_ioctl_setnick(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_point *nrq,
				 char *nickbuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;

	if (nrq->length > IW_ESSID_MAX_SIZE)
		return -E2BIG;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	memset(priv->nick, 0, sizeof(priv->nick));
	memcpy(priv->nick, nickbuf, nrq->length);

	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_getnick(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_point *nrq,
				 char *nickbuf)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	memcpy(nickbuf, priv->nick, IW_ESSID_MAX_SIZE);
	orinoco_unlock(priv, &flags);

	nrq->length = strlen(priv->nick);

	return 0;
}

static int orinoco_ioctl_setfreq(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_freq *frq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int chan = -1;
	unsigned long flags;
	int err = -EINPROGRESS;		/* Call commit handler */

	/* In infrastructure mode the AP sets the channel */
	if (priv->iw_mode == IW_MODE_INFRA)
		return -EBUSY;

	if ( (frq->e == 0) && (frq->m <= 1000) ) {
		/* Setting by channel number */
		chan = frq->m;
	} else {
		/* Setting by frequency - search the table */
		int mult = 1;
		int i;

		for (i = 0; i < (6 - frq->e); i++)
			mult *= 10;

		for (i = 0; i < NUM_CHANNELS; i++)
			if (frq->m == (channel_frequency[i] * mult))
				chan = i+1;
	}

	if ( (chan < 1) || (chan > NUM_CHANNELS) ||
	     ! (priv->channel_mask & (1 << (chan-1)) ) )
		return -EINVAL;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	priv->channel = chan;
	if (priv->iw_mode == IW_MODE_MONITOR) {
		/* Fast channel change - no commit if successful */
		hermes_t *hw = &priv->hw;
		err = hermes_docmd_wait(hw, HERMES_CMD_TEST |
					    HERMES_TEST_SET_CHANNEL,
					chan, NULL);
	}
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getfreq(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_freq *frq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int tmp;

	/* Locking done in there */
	tmp = orinoco_hw_get_freq(priv);
	if (tmp < 0) {
		return tmp;
	}

	frq->m = tmp;
	frq->e = 1;

	return 0;
}

static int orinoco_ioctl_getsens(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *srq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	u16 val;
	int err;
	unsigned long flags;

	if (!priv->has_sensitivity)
		return -EOPNOTSUPP;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	err = hermes_read_wordrec(hw, USER_BAP,
				  HERMES_RID_CNFSYSTEMSCALE, &val);
	orinoco_unlock(priv, &flags);

	if (err)
		return err;

	srq->value = val;
	srq->fixed = 0; /* auto */

	return 0;
}

static int orinoco_ioctl_setsens(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *srq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int val = srq->value;
	unsigned long flags;

	if (!priv->has_sensitivity)
		return -EOPNOTSUPP;

	if ((val < 1) || (val > 3))
		return -EINVAL;
	
	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	priv->ap_density = val;
	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_setrts(struct net_device *dev,
				struct iw_request_info *info,
				struct iw_param *rrq,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int val = rrq->value;
	unsigned long flags;

	if (rrq->disabled)
		val = 2347;

	if ( (val < 0) || (val > 2347) )
		return -EINVAL;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	priv->rts_thresh = val;
	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_getrts(struct net_device *dev,
				struct iw_request_info *info,
				struct iw_param *rrq,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);

	rrq->value = priv->rts_thresh;
	rrq->disabled = (rrq->value == 2347);
	rrq->fixed = 1;

	return 0;
}

static int orinoco_ioctl_setfrag(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *frq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = -EINPROGRESS;		/* Call commit handler */
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if (priv->has_mwo) {
		if (frq->disabled)
			priv->mwo_robust = 0;
		else {
			if (frq->fixed)
				printk(KERN_WARNING "%s: Fixed fragmentation is "
				       "not supported on this firmware. "
				       "Using MWO robust instead.\n", dev->name);
			priv->mwo_robust = 1;
		}
	} else {
		if (frq->disabled)
			priv->frag_thresh = 2346;
		else {
			if ( (frq->value < 256) || (frq->value > 2346) )
				err = -EINVAL;
			else
				priv->frag_thresh = frq->value & ~0x1; /* must be even */
		}
	}

	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getfrag(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *frq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err;
	u16 val;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	
	if (priv->has_mwo) {
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CNFMWOROBUST_AGERE,
					  &val);
		if (err)
			val = 0;

		frq->value = val ? 2347 : 0;
		frq->disabled = ! val;
		frq->fixed = 0;
	} else {
		err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFFRAGMENTATIONTHRESHOLD,
					  &val);
		if (err)
			val = 0;

		frq->value = val;
		frq->disabled = (val >= 2346);
		frq->fixed = 1;
	}

	orinoco_unlock(priv, &flags);
	
	return err;
}

static int orinoco_ioctl_setrate(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *rrq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int ratemode = -1;
	int bitrate; /* 100s of kilobits */
	int i;
	unsigned long flags;
	
	/* As the user space doesn't know our highest rate, it uses -1
	 * to ask us to set the highest rate.  Test it using "iwconfig
	 * ethX rate auto" - Jean II */
	if (rrq->value == -1)
		bitrate = 110;
	else {
		if (rrq->value % 100000)
			return -EINVAL;
		bitrate = rrq->value / 100000;
	}

	if ( (bitrate != 10) && (bitrate != 20) &&
	     (bitrate != 55) && (bitrate != 110) )
		return -EINVAL;

	for (i = 0; i < BITRATE_TABLE_SIZE; i++)
		if ( (bitrate_table[i].bitrate == bitrate) &&
		     (bitrate_table[i].automatic == ! rrq->fixed) ) {
			ratemode = i;
			break;
		}
	
	if (ratemode == -1)
		return -EINVAL;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	priv->bitratemode = ratemode;
	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;
}

static int orinoco_ioctl_getrate(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_param *rrq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err = 0;
	int ratemode;
	int i;
	u16 val;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	ratemode = priv->bitratemode;

	BUG_ON((ratemode < 0) || (ratemode >= BITRATE_TABLE_SIZE));

	rrq->value = bitrate_table[ratemode].bitrate * 100000;
	rrq->fixed = ! bitrate_table[ratemode].automatic;
	rrq->disabled = 0;

	/* If the interface is running we try to find more about the
	   current mode */
	if (netif_running(dev)) {
		err = hermes_read_wordrec(hw, USER_BAP,
					  HERMES_RID_CURRENTTXRATE, &val);
		if (err)
			goto out;
		
		switch (priv->firmware_type) {
		case FIRMWARE_TYPE_AGERE: /* Lucent style rate */
			/* Note : in Lucent firmware, the return value of
			 * HERMES_RID_CURRENTTXRATE is the bitrate in Mb/s,
			 * and therefore is totally different from the
			 * encoding of HERMES_RID_CNFTXRATECONTROL.
			 * Don't forget that 6Mb/s is really 5.5Mb/s */
			if (val == 6)
				rrq->value = 5500000;
			else
				rrq->value = val * 1000000;
			break;
		case FIRMWARE_TYPE_INTERSIL: /* Intersil style rate */
		case FIRMWARE_TYPE_SYMBOL: /* Symbol style rate */
			for (i = 0; i < BITRATE_TABLE_SIZE; i++)
				if (bitrate_table[i].intersil_txratectrl == val) {
					ratemode = i;
					break;
				}
			if (i >= BITRATE_TABLE_SIZE)
				printk(KERN_INFO "%s: Unable to determine current bitrate (0x%04hx)\n",
				       dev->name, val);

			rrq->value = bitrate_table[ratemode].bitrate * 100000;
			break;
		default:
			BUG();
		}
	}

 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_setpower(struct net_device *dev,
				  struct iw_request_info *info,
				  struct iw_param *prq,
				  char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = -EINPROGRESS;		/* Call commit handler */
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if (prq->disabled) {
		priv->pm_on = 0;
	} else {
		switch (prq->flags & IW_POWER_MODE) {
		case IW_POWER_UNICAST_R:
			priv->pm_mcast = 0;
			priv->pm_on = 1;
			break;
		case IW_POWER_ALL_R:
			priv->pm_mcast = 1;
			priv->pm_on = 1;
			break;
		case IW_POWER_ON:
			/* No flags : but we may have a value - Jean II */
			break;
		default:
			err = -EINVAL;
			goto out;
		}
		
		if (prq->flags & IW_POWER_TIMEOUT) {
			priv->pm_on = 1;
			priv->pm_timeout = prq->value / 1000;
		}
		if (prq->flags & IW_POWER_PERIOD) {
			priv->pm_on = 1;
			priv->pm_period = prq->value / 1000;
		}
		/* It's valid to not have a value if we are just toggling
		 * the flags... Jean II */
		if(!priv->pm_on) {
			err = -EINVAL;
			goto out;
		}			
	}

 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getpower(struct net_device *dev,
				  struct iw_request_info *info,
				  struct iw_param *prq,
				  char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err = 0;
	u16 enable, period, timeout, mcast;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFPMENABLED, &enable);
	if (err)
		goto out;

	err = hermes_read_wordrec(hw, USER_BAP,
				  HERMES_RID_CNFMAXSLEEPDURATION, &period);
	if (err)
		goto out;

	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFPMHOLDOVERDURATION, &timeout);
	if (err)
		goto out;

	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_CNFMULTICASTRECEIVE, &mcast);
	if (err)
		goto out;

	prq->disabled = !enable;
	/* Note : by default, display the period */
	if ((prq->flags & IW_POWER_TYPE) == IW_POWER_TIMEOUT) {
		prq->flags = IW_POWER_TIMEOUT;
		prq->value = timeout * 1000;
	} else {
		prq->flags = IW_POWER_PERIOD;
		prq->value = period * 1000;
	}
	if (mcast)
		prq->flags |= IW_POWER_ALL_R;
	else
		prq->flags |= IW_POWER_UNICAST_R;

 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_set_encodeext(struct net_device *dev,
				       struct iw_request_info *info,
				       union iwreq_data *wrqu,
				       char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct iw_point *encoding = &wrqu->encoding;
	struct iw_encode_ext *ext = (struct iw_encode_ext *)extra;
	int idx, alg = ext->alg, set_key = 1;
	unsigned long flags;
	int err = -EINVAL;
	u16 key_len;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	/* Determine and validate the key index */
	idx = encoding->flags & IW_ENCODE_INDEX;
	if (idx) {
		if ((idx < 1) || (idx > WEP_KEYS))
			goto out;
		idx--;
	} else
		idx = priv->tx_key;

	if (encoding->flags & IW_ENCODE_DISABLED)
	    alg = IW_ENCODE_ALG_NONE;

	if (priv->has_wpa && (alg != IW_ENCODE_ALG_TKIP)) {
		/* Clear any TKIP TX key we had */
		(void) orinoco_clear_tkip_key(priv, priv->tx_key);
	}

	if (ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY) {
		priv->tx_key = idx;
		set_key = ((alg == IW_ENCODE_ALG_TKIP) ||
			   (ext->key_len > 0)) ? 1 : 0;
	}

	if (set_key) {
		/* Set the requested key first */
		switch (alg) {
		case IW_ENCODE_ALG_NONE:
			priv->encode_alg = alg;
			priv->keys[idx].len = 0;
			break;

		case IW_ENCODE_ALG_WEP:
			if (ext->key_len > SMALL_KEY_SIZE)
				key_len = LARGE_KEY_SIZE;
			else if (ext->key_len > 0)
				key_len = SMALL_KEY_SIZE;
			else
				goto out;

			priv->encode_alg = alg;
			priv->keys[idx].len = cpu_to_le16(key_len);

			key_len = min(ext->key_len, key_len);

			memset(priv->keys[idx].data, 0, ORINOCO_MAX_KEY_SIZE);
			memcpy(priv->keys[idx].data, ext->key, key_len);
			break;

		case IW_ENCODE_ALG_TKIP:
		{
			hermes_t *hw = &priv->hw;
			u8 *tkip_iv = NULL;

			if (!priv->has_wpa ||
			    (ext->key_len > sizeof(priv->tkip_key[0])))
				goto out;

			priv->encode_alg = alg;
			memset(&priv->tkip_key[idx], 0,
			       sizeof(priv->tkip_key[idx]));
			memcpy(&priv->tkip_key[idx], ext->key, ext->key_len);

			if (ext->ext_flags & IW_ENCODE_EXT_RX_SEQ_VALID)
				tkip_iv = &ext->rx_seq[0];

			err = __orinoco_hw_set_tkip_key(hw, idx,
				 ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY,
				 (u8 *) &priv->tkip_key[idx],
				 tkip_iv, NULL);
			if (err)
				printk(KERN_ERR "%s: Error %d setting TKIP key"
				       "\n", dev->name, err);

			goto out;
		}
		default:
			goto out;
		}
	}
	err = -EINPROGRESS;
 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_get_encodeext(struct net_device *dev,
				       struct iw_request_info *info,
				       union iwreq_data *wrqu,
				       char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct iw_point *encoding = &wrqu->encoding;
	struct iw_encode_ext *ext = (struct iw_encode_ext *)extra;
	int idx, max_key_len;
	unsigned long flags;
	int err;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	err = -EINVAL;
	max_key_len = encoding->length - sizeof(*ext);
	if (max_key_len < 0)
		goto out;

	idx = encoding->flags & IW_ENCODE_INDEX;
	if (idx) {
		if ((idx < 1) || (idx > WEP_KEYS))
			goto out;
		idx--;
	} else
		idx = priv->tx_key;

	encoding->flags = idx + 1;
	memset(ext, 0, sizeof(*ext));

	ext->alg = priv->encode_alg;
	switch (priv->encode_alg) {
	case IW_ENCODE_ALG_NONE:
		ext->key_len = 0;
		encoding->flags |= IW_ENCODE_DISABLED;
		break;
	case IW_ENCODE_ALG_WEP:
		ext->key_len = min_t(u16, le16_to_cpu(priv->keys[idx].len),
				     max_key_len);
		memcpy(ext->key, priv->keys[idx].data, ext->key_len);
		encoding->flags |= IW_ENCODE_ENABLED;
		break;
	case IW_ENCODE_ALG_TKIP:
		ext->key_len = min_t(u16, sizeof(struct orinoco_tkip_key),
				     max_key_len);
		memcpy(ext->key, &priv->tkip_key[idx], ext->key_len);
		encoding->flags |= IW_ENCODE_ENABLED;
		break;
	}

	err = 0;
 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_set_auth(struct net_device *dev,
				  struct iw_request_info *info,
				  union iwreq_data *wrqu, char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	struct iw_param *param = &wrqu->param;
	unsigned long flags;
	int ret = -EINPROGRESS;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	switch (param->flags & IW_AUTH_INDEX) {
	case IW_AUTH_WPA_VERSION:
	case IW_AUTH_CIPHER_PAIRWISE:
	case IW_AUTH_CIPHER_GROUP:
	case IW_AUTH_RX_UNENCRYPTED_EAPOL:
	case IW_AUTH_PRIVACY_INVOKED:
	case IW_AUTH_DROP_UNENCRYPTED:
		/*
		 * orinoco does not use these parameters
		 */
		break;

	case IW_AUTH_KEY_MGMT:
		/* wl_lkm implies value 2 == PSK for Hermes I
		 * which ties in with WEXT
		 * no other hints tho :(
		 */
		priv->key_mgmt = param->value;
		break;

	case IW_AUTH_TKIP_COUNTERMEASURES:
		/* When countermeasures are enabled, shut down the
		 * card; when disabled, re-enable the card. This must
		 * take effect immediately.
		 *
		 * TODO: Make sure that the EAPOL message is getting
		 *       out before card disabled
		 */
		if (param->value) {
			priv->tkip_cm_active = 1;
			ret = hermes_enable_port(hw, 0);
		} else {
			priv->tkip_cm_active = 0;
			ret = hermes_disable_port(hw, 0);
		}
		break;

	case IW_AUTH_80211_AUTH_ALG:
		if (param->value & IW_AUTH_ALG_SHARED_KEY)
			priv->wep_restrict = 1;
		else if (param->value & IW_AUTH_ALG_OPEN_SYSTEM)
			priv->wep_restrict = 0;
		else
			ret = -EINVAL;
		break;

	case IW_AUTH_WPA_ENABLED:
		if (priv->has_wpa) {
			priv->wpa_enabled = param->value ? 1 : 0;
		} else {
			if (param->value)
				ret = -EOPNOTSUPP;
			/* else silently accept disable of WPA */
			priv->wpa_enabled = 0;
		}
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	orinoco_unlock(priv, &flags);
	return ret;
}

static int orinoco_ioctl_get_auth(struct net_device *dev,
				  struct iw_request_info *info,
				  union iwreq_data *wrqu, char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct iw_param *param = &wrqu->param;
	unsigned long flags;
	int ret = 0;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	switch (param->flags & IW_AUTH_INDEX) {
	case IW_AUTH_KEY_MGMT:
		param->value = priv->key_mgmt;
		break;

	case IW_AUTH_TKIP_COUNTERMEASURES:
		param->value = priv->tkip_cm_active;
		break;

	case IW_AUTH_80211_AUTH_ALG:
		if (priv->wep_restrict)
			param->value = IW_AUTH_ALG_SHARED_KEY;
		else
			param->value = IW_AUTH_ALG_OPEN_SYSTEM;
		break;

	case IW_AUTH_WPA_ENABLED:
		param->value = priv->wpa_enabled;
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	orinoco_unlock(priv, &flags);
	return ret;
}

static int orinoco_ioctl_set_genie(struct net_device *dev,
				   struct iw_request_info *info,
				   union iwreq_data *wrqu, char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	u8 *buf;
	unsigned long flags;

	if ((wrqu->data.length > MAX_WPA_IE_LEN) ||
	    (wrqu->data.length && (extra == NULL)))
		return -EINVAL;

	if (wrqu->data.length) {
		buf = kmalloc(wrqu->data.length, GFP_KERNEL);
		if (buf == NULL)
			return -ENOMEM;

		memcpy(buf, extra, wrqu->data.length);
	} else
		buf = NULL;

	if (orinoco_lock(priv, &flags) != 0) {
		kfree(buf);
		return -EBUSY;
	}

	kfree(priv->wpa_ie);
	priv->wpa_ie = buf;
	priv->wpa_ie_len = wrqu->data.length;

	if (priv->wpa_ie) {
		/* Looks like wl_lkm wants to check the auth alg, and
		 * somehow pass it to the firmware.
		 * Instead it just calls the key mgmt rid
		 *   - we do this in set auth.
		 */
	}

	orinoco_unlock(priv, &flags);
	return 0;
}

static int orinoco_ioctl_get_genie(struct net_device *dev,
				   struct iw_request_info *info,
				   union iwreq_data *wrqu, char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;
	int err = 0;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if ((priv->wpa_ie_len == 0) || (priv->wpa_ie == NULL)) {
		wrqu->data.length = 0;
		goto out;
	}

	if (wrqu->data.length < priv->wpa_ie_len) {
		err = -E2BIG;
		goto out;
	}

	wrqu->data.length = priv->wpa_ie_len;
	memcpy(extra, priv->wpa_ie, priv->wpa_ie_len);

out:
	orinoco_unlock(priv, &flags);
	return err;
}

static int orinoco_ioctl_set_mlme(struct net_device *dev,
				  struct iw_request_info *info,
				  union iwreq_data *wrqu, char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	struct iw_mlme *mlme = (struct iw_mlme *)extra;
	unsigned long flags;
	int ret = 0;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	switch (mlme->cmd) {
	case IW_MLME_DEAUTH:
		/* silently ignore */
		break;

	case IW_MLME_DISASSOC:
	{
		struct {
			u8 addr[ETH_ALEN];
			__le16 reason_code;
		} __attribute__ ((packed)) buf;

		memcpy(buf.addr, mlme->addr.sa_data, ETH_ALEN);
		buf.reason_code = cpu_to_le16(mlme->reason_code);
		ret = HERMES_WRITE_RECORD(hw, USER_BAP,
					  HERMES_RID_CNFDISASSOCIATE,
					  &buf);
		break;
	}
	default:
		ret = -EOPNOTSUPP;
	}

	orinoco_unlock(priv, &flags);
	return ret;
}

static int orinoco_ioctl_getretry(struct net_device *dev,
				  struct iw_request_info *info,
				  struct iw_param *rrq,
				  char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int err = 0;
	u16 short_limit, long_limit, lifetime;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;
	
	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_SHORTRETRYLIMIT,
				  &short_limit);
	if (err)
		goto out;

	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_LONGRETRYLIMIT,
				  &long_limit);
	if (err)
		goto out;

	err = hermes_read_wordrec(hw, USER_BAP, HERMES_RID_MAXTRANSMITLIFETIME,
				  &lifetime);
	if (err)
		goto out;

	rrq->disabled = 0;		/* Can't be disabled */

	/* Note : by default, display the retry number */
	if ((rrq->flags & IW_RETRY_TYPE) == IW_RETRY_LIFETIME) {
		rrq->flags = IW_RETRY_LIFETIME;
		rrq->value = lifetime * 1000;	/* ??? */
	} else {
		/* By default, display the min number */
		if ((rrq->flags & IW_RETRY_LONG)) {
			rrq->flags = IW_RETRY_LIMIT | IW_RETRY_LONG;
			rrq->value = long_limit;
		} else {
			rrq->flags = IW_RETRY_LIMIT;
			rrq->value = short_limit;
			if(short_limit != long_limit)
				rrq->flags |= IW_RETRY_SHORT;
		}
	}

 out:
	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_reset(struct net_device *dev,
			       struct iw_request_info *info,
			       void *wrqu,
			       char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);

	if (! capable(CAP_NET_ADMIN))
		return -EPERM;

	if (info->cmd == (SIOCIWFIRSTPRIV + 0x1)) {
		printk(KERN_DEBUG "%s: Forcing reset!\n", dev->name);

		/* Firmware reset */
		orinoco_reset(&priv->reset_work);
	} else {
		printk(KERN_DEBUG "%s: Force scheduling reset!\n", dev->name);

		schedule_work(&priv->reset_work);
	}

	return 0;
}

static int orinoco_ioctl_setibssport(struct net_device *dev,
				     struct iw_request_info *info,
				     void *wrqu,
				     char *extra)

{
	struct orinoco_private *priv = netdev_priv(dev);
	int val = *( (int *) extra );
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	priv->ibss_port = val ;

	/* Actually update the mode we are using */
	set_port_type(priv);

	orinoco_unlock(priv, &flags);
	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_getibssport(struct net_device *dev,
				     struct iw_request_info *info,
				     void *wrqu,
				     char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int *val = (int *) extra;

	*val = priv->ibss_port;
	return 0;
}

static int orinoco_ioctl_setport3(struct net_device *dev,
				  struct iw_request_info *info,
				  void *wrqu,
				  char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int val = *( (int *) extra );
	int err = 0;
	unsigned long flags;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	switch (val) {
	case 0: /* Try to do IEEE ad-hoc mode */
		if (! priv->has_ibss) {
			err = -EINVAL;
			break;
		}
		priv->prefer_port3 = 0;
			
		break;

	case 1: /* Try to do Lucent proprietary ad-hoc mode */
		if (! priv->has_port3) {
			err = -EINVAL;
			break;
		}
		priv->prefer_port3 = 1;
		break;

	default:
		err = -EINVAL;
	}

	if (! err) {
		/* Actually update the mode we are using */
		set_port_type(priv);
		err = -EINPROGRESS;
	}

	orinoco_unlock(priv, &flags);

	return err;
}

static int orinoco_ioctl_getport3(struct net_device *dev,
				  struct iw_request_info *info,
				  void *wrqu,
				  char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int *val = (int *) extra;

	*val = priv->prefer_port3;
	return 0;
}

static int orinoco_ioctl_setpreamble(struct net_device *dev,
				     struct iw_request_info *info,
				     void *wrqu,
				     char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	unsigned long flags;
	int val;

	if (! priv->has_preamble)
		return -EOPNOTSUPP;

	/* 802.11b has recently defined some short preamble.
	 * Basically, the Phy header has been reduced in size.
	 * This increase performance, especially at high rates
	 * (the preamble is transmitted at 1Mb/s), unfortunately
	 * this give compatibility troubles... - Jean II */
	val = *( (int *) extra );

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if (val)
		priv->preamble = 1;
	else
		priv->preamble = 0;

	orinoco_unlock(priv, &flags);

	return -EINPROGRESS;		/* Call commit handler */
}

static int orinoco_ioctl_getpreamble(struct net_device *dev,
				     struct iw_request_info *info,
				     void *wrqu,
				     char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int *val = (int *) extra;

	if (! priv->has_preamble)
		return -EOPNOTSUPP;

	*val = priv->preamble;
	return 0;
}

/* ioctl interface to hermes_read_ltv()
 * To use with iwpriv, pass the RID as the token argument, e.g.
 * iwpriv get_rid [0xfc00]
 * At least Wireless Tools 25 is required to use iwpriv.
 * For Wireless Tools 25 and 26 append "dummy" are the end. */
static int orinoco_ioctl_getrid(struct net_device *dev,
				struct iw_request_info *info,
				struct iw_point *data,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	int rid = data->flags;
	u16 length;
	int err;
	unsigned long flags;

	/* It's a "get" function, but we don't want users to access the
	 * WEP key and other raw firmware data */
	if (! capable(CAP_NET_ADMIN))
		return -EPERM;

	if (rid < 0xfc00 || rid > 0xffff)
		return -EINVAL;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	err = hermes_read_ltv(hw, USER_BAP, rid, MAX_RID_LEN, &length,
			      extra);
	if (err)
		goto out;

	data->length = min_t(u16, HERMES_RECLEN_TO_BYTES(length),
			     MAX_RID_LEN);

 out:
	orinoco_unlock(priv, &flags);
	return err;
}

/* Trigger a scan (look for other cells in the vicinity) */
static int orinoco_ioctl_setscan(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_point *srq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	hermes_t *hw = &priv->hw;
	struct iw_scan_req *si = (struct iw_scan_req *) extra;
	int err = 0;
	unsigned long flags;

	/* Note : you may have realised that, as this is a SET operation,
	 * this is privileged and therefore a normal user can't
	 * perform scanning.
	 * This is not an error, while the device perform scanning,
	 * traffic doesn't flow, so it's a perfect DoS...
	 * Jean II */

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	/* Scanning with port 0 disabled would fail */
	if (!netif_running(dev)) {
		err = -ENETDOWN;
		goto out;
	}

	/* In monitor mode, the scan results are always empty.
	 * Probe responses are passed to the driver as received
	 * frames and could be processed in software. */
	if (priv->iw_mode == IW_MODE_MONITOR) {
		err = -EOPNOTSUPP;
		goto out;
	}

	/* Note : because we don't lock out the irq handler, the way
	 * we access scan variables in priv is critical.
	 *	o scan_inprogress : not touched by irq handler
	 *	o scan_mode : not touched by irq handler
	 * Before modifying anything on those variables, please think hard !
	 * Jean II */

	/* Save flags */
	priv->scan_mode = srq->flags;

	/* Always trigger scanning, even if it's in progress.
	 * This way, if the info frame get lost, we will recover somewhat
	 * gracefully  - Jean II */

	if (priv->has_hostscan) {
		switch (priv->firmware_type) {
		case FIRMWARE_TYPE_SYMBOL:
			err = hermes_write_wordrec(hw, USER_BAP,
						   HERMES_RID_CNFHOSTSCAN_SYMBOL,
						   HERMES_HOSTSCAN_SYMBOL_ONCE |
						   HERMES_HOSTSCAN_SYMBOL_BCAST);
			break;
		case FIRMWARE_TYPE_INTERSIL: {
			__le16 req[3];

			req[0] = cpu_to_le16(0x3fff);	/* All channels */
			req[1] = cpu_to_le16(0x0001);	/* rate 1 Mbps */
			req[2] = 0;			/* Any ESSID */
			err = HERMES_WRITE_RECORD(hw, USER_BAP,
						  HERMES_RID_CNFHOSTSCAN, &req);
		}
		break;
		case FIRMWARE_TYPE_AGERE:
			if (priv->scan_mode & IW_SCAN_THIS_ESSID) {
				struct hermes_idstring idbuf;
				size_t len = min(sizeof(idbuf.val),
						 (size_t) si->essid_len);
				idbuf.len = cpu_to_le16(len);
				memcpy(idbuf.val, si->essid, len);

				err = hermes_write_ltv(hw, USER_BAP,
					       HERMES_RID_CNFSCANSSID_AGERE,
					       HERMES_BYTES_TO_RECLEN(len + 2),
					       &idbuf);
			} else
				err = hermes_write_wordrec(hw, USER_BAP,
						   HERMES_RID_CNFSCANSSID_AGERE,
						   0);	/* Any ESSID */
			if (err)
				break;

			if (priv->has_ext_scan) {
				/* Clear scan results at the start of
				 * an extended scan */
				orinoco_clear_scan_results(priv,
						msecs_to_jiffies(15000));

				/* TODO: Is this available on older firmware?
				 *   Can we use it to scan specific channels
				 *   for IW_SCAN_THIS_FREQ? */
				err = hermes_write_wordrec(hw, USER_BAP,
						HERMES_RID_CNFSCANCHANNELS2GHZ,
						0x7FFF);
				if (err)
					goto out;

				err = hermes_inquire(hw,
						     HERMES_INQ_CHANNELINFO);
			} else
				err = hermes_inquire(hw, HERMES_INQ_SCAN);
			break;
		}
	} else
		err = hermes_inquire(hw, HERMES_INQ_SCAN);

	/* One more client */
	if (! err)
		priv->scan_inprogress = 1;

 out:
	orinoco_unlock(priv, &flags);
	return err;
}

#define MAX_CUSTOM_LEN 64

/* Translate scan data returned from the card to a card independant
 * format that the Wireless Tools will understand - Jean II */
static inline char *orinoco_translate_scan(struct net_device *dev,
					   struct iw_request_info *info,
					   char *current_ev,
					   char *end_buf,
					   union hermes_scan_info *bss,
					   unsigned int last_scanned)
{
	struct orinoco_private *priv = netdev_priv(dev);
	u16			capabilities;
	u16			channel;
	struct iw_event		iwe;		/* Temporary buffer */
	char custom[MAX_CUSTOM_LEN];

	memset(&iwe, 0, sizeof(iwe));

	/* First entry *MUST* be the AP MAC address */
	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	memcpy(iwe.u.ap_addr.sa_data, bss->a.bssid, ETH_ALEN);
	current_ev = iwe_stream_add_event(info, current_ev, end_buf,
					  &iwe, IW_EV_ADDR_LEN);

	/* Other entries will be displayed in the order we give them */

	/* Add the ESSID */
	iwe.u.data.length = le16_to_cpu(bss->a.essid_len);
	if (iwe.u.data.length > 32)
		iwe.u.data.length = 32;
	iwe.cmd = SIOCGIWESSID;
	iwe.u.data.flags = 1;
	current_ev = iwe_stream_add_point(info, current_ev, end_buf,
					  &iwe, bss->a.essid);

	/* Add mode */
	iwe.cmd = SIOCGIWMODE;
	capabilities = le16_to_cpu(bss->a.capabilities);
	if (capabilities & (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS)) {
		if (capabilities & WLAN_CAPABILITY_ESS)
			iwe.u.mode = IW_MODE_MASTER;
		else
			iwe.u.mode = IW_MODE_ADHOC;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_UINT_LEN);
	}

	channel = bss->s.channel;
	if ((channel >= 1) && (channel <= NUM_CHANNELS)) {
		/* Add channel and frequency */
		iwe.cmd = SIOCGIWFREQ;
		iwe.u.freq.m = channel;
		iwe.u.freq.e = 0;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_FREQ_LEN);

		iwe.u.freq.m = channel_frequency[channel-1] * 100000;
		iwe.u.freq.e = 1;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_FREQ_LEN);
	}

	/* Add quality statistics. level and noise in dB. No link quality */
	iwe.cmd = IWEVQUAL;
	iwe.u.qual.updated = IW_QUAL_DBM | IW_QUAL_QUAL_INVALID;
	iwe.u.qual.level = (__u8) le16_to_cpu(bss->a.level) - 0x95;
	iwe.u.qual.noise = (__u8) le16_to_cpu(bss->a.noise) - 0x95;
	/* Wireless tools prior to 27.pre22 will show link quality
	 * anyway, so we provide a reasonable value. */
	if (iwe.u.qual.level > iwe.u.qual.noise)
		iwe.u.qual.qual = iwe.u.qual.level - iwe.u.qual.noise;
	else
		iwe.u.qual.qual = 0;
	current_ev = iwe_stream_add_event(info, current_ev, end_buf,
					  &iwe, IW_EV_QUAL_LEN);

	/* Add encryption capability */
	iwe.cmd = SIOCGIWENCODE;
	if (capabilities & WLAN_CAPABILITY_PRIVACY)
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	else
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	iwe.u.data.length = 0;
	current_ev = iwe_stream_add_point(info, current_ev, end_buf,
					  &iwe, NULL);

	/* Bit rate is not available in Lucent/Agere firmwares */
	if (priv->firmware_type != FIRMWARE_TYPE_AGERE) {
		char *current_val = current_ev + iwe_stream_lcp_len(info);
		int i;
		int step;

		if (priv->firmware_type == FIRMWARE_TYPE_SYMBOL)
			step = 2;
		else
			step = 1;

		iwe.cmd = SIOCGIWRATE;
		/* Those two flags are ignored... */
		iwe.u.bitrate.fixed = iwe.u.bitrate.disabled = 0;
		/* Max 10 values */
		for (i = 0; i < 10; i += step) {
			/* NULL terminated */
			if (bss->p.rates[i] == 0x0)
				break;
			/* Bit rate given in 500 kb/s units (+ 0x80) */
			iwe.u.bitrate.value =
				((bss->p.rates[i] & 0x7f) * 500000);
			current_val = iwe_stream_add_value(info, current_ev,
							   current_val,
							   end_buf, &iwe,
							   IW_EV_PARAM_LEN);
		}
		/* Check if we added any event */
		if ((current_val - current_ev) > iwe_stream_lcp_len(info))
			current_ev = current_val;
	}

	/* Beacon interval */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     "bcn_int=%d",
				     le16_to_cpu(bss->a.beacon_interv));
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	/* Capabilites */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     "capab=0x%04x",
				     capabilities);
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	/* Add EXTRA: Age to display seconds since last beacon/probe response
	 * for given network. */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     " Last beacon: %dms ago",
				     jiffies_to_msecs(jiffies - last_scanned));
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	return current_ev;
}

static inline char *orinoco_translate_ext_scan(struct net_device *dev,
					       struct iw_request_info *info,
					       char *current_ev,
					       char *end_buf,
					       struct agere_ext_scan_info *bss,
					       unsigned int last_scanned)
{
	u16			capabilities;
	u16			channel;
	struct iw_event		iwe;		/* Temporary buffer */
	char custom[MAX_CUSTOM_LEN];
	u8 *ie;

	memset(&iwe, 0, sizeof(iwe));

	/* First entry *MUST* be the AP MAC address */
	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	memcpy(iwe.u.ap_addr.sa_data, bss->bssid, ETH_ALEN);
	current_ev = iwe_stream_add_event(info, current_ev, end_buf,
					  &iwe, IW_EV_ADDR_LEN);

	/* Other entries will be displayed in the order we give them */

	/* Add the ESSID */
	ie = bss->data;
	iwe.u.data.length = ie[1];
	if (iwe.u.data.length) {
		if (iwe.u.data.length > 32)
			iwe.u.data.length = 32;
		iwe.cmd = SIOCGIWESSID;
		iwe.u.data.flags = 1;
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, &ie[2]);
	}

	/* Add mode */
	capabilities = le16_to_cpu(bss->capabilities);
	if (capabilities & (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS)) {
		iwe.cmd = SIOCGIWMODE;
		if (capabilities & WLAN_CAPABILITY_ESS)
			iwe.u.mode = IW_MODE_MASTER;
		else
			iwe.u.mode = IW_MODE_ADHOC;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_UINT_LEN);
	}

	ie = orinoco_get_ie(bss->data, sizeof(bss->data), MFIE_TYPE_DS_SET);
	channel = ie ? ie[2] : 0;
	if ((channel >= 1) && (channel <= NUM_CHANNELS)) {
		/* Add channel and frequency */
		iwe.cmd = SIOCGIWFREQ;
		iwe.u.freq.m = channel;
		iwe.u.freq.e = 0;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_FREQ_LEN);

		iwe.u.freq.m = channel_frequency[channel-1] * 100000;
		iwe.u.freq.e = 1;
		current_ev = iwe_stream_add_event(info, current_ev, end_buf,
						  &iwe, IW_EV_FREQ_LEN);
	}

	/* Add quality statistics. level and noise in dB. No link quality */
	iwe.cmd = IWEVQUAL;
	iwe.u.qual.updated = IW_QUAL_DBM | IW_QUAL_QUAL_INVALID;
	iwe.u.qual.level = bss->level - 0x95;
	iwe.u.qual.noise = bss->noise - 0x95;
	/* Wireless tools prior to 27.pre22 will show link quality
	 * anyway, so we provide a reasonable value. */
	if (iwe.u.qual.level > iwe.u.qual.noise)
		iwe.u.qual.qual = iwe.u.qual.level - iwe.u.qual.noise;
	else
		iwe.u.qual.qual = 0;
	current_ev = iwe_stream_add_event(info, current_ev, end_buf,
					  &iwe, IW_EV_QUAL_LEN);

	/* Add encryption capability */
	iwe.cmd = SIOCGIWENCODE;
	if (capabilities & WLAN_CAPABILITY_PRIVACY)
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	else
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	iwe.u.data.length = 0;
	current_ev = iwe_stream_add_point(info, current_ev, end_buf,
					  &iwe, NULL);

	/* WPA IE */
	ie = orinoco_get_wpa_ie(bss->data, sizeof(bss->data));
	if (ie) {
		iwe.cmd = IWEVGENIE;
		iwe.u.data.length = ie[1] + 2;
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, ie);
	}

	/* RSN IE */
	ie = orinoco_get_ie(bss->data, sizeof(bss->data), MFIE_TYPE_RSN);
	if (ie) {
		iwe.cmd = IWEVGENIE;
		iwe.u.data.length = ie[1] + 2;
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, ie);
	}

	ie = orinoco_get_ie(bss->data, sizeof(bss->data), MFIE_TYPE_RATES);
	if (ie) {
		char *p = current_ev + iwe_stream_lcp_len(info);
		int i;

		iwe.cmd = SIOCGIWRATE;
		/* Those two flags are ignored... */
		iwe.u.bitrate.fixed = iwe.u.bitrate.disabled = 0;

		for (i = 2; i < (ie[1] + 2); i++) {
			iwe.u.bitrate.value = ((ie[i] & 0x7F) * 500000);
			p = iwe_stream_add_value(info, current_ev, p, end_buf,
						 &iwe, IW_EV_PARAM_LEN);
		}
		/* Check if we added any event */
		if (p > (current_ev + iwe_stream_lcp_len(info)))
			current_ev = p;
	}

	/* Timestamp */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length =
		snprintf(custom, MAX_CUSTOM_LEN, "tsf=%016llx",
			 (unsigned long long) le64_to_cpu(bss->timestamp));
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	/* Beacon interval */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     "bcn_int=%d",
				     le16_to_cpu(bss->beacon_interval));
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	/* Capabilites */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     "capab=0x%04x",
				     capabilities);
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	/* Add EXTRA: Age to display seconds since last beacon/probe response
	 * for given network. */
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = snprintf(custom, MAX_CUSTOM_LEN,
				     " Last beacon: %dms ago",
				     jiffies_to_msecs(jiffies - last_scanned));
	if (iwe.u.data.length)
		current_ev = iwe_stream_add_point(info, current_ev, end_buf,
						  &iwe, custom);

	return current_ev;
}

/* Return results of a scan */
static int orinoco_ioctl_getscan(struct net_device *dev,
				 struct iw_request_info *info,
				 struct iw_point *srq,
				 char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	int err = 0;
	unsigned long flags;
	char *current_ev = extra;

	if (orinoco_lock(priv, &flags) != 0)
		return -EBUSY;

	if (priv->scan_inprogress) {
		/* Important note : we don't want to block the caller
		 * until results are ready for various reasons.
		 * First, managing wait queues is complex and racy.
		 * Second, we grab some rtnetlink lock before comming
		 * here (in dev_ioctl()).
		 * Third, we generate an Wireless Event, so the
		 * caller can wait itself on that - Jean II */
		err = -EAGAIN;
		goto out;
	}

	if (priv->has_ext_scan) {
		struct xbss_element *bss;

		list_for_each_entry(bss, &priv->bss_list, list) {
			/* Translate this entry to WE format */
			current_ev =
				orinoco_translate_ext_scan(dev, info,
							   current_ev,
							   extra + srq->length,
							   &bss->bss,
							   bss->last_scanned);

			/* Check if there is space for one more entry */
			if ((extra + srq->length - current_ev)
			    <= IW_EV_ADDR_LEN) {
				/* Ask user space to try again with a
				 * bigger buffer */
				err = -E2BIG;
				goto out;
			}
		}

	} else {
		struct bss_element *bss;

		list_for_each_entry(bss, &priv->bss_list, list) {
			/* Translate this entry to WE format */
			current_ev = orinoco_translate_scan(dev, info,
							    current_ev,
							    extra + srq->length,
							    &bss->bss,
							    bss->last_scanned);

			/* Check if there is space for one more entry */
			if ((extra + srq->length - current_ev)
			    <= IW_EV_ADDR_LEN) {
				/* Ask user space to try again with a
				 * bigger buffer */
				err = -E2BIG;
				goto out;
			}
		}
	}

	srq->length = (current_ev - extra);
	srq->flags = (__u16) priv->scan_mode;

out:
	orinoco_unlock(priv, &flags);
	return err;
}

/* Commit handler, called after set operations */
static int orinoco_ioctl_commit(struct net_device *dev,
				struct iw_request_info *info,
				void *wrqu,
				char *extra)
{
	struct orinoco_private *priv = netdev_priv(dev);
	struct hermes *hw = &priv->hw;
	unsigned long flags;
	int err = 0;

	if (!priv->open)
		return 0;

	if (priv->broken_disableport) {
		orinoco_reset(&priv->reset_work);
		return 0;
	}

	if (orinoco_lock(priv, &flags) != 0)
		return err;

	err = hermes_disable_port(hw, 0);
	if (err) {
		printk(KERN_WARNING "%s: Unable to disable port "
		       "while reconfiguring card\n", dev->name);
		priv->broken_disableport = 1;
		goto out;
	}

	err = __orinoco_program_rids(dev);
	if (err) {
		printk(KERN_WARNING "%s: Unable to reconfigure card\n",
		       dev->name);
		goto out;
	}

	err = hermes_enable_port(hw, 0);
	if (err) {
		printk(KERN_WARNING "%s: Unable to enable port while reconfiguring card\n",
		       dev->name);
		goto out;
	}

 out:
	if (err) {
		printk(KERN_WARNING "%s: Resetting instead...\n", dev->name);
		schedule_work(&priv->reset_work);
		err = 0;
	}

	orinoco_unlock(priv, &flags);
	return err;
}

static const struct iw_priv_args orinoco_privtab[] = {
	{ SIOCIWFIRSTPRIV + 0x0, 0, 0, "force_reset" },
	{ SIOCIWFIRSTPRIV + 0x1, 0, 0, "card_reset" },
	{ SIOCIWFIRSTPRIV + 0x2, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  0, "set_port3" },
	{ SIOCIWFIRSTPRIV + 0x3, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  "get_port3" },
	{ SIOCIWFIRSTPRIV + 0x4, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  0, "set_preamble" },
	{ SIOCIWFIRSTPRIV + 0x5, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  "get_preamble" },
	{ SIOCIWFIRSTPRIV + 0x6, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  0, "set_ibssport" },
	{ SIOCIWFIRSTPRIV + 0x7, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	  "get_ibssport" },
	{ SIOCIWFIRSTPRIV + 0x9, 0, IW_PRIV_TYPE_BYTE | MAX_RID_LEN,
	  "get_rid" },
};


/*
 * Structures to export the Wireless Handlers
 */

#define STD_IW_HANDLER(id, func) \
	[IW_IOCTL_IDX(id)] = (iw_handler) func
static const iw_handler	orinoco_handler[] = {
	STD_IW_HANDLER(SIOCSIWCOMMIT,	orinoco_ioctl_commit),
	STD_IW_HANDLER(SIOCGIWNAME,	orinoco_ioctl_getname),
	STD_IW_HANDLER(SIOCSIWFREQ,	orinoco_ioctl_setfreq),
	STD_IW_HANDLER(SIOCGIWFREQ,	orinoco_ioctl_getfreq),
	STD_IW_HANDLER(SIOCSIWMODE,	orinoco_ioctl_setmode),
	STD_IW_HANDLER(SIOCGIWMODE,	orinoco_ioctl_getmode),
	STD_IW_HANDLER(SIOCSIWSENS,	orinoco_ioctl_setsens),
	STD_IW_HANDLER(SIOCGIWSENS,	orinoco_ioctl_getsens),
	STD_IW_HANDLER(SIOCGIWRANGE,	orinoco_ioctl_getiwrange),
	STD_IW_HANDLER(SIOCSIWSPY,	iw_handler_set_spy),
	STD_IW_HANDLER(SIOCGIWSPY,	iw_handler_get_spy),
	STD_IW_HANDLER(SIOCSIWTHRSPY,	iw_handler_set_thrspy),
	STD_IW_HANDLER(SIOCGIWTHRSPY,	iw_handler_get_thrspy),
	STD_IW_HANDLER(SIOCSIWAP,	orinoco_ioctl_setwap),
	STD_IW_HANDLER(SIOCGIWAP,	orinoco_ioctl_getwap),
	STD_IW_HANDLER(SIOCSIWSCAN,	orinoco_ioctl_setscan),
	STD_IW_HANDLER(SIOCGIWSCAN,	orinoco_ioctl_getscan),
	STD_IW_HANDLER(SIOCSIWESSID,	orinoco_ioctl_setessid),
	STD_IW_HANDLER(SIOCGIWESSID,	orinoco_ioctl_getessid),
	STD_IW_HANDLER(SIOCSIWNICKN,	orinoco_ioctl_setnick),
	STD_IW_HANDLER(SIOCGIWNICKN,	orinoco_ioctl_getnick),
	STD_IW_HANDLER(SIOCSIWRATE,	orinoco_ioctl_setrate),
	STD_IW_HANDLER(SIOCGIWRATE,	orinoco_ioctl_getrate),
	STD_IW_HANDLER(SIOCSIWRTS,	orinoco_ioctl_setrts),
	STD_IW_HANDLER(SIOCGIWRTS,	orinoco_ioctl_getrts),
	STD_IW_HANDLER(SIOCSIWFRAG,	orinoco_ioctl_setfrag),
	STD_IW_HANDLER(SIOCGIWFRAG,	orinoco_ioctl_getfrag),
	STD_IW_HANDLER(SIOCGIWRETRY,	orinoco_ioctl_getretry),
	STD_IW_HANDLER(SIOCSIWENCODE,	orinoco_ioctl_setiwencode),
	STD_IW_HANDLER(SIOCGIWENCODE,	orinoco_ioctl_getiwencode),
	STD_IW_HANDLER(SIOCSIWPOWER,	orinoco_ioctl_setpower),
	STD_IW_HANDLER(SIOCGIWPOWER,	orinoco_ioctl_getpower),
	STD_IW_HANDLER(SIOCSIWGENIE,	orinoco_ioctl_set_genie),
	STD_IW_HANDLER(SIOCGIWGENIE,	orinoco_ioctl_get_genie),
	STD_IW_HANDLER(SIOCSIWMLME,	orinoco_ioctl_set_mlme),
	STD_IW_HANDLER(SIOCSIWAUTH,	orinoco_ioctl_set_auth),
	STD_IW_HANDLER(SIOCGIWAUTH,	orinoco_ioctl_get_auth),
	STD_IW_HANDLER(SIOCSIWENCODEEXT, orinoco_ioctl_set_encodeext),
	STD_IW_HANDLER(SIOCGIWENCODEEXT, orinoco_ioctl_get_encodeext),
};


/*
  Added typecasting since we no longer use iwreq_data -- Moustafa
 */
static const iw_handler	orinoco_private_handler[] = {
	[0] = (iw_handler) orinoco_ioctl_reset,
	[1] = (iw_handler) orinoco_ioctl_reset,
	[2] = (iw_handler) orinoco_ioctl_setport3,
	[3] = (iw_handler) orinoco_ioctl_getport3,
	[4] = (iw_handler) orinoco_ioctl_setpreamble,
	[5] = (iw_handler) orinoco_ioctl_getpreamble,
	[6] = (iw_handler) orinoco_ioctl_setibssport,
	[7] = (iw_handler) orinoco_ioctl_getibssport,
	[9] = (iw_handler) orinoco_ioctl_getrid,
};

static const struct iw_handler_def orinoco_handler_def = {
	.num_standard = ARRAY_SIZE(orinoco_handler),
	.num_private = ARRAY_SIZE(orinoco_private_handler),
	.num_private_args = ARRAY_SIZE(orinoco_privtab),
	.standard = orinoco_handler,
	.private = orinoco_private_handler,
	.private_args = orinoco_privtab,
	.get_wireless_stats = orinoco_get_wireless_stats,
};

static void orinoco_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	struct orinoco_private *priv = netdev_priv(dev);

	strncpy(info->driver, DRIVER_NAME, sizeof(info->driver) - 1);
	strncpy(info->version, DRIVER_VERSION, sizeof(info->version) - 1);
	strncpy(info->fw_version, priv->fw_name, sizeof(info->fw_version) - 1);
	if (dev->dev.parent)
		strncpy(info->bus_info, dev->dev.parent->bus_id,
			sizeof(info->bus_info) - 1);
	else
		snprintf(info->bus_info, sizeof(info->bus_info) - 1,
			 "PCMCIA %p", priv->hw.iobase);
}

static const struct ethtool_ops orinoco_ethtool_ops = {
	.get_drvinfo = orinoco_get_drvinfo,
	.get_link = ethtool_op_get_link,
};

/********************************************************************/
/* Module initialization                                            */
/********************************************************************/

EXPORT_SYMBOL(alloc_orinocodev);
EXPORT_SYMBOL(free_orinocodev);

EXPORT_SYMBOL(__orinoco_up);
EXPORT_SYMBOL(__orinoco_down);
EXPORT_SYMBOL(orinoco_reinit_firmware);

EXPORT_SYMBOL(orinoco_interrupt);

/* Can't be declared "const" or the whole __initdata section will
 * become const */
static char version[] __initdata = DRIVER_NAME " " DRIVER_VERSION
	" (David Gibson <hermes@gibson.dropbear.id.au>, "
	"Pavel Roskin <proski@gnu.org>, et al)";

static int __init init_orinoco(void)
{
	printk(KERN_DEBUG "%s\n", version);
	return 0;
}

static void __exit exit_orinoco(void)
{
}

module_init(init_orinoco);
module_exit(exit_orinoco);
