/* audit.c -- Auditing support
 * Gateway between the kernel (e.g., selinux) and the user-space audit daemon.
 * System-call specific features have moved to auditsc.c
 *
 * Copyright 2003-2004 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Written by Rickard E. (Rik) Faith <faith@redhat.com>
 *
 * Goals: 1) Integrate fully with SELinux.
 *	  2) Minimal run-time overhead:
 *	     a) Minimal when syscall auditing is disabled (audit_enable=0).
 *	     b) Small when syscall auditing is enabled and no audit record
 *		is generated (defer as much work as possible to record
 *		generation time):
 *		i) context is allocated,
 *		ii) names from getname are stored without a copy, and
 *		iii) inode information stored from path_lookup.
 *	  3) Ability to disable syscall auditing at boot time (audit=0).
 *	  4) Usable by other parts of the kernel (if audit_log* is called,
 *	     then a syscall record will be generated automatically for the
 *	     current syscall).
 *	  5) Netlink interface to user-space.
 *	  6) Support low-overhead kernel-based filtering to minimize the
 *	     information that must be passed to user-space.
 *
 * Example user-space utilities: http://people.redhat.com/sgrubb/audit/
 */

#include <linux/init.h>
#include <asm/atomic.h>
#include <asm/types.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/kthread.h>

#include <linux/audit.h>

#include <net/sock.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>

/* No auditing will take place until audit_initialized != 0.
 * (Initialization happens after skb_init is called.) */
static int	audit_initialized;

/* No syscall auditing will take place unless audit_enabled != 0. */
int		audit_enabled;

/* Default state when kernel boots without any parameters. */
static int	audit_default;

/* If auditing cannot proceed, audit_failure selects what happens. */
static int	audit_failure = AUDIT_FAIL_PRINTK;

/* If audit records are to be written to the netlink socket, audit_pid
 * contains the (non-zero) pid. */
int		audit_pid;

/* If audit_limit is non-zero, limit the rate of sending audit records
 * to that number per second.  This prevents DoS attacks, but results in
 * audit records being dropped. */
static int	audit_rate_limit;

/* Number of outstanding audit_buffers allowed. */
static int	audit_backlog_limit = 64;
static int	audit_backlog_wait_time = 60 * HZ;
static int	audit_backlog_wait_overflow = 0;

/* The identity of the user shutting down the audit system. */
uid_t		audit_sig_uid = -1;
pid_t		audit_sig_pid = -1;

/* Records can be lost in several ways:
   0) [suppressed in audit_alloc]
   1) out of memory in audit_log_start [kmalloc of struct audit_buffer]
   2) out of memory in audit_log_move [alloc_skb]
   3) suppressed due to audit_rate_limit
   4) suppressed due to audit_backlog_limit
*/
static atomic_t    audit_lost = ATOMIC_INIT(0);

/* The netlink socket. */
static struct sock *audit_sock;

/* The audit_freelist is a list of pre-allocated audit buffers (if more
 * than AUDIT_MAXFREE are in use, the audit buffer is freed instead of
 * being placed on the freelist). */
static DEFINE_SPINLOCK(audit_freelist_lock);
static int	   audit_freelist_count = 0;
static LIST_HEAD(audit_freelist);

static struct sk_buff_head audit_skb_queue;
static struct task_struct *kauditd_task;
static DECLARE_WAIT_QUEUE_HEAD(kauditd_wait);
static DECLARE_WAIT_QUEUE_HEAD(audit_backlog_wait);

/* The netlink socket is only to be read by 1 CPU, which lets us assume
 * that list additions and deletions never happen simultaneously in
 * auditsc.c */
DECLARE_MUTEX(audit_netlink_sem);

/* AUDIT_BUFSIZ is the size of the temporary buffer used for formatting
 * audit records.  Since printk uses a 1024 byte buffer, this buffer
 * should be at least that large. */
#define AUDIT_BUFSIZ 1024

/* AUDIT_MAXFREE is the number of empty audit_buffers we keep on the
 * audit_freelist.  Doing so eliminates many kmalloc/kfree calls. */
#define AUDIT_MAXFREE  (2*NR_CPUS)

