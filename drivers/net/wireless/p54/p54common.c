
/*
 * Common code for mac80211 Prism54 drivers
 *
 * Copyright (c) 2006, Michael Wu <flamingice@sourmilk.net>
 * Copyright (c) 2007, Christian Lamparter <chunkeey@web.de>
 *
 * Based on the islsm (softmac prism54) driver, which is:
 * Copyright 2004-2006 Jean-Baptiste Note <jbnote@gmail.com>, et al.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/etherdevice.h>

#include <net/mac80211.h>

#include "p54.h"
#include "p54common.h"

MODULE_AUTHOR("Michael Wu <flamingice@sourmilk.net>");
MODULE_DESCRIPTION("Softmac Prism54 common code");
MODULE_LICENSE("GPL");
MODULE_ALIAS("prism54common");

static struct ieee80211_rate p54_bgrates[] = {
	{ .bitrate = 10, .hw_value = 0, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 20, .hw_value = 1, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .hw_value = 2, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 3, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60, .hw_value = 4, },
	{ .bitrate = 90, .hw_value = 5, },
	{ .bitrate = 120, .hw_value = 6, },
	{ .bitrate = 180, .hw_value = 7, },
	{ .bitrate = 240, .hw_value = 8, },
	{ .bitrate = 360, .hw_value = 9, },
	{ .bitrate = 480, .hw_value = 10, },
	{ .bitrate = 540, .hw_value = 11, },
};

static struct ieee80211_channel p54_bgchannels[] = {
	{ .center_freq = 2412, .hw_value = 1, },
	{ .center_freq = 2417, .hw_value = 2, },
	{ .center_freq = 2422, .hw_value = 3, },
	{ .center_freq = 2427, .hw_value = 4, },
	{ .center_freq = 2432, .hw_value = 5, },
	{ .center_freq = 2437, .hw_value = 6, },
	{ .center_freq = 2442, .hw_value = 7, },
	{ .center_freq = 2447, .hw_value = 8, },
	{ .center_freq = 2452, .hw_value = 9, },
	{ .center_freq = 2457, .hw_value = 10, },
	{ .center_freq = 2462, .hw_value = 11, },
	{ .center_freq = 2467, .hw_value = 12, },
	{ .center_freq = 2472, .hw_value = 13, },
	{ .center_freq = 2484, .hw_value = 14, },
};

static struct ieee80211_supported_band band_2GHz = {
	.channels = p54_bgchannels,
	.n_channels = ARRAY_SIZE(p54_bgchannels),
	.bitrates = p54_bgrates,
	.n_bitrates = ARRAY_SIZE(p54_bgrates),
};

static struct ieee80211_rate p54_arates[] = {
	{ .bitrate = 60, .hw_value = 4, },
	{ .bitrate = 90, .hw_value = 5, },
	{ .bitrate = 120, .hw_value = 6, },
	{ .bitrate = 180, .hw_value = 7, },
	{ .bitrate = 240, .hw_value = 8, },
	{ .bitrate = 360, .hw_value = 9, },
	{ .bitrate = 480, .hw_value = 10, },
	{ .bitrate = 540, .hw_value = 11, },
};

static struct ieee80211_channel p54_achannels[] = {
	{ .center_freq = 4920 },
	{ .center_freq = 4940 },
	{ .center_freq = 4960 },
	{ .center_freq = 4980 },
	{ .center_freq = 5040 },
	{ .center_freq = 5060 },
	{ .center_freq = 5080 },
	{ .center_freq = 5170 },
	{ .center_freq = 5180 },
	{ .center_freq = 5190 },
	{ .center_freq = 5200 },
	{ .center_freq = 5210 },
	{ .center_freq = 5220 },
	{ .center_freq = 5230 },
	{ .center_freq = 5240 },
	{ .center_freq = 5260 },
	{ .center_freq = 5280 },
	{ .center_freq = 5300 },
	{ .center_freq = 5320 },
	{ .center_freq = 5500 },
	{ .center_freq = 5520 },
	{ .center_freq = 5540 },
	{ .center_freq = 5560 },
	{ .center_freq = 5580 },
	{ .center_freq = 5600 },
	{ .center_freq = 5620 },
	{ .center_freq = 5640 },
	{ .center_freq = 5660 },
	{ .center_freq = 5680 },
	{ .center_freq = 5700 },
	{ .center_freq = 5745 },
	{ .center_freq = 5765 },
	{ .center_freq = 5785 },
	{ .center_freq = 5805 },
	{ .center_freq = 5825 },
};

static struct ieee80211_supported_band band_5GHz = {
	.channels = p54_achannels,
	.n_channels = ARRAY_SIZE(p54_achannels),
	.bitrates = p54_arates,
	.n_bitrates = ARRAY_SIZE(p54_arates),
};

int p54_parse_firmware(struct ieee80211_hw *dev, const struct firmware *fw)
{
	struct p54_common *priv = dev->priv;
	struct bootrec_exp_if *exp_if;
	struct bootrec *bootrec;
	u32 *data = (u32 *)fw->data;
	u32 *end_data = (u32 *)fw->data + (fw->size >> 2);
	u8 *fw_version = NULL;
	size_t len;
	int i;

	if (priv->rx_start)
		return 0;

	while (data < end_data && *data)
		data++;

	while (data < end_data && !*data)
		data++;

	bootrec = (struct bootrec *) data;

	while (bootrec->data <= end_data &&
	       (bootrec->data + (len = le32_to_cpu(bootrec->len))) <= end_data) {
		u32 code = le32_to_cpu(bootrec->code);
		switch (code) {
		case BR_CODE_COMPONENT_ID:
			priv->fw_interface = be32_to_cpup((__be32 *)
					     bootrec->data);
			switch (priv->fw_interface) {
			case FW_FMAC:
				printk(KERN_INFO "p54: FreeMAC firmware\n");
				break;
			case FW_LM20:
				printk(KERN_INFO "p54: LM20 firmware\n");
				break;
			case FW_LM86:
				printk(KERN_INFO "p54: LM86 firmware\n");
				break;
			case FW_LM87:
				printk(KERN_INFO "p54: LM87 firmware\n");
				break;
			default:
				printk(KERN_INFO "p54: unknown firmware\n");
				break;
			}
			break;
		case BR_CODE_COMPONENT_VERSION:
			/* 24 bytes should be enough for all firmwares */
			if (strnlen((unsigned char*)bootrec->data, 24) < 24)
				fw_version = (unsigned char*)bootrec->data;
			break;
		case BR_CODE_DESCR: {
			struct bootrec_desc *desc =
				(struct bootrec_desc *)bootrec->data;
			priv->rx_start = le32_to_cpu(desc->rx_start);
			/* FIXME add sanity checking */
			priv->rx_end = le32_to_cpu(desc->rx_end) - 0x3500;
			priv->headroom = desc->headroom;
			priv->tailroom = desc->tailroom;
			if (le32_to_cpu(bootrec->len) == 11)
				priv->rx_mtu = le16_to_cpu(bootrec->rx_mtu);
			else
				priv->rx_mtu = (size_t)
					0x620 - priv->tx_hdr_len;
			break;
			}
		case BR_CODE_EXPOSED_IF:
			exp_if = (struct bootrec_exp_if *) bootrec->data;
			for (i = 0; i < (len * sizeof(*exp_if) / 4); i++)
				if (exp_if[i].if_id == cpu_to_le16(0x1a))
					priv->fw_var = le16_to_cpu(exp_if[i].variant);
			break;
		case BR_CODE_DEPENDENT_IF:
			break;
		case BR_CODE_END_OF_BRA:
		case LEGACY_BR_CODE_END_OF_BRA:
			end_data = NULL;
			break;
		default:
			break;
		}
		bootrec = (struct bootrec *)&bootrec->data[len];
	}

	if (fw_version)
		printk(KERN_INFO "p54: FW rev %s - Softmac protocol %x.%x\n",
			fw_version, priv->fw_var >> 8, priv->fw_var & 0xff);

	if (priv->fw_var >= 0x300) {
		/* Firmware supports QoS, use it! */
		priv->tx_stats[4].limit = 3;
		priv->tx_stats[5].limit = 4;
		priv->tx_stats[6].limit = 3;
		priv->tx_stats[7].limit = 1;
		dev->queues = 4;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(p54_parse_firmware);

static int p54_convert_rev0(struct ieee80211_hw *dev,
			    struct pda_pa_curve_data *curve_data)
{
	struct p54_common *priv = dev->priv;
	struct p54_pa_curve_data_sample *dst;
	struct pda_pa_curve_data_sample_rev0 *src;
	size_t cd_len = sizeof(*curve_data) +
		(curve_data->points_per_channel*sizeof(*dst) + 2) *
		 curve_data->channels;
	unsigned int i, j;
	void *source, *target;

	priv->curve_data = kmalloc(cd_len, GFP_KERNEL);
	if (!priv->curve_data)
		return -ENOMEM;

	memcpy(priv->curve_data, curve_data, sizeof(*curve_data));
	source = curve_data->data;
	target = priv->curve_data->data;
	for (i = 0; i < curve_data->channels; i++) {
		__le16 *freq = source;
		source += sizeof(__le16);
		*((__le16 *)target) = *freq;
		target += sizeof(__le16);
		for (j = 0; j < curve_data->points_per_channel; j++) {
			dst = target;
			src = source;

			dst->rf_power = src->rf_power;
			dst->pa_detector = src->pa_detector;
			dst->data_64qam = src->pcv;
			/* "invent" the points for the other modulations */
#define SUB(x,y) (u8)((x) - (y)) > (x) ? 0 : (x) - (y)
			dst->data_16qam = SUB(src->pcv, 12);
			dst->data_qpsk = SUB(dst->data_16qam, 12);
			dst->data_bpsk = SUB(dst->data_qpsk, 12);
			dst->data_barker = SUB(dst->data_bpsk, 14);
#undef SUB
			target += sizeof(*dst);
			source += sizeof(*src);
		}
	}

	return 0;
}

static int p54_convert_rev1(struct ieee80211_hw *dev,
			    struct pda_pa_curve_data *curve_data)
{
	struct p54_common *priv = dev->priv;
	struct p54_pa_curve_data_sample *dst;
	struct pda_pa_curve_data_sample_rev1 *src;
	size_t cd_len = sizeof(*curve_data) +
		(curve_data->points_per_channel*sizeof(*dst) + 2) *
		 curve_data->channels;
	unsigned int i, j;
	void *source, *target;

	priv->curve_data = kmalloc(cd_len, GFP_KERNEL);
	if (!priv->curve_data)
		return -ENOMEM;

	memcpy(priv->curve_data, curve_data, sizeof(*curve_data));
	source = curve_data->data;
	target = priv->curve_data->data;
	for (i = 0; i < curve_data->channels; i++) {
		__le16 *freq = source;
		source += sizeof(__le16);
		*((__le16 *)target) = *freq;
		target += sizeof(__le16);
		for (j = 0; j < curve_data->points_per_channel; j++) {
			memcpy(target, source, sizeof(*src));

			target += sizeof(*dst);
			source += sizeof(*src);
		}
		source++;
	}

	return 0;
}

static const char *p54_rf_chips[] = { "NULL", "Duette3", "Duette2",
                              "Frisbee", "Xbow", "Longbow", "NULL", "NULL" };
static int p54_init_xbow_synth(struct ieee80211_hw *dev);

static int p54_parse_eeprom(struct ieee80211_hw *dev, void *eeprom, int len)
{
	struct p54_common *priv = dev->priv;
	struct eeprom_pda_wrap *wrap = NULL;
	struct pda_entry *entry;
	unsigned int data_len, entry_len;
	void *tmp;
	int err;
	u8 *end = (u8 *)eeprom + len;
	u16 synth = 0;
	DECLARE_MAC_BUF(mac);

	wrap = (struct eeprom_pda_wrap *) eeprom;
	entry = (void *)wrap->data + le16_to_cpu(wrap->len);

	/* verify that at least the entry length/code fits */
	while ((u8 *)entry <= end - sizeof(*entry)) {
		entry_len = le16_to_cpu(entry->len);
		data_len = ((entry_len - 1) << 1);

		/* abort if entry exceeds whole structure */
		if ((u8 *)entry + sizeof(*entry) + data_len > end)
			break;

		switch (le16_to_cpu(entry->code)) {
		case PDR_MAC_ADDRESS:
			SET_IEEE80211_PERM_ADDR(dev, entry->data);
			break;
		case PDR_PRISM_PA_CAL_OUTPUT_POWER_LIMITS:
			if (data_len < 2) {
				err = -EINVAL;
				goto err;
			}

			if (2 + entry->data[1]*sizeof(*priv->output_limit) > data_len) {
				err = -EINVAL;
				goto err;
			}

			priv->output_limit = kmalloc(entry->data[1] *
				sizeof(*priv->output_limit), GFP_KERNEL);

			if (!priv->output_limit) {
				err = -ENOMEM;
				goto err;
			}

			memcpy(priv->output_limit, &entry->data[2],
			       entry->data[1]*sizeof(*priv->output_limit));
			priv->output_limit_len = entry->data[1];
			break;
		case PDR_PRISM_PA_CAL_CURVE_DATA: {
			struct pda_pa_curve_data *curve_data =
				(struct pda_pa_curve_data *)entry->data;
			if (data_len < sizeof(*curve_data)) {
				err = -EINVAL;
				goto err;
			}

			switch (curve_data->cal_method_rev) {
			case 0:
				err = p54_convert_rev0(dev, curve_data);
				break;
			case 1:
				err = p54_convert_rev1(dev, curve_data);
				break;
			default:
				printk(KERN_ERR "p54: unknown curve data "
						"revision %d\n",
						curve_data->cal_method_rev);
				err = -ENODEV;
				break;
			}
			if (err)
				goto err;

		}
		case PDR_PRISM_ZIF_TX_IQ_CALIBRATION:
			priv->iq_autocal = kmalloc(data_len, GFP_KERNEL);
			if (!priv->iq_autocal) {
				err = -ENOMEM;
				goto err;
			}

			memcpy(priv->iq_autocal, entry->data, data_len);
			priv->iq_autocal_len = data_len / sizeof(struct pda_iq_autocal_entry);
			break;
		case PDR_INTERFACE_LIST:
			tmp = entry->data;
			while ((u8 *)tmp < entry->data + data_len) {
				struct bootrec_exp_if *exp_if = tmp;
				if (le16_to_cpu(exp_if->if_id) == 0xf)
					synth = le16_to_cpu(exp_if->variant);
				tmp += sizeof(struct bootrec_exp_if);
			}
			break;
		case PDR_HARDWARE_PLATFORM_COMPONENT_ID:
			priv->version = *(u8 *)(entry->data + 1);
			break;
		case PDR_END:
			/* make it overrun */
			entry_len = len;
			break;
		default:
			printk(KERN_INFO "p54: unknown eeprom code : 0x%x\n",
				le16_to_cpu(entry->code));
			break;
		}

		entry = (void *)entry + (entry_len + 1)*2;
	}

	if (!synth || !priv->iq_autocal || !priv->output_limit ||
	    !priv->curve_data) {
		printk(KERN_ERR "p54: not all required entries found in eeprom!\n");
		err = -EINVAL;
		goto err;
	}

	priv->rxhw = synth & 0x07;
	if (priv->rxhw == 4)
		p54_init_xbow_synth(dev);
	if (!(synth & 0x40))
		dev->wiphy->bands[IEEE80211_BAND_2GHZ] = &band_2GHz;
	if (!(synth & 0x80))
		dev->wiphy->bands[IEEE80211_BAND_5GHZ] = &band_5GHz;

	if (!is_valid_ether_addr(dev->wiphy->perm_addr)) {
		u8 perm_addr[ETH_ALEN];

		printk(KERN_WARNING "%s: Invalid hwaddr! Using randomly generated MAC addr\n",
			wiphy_name(dev->wiphy));
		random_ether_addr(perm_addr);
		SET_IEEE80211_PERM_ADDR(dev, perm_addr);
	}

	printk(KERN_INFO "%s: hwaddr %s, MAC:isl38%02x RF:%s\n",
		wiphy_name(dev->wiphy),
		print_mac(mac, dev->wiphy->perm_addr),
		priv->version, p54_rf_chips[priv->rxhw]);

	return 0;

  err:
	if (priv->iq_autocal) {
		kfree(priv->iq_autocal);
		priv->iq_autocal = NULL;
	}

	if (priv->output_limit) {
		kfree(priv->output_limit);
		priv->output_limit = NULL;
	}

	if (priv->curve_data) {
		kfree(priv->curve_data);
		priv->curve_data = NULL;
	}

	printk(KERN_ERR "p54: eeprom parse failed!\n");
	return err;
}

static int p54_rssi_to_dbm(struct ieee80211_hw *dev, int rssi)
{
	/* TODO: get the rssi_add & rssi_mul data from the eeprom */
	return ((rssi * 0x83) / 64 - 400) / 4;
}

static int p54_rx_data(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct p54_common *priv = dev->priv;
	struct p54_rx_hdr *hdr = (struct p54_rx_hdr *) skb->data;
	struct ieee80211_rx_status rx_status = {0};
	u16 freq = le16_to_cpu(hdr->freq);
	size_t header_len = sizeof(*hdr);
	u32 tsf32;

	if (!(hdr->magic & cpu_to_le16(0x0001))) {
		if (priv->filter_flags & FIF_FCSFAIL)
			rx_status.flag |= RX_FLAG_FAILED_FCS_CRC;
		else
			return 0;
	}

	rx_status.signal = p54_rssi_to_dbm(dev, hdr->rssi);
	rx_status.noise = priv->noise;
	/* XX correct? */
	rx_status.qual = (100 * hdr->rssi) / 127;
	rx_status.rate_idx = (dev->conf.channel->band == IEEE80211_BAND_2GHZ ?
			hdr->rate : (hdr->rate - 4)) & 0xf;
	rx_status.freq = freq;
	rx_status.band =  dev->conf.channel->band;
	rx_status.antenna = hdr->antenna;

	tsf32 = le32_to_cpu(hdr->tsf32);
	if (tsf32 < priv->tsf_low32)
		priv->tsf_high32++;
	rx_status.mactime = ((u64)priv->tsf_high32) << 32 | tsf32;
	priv->tsf_low32 = tsf32;

	rx_status.flag |= RX_FLAG_TSFT;

	if (hdr->magic & cpu_to_le16(0x4000))
		header_len += hdr->align[0];

	skb_pull(skb, header_len);
	skb_trim(skb, le16_to_cpu(hdr->len));

	ieee80211_rx_irqsafe(dev, skb, &rx_status);

	return -1;
}

static void inline p54_wake_free_queues(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	int i;

	for (i = 0; i < dev->queues; i++)
		if (priv->tx_stats[i + 4].len < priv->tx_stats[i + 4].limit)
			ieee80211_wake_queue(dev, i);
}

static void p54_rx_frame_sent(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr = (struct p54_control_hdr *) skb->data;
	struct p54_frame_sent_hdr *payload = (struct p54_frame_sent_hdr *) hdr->data;
	struct sk_buff *entry = (struct sk_buff *) priv->tx_queue.next;
	u32 addr = le32_to_cpu(hdr->req_id) - priv->headroom;
	struct memrecord *range = NULL;
	u32 freed = 0;
	u32 last_addr = priv->rx_start;
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_queue.lock, flags);
	while (entry != (struct sk_buff *)&priv->tx_queue) {
		struct ieee80211_tx_info *info = IEEE80211_SKB_CB(entry);
		range = (void *)info->driver_data;
		if (range->start_addr == addr) {
			struct p54_control_hdr *entry_hdr;
			struct p54_tx_control_allocdata *entry_data;
			int pad = 0;

			if (entry->next != (struct sk_buff *)&priv->tx_queue) {
				struct ieee80211_tx_info *ni;
				struct memrecord *mr;

				ni = IEEE80211_SKB_CB(entry->next);
				mr = (struct memrecord *)ni->driver_data;
				freed = mr->start_addr - last_addr;
			} else
				freed = priv->rx_end - last_addr;

			last_addr = range->end_addr;
			__skb_unlink(entry, &priv->tx_queue);
			spin_unlock_irqrestore(&priv->tx_queue.lock, flags);

			memset(&info->status, 0, sizeof(info->status));
			entry_hdr = (struct p54_control_hdr *) entry->data;
			entry_data = (struct p54_tx_control_allocdata *) entry_hdr->data;
			if ((entry_hdr->magic1 & cpu_to_le16(0x4000)) != 0)
				pad = entry_data->align[0];

			priv->tx_stats[entry_data->hw_queue].len--;
			if (!(info->flags & IEEE80211_TX_CTL_NO_ACK)) {
				if (!(payload->status & 0x01))
					info->flags |= IEEE80211_TX_STAT_ACK;
				else
					info->status.excessive_retries = 1;
			}
			info->status.retry_count = payload->retries - 1;
			info->status.ack_signal = p54_rssi_to_dbm(dev,
					le16_to_cpu(payload->ack_rssi));
			skb_pull(entry, sizeof(*hdr) + pad + sizeof(*entry_data));
			ieee80211_tx_status_irqsafe(dev, entry);
			goto out;
		} else
			last_addr = range->end_addr;
		entry = entry->next;
	}
	spin_unlock_irqrestore(&priv->tx_queue.lock, flags);

out:
	if (freed >= IEEE80211_MAX_RTS_THRESHOLD + 0x170 +
	    sizeof(struct p54_control_hdr))
		p54_wake_free_queues(dev);
}

