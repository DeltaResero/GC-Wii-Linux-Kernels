/*
	Copyright (C) 2004 - 2008 rt2x00 SourceForge Project
	<http://rt2x00.serialmonkey.com>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the
	Free Software Foundation, Inc.,
	59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
	Module: rt2x00lib
	Abstract: rt2x00 generic device routines.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include "rt2x00.h"
#include "rt2x00lib.h"

/*
 * Link tuning handlers
 */
void rt2x00lib_reset_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	if (!test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Reset link information.
	 * Both the currently active vgc level as well as
	 * the link tuner counter should be reset. Resetting
	 * the counter is important for devices where the
	 * device should only perform link tuning during the
	 * first minute after being enabled.
	 */
	rt2x00dev->link.count = 0;
	rt2x00dev->link.vgc_level = 0;

	/*
	 * Reset the link tuner.
	 */
	rt2x00dev->ops->lib->reset_tuner(rt2x00dev);
}

static void rt2x00lib_start_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	/*
	 * Clear all (possibly) pre-existing quality statistics.
	 */
	memset(&rt2x00dev->link.qual, 0, sizeof(rt2x00dev->link.qual));

	/*
	 * The RX and TX percentage should start at 50%
	 * this will assure we will get at least get some
	 * decent value when the link tuner starts.
	 * The value will be dropped and overwritten with
	 * the correct (measured )value anyway during the
	 * first run of the link tuner.
	 */
	rt2x00dev->link.qual.rx_percentage = 50;
	rt2x00dev->link.qual.tx_percentage = 50;

	rt2x00lib_reset_link_tuner(rt2x00dev);

	queue_delayed_work(rt2x00dev->hw->workqueue,
			   &rt2x00dev->link.work, LINK_TUNE_INTERVAL);
}

static void rt2x00lib_stop_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	cancel_delayed_work_sync(&rt2x00dev->link.work);
}

/*
 * Radio control handlers.
 */
int rt2x00lib_enable_radio(struct rt2x00_dev *rt2x00dev)
{
	int status;

	/*
	 * Don't enable the radio twice.
	 * And check if the hardware button has been disabled.
	 */
	if (test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags) ||
	    test_bit(DEVICE_STATE_DISABLED_RADIO_HW, &rt2x00dev->flags))
		return 0;

	/*
	 * Initialize all data queues.
	 */
	rt2x00queue_init_queues(rt2x00dev);

	/*
	 * Enable radio.
	 */
	status =
	    rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_ON);
	if (status)
		return status;

	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_IRQ_ON);

	rt2x00leds_led_radio(rt2x00dev, true);
	rt2x00led_led_activity(rt2x00dev, true);

	set_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags);

	/*
	 * Enable RX.
	 */
	rt2x00lib_toggle_rx(rt2x00dev, STATE_RADIO_RX_ON);

	/*
	 * Start the TX queues.
	 */
	ieee80211_wake_queues(rt2x00dev->hw);

	return 0;
}

void rt2x00lib_disable_radio(struct rt2x00_dev *rt2x00dev)
{
	if (!test_and_clear_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Stop the TX queues.
	 */
	ieee80211_stop_queues(rt2x00dev->hw);

	/*
	 * Disable RX.
	 */
	rt2x00lib_toggle_rx(rt2x00dev, STATE_RADIO_RX_OFF);

	/*
	 * Disable radio.
	 */
	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_OFF);
	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_IRQ_OFF);
	rt2x00led_led_activity(rt2x00dev, false);
	rt2x00leds_led_radio(rt2x00dev, false);
}

void rt2x00lib_toggle_rx(struct rt2x00_dev *rt2x00dev, enum dev_state state)
{
	/*
	 * When we are disabling the RX, we should also stop the link tuner.
	 */
	if (state == STATE_RADIO_RX_OFF)
		rt2x00lib_stop_link_tuner(rt2x00dev);

	rt2x00dev->ops->lib->set_device_state(rt2x00dev, state);

	/*
	 * When we are enabling the RX, we should also start the link tuner.
	 */
	if (state == STATE_RADIO_RX_ON &&
	    (rt2x00dev->intf_ap_count || rt2x00dev->intf_sta_count))
		rt2x00lib_start_link_tuner(rt2x00dev);
}

static void rt2x00lib_evaluate_antenna_sample(struct rt2x00_dev *rt2x00dev)
{
	struct antenna_setup ant;
	int sample_a =
	    rt2x00_get_link_ant_rssi_history(&rt2x00dev->link, ANTENNA_A);
	int sample_b =
	    rt2x00_get_link_ant_rssi_history(&rt2x00dev->link, ANTENNA_B);

	memcpy(&ant, &rt2x00dev->link.ant.active, sizeof(ant));

	/*
	 * We are done sampling. Now we should evaluate the results.
	 */
	rt2x00dev->link.ant.flags &= ~ANTENNA_MODE_SAMPLE;

	/*
	 * During the last period we have sampled the RSSI
	 * from both antenna's. It now is time to determine
	 * which antenna demonstrated the best performance.
	 * When we are already on the antenna with the best
	 * performance, then there really is nothing for us
	 * left to do.
	 */
	if (sample_a == sample_b)
		return;

	if (rt2x00dev->link.ant.flags & ANTENNA_RX_DIVERSITY)
		ant.rx = (sample_a > sample_b) ? ANTENNA_A : ANTENNA_B;

	if (rt2x00dev->link.ant.flags & ANTENNA_TX_DIVERSITY)
		ant.tx = (sample_a > sample_b) ? ANTENNA_A : ANTENNA_B;

	rt2x00lib_config_antenna(rt2x00dev, &ant);
}

