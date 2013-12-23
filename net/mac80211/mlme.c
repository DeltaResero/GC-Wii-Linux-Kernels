/*
 * BSS client mode implementation
 * Copyright 2003, Jouni Malinen <jkmaline@cc.hut.fi>
 * Copyright 2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>
#include <linux/random.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/iw_handler.h>
#include <net/mac80211.h>
#include <asm/unaligned.h>

#include "ieee80211_i.h"
#include "rate.h"
#include "led.h"

#define IEEE80211_ASSOC_SCANS_MAX_TRIES 2
#define IEEE80211_AUTH_TIMEOUT (HZ / 5)
#define IEEE80211_AUTH_MAX_TRIES 3
#define IEEE80211_ASSOC_TIMEOUT (HZ / 5)
#define IEEE80211_ASSOC_MAX_TRIES 3
#define IEEE80211_MONITORING_INTERVAL (2 * HZ)
#define IEEE80211_PROBE_INTERVAL (60 * HZ)
#define IEEE80211_RETRY_AUTH_INTERVAL (1 * HZ)
#define IEEE80211_SCAN_INTERVAL (2 * HZ)
#define IEEE80211_SCAN_INTERVAL_SLOW (15 * HZ)
#define IEEE80211_IBSS_JOIN_TIMEOUT (7 * HZ)

#define IEEE80211_IBSS_MERGE_INTERVAL (30 * HZ)
#define IEEE80211_IBSS_INACTIVITY_LIMIT (60 * HZ)

#define IEEE80211_IBSS_MAX_STA_ENTRIES 128


/* utils */
static int ecw2cw(int ecw)
{
	return (1 << ecw) - 1;
}

static u8 *ieee80211_bss_get_ie(struct ieee80211_bss *bss, u8 ie)
{
	u8 *end, *pos;

	pos = bss->ies;
	if (pos == NULL)
		return NULL;
	end = pos + bss->ies_len;

	while (pos + 1 < end) {
		if (pos + 2 + pos[1] > end)
			break;
		if (pos[0] == ie)
			return pos;
		pos += 2 + pos[1];
	}

	return NULL;
}

static int ieee80211_compatible_rates(struct ieee80211_bss *bss,
				      struct ieee80211_supported_band *sband,
				      u64 *rates)
{
	int i, j, count;
	*rates = 0;
	count = 0;
	for (i = 0; i < bss->supp_rates_len; i++) {
		int rate = (bss->supp_rates[i] & 0x7F) * 5;

		for (j = 0; j < sband->n_bitrates; j++)
			if (sband->bitrates[j].bitrate == rate) {
				*rates |= BIT(j);
				count++;
				break;
			}
	}

	return count;
}

/* also used by mesh code */
u64 ieee80211_sta_get_rates(struct ieee80211_local *local,
			    struct ieee802_11_elems *elems,
			    enum ieee80211_band band)
{
	struct ieee80211_supported_band *sband;
	struct ieee80211_rate *bitrates;
	size_t num_rates;
	u64 supp_rates;
	int i, j;
	sband = local->hw.wiphy->bands[band];

	if (!sband) {
		WARN_ON(1);
		sband = local->hw.wiphy->bands[local->hw.conf.channel->band];
	}

	bitrates = sband->bitrates;
	num_rates = sband->n_bitrates;
	supp_rates = 0;
	for (i = 0; i < elems->supp_rates_len +
		     elems->ext_supp_rates_len; i++) {
		u8 rate = 0;
		int own_rate;
		if (i < elems->supp_rates_len)
			rate = elems->supp_rates[i];
		else if (elems->ext_supp_rates)
			rate = elems->ext_supp_rates
				[i - elems->supp_rates_len];
		own_rate = 5 * (rate & 0x7f);
		for (j = 0; j < num_rates; j++)
			if (bitrates[j].bitrate == own_rate)
				supp_rates |= BIT(j);
	}
	return supp_rates;
}

/* frame sending functions */

/* also used by scanning code */
void ieee80211_send_probe_req(struct ieee80211_sub_if_data *sdata, u8 *dst,
			      u8 *ssid, size_t ssid_len)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_supported_band *sband;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u8 *pos, *supp_rates, *esupp_rates = NULL;
	int i;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + sizeof(*mgmt) + 200);
	if (!skb) {
		printk(KERN_DEBUG "%s: failed to allocate buffer for probe "
		       "request\n", sdata->dev->name);
		return;
	}
	skb_reserve(skb, local->hw.extra_tx_headroom);

	mgmt = (struct ieee80211_mgmt *) skb_put(skb, 24);
	memset(mgmt, 0, 24);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_PROBE_REQ);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	if (dst) {
		memcpy(mgmt->da, dst, ETH_ALEN);
		memcpy(mgmt->bssid, dst, ETH_ALEN);
	} else {
		memset(mgmt->da, 0xff, ETH_ALEN);
		memset(mgmt->bssid, 0xff, ETH_ALEN);
	}
	pos = skb_put(skb, 2 + ssid_len);
	*pos++ = WLAN_EID_SSID;
	*pos++ = ssid_len;
	memcpy(pos, ssid, ssid_len);

	supp_rates = skb_put(skb, 2);
	supp_rates[0] = WLAN_EID_SUPP_RATES;
	supp_rates[1] = 0;
	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

	for (i = 0; i < sband->n_bitrates; i++) {
		struct ieee80211_rate *rate = &sband->bitrates[i];
		if (esupp_rates) {
			pos = skb_put(skb, 1);
			esupp_rates[1]++;
		} else if (supp_rates[1] == 8) {
			esupp_rates = skb_put(skb, 3);
			esupp_rates[0] = WLAN_EID_EXT_SUPP_RATES;
			esupp_rates[1] = 1;
			pos = &esupp_rates[2];
		} else {
			pos = skb_put(skb, 1);
			supp_rates[1]++;
		}
		*pos = rate->bitrate / 5;
	}

	ieee80211_tx_skb(sdata, skb, 0);
}

static void ieee80211_send_auth(struct ieee80211_sub_if_data *sdata,
				struct ieee80211_if_sta *ifsta,
				int transaction, u8 *extra, size_t extra_len,
				int encrypt)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom +
			    sizeof(*mgmt) + 6 + extra_len);
	if (!skb) {
		printk(KERN_DEBUG "%s: failed to allocate buffer for auth "
		       "frame\n", sdata->dev->name);
		return;
	}
	skb_reserve(skb, local->hw.extra_tx_headroom);

	mgmt = (struct ieee80211_mgmt *) skb_put(skb, 24 + 6);
	memset(mgmt, 0, 24 + 6);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_AUTH);
	if (encrypt)
		mgmt->frame_control |= cpu_to_le16(IEEE80211_FCTL_PROTECTED);
	memcpy(mgmt->da, ifsta->bssid, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	memcpy(mgmt->bssid, ifsta->bssid, ETH_ALEN);
	mgmt->u.auth.auth_alg = cpu_to_le16(ifsta->auth_alg);
	mgmt->u.auth.auth_transaction = cpu_to_le16(transaction);
	ifsta->auth_transaction = transaction + 1;
	mgmt->u.auth.status_code = cpu_to_le16(0);
	if (extra)
		memcpy(skb_put(skb, extra_len), extra, extra_len);

	ieee80211_tx_skb(sdata, skb, encrypt);
}

static void ieee80211_send_assoc(struct ieee80211_sub_if_data *sdata,
				 struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u8 *pos, *ies, *ht_ie;
	int i, len, count, rates_len, supp_rates_len;
	u16 capab;
	struct ieee80211_bss *bss;
	int wmm = 0;
	struct ieee80211_supported_band *sband;
	u64 rates = 0;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom +
			    sizeof(*mgmt) + 200 + ifsta->extra_ie_len +
			    ifsta->ssid_len);
	if (!skb) {
		printk(KERN_DEBUG "%s: failed to allocate buffer for assoc "
		       "frame\n", sdata->dev->name);
		return;
	}
	skb_reserve(skb, local->hw.extra_tx_headroom);

	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

	capab = ifsta->capab;

	if (local->hw.conf.channel->band == IEEE80211_BAND_2GHZ) {
		if (!(local->hw.flags & IEEE80211_HW_2GHZ_SHORT_SLOT_INCAPABLE))
			capab |= WLAN_CAPABILITY_SHORT_SLOT_TIME;
		if (!(local->hw.flags & IEEE80211_HW_2GHZ_SHORT_PREAMBLE_INCAPABLE))
			capab |= WLAN_CAPABILITY_SHORT_PREAMBLE;
	}

	bss = ieee80211_rx_bss_get(local, ifsta->bssid,
				   local->hw.conf.channel->center_freq,
				   ifsta->ssid, ifsta->ssid_len);
	if (bss) {
		if (bss->capability & WLAN_CAPABILITY_PRIVACY)
			capab |= WLAN_CAPABILITY_PRIVACY;
		if (bss->wmm_used)
			wmm = 1;

		/* get all rates supported by the device and the AP as
		 * some APs don't like getting a superset of their rates
		 * in the association request (e.g. D-Link DAP 1353 in
		 * b-only mode) */
		rates_len = ieee80211_compatible_rates(bss, sband, &rates);

		if ((bss->capability & WLAN_CAPABILITY_SPECTRUM_MGMT) &&
		    (local->hw.flags & IEEE80211_HW_SPECTRUM_MGMT))
			capab |= WLAN_CAPABILITY_SPECTRUM_MGMT;

		ieee80211_rx_bss_put(local, bss);
	} else {
		rates = ~0;
		rates_len = sband->n_bitrates;
	}

	mgmt = (struct ieee80211_mgmt *) skb_put(skb, 24);
	memset(mgmt, 0, 24);
	memcpy(mgmt->da, ifsta->bssid, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	memcpy(mgmt->bssid, ifsta->bssid, ETH_ALEN);

	if (ifsta->flags & IEEE80211_STA_PREV_BSSID_SET) {
		skb_put(skb, 10);
		mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_REASSOC_REQ);
		mgmt->u.reassoc_req.capab_info = cpu_to_le16(capab);
		mgmt->u.reassoc_req.listen_interval =
				cpu_to_le16(local->hw.conf.listen_interval);
		memcpy(mgmt->u.reassoc_req.current_ap, ifsta->prev_bssid,
		       ETH_ALEN);
	} else {
		skb_put(skb, 4);
		mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_ASSOC_REQ);
		mgmt->u.assoc_req.capab_info = cpu_to_le16(capab);
		mgmt->u.assoc_req.listen_interval =
				cpu_to_le16(local->hw.conf.listen_interval);
	}

	/* SSID */
	ies = pos = skb_put(skb, 2 + ifsta->ssid_len);
	*pos++ = WLAN_EID_SSID;
	*pos++ = ifsta->ssid_len;
	memcpy(pos, ifsta->ssid, ifsta->ssid_len);

	/* add all rates which were marked to be used above */
	supp_rates_len = rates_len;
	if (supp_rates_len > 8)
		supp_rates_len = 8;

	len = sband->n_bitrates;
	pos = skb_put(skb, supp_rates_len + 2);
	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = supp_rates_len;

	count = 0;
	for (i = 0; i < sband->n_bitrates; i++) {
		if (BIT(i) & rates) {
			int rate = sband->bitrates[i].bitrate;
			*pos++ = (u8) (rate / 5);
			if (++count == 8)
				break;
		}
	}

	if (rates_len > count) {
		pos = skb_put(skb, rates_len - count + 2);
		*pos++ = WLAN_EID_EXT_SUPP_RATES;
		*pos++ = rates_len - count;

		for (i++; i < sband->n_bitrates; i++) {
			if (BIT(i) & rates) {
				int rate = sband->bitrates[i].bitrate;
				*pos++ = (u8) (rate / 5);
			}
		}
	}

	if (capab & WLAN_CAPABILITY_SPECTRUM_MGMT) {
		/* 1. power capabilities */
		pos = skb_put(skb, 4);
		*pos++ = WLAN_EID_PWR_CAPABILITY;
		*pos++ = 2;
		*pos++ = 0; /* min tx power */
		*pos++ = local->hw.conf.channel->max_power; /* max tx power */

		/* 2. supported channels */
		/* TODO: get this in reg domain format */
		pos = skb_put(skb, 2 * sband->n_channels + 2);
		*pos++ = WLAN_EID_SUPPORTED_CHANNELS;
		*pos++ = 2 * sband->n_channels;
		for (i = 0; i < sband->n_channels; i++) {
			*pos++ = ieee80211_frequency_to_channel(
					sband->channels[i].center_freq);
			*pos++ = 1; /* one channel in the subband*/
		}
	}

	if (ifsta->extra_ie) {
		pos = skb_put(skb, ifsta->extra_ie_len);
		memcpy(pos, ifsta->extra_ie, ifsta->extra_ie_len);
	}

	if (wmm && (ifsta->flags & IEEE80211_STA_WMM_ENABLED)) {
		pos = skb_put(skb, 9);
		*pos++ = WLAN_EID_VENDOR_SPECIFIC;
		*pos++ = 7; /* len */
		*pos++ = 0x00; /* Microsoft OUI 00:50:F2 */
		*pos++ = 0x50;
		*pos++ = 0xf2;
		*pos++ = 2; /* WME */
		*pos++ = 0; /* WME info */
		*pos++ = 1; /* WME ver */
		*pos++ = 0;
	}

	/* wmm support is a must to HT */
	if (wmm && (ifsta->flags & IEEE80211_STA_WMM_ENABLED) &&
	    sband->ht_cap.ht_supported &&
	    (ht_ie = ieee80211_bss_get_ie(bss, WLAN_EID_HT_INFORMATION)) &&
	    ht_ie[1] >= sizeof(struct ieee80211_ht_info)) {
		struct ieee80211_ht_info *ht_info =
			(struct ieee80211_ht_info *)(ht_ie + 2);
		u16 cap = sband->ht_cap.cap;
		__le16 tmp;
		u32 flags = local->hw.conf.channel->flags;

		switch (ht_info->ht_param & IEEE80211_HT_PARAM_CHA_SEC_OFFSET) {
		case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
			if (flags & IEEE80211_CHAN_NO_FAT_ABOVE) {
				cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
				cap &= ~IEEE80211_HT_CAP_SGI_40;
			}
			break;
		case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
			if (flags & IEEE80211_CHAN_NO_FAT_BELOW) {
				cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
				cap &= ~IEEE80211_HT_CAP_SGI_40;
			}
			break;
		}

		tmp = cpu_to_le16(cap);
		pos = skb_put(skb, sizeof(struct ieee80211_ht_cap)+2);
		*pos++ = WLAN_EID_HT_CAPABILITY;
		*pos++ = sizeof(struct ieee80211_ht_cap);
		memset(pos, 0, sizeof(struct ieee80211_ht_cap));
		memcpy(pos, &tmp, sizeof(u16));
		pos += sizeof(u16);
		/* TODO: needs a define here for << 2 */
		*pos++ = sband->ht_cap.ampdu_factor |
			 (sband->ht_cap.ampdu_density << 2);
		memcpy(pos, &sband->ht_cap.mcs, sizeof(sband->ht_cap.mcs));
	}

	kfree(ifsta->assocreq_ies);
	ifsta->assocreq_ies_len = (skb->data + skb->len) - ies;
	ifsta->assocreq_ies = kmalloc(ifsta->assocreq_ies_len, GFP_KERNEL);
	if (ifsta->assocreq_ies)
		memcpy(ifsta->assocreq_ies, ies, ifsta->assocreq_ies_len);

	ieee80211_tx_skb(sdata, skb, 0);
}


