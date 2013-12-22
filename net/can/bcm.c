/*
 * bcm.c - Broadcast Manager to filter/send (cyclic) CAN content
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/uio.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/can.h>
#include <linux/can/core.h>
#include <linux/can/bcm.h>
#include <net/sock.h>
#include <net/net_namespace.h>

/* use of last_frames[index].can_dlc */
#define RX_RECV    0x40 /* received data for this element */
#define RX_THR     0x80 /* element not been sent due to throttle feature */
#define BCM_CAN_DLC_MASK 0x0F /* clean private flags in can_dlc by masking */

/* get best masking value for can_rx_register() for a given single can_id */
#define REGMASK(id) ((id & CAN_RTR_FLAG) | ((id & CAN_EFF_FLAG) ? \
			(CAN_EFF_MASK | CAN_EFF_FLAG) : CAN_SFF_MASK))

#define CAN_BCM_VERSION CAN_VERSION
static __initdata const char banner[] = KERN_INFO
	"can: broadcast manager protocol (rev " CAN_BCM_VERSION ")\n";

MODULE_DESCRIPTION("PF_CAN broadcast manager protocol");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Oliver Hartkopp <oliver.hartkopp@volkswagen.de>");

/* easy access to can_frame payload */
static inline u64 GET_U64(const struct can_frame *cp)
{
	return *(u64 *)cp->data;
}

struct bcm_op {
	struct list_head list;
	int ifindex;
	canid_t can_id;
	int flags;
	unsigned long j_ival1, j_ival2, j_lastmsg;
	unsigned long frames_abs, frames_filtered;
	struct timer_list timer, thrtimer;
	struct timeval ival1, ival2;
	ktime_t rx_stamp;
	int rx_ifindex;
	int count;
	int nframes;
	int currframe;
	struct can_frame *frames;
	struct can_frame *last_frames;
	struct can_frame sframe;
	struct can_frame last_sframe;
	struct sock *sk;
	struct net_device *rx_reg_dev;
};

static struct proc_dir_entry *proc_dir;

struct bcm_sock {
	struct sock sk;
	int bound;
	int ifindex;
	struct notifier_block notifier;
	struct list_head rx_ops;
	struct list_head tx_ops;
	unsigned long dropped_usr_msgs;
	struct proc_dir_entry *bcm_proc_read;
	char procname [9]; /* pointer printed in ASCII with \0 */
};

static inline struct bcm_sock *bcm_sk(const struct sock *sk)
{
	return (struct bcm_sock *)sk;
}

#define CFSIZ sizeof(struct can_frame)
#define OPSIZ sizeof(struct bcm_op)
#define MHSIZ sizeof(struct bcm_msg_head)

/*
 * rounded_tv2jif - calculate jiffies from timeval including optional up
 * @tv: pointer to timeval
 *
 * Description:
 * Unlike timeval_to_jiffies() provided in include/linux/jiffies.h, this
 * function is intentionally more relaxed on precise timer ticks to get
 * exact one jiffy for requested 1000us on a 1000HZ machine.
 * This code is to be removed when upgrading to kernel hrtimer.
 *
 * Return:
 *  calculated jiffies (max: ULONG_MAX)
 */
static unsigned long rounded_tv2jif(const struct timeval *tv)
{
	unsigned long sec  = tv->tv_sec;
	unsigned long usec = tv->tv_usec;
	unsigned long jif;

	if (sec > ULONG_MAX / HZ)
		return ULONG_MAX;

	/* round up to get at least the requested time */
	usec += 1000000 / HZ - 1;

	jif  = usec / (1000000 / HZ);

	if (sec * HZ > ULONG_MAX - jif)
		return ULONG_MAX;

	return jif + sec * HZ;
}

/*
 * procfs functions
 */
static char *bcm_proc_getifname(int ifindex)
{
	struct net_device *dev;

	if (!ifindex)
		return "any";

	/* no usage counting */
	dev = __dev_get_by_index(&init_net, ifindex);
	if (dev)
		return dev->name;

	return "???";
}