static void rt2x00lib_evaluate_antenna_eval(struct rt2x00_dev *rt2x00dev)
{
	struct antenna_setup ant;
	int rssi_curr = rt2x00_get_link_ant_rssi(&rt2x00dev->link);
	int rssi_old = rt2x00_update_ant_rssi(&rt2x00dev->link, rssi_curr);

	memcpy(&ant, &rt2x00dev->link.ant.active, sizeof(ant));

	/*
	 * Legacy driver indicates that we should swap antenna's
	 * when the difference in RSSI is greater that 5. This
	 * also should be done when the RSSI was actually better
	 * then the previous sample.
	 * When the difference exceeds the threshold we should
	 * sample the rssi from the other antenna to make a valid
	 * comparison between the 2 antennas.
	 */
	if (abs(rssi_curr - rssi_old) < 5)
		return;

	rt2x00dev->link.ant.flags |= ANTENNA_MODE_SAMPLE;

	if (rt2x00dev->link.ant.flags & ANTENNA_RX_DIVERSITY)
		ant.rx = (ant.rx == ANTENNA_A) ? ANTENNA_B : ANTENNA_A;

	if (rt2x00dev->link.ant.flags & ANTENNA_TX_DIVERSITY)
		ant.tx = (ant.tx == ANTENNA_A) ? ANTENNA_B : ANTENNA_A;

	rt2x00lib_config_antenna(rt2x00dev, &ant);
}

static void rt2x00lib_evaluate_antenna(struct rt2x00_dev *rt2x00dev)
{
	/*
	 * Determine if software diversity is enabled for
	 * either the TX or RX antenna (or both).
	 * Always perform this check since within the link
	 * tuner interval the configuration might have changed.
	 */
	rt2x00dev->link.ant.flags &= ~ANTENNA_RX_DIVERSITY;
	rt2x00dev->link.ant.flags &= ~ANTENNA_TX_DIVERSITY;

	if (rt2x00dev->default_ant.rx == ANTENNA_SW_DIVERSITY)
		rt2x00dev->link.ant.flags |= ANTENNA_RX_DIVERSITY;
	if (rt2x00dev->default_ant.tx == ANTENNA_SW_DIVERSITY)
		rt2x00dev->link.ant.flags |= ANTENNA_TX_DIVERSITY;

	if (!(rt2x00dev->link.ant.flags & ANTENNA_RX_DIVERSITY) &&
	    !(rt2x00dev->link.ant.flags & ANTENNA_TX_DIVERSITY)) {
		rt2x00dev->link.ant.flags = 0;
		return;
	}

	/*
	 * If we have only sampled the data over the last period
	 * we should now harvest the data. Otherwise just evaluate
	 * the data. The latter should only be performed once
	 * every 2 seconds.
	 */
	if (rt2x00dev->link.ant.flags & ANTENNA_MODE_SAMPLE)
		rt2x00lib_evaluate_antenna_sample(rt2x00dev);
	else if (rt2x00dev->link.count & 1)
		rt2x00lib_evaluate_antenna_eval(rt2x00dev);
}

static void rt2x00lib_update_link_stats(struct link *link, int rssi)
{
	int avg_rssi = rssi;

	/*
	 * Update global RSSI
	 */
	if (link->qual.avg_rssi)
		avg_rssi = MOVING_AVERAGE(link->qual.avg_rssi, rssi, 8);
	link->qual.avg_rssi = avg_rssi;

	/*
	 * Update antenna RSSI
	 */
	if (link->ant.rssi_ant)
		rssi = MOVING_AVERAGE(link->ant.rssi_ant, rssi, 8);
	link->ant.rssi_ant = rssi;
}

static void rt2x00lib_precalculate_link_signal(struct link_qual *qual)
{
	if (qual->rx_failed || qual->rx_success)
		qual->rx_percentage =
		    (qual->rx_success * 100) /
		    (qual->rx_failed + qual->rx_success);
	else
		qual->rx_percentage = 50;

	if (qual->tx_failed || qual->tx_success)
		qual->tx_percentage =
		    (qual->tx_success * 100) /
		    (qual->tx_failed + qual->tx_success);
	else
		qual->tx_percentage = 50;

	qual->rx_success = 0;
	qual->rx_failed = 0;
	qual->tx_success = 0;
	qual->tx_failed = 0;
}

static int rt2x00lib_calculate_link_signal(struct rt2x00_dev *rt2x00dev,
					   int rssi)
{
	int rssi_percentage = 0;
	int signal;

	/*
	 * We need a positive value for the RSSI.
	 */
	if (rssi < 0)
		rssi += rt2x00dev->rssi_offset;

	/*
	 * Calculate the different percentages,
	 * which will be used for the signal.
	 */
	if (rt2x00dev->rssi_offset)
		rssi_percentage = (rssi * 100) / rt2x00dev->rssi_offset;

	/*
	 * Add the individual percentages and use the WEIGHT
	 * defines to calculate the current link signal.
	 */
	signal = ((WEIGHT_RSSI * rssi_percentage) +
		  (WEIGHT_TX * rt2x00dev->link.qual.tx_percentage) +
		  (WEIGHT_RX * rt2x00dev->link.qual.rx_percentage)) / 100;

	return (signal > 100) ? 100 : signal;
}