static void p54_rx_eeprom_readback(struct ieee80211_hw *dev,
				   struct sk_buff *skb)
{
	struct p54_control_hdr *hdr = (struct p54_control_hdr *) skb->data;
	struct p54_eeprom_lm86 *eeprom = (struct p54_eeprom_lm86 *) hdr->data;
	struct p54_common *priv = dev->priv;

	if (!priv->eeprom)
		return ;

	memcpy(priv->eeprom, eeprom->data, le16_to_cpu(eeprom->len));

	complete(&priv->eeprom_comp);
}

static void p54_rx_stats(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr = (struct p54_control_hdr *) skb->data;
	struct p54_statistics *stats = (struct p54_statistics *) hdr->data;
	u32 tsf32 = le32_to_cpu(stats->tsf32);

	if (tsf32 < priv->tsf_low32)
		priv->tsf_high32++;
	priv->tsf_low32 = tsf32;

	priv->stats.dot11RTSFailureCount = le32_to_cpu(stats->rts_fail);
	priv->stats.dot11RTSSuccessCount = le32_to_cpu(stats->rts_success);
	priv->stats.dot11FCSErrorCount = le32_to_cpu(stats->rx_bad_fcs);

	priv->noise = p54_rssi_to_dbm(dev, le32_to_cpu(stats->noise));
	complete(&priv->stats_comp);

	mod_timer(&priv->stats_timer, jiffies + 5 * HZ);
}

