/*
 * Copyright 2002-2005, Instant802 Networks, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/timer.h>
#include <linux/rtnetlink.h>

#include <net/mac80211.h>
#include "ieee80211_i.h"
#include "driver-ops.h"
#include "rate.h"
#include "sta_info.h"
#include "debugfs_sta.h"
#include "mesh.h"

/**
 * DOC: STA information lifetime rules
 *
 * STA info structures (&struct sta_info) are managed in a hash table
 * for faster lookup and a list for iteration. They are managed using
 * RCU, i.e. access to the list and hash table is protected by RCU.
 *
 * Upon allocating a STA info structure with sta_info_alloc(), the caller owns
 * that structure. It must then either destroy it using sta_info_destroy()
 * (which is pretty useless) or insert it into the hash table using
 * sta_info_insert() which demotes the reference from ownership to a regular
 * RCU-protected reference; if the function is called without protection by an
 * RCU critical section the reference is instantly invalidated. Note that the
 * caller may not do much with the STA info before inserting it, in particular,
 * it may not start any mesh peer link management or add encryption keys.
 *
 * When the insertion fails (sta_info_insert()) returns non-zero), the
 * structure will have been freed by sta_info_insert()!
 *
 * sta entries are added by mac80211 when you establish a link with a
 * peer. This means different things for the different type of interfaces
 * we support. For a regular station this mean we add the AP sta when we
 * receive an assocation response from the AP. For IBSS this occurs when
 * we receive a probe response or a beacon from target IBSS network. For
 * WDS we add the sta for the peer imediately upon device open. When using
 * AP mode we add stations for each respective station upon request from
 * userspace through nl80211.
 *
 * Because there are debugfs entries for each station, and adding those
 * must be able to sleep, it is also possible to "pin" a station entry,
 * that means it can be removed from the hash table but not be freed.
 * See the comment in __sta_info_unlink() for more information, this is
 * an internal capability only.
 *
 * In order to remove a STA info structure, the caller needs to first
 * unlink it (sta_info_unlink()) from the list and hash tables and
 * then destroy it; sta_info_destroy() will wait for an RCU grace period
 * to elapse before actually freeing it. Due to the pinning and the
 * possibility of multiple callers trying to remove the same STA info at
 * the same time, sta_info_unlink() can clear the STA info pointer it is
 * passed to indicate that the STA info is owned by somebody else now.
 *
 * If sta_info_unlink() did not clear the pointer then the caller owns
 * the STA info structure now and is responsible of destroying it with
 * a call to sta_info_destroy().
 *
 * In all other cases, there is no concept of ownership on a STA entry,
 * each structure is owned by the global hash table/list until it is
 * removed. All users of the structure need to be RCU protected so that
 * the structure won't be freed before they are done using it.
 */

/* Caller must hold local->sta_lock */
static int sta_info_hash_del(struct ieee80211_local *local,
			     struct sta_info *sta)
{
	struct sta_info *s;

	s = local->sta_hash[STA_HASH(sta->sta.addr)];
	if (!s)
		return -ENOENT;
	if (s == sta) {
		rcu_assign_pointer(local->sta_hash[STA_HASH(sta->sta.addr)],
				   s->hnext);
		return 0;
	}

	while (s->hnext && s->hnext != sta)
		s = s->hnext;
	if (s->hnext) {
		rcu_assign_pointer(s->hnext, sta->hnext);
		return 0;
	}

	return -ENOENT;
}

/* protected by RCU */
struct sta_info *sta_info_get(struct ieee80211_local *local, const u8 *addr)
{
	struct sta_info *sta;

	sta = rcu_dereference(local->sta_hash[STA_HASH(addr)]);
	while (sta) {
		if (memcmp(sta->sta.addr, addr, ETH_ALEN) == 0)
			break;
		sta = rcu_dereference(sta->hnext);
	}
	return sta;
}

struct sta_info *sta_info_get_by_idx(struct ieee80211_local *local, int idx,
				     struct net_device *dev)
{
	struct sta_info *sta;
	int i = 0;

	list_for_each_entry_rcu(sta, &local->sta_list, list) {
		if (dev && dev != sta->sdata->dev)
			continue;
		if (i < idx) {
			++i;
			continue;
		}
		return sta;
	}

	return NULL;
}

/**
 * __sta_info_free - internal STA free helper
 *
 * @local: pointer to the global information
 * @sta: STA info to free
 *
 * This function must undo everything done by sta_info_alloc()
 * that may happen before sta_info_insert().
 */