static void rt2x00lib_link_tuner(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, link.work.work);

	/*
	 * When the radio is shutting down we should
	 * immediately cease all link tuning.
	 */
	if (!test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Update statistics.
	 */
	rt2x00dev->ops->lib->link_stats(rt2x00dev, &rt2x00dev->link.qual);
	rt2x00dev->low_level_stats.dot11FCSErrorCount +=
	    rt2x00dev->link.qual.rx_failed;

	/*
	 * Only perform the link tuning when Link tuning
	 * has been enabled (This could have been disabled from the EEPROM).
	 */
	if (!test_bit(CONFIG_DISABLE_LINK_TUNING, &rt2x00dev->flags))
		rt2x00dev->ops->lib->link_tuner(rt2x00dev);

	/*
	 * Precalculate a portion of the link signal which is
	 * in based on the tx/rx success/failure counters.
	 */
	rt2x00lib_precalculate_link_signal(&rt2x00dev->link.qual);

	/*
	 * Send a signal to the led to update the led signal strength.
	 */
	rt2x00leds_led_quality(rt2x00dev, rt2x00dev->link.qual.avg_rssi);

	/*
	 * Evaluate antenna setup, make this the last step since this could
	 * possibly reset some statistics.
	 */
	rt2x00lib_evaluate_antenna(rt2x00dev);

	/*
	 * Increase tuner counter, and reschedule the next link tuner run.
	 */
	rt2x00dev->link.count++;
	queue_delayed_work(rt2x00dev->hw->workqueue,
			   &rt2x00dev->link.work, LINK_TUNE_INTERVAL);
}

static void rt2x00lib_packetfilter_scheduled(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, filter_work);

	rt2x00dev->ops->lib->config_filter(rt2x00dev, rt2x00dev->packet_filter);
}

static void rt2x00lib_intf_scheduled_iter(void *data, u8 *mac,
					  struct ieee80211_vif *vif)
{
	struct rt2x00_dev *rt2x00dev = data;
	struct rt2x00_intf *intf = vif_to_intf(vif);
	struct ieee80211_bss_conf conf;
	int delayed_flags;

	/*
	 * Copy all data we need during this action under the protection
	 * of a spinlock. Otherwise race conditions might occur which results
	 * into an invalid configuration.
	 */
	spin_lock(&intf->lock);

	memcpy(&conf, &vif->bss_conf, sizeof(conf));
	delayed_flags = intf->delayed_flags;
	intf->delayed_flags = 0;

	spin_unlock(&intf->lock);

	/*
	 * It is possible the radio was disabled while the work had been
	 * scheduled. If that happens we should return here immediately,
	 * note that in the spinlock protected area above the delayed_flags
	 * have been cleared correctly.
	 */
	if (!test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	if (delayed_flags & DELAYED_UPDATE_BEACON)
		rt2x00queue_update_beacon(rt2x00dev, vif);

	if (delayed_flags & DELAYED_CONFIG_ERP)
		rt2x00lib_config_erp(rt2x00dev, intf, &conf);

	if (delayed_flags & DELAYED_LED_ASSOC)
		rt2x00leds_led_assoc(rt2x00dev, !!rt2x00dev->intf_associated);
}

static void rt2x00lib_intf_scheduled(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, intf_work);

	/*
	 * Iterate over each interface and perform the
	 * requested configurations.
	 */
	ieee80211_iterate_active_interfaces(rt2x00dev->hw,
					    rt2x00lib_intf_scheduled_iter,
					    rt2x00dev);
}

/*
 * Interrupt context handlers.
 */
static void rt2x00lib_beacondone_iter(void *data, u8 *mac,
				      struct ieee80211_vif *vif)
{
	struct rt2x00_dev *rt2x00dev = data;
	struct rt2x00_intf *intf = vif_to_intf(vif);

	if (vif->type != NL80211_IFTYPE_AP &&
	    vif->type != NL80211_IFTYPE_ADHOC)
		return;

	/*
	 * Clean up the beacon skb.
	 */
	rt2x00queue_free_skb(rt2x00dev, intf->beacon->skb);
	intf->beacon->skb = NULL;

	spin_lock(&intf->lock);
	intf->delayed_flags |= DELAYED_UPDATE_BEACON;
	spin_unlock(&intf->lock);
}