/* The audit_buffer is used when formatting an audit record.  The caller
 * locks briefly to get the record off the freelist or to allocate the
 * buffer, and locks briefly to send the buffer to the netlink layer or
 * to place it on a transmit queue.  Multiple audit_buffers can be in
 * use simultaneously. */
struct audit_buffer {
	struct list_head     list;
	struct sk_buff       *skb;	/* formatted skb ready to send */
	struct audit_context *ctx;	/* NULL or associated context */
	int		     gfp_mask;
};

static void audit_set_pid(struct audit_buffer *ab, pid_t pid)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)ab->skb->data;
	nlh->nlmsg_pid = pid;
}

static void audit_panic(const char *message)
{
	switch (audit_failure)
	{
	case AUDIT_FAIL_SILENT:
		break;
	case AUDIT_FAIL_PRINTK:
		printk(KERN_ERR "audit: %s\n", message);
		break;
	case AUDIT_FAIL_PANIC:
		panic("audit: %s\n", message);
		break;
	}
}

static inline int audit_rate_check(void)
{
	static unsigned long	last_check = 0;
	static int		messages   = 0;
	static DEFINE_SPINLOCK(lock);
	unsigned long		flags;
	unsigned long		now;
	unsigned long		elapsed;
	int			retval	   = 0;

	if (!audit_rate_limit) return 1;

	spin_lock_irqsave(&lock, flags);
	if (++messages < audit_rate_limit) {
		retval = 1;
	} else {
		now     = jiffies;
		elapsed = now - last_check;
		if (elapsed > HZ) {
			last_check = now;
			messages   = 0;
			retval     = 1;
		}
	}
	spin_unlock_irqrestore(&lock, flags);

	return retval;
}

/* Emit at least 1 message per second, even if audit_rate_check is
 * throttling. */
void audit_log_lost(const char *message)
{
	static unsigned long	last_msg = 0;
	static DEFINE_SPINLOCK(lock);
	unsigned long		flags;
	unsigned long		now;
	int			print;

	atomic_inc(&audit_lost);

	print = (audit_failure == AUDIT_FAIL_PANIC || !audit_rate_limit);

	if (!print) {
		spin_lock_irqsave(&lock, flags);
		now = jiffies;
		if (now - last_msg > HZ) {
			print = 1;
			last_msg = now;
		}
		spin_unlock_irqrestore(&lock, flags);
	}

	if (print) {
		printk(KERN_WARNING
		       "audit: audit_lost=%d audit_rate_limit=%d audit_backlog_limit=%d\n",
		       atomic_read(&audit_lost),
		       audit_rate_limit,
		       audit_backlog_limit);
		audit_panic(message);
	}

}

static int audit_set_rate_limit(int limit, uid_t loginuid)
{
	int old		 = audit_rate_limit;
	audit_rate_limit = limit;
	audit_log(NULL, GFP_KERNEL, AUDIT_CONFIG_CHANGE, 
			"audit_rate_limit=%d old=%d by auid=%u",
			audit_rate_limit, old, loginuid);
	return old;
}

static int audit_set_backlog_limit(int limit, uid_t loginuid)
{
	int old		 = audit_backlog_limit;
	audit_backlog_limit = limit;
	audit_log(NULL, GFP_KERNEL, AUDIT_CONFIG_CHANGE,
			"audit_backlog_limit=%d old=%d by auid=%u",
			audit_backlog_limit, old, loginuid);
	return old;
}

static int audit_set_enabled(int state, uid_t loginuid)
{
	int old		 = audit_enabled;
	if (state != 0 && state != 1)
		return -EINVAL;
	audit_enabled = state;
	audit_log(NULL, GFP_KERNEL, AUDIT_CONFIG_CHANGE,
			"audit_enabled=%d old=%d by auid=%u",
			audit_enabled, old, loginuid);
	return old;
}

static int audit_set_failure(int state, uid_t loginuid)
{
	int old		 = audit_failure;
	if (state != AUDIT_FAIL_SILENT
	    && state != AUDIT_FAIL_PRINTK
	    && state != AUDIT_FAIL_PANIC)
		return -EINVAL;
	audit_failure = state;
	audit_log(NULL, GFP_KERNEL, AUDIT_CONFIG_CHANGE,
			"audit_failure=%d old=%d by auid=%u",
			audit_failure, old, loginuid);
	return old;
}