static void ieee80211_send_deauth_disassoc(struct ieee80211_sub_if_data *sdata,
					   u16 stype, u16 reason)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + sizeof(*mgmt));
	if (!skb) {
		printk(KERN_DEBUG "%s: failed to allocate buffer for "
		       "deauth/disassoc frame\n", sdata->dev->name);
		return;
	}
	skb_reserve(skb, local->hw.extra_tx_headroom);

	mgmt = (struct ieee80211_mgmt *) skb_put(skb, 24);
	memset(mgmt, 0, 24);
	memcpy(mgmt->da, ifsta->bssid, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	memcpy(mgmt->bssid, ifsta->bssid, ETH_ALEN);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
	skb_put(skb, 2);
	/* u.deauth.reason_code == u.disassoc.reason_code */
	mgmt->u.deauth.reason_code = cpu_to_le16(reason);

	ieee80211_tx_skb(sdata, skb, 0);
}

/* MLME */
static void ieee80211_sta_def_wmm_params(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_bss *bss)
{
	struct ieee80211_local *local = sdata->local;
	int i, have_higher_than_11mbit = 0;

	/* cf. IEEE 802.11 9.2.12 */
	for (i = 0; i < bss->supp_rates_len; i++)
		if ((bss->supp_rates[i] & 0x7f) * 5 > 110)
			have_higher_than_11mbit = 1;

	if (local->hw.conf.channel->band == IEEE80211_BAND_2GHZ &&
	    have_higher_than_11mbit)
		sdata->flags |= IEEE80211_SDATA_OPERATING_GMODE;
	else
		sdata->flags &= ~IEEE80211_SDATA_OPERATING_GMODE;

	ieee80211_set_wmm_default(sdata);
}

static void ieee80211_sta_wmm_params(struct ieee80211_local *local,
				     struct ieee80211_if_sta *ifsta,
				     u8 *wmm_param, size_t wmm_param_len)
{
	struct ieee80211_tx_queue_params params;
	size_t left;
	int count;
	u8 *pos;

	if (!(ifsta->flags & IEEE80211_STA_WMM_ENABLED))
		return;

	if (!wmm_param)
		return;

	if (wmm_param_len < 8 || wmm_param[5] /* version */ != 1)
		return;
	count = wmm_param[6] & 0x0f;
	if (count == ifsta->wmm_last_param_set)
		return;
	ifsta->wmm_last_param_set = count;

	pos = wmm_param + 8;
	left = wmm_param_len - 8;

	memset(&params, 0, sizeof(params));

	if (!local->ops->conf_tx)
		return;

	local->wmm_acm = 0;
	for (; left >= 4; left -= 4, pos += 4) {
		int aci = (pos[0] >> 5) & 0x03;
		int acm = (pos[0] >> 4) & 0x01;
		int queue;

		switch (aci) {
		case 1:
			queue = 3;
			if (acm)
				local->wmm_acm |= BIT(0) | BIT(3);
			break;
		case 2:
			queue = 1;
			if (acm)
				local->wmm_acm |= BIT(4) | BIT(5);
			break;
		case 3:
			queue = 0;
			if (acm)
				local->wmm_acm |= BIT(6) | BIT(7);
			break;
		case 0:
		default:
			queue = 2;
			if (acm)
				local->wmm_acm |= BIT(1) | BIT(2);
			break;
		}

		params.aifs = pos[0] & 0x0f;
		params.cw_max = ecw2cw((pos[1] & 0xf0) >> 4);
		params.cw_min = ecw2cw(pos[1] & 0x0f);
		params.txop = get_unaligned_le16(pos + 2);
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
		printk(KERN_DEBUG "%s: WMM queue=%d aci=%d acm=%d aifs=%d "
		       "cWmin=%d cWmax=%d txop=%d\n",
		       local->mdev->name, queue, aci, acm, params.aifs, params.cw_min,
		       params.cw_max, params.txop);
#endif
		/* TODO: handle ACM (block TX, fallback to next lowest allowed
		 * AC for now) */
		if (local->ops->conf_tx(local_to_hw(local), queue, &params)) {
			printk(KERN_DEBUG "%s: failed to set TX queue "
			       "parameters for queue %d\n", local->mdev->name, queue);
		}
	}
}

static u32 ieee80211_handle_bss_capability(struct ieee80211_sub_if_data *sdata,
					   u16 capab, bool erp_valid, u8 erp)
{
	struct ieee80211_bss_conf *bss_conf = &sdata->vif.bss_conf;
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;
#endif
	u32 changed = 0;
	bool use_protection;
	bool use_short_preamble;
	bool use_short_slot;

	if (erp_valid) {
		use_protection = (erp & WLAN_ERP_USE_PROTECTION) != 0;
		use_short_preamble = (erp & WLAN_ERP_BARKER_PREAMBLE) == 0;
	} else {
		use_protection = false;
		use_short_preamble = !!(capab & WLAN_CAPABILITY_SHORT_PREAMBLE);
	}

	use_short_slot = !!(capab & WLAN_CAPABILITY_SHORT_SLOT_TIME);

	if (use_protection != bss_conf->use_cts_prot) {
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
		if (net_ratelimit()) {
			printk(KERN_DEBUG "%s: CTS protection %s (BSSID=%pM)\n",
			       sdata->dev->name,
			       use_protection ? "enabled" : "disabled",
			       ifsta->bssid);
		}
#endif
		bss_conf->use_cts_prot = use_protection;
		changed |= BSS_CHANGED_ERP_CTS_PROT;
	}

	if (use_short_preamble != bss_conf->use_short_preamble) {
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
		if (net_ratelimit()) {
			printk(KERN_DEBUG "%s: switched to %s barker preamble"
			       " (BSSID=%pM)\n",
			       sdata->dev->name,
			       use_short_preamble ? "short" : "long",
			       ifsta->bssid);
		}
#endif
		bss_conf->use_short_preamble = use_short_preamble;
		changed |= BSS_CHANGED_ERP_PREAMBLE;
	}

	if (use_short_slot != bss_conf->use_short_slot) {
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
		if (net_ratelimit()) {
			printk(KERN_DEBUG "%s: switched to %s slot time"
			       " (BSSID=%pM)\n",
			       sdata->dev->name,
			       use_short_slot ? "short" : "long",
			       ifsta->bssid);
		}
#endif
		bss_conf->use_short_slot = use_short_slot;
		changed |= BSS_CHANGED_ERP_SLOT;
	}

	return changed;
}

static void ieee80211_sta_send_apinfo(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_if_sta *ifsta)
{
	union iwreq_data wrqu;
	memset(&wrqu, 0, sizeof(wrqu));
	if (ifsta->flags & IEEE80211_STA_ASSOCIATED)
		memcpy(wrqu.ap_addr.sa_data, sdata->u.sta.bssid, ETH_ALEN);
	wrqu.ap_addr.sa_family = ARPHRD_ETHER;
	wireless_send_event(sdata->dev, SIOCGIWAP, &wrqu, NULL);
}

