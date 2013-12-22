/******************************************************************************
 *
 * Copyright(c) 2005 - 2008 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 * James P. Ketrenos <ipw2100-admin@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 *****************************************************************************/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/wireless.h>
#include <net/mac80211.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>

#include <linux/workqueue.h>

#include "iwl-3945.h"

#define RS_NAME "iwl-3945-rs"

struct iwl3945_rate_scale_data {
	u64 data;
	s32 success_counter;
	s32 success_ratio;
	s32 counter;
	s32 average_tpt;
	unsigned long stamp;
};

struct iwl3945_rs_sta {
	spinlock_t lock;
	s32 *expected_tpt;
	unsigned long last_partial_flush;
	unsigned long last_flush;
	u32 flush_time;
	u32 last_tx_packets;
	u32 tx_packets;
	u8 tgg;
	u8 flush_pending;
	u8 start_rate;
	u8 ibss_sta_added;
	struct timer_list rate_scale_flush;
	struct iwl3945_rate_scale_data win[IWL_RATE_COUNT];

	/* used to be in sta_info */
	int last_txrate_idx;
};

static s32 iwl3945_expected_tpt_g[IWL_RATE_COUNT] = {
	7, 13, 35, 58, 0, 0, 76, 104, 130, 168, 191, 202
};

static s32 iwl3945_expected_tpt_g_prot[IWL_RATE_COUNT] = {
	7, 13, 35, 58, 0, 0, 0, 80, 93, 113, 123, 125
};

static s32 iwl3945_expected_tpt_a[IWL_RATE_COUNT] = {
	0, 0, 0, 0, 40, 57, 72, 98, 121, 154, 177, 186
};

static s32 iwl3945_expected_tpt_b[IWL_RATE_COUNT] = {
	7, 13, 35, 58, 0, 0, 0, 0, 0, 0, 0, 0
};

struct iwl3945_tpt_entry {
	s8 min_rssi;
	u8 index;
};

static struct iwl3945_tpt_entry iwl3945_tpt_table_a[] = {
	{-60, IWL_RATE_54M_INDEX},
	{-64, IWL_RATE_48M_INDEX},
	{-72, IWL_RATE_36M_INDEX},
	{-80, IWL_RATE_24M_INDEX},
	{-84, IWL_RATE_18M_INDEX},
	{-85, IWL_RATE_12M_INDEX},
	{-87, IWL_RATE_9M_INDEX},
	{-89, IWL_RATE_6M_INDEX}
};

static struct iwl3945_tpt_entry iwl3945_tpt_table_g[] = {
	{-60, IWL_RATE_54M_INDEX},
	{-64, IWL_RATE_48M_INDEX},
	{-68, IWL_RATE_36M_INDEX},
	{-80, IWL_RATE_24M_INDEX},
	{-84, IWL_RATE_18M_INDEX},
	{-85, IWL_RATE_12M_INDEX},
	{-86, IWL_RATE_11M_INDEX},
	{-88, IWL_RATE_5M_INDEX},
	{-90, IWL_RATE_2M_INDEX},
	{-92, IWL_RATE_1M_INDEX}
};

#define IWL_RATE_MAX_WINDOW          62
#define IWL_RATE_FLUSH        (3*HZ/10)
#define IWL_RATE_WIN_FLUSH       (HZ/2)
#define IWL_RATE_HIGH_TH          11520
#define IWL_RATE_MIN_FAILURE_TH       8
#define IWL_RATE_MIN_SUCCESS_TH       8
#define IWL_RATE_DECREASE_TH       1920

static u8 iwl3945_get_rate_index_by_rssi(s32 rssi, enum ieee80211_band band)
{
	u32 index = 0;
	u32 table_size = 0;
	struct iwl3945_tpt_entry *tpt_table = NULL;

	if ((rssi < IWL_MIN_RSSI_VAL) || (rssi > IWL_MAX_RSSI_VAL))
		rssi = IWL_MIN_RSSI_VAL;

	switch (band) {
	case IEEE80211_BAND_2GHZ:
		tpt_table = iwl3945_tpt_table_g;
		table_size = ARRAY_SIZE(iwl3945_tpt_table_g);
		break;

	case IEEE80211_BAND_5GHZ:
		tpt_table = iwl3945_tpt_table_a;
		table_size = ARRAY_SIZE(iwl3945_tpt_table_a);
		break;

	default:
		BUG();
		break;
	}

	while ((index < table_size) && (rssi < tpt_table[index].min_rssi))
		index++;

	index = min(index, (table_size - 1));

	return tpt_table[index].index;
}