void rt2x00lib_beacondone(struct rt2x00_dev *rt2x00dev)
{
	if (!test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	ieee80211_iterate_active_interfaces_atomic(rt2x00dev->hw,
						   rt2x00lib_beacondone_iter,
						   rt2x00dev);

	schedule_work(&rt2x00dev->intf_work);
}
EXPORT_SYMBOL_GPL(rt2x00lib_beacondone);

void rt2x00lib_txdone(struct queue_entry *entry,
		      struct txdone_entry_desc *txdesc)
{
	struct rt2x00_dev *rt2x00dev = entry->queue->rt2x00dev;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(entry->skb);
	struct skb_frame_desc *skbdesc = get_skb_frame_desc(entry->skb);
	enum data_queue_qid qid = skb_get_queue_mapping(entry->skb);
	u8 rate_idx, rate_flags;

	/*
	 * Unmap the skb.
	 */
	rt2x00queue_unmap_skb(rt2x00dev, entry->skb);

	/*
	 * If the IV/EIV data was stripped from the frame before it was
	 * passed to the hardware, we should now reinsert it again because
	 * mac80211 will expect the the same data to be present it the
	 * frame as it was passed to us.
	 */
	if (test_bit(CONFIG_SUPPORT_HW_CRYPTO, &rt2x00dev->flags))
		rt2x00crypto_tx_insert_iv(entry->skb);

	/*
	 * Send frame to debugfs immediately, after this call is completed
	 * we are going to overwrite the skb->cb array.
	 */
	rt2x00debug_dump_frame(rt2x00dev, DUMP_FRAME_TXDONE, entry->skb);

	/*
	 * Update TX statistics.
	 */
	rt2x00dev->link.qual.tx_success +=
	    test_bit(TXDONE_SUCCESS, &txdesc->flags);
	rt2x00dev->link.qual.tx_failed +=
	    test_bit(TXDONE_FAILURE, &txdesc->flags);

	rate_idx = skbdesc->tx_rate_idx;
	rate_flags = skbdesc->tx_rate_flags;

	/*
	 * Initialize TX status
	 */
	memset(&tx_info->status, 0, sizeof(tx_info->status));
	tx_info->status.ack_signal = 0;
	tx_info->status.rates[0].idx = rate_idx;
	tx_info->status.rates[0].flags = rate_flags;
	tx_info->status.rates[0].count = txdesc->retry + 1;
	tx_info->status.rates[1].idx = -1; /* terminate */

	if (!(tx_info->flags & IEEE80211_TX_CTL_NO_ACK)) {
		if (test_bit(TXDONE_SUCCESS, &txdesc->flags))
			tx_info->flags |= IEEE80211_TX_STAT_ACK;
		else if (test_bit(TXDONE_FAILURE, &txdesc->flags))
			rt2x00dev->low_level_stats.dot11ACKFailureCount++;
	}

	if (rate_flags & IEEE80211_TX_RC_USE_RTS_CTS) {
		if (test_bit(TXDONE_SUCCESS, &txdesc->flags))
			rt2x00dev->low_level_stats.dot11RTSSuccessCount++;
		else if (test_bit(TXDONE_FAILURE, &txdesc->flags))
			rt2x00dev->low_level_stats.dot11RTSFailureCount++;
	}

	/*
	 * Only send the status report to mac80211 when TX status was
	 * requested by it. If this was a extra frame coming through
	 * a mac80211 library call (RTS/CTS) then we should not send the
	 * status report back.
	 */
	if (tx_info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS)
		ieee80211_tx_status_irqsafe(rt2x00dev->hw, entry->skb);
	else
		dev_kfree_skb_irq(entry->skb);

	/*
	 * Make this entry available for reuse.
	 */
	entry->skb = NULL;
	entry->flags = 0;

	rt2x00dev->ops->lib->clear_entry(entry);

	clear_bit(ENTRY_OWNER_DEVICE_DATA, &entry->flags);
	rt2x00queue_index_inc(entry->queue, Q_INDEX_DONE);

	/*
	 * If the data queue was below the threshold before the txdone
	 * handler we must make sure the packet queue in the mac80211 stack
	 * is reenabled when the txdone handler has finished.
	 */
	if (!rt2x00queue_threshold(entry->queue))
		ieee80211_wake_queue(rt2x00dev->hw, qid);
}
EXPORT_SYMBOL_GPL(rt2x00lib_txdone);

void rt2x00lib_rxdone(struct rt2x00_dev *rt2x00dev,
		      struct queue_entry *entry)
{
	struct rxdone_entry_desc rxdesc;
	struct sk_buff *skb;
	struct ieee80211_rx_status *rx_status = &rt2x00dev->rx_status;
	struct ieee80211_supported_band *sband;
	struct ieee80211_hdr *hdr;
	const struct rt2x00_rate *rate;
	unsigned int header_length;
	unsigned int align;
	unsigned int i;
	int idx = -1;

	/*
	 * Allocate a new sk_buffer. If no new buffer available, drop the
	 * received frame and reuse the existing buffer.
	 */
	skb = rt2x00queue_alloc_rxskb(rt2x00dev, entry);
	if (!skb)
		return;

	/*
	 * Unmap the skb.
	 */
	rt2x00queue_unmap_skb(rt2x00dev, entry->skb);

	/*
	 * Extract the RXD details.
	 */
	memset(&rxdesc, 0, sizeof(rxdesc));
	rt2x00dev->ops->lib->fill_rxdone(entry, &rxdesc);

	/*
	 * The data behind the ieee80211 header must be
	 * aligned on a 4 byte boundary.
	 */
	header_length = ieee80211_get_hdrlen_from_skb(entry->skb);
	align = ((unsigned long)(entry->skb->data + header_length)) & 3;

	/*
	 * Hardware might have stripped the IV/EIV/ICV data,
	 * in that case it is possible that the data was
	 * provided seperately (through hardware descriptor)
	 * in which case we should reinsert the data into the frame.
	 */
	if ((rxdesc.dev_flags & RXDONE_CRYPTO_IV) &&
	    (rxdesc.flags & RX_FLAG_IV_STRIPPED)) {
		rt2x00crypto_rx_insert_iv(entry->skb, align,
					  header_length, &rxdesc);
	} else if (align) {
		skb_push(entry->skb, align);
		/* Move entire frame in 1 command */
		memmove(entry->skb->data, entry->skb->data + align,
			rxdesc.size);
	}

	/* Update data pointers, trim buffer to correct size */
	skb_trim(entry->skb, rxdesc.size);

	/*
	 * Update RX statistics.
	 */
	sband = &rt2x00dev->bands[rt2x00dev->curr_band];
	for (i = 0; i < sband->n_bitrates; i++) {
		rate = rt2x00_get_rate(sband->bitrates[i].hw_value);

		if (((rxdesc.dev_flags & RXDONE_SIGNAL_PLCP) &&
		     (rate->plcp == rxdesc.signal)) ||
		    ((rxdesc.dev_flags & RXDONE_SIGNAL_BITRATE) &&
		      (rate->bitrate == rxdesc.signal))) {
			idx = i;
			break;
		}
	}

	if (idx < 0) {
		WARNING(rt2x00dev, "Frame received with unrecognized signal,"
			"signal=0x%.2x, plcp=%d.\n", rxdesc.signal,
			!!(rxdesc.dev_flags & RXDONE_SIGNAL_PLCP));
		idx = 0;
	}

	/*
	 * Only update link status if this is a beacon frame carrying our bssid.
	 */
	hdr = (struct ieee80211_hdr *)entry->skb->data;
	if (ieee80211_is_beacon(hdr->frame_control) &&
	    (rxdesc.dev_flags & RXDONE_MY_BSS))
		rt2x00lib_update_link_stats(&rt2x00dev->link, rxdesc.rssi);

	rt2x00debug_update_crypto(rt2x00dev,
				  rxdesc.cipher,
				  rxdesc.cipher_status);

	rt2x00dev->link.qual.rx_success++;

	rx_status->mactime = rxdesc.timestamp;
	rx_status->rate_idx = idx;
	rx_status->qual =
	    rt2x00lib_calculate_link_signal(rt2x00dev, rxdesc.rssi);
	rx_status->signal = rxdesc.rssi;
	rx_status->flag = rxdesc.flags;
	rx_status->antenna = rt2x00dev->link.ant.active.rx;

	/*
	 * Send frame to mac80211 & debugfs.
	 * mac80211 will clean up the skb structure.
	 */
	rt2x00debug_dump_frame(rt2x00dev, DUMP_FRAME_RXDONE, entry->skb);
	ieee80211_rx_irqsafe(rt2x00dev->hw, entry->skb, rx_status);

	/*
	 * Replace the skb with the freshly allocated one.
	 */
	entry->skb = skb;
	entry->flags = 0;

	rt2x00dev->ops->lib->clear_entry(entry);

	rt2x00queue_index_inc(entry->queue, Q_INDEX);
}
EXPORT_SYMBOL_GPL(rt2x00lib_rxdone);

/*
 * Driver initialization handlers.
 */
const struct rt2x00_rate rt2x00_supported_rates[12] = {
	{
		.flags = DEV_RATE_CCK,
		.bitrate = 10,
		.ratemask = BIT(0),
		.plcp = 0x00,
	},
	{
		.flags = DEV_RATE_CCK | DEV_RATE_SHORT_PREAMBLE,
		.bitrate = 20,
		.ratemask = BIT(1),
		.plcp = 0x01,
	},
	{
		.flags = DEV_RATE_CCK | DEV_RATE_SHORT_PREAMBLE,
		.bitrate = 55,
		.ratemask = BIT(2),
		.plcp = 0x02,
	},
	{
		.flags = DEV_RATE_CCK | DEV_RATE_SHORT_PREAMBLE,
		.bitrate = 110,
		.ratemask = BIT(3),
		.plcp = 0x03,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 60,
		.ratemask = BIT(4),
		.plcp = 0x0b,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 90,
		.ratemask = BIT(5),
		.plcp = 0x0f,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 120,
		.ratemask = BIT(6),
		.plcp = 0x0a,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 180,
		.ratemask = BIT(7),
		.plcp = 0x0e,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 240,
		.ratemask = BIT(8),
		.plcp = 0x09,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 360,
		.ratemask = BIT(9),
		.plcp = 0x0d,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 480,
		.ratemask = BIT(10),
		.plcp = 0x08,
	},
	{
		.flags = DEV_RATE_OFDM,
		.bitrate = 540,
		.ratemask = BIT(11),
		.plcp = 0x0c,
	},
};

static void rt2x00lib_channel(struct ieee80211_channel *entry,
			      const int channel, const int tx_power,
			      const int value)
{
	entry->center_freq = ieee80211_channel_to_frequency(channel);
	entry->hw_value = value;
	entry->max_power = tx_power;
	entry->max_antenna_gain = 0xff;
}

static void rt2x00lib_rate(struct ieee80211_rate *entry,
			   const u16 index, const struct rt2x00_rate *rate)
{
	entry->flags = 0;
	entry->bitrate = rate->bitrate;
	entry->hw_value =index;
	entry->hw_value_short = index;

	if (rate->flags & DEV_RATE_SHORT_PREAMBLE)
		entry->flags |= IEEE80211_RATE_SHORT_PREAMBLE;
}

static int rt2x00lib_probe_hw_modes(struct rt2x00_dev *rt2x00dev,
				    struct hw_mode_spec *spec)
{
	struct ieee80211_hw *hw = rt2x00dev->hw;
	struct ieee80211_channel *channels;
	struct ieee80211_rate *rates;
	unsigned int num_rates;
	unsigned int i;

	num_rates = 0;
	if (spec->supported_rates & SUPPORT_RATE_CCK)
		num_rates += 4;
	if (spec->supported_rates & SUPPORT_RATE_OFDM)
		num_rates += 8;

	channels = kzalloc(sizeof(*channels) * spec->num_channels, GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	rates = kzalloc(sizeof(*rates) * num_rates, GFP_KERNEL);
	if (!rates)
		goto exit_free_channels;

	/*
	 * Initialize Rate list.
	 */
	for (i = 0; i < num_rates; i++)
		rt2x00lib_rate(&rates[i], i, rt2x00_get_rate(i));

	/*
	 * Initialize Channel list.
	 */
	for (i = 0; i < spec->num_channels; i++) {
		rt2x00lib_channel(&channels[i],
				  spec->channels[i].channel,
				  spec->channels_info[i].tx_power1, i);
	}

	/*
	 * Intitialize 802.11b, 802.11g
	 * Rates: CCK, OFDM.
	 * Channels: 2.4 GHz
	 */
	if (spec->supported_bands & SUPPORT_BAND_2GHZ) {
		rt2x00dev->bands[IEEE80211_BAND_2GHZ].n_channels = 14;
		rt2x00dev->bands[IEEE80211_BAND_2GHZ].n_bitrates = num_rates;
		rt2x00dev->bands[IEEE80211_BAND_2GHZ].channels = channels;
		rt2x00dev->bands[IEEE80211_BAND_2GHZ].bitrates = rates;
		hw->wiphy->bands[IEEE80211_BAND_2GHZ] =
		    &rt2x00dev->bands[IEEE80211_BAND_2GHZ];
	}

	/*
	 * Intitialize 802.11a
	 * Rates: OFDM.
	 * Channels: OFDM, UNII, HiperLAN2.
	 */
	if (spec->supported_bands & SUPPORT_BAND_5GHZ) {
		rt2x00dev->bands[IEEE80211_BAND_5GHZ].n_channels =
		    spec->num_channels - 14;
		rt2x00dev->bands[IEEE80211_BAND_5GHZ].n_bitrates =
		    num_rates - 4;
		rt2x00dev->bands[IEEE80211_BAND_5GHZ].channels = &channels[14];
		rt2x00dev->bands[IEEE80211_BAND_5GHZ].bitrates = &rates[4];
		hw->wiphy->bands[IEEE80211_BAND_5GHZ] =
		    &rt2x00dev->bands[IEEE80211_BAND_5GHZ];
	}

	return 0;

 exit_free_channels:
	kfree(channels);
	ERROR(rt2x00dev, "Allocation ieee80211 modes failed.\n");
	return -ENOMEM;
}

static void rt2x00lib_remove_hw(struct rt2x00_dev *rt2x00dev)
{
	if (test_bit(DEVICE_STATE_REGISTERED_HW, &rt2x00dev->flags))
		ieee80211_unregister_hw(rt2x00dev->hw);

	if (likely(rt2x00dev->hw->wiphy->bands[IEEE80211_BAND_2GHZ])) {
		kfree(rt2x00dev->hw->wiphy->bands[IEEE80211_BAND_2GHZ]->channels);
		kfree(rt2x00dev->hw->wiphy->bands[IEEE80211_BAND_2GHZ]->bitrates);
		rt2x00dev->hw->wiphy->bands[IEEE80211_BAND_2GHZ] = NULL;
		rt2x00dev->hw->wiphy->bands[IEEE80211_BAND_5GHZ] = NULL;
	}

	kfree(rt2x00dev->spec.channels_info);
}

static int rt2x00lib_probe_hw(struct rt2x00_dev *rt2x00dev)
{
	struct hw_mode_spec *spec = &rt2x00dev->spec;
	int status;

	if (test_bit(DEVICE_STATE_REGISTERED_HW, &rt2x00dev->flags))
		return 0;

	/*
	 * Initialize HW modes.
	 */
	status = rt2x00lib_probe_hw_modes(rt2x00dev, spec);
	if (status)
		return status;

	/*
	 * Initialize HW fields.
	 */
	rt2x00dev->hw->queues = rt2x00dev->ops->tx_queues;

	/*
	 * Register HW.
	 */
	status = ieee80211_register_hw(rt2x00dev->hw);
	if (status) {
		rt2x00lib_remove_hw(rt2x00dev);
		return status;
	}

	set_bit(DEVICE_STATE_REGISTERED_HW, &rt2x00dev->flags);

	return 0;
}

/*
 * Initialization/uninitialization handlers.
 */
static void rt2x00lib_uninitialize(struct rt2x00_dev *rt2x00dev)
{
	if (!test_and_clear_bit(DEVICE_STATE_INITIALIZED, &rt2x00dev->flags))
		return;

	/*
	 * Unregister extra components.
	 */
	rt2x00rfkill_unregister(rt2x00dev);

	/*
	 * Allow the HW to uninitialize.
	 */
	rt2x00dev->ops->lib->uninitialize(rt2x00dev);

	/*
	 * Free allocated queue entries.
	 */
	rt2x00queue_uninitialize(rt2x00dev);
}

static int rt2x00lib_initialize(struct rt2x00_dev *rt2x00dev)
{
	int status;

	if (test_bit(DEVICE_STATE_INITIALIZED, &rt2x00dev->flags))
		return 0;

	/*
	 * Allocate all queue entries.
	 */
	status = rt2x00queue_initialize(rt2x00dev);
	if (status)
		return status;

	/*
	 * Initialize the device.
	 */
	status = rt2x00dev->ops->lib->initialize(rt2x00dev);
	if (status) {
		rt2x00queue_uninitialize(rt2x00dev);
		return status;
	}

	set_bit(DEVICE_STATE_INITIALIZED, &rt2x00dev->flags);

	/*
	 * Register the extra components.
	 */
	rt2x00rfkill_register(rt2x00dev);

	return 0;
}

int rt2x00lib_start(struct rt2x00_dev *rt2x00dev)
{
	int retval;

	if (test_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags))
		return 0;

	/*
	 * If this is the first interface which is added,
	 * we should load the firmware now.
	 */
	retval = rt2x00lib_load_firmware(rt2x00dev);
	if (retval)
		return retval;

	/*
	 * Initialize the device.
	 */
	retval = rt2x00lib_initialize(rt2x00dev);
	if (retval)
		return retval;

	rt2x00dev->intf_ap_count = 0;
	rt2x00dev->intf_sta_count = 0;
	rt2x00dev->intf_associated = 0;

	set_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags);

	return 0;
}