static int p54_rx_control(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct p54_control_hdr *hdr = (struct p54_control_hdr *) skb->data;

	switch (le16_to_cpu(hdr->type)) {
	case P54_CONTROL_TYPE_TXDONE:
		p54_rx_frame_sent(dev, skb);
		break;
	case P54_CONTROL_TYPE_BBP:
		break;
	case P54_CONTROL_TYPE_STAT_READBACK:
		p54_rx_stats(dev, skb);
		break;
	case P54_CONTROL_TYPE_EEPROM_READBACK:
		p54_rx_eeprom_readback(dev, skb);
		break;
	default:
		printk(KERN_DEBUG "%s: not handling 0x%02x type control frame\n",
		       wiphy_name(dev->wiphy), le16_to_cpu(hdr->type));
		break;
	}

	return 0;
}

/* returns zero if skb can be reused */
int p54_rx(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	u8 type = le16_to_cpu(*((__le16 *)skb->data)) >> 8;

	if (type == 0x80)
		return p54_rx_control(dev, skb);
	else
		return p54_rx_data(dev, skb);
}
EXPORT_SYMBOL_GPL(p54_rx);

/*
 * So, the firmware is somewhat stupid and doesn't know what places in its
 * memory incoming data should go to. By poking around in the firmware, we
 * can find some unused memory to upload our packets to. However, data that we
 * want the card to TX needs to stay intact until the card has told us that
 * it is done with it. This function finds empty places we can upload to and
 * marks allocated areas as reserved if necessary. p54_rx_frame_sent frees
 * allocated areas.
 */