static void ieee80211_sta_send_associnfo(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_if_sta *ifsta)
{
	char *buf;
	size_t len;
	int i;
	union iwreq_data wrqu;

	if (!ifsta->assocreq_ies && !ifsta->assocresp_ies)
		return;

	buf = kmalloc(50 + 2 * (ifsta->assocreq_ies_len +
				ifsta->assocresp_ies_len), GFP_KERNEL);
	if (!buf)
		return;

	len = sprintf(buf, "ASSOCINFO(");
	if (ifsta->assocreq_ies) {
		len += sprintf(buf + len, "ReqIEs=");
		for (i = 0; i < ifsta->assocreq_ies_len; i++) {
			len += sprintf(buf + len, "%02x",
				       ifsta->assocreq_ies[i]);
		}
	}
	if (ifsta->assocresp_ies) {
		if (ifsta->assocreq_ies)
			len += sprintf(buf + len, " ");
		len += sprintf(buf + len, "RespIEs=");
		for (i = 0; i < ifsta->assocresp_ies_len; i++) {
			len += sprintf(buf + len, "%02x",
				       ifsta->assocresp_ies[i]);
		}
	}
	len += sprintf(buf + len, ")");

	if (len > IW_CUSTOM_MAX) {
		len = sprintf(buf, "ASSOCRESPIE=");
		for (i = 0; i < ifsta->assocresp_ies_len; i++) {
			len += sprintf(buf + len, "%02x",
				       ifsta->assocresp_ies[i]);
		}
	}

	if (len <= IW_CUSTOM_MAX) {
		memset(&wrqu, 0, sizeof(wrqu));
		wrqu.data.length = len;
		wireless_send_event(sdata->dev, IWEVCUSTOM, &wrqu, buf);
	}

	kfree(buf);
}


static void ieee80211_set_associated(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta,
				     u32 bss_info_changed)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_conf *conf = &local_to_hw(local)->conf;

	struct ieee80211_bss *bss;

	bss_info_changed |= BSS_CHANGED_ASSOC;
	ifsta->flags |= IEEE80211_STA_ASSOCIATED;

	if (sdata->vif.type != NL80211_IFTYPE_STATION)
		return;

	bss = ieee80211_rx_bss_get(local, ifsta->bssid,
				   conf->channel->center_freq,
				   ifsta->ssid, ifsta->ssid_len);
	if (bss) {
		/* set timing information */
		sdata->vif.bss_conf.beacon_int = bss->beacon_int;
		sdata->vif.bss_conf.timestamp = bss->timestamp;
		sdata->vif.bss_conf.dtim_period = bss->dtim_period;

		bss_info_changed |= ieee80211_handle_bss_capability(sdata,
			bss->capability, bss->has_erp_value, bss->erp_value);

		ieee80211_rx_bss_put(local, bss);
	}

	ifsta->flags |= IEEE80211_STA_PREV_BSSID_SET;
	memcpy(ifsta->prev_bssid, sdata->u.sta.bssid, ETH_ALEN);
	ieee80211_sta_send_associnfo(sdata, ifsta);

	ifsta->last_probe = jiffies;
	ieee80211_led_assoc(local, 1);

	sdata->vif.bss_conf.assoc = 1;
	/*
	 * For now just always ask the driver to update the basic rateset
	 * when we have associated, we aren't checking whether it actually
	 * changed or not.
	 */
	bss_info_changed |= BSS_CHANGED_BASIC_RATES;
	ieee80211_bss_info_change_notify(sdata, bss_info_changed);

	if (local->powersave) {
		if (local->dynamic_ps_timeout > 0)
			mod_timer(&local->dynamic_ps_timer, jiffies +
				  msecs_to_jiffies(local->dynamic_ps_timeout));
		else {
			conf->flags |= IEEE80211_CONF_PS;
			ieee80211_hw_config(local,
					    IEEE80211_CONF_CHANGE_PS);
		}
	}

	netif_tx_start_all_queues(sdata->dev);
	netif_carrier_on(sdata->dev);

	ieee80211_sta_send_apinfo(sdata, ifsta);
}

static void ieee80211_direct_probe(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta)
{
	ifsta->direct_probe_tries++;
	if (ifsta->direct_probe_tries > IEEE80211_AUTH_MAX_TRIES) {
		printk(KERN_DEBUG "%s: direct probe to AP %pM timed out\n",
		       sdata->dev->name, ifsta->bssid);
		ifsta->state = IEEE80211_STA_MLME_DISABLED;
		ieee80211_sta_send_apinfo(sdata, ifsta);
		return;
	}

	printk(KERN_DEBUG "%s: direct probe to AP %pM try %d\n",
			sdata->dev->name, ifsta->bssid,
			ifsta->direct_probe_tries);

	ifsta->state = IEEE80211_STA_MLME_DIRECT_PROBE;

	set_bit(IEEE80211_STA_REQ_DIRECT_PROBE, &ifsta->request);

	/* Direct probe is sent to broadcast address as some APs
	 * will not answer to direct packet in unassociated state.
	 */
	ieee80211_send_probe_req(sdata, NULL,
				 ifsta->ssid, ifsta->ssid_len);

	mod_timer(&ifsta->timer, jiffies + IEEE80211_AUTH_TIMEOUT);
}


static void ieee80211_authenticate(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta)
{
	ifsta->auth_tries++;
	if (ifsta->auth_tries > IEEE80211_AUTH_MAX_TRIES) {
		printk(KERN_DEBUG "%s: authentication with AP %pM"
		       " timed out\n",
		       sdata->dev->name, ifsta->bssid);
		ifsta->state = IEEE80211_STA_MLME_DISABLED;
		ieee80211_sta_send_apinfo(sdata, ifsta);
		return;
	}

	ifsta->state = IEEE80211_STA_MLME_AUTHENTICATE;
	printk(KERN_DEBUG "%s: authenticate with AP %pM\n",
	       sdata->dev->name, ifsta->bssid);

	ieee80211_send_auth(sdata, ifsta, 1, NULL, 0, 0);

	mod_timer(&ifsta->timer, jiffies + IEEE80211_AUTH_TIMEOUT);
}

/*
 * The disassoc 'reason' argument can be either our own reason
 * if self disconnected or a reason code from the AP.
 */
static void ieee80211_set_disassoc(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta, bool deauth,
				   bool self_disconnected, u16 reason)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	u32 changed = 0, config_changed = 0;

	rcu_read_lock();

	sta = sta_info_get(local, ifsta->bssid);
	if (!sta) {
		rcu_read_unlock();
		return;
	}

	if (deauth) {
		ifsta->direct_probe_tries = 0;
		ifsta->auth_tries = 0;
	}
	ifsta->assoc_scan_tries = 0;
	ifsta->assoc_tries = 0;

	netif_tx_stop_all_queues(sdata->dev);
	netif_carrier_off(sdata->dev);

	ieee80211_sta_tear_down_BA_sessions(sdata, sta->sta.addr);

	if (self_disconnected) {
		if (deauth)
			ieee80211_send_deauth_disassoc(sdata,
				IEEE80211_STYPE_DEAUTH, reason);
		else
			ieee80211_send_deauth_disassoc(sdata,
				IEEE80211_STYPE_DISASSOC, reason);
	}

	ifsta->flags &= ~IEEE80211_STA_ASSOCIATED;
	changed |= ieee80211_reset_erp_info(sdata);

	ieee80211_led_assoc(local, 0);
	changed |= BSS_CHANGED_ASSOC;
	sdata->vif.bss_conf.assoc = false;

	ieee80211_sta_send_apinfo(sdata, ifsta);

	if (self_disconnected || reason == WLAN_REASON_DISASSOC_STA_HAS_LEFT)
		ifsta->state = IEEE80211_STA_MLME_DISABLED;

	rcu_read_unlock();

	local->hw.conf.ht.enabled = false;
	local->oper_channel_type = NL80211_CHAN_NO_HT;
	config_changed |= IEEE80211_CONF_CHANGE_HT;

	del_timer_sync(&local->dynamic_ps_timer);
	cancel_work_sync(&local->dynamic_ps_enable_work);

	if (local->hw.conf.flags & IEEE80211_CONF_PS) {
		local->hw.conf.flags &= ~IEEE80211_CONF_PS;
		config_changed |= IEEE80211_CONF_CHANGE_PS;
	}

	ieee80211_hw_config(local, config_changed);
	ieee80211_bss_info_change_notify(sdata, changed);

	rcu_read_lock();

	sta = sta_info_get(local, ifsta->bssid);
	if (!sta) {
		rcu_read_unlock();
		return;
	}

	sta_info_unlink(&sta);

	rcu_read_unlock();

	sta_info_destroy(sta);
}

static int ieee80211_sta_wep_configured(struct ieee80211_sub_if_data *sdata)
{
	if (!sdata || !sdata->default_key ||
	    sdata->default_key->conf.alg != ALG_WEP)
		return 0;
	return 1;
}

static int ieee80211_privacy_mismatch(struct ieee80211_sub_if_data *sdata,
				      struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss;
	int bss_privacy;
	int wep_privacy;
	int privacy_invoked;

	if (!ifsta || (ifsta->flags & IEEE80211_STA_MIXED_CELL))
		return 0;

	bss = ieee80211_rx_bss_get(local, ifsta->bssid,
				   local->hw.conf.channel->center_freq,
				   ifsta->ssid, ifsta->ssid_len);
	if (!bss)
		return 0;

	bss_privacy = !!(bss->capability & WLAN_CAPABILITY_PRIVACY);
	wep_privacy = !!ieee80211_sta_wep_configured(sdata);
	privacy_invoked = !!(ifsta->flags & IEEE80211_STA_PRIVACY_INVOKED);

	ieee80211_rx_bss_put(local, bss);

	if ((bss_privacy == wep_privacy) || (bss_privacy == privacy_invoked))
		return 0;

	return 1;
}

static void ieee80211_associate(struct ieee80211_sub_if_data *sdata,
				struct ieee80211_if_sta *ifsta)
{
	ifsta->assoc_tries++;
	if (ifsta->assoc_tries > IEEE80211_ASSOC_MAX_TRIES) {
		printk(KERN_DEBUG "%s: association with AP %pM"
		       " timed out\n",
		       sdata->dev->name, ifsta->bssid);
		ifsta->state = IEEE80211_STA_MLME_DISABLED;
		ieee80211_sta_send_apinfo(sdata, ifsta);
		return;
	}

	ifsta->state = IEEE80211_STA_MLME_ASSOCIATE;
	printk(KERN_DEBUG "%s: associate with AP %pM\n",
	       sdata->dev->name, ifsta->bssid);
	if (ieee80211_privacy_mismatch(sdata, ifsta)) {
		printk(KERN_DEBUG "%s: mismatch in privacy configuration and "
		       "mixed-cell disabled - abort association\n", sdata->dev->name);
		ifsta->state = IEEE80211_STA_MLME_DISABLED;
		return;
	}

	ieee80211_send_assoc(sdata, ifsta);

	mod_timer(&ifsta->timer, jiffies + IEEE80211_ASSOC_TIMEOUT);
}


static void ieee80211_associated(struct ieee80211_sub_if_data *sdata,
				 struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	int disassoc;

	/* TODO: start monitoring current AP signal quality and number of
	 * missed beacons. Scan other channels every now and then and search
	 * for better APs. */
	/* TODO: remove expired BSSes */

	ifsta->state = IEEE80211_STA_MLME_ASSOCIATED;

	rcu_read_lock();

	sta = sta_info_get(local, ifsta->bssid);
	if (!sta) {
		printk(KERN_DEBUG "%s: No STA entry for own AP %pM\n",
		       sdata->dev->name, ifsta->bssid);
		disassoc = 1;
	} else {
		disassoc = 0;
		if (time_after(jiffies,
			       sta->last_rx + IEEE80211_MONITORING_INTERVAL)) {
			if (ifsta->flags & IEEE80211_STA_PROBEREQ_POLL) {
				printk(KERN_DEBUG "%s: No ProbeResp from "
				       "current AP %pM - assume out of "
				       "range\n",
				       sdata->dev->name, ifsta->bssid);
				disassoc = 1;
			} else
				ieee80211_send_probe_req(sdata, ifsta->bssid,
							 ifsta->ssid,
							 ifsta->ssid_len);
			ifsta->flags ^= IEEE80211_STA_PROBEREQ_POLL;
		} else {
			ifsta->flags &= ~IEEE80211_STA_PROBEREQ_POLL;
			if (time_after(jiffies, ifsta->last_probe +
				       IEEE80211_PROBE_INTERVAL)) {
				ifsta->last_probe = jiffies;
				ieee80211_send_probe_req(sdata, ifsta->bssid,
							 ifsta->ssid,
							 ifsta->ssid_len);
			}
		}
	}

	rcu_read_unlock();

	if (disassoc)
		ieee80211_set_disassoc(sdata, ifsta, true, true,
					WLAN_REASON_PREV_AUTH_NOT_VALID);
	else
		mod_timer(&ifsta->timer, jiffies +
				      IEEE80211_MONITORING_INTERVAL);
}


