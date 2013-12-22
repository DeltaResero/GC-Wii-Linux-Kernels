/*
 * Copyright 2003-2005	Devicescape Software, Inc.
 * Copyright (c) 2006	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007	Johannes Berg <johannes@sipsolutions.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/debugfs.h>
#include <linux/ieee80211.h>
#include "ieee80211_i.h"
#include "debugfs.h"
#include "debugfs_sta.h"
#include "sta_info.h"

/* sta attributtes */

#define STA_READ(name, buflen, field, format_string)			\
static ssize_t sta_ ##name## _read(struct file *file,			\
				   char __user *userbuf,		\
				   size_t count, loff_t *ppos)		\
{									\
	int res;							\
	struct sta_info *sta = file->private_data;			\
	char buf[buflen];						\
	res = scnprintf(buf, buflen, format_string, sta->field);	\
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);	\
}
#define STA_READ_D(name, field) STA_READ(name, 20, field, "%d\n")
#define STA_READ_U(name, field) STA_READ(name, 20, field, "%u\n")
#define STA_READ_LU(name, field) STA_READ(name, 20, field, "%lu\n")
#define STA_READ_S(name, field) STA_READ(name, 20, field, "%s\n")

#define STA_OPS(name)							\
static const struct file_operations sta_ ##name## _ops = {		\
	.read = sta_##name##_read,					\
	.open = mac80211_open_file_generic,				\
}

#define STA_OPS_WR(name)						\
static const struct file_operations sta_ ##name## _ops = {		\
	.read = sta_##name##_read,					\
	.write = sta_##name##_write,					\
	.open = mac80211_open_file_generic,				\
}

#define STA_FILE(name, field, format)					\
		STA_READ_##format(name, field)				\
		STA_OPS(name)

STA_FILE(aid, aid, D);
STA_FILE(dev, sdata->dev->name, S);
STA_FILE(rx_packets, rx_packets, LU);
STA_FILE(tx_packets, tx_packets, LU);
STA_FILE(rx_bytes, rx_bytes, LU);
STA_FILE(tx_bytes, tx_bytes, LU);
STA_FILE(rx_duplicates, num_duplicates, LU);
STA_FILE(rx_fragments, rx_fragments, LU);
STA_FILE(rx_dropped, rx_dropped, LU);
STA_FILE(tx_fragments, tx_fragments, LU);
STA_FILE(tx_filtered, tx_filtered_count, LU);
STA_FILE(tx_retry_failed, tx_retry_failed, LU);
STA_FILE(tx_retry_count, tx_retry_count, LU);
STA_FILE(last_signal, last_signal, D);
STA_FILE(last_qual, last_qual, D);
STA_FILE(last_noise, last_noise, D);
STA_FILE(wep_weak_iv_count, wep_weak_iv_count, LU);

static ssize_t sta_flags_read(struct file *file, char __user *userbuf,
			      size_t count, loff_t *ppos)
{
	char buf[100];
	struct sta_info *sta = file->private_data;
	u32 staflags = get_sta_flags(sta);
	int res = scnprintf(buf, sizeof(buf), "%s%s%s%s%s%s%s",
		staflags & WLAN_STA_AUTH ? "AUTH\n" : "",
		staflags & WLAN_STA_ASSOC ? "ASSOC\n" : "",
		staflags & WLAN_STA_PS ? "PS\n" : "",
		staflags & WLAN_STA_AUTHORIZED ? "AUTHORIZED\n" : "",
		staflags & WLAN_STA_SHORT_PREAMBLE ? "SHORT PREAMBLE\n" : "",
		staflags & WLAN_STA_WME ? "WME\n" : "",
		staflags & WLAN_STA_WDS ? "WDS\n" : "");
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);
}
STA_OPS(flags);

static ssize_t sta_num_ps_buf_frames_read(struct file *file,
					  char __user *userbuf,
					  size_t count, loff_t *ppos)
{
	char buf[20];
	struct sta_info *sta = file->private_data;
	int res = scnprintf(buf, sizeof(buf), "%u\n",
			    skb_queue_len(&sta->ps_tx_buf));
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);
}
STA_OPS(num_ps_buf_frames);

static ssize_t sta_inactive_ms_read(struct file *file, char __user *userbuf,
				    size_t count, loff_t *ppos)
{
	char buf[20];
	struct sta_info *sta = file->private_data;
	int res = scnprintf(buf, sizeof(buf), "%d\n",
			    jiffies_to_msecs(jiffies - sta->last_rx));
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);
}
STA_OPS(inactive_ms);

static ssize_t sta_last_seq_ctrl_read(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	char buf[15*NUM_RX_DATA_QUEUES], *p = buf;
	int i;
	struct sta_info *sta = file->private_data;
	for (i = 0; i < NUM_RX_DATA_QUEUES; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%x ",
			       le16_to_cpu(sta->last_seq_ctrl[i]));
	p += scnprintf(p, sizeof(buf)+buf-p, "\n");
	return simple_read_from_buffer(userbuf, count, ppos, buf, p - buf);
}
STA_OPS(last_seq_ctrl);