static void __sta_info_free(struct ieee80211_local *local,
			    struct sta_info *sta)
{
	rate_control_free_sta(sta);
	rate_control_put(sta->rate_ctrl);

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Destroyed STA %pM\n",
	       wiphy_name(local->hw.wiphy), sta->sta.addr);
#endif /* CONFIG_MAC80211_VERBOSE_DEBUG */

	kfree(sta);
}

void sta_info_destroy(struct sta_info *sta)
{
	struct ieee80211_local *local;
	struct sk_buff *skb;
	int i;

	might_sleep();

	if (!sta)
		return;

	local = sta->local;

	rate_control_remove_sta_debugfs(sta);
	ieee80211_sta_debugfs_remove(sta);

#ifdef CONFIG_MAC80211_MESH
	if (ieee80211_vif_is_mesh(&sta->sdata->vif))
		mesh_plink_deactivate(sta);
#endif

	/*
	 * We have only unlinked the key, and actually destroying it
	 * may mean it is removed from hardware which requires that
	 * the key->sta pointer is still valid, so flush the key todo
	 * list here.
	 *
	 * ieee80211_key_todo() will synchronize_rcu() so after this
	 * nothing can reference this sta struct any more.
	 */
	ieee80211_key_todo();

#ifdef CONFIG_MAC80211_MESH
	if (ieee80211_vif_is_mesh(&sta->sdata->vif))
		del_timer_sync(&sta->plink_timer);
#endif

	while ((skb = skb_dequeue(&sta->ps_tx_buf)) != NULL) {
		local->total_ps_buffered--;
		dev_kfree_skb_any(skb);
	}

	while ((skb = skb_dequeue(&sta->tx_filtered)) != NULL)
		dev_kfree_skb_any(skb);

	for (i = 0; i <  STA_TID_NUM; i++) {
		struct tid_ampdu_rx *tid_rx;
		struct tid_ampdu_tx *tid_tx;

		spin_lock_bh(&sta->lock);
		tid_rx = sta->ampdu_mlme.tid_rx[i];
		/* Make sure timer won't free the tid_rx struct, see below */
		if (tid_rx)
			tid_rx->shutdown = true;

		spin_unlock_bh(&sta->lock);

		/*
		 * Outside spinlock - shutdown is true now so that the timer
		 * won't free tid_rx, we have to do that now. Can't let the
		 * timer do it because we have to sync the timer outside the
		 * lock that it takes itself.
		 */
		if (tid_rx) {
			del_timer_sync(&tid_rx->session_timer);
			kfree(tid_rx);
		}

		/*
		 * No need to do such complications for TX agg sessions, the
		 * path leading to freeing the tid_tx struct goes via a call
		 * from the driver, and thus needs to look up the sta struct
		 * again, which cannot be found when we get here. Hence, we
		 * just need to delete the timer and free the aggregation
		 * info; we won't be telling the peer about it then but that
		 * doesn't matter if we're not talking to it again anyway.
		 */
		tid_tx = sta->ampdu_mlme.tid_tx[i];
		if (tid_tx) {
			del_timer_sync(&tid_tx->addba_resp_timer);
			/*
			 * STA removed while aggregation session being
			 * started? Bit odd, but purge frames anyway.
			 */
			skb_queue_purge(&tid_tx->pending);
			kfree(tid_tx);
		}
	}

	__sta_info_free(local, sta);
}


/* Caller must hold local->sta_lock */
static void sta_info_hash_add(struct ieee80211_local *local,
			      struct sta_info *sta)
{
	sta->hnext = local->sta_hash[STA_HASH(sta->sta.addr)];
	rcu_assign_pointer(local->sta_hash[STA_HASH(sta->sta.addr)], sta);
}