static void ieee80211_auth_completed(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta)
{
	printk(KERN_DEBUG "%s: authenticated\n", sdata->dev->name);
	ifsta->flags |= IEEE80211_STA_AUTHENTICATED;
	ieee80211_associate(sdata, ifsta);
}


static void ieee80211_auth_challenge(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta,
				     struct ieee80211_mgmt *mgmt,
				     size_t len)
{
	u8 *pos;
	struct ieee802_11_elems elems;

	pos = mgmt->u.auth.variable;
	ieee802_11_parse_elems(pos, len - (pos - (u8 *) mgmt), &elems);
	if (!elems.challenge)
		return;
	ieee80211_send_auth(sdata, ifsta, 3, elems.challenge - 2,
			    elems.challenge_len + 2, 1);
}

static void ieee80211_rx_mgmt_auth(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta,
				   struct ieee80211_mgmt *mgmt,
				   size_t len)
{
	u16 auth_alg, auth_transaction, status_code;

	if (ifsta->state != IEEE80211_STA_MLME_AUTHENTICATE &&
	    sdata->vif.type != NL80211_IFTYPE_ADHOC)
		return;

	if (len < 24 + 6)
		return;

	if (sdata->vif.type != NL80211_IFTYPE_ADHOC &&
	    memcmp(ifsta->bssid, mgmt->sa, ETH_ALEN) != 0)
		return;

	if (sdata->vif.type != NL80211_IFTYPE_ADHOC &&
	    memcmp(ifsta->bssid, mgmt->bssid, ETH_ALEN) != 0)
		return;

	auth_alg = le16_to_cpu(mgmt->u.auth.auth_alg);
	auth_transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);
	status_code = le16_to_cpu(mgmt->u.auth.status_code);

	if (sdata->vif.type == NL80211_IFTYPE_ADHOC) {
		/*
		 * IEEE 802.11 standard does not require authentication in IBSS
		 * networks and most implementations do not seem to use it.
		 * However, try to reply to authentication attempts if someone
		 * has actually implemented this.
		 */
		if (auth_alg != WLAN_AUTH_OPEN || auth_transaction != 1)
			return;
		ieee80211_send_auth(sdata, ifsta, 2, NULL, 0, 0);
	}

	if (auth_alg != ifsta->auth_alg ||
	    auth_transaction != ifsta->auth_transaction)
		return;

	if (status_code != WLAN_STATUS_SUCCESS) {
		if (status_code == WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG) {
			u8 algs[3];
			const int num_algs = ARRAY_SIZE(algs);
			int i, pos;
			algs[0] = algs[1] = algs[2] = 0xff;
			if (ifsta->auth_algs & IEEE80211_AUTH_ALG_OPEN)
				algs[0] = WLAN_AUTH_OPEN;
			if (ifsta->auth_algs & IEEE80211_AUTH_ALG_SHARED_KEY)
				algs[1] = WLAN_AUTH_SHARED_KEY;
			if (ifsta->auth_algs & IEEE80211_AUTH_ALG_LEAP)
				algs[2] = WLAN_AUTH_LEAP;
			if (ifsta->auth_alg == WLAN_AUTH_OPEN)
				pos = 0;
			else if (ifsta->auth_alg == WLAN_AUTH_SHARED_KEY)
				pos = 1;
			else
				pos = 2;
			for (i = 0; i < num_algs; i++) {
				pos++;
				if (pos >= num_algs)
					pos = 0;
				if (algs[pos] == ifsta->auth_alg ||
				    algs[pos] == 0xff)
					continue;
				if (algs[pos] == WLAN_AUTH_SHARED_KEY &&
				    !ieee80211_sta_wep_configured(sdata))
					continue;
				ifsta->auth_alg = algs[pos];
				break;
			}
		}
		return;
	}

	switch (ifsta->auth_alg) {
	case WLAN_AUTH_OPEN:
	case WLAN_AUTH_LEAP:
		ieee80211_auth_completed(sdata, ifsta);
		break;
	case WLAN_AUTH_SHARED_KEY:
		if (ifsta->auth_transaction == 4)
			ieee80211_auth_completed(sdata, ifsta);
		else
			ieee80211_auth_challenge(sdata, ifsta, mgmt, len);
		break;
	}
}


static void ieee80211_rx_mgmt_deauth(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta,
				     struct ieee80211_mgmt *mgmt,
				     size_t len)
{
	u16 reason_code;

	if (len < 24 + 2)
		return;

	if (memcmp(ifsta->bssid, mgmt->sa, ETH_ALEN))
		return;

	reason_code = le16_to_cpu(mgmt->u.deauth.reason_code);

	if (ifsta->flags & IEEE80211_STA_AUTHENTICATED)
		printk(KERN_DEBUG "%s: deauthenticated (Reason: %u)\n",
				sdata->dev->name, reason_code);

	if (ifsta->state == IEEE80211_STA_MLME_AUTHENTICATE ||
	    ifsta->state == IEEE80211_STA_MLME_ASSOCIATE ||
	    ifsta->state == IEEE80211_STA_MLME_ASSOCIATED) {
		ifsta->state = IEEE80211_STA_MLME_DIRECT_PROBE;
		mod_timer(&ifsta->timer, jiffies +
				      IEEE80211_RETRY_AUTH_INTERVAL);
	}

	ieee80211_set_disassoc(sdata, ifsta, true, false, 0);
	ifsta->flags &= ~IEEE80211_STA_AUTHENTICATED;
}


static void ieee80211_rx_mgmt_disassoc(struct ieee80211_sub_if_data *sdata,
				       struct ieee80211_if_sta *ifsta,
				       struct ieee80211_mgmt *mgmt,
				       size_t len)
{
	u16 reason_code;

	if (len < 24 + 2)
		return;

	if (memcmp(ifsta->bssid, mgmt->sa, ETH_ALEN))
		return;

	reason_code = le16_to_cpu(mgmt->u.disassoc.reason_code);

	if (ifsta->flags & IEEE80211_STA_ASSOCIATED)
		printk(KERN_DEBUG "%s: disassociated (Reason: %u)\n",
				sdata->dev->name, reason_code);

	if (ifsta->state == IEEE80211_STA_MLME_ASSOCIATED) {
		ifsta->state = IEEE80211_STA_MLME_ASSOCIATE;
		mod_timer(&ifsta->timer, jiffies +
				      IEEE80211_RETRY_AUTH_INTERVAL);
	}

	ieee80211_set_disassoc(sdata, ifsta, false, false, reason_code);
}


static void ieee80211_rx_mgmt_assoc_resp(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_if_sta *ifsta,
					 struct ieee80211_mgmt *mgmt,
					 size_t len,
					 int reassoc)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_supported_band *sband;
	struct sta_info *sta;
	u64 rates, basic_rates;
	u16 capab_info, status_code, aid;
	struct ieee802_11_elems elems;
	struct ieee80211_bss_conf *bss_conf = &sdata->vif.bss_conf;
	u8 *pos;
	u32 changed = 0;
	int i, j;
	bool have_higher_than_11mbit = false, newsta = false;
	u16 ap_ht_cap_flags;

	/* AssocResp and ReassocResp have identical structure, so process both
	 * of them in this function. */

	if (ifsta->state != IEEE80211_STA_MLME_ASSOCIATE)
		return;

	if (len < 24 + 6)
		return;

	if (memcmp(ifsta->bssid, mgmt->sa, ETH_ALEN) != 0)
		return;

	capab_info = le16_to_cpu(mgmt->u.assoc_resp.capab_info);
	status_code = le16_to_cpu(mgmt->u.assoc_resp.status_code);
	aid = le16_to_cpu(mgmt->u.assoc_resp.aid);

	printk(KERN_DEBUG "%s: RX %sssocResp from %pM (capab=0x%x "
	       "status=%d aid=%d)\n",
	       sdata->dev->name, reassoc ? "Rea" : "A", mgmt->sa,
	       capab_info, status_code, (u16)(aid & ~(BIT(15) | BIT(14))));

	if (status_code != WLAN_STATUS_SUCCESS) {
		printk(KERN_DEBUG "%s: AP denied association (code=%d)\n",
		       sdata->dev->name, status_code);
		/* if this was a reassociation, ensure we try a "full"
		 * association next time. This works around some broken APs
		 * which do not correctly reject reassociation requests. */
		ifsta->flags &= ~IEEE80211_STA_PREV_BSSID_SET;
		return;
	}

	if ((aid & (BIT(15) | BIT(14))) != (BIT(15) | BIT(14)))
		printk(KERN_DEBUG "%s: invalid aid value %d; bits 15:14 not "
		       "set\n", sdata->dev->name, aid);
	aid &= ~(BIT(15) | BIT(14));

	pos = mgmt->u.assoc_resp.variable;
	ieee802_11_parse_elems(pos, len - (pos - (u8 *) mgmt), &elems);

	if (!elems.supp_rates) {
		printk(KERN_DEBUG "%s: no SuppRates element in AssocResp\n",
		       sdata->dev->name);
		return;
	}

	printk(KERN_DEBUG "%s: associated\n", sdata->dev->name);
	ifsta->aid = aid;
	ifsta->ap_capab = capab_info;

	kfree(ifsta->assocresp_ies);
	ifsta->assocresp_ies_len = len - (pos - (u8 *) mgmt);
	ifsta->assocresp_ies = kmalloc(ifsta->assocresp_ies_len, GFP_KERNEL);
	if (ifsta->assocresp_ies)
		memcpy(ifsta->assocresp_ies, pos, ifsta->assocresp_ies_len);

	rcu_read_lock();

	/* Add STA entry for the AP */
	sta = sta_info_get(local, ifsta->bssid);
	if (!sta) {
		struct ieee80211_bss *bss;

		newsta = true;

		sta = sta_info_alloc(sdata, ifsta->bssid, GFP_ATOMIC);
		if (!sta) {
			printk(KERN_DEBUG "%s: failed to alloc STA entry for"
			       " the AP\n", sdata->dev->name);
			rcu_read_unlock();
			return;
		}
		bss = ieee80211_rx_bss_get(local, ifsta->bssid,
					   local->hw.conf.channel->center_freq,
					   ifsta->ssid, ifsta->ssid_len);
		if (bss) {
			sta->last_signal = bss->signal;
			sta->last_qual = bss->qual;
			sta->last_noise = bss->noise;
			ieee80211_rx_bss_put(local, bss);
		}

		/* update new sta with its last rx activity */
		sta->last_rx = jiffies;
	}

	/*
	 * FIXME: Do we really need to update the sta_info's information here?
	 *	  We already know about the AP (we found it in our list) so it
	 *	  should already be filled with the right info, no?
	 *	  As is stands, all this is racy because typically we assume
	 *	  the information that is filled in here (except flags) doesn't
	 *	  change while a STA structure is alive. As such, it should move
	 *	  to between the sta_info_alloc() and sta_info_insert() above.
	 */

	set_sta_flags(sta, WLAN_STA_AUTH | WLAN_STA_ASSOC | WLAN_STA_ASSOC_AP |
			   WLAN_STA_AUTHORIZED);

	rates = 0;
	basic_rates = 0;
	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

	for (i = 0; i < elems.supp_rates_len; i++) {
		int rate = (elems.supp_rates[i] & 0x7f) * 5;
		bool is_basic = !!(elems.supp_rates[i] & 0x80);

		if (rate > 110)
			have_higher_than_11mbit = true;

		for (j = 0; j < sband->n_bitrates; j++) {
			if (sband->bitrates[j].bitrate == rate) {
				rates |= BIT(j);
				if (is_basic)
					basic_rates |= BIT(j);
				break;
			}
		}
	}

	for (i = 0; i < elems.ext_supp_rates_len; i++) {
		int rate = (elems.ext_supp_rates[i] & 0x7f) * 5;
		bool is_basic = !!(elems.ext_supp_rates[i] & 0x80);

		if (rate > 110)
			have_higher_than_11mbit = true;

		for (j = 0; j < sband->n_bitrates; j++) {
			if (sband->bitrates[j].bitrate == rate) {
				rates |= BIT(j);
				if (is_basic)
					basic_rates |= BIT(j);
				break;
			}
		}
	}

	sta->sta.supp_rates[local->hw.conf.channel->band] = rates;
	sdata->vif.bss_conf.basic_rates = basic_rates;

	/* cf. IEEE 802.11 9.2.12 */
	if (local->hw.conf.channel->band == IEEE80211_BAND_2GHZ &&
	    have_higher_than_11mbit)
		sdata->flags |= IEEE80211_SDATA_OPERATING_GMODE;
	else
		sdata->flags &= ~IEEE80211_SDATA_OPERATING_GMODE;

	if (elems.ht_cap_elem)
		ieee80211_ht_cap_ie_to_sta_ht_cap(sband,
				elems.ht_cap_elem, &sta->sta.ht_cap);

	ap_ht_cap_flags = sta->sta.ht_cap.cap;

	rate_control_rate_init(sta);

	if (elems.wmm_param)
		set_sta_flags(sta, WLAN_STA_WME);

	if (newsta) {
		int err = sta_info_insert(sta);
		if (err) {
			printk(KERN_DEBUG "%s: failed to insert STA entry for"
			       " the AP (error %d)\n", sdata->dev->name, err);
			rcu_read_unlock();
			return;
		}
	}

	rcu_read_unlock();

	if (elems.wmm_param)
		ieee80211_sta_wmm_params(local, ifsta, elems.wmm_param,
					 elems.wmm_param_len);

	if (elems.ht_info_elem && elems.wmm_param &&
	    (ifsta->flags & IEEE80211_STA_WMM_ENABLED))
		changed |= ieee80211_enable_ht(sdata, elems.ht_info_elem,
					       ap_ht_cap_flags);

	/* set AID and assoc capability,
	 * ieee80211_set_associated() will tell the driver */
	bss_conf->aid = aid;
	bss_conf->assoc_capability = capab_info;
	ieee80211_set_associated(sdata, ifsta, changed);

	ieee80211_associated(sdata, ifsta);
}