static void iwl3945_clear_window(struct iwl3945_rate_scale_data *window)
{
	window->data = 0;
	window->success_counter = 0;
	window->success_ratio = -1;
	window->counter = 0;
	window->average_tpt = IWL_INV_TPT;
	window->stamp = 0;
}

/**
 * iwl3945_rate_scale_flush_windows - flush out the rate scale windows
 *
 * Returns the number of windows that have gathered data but were
 * not flushed.  If there were any that were not flushed, then
 * reschedule the rate flushing routine.
 */
static int iwl3945_rate_scale_flush_windows(struct iwl3945_rs_sta *rs_sta)
{
	int unflushed = 0;
	int i;
	unsigned long flags;

	/*
	 * For each rate, if we have collected data on that rate
	 * and it has been more than IWL_RATE_WIN_FLUSH
	 * since we flushed, clear out the gathered statistics
	 */
	for (i = 0; i < IWL_RATE_COUNT; i++) {
		if (!rs_sta->win[i].counter)
			continue;

		spin_lock_irqsave(&rs_sta->lock, flags);
		if (time_after(jiffies, rs_sta->win[i].stamp +
			       IWL_RATE_WIN_FLUSH)) {
			IWL_DEBUG_RATE("flushing %d samples of rate "
				       "index %d\n",
				       rs_sta->win[i].counter, i);
			iwl3945_clear_window(&rs_sta->win[i]);
		} else
			unflushed++;
		spin_unlock_irqrestore(&rs_sta->lock, flags);
	}

	return unflushed;
}

#define IWL_RATE_FLUSH_MAX              5000	/* msec */
#define IWL_RATE_FLUSH_MIN              50	/* msec */

static void iwl3945_bg_rate_scale_flush(unsigned long data)
{
	struct iwl3945_rs_sta *rs_sta = (void *)data;
	int unflushed = 0;
	unsigned long flags;
	u32 packet_count, duration, pps;

	IWL_DEBUG_RATE("enter\n");

	unflushed = iwl3945_rate_scale_flush_windows(rs_sta);

	spin_lock_irqsave(&rs_sta->lock, flags);

	rs_sta->flush_pending = 0;

	/* Number of packets Rx'd since last time this timer ran */
	packet_count = (rs_sta->tx_packets - rs_sta->last_tx_packets) + 1;

	rs_sta->last_tx_packets = rs_sta->tx_packets + 1;

	if (unflushed) {
		duration =
		    jiffies_to_msecs(jiffies - rs_sta->last_partial_flush);
/*              duration = jiffies_to_msecs(rs_sta->flush_time); */

		IWL_DEBUG_RATE("Tx'd %d packets in %dms\n",
			       packet_count, duration);

		/* Determine packets per second */
		if (duration)
			pps = (packet_count * 1000) / duration;
		else
			pps = 0;

		if (pps) {
			duration = IWL_RATE_FLUSH_MAX / pps;
			if (duration < IWL_RATE_FLUSH_MIN)
				duration = IWL_RATE_FLUSH_MIN;
		} else
			duration = IWL_RATE_FLUSH_MAX;

		rs_sta->flush_time = msecs_to_jiffies(duration);

		IWL_DEBUG_RATE("new flush period: %d msec ave %d\n",
			       duration, packet_count);

		mod_timer(&rs_sta->rate_scale_flush, jiffies +
			  rs_sta->flush_time);

		rs_sta->last_partial_flush = jiffies;
	}

	/* If there weren't any unflushed entries, we don't schedule the timer
	 * to run again */

	rs_sta->last_flush = jiffies;

	spin_unlock_irqrestore(&rs_sta->lock, flags);

	IWL_DEBUG_RATE("leave\n");
}