struct sta_info *sta_info_alloc(struct ieee80211_sub_if_data *sdata,
				u8 *addr, gfp_t gfp)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	int i;

	sta = kzalloc(sizeof(*sta) + local->hw.sta_data_size, gfp);
	if (!sta)
		return NULL;

	spin_lock_init(&sta->lock);
	spin_lock_init(&sta->flaglock);

	memcpy(sta->sta.addr, addr, ETH_ALEN);
	sta->local = local;
	sta->sdata = sdata;
	sta->last_rx = jiffies;

	sta->rate_ctrl = rate_control_get(local->rate_ctrl);
	sta->rate_ctrl_priv = rate_control_alloc_sta(sta->rate_ctrl,
						     &sta->sta, gfp);
	if (!sta->rate_ctrl_priv) {
		rate_control_put(sta->rate_ctrl);
		kfree(sta);
		return NULL;
	}

	for (i = 0; i < STA_TID_NUM; i++) {
		/* timer_to_tid must be initialized with identity mapping to
		 * enable session_timer's data differentiation. refer to
		 * sta_rx_agg_session_timer_expired for useage */
		sta->timer_to_tid[i] = i;
		/* rx */
		sta->ampdu_mlme.tid_state_rx[i] = HT_AGG_STATE_IDLE;
		sta->ampdu_mlme.tid_rx[i] = NULL;
		/* tx */
		sta->ampdu_mlme.tid_state_tx[i] = HT_AGG_STATE_IDLE;
		sta->ampdu_mlme.tid_tx[i] = NULL;
		sta->ampdu_mlme.addba_req_num[i] = 0;
	}
	skb_queue_head_init(&sta->ps_tx_buf);
	skb_queue_head_init(&sta->tx_filtered);

	for (i = 0; i < NUM_RX_DATA_QUEUES; i++)
		sta->last_seq_ctrl[i] = cpu_to_le16(USHORT_MAX);

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Allocated STA %pM\n",
	       wiphy_name(local->hw.wiphy), sta->sta.addr);
#endif /* CONFIG_MAC80211_VERBOSE_DEBUG */

#ifdef CONFIG_MAC80211_MESH
	sta->plink_state = PLINK_LISTEN;
	init_timer(&sta->plink_timer);
#endif

	return sta;
}

int sta_info_insert(struct sta_info *sta)
{
	struct ieee80211_local *local = sta->local;
	struct ieee80211_sub_if_data *sdata = sta->sdata;
	unsigned long flags;
	int err = 0;

	/*
	 * Can't be a WARN_ON because it can be triggered through a race:
	 * something inserts a STA (on one CPU) without holding the RTNL
	 * and another CPU turns off the net device.
	 */
	if (unlikely(!netif_running(sdata->dev))) {
		err = -ENETDOWN;
		goto out_free;
	}

	if (WARN_ON(compare_ether_addr(sta->sta.addr, sdata->dev->dev_addr) == 0 ||
		    is_multicast_ether_addr(sta->sta.addr))) {
		err = -EINVAL;
		goto out_free;
	}

	spin_lock_irqsave(&local->sta_lock, flags);
	/* check if STA exists already */
	if (sta_info_get(local, sta->sta.addr)) {
		spin_unlock_irqrestore(&local->sta_lock, flags);
		err = -EEXIST;
		goto out_free;
	}
	list_add(&sta->list, &local->sta_list);
	local->sta_generation++;
	local->num_sta++;
	sta_info_hash_add(local, sta);

	/* notify driver */
	if (local->ops->sta_notify) {
		if (sdata->vif.type == NL80211_IFTYPE_AP_VLAN)
			sdata = container_of(sdata->bss,
					     struct ieee80211_sub_if_data,
					     u.ap);

		drv_sta_notify(local, &sdata->vif, STA_NOTIFY_ADD, &sta->sta);
		sdata = sta->sdata;
	}

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Inserted STA %pM\n",
	       wiphy_name(local->hw.wiphy), sta->sta.addr);
#endif /* CONFIG_MAC80211_VERBOSE_DEBUG */

	spin_unlock_irqrestore(&local->sta_lock, flags);

#ifdef CONFIG_MAC80211_DEBUGFS
	/*
	 * Debugfs entry adding might sleep, so schedule process
	 * context task for adding entry for STAs that do not yet
	 * have one.
	 * NOTE: due to auto-freeing semantics this may only be done
	 *       if the insertion is successful!
	 */
	schedule_work(&local->sta_debugfs_add);
#endif

	if (ieee80211_vif_is_mesh(&sdata->vif))
		mesh_accept_plinks_update(sdata);

	return 0;
 out_free:
	BUG_ON(!err);
	__sta_info_free(local, sta);
	return err;
}

static inline void __bss_tim_set(struct ieee80211_if_ap *bss, u16 aid)
{
	/*
	 * This format has been mandated by the IEEE specifications,
	 * so this line may not be changed to use the __set_bit() format.
	 */
	bss->tim[aid / 8] |= (1 << (aid % 8));
}