int kauditd_thread(void *dummy)
{
	struct sk_buff *skb;

	while (1) {
		skb = skb_dequeue(&audit_skb_queue);
		wake_up(&audit_backlog_wait);
		if (skb) {
			if (audit_pid) {
				int err = netlink_unicast(audit_sock, skb, audit_pid, 0);
				if (err < 0) {
					BUG_ON(err != -ECONNREFUSED); /* Shoudn't happen */
					printk(KERN_ERR "audit: *NO* daemon at audit_pid=%d\n", audit_pid);
					audit_pid = 0;
				}
			} else {
				printk(KERN_NOTICE "%s\n", skb->data + NLMSG_SPACE(0));
				kfree_skb(skb);
			}
		} else {
			DECLARE_WAITQUEUE(wait, current);
			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&kauditd_wait, &wait);

			if (!skb_queue_len(&audit_skb_queue)) {
				try_to_freeze();
				schedule();
			}

			__set_current_state(TASK_RUNNING);
			remove_wait_queue(&kauditd_wait, &wait);
		}
	}
}

void audit_send_reply(int pid, int seq, int type, int done, int multi,
		      void *payload, int size)
{
	struct sk_buff	*skb;
	struct nlmsghdr	*nlh;
	int		len = NLMSG_SPACE(size);
	void		*data;
	int		flags = multi ? NLM_F_MULTI : 0;
	int		t     = done  ? NLMSG_DONE  : type;

	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return;

	nlh		 = NLMSG_PUT(skb, pid, seq, t, size);
	nlh->nlmsg_flags = flags;
	data		 = NLMSG_DATA(nlh);
	memcpy(data, payload, size);

	/* Ignore failure. It'll only happen if the sender goes away,
	   because our timeout is set to infinite. */
	netlink_unicast(audit_sock, skb, pid, 0);
	return;

nlmsg_failure:			/* Used by NLMSG_PUT */
	if (skb)
		kfree_skb(skb);
}

/*
 * Check for appropriate CAP_AUDIT_ capabilities on incoming audit
 * control messages.
 */
static int audit_netlink_ok(kernel_cap_t eff_cap, u16 msg_type)
{
	int err = 0;

	switch (msg_type) {
	case AUDIT_GET:
	case AUDIT_LIST:
	case AUDIT_SET:
	case AUDIT_ADD:
	case AUDIT_DEL:
	case AUDIT_SIGNAL_INFO:
		if (!cap_raised(eff_cap, CAP_AUDIT_CONTROL))
			err = -EPERM;
		break;
	case AUDIT_USER:
	case AUDIT_FIRST_USER_MSG...AUDIT_LAST_USER_MSG:
		if (!cap_raised(eff_cap, CAP_AUDIT_WRITE))
			err = -EPERM;
		break;
	default:  /* bad msg */
		err = -EINVAL;
	}

	return err;
}