static int ieee80211_sta_join_ibss(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta,
				   struct ieee80211_bss *bss)
{
	struct ieee80211_local *local = sdata->local;
	int res, rates, i, j;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u8 *pos;
	struct ieee80211_supported_band *sband;
	union iwreq_data wrqu;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + 400);
	if (!skb) {
		printk(KERN_DEBUG "%s: failed to allocate buffer for probe "
		       "response\n", sdata->dev->name);
		return -ENOMEM;
	}

	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

	/* Remove possible STA entries from other IBSS networks. */
	sta_info_flush_delayed(sdata);

	if (local->ops->reset_tsf) {
		/* Reset own TSF to allow time synchronization work. */
		local->ops->reset_tsf(local_to_hw(local));
	}
	memcpy(ifsta->bssid, bss->bssid, ETH_ALEN);
	res = ieee80211_if_config(sdata, IEEE80211_IFCC_BSSID);
	if (res)
		return res;

	local->hw.conf.beacon_int = bss->beacon_int >= 10 ? bss->beacon_int : 10;

	sdata->drop_unencrypted = bss->capability &
		WLAN_CAPABILITY_PRIVACY ? 1 : 0;

	res = ieee80211_set_freq(sdata, bss->freq);

	if (res)
		return res;

	/* Build IBSS probe response */

	skb_reserve(skb, local->hw.extra_tx_headroom);

	mgmt = (struct ieee80211_mgmt *)
		skb_put(skb, 24 + sizeof(mgmt->u.beacon));
	memset(mgmt, 0, 24 + sizeof(mgmt->u.beacon));
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						IEEE80211_STYPE_PROBE_RESP);
	memset(mgmt->da, 0xff, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	memcpy(mgmt->bssid, ifsta->bssid, ETH_ALEN);
	mgmt->u.beacon.beacon_int =
		cpu_to_le16(local->hw.conf.beacon_int);
	mgmt->u.beacon.timestamp = cpu_to_le64(bss->timestamp);
	mgmt->u.beacon.capab_info = cpu_to_le16(bss->capability);

	pos = skb_put(skb, 2 + ifsta->ssid_len);
	*pos++ = WLAN_EID_SSID;
	*pos++ = ifsta->ssid_len;
	memcpy(pos, ifsta->ssid, ifsta->ssid_len);

	rates = bss->supp_rates_len;
	if (rates > 8)
		rates = 8;
	pos = skb_put(skb, 2 + rates);
	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = rates;
	memcpy(pos, bss->supp_rates, rates);

	if (bss->band == IEEE80211_BAND_2GHZ) {
		pos = skb_put(skb, 2 + 1);
		*pos++ = WLAN_EID_DS_PARAMS;
		*pos++ = 1;
		*pos++ = ieee80211_frequency_to_channel(bss->freq);
	}

	pos = skb_put(skb, 2 + 2);
	*pos++ = WLAN_EID_IBSS_PARAMS;
	*pos++ = 2;
	/* FIX: set ATIM window based on scan results */
	*pos++ = 0;
	*pos++ = 0;

	if (bss->supp_rates_len > 8) {
		rates = bss->supp_rates_len - 8;
		pos = skb_put(skb, 2 + rates);
		*pos++ = WLAN_EID_EXT_SUPP_RATES;
		*pos++ = rates;
		memcpy(pos, &bss->supp_rates[8], rates);
	}

	ifsta->probe_resp = skb;

	ieee80211_if_config(sdata, IEEE80211_IFCC_BEACON);


	rates = 0;
	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];
	for (i = 0; i < bss->supp_rates_len; i++) {
		int bitrate = (bss->supp_rates[i] & 0x7f) * 5;
		for (j = 0; j < sband->n_bitrates; j++)
			if (sband->bitrates[j].bitrate == bitrate)
				rates |= BIT(j);
	}
	ifsta->supp_rates_bits[local->hw.conf.channel->band] = rates;

	ieee80211_sta_def_wmm_params(sdata, bss);

	ifsta->state = IEEE80211_STA_MLME_IBSS_JOINED;
	mod_timer(&ifsta->timer, jiffies + IEEE80211_IBSS_MERGE_INTERVAL);

	ieee80211_led_assoc(local, true);

	memset(&wrqu, 0, sizeof(wrqu));
	memcpy(wrqu.ap_addr.sa_data, bss->bssid, ETH_ALEN);
	wireless_send_event(sdata->dev, SIOCGIWAP, &wrqu, NULL);

	return res;
}

static void ieee80211_rx_bss_info(struct ieee80211_sub_if_data *sdata,
				  struct ieee80211_mgmt *mgmt,
				  size_t len,
				  struct ieee80211_rx_status *rx_status,
				  struct ieee802_11_elems *elems,
				  bool beacon)
{
	struct ieee80211_local *local = sdata->local;
	int freq;
	struct ieee80211_bss *bss;
	struct sta_info *sta;
	struct ieee80211_channel *channel;
	u64 beacon_timestamp, rx_timestamp;
	u64 supp_rates = 0;
	enum ieee80211_band band = rx_status->band;

	if (elems->ds_params && elems->ds_params_len == 1)
		freq = ieee80211_channel_to_frequency(elems->ds_params[0]);
	else
		freq = rx_status->freq;

	channel = ieee80211_get_channel(local->hw.wiphy, freq);

	if (!channel || channel->flags & IEEE80211_CHAN_DISABLED)
		return;

	if (sdata->vif.type == NL80211_IFTYPE_ADHOC && elems->supp_rates &&
	    memcmp(mgmt->bssid, sdata->u.sta.bssid, ETH_ALEN) == 0) {
		supp_rates = ieee80211_sta_get_rates(local, elems, band);

		rcu_read_lock();

		sta = sta_info_get(local, mgmt->sa);
		if (sta) {
			u64 prev_rates;

			prev_rates = sta->sta.supp_rates[band];
			/* make sure mandatory rates are always added */
			sta->sta.supp_rates[band] = supp_rates |
				ieee80211_mandatory_rates(local, band);

#ifdef CONFIG_MAC80211_IBSS_DEBUG
			if (sta->sta.supp_rates[band] != prev_rates)
				printk(KERN_DEBUG "%s: updated supp_rates set "
				    "for %pM based on beacon info (0x%llx | "
				    "0x%llx -> 0x%llx)\n",
				    sdata->dev->name,
				    sta->sta.addr,
				    (unsigned long long) prev_rates,
				    (unsigned long long) supp_rates,
				    (unsigned long long) sta->sta.supp_rates[band]);
#endif
		} else {
			ieee80211_ibss_add_sta(sdata, mgmt->bssid, mgmt->sa, supp_rates);
		}

		rcu_read_unlock();
	}

	bss = ieee80211_bss_info_update(local, rx_status, mgmt, len, elems,
					freq, beacon);
	if (!bss)
		return;

	/* was just updated in ieee80211_bss_info_update */
	beacon_timestamp = bss->timestamp;

	/*
	 * In STA mode, the remaining parameters should not be overridden
	 * by beacons because they're not necessarily accurate there.
	 */
	if (sdata->vif.type != NL80211_IFTYPE_ADHOC &&
	    bss->last_probe_resp && beacon) {
		ieee80211_rx_bss_put(local, bss);
		return;
	}

	/* check if we need to merge IBSS */
	if (sdata->vif.type == NL80211_IFTYPE_ADHOC && beacon &&
	    bss->capability & WLAN_CAPABILITY_IBSS &&
	    bss->freq == local->oper_channel->center_freq &&
	    elems->ssid_len == sdata->u.sta.ssid_len &&
	    memcmp(elems->ssid, sdata->u.sta.ssid,
				sdata->u.sta.ssid_len) == 0) {
		if (rx_status->flag & RX_FLAG_TSFT) {
			/* in order for correct IBSS merging we need mactime
			 *
			 * since mactime is defined as the time the first data
			 * symbol of the frame hits the PHY, and the timestamp
			 * of the beacon is defined as "the time that the data
			 * symbol containing the first bit of the timestamp is
			 * transmitted to the PHY plus the transmitting STA’s
			 * delays through its local PHY from the MAC-PHY
			 * interface to its interface with the WM"
			 * (802.11 11.1.2) - equals the time this bit arrives at
			 * the receiver - we have to take into account the
			 * offset between the two.
			 * e.g: at 1 MBit that means mactime is 192 usec earlier
			 * (=24 bytes * 8 usecs/byte) than the beacon timestamp.
			 */
			int rate;
			if (rx_status->flag & RX_FLAG_HT) {
				rate = 65; /* TODO: HT rates */
			} else {
				rate = local->hw.wiphy->bands[band]->
					bitrates[rx_status->rate_idx].bitrate;
			}
			rx_timestamp = rx_status->mactime + (24 * 8 * 10 / rate);
		} else if (local && local->ops && local->ops->get_tsf)
			/* second best option: get current TSF */
			rx_timestamp = local->ops->get_tsf(local_to_hw(local));
		else
			/* can't merge without knowing the TSF */
			rx_timestamp = -1LLU;
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "RX beacon SA=%pM BSSID="
		       "%pM TSF=0x%llx BCN=0x%llx diff=%lld @%lu\n",
		       mgmt->sa, mgmt->bssid,
		       (unsigned long long)rx_timestamp,
		       (unsigned long long)beacon_timestamp,
		       (unsigned long long)(rx_timestamp - beacon_timestamp),
		       jiffies);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */
		if (beacon_timestamp > rx_timestamp) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
			printk(KERN_DEBUG "%s: beacon TSF higher than "
			       "local TSF - IBSS merge with BSSID %pM\n",
			       sdata->dev->name, mgmt->bssid);
#endif
			ieee80211_sta_join_ibss(sdata, &sdata->u.sta, bss);
			ieee80211_ibss_add_sta(sdata, mgmt->bssid, mgmt->sa, supp_rates);
		}
	}

	ieee80211_rx_bss_put(local, bss);
}