static inline void __bss_tim_clear(struct ieee80211_if_ap *bss, u16 aid)
{
	/*
	 * This format has been mandated by the IEEE specifications,
	 * so this line may not be changed to use the __clear_bit() format.
	 */
	bss->tim[aid / 8] &= ~(1 << (aid % 8));
}

static void __sta_info_set_tim_bit(struct ieee80211_if_ap *bss,
				   struct sta_info *sta)
{
	BUG_ON(!bss);

	__bss_tim_set(bss, sta->sta.aid);

	if (sta->local->ops->set_tim) {
		sta->local->tim_in_locked_section = true;
		drv_set_tim(sta->local, &sta->sta, true);
		sta->local->tim_in_locked_section = false;
	}
}

void sta_info_set_tim_bit(struct sta_info *sta)
{
	unsigned long flags;

	BUG_ON(!sta->sdata->bss);

	spin_lock_irqsave(&sta->local->sta_lock, flags);
	__sta_info_set_tim_bit(sta->sdata->bss, sta);
	spin_unlock_irqrestore(&sta->local->sta_lock, flags);
}

static void __sta_info_clear_tim_bit(struct ieee80211_if_ap *bss,
				     struct sta_info *sta)
{
	BUG_ON(!bss);

	__bss_tim_clear(bss, sta->sta.aid);

	if (sta->local->ops->set_tim) {
		sta->local->tim_in_locked_section = true;
		drv_set_tim(sta->local, &sta->sta, false);
		sta->local->tim_in_locked_section = false;
	}
}

void sta_info_clear_tim_bit(struct sta_info *sta)
{
	unsigned long flags;

	BUG_ON(!sta->sdata->bss);

	spin_lock_irqsave(&sta->local->sta_lock, flags);
	__sta_info_clear_tim_bit(sta->sdata->bss, sta);
	spin_unlock_irqrestore(&sta->local->sta_lock, flags);
}

static void __sta_info_unlink(struct sta_info **sta)
{
	struct ieee80211_local *local = (*sta)->local;
	struct ieee80211_sub_if_data *sdata = (*sta)->sdata;
	/*
	 * pull caller's reference if we're already gone.
	 */
	if (sta_info_hash_del(local, *sta)) {
		*sta = NULL;
		return;
	}

	if ((*sta)->key) {
		ieee80211_key_free((*sta)->key);
		WARN_ON((*sta)->key);
	}

	list_del(&(*sta)->list);

	if (test_and_clear_sta_flags(*sta, WLAN_STA_PS)) {
		BUG_ON(!sdata->bss);

		atomic_dec(&sdata->bss->num_sta_ps);
		__sta_info_clear_tim_bit(sdata->bss, *sta);
	}

	local->num_sta--;
	local->sta_generation++;

	if (local->ops->sta_notify) {
		if (sdata->vif.type == NL80211_IFTYPE_AP_VLAN)
			sdata = container_of(sdata->bss,
					     struct ieee80211_sub_if_data,
					     u.ap);

		drv_sta_notify(local, &sdata->vif, STA_NOTIFY_REMOVE,
			       &(*sta)->sta);
		sdata = (*sta)->sdata;
	}

	if (ieee80211_vif_is_mesh(&sdata->vif)) {
		mesh_accept_plinks_update(sdata);
#ifdef CONFIG_MAC80211_MESH
		del_timer(&(*sta)->plink_timer);
#endif
	}

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Removed STA %pM\n",
	       wiphy_name(local->hw.wiphy), (*sta)->sta.addr);
#endif /* CONFIG_MAC80211_VERBOSE_DEBUG */

	/*
	 * Finally, pull caller's reference if the STA is pinned by the
	 * task that is adding the debugfs entries. In that case, we
	 * leave the STA "to be freed".
	 *
	 * The rules are not trivial, but not too complex either:
	 *  (1) pin_status is only modified under the sta_lock
	 *  (2) STAs may only be pinned under the RTNL so that
	 *	sta_info_flush() is guaranteed to actually destroy
	 *	all STAs that are active for a given interface, this
	 *	is required for correctness because otherwise we
	 *	could notify a driver that an interface is going
	 *	away and only after that (!) notify it about a STA
	 *	on that interface going away.
	 *  (3) sta_info_debugfs_add_work() will set the status
	 *	to PINNED when it found an item that needs a new
	 *	debugfs directory created. In that case, that item
	 *	must not be freed although all *RCU* users are done
	 *	with it. Hence, we tell the caller of _unlink()
	 *	that the item is already gone (as can happen when
	 *	two tasks try to unlink/destroy at the same time)
	 *  (4) We set the pin_status to DESTROY here when we
	 *	find such an item.
	 *  (5) sta_info_debugfs_add_work() will reset the pin_status
	 *	from PINNED to NORMAL when it is done with the item,
	 *	but will check for DESTROY before resetting it in
	 *	which case it will free the item.
	 */
	if ((*sta)->pin_status == STA_INFO_PIN_STAT_PINNED) {
		(*sta)->pin_status = STA_INFO_PIN_STAT_DESTROY;
		*sta = NULL;
		return;
	}
}