static int audit_receive_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	u32			uid, pid, seq;
	void			*data;
	struct audit_status	*status_get, status_set;
	int			err;
	struct audit_buffer	*ab;
	u16			msg_type = nlh->nlmsg_type;
	uid_t			loginuid; /* loginuid of sender */
	struct audit_sig_info   sig_data;

	err = audit_netlink_ok(NETLINK_CB(skb).eff_cap, msg_type);
	if (err)
		return err;

	/* As soon as there's any sign of userspace auditd, start kauditd to talk to it */
	if (!kauditd_task)
		kauditd_task = kthread_run(kauditd_thread, NULL, "kauditd");
	if (IS_ERR(kauditd_task)) {
		err = PTR_ERR(kauditd_task);
		kauditd_task = NULL;
		return err;
	}

	pid  = NETLINK_CREDS(skb)->pid;
	uid  = NETLINK_CREDS(skb)->uid;
	loginuid = NETLINK_CB(skb).loginuid;
	seq  = nlh->nlmsg_seq;
	data = NLMSG_DATA(nlh);

	switch (msg_type) {
	case AUDIT_GET:
		status_set.enabled	 = audit_enabled;
		status_set.failure	 = audit_failure;
		status_set.pid		 = audit_pid;
		status_set.rate_limit	 = audit_rate_limit;
		status_set.backlog_limit = audit_backlog_limit;
		status_set.lost		 = atomic_read(&audit_lost);
		status_set.backlog	 = skb_queue_len(&audit_skb_queue);
		audit_send_reply(NETLINK_CB(skb).pid, seq, AUDIT_GET, 0, 0,
				 &status_set, sizeof(status_set));
		break;
	case AUDIT_SET:
		if (nlh->nlmsg_len < sizeof(struct audit_status))
			return -EINVAL;
		status_get   = (struct audit_status *)data;
		if (status_get->mask & AUDIT_STATUS_ENABLED) {
			err = audit_set_enabled(status_get->enabled, loginuid);
			if (err < 0) return err;
		}
		if (status_get->mask & AUDIT_STATUS_FAILURE) {
			err = audit_set_failure(status_get->failure, loginuid);
			if (err < 0) return err;
		}
		if (status_get->mask & AUDIT_STATUS_PID) {
			int old   = audit_pid;
			audit_pid = status_get->pid;
			audit_log(NULL, GFP_KERNEL, AUDIT_CONFIG_CHANGE,
				"audit_pid=%d old=%d by auid=%u",
				  audit_pid, old, loginuid);
		}
		if (status_get->mask & AUDIT_STATUS_RATE_LIMIT)
			audit_set_rate_limit(status_get->rate_limit, loginuid);
		if (status_get->mask & AUDIT_STATUS_BACKLOG_LIMIT)
			audit_set_backlog_limit(status_get->backlog_limit,
							loginuid);
		break;
	case AUDIT_USER:
	case AUDIT_FIRST_USER_MSG...AUDIT_LAST_USER_MSG:
		if (!audit_enabled && msg_type != AUDIT_USER_AVC)
			return 0;

		err = audit_filter_user(&NETLINK_CB(skb), msg_type);
		if (err == 1) {
			err = 0;
			ab = audit_log_start(NULL, GFP_KERNEL, msg_type);
			if (ab) {
				audit_log_format(ab,
						 "user pid=%d uid=%u auid=%u msg='%.1024s'",
						 pid, uid, loginuid, (char *)data);
				audit_set_pid(ab, pid);
				audit_log_end(ab);
			}
		}
		break;
	case AUDIT_ADD:
	case AUDIT_DEL:
		if (nlh->nlmsg_len < sizeof(struct audit_rule))
			return -EINVAL;
		/* fallthrough */
	case AUDIT_LIST:
		err = audit_receive_filter(nlh->nlmsg_type, NETLINK_CB(skb).pid,
					   uid, seq, data, loginuid);
		break;
	case AUDIT_SIGNAL_INFO:
		sig_data.uid = audit_sig_uid;
		sig_data.pid = audit_sig_pid;
		audit_send_reply(NETLINK_CB(skb).pid, seq, AUDIT_SIGNAL_INFO, 
				0, 0, &sig_data, sizeof(sig_data));
		break;
	default:
		err = -EINVAL;
		break;
	}

	return err < 0 ? err : 0;
}

/* Get message from skb (based on rtnetlink_rcv_skb).  Each message is
 * processed by audit_receive_msg.  Malformed skbs with wrong length are
 * discarded silently.  */
static void audit_receive_skb(struct sk_buff *skb)
{
	int		err;
	struct nlmsghdr	*nlh;
	u32		rlen;

	while (skb->len >= NLMSG_SPACE(0)) {
		nlh = (struct nlmsghdr *)skb->data;
		if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len)
			return;
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		if ((err = audit_receive_msg(skb, nlh))) {
			netlink_ack(skb, nlh, err);
		} else if (nlh->nlmsg_flags & NLM_F_ACK)
			netlink_ack(skb, nlh, 0);
		skb_pull(skb, rlen);
	}
}

/* Receive messages from netlink socket. */
static void audit_receive(struct sock *sk, int length)
{
	struct sk_buff  *skb;
	unsigned int qlen;

	down(&audit_netlink_sem);

	for (qlen = skb_queue_len(&sk->sk_receive_queue); qlen; qlen--) {
		skb = skb_dequeue(&sk->sk_receive_queue);
		audit_receive_skb(skb);
		kfree_skb(skb);
	}
	up(&audit_netlink_sem);
}