void rt2x00lib_stop(struct rt2x00_dev *rt2x00dev)
{
	if (!test_and_clear_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags))
		return;

	/*
	 * Perhaps we can add something smarter here,
	 * but for now just disabling the radio should do.
	 */
	rt2x00lib_disable_radio(rt2x00dev);

	rt2x00dev->intf_ap_count = 0;
	rt2x00dev->intf_sta_count = 0;
	rt2x00dev->intf_associated = 0;
}

/*
 * driver allocation handlers.
 */
int rt2x00lib_probe_dev(struct rt2x00_dev *rt2x00dev)
{
	int retval = -ENOMEM;

	mutex_init(&rt2x00dev->csr_mutex);

	/*
	 * Make room for rt2x00_intf inside the per-interface
	 * structure ieee80211_vif.
	 */
	rt2x00dev->hw->vif_data_size = sizeof(struct rt2x00_intf);

	/*
	 * Determine which operating modes are supported, all modes
	 * which require beaconing, depend on the availability of
	 * beacon entries.
	 */
	rt2x00dev->hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	if (rt2x00dev->ops->bcn->entry_num > 0)
		rt2x00dev->hw->wiphy->interface_modes |=
		    BIT(NL80211_IFTYPE_ADHOC) |
		    BIT(NL80211_IFTYPE_AP);

	/*
	 * Let the driver probe the device to detect the capabilities.
	 */
	retval = rt2x00dev->ops->lib->probe_hw(rt2x00dev);
	if (retval) {
		ERROR(rt2x00dev, "Failed to allocate device.\n");
		goto exit;
	}

	/*
	 * Initialize configuration work.
	 */
	INIT_WORK(&rt2x00dev->intf_work, rt2x00lib_intf_scheduled);
	INIT_WORK(&rt2x00dev->filter_work, rt2x00lib_packetfilter_scheduled);
	INIT_DELAYED_WORK(&rt2x00dev->link.work, rt2x00lib_link_tuner);

	/*
	 * Allocate queue array.
	 */
	retval = rt2x00queue_allocate(rt2x00dev);
	if (retval)
		goto exit;

	/*
	 * Initialize ieee80211 structure.
	 */
	retval = rt2x00lib_probe_hw(rt2x00dev);
	if (retval) {
		ERROR(rt2x00dev, "Failed to initialize hw.\n");
		goto exit;
	}

	/*
	 * Register extra components.
	 */
	rt2x00leds_register(rt2x00dev);
	rt2x00debug_register(rt2x00dev);

	set_bit(DEVICE_STATE_PRESENT, &rt2x00dev->flags);

	return 0;

exit:
	rt2x00lib_remove_dev(rt2x00dev);

	return retval;
}
EXPORT_SYMBOL_GPL(rt2x00lib_probe_dev);