void sta_info_unlink(struct sta_info **sta)
{
	struct ieee80211_local *local = (*sta)->local;
	unsigned long flags;

	spin_lock_irqsave(&local->sta_lock, flags);
	__sta_info_unlink(sta);
	spin_unlock_irqrestore(&local->sta_lock, flags);
}

static int sta_info_buffer_expired(struct sta_info *sta,
				   struct sk_buff *skb)
{
	struct ieee80211_tx_info *info;
	int timeout;

	if (!skb)
		return 0;

	info = IEEE80211_SKB_CB(skb);

	/* Timeout: (2 * listen_interval * beacon_int * 1024 / 1000000) sec */
	timeout = (sta->listen_interval *
		   sta->sdata->vif.bss_conf.beacon_int *
		   32 / 15625) * HZ;
	if (timeout < STA_TX_BUFFER_EXPIRE)
		timeout = STA_TX_BUFFER_EXPIRE;
	return time_after(jiffies, info->control.jiffies + timeout);
}


static void sta_info_cleanup_expire_buffered(struct ieee80211_local *local,
					     struct sta_info *sta)
{
	unsigned long flags;
	struct sk_buff *skb;
	struct ieee80211_sub_if_data *sdata;

	if (skb_queue_empty(&sta->ps_tx_buf))
		return;

	for (;;) {
		spin_lock_irqsave(&sta->ps_tx_buf.lock, flags);
		skb = skb_peek(&sta->ps_tx_buf);
		if (sta_info_buffer_expired(sta, skb))
			skb = __skb_dequeue(&sta->ps_tx_buf);
		else
			skb = NULL;
		spin_unlock_irqrestore(&sta->ps_tx_buf.lock, flags);

		if (!skb)
			break;

		sdata = sta->sdata;
		local->total_ps_buffered--;
#ifdef CONFIG_MAC80211_VERBOSE_PS_DEBUG
		printk(KERN_DEBUG "Buffered frame expired (STA %pM)\n",
		       sta->sta.addr);
#endif
		dev_kfree_skb(skb);

		if (skb_queue_empty(&sta->ps_tx_buf))
			sta_info_clear_tim_bit(sta);
	}
}


static void sta_info_cleanup(unsigned long data)
{
	struct ieee80211_local *local = (struct ieee80211_local *) data;
	struct sta_info *sta;

	rcu_read_lock();
	list_for_each_entry_rcu(sta, &local->sta_list, list)
		sta_info_cleanup_expire_buffered(local, sta);
	rcu_read_unlock();

	if (local->quiescing)
		return;

	local->sta_cleanup.expires =
		round_jiffies(jiffies + STA_INFO_CLEANUP_INTERVAL);
	add_timer(&local->sta_cleanup);
}

#ifdef CONFIG_MAC80211_DEBUGFS
/*
 * See comment in __sta_info_unlink,
 * caller must hold local->sta_lock.
 */
static void __sta_info_pin(struct sta_info *sta)
{
	WARN_ON(sta->pin_status != STA_INFO_PIN_STAT_NORMAL);
	sta->pin_status = STA_INFO_PIN_STAT_PINNED;
}

/*
 * See comment in __sta_info_unlink, returns sta if it
 * needs to be destroyed.
 */
static struct sta_info *__sta_info_unpin(struct sta_info *sta)
{
	struct sta_info *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sta->local->sta_lock, flags);
	WARN_ON(sta->pin_status != STA_INFO_PIN_STAT_DESTROY &&
		sta->pin_status != STA_INFO_PIN_STAT_PINNED);
	if (sta->pin_status == STA_INFO_PIN_STAT_DESTROY)
		ret = sta;
	sta->pin_status = STA_INFO_PIN_STAT_NORMAL;
	spin_unlock_irqrestore(&sta->local->sta_lock, flags);

	return ret;
}