static int bcm_read_proc(char *page, char **start, off_t off,
			 int count, int *eof, void *data)
{
	int len = 0;
	struct sock *sk = (struct sock *)data;
	struct bcm_sock *bo = bcm_sk(sk);
	struct bcm_op *op;

	len += snprintf(page + len, PAGE_SIZE - len, ">>> socket %p",
			sk->sk_socket);
	len += snprintf(page + len, PAGE_SIZE - len, " / sk %p", sk);
	len += snprintf(page + len, PAGE_SIZE - len, " / bo %p", bo);
	len += snprintf(page + len, PAGE_SIZE - len, " / dropped %lu",
			bo->dropped_usr_msgs);
	len += snprintf(page + len, PAGE_SIZE - len, " / bound %s",
			bcm_proc_getifname(bo->ifindex));
	len += snprintf(page + len, PAGE_SIZE - len, " <<<\n");

	list_for_each_entry(op, &bo->rx_ops, list) {

		unsigned long reduction;

		/* print only active entries & prevent division by zero */
		if (!op->frames_abs)
			continue;

		len += snprintf(page + len, PAGE_SIZE - len,
				"rx_op: %03X %-5s ",
				op->can_id, bcm_proc_getifname(op->ifindex));
		len += snprintf(page + len, PAGE_SIZE - len, "[%d]%c ",
				op->nframes,
				(op->flags & RX_CHECK_DLC)?'d':' ');
		if (op->j_ival1)
			len += snprintf(page + len, PAGE_SIZE - len,
					"timeo=%ld ", op->j_ival1);

		if (op->j_ival2)
			len += snprintf(page + len, PAGE_SIZE - len,
					"thr=%ld ", op->j_ival2);

		len += snprintf(page + len, PAGE_SIZE - len,
				"# recv %ld (%ld) => reduction: ",
				op->frames_filtered, op->frames_abs);

		reduction = 100 - (op->frames_filtered * 100) / op->frames_abs;

		len += snprintf(page + len, PAGE_SIZE - len, "%s%ld%%\n",
				(reduction == 100)?"near ":"", reduction);

		if (len > PAGE_SIZE - 200) {
			/* mark output cut off */
			len += snprintf(page + len, PAGE_SIZE - len, "(..)\n");
			break;
		}
	}

	list_for_each_entry(op, &bo->tx_ops, list) {

		len += snprintf(page + len, PAGE_SIZE - len,
				"tx_op: %03X %s [%d] ",
				op->can_id, bcm_proc_getifname(op->ifindex),
				op->nframes);
		if (op->j_ival1)
			len += snprintf(page + len, PAGE_SIZE - len, "t1=%ld ",
					op->j_ival1);

		if (op->j_ival2)
			len += snprintf(page + len, PAGE_SIZE - len, "t2=%ld ",
					op->j_ival2);

		len += snprintf(page + len, PAGE_SIZE - len, "# sent %ld\n",
				op->frames_abs);

		if (len > PAGE_SIZE - 100) {
			/* mark output cut off */
			len += snprintf(page + len, PAGE_SIZE - len, "(..)\n");
			break;
		}
	}

	len += snprintf(page + len, PAGE_SIZE - len, "\n");

	*eof = 1;
	return len;
}

/*
 * bcm_can_tx - send the (next) CAN frame to the appropriate CAN interface
 *              of the given bcm tx op
 */
static void bcm_can_tx(struct bcm_op *op)
{
	struct sk_buff *skb;
	struct net_device *dev;
	struct can_frame *cf = &op->frames[op->currframe];

	/* no target device? => exit */
	if (!op->ifindex)
		return;

	dev = dev_get_by_index(&init_net, op->ifindex);
	if (!dev) {
		/* RFC: should this bcm_op remove itself here? */
		return;
	}

	skb = alloc_skb(CFSIZ, gfp_any());
	if (!skb)
		goto out;

	memcpy(skb_put(skb, CFSIZ), cf, CFSIZ);

	/* send with loopback */
	skb->dev = dev;
	skb->sk = op->sk;
	can_send(skb, 1);

	/* update statistics */
	op->currframe++;
	op->frames_abs++;

	/* reached last frame? */
	if (op->currframe >= op->nframes)
		op->currframe = 0;
 out:
	dev_put(dev);
}

/*
 * bcm_send_to_user - send a BCM message to the userspace
 *                    (consisting of bcm_msg_head + x CAN frames)
 */
static void bcm_send_to_user(struct bcm_op *op, struct bcm_msg_head *head,
			     struct can_frame *frames, int has_timestamp)
{
	struct sk_buff *skb;
	struct can_frame *firstframe;
	struct sockaddr_can *addr;
	struct sock *sk = op->sk;
	int datalen = head->nframes * CFSIZ;
	int err;

	skb = alloc_skb(sizeof(*head) + datalen, gfp_any());
	if (!skb)
		return;

	memcpy(skb_put(skb, sizeof(*head)), head, sizeof(*head));

	if (head->nframes) {
		/* can_frames starting here */
		firstframe = (struct can_frame *)skb_tail_pointer(skb);

		memcpy(skb_put(skb, datalen), frames, datalen);

		/*
		 * the BCM uses the can_dlc-element of the can_frame
		 * structure for internal purposes. This is only
		 * relevant for updates that are generated by the
		 * BCM, where nframes is 1
		 */
		if (head->nframes == 1)
			firstframe->can_dlc &= BCM_CAN_DLC_MASK;
	}

	if (has_timestamp) {
		/* restore rx timestamp */
		skb->tstamp = op->rx_stamp;
	}

	/*
	 *  Put the datagram to the queue so that bcm_recvmsg() can
	 *  get it from there.  We need to pass the interface index to
	 *  bcm_recvmsg().  We pass a whole struct sockaddr_can in skb->cb
	 *  containing the interface index.
	 */

	BUILD_BUG_ON(sizeof(skb->cb) < sizeof(struct sockaddr_can));
	addr = (struct sockaddr_can *)skb->cb;
	memset(addr, 0, sizeof(*addr));
	addr->can_family  = AF_CAN;
	addr->can_ifindex = op->rx_ifindex;

	err = sock_queue_rcv_skb(sk, skb);
	if (err < 0) {
		struct bcm_sock *bo = bcm_sk(sk);

		kfree_skb(skb);
		/* don't care about overflows in this statistic */
		bo->dropped_usr_msgs++;
	}
}

/*
 * bcm_tx_timeout_handler - performes cyclic CAN frame transmissions
 */
static void bcm_tx_timeout_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op *)data;

	if (op->j_ival1 && (op->count > 0)) {

		op->count--;
		if (!op->count && (op->flags & TX_COUNTEVT)) {
			struct bcm_msg_head msg_head;

			/* create notification to user */
			msg_head.opcode  = TX_EXPIRED;
			msg_head.flags   = op->flags;
			msg_head.count   = op->count;
			msg_head.ival1   = op->ival1;
			msg_head.ival2   = op->ival2;
			msg_head.can_id  = op->can_id;
			msg_head.nframes = 0;

			bcm_send_to_user(op, &msg_head, NULL, 0);
		}
	}

	if (op->j_ival1 && (op->count > 0)) {

		/* send (next) frame */
		bcm_can_tx(op);
		mod_timer(&op->timer, jiffies + op->j_ival1);

	} else {
		if (op->j_ival2) {

			/* send (next) frame */
			bcm_can_tx(op);
			mod_timer(&op->timer, jiffies + op->j_ival2);
		}
	}

	return;
}