static void p54_assign_address(struct ieee80211_hw *dev, struct sk_buff *skb,
			       struct p54_control_hdr *data, u32 len)
{
	struct p54_common *priv = dev->priv;
	struct sk_buff *entry = priv->tx_queue.next;
	struct sk_buff *target_skb = NULL;
	u32 last_addr = priv->rx_start;
	u32 largest_hole = 0;
	u32 target_addr = priv->rx_start;
	unsigned long flags;
	unsigned int left;
	len = (len + priv->headroom + priv->tailroom + 3) & ~0x3;

	spin_lock_irqsave(&priv->tx_queue.lock, flags);
	left = skb_queue_len(&priv->tx_queue);
	while (left--) {
		u32 hole_size;
		struct ieee80211_tx_info *info = IEEE80211_SKB_CB(entry);
		struct memrecord *range = (void *)info->driver_data;
		hole_size = range->start_addr - last_addr;
		if (!target_skb && hole_size >= len) {
			target_skb = entry->prev;
			hole_size -= len;
			target_addr = last_addr;
		}
		largest_hole = max(largest_hole, hole_size);
		last_addr = range->end_addr;
		entry = entry->next;
	}
	if (!target_skb && priv->rx_end - last_addr >= len) {
		target_skb = priv->tx_queue.prev;
		largest_hole = max(largest_hole, priv->rx_end - last_addr - len);
		if (!skb_queue_empty(&priv->tx_queue)) {
			struct ieee80211_tx_info *info = IEEE80211_SKB_CB(target_skb);
			struct memrecord *range = (void *)info->driver_data;
			target_addr = range->end_addr;
		}
	} else
		largest_hole = max(largest_hole, priv->rx_end - last_addr);