/**
 * iwl3945_collect_tx_data - Update the success/failure sliding window
 *
 * We keep a sliding window of the last 64 packets transmitted
 * at this rate.  window->data contains the bitmask of successful
 * packets.
 */
static void iwl3945_collect_tx_data(struct iwl3945_rs_sta *rs_sta,
				struct iwl3945_rate_scale_data *window,
				int success, int retries)
{
	unsigned long flags;

	if (!retries) {
		IWL_DEBUG_RATE("leave: retries == 0 -- should be at least 1\n");
		return;
	}

	while (retries--) {
		spin_lock_irqsave(&rs_sta->lock, flags);

		/* If we have filled up the window then subtract one from the
		 * success counter if the high-bit is counting toward
		 * success */
		if (window->counter == IWL_RATE_MAX_WINDOW) {
			if (window->data & (1ULL << (IWL_RATE_MAX_WINDOW - 1)))
				window->success_counter--;
		} else
			window->counter++;

		/* Slide the window to the left one bit */
		window->data = (window->data << 1);

		/* If this packet was a success then set the low bit high */
		if (success) {
			window->success_counter++;
			window->data |= 1;
		}

		/* window->counter can't be 0 -- it is either >0 or
		 * IWL_RATE_MAX_WINDOW */
		window->success_ratio = 12800 * window->success_counter /
		    window->counter;

		/* Tag this window as having been updated */
		window->stamp = jiffies;

		spin_unlock_irqrestore(&rs_sta->lock, flags);
	}
}

static void rs_rate_init(void *priv, struct ieee80211_supported_band *sband,
			 struct ieee80211_sta *sta, void *priv_sta)
{
	struct iwl3945_rs_sta *rs_sta = priv_sta;
	int i;

	IWL_DEBUG_RATE("enter\n");

	/* TODO: what is a good starting rate for STA? About middle? Maybe not
	 * the lowest or the highest rate.. Could consider using RSSI from
	 * previous packets? Need to have IEEE 802.1X auth succeed immediately
	 * after assoc.. */

	for (i = IWL_RATE_COUNT - 1; i >= 0; i--) {
		if (sta->supp_rates[sband->band] & (1 << i)) {
			rs_sta->last_txrate_idx = i;
			break;
		}
	}

	/* For 5 GHz band it start at IWL_FIRST_OFDM_RATE */
	if (sband->band == IEEE80211_BAND_5GHZ)
		rs_sta->last_txrate_idx += IWL_FIRST_OFDM_RATE;

	IWL_DEBUG_RATE("leave\n");
}

static void *rs_alloc(struct ieee80211_hw *hw, struct dentry *debugfsdir)
{
	return hw->priv;
}

/* rate scale requires free function to be implemented */
static void rs_free(void *priv)
{
	return;
}

static void rs_clear(void *priv)
{
	return;
}


static void *rs_alloc_sta(void *priv, struct ieee80211_sta *sta, gfp_t gfp)
{
	struct iwl3945_rs_sta *rs_sta;
	struct iwl3945_sta_priv *psta = (void *) sta->drv_priv;
	int i;

	/*
	 * XXX: If it's using sta->drv_priv anyway, it might
	 *	as well just put all the information there.
	 */

	IWL_DEBUG_RATE("enter\n");

	rs_sta = kzalloc(sizeof(struct iwl3945_rs_sta), gfp);
	if (!rs_sta) {
		IWL_DEBUG_RATE("leave: ENOMEM\n");
		return NULL;
	}

	psta->rs_sta = rs_sta;

	spin_lock_init(&rs_sta->lock);

	rs_sta->start_rate = IWL_RATE_INVALID;

	/* default to just 802.11b */
	rs_sta->expected_tpt = iwl3945_expected_tpt_b;

	rs_sta->last_partial_flush = jiffies;
	rs_sta->last_flush = jiffies;
	rs_sta->flush_time = IWL_RATE_FLUSH;
	rs_sta->last_tx_packets = 0;
	rs_sta->ibss_sta_added = 0;

	init_timer(&rs_sta->rate_scale_flush);
	rs_sta->rate_scale_flush.data = (unsigned long)rs_sta;
	rs_sta->rate_scale_flush.function = &iwl3945_bg_rate_scale_flush;

	for (i = 0; i < IWL_RATE_COUNT; i++)
		iwl3945_clear_window(&rs_sta->win[i]);

	IWL_DEBUG_RATE("leave\n");

	return rs_sta;
}