static void ieee80211_rx_mgmt_probe_resp(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_mgmt *mgmt,
					 size_t len,
					 struct ieee80211_rx_status *rx_status)
{
	size_t baselen;
	struct ieee802_11_elems elems;
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;

	if (memcmp(mgmt->da, sdata->dev->dev_addr, ETH_ALEN))
		return; /* ignore ProbeResp to foreign address */

	baselen = (u8 *) mgmt->u.probe_resp.variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	ieee802_11_parse_elems(mgmt->u.probe_resp.variable, len - baselen,
				&elems);

	ieee80211_rx_bss_info(sdata, mgmt, len, rx_status, &elems, false);

	/* direct probe may be part of the association flow */
	if (test_and_clear_bit(IEEE80211_STA_REQ_DIRECT_PROBE,
							&ifsta->request)) {
		printk(KERN_DEBUG "%s direct probe responded\n",
		       sdata->dev->name);
		ieee80211_authenticate(sdata, ifsta);
	}
}


static void ieee80211_rx_mgmt_beacon(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt,
				     size_t len,
				     struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_if_sta *ifsta;
	size_t baselen;
	struct ieee802_11_elems elems;
	struct ieee80211_local *local = sdata->local;
	u32 changed = 0;
	bool erp_valid;
	u8 erp_value = 0;

	/* Process beacon from the current BSS */
	baselen = (u8 *) mgmt->u.beacon.variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	ieee802_11_parse_elems(mgmt->u.beacon.variable, len - baselen, &elems);

	ieee80211_rx_bss_info(sdata, mgmt, len, rx_status, &elems, true);

	if (sdata->vif.type != NL80211_IFTYPE_STATION)
		return;
	ifsta = &sdata->u.sta;

	if (!(ifsta->flags & IEEE80211_STA_ASSOCIATED) ||
	    memcmp(ifsta->bssid, mgmt->bssid, ETH_ALEN) != 0)
		return;

	ieee80211_sta_wmm_params(local, ifsta, elems.wmm_param,
				 elems.wmm_param_len);


	if (elems.erp_info && elems.erp_info_len >= 1) {
		erp_valid = true;
		erp_value = elems.erp_info[0];
	} else {
		erp_valid = false;
	}
	changed |= ieee80211_handle_bss_capability(sdata,
			le16_to_cpu(mgmt->u.beacon.capab_info),
			erp_valid, erp_value);


	if (elems.ht_cap_elem && elems.ht_info_elem && elems.wmm_param) {
		struct sta_info *sta;
		struct ieee80211_supported_band *sband;
		u16 ap_ht_cap_flags;

		rcu_read_lock();

		sta = sta_info_get(local, ifsta->bssid);
		if (!sta) {
			rcu_read_unlock();
			return;
		}

		sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

		ieee80211_ht_cap_ie_to_sta_ht_cap(sband,
				elems.ht_cap_elem, &sta->sta.ht_cap);

		ap_ht_cap_flags = sta->sta.ht_cap.cap;

		rcu_read_unlock();

		changed |= ieee80211_enable_ht(sdata, elems.ht_info_elem,
					       ap_ht_cap_flags);
	}

	if (elems.country_elem) {
		/* Note we are only reviewing this on beacons
		 * for the BSSID we are associated to */
		regulatory_hint_11d(local->hw.wiphy,
			elems.country_elem, elems.country_elem_len);
	}

	ieee80211_bss_info_change_notify(sdata, changed);
}


static void ieee80211_rx_mgmt_probe_req(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_if_sta *ifsta,
					struct ieee80211_mgmt *mgmt,
					size_t len,
					struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_local *local = sdata->local;
	int tx_last_beacon;
	struct sk_buff *skb;
	struct ieee80211_mgmt *resp;
	u8 *pos, *end;

	if (sdata->vif.type != NL80211_IFTYPE_ADHOC ||
	    ifsta->state != IEEE80211_STA_MLME_IBSS_JOINED ||
	    len < 24 + 2 || !ifsta->probe_resp)
		return;

	if (local->ops->tx_last_beacon)
		tx_last_beacon = local->ops->tx_last_beacon(local_to_hw(local));
	else
		tx_last_beacon = 1;

#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: RX ProbeReq SA=%pM DA=%pM BSSID=%pM"
	       " (tx_last_beacon=%d)\n",
	       sdata->dev->name, mgmt->sa, mgmt->da,
	       mgmt->bssid, tx_last_beacon);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	if (!tx_last_beacon)
		return;

	if (memcmp(mgmt->bssid, ifsta->bssid, ETH_ALEN) != 0 &&
	    memcmp(mgmt->bssid, "\xff\xff\xff\xff\xff\xff", ETH_ALEN) != 0)
		return;

	end = ((u8 *) mgmt) + len;
	pos = mgmt->u.probe_req.variable;
	if (pos[0] != WLAN_EID_SSID ||
	    pos + 2 + pos[1] > end) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "%s: Invalid SSID IE in ProbeReq "
		       "from %pM\n",
		       sdata->dev->name, mgmt->sa);
#endif
		return;
	}
	if (pos[1] != 0 &&
	    (pos[1] != ifsta->ssid_len ||
	     memcmp(pos + 2, ifsta->ssid, ifsta->ssid_len) != 0)) {
		/* Ignore ProbeReq for foreign SSID */
		return;
	}

	/* Reply with ProbeResp */
	skb = skb_copy(ifsta->probe_resp, GFP_KERNEL);
	if (!skb)
		return;

	resp = (struct ieee80211_mgmt *) skb->data;
	memcpy(resp->da, mgmt->sa, ETH_ALEN);
#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: Sending ProbeResp to %pM\n",
	       sdata->dev->name, resp->da);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */
	ieee80211_tx_skb(sdata, skb, 0);
}

void ieee80211_sta_rx_mgmt(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb,
			   struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_sta *ifsta;
	struct ieee80211_mgmt *mgmt;
	u16 fc;

	if (skb->len < 24)
		goto fail;

	ifsta = &sdata->u.sta;

	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = le16_to_cpu(mgmt->frame_control);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_PROBE_REQ:
	case IEEE80211_STYPE_PROBE_RESP:
	case IEEE80211_STYPE_BEACON:
		memcpy(skb->cb, rx_status, sizeof(*rx_status));
	case IEEE80211_STYPE_AUTH:
	case IEEE80211_STYPE_ASSOC_RESP:
	case IEEE80211_STYPE_REASSOC_RESP:
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		skb_queue_tail(&ifsta->skb_queue, skb);
		queue_work(local->hw.workqueue, &ifsta->work);
		return;
	}

 fail:
	kfree_skb(skb);
}