	if (skb) {
		struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
		struct memrecord *range = (void *)info->driver_data;
		range->start_addr = target_addr;
		range->end_addr = target_addr + len;
		__skb_queue_after(&priv->tx_queue, target_skb, skb);
		if (largest_hole < priv->rx_mtu + priv->headroom +
				   priv->tailroom +
				   sizeof(struct p54_control_hdr))
			ieee80211_stop_queues(dev);
	}
	spin_unlock_irqrestore(&priv->tx_queue.lock, flags);

	data->req_id = cpu_to_le32(target_addr + priv->headroom);
}

int p54_read_eeprom(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr = NULL, *org_hdr;
	struct p54_eeprom_lm86 *eeprom_hdr;
	size_t eeprom_size = 0x2020, offset = 0, blocksize;
	int ret = -ENOMEM;
	void *eeprom = NULL;

	org_hdr = kzalloc(priv->tx_hdr_len + sizeof(*hdr) +
			  sizeof(*eeprom_hdr) + EEPROM_READBACK_LEN,
			  GFP_KERNEL);
	if (!org_hdr)
		goto free;

	hdr = (void *) org_hdr + priv->tx_hdr_len;
	priv->eeprom = kzalloc(EEPROM_READBACK_LEN, GFP_KERNEL);
	if (!priv->eeprom)
		goto free;

	eeprom = kzalloc(eeprom_size, GFP_KERNEL);
	if (!eeprom)
		goto free;

	hdr->magic1 = cpu_to_le16(0x8000);
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_EEPROM_READBACK);
	hdr->retry1 = hdr->retry2 = 0;
	eeprom_hdr = (struct p54_eeprom_lm86 *) hdr->data;

	while (eeprom_size) {
		blocksize = min(eeprom_size, (size_t)EEPROM_READBACK_LEN);
		hdr->len = cpu_to_le16(blocksize + sizeof(*eeprom_hdr));
		eeprom_hdr->offset = cpu_to_le16(offset);
		eeprom_hdr->len = cpu_to_le16(blocksize);
		p54_assign_address(dev, NULL, hdr, le16_to_cpu(hdr->len) +
				   sizeof(*hdr));
		priv->tx(dev, hdr, le16_to_cpu(hdr->len) + sizeof(*hdr), 0);

		if (!wait_for_completion_interruptible_timeout(&priv->eeprom_comp, HZ)) {
			printk(KERN_ERR "%s: device does not respond!\n",
				wiphy_name(dev->wiphy));
			ret = -EBUSY;
			goto free;
	        }

		memcpy(eeprom + offset, priv->eeprom, blocksize);
		offset += blocksize;
		eeprom_size -= blocksize;
	}

	ret = p54_parse_eeprom(dev, eeprom, offset);
free:
	kfree(priv->eeprom);
	priv->eeprom = NULL;
	kfree(org_hdr);
	kfree(eeprom);

	return ret;
}
EXPORT_SYMBOL_GPL(p54_read_eeprom);

static int p54_tx(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_tx_queue_stats *current_queue;
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct ieee80211_hdr *ieee80211hdr = (struct ieee80211_hdr *)skb->data;
	struct p54_tx_control_allocdata *txhdr;
	size_t padding, len;
	u8 rate;
	u8 cts_rate = 0x20;

	current_queue = &priv->tx_stats[skb_get_queue_mapping(skb) + 4];
	if (unlikely(current_queue->len > current_queue->limit))
		return NETDEV_TX_BUSY;
	current_queue->len++;
	current_queue->count++;
	if (current_queue->len == current_queue->limit)
		ieee80211_stop_queue(dev, skb_get_queue_mapping(skb));

	padding = (unsigned long)(skb->data - (sizeof(*hdr) + sizeof(*txhdr))) & 3;
	len = skb->len;

	txhdr = (struct p54_tx_control_allocdata *)
			skb_push(skb, sizeof(*txhdr) + padding);
	hdr = (struct p54_control_hdr *) skb_push(skb, sizeof(*hdr));

	if (padding)
		hdr->magic1 = cpu_to_le16(0x4010);
	else
		hdr->magic1 = cpu_to_le16(0x0010);
	hdr->len = cpu_to_le16(len);
	hdr->type = (info->flags & IEEE80211_TX_CTL_NO_ACK) ? 0 : cpu_to_le16(1);
	hdr->retry1 = hdr->retry2 = info->control.retry_limit;

	/* TODO: add support for alternate retry TX rates */
	rate = ieee80211_get_tx_rate(dev, info)->hw_value;
	if (info->flags & IEEE80211_TX_CTL_SHORT_PREAMBLE) {
		rate |= 0x10;
		cts_rate |= 0x10;
	}
	if (info->flags & IEEE80211_TX_CTL_USE_RTS_CTS) {
		rate |= 0x40;
		cts_rate |= ieee80211_get_rts_cts_rate(dev, info)->hw_value;
	} else if (info->flags & IEEE80211_TX_CTL_USE_CTS_PROTECT) {
		rate |= 0x20;
		cts_rate |= ieee80211_get_rts_cts_rate(dev, info)->hw_value;
	}
	memset(txhdr->rateset, rate, 8);
	txhdr->key_type = 0;
	txhdr->key_len = 0;
	txhdr->hw_queue = skb_get_queue_mapping(skb) + 4;
	txhdr->tx_antenna = (info->antenna_sel_tx == 0) ?
		2 : info->antenna_sel_tx - 1;
	txhdr->output_power = priv->output_power;
	txhdr->cts_rate = (info->flags & IEEE80211_TX_CTL_NO_ACK) ?
			  0 : cts_rate;
	if (padding)
		txhdr->align[0] = padding;

	/* FIXME: The sequence that follows is needed for this driver to
	 * work with mac80211 since "mac80211: fix TX sequence numbers".
	 * As with the temporary code in rt2x00, changes will be needed
	 * to get proper sequence numbers on beacons. In addition, this
	 * patch places the sequence number in the hardware state, which
	 * limits us to a single virtual state.
	 */
	if (info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ) {
		if (info->flags & IEEE80211_TX_CTL_FIRST_FRAGMENT)
			priv->seqno += 0x10;
		ieee80211hdr->seq_ctrl &= cpu_to_le16(IEEE80211_SCTL_FRAG);
		ieee80211hdr->seq_ctrl |= cpu_to_le16(priv->seqno);
	}
	/* modifies skb->cb and with it info, so must be last! */
	p54_assign_address(dev, skb, hdr, skb->len);

	priv->tx(dev, hdr, skb->len, 0);
	return 0;
}