static ssize_t sta_agg_status_read(struct file *file, char __user *userbuf,
					size_t count, loff_t *ppos)
{
	char buf[768], *p = buf;
	int i;
	struct sta_info *sta = file->private_data;
	p += scnprintf(p, sizeof(buf)+buf-p, "Agg state for STA is:\n");
	p += scnprintf(p, sizeof(buf)+buf-p, " STA next dialog_token is %d \n "
			"TIDs info is: \n TID :",
			(sta->ampdu_mlme.dialog_token_allocator + 1));
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d", i);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n RX  :");
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d",
			sta->ampdu_mlme.tid_state_rx[i]);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n DTKN:");
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d",
			sta->ampdu_mlme.tid_state_rx[i]?
			sta->ampdu_mlme.tid_rx[i]->dialog_token : 0);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n TX  :");
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d",
			sta->ampdu_mlme.tid_state_tx[i]);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n DTKN:");
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d",
			sta->ampdu_mlme.tid_state_tx[i]?
			sta->ampdu_mlme.tid_tx[i]->dialog_token : 0);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n SSN :");
	for (i = 0; i < STA_TID_NUM; i++)
		p += scnprintf(p, sizeof(buf)+buf-p, "%5d",
			sta->ampdu_mlme.tid_state_tx[i]?
			sta->ampdu_mlme.tid_tx[i]->ssn : 0);

	p += scnprintf(p, sizeof(buf)+buf-p, "\n");

	return simple_read_from_buffer(userbuf, count, ppos, buf, p - buf);
}

static ssize_t sta_agg_status_write(struct file *file,
		const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct sta_info *sta = file->private_data;
	struct net_device *dev = sta->sdata->dev;
	struct ieee80211_local *local = wdev_priv(dev->ieee80211_ptr);
	struct ieee80211_hw *hw = &local->hw;
	u8 *da = sta->addr;
	static int tid_static_tx[16] = {0, 0, 0, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0, 0};
	static int tid_static_rx[16] = {1, 1, 1, 1, 1, 1, 1, 1,
					1, 1, 1, 1, 1, 1, 1, 1};
	char *endp;
	char buf[32];
	int buf_size, rs;
	unsigned int tid_num;
	char state[4];

	memset(buf, 0x00, sizeof(buf));
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	tid_num = simple_strtoul(buf, &endp, 0);
	if (endp == buf)
		return -EINVAL;

	if ((tid_num >= 100) && (tid_num <= 115)) {
		/* toggle Rx aggregation command */
		tid_num = tid_num - 100;
		if (tid_static_rx[tid_num] == 1) {
			strcpy(state, "off");
			ieee80211_sta_stop_rx_ba_session(dev, da, tid_num, 0,
					WLAN_REASON_QSTA_REQUIRE_SETUP);
			sta->ampdu_mlme.tid_state_rx[tid_num] |=
					HT_AGG_STATE_DEBUGFS_CTL;
			tid_static_rx[tid_num] = 0;
		} else {
			strcpy(state, "on ");
			sta->ampdu_mlme.tid_state_rx[tid_num] &=
					~HT_AGG_STATE_DEBUGFS_CTL;
			tid_static_rx[tid_num] = 1;
		}
		printk(KERN_DEBUG "debugfs - try switching tid %u %s\n",
				tid_num, state);
	} else if ((tid_num >= 0) && (tid_num <= 15)) {
		/* toggle Tx aggregation command */
		if (tid_static_tx[tid_num] == 0) {
			strcpy(state, "on ");
			rs =  ieee80211_start_tx_ba_session(hw, da, tid_num);
			if (rs == 0)
				tid_static_tx[tid_num] = 1;
		} else {
			strcpy(state, "off");
			rs =  ieee80211_stop_tx_ba_session(hw, da, tid_num, 1);
			if (rs == 0)
				tid_static_tx[tid_num] = 0;
		}
		printk(KERN_DEBUG "debugfs - switching tid %u %s, return=%d\n",
				tid_num, state, rs);
	}

	return count;
}
STA_OPS_WR(agg_status);

#define DEBUGFS_ADD(name) \
	sta->debugfs.name = debugfs_create_file(#name, 0400, \
		sta->debugfs.dir, sta, &sta_ ##name## _ops);

#define DEBUGFS_DEL(name) \
	debugfs_remove(sta->debugfs.name);\
	sta->debugfs.name = NULL;


void ieee80211_sta_debugfs_add(struct sta_info *sta)
{
	struct dentry *stations_dir = sta->local->debugfs.stations;
	DECLARE_MAC_BUF(mbuf);
	u8 *mac;

	if (!stations_dir)
		return;

	mac = print_mac(mbuf, sta->addr);

	sta->debugfs.dir = debugfs_create_dir(mac, stations_dir);
	if (!sta->debugfs.dir)
		return;

	DEBUGFS_ADD(flags);
	DEBUGFS_ADD(num_ps_buf_frames);
	DEBUGFS_ADD(inactive_ms);
	DEBUGFS_ADD(last_seq_ctrl);
	DEBUGFS_ADD(agg_status);
}

void ieee80211_sta_debugfs_remove(struct sta_info *sta)
{
	DEBUGFS_DEL(flags);
	DEBUGFS_DEL(num_ps_buf_frames);
	DEBUGFS_DEL(inactive_ms);
	DEBUGFS_DEL(last_seq_ctrl);
	DEBUGFS_DEL(agg_status);

	debugfs_remove(sta->debugfs.dir);
	sta->debugfs.dir = NULL;
}