void rt2x00lib_remove_dev(struct rt2x00_dev *rt2x00dev)
{
	clear_bit(DEVICE_STATE_PRESENT, &rt2x00dev->flags);

	/*
	 * Disable radio.
	 */
	rt2x00lib_disable_radio(rt2x00dev);

	/*
	 * Uninitialize device.
	 */
	rt2x00lib_uninitialize(rt2x00dev);

	/*
	 * Free extra components
	 */
	rt2x00debug_deregister(rt2x00dev);
	rt2x00leds_unregister(rt2x00dev);

	/*
	 * Free ieee80211_hw memory.
	 */
	rt2x00lib_remove_hw(rt2x00dev);

	/*
	 * Free firmware image.
	 */
	rt2x00lib_free_firmware(rt2x00dev);

	/*
	 * Free queue structures.
	 */
	rt2x00queue_free(rt2x00dev);
}
EXPORT_SYMBOL_GPL(rt2x00lib_remove_dev);

/*
 * Device state handlers
 */
#ifdef CONFIG_PM
int rt2x00lib_suspend(struct rt2x00_dev *rt2x00dev, pm_message_t state)
{
	int retval;

	NOTICE(rt2x00dev, "Going to sleep.\n");

	/*
	 * Only continue if mac80211 has open interfaces.
	 */
	if (!test_and_clear_bit(DEVICE_STATE_PRESENT, &rt2x00dev->flags) ||
	    !test_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags))
		goto exit;

	set_bit(DEVICE_STATE_STARTED_SUSPEND, &rt2x00dev->flags);

	/*
	 * Disable radio.
	 */
	rt2x00lib_stop(rt2x00dev);
	rt2x00lib_uninitialize(rt2x00dev);

	/*
	 * Suspend/disable extra components.
	 */
	rt2x00leds_suspend(rt2x00dev);
	rt2x00debug_deregister(rt2x00dev);