static void rs_free_sta(void *priv, struct ieee80211_sta *sta,
			void *priv_sta)
{
	struct iwl3945_sta_priv *psta = (void *) sta->drv_priv;
	struct iwl3945_rs_sta *rs_sta = priv_sta;

	psta->rs_sta = NULL;

	IWL_DEBUG_RATE("enter\n");
	del_timer_sync(&rs_sta->rate_scale_flush);
	kfree(rs_sta);
	IWL_DEBUG_RATE("leave\n");
}


/*
 * get ieee prev rate from rate scale table.
 * for A and B mode we need to overright prev
 * value
 */
static int rs_adjust_next_rate(struct iwl3945_priv *priv, int rate)
{
	int next_rate = iwl3945_get_prev_ieee_rate(rate);

	switch (priv->band) {
	case IEEE80211_BAND_5GHZ:
		if (rate == IWL_RATE_12M_INDEX)
			next_rate = IWL_RATE_9M_INDEX;
		else if (rate == IWL_RATE_6M_INDEX)
			next_rate = IWL_RATE_6M_INDEX;
		break;
/* XXX cannot be invoked in current mac80211 so not a regression
	case MODE_IEEE80211B:
		if (rate == IWL_RATE_11M_INDEX_TABLE)
			next_rate = IWL_RATE_5M_INDEX_TABLE;
		break;
 */
	default:
		break;
	}

	return next_rate;
}
/**
 * rs_tx_status - Update rate control values based on Tx results
 *
 * NOTE: Uses iwl3945_priv->retry_rate for the # of retries attempted by
 * the hardware for each rate.
 */
static void rs_tx_status(void *priv_rate, struct ieee80211_supported_band *sband,
			 struct ieee80211_sta *sta, void *priv_sta,
			 struct sk_buff *skb)
{
	u8 retries, current_count;
	int scale_rate_index, first_index, last_index;
	unsigned long flags;
	struct iwl3945_priv *priv = (struct iwl3945_priv *)priv_rate;
	struct iwl3945_rs_sta *rs_sta = priv_sta;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	IWL_DEBUG_RATE("enter\n");

	retries = info->status.retry_count;
	first_index = sband->bitrates[info->tx_rate_idx].hw_value;
	if ((first_index < 0) || (first_index >= IWL_RATE_COUNT)) {
		IWL_DEBUG_RATE("leave: Rate out of bounds: %d\n", first_index);
		return;
	}

	if (!priv_sta) {
		IWL_DEBUG_RATE("leave: No STA priv data to update!\n");
		return;
	}

	rs_sta->tx_packets++;

	scale_rate_index = first_index;
	last_index = first_index;

	/*
	 * Update the window for each rate.  We determine which rates
	 * were Tx'd based on the total number of retries vs. the number
	 * of retries configured for each rate -- currently set to the
	 * priv value 'retry_rate' vs. rate specific
	 *
	 * On exit from this while loop last_index indicates the rate
	 * at which the frame was finally transmitted (or failed if no
	 * ACK)
	 */
	while (retries > 0) {
		if (retries < priv->retry_rate) {
			current_count = retries;
			last_index = scale_rate_index;
		} else {
			current_count = priv->retry_rate;
			last_index = rs_adjust_next_rate(priv,
							 scale_rate_index);
		}

		/* Update this rate accounting for as many retries
		 * as was used for it (per current_count) */
		iwl3945_collect_tx_data(rs_sta,
				    &rs_sta->win[scale_rate_index],
				    0, current_count);
		IWL_DEBUG_RATE("Update rate %d for %d retries.\n",
			       scale_rate_index, current_count);

		retries -= current_count;

		if (retries)
			scale_rate_index =
			    rs_adjust_next_rate(priv, scale_rate_index);
	}


	/* Update the last index window with success/failure based on ACK */
	IWL_DEBUG_RATE("Update rate %d with %s.\n",
		       last_index,
		       (info->flags & IEEE80211_TX_STAT_ACK) ?
		       "success" : "failure");
	iwl3945_collect_tx_data(rs_sta,
			    &rs_sta->win[last_index],
			    info->flags & IEEE80211_TX_STAT_ACK, 1);