static int p54_set_filter(struct ieee80211_hw *dev, u16 filter_type,
			  const u8 *bssid)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_filter *filter;
	size_t data_len;

	hdr = kzalloc(sizeof(*hdr) + sizeof(*filter) +
		      priv->tx_hdr_len, GFP_ATOMIC);
	if (!hdr)
		return -ENOMEM;

	hdr = (void *)hdr + priv->tx_hdr_len;

	filter = (struct p54_tx_control_filter *) hdr->data;
	hdr->magic1 = cpu_to_le16(0x8001);
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_FILTER_SET);

	priv->filter_type = filter->filter_type = cpu_to_le16(filter_type);
	memcpy(filter->mac_addr, priv->mac_addr, ETH_ALEN);
	if (!bssid)
		memset(filter->bssid, ~0, ETH_ALEN);
	else
		memcpy(filter->bssid, bssid, ETH_ALEN);

	filter->rx_antenna = priv->rx_antenna;

	if (priv->fw_var < 0x500) {
		data_len = P54_TX_CONTROL_FILTER_V1_LEN;
		filter->v1.basic_rate_mask = cpu_to_le32(0x15F);
		filter->v1.rx_addr = cpu_to_le32(priv->rx_end);
		filter->v1.max_rx = cpu_to_le16(priv->rx_mtu);
		filter->v1.rxhw = cpu_to_le16(priv->rxhw);
		filter->v1.wakeup_timer = cpu_to_le16(500);
	} else {
		data_len = P54_TX_CONTROL_FILTER_V2_LEN;
		filter->v2.rx_addr = cpu_to_le32(priv->rx_end);
		filter->v2.max_rx = cpu_to_le16(priv->rx_mtu);
		filter->v2.rxhw = cpu_to_le16(priv->rxhw);
		filter->v2.timer = cpu_to_le16(1000);
	}

	hdr->len = cpu_to_le16(data_len);
	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + data_len);
	priv->tx(dev, hdr, sizeof(*hdr) + data_len, 1);
	return 0;
}

static int p54_set_freq(struct ieee80211_hw *dev, __le16 freq)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_channel *chan;
	unsigned int i;
	size_t data_len;
	void *entry;

	hdr = kzalloc(sizeof(*hdr) + sizeof(*chan) +
		      priv->tx_hdr_len, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	hdr = (void *)hdr + priv->tx_hdr_len;

	chan = (struct p54_tx_control_channel *) hdr->data;

	hdr->magic1 = cpu_to_le16(0x8001);

	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_CHANNEL_CHANGE);

	chan->flags = cpu_to_le16(0x1);
	chan->dwell = cpu_to_le16(0x0);

	for (i = 0; i < priv->iq_autocal_len; i++) {
		if (priv->iq_autocal[i].freq != freq)
			continue;

		memcpy(&chan->iq_autocal, &priv->iq_autocal[i],
		       sizeof(*priv->iq_autocal));
		break;
	}
	if (i == priv->iq_autocal_len)
		goto err;

	for (i = 0; i < priv->output_limit_len; i++) {
		if (priv->output_limit[i].freq != freq)
			continue;

		chan->val_barker = 0x38;
		chan->val_bpsk = chan->dup_bpsk =
			priv->output_limit[i].val_bpsk;
		chan->val_qpsk = chan->dup_qpsk =
			priv->output_limit[i].val_qpsk;
		chan->val_16qam = chan->dup_16qam =
			priv->output_limit[i].val_16qam;
		chan->val_64qam = chan->dup_64qam =
			priv->output_limit[i].val_64qam;
		break;
	}
	if (i == priv->output_limit_len)
		goto err;

	entry = priv->curve_data->data;
	for (i = 0; i < priv->curve_data->channels; i++) {
		if (*((__le16 *)entry) != freq) {
			entry += sizeof(__le16);
			entry += sizeof(struct p54_pa_curve_data_sample) *
				 priv->curve_data->points_per_channel;
			continue;
		}

		entry += sizeof(__le16);
		chan->pa_points_per_curve =
			min(priv->curve_data->points_per_channel, (u8) 8);

		memcpy(chan->curve_data, entry, sizeof(*chan->curve_data) *
		       chan->pa_points_per_curve);
		break;
	}

	if (priv->fw_var < 0x500) {
		data_len = P54_TX_CONTROL_CHANNEL_V1_LEN;
		chan->v1.rssical_mul = cpu_to_le16(130);
		chan->v1.rssical_add = cpu_to_le16(0xfe70);
	} else {
		data_len = P54_TX_CONTROL_CHANNEL_V2_LEN;
		chan->v2.rssical_mul = cpu_to_le16(130);
		chan->v2.rssical_add = cpu_to_le16(0xfe70);
		chan->v2.basic_rate_mask = cpu_to_le32(0x15f);
	}

	hdr->len = cpu_to_le16(data_len);
	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + data_len);
	priv->tx(dev, hdr, sizeof(*hdr) + data_len, 1);
	return 0;

 err:
	printk(KERN_ERR "%s: frequency change failed\n", wiphy_name(dev->wiphy));
	kfree(hdr);
	return -EINVAL;
}

static int p54_set_leds(struct ieee80211_hw *dev, int mode, int link, int act)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_led *led;

	hdr = kzalloc(sizeof(*hdr) + sizeof(*led) +
		      priv->tx_hdr_len, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	hdr = (void *)hdr + priv->tx_hdr_len;
	hdr->magic1 = cpu_to_le16(0x8001);
	hdr->len = cpu_to_le16(sizeof(*led));
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_LED);
	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + sizeof(*led));

	led = (struct p54_tx_control_led *) hdr->data;
	led->mode = cpu_to_le16(mode);
	led->led_permanent = cpu_to_le16(link);
	led->led_temporary = cpu_to_le16(act);
	led->duration = cpu_to_le16(1000);

	priv->tx(dev, hdr, sizeof(*hdr) + sizeof(*led), 1);

	return 0;
}