exit:
	/*
	 * Set device mode to sleep for power management,
	 * on some hardware this call seems to consistently fail.
	 * From the specifications it is hard to tell why it fails,
	 * and if this is a "bad thing".
	 * Overall it is safe to just ignore the failure and
	 * continue suspending. The only downside is that the
	 * device will not be in optimal power save mode, but with
	 * the radio and the other components already disabled the
	 * device is as good as disabled.
	 */
	retval = rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_SLEEP);
	if (retval)
		WARNING(rt2x00dev, "Device failed to enter sleep state, "
			"continue suspending.\n");

	return 0;
}
EXPORT_SYMBOL_GPL(rt2x00lib_suspend);

static void rt2x00lib_resume_intf(void *data, u8 *mac,
				  struct ieee80211_vif *vif)
{
	struct rt2x00_dev *rt2x00dev = data;
	struct rt2x00_intf *intf = vif_to_intf(vif);

	spin_lock(&intf->lock);

	rt2x00lib_config_intf(rt2x00dev, intf,
			      vif->type, intf->mac, intf->bssid);


	/*
	 * Master or Ad-hoc mode require a new beacon update.
	 */
	if (vif->type == NL80211_IFTYPE_AP ||
	    vif->type == NL80211_IFTYPE_ADHOC)
		intf->delayed_flags |= DELAYED_UPDATE_BEACON;