/*
 * bcm_rx_changed - create a RX_CHANGED notification due to changed content
 */
static void bcm_rx_changed(struct bcm_op *op, struct can_frame *data)
{
	struct bcm_msg_head head;

	op->j_lastmsg = jiffies;

	/* update statistics */
	op->frames_filtered++;

	/* prevent statistics overflow */
	if (op->frames_filtered > ULONG_MAX/100)
		op->frames_filtered = op->frames_abs = 0;

	head.opcode  = RX_CHANGED;
	head.flags   = op->flags;
	head.count   = op->count;
	head.ival1   = op->ival1;
	head.ival2   = op->ival2;
	head.can_id  = op->can_id;
	head.nframes = 1;

	bcm_send_to_user(op, &head, data, 1);
}

/*
 * bcm_rx_update_and_send - process a detected relevant receive content change
 *                          1. update the last received data
 *                          2. send a notification to the user (if possible)
 */
static void bcm_rx_update_and_send(struct bcm_op *op,
				   struct can_frame *lastdata,
				   struct can_frame *rxdata)
{
	unsigned long nexttx = op->j_lastmsg + op->j_ival2;

	memcpy(lastdata, rxdata, CFSIZ);

	/* mark as used */
	lastdata->can_dlc |= RX_RECV;

	/* throttle bcm_rx_changed ? */
	if ((op->thrtimer.expires) ||
	    ((op->j_ival2) && (nexttx > jiffies))) {
		/* we are already waiting OR we have to start waiting */

		/* mark as 'throttled' */
		lastdata->can_dlc |= RX_THR;

		if (!(op->thrtimer.expires)) {
			/* start the timer only the first time */
			mod_timer(&op->thrtimer, nexttx);
		}

	} else {
		/* send RX_CHANGED to the user immediately */
		bcm_rx_changed(op, rxdata);
	}
}

/*
 * bcm_rx_cmp_to_index - (bit)compares the currently received data to formerly
 *                       received data stored in op->last_frames[]
 */
static void bcm_rx_cmp_to_index(struct bcm_op *op, int index,
				struct can_frame *rxdata)
{
	/*
	 * no one uses the MSBs of can_dlc for comparation,
	 * so we use it here to detect the first time of reception
	 */

	if (!(op->last_frames[index].can_dlc & RX_RECV)) {
		/* received data for the first time => send update to user */
		bcm_rx_update_and_send(op, &op->last_frames[index], rxdata);
		return;
	}

	/* do a real check in can_frame data section */

	if ((GET_U64(&op->frames[index]) & GET_U64(rxdata)) !=
	    (GET_U64(&op->frames[index]) & GET_U64(&op->last_frames[index]))) {
		bcm_rx_update_and_send(op, &op->last_frames[index], rxdata);
		return;
	}

	if (op->flags & RX_CHECK_DLC) {
		/* do a real check in can_frame dlc */
		if (rxdata->can_dlc != (op->last_frames[index].can_dlc &
					BCM_CAN_DLC_MASK)) {
			bcm_rx_update_and_send(op, &op->last_frames[index],
					       rxdata);
			return;
		}
	}
}

/*
 * bcm_rx_starttimer - enable timeout monitoring for CAN frame receiption
 */
static void bcm_rx_starttimer(struct bcm_op *op)
{
	if (op->flags & RX_NO_AUTOTIMER)
		return;

	if (op->j_ival1)
		mod_timer(&op->timer, jiffies + op->j_ival1);
}

/*
 * bcm_rx_timeout_handler - when the (cyclic) CAN frame receiption timed out
 */
static void bcm_rx_timeout_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op *)data;
	struct bcm_msg_head msg_head;

	msg_head.opcode  = RX_TIMEOUT;
	msg_head.flags   = op->flags;
	msg_head.count   = op->count;
	msg_head.ival1   = op->ival1;
	msg_head.ival2   = op->ival2;
	msg_head.can_id  = op->can_id;
	msg_head.nframes = 0;

	bcm_send_to_user(op, &msg_head, NULL, 0);

	/* no restart of the timer is done here! */

	/* if user wants to be informed, when cyclic CAN-Messages come back */
	if ((op->flags & RX_ANNOUNCE_RESUME) && op->last_frames) {
		/* clear received can_frames to indicate 'nothing received' */
		memset(op->last_frames, 0, op->nframes * CFSIZ);
	}
}

/*
 * bcm_rx_thr_handler - the time for blocked content updates is over now:
 *                      Check for throttled data and send it to the userspace
 */
static void bcm_rx_thr_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op *)data;
	int i = 0;

	/* mark disabled / consumed timer */
	op->thrtimer.expires = 0;

	if (op->nframes > 1) {
		/* for MUX filter we start at index 1 */
		for (i = 1; i < op->nframes; i++) {
			if ((op->last_frames) &&
			    (op->last_frames[i].can_dlc & RX_THR)) {
				op->last_frames[i].can_dlc &= ~RX_THR;
				bcm_rx_changed(op, &op->last_frames[i]);
			}
		}

	} else {
		/* for RX_FILTER_ID and simple filter */
		if (op->last_frames && (op->last_frames[0].can_dlc & RX_THR)) {
			op->last_frames[0].can_dlc &= ~RX_THR;
			bcm_rx_changed(op, &op->last_frames[0]);
		}
	}
}