	/* We updated the rate scale window -- if its been more than
	 * flush_time since the last run, schedule the flush
	 * again */
	spin_lock_irqsave(&rs_sta->lock, flags);

	if (!rs_sta->flush_pending &&
	    time_after(jiffies, rs_sta->last_partial_flush +
		       rs_sta->flush_time)) {

		rs_sta->flush_pending = 1;
		mod_timer(&rs_sta->rate_scale_flush,
			  jiffies + rs_sta->flush_time);
	}

	spin_unlock_irqrestore(&rs_sta->lock, flags);

	IWL_DEBUG_RATE("leave\n");

	return;
}

static u16 iwl3945_get_adjacent_rate(struct iwl3945_rs_sta *rs_sta,
				 u8 index, u16 rate_mask, enum ieee80211_band band)
{
	u8 high = IWL_RATE_INVALID;
	u8 low = IWL_RATE_INVALID;

	/* 802.11A walks to the next literal adjacent rate in
	 * the rate table */
	if (unlikely(band == IEEE80211_BAND_5GHZ)) {
		int i;
		u32 mask;

		/* Find the previous rate that is in the rate mask */
		i = index - 1;
		for (mask = (1 << i); i >= 0; i--, mask >>= 1) {
			if (rate_mask & mask) {
				low = i;
				break;
			}
		}

		/* Find the next rate that is in the rate mask */
		i = index + 1;
		for (mask = (1 << i); i < IWL_RATE_COUNT; i++, mask <<= 1) {
			if (rate_mask & mask) {
				high = i;
				break;
			}
		}

		return (high << 8) | low;
	}

	low = index;
	while (low != IWL_RATE_INVALID) {
		if (rs_sta->tgg)
			low = iwl3945_rates[low].prev_rs_tgg;
		else
			low = iwl3945_rates[low].prev_rs;
		if (low == IWL_RATE_INVALID)
			break;
		if (rate_mask & (1 << low))
			break;
		IWL_DEBUG_RATE("Skipping masked lower rate: %d\n", low);
	}

	high = index;
	while (high != IWL_RATE_INVALID) {
		if (rs_sta->tgg)
			high = iwl3945_rates[high].next_rs_tgg;
		else
			high = iwl3945_rates[high].next_rs;
		if (high == IWL_RATE_INVALID)
			break;
		if (rate_mask & (1 << high))
			break;
		IWL_DEBUG_RATE("Skipping masked higher rate: %d\n", high);
	}

	return (high << 8) | low;
}

/**
 * rs_get_rate - find the rate for the requested packet
 *
 * Returns the ieee80211_rate structure allocated by the driver.
 *
 * The rate control algorithm has no internal mapping between hw_mode's
 * rate ordering and the rate ordering used by the rate control algorithm.
 *
 * The rate control algorithm uses a single table of rates that goes across
 * the entire A/B/G spectrum vs. being limited to just one particular
 * hw_mode.
 *
 * As such, we can't convert the index obtained below into the hw_mode's
 * rate table and must reference the driver allocated rate table
 *
 */
static void rs_get_rate(void *priv_r, struct ieee80211_supported_band *sband,
			struct ieee80211_sta *sta, void *priv_sta,
			struct sk_buff *skb, struct rate_selection *sel)
{
	u8 low = IWL_RATE_INVALID;
	u8 high = IWL_RATE_INVALID;
	u16 high_low;
	int index;
	struct iwl3945_rs_sta *rs_sta = priv_sta;
	struct iwl3945_rate_scale_data *window = NULL;
	int current_tpt = IWL_INV_TPT;
	int low_tpt = IWL_INV_TPT;
	int high_tpt = IWL_INV_TPT;
	u32 fail_count;
	s8 scale_action = 0;
	unsigned long flags;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	u16 fc;
	u16 rate_mask = 0;
	struct iwl3945_priv *priv = (struct iwl3945_priv *)priv_r;
	DECLARE_MAC_BUF(mac);

	IWL_DEBUG_RATE("enter\n");