/* Initialize audit support at boot time. */
static int __init audit_init(void)
{
	printk(KERN_INFO "audit: initializing netlink socket (%s)\n",
	       audit_default ? "enabled" : "disabled");
	audit_sock = netlink_kernel_create(NETLINK_AUDIT, 0, audit_receive,
					   THIS_MODULE);
	if (!audit_sock)
		audit_panic("cannot initialize netlink socket");

	audit_sock->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;
	skb_queue_head_init(&audit_skb_queue);
	audit_initialized = 1;
	audit_enabled = audit_default;
	audit_log(NULL, GFP_KERNEL, AUDIT_KERNEL, "initialized");
	return 0;
}
__initcall(audit_init);

/* Process kernel command-line parameter at boot time.  audit=0 or audit=1. */
static int __init audit_enable(char *str)
{
	audit_default = !!simple_strtol(str, NULL, 0);
	printk(KERN_INFO "audit: %s%s\n",
	       audit_default ? "enabled" : "disabled",
	       audit_initialized ? "" : " (after initialization)");
	if (audit_initialized)
		audit_enabled = audit_default;
	return 0;
}

__setup("audit=", audit_enable);

static void audit_buffer_free(struct audit_buffer *ab)
{
	unsigned long flags;

	if (!ab)
		return;

	if (ab->skb)
		kfree_skb(ab->skb);

	spin_lock_irqsave(&audit_freelist_lock, flags);
	if (++audit_freelist_count > AUDIT_MAXFREE)
		kfree(ab);
	else
		list_add(&ab->list, &audit_freelist);
	spin_unlock_irqrestore(&audit_freelist_lock, flags);
}

static struct audit_buffer * audit_buffer_alloc(struct audit_context *ctx,
						gfp_t gfp_mask, int type)
{
	unsigned long flags;
	struct audit_buffer *ab = NULL;
	struct nlmsghdr *nlh;

	spin_lock_irqsave(&audit_freelist_lock, flags);
	if (!list_empty(&audit_freelist)) {
		ab = list_entry(audit_freelist.next,
				struct audit_buffer, list);
		list_del(&ab->list);
		--audit_freelist_count;
	}
	spin_unlock_irqrestore(&audit_freelist_lock, flags);

	if (!ab) {
		ab = kmalloc(sizeof(*ab), gfp_mask);
		if (!ab)
			goto err;
	}

	ab->skb = alloc_skb(AUDIT_BUFSIZ, gfp_mask);
	if (!ab->skb)
		goto err;

	ab->ctx = ctx;
	ab->gfp_mask = gfp_mask;
	nlh = (struct nlmsghdr *)skb_put(ab->skb, NLMSG_SPACE(0));
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_pid = 0;
	nlh->nlmsg_seq = 0;
	return ab;
err:
	audit_buffer_free(ab);
	return NULL;
}

/* Compute a serial number for the audit record.  Audit records are
 * written to user-space as soon as they are generated, so a complete
 * audit record may be written in several pieces.  The timestamp of the
 * record and this serial number are used by the user-space tools to
 * determine which pieces belong to the same audit record.  The
 * (timestamp,serial) tuple is unique for each syscall and is live from
 * syscall entry to syscall exit.
 *
 * NOTE: Another possibility is to store the formatted records off the
 * audit context (for those records that have a context), and emit them
 * all at syscall exit.  However, this could delay the reporting of
 * significant errors until syscall exit (or never, if the system
 * halts). */

unsigned int audit_serial(void)
{
	static spinlock_t serial_lock = SPIN_LOCK_UNLOCKED;
	static unsigned int serial = 0;

	unsigned long flags;
	unsigned int ret;

	spin_lock_irqsave(&serial_lock, flags);
	do {
		ret = ++serial;
	} while (unlikely(!ret));
	spin_unlock_irqrestore(&serial_lock, flags);

	return ret;
}

static inline void audit_get_stamp(struct audit_context *ctx, 
				   struct timespec *t, unsigned int *serial)
{
	if (ctx)
		auditsc_get_stamp(ctx, t, serial);
	else {
		*t = CURRENT_TIME;
		*serial = audit_serial();
	}
}

/* Obtain an audit buffer.  This routine does locking to obtain the
 * audit buffer, but then no locking is required for calls to
 * audit_log_*format.  If the tsk is a task that is currently in a
 * syscall, then the syscall is marked as auditable and an audit record
 * will be written at syscall exit.  If there is no associated task, tsk
 * should be NULL. */