#define P54_SET_QUEUE(queue, ai_fs, cw_min, cw_max, _txop)	\
do {	 							\
	queue.aifs = cpu_to_le16(ai_fs);			\
	queue.cwmin = cpu_to_le16(cw_min);			\
	queue.cwmax = cpu_to_le16(cw_max);			\
	queue.txop = cpu_to_le16(_txop);			\
} while(0)

static void p54_init_vdcf(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_vdcf *vdcf;

	/* all USB V1 adapters need a extra headroom */
	hdr = (void *)priv->cached_vdcf + priv->tx_hdr_len;
	hdr->magic1 = cpu_to_le16(0x8001);
	hdr->len = cpu_to_le16(sizeof(*vdcf));
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_DCFINIT);
	hdr->req_id = cpu_to_le32(priv->rx_start);

	vdcf = (struct p54_tx_control_vdcf *) hdr->data;

	P54_SET_QUEUE(vdcf->queue[0], 0x0002, 0x0003, 0x0007, 47);
	P54_SET_QUEUE(vdcf->queue[1], 0x0002, 0x0007, 0x000f, 94);
	P54_SET_QUEUE(vdcf->queue[2], 0x0003, 0x000f, 0x03ff, 0);
	P54_SET_QUEUE(vdcf->queue[3], 0x0007, 0x000f, 0x03ff, 0);
}

static void p54_set_vdcf(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_vdcf *vdcf;

	hdr = (void *)priv->cached_vdcf + priv->tx_hdr_len;

	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + sizeof(*vdcf));

	vdcf = (struct p54_tx_control_vdcf *) hdr->data;

	if (dev->conf.flags & IEEE80211_CONF_SHORT_SLOT_TIME) {
		vdcf->slottime = 9;
		vdcf->magic1 = 0x10;
		vdcf->magic2 = 0x00;
	} else {
		vdcf->slottime = 20;
		vdcf->magic1 = 0x0a;
		vdcf->magic2 = 0x06;
	}

	/* (see prism54/isl_oid.h for further details) */
	vdcf->frameburst = cpu_to_le16(0);

	priv->tx(dev, hdr, sizeof(*hdr) + sizeof(*vdcf), 0);
}

static int p54_start(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	int err;

	if (!priv->cached_vdcf) {
		priv->cached_vdcf = kzalloc(sizeof(struct p54_tx_control_vdcf)+
			priv->tx_hdr_len + sizeof(struct p54_control_hdr),
			GFP_KERNEL);

		if (!priv->cached_vdcf)
			return -ENOMEM;
	}

	if (!priv->cached_stats) {
		priv->cached_stats = kzalloc(sizeof(struct p54_statistics) +
			priv->tx_hdr_len + sizeof(struct p54_control_hdr),
			GFP_KERNEL);

		if (!priv->cached_stats) {
			kfree(priv->cached_vdcf);
			priv->cached_vdcf = NULL;
			return -ENOMEM;
		}
	}

	err = priv->open(dev);
	if (!err)
		priv->mode = NL80211_IFTYPE_MONITOR;

	p54_init_vdcf(dev);

	mod_timer(&priv->stats_timer, jiffies + HZ);
	return err;
}

static void p54_stop(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	struct sk_buff *skb;

	del_timer(&priv->stats_timer);
	while ((skb = skb_dequeue(&priv->tx_queue)))
		kfree_skb(skb);
	priv->stop(dev);
	priv->tsf_high32 = priv->tsf_low32 = 0;
	priv->mode = NL80211_IFTYPE_UNSPECIFIED;
}

static int p54_add_interface(struct ieee80211_hw *dev,
			     struct ieee80211_if_init_conf *conf)
{
	struct p54_common *priv = dev->priv;

	if (priv->mode != NL80211_IFTYPE_MONITOR)
		return -EOPNOTSUPP;

	switch (conf->type) {
	case NL80211_IFTYPE_STATION:
		priv->mode = conf->type;
		break;
	default:
		return -EOPNOTSUPP;
	}

	memcpy(priv->mac_addr, conf->mac_addr, ETH_ALEN);

	p54_set_filter(dev, 0, NULL);

	switch (conf->type) {
	case NL80211_IFTYPE_STATION:
		p54_set_filter(dev, 1, NULL);
		break;
	default:
		BUG();	/* impossible */
		break;
	}

	p54_set_leds(dev, 1, 0, 0);

	return 0;
}

static void p54_remove_interface(struct ieee80211_hw *dev,
				 struct ieee80211_if_init_conf *conf)
{
	struct p54_common *priv = dev->priv;
	priv->mode = NL80211_IFTYPE_MONITOR;
	memset(priv->mac_addr, 0, ETH_ALEN);
	p54_set_filter(dev, 0, NULL);
}

static int p54_config(struct ieee80211_hw *dev, struct ieee80211_conf *conf)
{
	int ret;
	struct p54_common *priv = dev->priv;

	mutex_lock(&priv->conf_mutex);
	priv->rx_antenna = (conf->antenna_sel_rx == 0) ?
		2 : conf->antenna_sel_tx - 1;
	priv->output_power = conf->power_level << 2;
	ret = p54_set_freq(dev, cpu_to_le16(conf->channel->center_freq));
	p54_set_vdcf(dev);
	mutex_unlock(&priv->conf_mutex);
	return ret;
}

static int p54_config_interface(struct ieee80211_hw *dev,
				struct ieee80211_vif *vif,
				struct ieee80211_if_conf *conf)
{
	struct p54_common *priv = dev->priv;

	mutex_lock(&priv->conf_mutex);
	p54_set_filter(dev, 0, conf->bssid);
	p54_set_leds(dev, 1, !is_multicast_ether_addr(conf->bssid), 0);
	memcpy(priv->bssid, conf->bssid, ETH_ALEN);
	mutex_unlock(&priv->conf_mutex);
	return 0;
}