	spin_unlock(&intf->lock);
}

int rt2x00lib_resume(struct rt2x00_dev *rt2x00dev)
{
	int retval;

	NOTICE(rt2x00dev, "Waking up.\n");

	/*
	 * Restore/enable extra components.
	 */
	rt2x00debug_register(rt2x00dev);
	rt2x00leds_resume(rt2x00dev);

	/*
	 * Only continue if mac80211 had open interfaces.
	 */
	if (!test_and_clear_bit(DEVICE_STATE_STARTED_SUSPEND, &rt2x00dev->flags))
		return 0;

	/*
	 * Reinitialize device and all active interfaces.
	 */
	retval = rt2x00lib_start(rt2x00dev);
	if (retval)
		goto exit;

	/*
	 * Reconfigure device.
	 */
	retval = rt2x00mac_config(rt2x00dev->hw, ~0);
	if (retval)
		goto exit;

	/*
	 * Iterator over each active interface to
	 * reconfigure the hardware.
	 */
	ieee80211_iterate_active_interfaces(rt2x00dev->hw,
					    rt2x00lib_resume_intf, rt2x00dev);

	/*
	 * We are ready again to receive requests from mac80211.
	 */
	set_bit(DEVICE_STATE_PRESENT, &rt2x00dev->flags);

	/*
	 * It is possible that during that mac80211 has attempted
	 * to send frames while we were suspending or resuming.
	 * In that case we have disabled the TX queue and should
	 * now enable it again
	 */
	ieee80211_wake_queues(rt2x00dev->hw);

	/*
	 * During interface iteration we might have changed the
	 * delayed_flags, time to handles the event by calling
	 * the work handler directly.
	 */
	rt2x00lib_intf_scheduled(&rt2x00dev->intf_work);

	return 0;

exit:
	rt2x00lib_stop(rt2x00dev);
	rt2x00lib_uninitialize(rt2x00dev);
	rt2x00debug_deregister(rt2x00dev);

	return retval;
}
EXPORT_SYMBOL_GPL(rt2x00lib_resume);
#endif /* CONFIG_PM */

/*
 * rt2x00lib module information.
 */
MODULE_AUTHOR(DRV_PROJECT);
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("rt2x00 library");
MODULE_LICENSE("GPL");