static void ieee80211_sta_rx_queued_mgmt(struct ieee80211_sub_if_data *sdata,
					 struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx_status;
	struct ieee80211_if_sta *ifsta;
	struct ieee80211_mgmt *mgmt;
	u16 fc;

	ifsta = &sdata->u.sta;

	rx_status = (struct ieee80211_rx_status *) skb->cb;
	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = le16_to_cpu(mgmt->frame_control);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_PROBE_REQ:
		ieee80211_rx_mgmt_probe_req(sdata, ifsta, mgmt, skb->len,
					    rx_status);
		break;
	case IEEE80211_STYPE_PROBE_RESP:
		ieee80211_rx_mgmt_probe_resp(sdata, mgmt, skb->len, rx_status);
		break;
	case IEEE80211_STYPE_BEACON:
		ieee80211_rx_mgmt_beacon(sdata, mgmt, skb->len, rx_status);
		break;
	case IEEE80211_STYPE_AUTH:
		ieee80211_rx_mgmt_auth(sdata, ifsta, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_ASSOC_RESP:
		ieee80211_rx_mgmt_assoc_resp(sdata, ifsta, mgmt, skb->len, 0);
		break;
	case IEEE80211_STYPE_REASSOC_RESP:
		ieee80211_rx_mgmt_assoc_resp(sdata, ifsta, mgmt, skb->len, 1);
		break;
	case IEEE80211_STYPE_DEAUTH:
		ieee80211_rx_mgmt_deauth(sdata, ifsta, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_DISASSOC:
		ieee80211_rx_mgmt_disassoc(sdata, ifsta, mgmt, skb->len);
		break;
	}

	kfree_skb(skb);
}


static int ieee80211_sta_active_ibss(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	int active = 0;
	struct sta_info *sta;

	rcu_read_lock();

	list_for_each_entry_rcu(sta, &local->sta_list, list) {
		if (sta->sdata == sdata &&
		    time_after(sta->last_rx + IEEE80211_IBSS_MERGE_INTERVAL,
			       jiffies)) {
			active++;
			break;
		}
	}

	rcu_read_unlock();

	return active;
}


static void ieee80211_sta_merge_ibss(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta)
{
	mod_timer(&ifsta->timer, jiffies + IEEE80211_IBSS_MERGE_INTERVAL);

	ieee80211_sta_expire(sdata, IEEE80211_IBSS_INACTIVITY_LIMIT);
	if (ieee80211_sta_active_ibss(sdata))
		return;

	printk(KERN_DEBUG "%s: No active IBSS STAs - trying to scan for other "
	       "IBSS networks with same SSID (merge)\n", sdata->dev->name);
	ieee80211_request_scan(sdata, ifsta->ssid, ifsta->ssid_len);
}


static void ieee80211_sta_timer(unsigned long data)
{
	struct ieee80211_sub_if_data *sdata =
		(struct ieee80211_sub_if_data *) data;
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;
	struct ieee80211_local *local = sdata->local;

	set_bit(IEEE80211_STA_REQ_RUN, &ifsta->request);
	queue_work(local->hw.workqueue, &ifsta->work);
}

static void ieee80211_sta_reset_auth(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;

	if (local->ops->reset_tsf) {
		/* Reset own TSF to allow time synchronization work. */
		local->ops->reset_tsf(local_to_hw(local));
	}

	ifsta->wmm_last_param_set = -1; /* allow any WMM update */


	if (ifsta->auth_algs & IEEE80211_AUTH_ALG_OPEN)
		ifsta->auth_alg = WLAN_AUTH_OPEN;
	else if (ifsta->auth_algs & IEEE80211_AUTH_ALG_SHARED_KEY)
		ifsta->auth_alg = WLAN_AUTH_SHARED_KEY;
	else if (ifsta->auth_algs & IEEE80211_AUTH_ALG_LEAP)
		ifsta->auth_alg = WLAN_AUTH_LEAP;
	else
		ifsta->auth_alg = WLAN_AUTH_OPEN;
	ifsta->auth_transaction = -1;
	ifsta->flags &= ~IEEE80211_STA_ASSOCIATED;
	ifsta->assoc_scan_tries = 0;
	ifsta->direct_probe_tries = 0;
	ifsta->auth_tries = 0;
	ifsta->assoc_tries = 0;
	netif_tx_stop_all_queues(sdata->dev);
	netif_carrier_off(sdata->dev);
}


static int ieee80211_sta_match_ssid(struct ieee80211_if_sta *ifsta,
				    const char *ssid, int ssid_len)
{
	int tmp, hidden_ssid;

	if (ssid_len == ifsta->ssid_len &&
	    !memcmp(ifsta->ssid, ssid, ssid_len))
		return 1;

	if (ifsta->flags & IEEE80211_STA_AUTO_BSSID_SEL)
		return 0;

	hidden_ssid = 1;
	tmp = ssid_len;
	while (tmp--) {
		if (ssid[tmp] != '\0') {
			hidden_ssid = 0;
			break;
		}
	}

	if (hidden_ssid && (ifsta->ssid_len == ssid_len || ssid_len == 0))
		return 1;

	if (ssid_len == 1 && ssid[0] == ' ')
		return 1;

	return 0;
}

static int ieee80211_sta_create_ibss(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss;
	struct ieee80211_supported_band *sband;
	u8 bssid[ETH_ALEN], *pos;
	int i;
	int ret;

#if 0
	/* Easier testing, use fixed BSSID. */
	memset(bssid, 0xfe, ETH_ALEN);
#else
	/* Generate random, not broadcast, locally administered BSSID. Mix in
	 * own MAC address to make sure that devices that do not have proper
	 * random number generator get different BSSID. */
	get_random_bytes(bssid, ETH_ALEN);
	for (i = 0; i < ETH_ALEN; i++)
		bssid[i] ^= sdata->dev->dev_addr[i];
	bssid[0] &= ~0x01;
	bssid[0] |= 0x02;
#endif

	printk(KERN_DEBUG "%s: Creating new IBSS network, BSSID %pM\n",
	       sdata->dev->name, bssid);

	bss = ieee80211_rx_bss_add(local, bssid,
				   local->hw.conf.channel->center_freq,
				   sdata->u.sta.ssid, sdata->u.sta.ssid_len);
	if (!bss)
		return -ENOMEM;

	bss->band = local->hw.conf.channel->band;
	sband = local->hw.wiphy->bands[bss->band];

	if (local->hw.conf.beacon_int == 0)
		local->hw.conf.beacon_int = 100;
	bss->beacon_int = local->hw.conf.beacon_int;
	bss->last_update = jiffies;
	bss->capability = WLAN_CAPABILITY_IBSS;

	if (sdata->default_key)
		bss->capability |= WLAN_CAPABILITY_PRIVACY;
	else
		sdata->drop_unencrypted = 0;

	bss->supp_rates_len = sband->n_bitrates;
	pos = bss->supp_rates;
	for (i = 0; i < sband->n_bitrates; i++) {
		int rate = sband->bitrates[i].bitrate;
		*pos++ = (u8) (rate / 5);
	}

	ret = ieee80211_sta_join_ibss(sdata, ifsta, bss);
	ieee80211_rx_bss_put(local, bss);
	return ret;
}


static int ieee80211_sta_find_ibss(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss;
	int found = 0;
	u8 bssid[ETH_ALEN];
	int active_ibss;

	if (ifsta->ssid_len == 0)
		return -EINVAL;

	active_ibss = ieee80211_sta_active_ibss(sdata);
#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: sta_find_ibss (active_ibss=%d)\n",
	       sdata->dev->name, active_ibss);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */
	spin_lock_bh(&local->bss_lock);
	list_for_each_entry(bss, &local->bss_list, list) {
		if (ifsta->ssid_len != bss->ssid_len ||
		    memcmp(ifsta->ssid, bss->ssid, bss->ssid_len) != 0
		    || !(bss->capability & WLAN_CAPABILITY_IBSS))
			continue;
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "   bssid=%pM found\n", bss->bssid);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */
		memcpy(bssid, bss->bssid, ETH_ALEN);
		found = 1;
		if (active_ibss || memcmp(bssid, ifsta->bssid, ETH_ALEN) != 0)
			break;
	}
	spin_unlock_bh(&local->bss_lock);

#ifdef CONFIG_MAC80211_IBSS_DEBUG
	if (found)
		printk(KERN_DEBUG "   sta_find_ibss: selected %pM current "
		       "%pM\n", bssid, ifsta->bssid);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	if (found && memcmp(ifsta->bssid, bssid, ETH_ALEN) != 0) {
		int ret;
		int search_freq;

		if (ifsta->flags & IEEE80211_STA_AUTO_CHANNEL_SEL)
			search_freq = bss->freq;
		else
			search_freq = local->hw.conf.channel->center_freq;

		bss = ieee80211_rx_bss_get(local, bssid, search_freq,
					   ifsta->ssid, ifsta->ssid_len);
		if (!bss)
			goto dont_join;

		printk(KERN_DEBUG "%s: Selected IBSS BSSID %pM"
		       " based on configured SSID\n",
		       sdata->dev->name, bssid);
		ret = ieee80211_sta_join_ibss(sdata, ifsta, bss);
		ieee80211_rx_bss_put(local, bss);
		return ret;
	}

dont_join:
#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "   did not try to join ibss\n");
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	/* Selected IBSS not found in current scan results - try to scan */
	if (ifsta->state == IEEE80211_STA_MLME_IBSS_JOINED &&
	    !ieee80211_sta_active_ibss(sdata)) {
		mod_timer(&ifsta->timer, jiffies +
				      IEEE80211_IBSS_MERGE_INTERVAL);
	} else if (time_after(jiffies, local->last_scan_completed +
			      IEEE80211_SCAN_INTERVAL)) {
		printk(KERN_DEBUG "%s: Trigger new scan to find an IBSS to "
		       "join\n", sdata->dev->name);
		return ieee80211_request_scan(sdata, ifsta->ssid,
					      ifsta->ssid_len);
	} else if (ifsta->state != IEEE80211_STA_MLME_IBSS_JOINED) {
		int interval = IEEE80211_SCAN_INTERVAL;

		if (time_after(jiffies, ifsta->ibss_join_req +
			       IEEE80211_IBSS_JOIN_TIMEOUT)) {
			if ((ifsta->flags & IEEE80211_STA_CREATE_IBSS) &&
			    (!(local->oper_channel->flags &
					IEEE80211_CHAN_NO_IBSS)))
				return ieee80211_sta_create_ibss(sdata, ifsta);
			if (ifsta->flags & IEEE80211_STA_CREATE_IBSS) {
				printk(KERN_DEBUG "%s: IBSS not allowed on"
				       " %d MHz\n", sdata->dev->name,
				       local->hw.conf.channel->center_freq);
			}

			/* No IBSS found - decrease scan interval and continue
			 * scanning. */
			interval = IEEE80211_SCAN_INTERVAL_SLOW;
		}

		ifsta->state = IEEE80211_STA_MLME_IBSS_SEARCH;
		mod_timer(&ifsta->timer, jiffies + interval);
		return 0;
	}

	return 0;
}


static int ieee80211_sta_config_auth(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss, *selected = NULL;
	int top_rssi = 0, freq;

	spin_lock_bh(&local->bss_lock);
	freq = local->oper_channel->center_freq;
	list_for_each_entry(bss, &local->bss_list, list) {
		if (!(bss->capability & WLAN_CAPABILITY_ESS))
			continue;

		if ((ifsta->flags & (IEEE80211_STA_AUTO_SSID_SEL |
			IEEE80211_STA_AUTO_BSSID_SEL |
			IEEE80211_STA_AUTO_CHANNEL_SEL)) &&
		    (!!(bss->capability & WLAN_CAPABILITY_PRIVACY) ^
		     !!sdata->default_key))
			continue;

		if (!(ifsta->flags & IEEE80211_STA_AUTO_CHANNEL_SEL) &&
		    bss->freq != freq)
			continue;

		if (!(ifsta->flags & IEEE80211_STA_AUTO_BSSID_SEL) &&
		    memcmp(bss->bssid, ifsta->bssid, ETH_ALEN))
			continue;

		if (!(ifsta->flags & IEEE80211_STA_AUTO_SSID_SEL) &&
		    !ieee80211_sta_match_ssid(ifsta, bss->ssid, bss->ssid_len))
			continue;

		if (!selected || top_rssi < bss->signal) {
			selected = bss;
			top_rssi = bss->signal;
		}
	}
	if (selected)
		atomic_inc(&selected->users);
	spin_unlock_bh(&local->bss_lock);

	if (selected) {
		ieee80211_set_freq(sdata, selected->freq);
		if (!(ifsta->flags & IEEE80211_STA_SSID_SET))
			ieee80211_sta_set_ssid(sdata, selected->ssid,
					       selected->ssid_len);
		ieee80211_sta_set_bssid(sdata, selected->bssid);
		ieee80211_sta_def_wmm_params(sdata, selected);

		/* Send out direct probe if no probe resp was received or
		 * the one we have is outdated
		 */
		if (!selected->last_probe_resp ||
		    time_after(jiffies, selected->last_probe_resp
					+ IEEE80211_SCAN_RESULT_EXPIRE))
			ifsta->state = IEEE80211_STA_MLME_DIRECT_PROBE;
		else
			ifsta->state = IEEE80211_STA_MLME_AUTHENTICATE;

		ieee80211_rx_bss_put(local, selected);
		ieee80211_sta_reset_auth(sdata, ifsta);
		return 0;
	} else {
		if (ifsta->assoc_scan_tries < IEEE80211_ASSOC_SCANS_MAX_TRIES) {
			ifsta->assoc_scan_tries++;
			if (ifsta->flags & IEEE80211_STA_AUTO_SSID_SEL)
				ieee80211_start_scan(sdata, NULL, 0);
			else
				ieee80211_start_scan(sdata, ifsta->ssid,
							 ifsta->ssid_len);
			ifsta->state = IEEE80211_STA_MLME_AUTHENTICATE;
			set_bit(IEEE80211_STA_REQ_AUTH, &ifsta->request);
		} else
			ifsta->state = IEEE80211_STA_MLME_DISABLED;
	}
	return -1;
}