static void p54_configure_filter(struct ieee80211_hw *dev,
				 unsigned int changed_flags,
				 unsigned int *total_flags,
				 int mc_count, struct dev_mc_list *mclist)
{
	struct p54_common *priv = dev->priv;

	*total_flags &= FIF_BCN_PRBRESP_PROMISC |
			FIF_PROMISC_IN_BSS |
			FIF_FCSFAIL;

	priv->filter_flags = *total_flags;

	if (changed_flags & FIF_BCN_PRBRESP_PROMISC) {
		if (*total_flags & FIF_BCN_PRBRESP_PROMISC)
			p54_set_filter(dev, le16_to_cpu(priv->filter_type),
				 NULL);
		else
			p54_set_filter(dev, le16_to_cpu(priv->filter_type),
				 priv->bssid);
	}

	if (changed_flags & FIF_PROMISC_IN_BSS) {
		if (*total_flags & FIF_PROMISC_IN_BSS)
			p54_set_filter(dev, le16_to_cpu(priv->filter_type) |
				0x8, NULL);
		else
			p54_set_filter(dev, le16_to_cpu(priv->filter_type) &
				~0x8, priv->bssid);
	}
}

static int p54_conf_tx(struct ieee80211_hw *dev, u16 queue,
		       const struct ieee80211_tx_queue_params *params)
{
	struct p54_common *priv = dev->priv;
	struct p54_tx_control_vdcf *vdcf;

	vdcf = (struct p54_tx_control_vdcf *)(((struct p54_control_hdr *)
		((void *)priv->cached_vdcf + priv->tx_hdr_len))->data);

	if ((params) && !(queue > 4)) {
		P54_SET_QUEUE(vdcf->queue[queue], params->aifs,
			params->cw_min, params->cw_max, params->txop);
	} else
		return -EINVAL;

	p54_set_vdcf(dev);

	return 0;
}

static int p54_init_xbow_synth(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_tx_control_xbow_synth *xbow;

	hdr = kzalloc(sizeof(*hdr) + sizeof(*xbow) +
		      priv->tx_hdr_len, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	hdr = (void *)hdr + priv->tx_hdr_len;
	hdr->magic1 = cpu_to_le16(0x8001);
	hdr->len = cpu_to_le16(sizeof(*xbow));
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_XBOW_SYNTH_CFG);
	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + sizeof(*xbow));

	xbow = (struct p54_tx_control_xbow_synth *) hdr->data;
	xbow->magic1 = cpu_to_le16(0x1);
	xbow->magic2 = cpu_to_le16(0x2);
	xbow->freq = cpu_to_le16(5390);

	priv->tx(dev, hdr, sizeof(*hdr) + sizeof(*xbow), 1);

	return 0;
}

static void p54_statistics_timer(unsigned long data)
{
	struct ieee80211_hw *dev = (struct ieee80211_hw *) data;
	struct p54_common *priv = dev->priv;
	struct p54_control_hdr *hdr;
	struct p54_statistics *stats;

	BUG_ON(!priv->cached_stats);

	hdr = (void *)priv->cached_stats + priv->tx_hdr_len;
	hdr->magic1 = cpu_to_le16(0x8000);
	hdr->len = cpu_to_le16(sizeof(*stats));
	hdr->type = cpu_to_le16(P54_CONTROL_TYPE_STAT_READBACK);
	p54_assign_address(dev, NULL, hdr, sizeof(*hdr) + sizeof(*stats));

	priv->tx(dev, hdr, sizeof(*hdr) + sizeof(*stats), 0);
}

static int p54_get_stats(struct ieee80211_hw *dev,
			 struct ieee80211_low_level_stats *stats)
{
	struct p54_common *priv = dev->priv;

	del_timer(&priv->stats_timer);
	p54_statistics_timer((unsigned long)dev);

	if (!wait_for_completion_interruptible_timeout(&priv->stats_comp, HZ)) {
		printk(KERN_ERR "%s: device does not respond!\n",
			wiphy_name(dev->wiphy));
		return -EBUSY;
	}

	memcpy(stats, &priv->stats, sizeof(*stats));

	return 0;
}

static int p54_get_tx_stats(struct ieee80211_hw *dev,
			    struct ieee80211_tx_queue_stats *stats)
{
	struct p54_common *priv = dev->priv;

	memcpy(stats, &priv->tx_stats[4], sizeof(stats[0]) * dev->queues);

	return 0;
}

static const struct ieee80211_ops p54_ops = {
	.tx			= p54_tx,
	.start			= p54_start,
	.stop			= p54_stop,
	.add_interface		= p54_add_interface,
	.remove_interface	= p54_remove_interface,
	.config			= p54_config,
	.config_interface	= p54_config_interface,
	.configure_filter	= p54_configure_filter,
	.conf_tx		= p54_conf_tx,
	.get_stats		= p54_get_stats,
	.get_tx_stats		= p54_get_tx_stats
};

struct ieee80211_hw *p54_init_common(size_t priv_data_len)
{
	struct ieee80211_hw *dev;
	struct p54_common *priv;

	dev = ieee80211_alloc_hw(priv_data_len, &p54_ops);
	if (!dev)
		return NULL;

	priv = dev->priv;
	priv->mode = NL80211_IFTYPE_UNSPECIFIED;
	skb_queue_head_init(&priv->tx_queue);
	dev->flags = IEEE80211_HW_HOST_BROADCAST_PS_BUFFERING | /* not sure */
		     IEEE80211_HW_RX_INCLUDES_FCS |
		     IEEE80211_HW_SIGNAL_DBM |
		     IEEE80211_HW_NOISE_DBM;

	dev->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	dev->channel_change_time = 1000;	/* TODO: find actual value */

	priv->tx_stats[0].limit = 1;
	priv->tx_stats[1].limit = 1;
	priv->tx_stats[2].limit = 1;
	priv->tx_stats[3].limit = 1;
	priv->tx_stats[4].limit = 5;
	dev->queues = 1;
	priv->noise = -94;
	dev->extra_tx_headroom = sizeof(struct p54_control_hdr) + 4 +
				 sizeof(struct p54_tx_control_allocdata);

	mutex_init(&priv->conf_mutex);
	init_completion(&priv->eeprom_comp);
	init_completion(&priv->stats_comp);
	setup_timer(&priv->stats_timer, p54_statistics_timer,
		(unsigned long)dev);

	return dev;
}
EXPORT_SYMBOL_GPL(p54_init_common);

void p54_free_common(struct ieee80211_hw *dev)
{
	struct p54_common *priv = dev->priv;
	kfree(priv->cached_stats);
	kfree(priv->iq_autocal);
	kfree(priv->output_limit);
	kfree(priv->curve_data);
	kfree(priv->cached_vdcf);
}
EXPORT_SYMBOL_GPL(p54_free_common);

static int __init p54_init(void)
{
	return 0;
}

static void __exit p54_exit(void)
{
}

module_init(p54_init);
module_exit(p54_exit);