/*
 * bcm_rx_handler - handle a CAN frame receiption
 */
static void bcm_rx_handler(struct sk_buff *skb, void *data)
{
	struct bcm_op *op = (struct bcm_op *)data;
	struct can_frame rxframe;
	int i;

	/* disable timeout */
	del_timer(&op->timer);

	if (skb->len == sizeof(rxframe)) {
		memcpy(&rxframe, skb->data, sizeof(rxframe));
		/* save rx timestamp */
		op->rx_stamp = skb->tstamp;
		/* save originator for recvfrom() */
		op->rx_ifindex = skb->dev->ifindex;
		/* update statistics */
		op->frames_abs++;
		kfree_skb(skb);

	} else {
		kfree_skb(skb);
		return;
	}

	if (op->can_id != rxframe.can_id)
		return;

	if (op->flags & RX_RTR_FRAME) {
		/* send reply for RTR-request (placed in op->frames[0]) */
		bcm_can_tx(op);
		return;
	}

	if (op->flags & RX_FILTER_ID) {
		/* the easiest case */
		bcm_rx_update_and_send(op, &op->last_frames[0], &rxframe);
		bcm_rx_starttimer(op);
		return;
	}

	if (op->nframes == 1) {
		/* simple compare with index 0 */
		bcm_rx_cmp_to_index(op, 0, &rxframe);
		bcm_rx_starttimer(op);
		return;
	}

	if (op->nframes > 1) {
		/*
		 * multiplex compare
		 *
		 * find the first multiplex mask that fits.
		 * Remark: The MUX-mask is stored in index 0
		 */

		for (i = 1; i < op->nframes; i++) {
			if ((GET_U64(&op->frames[0]) & GET_U64(&rxframe)) ==
			    (GET_U64(&op->frames[0]) &
			     GET_U64(&op->frames[i]))) {
				bcm_rx_cmp_to_index(op, i, &rxframe);
				break;
			}
		}
		bcm_rx_starttimer(op);
	}
}

/*
 * helpers for bcm_op handling: find & delete bcm [rx|tx] op elements
 */
static struct bcm_op *bcm_find_op(struct list_head *ops, canid_t can_id,
				  int ifindex)
{
	struct bcm_op *op;

	list_for_each_entry(op, ops, list) {
		if ((op->can_id == can_id) && (op->ifindex == ifindex))
			return op;
	}

	return NULL;
}

static void bcm_remove_op(struct bcm_op *op)
{
	del_timer(&op->timer);
	del_timer(&op->thrtimer);

	if ((op->frames) && (op->frames != &op->sframe))
		kfree(op->frames);

	if ((op->last_frames) && (op->last_frames != &op->last_sframe))
		kfree(op->last_frames);

	kfree(op);

	return;
}

static void bcm_rx_unreg(struct net_device *dev, struct bcm_op *op)
{
	if (op->rx_reg_dev == dev) {
		can_rx_unregister(dev, op->can_id, REGMASK(op->can_id),
				  bcm_rx_handler, op);

		/* mark as removed subscription */
		op->rx_reg_dev = NULL;
	} else
		printk(KERN_ERR "can-bcm: bcm_rx_unreg: registered device "
		       "mismatch %p %p\n", op->rx_reg_dev, dev);
}

/*
 * bcm_delete_rx_op - find and remove a rx op (returns number of removed ops)
 */
static int bcm_delete_rx_op(struct list_head *ops, canid_t can_id, int ifindex)
{
	struct bcm_op *op, *n;

	list_for_each_entry_safe(op, n, ops, list) {
		if ((op->can_id == can_id) && (op->ifindex == ifindex)) {

			/*
			 * Don't care if we're bound or not (due to netdev
			 * problems) can_rx_unregister() is always a save
			 * thing to do here.
			 */
			if (op->ifindex) {
				/*
				 * Only remove subscriptions that had not
				 * been removed due to NETDEV_UNREGISTER
				 * in bcm_notifier()
				 */
				if (op->rx_reg_dev) {
					struct net_device *dev;

					dev = dev_get_by_index(&init_net,
							       op->ifindex);
					if (dev) {
						bcm_rx_unreg(dev, op);
						dev_put(dev);
					}
				}
			} else
				can_rx_unregister(NULL, op->can_id,
						  REGMASK(op->can_id),
						  bcm_rx_handler, op);

			list_del(&op->list);
			bcm_remove_op(op);
			return 1; /* done */
		}
	}

	return 0; /* not found */
}

/*
 * bcm_delete_tx_op - find and remove a tx op (returns number of removed ops)
 */
static int bcm_delete_tx_op(struct list_head *ops, canid_t can_id, int ifindex)
{
	struct bcm_op *op, *n;

	list_for_each_entry_safe(op, n, ops, list) {
		if ((op->can_id == can_id) && (op->ifindex == ifindex)) {
			list_del(&op->list);
			bcm_remove_op(op);
			return 1; /* done */
		}
	}

	return 0; /* not found */
}

/*
 * bcm_read_op - read out a bcm_op and send it to the user (for bcm_sendmsg)
 */
static int bcm_read_op(struct list_head *ops, struct bcm_msg_head *msg_head,
		       int ifindex)
{
	struct bcm_op *op = bcm_find_op(ops, msg_head->can_id, ifindex);

	if (!op)
		return -EINVAL;

	/* put current values into msg_head */
	msg_head->flags   = op->flags;
	msg_head->count   = op->count;
	msg_head->ival1   = op->ival1;
	msg_head->ival2   = op->ival2;
	msg_head->nframes = op->nframes;

	bcm_send_to_user(op, msg_head, op->frames, 0);

	return MHSIZ;
}