static void sta_info_debugfs_add_work(struct work_struct *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local, sta_debugfs_add);
	struct sta_info *sta, *tmp;
	unsigned long flags;

	/* We need to keep the RTNL across the whole pinned status. */
	rtnl_lock();
	while (1) {
		sta = NULL;

		spin_lock_irqsave(&local->sta_lock, flags);
		list_for_each_entry(tmp, &local->sta_list, list) {
			/*
			 * debugfs.add_has_run will be set by
			 * ieee80211_sta_debugfs_add regardless
			 * of what else it does.
			 */
			if (!tmp->debugfs.add_has_run) {
				sta = tmp;
				__sta_info_pin(sta);
				break;
			}
		}
		spin_unlock_irqrestore(&local->sta_lock, flags);

		if (!sta)
			break;

		ieee80211_sta_debugfs_add(sta);
		rate_control_add_sta_debugfs(sta);

		sta = __sta_info_unpin(sta);
		sta_info_destroy(sta);
	}
	rtnl_unlock();
}
#endif

void sta_info_init(struct ieee80211_local *local)
{
	spin_lock_init(&local->sta_lock);
	INIT_LIST_HEAD(&local->sta_list);

	setup_timer(&local->sta_cleanup, sta_info_cleanup,
		    (unsigned long)local);
	local->sta_cleanup.expires =
		round_jiffies(jiffies + STA_INFO_CLEANUP_INTERVAL);

#ifdef CONFIG_MAC80211_DEBUGFS
	INIT_WORK(&local->sta_debugfs_add, sta_info_debugfs_add_work);
#endif
}

int sta_info_start(struct ieee80211_local *local)
{
	add_timer(&local->sta_cleanup);
	return 0;
}

void sta_info_stop(struct ieee80211_local *local)
{
	del_timer(&local->sta_cleanup);
#ifdef CONFIG_MAC80211_DEBUGFS
	/*
	 * Make sure the debugfs adding work isn't pending after this
	 * because we're about to be destroyed. It doesn't matter
	 * whether it ran or not since we're going to flush all STAs
	 * anyway.
	 */
	cancel_work_sync(&local->sta_debugfs_add);
#endif

	sta_info_flush(local, NULL);
}

/**
 * sta_info_flush - flush matching STA entries from the STA table
 *
 * Returns the number of removed STA entries.
 *
 * @local: local interface data
 * @sdata: matching rule for the net device (sta->dev) or %NULL to match all STAs
 */
int sta_info_flush(struct ieee80211_local *local,
		   struct ieee80211_sub_if_data *sdata)
{
	struct sta_info *sta, *tmp;
	LIST_HEAD(tmp_list);
	int ret = 0;
	unsigned long flags;

	might_sleep();

	spin_lock_irqsave(&local->sta_lock, flags);
	list_for_each_entry_safe(sta, tmp, &local->sta_list, list) {
		if (!sdata || sdata == sta->sdata) {
			__sta_info_unlink(&sta);
			if (sta) {
				list_add_tail(&sta->list, &tmp_list);
				ret++;
			}
		}
	}
	spin_unlock_irqrestore(&local->sta_lock, flags);

	list_for_each_entry_safe(sta, tmp, &tmp_list, list)
		sta_info_destroy(sta);

	return ret;
}

void ieee80211_sta_expire(struct ieee80211_sub_if_data *sdata,
			  unsigned long exp_time)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta, *tmp;
	LIST_HEAD(tmp_list);
	unsigned long flags;

	spin_lock_irqsave(&local->sta_lock, flags);
	list_for_each_entry_safe(sta, tmp, &local->sta_list, list)
		if (time_after(jiffies, sta->last_rx + exp_time)) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
			printk(KERN_DEBUG "%s: expiring inactive STA %pM\n",
			       sdata->dev->name, sta->sta.addr);
#endif
			__sta_info_unlink(&sta);
			if (sta)
				list_add(&sta->list, &tmp_list);
		}
	spin_unlock_irqrestore(&local->sta_lock, flags);

	list_for_each_entry_safe(sta, tmp, &tmp_list, list)
		sta_info_destroy(sta);
}

struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_hw *hw,
					 const u8 *addr)
{
	struct sta_info *sta = sta_info_get(hw_to_local(hw), addr);

	if (!sta)
		return NULL;
	return &sta->sta;
}
EXPORT_SYMBOL(ieee80211_find_sta);