static void ieee80211_sta_work(struct work_struct *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data, u.sta.work);
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_sta *ifsta;
	struct sk_buff *skb;

	if (!netif_running(sdata->dev))
		return;

	if (local->sw_scanning || local->hw_scanning)
		return;

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_STATION &&
		    sdata->vif.type != NL80211_IFTYPE_ADHOC))
		return;
	ifsta = &sdata->u.sta;

	while ((skb = skb_dequeue(&ifsta->skb_queue)))
		ieee80211_sta_rx_queued_mgmt(sdata, skb);

	if (ifsta->state != IEEE80211_STA_MLME_DIRECT_PROBE &&
	    ifsta->state != IEEE80211_STA_MLME_AUTHENTICATE &&
	    ifsta->state != IEEE80211_STA_MLME_ASSOCIATE &&
	    test_and_clear_bit(IEEE80211_STA_REQ_SCAN, &ifsta->request)) {
		ieee80211_start_scan(sdata, ifsta->scan_ssid,
				     ifsta->scan_ssid_len);
		return;
	}

	if (test_and_clear_bit(IEEE80211_STA_REQ_AUTH, &ifsta->request)) {
		if (ieee80211_sta_config_auth(sdata, ifsta))
			return;
		clear_bit(IEEE80211_STA_REQ_RUN, &ifsta->request);
	} else if (!test_and_clear_bit(IEEE80211_STA_REQ_RUN, &ifsta->request))
		return;

	switch (ifsta->state) {
	case IEEE80211_STA_MLME_DISABLED:
		break;
	case IEEE80211_STA_MLME_DIRECT_PROBE:
		ieee80211_direct_probe(sdata, ifsta);
		break;
	case IEEE80211_STA_MLME_AUTHENTICATE:
		ieee80211_authenticate(sdata, ifsta);
		break;
	case IEEE80211_STA_MLME_ASSOCIATE:
		ieee80211_associate(sdata, ifsta);
		break;
	case IEEE80211_STA_MLME_ASSOCIATED:
		ieee80211_associated(sdata, ifsta);
		break;
	case IEEE80211_STA_MLME_IBSS_SEARCH:
		ieee80211_sta_find_ibss(sdata, ifsta);
		break;
	case IEEE80211_STA_MLME_IBSS_JOINED:
		ieee80211_sta_merge_ibss(sdata, ifsta);
		break;
	default:
		WARN_ON(1);
		break;
	}

	if (ieee80211_privacy_mismatch(sdata, ifsta)) {
		printk(KERN_DEBUG "%s: privacy configuration mismatch and "
		       "mixed-cell disabled - disassociate\n", sdata->dev->name);

		ieee80211_set_disassoc(sdata, ifsta, false, true,
					WLAN_REASON_UNSPECIFIED);
	}
}

static void ieee80211_restart_sta_timer(struct ieee80211_sub_if_data *sdata)
{
	if (sdata->vif.type == NL80211_IFTYPE_STATION)
		queue_work(sdata->local->hw.workqueue,
			   &sdata->u.sta.work);
}

/* interface setup */
void ieee80211_sta_setup_sdata(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_sta *ifsta;

	ifsta = &sdata->u.sta;
	INIT_WORK(&ifsta->work, ieee80211_sta_work);
	setup_timer(&ifsta->timer, ieee80211_sta_timer,
		    (unsigned long) sdata);
	skb_queue_head_init(&ifsta->skb_queue);

	ifsta->capab = WLAN_CAPABILITY_ESS;
	ifsta->auth_algs = IEEE80211_AUTH_ALG_OPEN |
		IEEE80211_AUTH_ALG_SHARED_KEY;
	ifsta->flags |= IEEE80211_STA_CREATE_IBSS |
		IEEE80211_STA_AUTO_BSSID_SEL |
		IEEE80211_STA_AUTO_CHANNEL_SEL;
	if (ieee80211_num_regular_queues(&sdata->local->hw) >= 4)
		ifsta->flags |= IEEE80211_STA_WMM_ENABLED;
}

/*
 * Add a new IBSS station, will also be called by the RX code when,
 * in IBSS mode, receiving a frame from a yet-unknown station, hence
 * must be callable in atomic context.
 */
struct sta_info *ieee80211_ibss_add_sta(struct ieee80211_sub_if_data *sdata,
					u8 *bssid,u8 *addr, u64 supp_rates)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	int band = local->hw.conf.channel->band;

	/* TODO: Could consider removing the least recently used entry and
	 * allow new one to be added. */
	if (local->num_sta >= IEEE80211_IBSS_MAX_STA_ENTRIES) {
		if (net_ratelimit()) {
			printk(KERN_DEBUG "%s: No room for a new IBSS STA "
			       "entry %pM\n", sdata->dev->name, addr);
		}
		return NULL;
	}

	if (compare_ether_addr(bssid, sdata->u.sta.bssid))
		return NULL;

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Adding new IBSS station %pM (dev=%s)\n",
	       wiphy_name(local->hw.wiphy), addr, sdata->dev->name);
#endif

	sta = sta_info_alloc(sdata, addr, GFP_ATOMIC);
	if (!sta)
		return NULL;

	set_sta_flags(sta, WLAN_STA_AUTHORIZED);

	/* make sure mandatory rates are always added */
	sta->sta.supp_rates[band] = supp_rates |
			ieee80211_mandatory_rates(local, band);

	rate_control_rate_init(sta);

	if (sta_info_insert(sta))
		return NULL;

	return sta;
}

/* configuration hooks */
void ieee80211_sta_req_auth(struct ieee80211_sub_if_data *sdata,
			    struct ieee80211_if_sta *ifsta)
{
	struct ieee80211_local *local = sdata->local;

	if (sdata->vif.type != NL80211_IFTYPE_STATION)
		return;

	if ((ifsta->flags & (IEEE80211_STA_BSSID_SET |
			     IEEE80211_STA_AUTO_BSSID_SEL)) &&
	    (ifsta->flags & (IEEE80211_STA_SSID_SET |
			     IEEE80211_STA_AUTO_SSID_SEL))) {

		if (ifsta->state == IEEE80211_STA_MLME_ASSOCIATED)
			ieee80211_set_disassoc(sdata, ifsta, true, true,
					       WLAN_REASON_DEAUTH_LEAVING);

		set_bit(IEEE80211_STA_REQ_AUTH, &ifsta->request);
		queue_work(local->hw.workqueue, &ifsta->work);
	}
}

int ieee80211_sta_set_ssid(struct ieee80211_sub_if_data *sdata, char *ssid, size_t len)
{
	struct ieee80211_if_sta *ifsta;

	if (len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;

	ifsta = &sdata->u.sta;

	if (ifsta->ssid_len != len || memcmp(ifsta->ssid, ssid, len) != 0) {
		memset(ifsta->ssid, 0, sizeof(ifsta->ssid));
		memcpy(ifsta->ssid, ssid, len);
		ifsta->ssid_len = len;
		ifsta->flags &= ~IEEE80211_STA_PREV_BSSID_SET;
	}

	if (len)
		ifsta->flags |= IEEE80211_STA_SSID_SET;
	else
		ifsta->flags &= ~IEEE80211_STA_SSID_SET;

	if (sdata->vif.type == NL80211_IFTYPE_ADHOC &&
	    !(ifsta->flags & IEEE80211_STA_BSSID_SET)) {
		ifsta->ibss_join_req = jiffies;
		ifsta->state = IEEE80211_STA_MLME_IBSS_SEARCH;
		return ieee80211_sta_find_ibss(sdata, ifsta);
	}

	return 0;
}

int ieee80211_sta_get_ssid(struct ieee80211_sub_if_data *sdata, char *ssid, size_t *len)
{
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;
	memcpy(ssid, ifsta->ssid, ifsta->ssid_len);
	*len = ifsta->ssid_len;
	return 0;
}

int ieee80211_sta_set_bssid(struct ieee80211_sub_if_data *sdata, u8 *bssid)
{
	struct ieee80211_if_sta *ifsta;
	int res;

	ifsta = &sdata->u.sta;

	if (memcmp(ifsta->bssid, bssid, ETH_ALEN) != 0) {
		memcpy(ifsta->bssid, bssid, ETH_ALEN);
		res = 0;
		/*
		 * Hack! See also ieee80211_sta_set_ssid.
		 */
		if (netif_running(sdata->dev))
			res = ieee80211_if_config(sdata, IEEE80211_IFCC_BSSID);
		if (res) {
			printk(KERN_DEBUG "%s: Failed to config new BSSID to "
			       "the low-level driver\n", sdata->dev->name);
			return res;
		}
	}

	if (is_valid_ether_addr(bssid))
		ifsta->flags |= IEEE80211_STA_BSSID_SET;
	else
		ifsta->flags &= ~IEEE80211_STA_BSSID_SET;

	return 0;
}

int ieee80211_sta_set_extra_ie(struct ieee80211_sub_if_data *sdata, char *ie, size_t len)
{
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;

	kfree(ifsta->extra_ie);
	if (len == 0) {
		ifsta->extra_ie = NULL;
		ifsta->extra_ie_len = 0;
		return 0;
	}
	ifsta->extra_ie = kmalloc(len, GFP_KERNEL);
	if (!ifsta->extra_ie) {
		ifsta->extra_ie_len = 0;
		return -ENOMEM;
	}
	memcpy(ifsta->extra_ie, ie, len);
	ifsta->extra_ie_len = len;
	return 0;
}

int ieee80211_sta_deauthenticate(struct ieee80211_sub_if_data *sdata, u16 reason)
{
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;

	printk(KERN_DEBUG "%s: deauthenticating by local choice (reason=%d)\n",
	       sdata->dev->name, reason);

	if (sdata->vif.type != NL80211_IFTYPE_STATION &&
	    sdata->vif.type != NL80211_IFTYPE_ADHOC)
		return -EINVAL;

	ieee80211_set_disassoc(sdata, ifsta, true, true, reason);
	return 0;
}

int ieee80211_sta_disassociate(struct ieee80211_sub_if_data *sdata, u16 reason)
{
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;

	printk(KERN_DEBUG "%s: disassociating by local choice (reason=%d)\n",
	       sdata->dev->name, reason);

	if (sdata->vif.type != NL80211_IFTYPE_STATION)
		return -EINVAL;

	if (!(ifsta->flags & IEEE80211_STA_ASSOCIATED))
		return -1;

	ieee80211_set_disassoc(sdata, ifsta, false, true, reason);
	return 0;
}

/* scan finished notification */
void ieee80211_mlme_notify_scan_completed(struct ieee80211_local *local)
{
	struct ieee80211_sub_if_data *sdata = local->scan_sdata;
	struct ieee80211_if_sta *ifsta;

	if (sdata && sdata->vif.type == NL80211_IFTYPE_ADHOC) {
		ifsta = &sdata->u.sta;
		if (!(ifsta->flags & IEEE80211_STA_BSSID_SET) ||
		    (!(ifsta->state == IEEE80211_STA_MLME_IBSS_JOINED) &&
		    !ieee80211_sta_active_ibss(sdata)))
			ieee80211_sta_find_ibss(sdata, ifsta);
	}

	/* Restart STA timers */
	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list)
		ieee80211_restart_sta_timer(sdata);
	rcu_read_unlock();
}

void ieee80211_dynamic_ps_disable_work(struct work_struct *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local,
			     dynamic_ps_disable_work);

	if (local->hw.conf.flags & IEEE80211_CONF_PS) {
		local->hw.conf.flags &= ~IEEE80211_CONF_PS;
		ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_PS);
	}

	ieee80211_wake_queues_by_reason(&local->hw,
					IEEE80211_QUEUE_STOP_REASON_PS);
}

void ieee80211_dynamic_ps_enable_work(struct work_struct *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local,
			     dynamic_ps_enable_work);

	if (local->hw.conf.flags & IEEE80211_CONF_PS)
		return;

	local->hw.conf.flags |= IEEE80211_CONF_PS;

	ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_PS);
}

void ieee80211_dynamic_ps_timer(unsigned long data)
{
	struct ieee80211_local *local = (void *) data;

	queue_work(local->hw.workqueue, &local->dynamic_ps_enable_work);
}