/*
 * bcm_tx_setup - create or update a bcm tx op (for bcm_sendmsg)
 */
static int bcm_tx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			int ifindex, struct sock *sk)
{
	struct bcm_sock *bo = bcm_sk(sk);
	struct bcm_op *op;
	int i, err;

	/* we need a real device to send frames */
	if (!ifindex)
		return -ENODEV;

	/* we need at least one can_frame */
	if (msg_head->nframes < 1)
		return -EINVAL;

	/* check the given can_id */
	op = bcm_find_op(&bo->tx_ops, msg_head->can_id, ifindex);

	if (op) {
		/* update existing BCM operation */

		/*
		 * Do we need more space for the can_frames than currently
		 * allocated? -> This is a _really_ unusual use-case and
		 * therefore (complexity / locking) it is not supported.
		 */
		if (msg_head->nframes > op->nframes)
			return -E2BIG;

		/* update can_frames content */
		for (i = 0; i < msg_head->nframes; i++) {
			err = memcpy_fromiovec((u8 *)&op->frames[i],
					       msg->msg_iov, CFSIZ);

			if (op->frames[i].can_dlc > 8)
				err = -EINVAL;

			if (err < 0)
				return err;

			if (msg_head->flags & TX_CP_CAN_ID) {
				/* copy can_id into frame */
				op->frames[i].can_id = msg_head->can_id;
			}
		}

	} else {
		/* insert new BCM operation for the given can_id */

		op = kzalloc(OPSIZ, GFP_KERNEL);
		if (!op)
			return -ENOMEM;

		op->can_id    = msg_head->can_id;

		/* create array for can_frames and copy the data */
		if (msg_head->nframes > 1) {
			op->frames = kmalloc(msg_head->nframes * CFSIZ,
					     GFP_KERNEL);
			if (!op->frames) {
				kfree(op);
				return -ENOMEM;
			}
		} else
			op->frames = &op->sframe;

		for (i = 0; i < msg_head->nframes; i++) {
			err = memcpy_fromiovec((u8 *)&op->frames[i],
					       msg->msg_iov, CFSIZ);

			if (op->frames[i].can_dlc > 8)
				err = -EINVAL;

			if (err < 0) {
				if (op->frames != &op->sframe)
					kfree(op->frames);
				kfree(op);
				return err;
			}

			if (msg_head->flags & TX_CP_CAN_ID) {
				/* copy can_id into frame */
				op->frames[i].can_id = msg_head->can_id;
			}
		}

		/* tx_ops never compare with previous received messages */
		op->last_frames = NULL;

		/* bcm_can_tx / bcm_tx_timeout_handler needs this */
		op->sk = sk;
		op->ifindex = ifindex;

		/* initialize uninitialized (kzalloc) structure */
		setup_timer(&op->timer, bcm_tx_timeout_handler,
			    (unsigned long)op);

		/* currently unused in tx_ops */
		init_timer(&op->thrtimer);

		/* add this bcm_op to the list of the tx_ops */
		list_add(&op->list, &bo->tx_ops);

	} /* if ((op = bcm_find_op(&bo->tx_ops, msg_head->can_id, ifindex))) */

	if (op->nframes != msg_head->nframes) {
		op->nframes   = msg_head->nframes;
		/* start multiple frame transmission with index 0 */
		op->currframe = 0;
	}

	/* check flags */

	op->flags = msg_head->flags;

	if (op->flags & TX_RESET_MULTI_IDX) {
		/* start multiple frame transmission with index 0 */
		op->currframe = 0;
	}

	if (op->flags & SETTIMER) {
		/* set timer values */
		op->count = msg_head->count;
		op->ival1 = msg_head->ival1;
		op->ival2 = msg_head->ival2;
		op->j_ival1 = rounded_tv2jif(&msg_head->ival1);
		op->j_ival2 = rounded_tv2jif(&msg_head->ival2);

		/* disable an active timer due to zero values? */
		if (!op->j_ival1 && !op->j_ival2)
			del_timer(&op->timer);
	}

	if ((op->flags & STARTTIMER) &&
	    ((op->j_ival1 && op->count) || op->j_ival2)) {

		/* spec: send can_frame when starting timer */
		op->flags |= TX_ANNOUNCE;

		if (op->j_ival1 && (op->count > 0)) {
			/* op->count-- is done in bcm_tx_timeout_handler */
			mod_timer(&op->timer, jiffies + op->j_ival1);
		} else
			mod_timer(&op->timer, jiffies + op->j_ival2);
	}

	if (op->flags & TX_ANNOUNCE)
		bcm_can_tx(op);

	return msg_head->nframes * CFSIZ + MHSIZ;
}

/*
 * bcm_rx_setup - create or update a bcm rx op (for bcm_sendmsg)
 */