struct audit_buffer *audit_log_start(struct audit_context *ctx, int gfp_mask,
				     int type)
{
	struct audit_buffer	*ab	= NULL;
	struct timespec		t;
	unsigned int		serial;
	int reserve;
	unsigned long timeout_start = jiffies;

	if (!audit_initialized)
		return NULL;

	if (gfp_mask & __GFP_WAIT)
		reserve = 0;
	else
		reserve = 5; /* Allow atomic callers to go up to five 
				entries over the normal backlog limit */

	while (audit_backlog_limit
	       && skb_queue_len(&audit_skb_queue) > audit_backlog_limit + reserve) {
		if (gfp_mask & __GFP_WAIT && audit_backlog_wait_time
		    && time_before(jiffies, timeout_start + audit_backlog_wait_time)) {

			/* Wait for auditd to drain the queue a little */
			DECLARE_WAITQUEUE(wait, current);
			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&audit_backlog_wait, &wait);

			if (audit_backlog_limit &&
			    skb_queue_len(&audit_skb_queue) > audit_backlog_limit)
				schedule_timeout(timeout_start + audit_backlog_wait_time - jiffies);

			__set_current_state(TASK_RUNNING);
			remove_wait_queue(&audit_backlog_wait, &wait);
			continue;
		}
		if (audit_rate_check())
			printk(KERN_WARNING
			       "audit: audit_backlog=%d > "
			       "audit_backlog_limit=%d\n",
			       skb_queue_len(&audit_skb_queue),
			       audit_backlog_limit);
		audit_log_lost("backlog limit exceeded");
		audit_backlog_wait_time = audit_backlog_wait_overflow;
		wake_up(&audit_backlog_wait);
		return NULL;
	}

	ab = audit_buffer_alloc(ctx, gfp_mask, type);
	if (!ab) {
		audit_log_lost("out of memory in audit_log_start");
		return NULL;
	}

	audit_get_stamp(ab->ctx, &t, &serial);

	audit_log_format(ab, "audit(%lu.%03lu:%u): ",
			 t.tv_sec, t.tv_nsec/1000000, serial);
	return ab;
}

/**
 * audit_expand - expand skb in the audit buffer
 * @ab: audit_buffer
 *
 * Returns 0 (no space) on failed expansion, or available space if
 * successful.
 */
static inline int audit_expand(struct audit_buffer *ab, int extra)
{
	struct sk_buff *skb = ab->skb;
	int ret = pskb_expand_head(skb, skb_headroom(skb), extra,
				   ab->gfp_mask);
	if (ret < 0) {
		audit_log_lost("out of memory in audit_expand");
		return 0;
	}
	return skb_tailroom(skb);
}

/* Format an audit message into the audit buffer.  If there isn't enough
 * room in the audit buffer, more room will be allocated and vsnprint
 * will be called a second time.  Currently, we assume that a printk
 * can't format message larger than 1024 bytes, so we don't either. */
static void audit_log_vformat(struct audit_buffer *ab, const char *fmt,
			      va_list args)
{
	int len, avail;
	struct sk_buff *skb;
	va_list args2;

	if (!ab)
		return;

	BUG_ON(!ab->skb);
	skb = ab->skb;
	avail = skb_tailroom(skb);
	if (avail == 0) {
		avail = audit_expand(ab, AUDIT_BUFSIZ);
		if (!avail)
			goto out;
	}
	va_copy(args2, args);
	len = vsnprintf(skb->tail, avail, fmt, args);
	if (len >= avail) {
		/* The printk buffer is 1024 bytes long, so if we get
		 * here and AUDIT_BUFSIZ is at least 1024, then we can
		 * log everything that printk could have logged. */
		avail = audit_expand(ab, max_t(unsigned, AUDIT_BUFSIZ, 1+len-avail));
		if (!avail)
			goto out;
		len = vsnprintf(skb->tail, avail, fmt, args2);
	}
	if (len > 0)
		skb_put(skb, len);
out:
	return;
}

/* Format a message into the audit buffer.  All the work is done in
 * audit_log_vformat. */