	if (sta)
		rate_mask = sta->supp_rates[sband->band];

	/* Send management frames and broadcast/multicast data using lowest
	 * rate. */
	fc = le16_to_cpu(hdr->frame_control);
	if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_DATA ||
	    is_multicast_ether_addr(hdr->addr1) ||
	    !sta || !priv_sta) {
		IWL_DEBUG_RATE("leave: No STA priv data to update!\n");
		if (!rate_mask)
			sel->rate_idx = rate_lowest_index(sband, NULL);
		else
			sel->rate_idx = rate_lowest_index(sband, sta);
		return;
	}

	index = min(rs_sta->last_txrate_idx & 0xffff, IWL_RATE_COUNT - 1);

	if (sband->band == IEEE80211_BAND_5GHZ)
		rate_mask = rate_mask << IWL_FIRST_OFDM_RATE;

	if ((priv->iw_mode == NL80211_IFTYPE_ADHOC) &&
	    !rs_sta->ibss_sta_added) {
		u8 sta_id = iwl3945_hw_find_station(priv, hdr->addr1);

		if (sta_id == IWL_INVALID_STATION) {
			IWL_DEBUG_RATE("LQ: ADD station %s\n",
				       print_mac(mac, hdr->addr1));
			sta_id = iwl3945_add_station(priv,
				    hdr->addr1, 0, CMD_ASYNC);
		}
		if (sta_id != IWL_INVALID_STATION)
			rs_sta->ibss_sta_added = 1;
	}

	spin_lock_irqsave(&rs_sta->lock, flags);

	if (rs_sta->start_rate != IWL_RATE_INVALID) {
		index = rs_sta->start_rate;
		rs_sta->start_rate = IWL_RATE_INVALID;
	}

	window = &(rs_sta->win[index]);

	fail_count = window->counter - window->success_counter;

	if (((fail_count <= IWL_RATE_MIN_FAILURE_TH) &&
	     (window->success_counter < IWL_RATE_MIN_SUCCESS_TH))) {
		window->average_tpt = IWL_INV_TPT;
		spin_unlock_irqrestore(&rs_sta->lock, flags);

		IWL_DEBUG_RATE("Invalid average_tpt on rate %d: "
			       "counter: %d, success_counter: %d, "
			       "expected_tpt is %sNULL\n",
			       index,
			       window->counter,
			       window->success_counter,
			       rs_sta->expected_tpt ? "not " : "");
		goto out;

	}

	window->average_tpt = ((window->success_ratio *
				rs_sta->expected_tpt[index] + 64) / 128);
	current_tpt = window->average_tpt;

	high_low = iwl3945_get_adjacent_rate(rs_sta, index, rate_mask,
					     sband->band);
	low = high_low & 0xff;
	high = (high_low >> 8) & 0xff;

	if (low != IWL_RATE_INVALID)
		low_tpt = rs_sta->win[low].average_tpt;

	if (high != IWL_RATE_INVALID)
		high_tpt = rs_sta->win[high].average_tpt;

	spin_unlock_irqrestore(&rs_sta->lock, flags);

	scale_action = 1;

	if ((window->success_ratio < IWL_RATE_DECREASE_TH) || !current_tpt) {
		IWL_DEBUG_RATE("decrease rate because of low success_ratio\n");
		scale_action = -1;
	} else if ((low_tpt == IWL_INV_TPT) && (high_tpt == IWL_INV_TPT))
		scale_action = 1;
	else if ((low_tpt != IWL_INV_TPT) && (high_tpt != IWL_INV_TPT) &&
		 (low_tpt < current_tpt) && (high_tpt < current_tpt)) {
		IWL_DEBUG_RATE("No action -- low [%d] & high [%d] < "
			       "current_tpt [%d]\n",
			       low_tpt, high_tpt, current_tpt);
		scale_action = 0;
	} else {
		if (high_tpt != IWL_INV_TPT) {
			if (high_tpt > current_tpt)
				scale_action = 1;
			else {
				IWL_DEBUG_RATE
				    ("decrease rate because of high tpt\n");
				scale_action = -1;
			}
		} else if (low_tpt != IWL_INV_TPT) {
			if (low_tpt > current_tpt) {
				IWL_DEBUG_RATE
				    ("decrease rate because of low tpt\n");
				scale_action = -1;
			} else
				scale_action = 1;
		}
	}

	if ((window->success_ratio > IWL_RATE_HIGH_TH) ||
	    (current_tpt > window->average_tpt)) {
		IWL_DEBUG_RATE("No action -- success_ratio [%d] > HIGH_TH or "
			       "current_tpt [%d] > average_tpt [%d]\n",
			       window->success_ratio,
			       current_tpt, window->average_tpt);
		scale_action = 0;
	}

	switch (scale_action) {
	case -1:
		if (low != IWL_RATE_INVALID)
			index = low;
		break;

	case 1:
		if (high != IWL_RATE_INVALID)
			index = high;

		break;

	case 0:
	default:
		break;
	}

	IWL_DEBUG_RATE("Selected %d (action %d) - low %d high %d\n",
		       index, scale_action, low, high);

 out:

	rs_sta->last_txrate_idx = index;
	if (sband->band == IEEE80211_BAND_5GHZ)
		sel->rate_idx = rs_sta->last_txrate_idx - IWL_FIRST_OFDM_RATE;
	else
		sel->rate_idx = rs_sta->last_txrate_idx;

	IWL_DEBUG_RATE("leave: %d\n", index);
}