static int bcm_rx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			int ifindex, struct sock *sk)
{
	struct bcm_sock *bo = bcm_sk(sk);
	struct bcm_op *op;
	int do_rx_register;
	int err = 0;

	if ((msg_head->flags & RX_FILTER_ID) || (!(msg_head->nframes))) {
		/* be robust against wrong usage ... */
		msg_head->flags |= RX_FILTER_ID;
		/* ignore trailing garbage */
		msg_head->nframes = 0;
	}

	if ((msg_head->flags & RX_RTR_FRAME) &&
	    ((msg_head->nframes != 1) ||
	     (!(msg_head->can_id & CAN_RTR_FLAG))))
		return -EINVAL;

	/* check the given can_id */
	op = bcm_find_op(&bo->rx_ops, msg_head->can_id, ifindex);
	if (op) {
		/* update existing BCM operation */

		/*
		 * Do we need more space for the can_frames than currently
		 * allocated? -> This is a _really_ unusual use-case and
		 * therefore (complexity / locking) it is not supported.
		 */
		if (msg_head->nframes > op->nframes)
			return -E2BIG;

		if (msg_head->nframes) {
			/* update can_frames content */
			err = memcpy_fromiovec((u8 *)op->frames,
					       msg->msg_iov,
					       msg_head->nframes * CFSIZ);
			if (err < 0)
				return err;

			/* clear last_frames to indicate 'nothing received' */
			memset(op->last_frames, 0, msg_head->nframes * CFSIZ);
		}

		op->nframes = msg_head->nframes;

		/* Only an update -> do not call can_rx_register() */
		do_rx_register = 0;

	} else {
		/* insert new BCM operation for the given can_id */
		op = kzalloc(OPSIZ, GFP_KERNEL);
		if (!op)
			return -ENOMEM;

		op->can_id    = msg_head->can_id;
		op->nframes   = msg_head->nframes;

		if (msg_head->nframes > 1) {
			/* create array for can_frames and copy the data */
			op->frames = kmalloc(msg_head->nframes * CFSIZ,
					     GFP_KERNEL);
			if (!op->frames) {
				kfree(op);
				return -ENOMEM;
			}

			/* create and init array for received can_frames */
			op->last_frames = kzalloc(msg_head->nframes * CFSIZ,
						  GFP_KERNEL);
			if (!op->last_frames) {
				kfree(op->frames);
				kfree(op);
				return -ENOMEM;
			}

		} else {
			op->frames = &op->sframe;
			op->last_frames = &op->last_sframe;
		}

		if (msg_head->nframes) {
			err = memcpy_fromiovec((u8 *)op->frames, msg->msg_iov,
					       msg_head->nframes * CFSIZ);
			if (err < 0) {
				if (op->frames != &op->sframe)
					kfree(op->frames);
				if (op->last_frames != &op->last_sframe)
					kfree(op->last_frames);
				kfree(op);
				return err;
			}
		}

		/* bcm_can_tx / bcm_tx_timeout_handler needs this */
		op->sk = sk;
		op->ifindex = ifindex;

		/* initialize uninitialized (kzalloc) structure */
		setup_timer(&op->timer, bcm_rx_timeout_handler,
			    (unsigned long)op);

		/* init throttle timer for RX_CHANGED */
		setup_timer(&op->thrtimer, bcm_rx_thr_handler,
			    (unsigned long)op);

		/* mark disabled timer */
		op->thrtimer.expires = 0;

		/* add this bcm_op to the list of the rx_ops */
		list_add(&op->list, &bo->rx_ops);

		/* call can_rx_register() */
		do_rx_register = 1;

	} /* if ((op = bcm_find_op(&bo->rx_ops, msg_head->can_id, ifindex))) */

	/* check flags */
	op->flags = msg_head->flags;

	if (op->flags & RX_RTR_FRAME) {

		/* no timers in RTR-mode */
		del_timer(&op->thrtimer);
		del_timer(&op->timer);

		/*
		 * funny feature in RX(!)_SETUP only for RTR-mode:
		 * copy can_id into frame BUT without RTR-flag to
		 * prevent a full-load-loopback-test ... ;-]
		 */
		if ((op->flags & TX_CP_CAN_ID) ||
		    (op->frames[0].can_id == op->can_id))
			op->frames[0].can_id = op->can_id & ~CAN_RTR_FLAG;

	} else {
		if (op->flags & SETTIMER) {

			/* set timer value */
			op->ival1 = msg_head->ival1;
			op->ival2 = msg_head->ival2;
			op->j_ival1 = rounded_tv2jif(&msg_head->ival1);
			op->j_ival2 = rounded_tv2jif(&msg_head->ival2);

			/* disable an active timer due to zero value? */
			if (!op->j_ival1)
				del_timer(&op->timer);

			/* free currently blocked msgs ? */
			if (op->thrtimer.expires) {
				/* send blocked msgs hereafter */
				mod_timer(&op->thrtimer, jiffies + 2);
			}

			/*
			 * if (op->j_ival2) is zero, no (new) throttling
			 * will happen. For details see functions
			 * bcm_rx_update_and_send() and bcm_rx_thr_handler()
			 */
		}

		if ((op->flags & STARTTIMER) && op->j_ival1)
			mod_timer(&op->timer, jiffies + op->j_ival1);
	}

	/* now we can register for can_ids, if we added a new bcm_op */
	if (do_rx_register) {
		if (ifindex) {
			struct net_device *dev;

			dev = dev_get_by_index(&init_net, ifindex);
			if (dev) {
				err = can_rx_register(dev, op->can_id,
						      REGMASK(op->can_id),
						      bcm_rx_handler, op,
						      "bcm");

				op->rx_reg_dev = dev;
				dev_put(dev);
			}

		} else
			err = can_rx_register(NULL, op->can_id,
					      REGMASK(op->can_id),
					      bcm_rx_handler, op, "bcm");
		if (err) {
			/* this bcm rx op is broken -> remove it */
			list_del(&op->list);
			bcm_remove_op(op);
			return err;
		}
	}

	return msg_head->nframes * CFSIZ + MHSIZ;
}

/*
 * bcm_tx_send - send a single CAN frame to the CAN interface (for bcm_sendmsg)
 */