void audit_log_format(struct audit_buffer *ab, const char *fmt, ...)
{
	va_list args;

	if (!ab)
		return;
	va_start(args, fmt);
	audit_log_vformat(ab, fmt, args);
	va_end(args);
}

/* This function will take the passed buf and convert it into a string of
 * ascii hex digits. The new string is placed onto the skb. */
void audit_log_hex(struct audit_buffer *ab, const unsigned char *buf, 
		size_t len)
{
	int i, avail, new_len;
	unsigned char *ptr;
	struct sk_buff *skb;
	static const unsigned char *hex = "0123456789ABCDEF";

	BUG_ON(!ab->skb);
	skb = ab->skb;
	avail = skb_tailroom(skb);
	new_len = len<<1;
	if (new_len >= avail) {
		/* Round the buffer request up to the next multiple */
		new_len = AUDIT_BUFSIZ*(((new_len-avail)/AUDIT_BUFSIZ) + 1);
		avail = audit_expand(ab, new_len);
		if (!avail)
			return;
	}

	ptr = skb->tail;
	for (i=0; i<len; i++) {
		*ptr++ = hex[(buf[i] & 0xF0)>>4]; /* Upper nibble */
		*ptr++ = hex[buf[i] & 0x0F];	  /* Lower nibble */
	}
	*ptr = 0;
	skb_put(skb, len << 1); /* new string is twice the old string */
}

/* This code will escape a string that is passed to it if the string
 * contains a control character, unprintable character, double quote mark, 
 * or a space. Unescaped strings will start and end with a double quote mark.
 * Strings that are escaped are printed in hex (2 digits per char). */
void audit_log_untrustedstring(struct audit_buffer *ab, const char *string)
{
	const unsigned char *p = string;

	while (*p) {
		if (*p == '"' || *p < 0x21 || *p > 0x7f) {
			audit_log_hex(ab, string, strlen(string));
			return;
		}
		p++;
	}
	audit_log_format(ab, "\"%s\"", string);
}

/* This is a helper-function to print the escaped d_path */
void audit_log_d_path(struct audit_buffer *ab, const char *prefix,
		      struct dentry *dentry, struct vfsmount *vfsmnt)
{
	char *p, *path;

	if (prefix)
		audit_log_format(ab, " %s", prefix);

	/* We will allow 11 spaces for ' (deleted)' to be appended */
	path = kmalloc(PATH_MAX+11, ab->gfp_mask);
	if (!path) {
		audit_log_format(ab, "<no memory>");
		return;
	}
	p = d_path(dentry, vfsmnt, path, PATH_MAX+11);
	if (IS_ERR(p)) { /* Should never happen since we send PATH_MAX */
		/* FIXME: can we save some information here? */
		audit_log_format(ab, "<too long>");
	} else 
		audit_log_untrustedstring(ab, p);
	kfree(path);
}

/* The netlink_* functions cannot be called inside an irq context, so
 * the audit buffer is places on a queue and a tasklet is scheduled to
 * remove them from the queue outside the irq context.  May be called in
 * any context. */
void audit_log_end(struct audit_buffer *ab)
{
	if (!ab)
		return;
	if (!audit_rate_check()) {
		audit_log_lost("rate limit exceeded");
	} else {
		if (audit_pid) {
			struct nlmsghdr *nlh = (struct nlmsghdr *)ab->skb->data;
			nlh->nlmsg_len = ab->skb->len - NLMSG_SPACE(0);
			skb_queue_tail(&audit_skb_queue, ab->skb);
			ab->skb = NULL;
			wake_up_interruptible(&kauditd_wait);
		} else {
			printk(KERN_NOTICE "%s\n", ab->skb->data + NLMSG_SPACE(0));
		}
	}
	audit_buffer_free(ab);
}

/* Log an audit record.  This is a convenience function that calls
 * audit_log_start, audit_log_vformat, and audit_log_end.  It may be
 * called in any context. */
void audit_log(struct audit_context *ctx, int gfp_mask, int type, 
	       const char *fmt, ...)
{
	struct audit_buffer *ab;
	va_list args;

	ab = audit_log_start(ctx, gfp_mask, type);
	if (ab) {
		va_start(args, fmt);
		audit_log_vformat(ab, fmt, args);
		va_end(args);
		audit_log_end(ab);
	}
}