static struct rate_control_ops rs_ops = {
	.module = NULL,
	.name = RS_NAME,
	.tx_status = rs_tx_status,
	.get_rate = rs_get_rate,
	.rate_init = rs_rate_init,
	.clear = rs_clear,
	.alloc = rs_alloc,
	.free = rs_free,
	.alloc_sta = rs_alloc_sta,
	.free_sta = rs_free_sta,
};

void iwl3945_rate_scale_init(struct ieee80211_hw *hw, s32 sta_id)
{
	struct iwl3945_priv *priv = hw->priv;
	s32 rssi = 0;
	unsigned long flags;
	struct iwl3945_rs_sta *rs_sta;
	struct ieee80211_sta *sta;
	struct iwl3945_sta_priv *psta;

	IWL_DEBUG_RATE("enter\n");

	rcu_read_lock();

	sta = ieee80211_find_sta(hw, priv->stations[sta_id].sta.sta.addr);
	psta = (void *) sta->drv_priv;
	if (!sta || !psta) {
		IWL_DEBUG_RATE("leave - no private rate data!\n");
		rcu_read_unlock();
		return;
	}

	rs_sta = psta->rs_sta;

	spin_lock_irqsave(&rs_sta->lock, flags);

	rs_sta->tgg = 0;
	switch (priv->band) {
	case IEEE80211_BAND_2GHZ:
		/* TODO: this always does G, not a regression */
		if (priv->active_rxon.flags & RXON_FLG_TGG_PROTECT_MSK) {
			rs_sta->tgg = 1;
			rs_sta->expected_tpt = iwl3945_expected_tpt_g_prot;
		} else
			rs_sta->expected_tpt = iwl3945_expected_tpt_g;
		break;

	case IEEE80211_BAND_5GHZ:
		rs_sta->expected_tpt = iwl3945_expected_tpt_a;
		break;
	case IEEE80211_NUM_BANDS:
		BUG();
		break;
	}

	rcu_read_unlock();
	spin_unlock_irqrestore(&rs_sta->lock, flags);

	rssi = priv->last_rx_rssi;
	if (rssi == 0)
		rssi = IWL_MIN_RSSI_VAL;

	IWL_DEBUG(IWL_DL_INFO | IWL_DL_RATE, "Network RSSI: %d\n", rssi);

	rs_sta->start_rate = iwl3945_get_rate_index_by_rssi(rssi, priv->band);

	IWL_DEBUG_RATE("leave: rssi %d assign rate index: "
		       "%d (plcp 0x%x)\n", rssi, rs_sta->start_rate,
		       iwl3945_rates[rs_sta->start_rate].plcp);
}

int iwl3945_rate_control_register(void)
{
	return ieee80211_rate_control_register(&rs_ops);
}

void iwl3945_rate_control_unregister(void)
{
	ieee80211_rate_control_unregister(&rs_ops);
}