static int bcm_tx_send(struct msghdr *msg, int ifindex, struct sock *sk)
{
	struct sk_buff *skb;
	struct net_device *dev;
	int err;

	/* we need a real device to send frames */
	if (!ifindex)
		return -ENODEV;

	skb = alloc_skb(CFSIZ, GFP_KERNEL);

	if (!skb)
		return -ENOMEM;

	err = memcpy_fromiovec(skb_put(skb, CFSIZ), msg->msg_iov, CFSIZ);
	if (err < 0) {
		kfree_skb(skb);
		return err;
	}

	dev = dev_get_by_index(&init_net, ifindex);
	if (!dev) {
		kfree_skb(skb);
		return -ENODEV;
	}

	skb->dev = dev;
	skb->sk  = sk;
	err = can_send(skb, 1); /* send with loopback */
	dev_put(dev);

	if (err)
		return err;

	return CFSIZ + MHSIZ;
}

/*
 * bcm_sendmsg - process BCM commands (opcodes) from the userspace
 */
static int bcm_sendmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct bcm_sock *bo = bcm_sk(sk);
	int ifindex = bo->ifindex; /* default ifindex for this bcm_op */
	struct bcm_msg_head msg_head;
	int ret; /* read bytes or error codes as return value */

	if (!bo->bound)
		return -ENOTCONN;

	/* check for valid message length from userspace */
	if (size < MHSIZ || (size - MHSIZ) % CFSIZ)
		return -EINVAL;

	/* check for alternative ifindex for this bcm_op */

	if (!ifindex && msg->msg_name) {
		/* no bound device as default => check msg_name */
		struct sockaddr_can *addr =
			(struct sockaddr_can *)msg->msg_name;

		if (addr->can_family != AF_CAN)
			return -EINVAL;

		/* ifindex from sendto() */
		ifindex = addr->can_ifindex;

		if (ifindex) {
			struct net_device *dev;

			dev = dev_get_by_index(&init_net, ifindex);
			if (!dev)
				return -ENODEV;

			if (dev->type != ARPHRD_CAN) {
				dev_put(dev);
				return -ENODEV;
			}

			dev_put(dev);
		}
	}

	/* read message head information */

	ret = memcpy_fromiovec((u8 *)&msg_head, msg->msg_iov, MHSIZ);
	if (ret < 0)
		return ret;

	lock_sock(sk);

	switch (msg_head.opcode) {

	case TX_SETUP:
		ret = bcm_tx_setup(&msg_head, msg, ifindex, sk);
		break;

	case RX_SETUP:
		ret = bcm_rx_setup(&msg_head, msg, ifindex, sk);
		break;

	case TX_DELETE:
		if (bcm_delete_tx_op(&bo->tx_ops, msg_head.can_id, ifindex))
			ret = MHSIZ;
		else
			ret = -EINVAL;
		break;

	case RX_DELETE:
		if (bcm_delete_rx_op(&bo->rx_ops, msg_head.can_id, ifindex))
			ret = MHSIZ;
		else
			ret = -EINVAL;
		break;

	case TX_READ:
		/* reuse msg_head for the reply to TX_READ */
		msg_head.opcode  = TX_STATUS;
		ret = bcm_read_op(&bo->tx_ops, &msg_head, ifindex);
		break;

	case RX_READ:
		/* reuse msg_head for the reply to RX_READ */
		msg_head.opcode  = RX_STATUS;
		ret = bcm_read_op(&bo->rx_ops, &msg_head, ifindex);
		break;

	case TX_SEND:
		/* we need exactly one can_frame behind the msg head */
		if ((msg_head.nframes != 1) || (size != CFSIZ + MHSIZ))
			ret = -EINVAL;
		else
			ret = bcm_tx_send(msg, ifindex, sk);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	release_sock(sk);

	return ret;
}

/*
 * notification handler for netdevice status changes
 */
static int bcm_notifier(struct notifier_block *nb, unsigned long msg,
			void *data)
{
	struct net_device *dev = (struct net_device *)data;
	struct bcm_sock *bo = container_of(nb, struct bcm_sock, notifier);
	struct sock *sk = &bo->sk;
	struct bcm_op *op;
	int notify_enodev = 0;

	if (dev->nd_net != &init_net)
		return NOTIFY_DONE;

	if (dev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	switch (msg) {

	case NETDEV_UNREGISTER:
		lock_sock(sk);

		/* remove device specific receive entries */
		list_for_each_entry(op, &bo->rx_ops, list)
			if (op->rx_reg_dev == dev)
				bcm_rx_unreg(dev, op);

		/* remove device reference, if this is our bound device */
		if (bo->bound && bo->ifindex == dev->ifindex) {
			bo->bound   = 0;
			bo->ifindex = 0;
			notify_enodev = 1;
		}

		release_sock(sk);

		if (notify_enodev) {
			sk->sk_err = ENODEV;
			if (!sock_flag(sk, SOCK_DEAD))
				sk->sk_error_report(sk);
		}
		break;

	case NETDEV_DOWN:
		if (bo->bound && bo->ifindex == dev->ifindex) {
			sk->sk_err = ENETDOWN;
			if (!sock_flag(sk, SOCK_DEAD))
				sk->sk_error_report(sk);
		}
	}

	return NOTIFY_DONE;
}

/*
 * initial settings for all BCM sockets to be set at socket creation time
 */
static int bcm_init(struct sock *sk)
{
	struct bcm_sock *bo = bcm_sk(sk);

	bo->bound            = 0;
	bo->ifindex          = 0;
	bo->dropped_usr_msgs = 0;
	bo->bcm_proc_read    = NULL;

	INIT_LIST_HEAD(&bo->tx_ops);
	INIT_LIST_HEAD(&bo->rx_ops);

	/* set notifier */
	bo->notifier.notifier_call = bcm_notifier;

	register_netdevice_notifier(&bo->notifier);

	return 0;
}

/*
 * standard socket functions
 */
static int bcm_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct bcm_sock *bo = bcm_sk(sk);
	struct bcm_op *op, *next;

	/* remove bcm_ops, timer, rx_unregister(), etc. */

	unregister_netdevice_notifier(&bo->notifier);

	lock_sock(sk);

	list_for_each_entry_safe(op, next, &bo->tx_ops, list)
		bcm_remove_op(op);

	list_for_each_entry_safe(op, next, &bo->rx_ops, list) {
		/*
		 * Don't care if we're bound or not (due to netdev problems)
		 * can_rx_unregister() is always a save thing to do here.
		 */
		if (op->ifindex) {
			/*
			 * Only remove subscriptions that had not
			 * been removed due to NETDEV_UNREGISTER
			 * in bcm_notifier()
			 */
			if (op->rx_reg_dev) {
				struct net_device *dev;

				dev = dev_get_by_index(&init_net, op->ifindex);
				if (dev) {
					bcm_rx_unreg(dev, op);
					dev_put(dev);
				}
			}
		} else
			can_rx_unregister(NULL, op->can_id,
					  REGMASK(op->can_id),
					  bcm_rx_handler, op);

		bcm_remove_op(op);
	}

	/* remove procfs entry */
	if (proc_dir && bo->bcm_proc_read)
		remove_proc_entry(bo->procname, proc_dir);

	/* remove device reference */
	if (bo->bound) {
		bo->bound   = 0;
		bo->ifindex = 0;
	}

	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int bcm_connect(struct socket *sock, struct sockaddr *uaddr, int len,
		       int flags)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct bcm_sock *bo = bcm_sk(sk);

	if (bo->bound)
		return -EISCONN;

	/* bind a device to this socket */
	if (addr->can_ifindex) {
		struct net_device *dev;

		dev = dev_get_by_index(&init_net, addr->can_ifindex);
		if (!dev)
			return -ENODEV;

		if (dev->type != ARPHRD_CAN) {
			dev_put(dev);
			return -ENODEV;
		}

		bo->ifindex = dev->ifindex;
		dev_put(dev);

	} else {
		/* no interface reference for ifindex = 0 ('any' CAN device) */
		bo->ifindex = 0;
	}

	bo->bound = 1;

	if (proc_dir) {
		/* unique socket address as filename */
		sprintf(bo->procname, "%p", sock);
		bo->bcm_proc_read = create_proc_read_entry(bo->procname, 0644,
							   proc_dir,
							   bcm_read_proc, sk);
	}

	return 0;
}

static int bcm_recvmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int error = 0;
	int noblock;
	int err;

	noblock =  flags & MSG_DONTWAIT;
	flags   &= ~MSG_DONTWAIT;
	skb = skb_recv_datagram(sk, flags, noblock, &error);
	if (!skb)
		return error;

	if (skb->len < size)
		size = skb->len;

	err = memcpy_toiovec(msg->msg_iov, skb->data, size);
	if (err < 0) {
		skb_free_datagram(sk, skb);
		return err;
	}

	sock_recv_timestamp(msg, sk, skb);

	if (msg->msg_name) {
		msg->msg_namelen = sizeof(struct sockaddr_can);
		memcpy(msg->msg_name, skb->cb, msg->msg_namelen);
	}

	skb_free_datagram(sk, skb);

	return size;
}

static struct proto_ops bcm_ops __read_mostly = {
	.family        = PF_CAN,
	.release       = bcm_release,
	.bind          = sock_no_bind,
	.connect       = bcm_connect,
	.socketpair    = sock_no_socketpair,
	.accept        = sock_no_accept,
	.getname       = sock_no_getname,
	.poll          = datagram_poll,
	.ioctl         = NULL,		/* use can_ioctl() from af_can.c */
	.listen        = sock_no_listen,
	.shutdown      = sock_no_shutdown,
	.setsockopt    = sock_no_setsockopt,
	.getsockopt    = sock_no_getsockopt,
	.sendmsg       = bcm_sendmsg,
	.recvmsg       = bcm_recvmsg,
	.mmap          = sock_no_mmap,
	.sendpage      = sock_no_sendpage,
};

static struct proto bcm_proto __read_mostly = {
	.name       = "CAN_BCM",
	.owner      = THIS_MODULE,
	.obj_size   = sizeof(struct bcm_sock),
	.init       = bcm_init,
};

static struct can_proto bcm_can_proto __read_mostly = {
	.type       = SOCK_DGRAM,
	.protocol   = CAN_BCM,
	.capability = -1,
	.ops        = &bcm_ops,
	.prot       = &bcm_proto,
};

static int __init bcm_module_init(void)
{
	int err;

	printk(banner);

	err = can_proto_register(&bcm_can_proto);
	if (err < 0) {
		printk(KERN_ERR "can: registration of bcm protocol failed\n");
		return err;
	}

	/* create /proc/net/can-bcm directory */
	proc_dir = proc_mkdir("can-bcm", init_net.proc_net);

	if (proc_dir)
		proc_dir->owner = THIS_MODULE;

	return 0;
}

static void __exit bcm_module_exit(void)
{
	can_proto_unregister(&bcm_can_proto);

	if (proc_dir)
		proc_net_remove(&init_net, "can-bcm");
}

module_init(bcm_module_init);
module_exit(bcm_module_exit);
