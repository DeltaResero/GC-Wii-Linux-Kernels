/*
 * Copyright (C)2003,2004 USAGI/WIDE Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Author:
 *	Yasuyuki Kozakai @USAGI <yasuyuki.kozakai@toshiba.co.jp>
 */

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/in6.h>
#include <linux/icmpv6.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>
#include <net/ip6_checksum.h>
#include <linux/seq_file.h>
#include <linux/netfilter_ipv6.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/ipv6/nf_conntrack_icmpv6.h>
#include <net/netfilter/nf_log.h>

static unsigned int nf_ct_icmpv6_timeout __read_mostly = 30*HZ;

static bool icmpv6_pkt_to_tuple(const struct sk_buff *skb,
				unsigned int dataoff,
				struct nf_conntrack_tuple *tuple)
{
	const struct icmp6hdr *hp;
	struct icmp6hdr _hdr;

	hp = skb_header_pointer(skb, dataoff, sizeof(_hdr), &_hdr);
	if (hp == NULL)
		return false;
	tuple->dst.u.icmp.type = hp->icmp6_type;
	tuple->src.u.icmp.id = hp->icmp6_identifier;
	tuple->dst.u.icmp.code = hp->icmp6_code;

	return true;
}

/* Add 1; spaces filled with 0. */
static const u_int8_t invmap[] = {
	[ICMPV6_ECHO_REQUEST - 128]	= ICMPV6_ECHO_REPLY + 1,
	[ICMPV6_ECHO_REPLY - 128]	= ICMPV6_ECHO_REQUEST + 1,
	[ICMPV6_NI_QUERY - 128]		= ICMPV6_NI_REPLY + 1,
	[ICMPV6_NI_REPLY - 128]		= ICMPV6_NI_QUERY +1
};

static bool icmpv6_invert_tuple(struct nf_conntrack_tuple *tuple,
				const struct nf_conntrack_tuple *orig)
{
	int type = orig->dst.u.icmp.type - 128;
	if (type < 0 || type >= sizeof(invmap) || !invmap[type])
		return false;

	tuple->src.u.icmp.id   = orig->src.u.icmp.id;
	tuple->dst.u.icmp.type = invmap[type] - 1;
	tuple->dst.u.icmp.code = orig->dst.u.icmp.code;
	return true;
}

/* Print out the per-protocol part of the tuple. */
static int icmpv6_print_tuple(struct seq_file *s,
			      const struct nf_conntrack_tuple *tuple)
{
	return seq_printf(s, "type=%u code=%u id=%u ",
			  tuple->dst.u.icmp.type,
			  tuple->dst.u.icmp.code,
			  ntohs(tuple->src.u.icmp.id));
}

/* Returns verdict for packet, or -1 for invalid. */
static int icmpv6_packet(struct nf_conn *ct,
		       const struct sk_buff *skb,
		       unsigned int dataoff,
		       enum ip_conntrack_info ctinfo,
		       u_int8_t pf,
		       unsigned int hooknum)
{
	/* Try to delete connection immediately after all replies:
	   won't actually vanish as we still have skb, and del_timer
	   means this will only run once even if count hits zero twice
	   (theoretically possible with SMP) */
	if (CTINFO2DIR(ctinfo) == IP_CT_DIR_REPLY) {
		if (atomic_dec_and_test(&ct->proto.icmp.count))
			nf_ct_kill_acct(ct, ctinfo, skb);
	} else {
		atomic_inc(&ct->proto.icmp.count);
		nf_conntrack_event_cache(IPCT_PROTOINFO_VOLATILE, ct);
		nf_ct_refresh_acct(ct, ctinfo, skb, nf_ct_icmpv6_timeout);
	}

	return NF_ACCEPT;
}

/* Called when a new connection for this protocol found. */
static bool icmpv6_new(struct nf_conn *ct, const struct sk_buff *skb,
		       unsigned int dataoff)
{
	static const u_int8_t valid_new[] = {
		[ICMPV6_ECHO_REQUEST - 128] = 1,
		[ICMPV6_NI_QUERY - 128] = 1
	};
	int type = ct->tuplehash[0].tuple.dst.u.icmp.type - 128;

	if (type < 0 || type >= sizeof(valid_new) || !valid_new[type]) {
		/* Can't create a new ICMPv6 `conn' with this. */
		pr_debug("icmpv6: can't create new conn with type %u\n",
			 type + 128);
		nf_ct_dump_tuple_ipv6(&ct->tuplehash[0].tuple);
		return false;
	}
	atomic_set(&ct->proto.icmp.count, 0);
	return true;
}

static int
icmpv6_error_message(struct net *net,
		     struct sk_buff *skb,
		     unsigned int icmp6off,
		     enum ip_conntrack_info *ctinfo,
		     unsigned int hooknum)
{
	struct nf_conntrack_tuple intuple, origtuple;
	const struct nf_conntrack_tuple_hash *h;
	const struct nf_conntrack_l4proto *inproto;

	NF_CT_ASSERT(skb->nfct == NULL);

	/* Are they talking about one of our connections? */
	if (!nf_ct_get_tuplepr(skb,
			       skb_network_offset(skb)
				+ sizeof(struct ipv6hdr)
				+ sizeof(struct icmp6hdr),
			       PF_INET6, &origtuple)) {
		pr_debug("icmpv6_error: Can't get tuple\n");
		return -NF_ACCEPT;
	}

	/* rcu_read_lock()ed by nf_hook_slow */
	inproto = __nf_ct_l4proto_find(PF_INET6, origtuple.dst.protonum);

	/* Ordinarily, we'd expect the inverted tupleproto, but it's
	   been preserved inside the ICMP. */
	if (!nf_ct_invert_tuple(&intuple, &origtuple,
				&nf_conntrack_l3proto_ipv6, inproto)) {
		pr_debug("icmpv6_error: Can't invert tuple\n");
		return -NF_ACCEPT;
	}

	*ctinfo = IP_CT_RELATED;

	h = nf_conntrack_find_get(net, &intuple);
	if (!h) {
		pr_debug("icmpv6_error: no match\n");
		return -NF_ACCEPT;
	} else {
		if (NF_CT_DIRECTION(h) == IP_CT_DIR_REPLY)
			*ctinfo += IP_CT_IS_REPLY;
	}

	/* Update skb to refer to this connection */
	skb->nfct = &nf_ct_tuplehash_to_ctrack(h)->ct_general;
	skb->nfctinfo = *ctinfo;
	return -NF_ACCEPT;
}

static int
icmpv6_error(struct net *net, struct sk_buff *skb, unsigned int dataoff,
	     enum ip_conntrack_info *ctinfo, u_int8_t pf, unsigned int hooknum)
{
	const struct icmp6hdr *icmp6h;
	struct icmp6hdr _ih;

	icmp6h = skb_header_pointer(skb, dataoff, sizeof(_ih), &_ih);
	if (icmp6h == NULL) {
		if (LOG_INVALID(net, IPPROTO_ICMPV6))
		nf_log_packet(PF_INET6, 0, skb, NULL, NULL, NULL,
			      "nf_ct_icmpv6: short packet ");
		return -NF_ACCEPT;
	}

	if (net->ct.sysctl_checksum && hooknum == NF_INET_PRE_ROUTING &&
	    nf_ip6_checksum(skb, hooknum, dataoff, IPPROTO_ICMPV6)) {
		nf_log_packet(PF_INET6, 0, skb, NULL, NULL, NULL,
			      "nf_ct_icmpv6: ICMPv6 checksum failed\n");
		return -NF_ACCEPT;
	}

	/* is not error message ? */
	if (icmp6h->icmp6_type >= 128)
		return NF_ACCEPT;

	return icmpv6_error_message(net, skb, dataoff, ctinfo, hooknum);
}

#if defined(CONFIG_NF_CT_NETLINK) || defined(CONFIG_NF_CT_NETLINK_MODULE)

#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
static int icmpv6_tuple_to_nlattr(struct sk_buff *skb,
				  const struct nf_conntrack_tuple *t)
{
	NLA_PUT_BE16(skb, CTA_PROTO_ICMPV6_ID, t->src.u.icmp.id);
	NLA_PUT_U8(skb, CTA_PROTO_ICMPV6_TYPE, t->dst.u.icmp.type);
	NLA_PUT_U8(skb, CTA_PROTO_ICMPV6_CODE, t->dst.u.icmp.code);

	return 0;

nla_put_failure:
	return -1;
}

static const struct nla_policy icmpv6_nla_policy[CTA_PROTO_MAX+1] = {
	[CTA_PROTO_ICMPV6_TYPE]	= { .type = NLA_U8 },
	[CTA_PROTO_ICMPV6_CODE]	= { .type = NLA_U8 },
	[CTA_PROTO_ICMPV6_ID]	= { .type = NLA_U16 },
};

static int icmpv6_nlattr_to_tuple(struct nlattr *tb[],
				struct nf_conntrack_tuple *tuple)
{
	if (!tb[CTA_PROTO_ICMPV6_TYPE]
	    || !tb[CTA_PROTO_ICMPV6_CODE]
	    || !tb[CTA_PROTO_ICMPV6_ID])
		return -EINVAL;

	tuple->dst.u.icmp.type = nla_get_u8(tb[CTA_PROTO_ICMPV6_TYPE]);
	tuple->dst.u.icmp.code = nla_get_u8(tb[CTA_PROTO_ICMPV6_CODE]);
	tuple->src.u.icmp.id = nla_get_be16(tb[CTA_PROTO_ICMPV6_ID]);

	if (tuple->dst.u.icmp.type < 128
	    || tuple->dst.u.icmp.type - 128 >= sizeof(invmap)
	    || !invmap[tuple->dst.u.icmp.type - 128])
		return -EINVAL;

	return 0;
}
#endif

#ifdef CONFIG_SYSCTL
static struct ctl_table_header *icmpv6_sysctl_header;
static struct ctl_table icmpv6_sysctl_table[] = {
	{
		.procname	= "nf_conntrack_icmpv6_timeout",
		.data		= &nf_ct_icmpv6_timeout,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec_jiffies,
	},
	{
		.ctl_name	= 0
	}
};
#endif /* CONFIG_SYSCTL */

struct nf_conntrack_l4proto nf_conntrack_l4proto_icmpv6 __read_mostly =
{
	.l3proto		= PF_INET6,
	.l4proto		= IPPROTO_ICMPV6,
	.name			= "icmpv6",
	.pkt_to_tuple		= icmpv6_pkt_to_tuple,
	.invert_tuple		= icmpv6_invert_tuple,
	.print_tuple		= icmpv6_print_tuple,
	.packet			= icmpv6_packet,
	.new			= icmpv6_new,
	.error			= icmpv6_error,
#if defined(CONFIG_NF_CT_NETLINK) || defined(CONFIG_NF_CT_NETLINK_MODULE)
	.tuple_to_nlattr	= icmpv6_tuple_to_nlattr,
	.nlattr_to_tuple	= icmpv6_nlattr_to_tuple,
	.nla_policy		= icmpv6_nla_policy,
#endif
#ifdef CONFIG_SYSCTL
	.ctl_table_header	= &icmpv6_sysctl_header,
	.ctl_table		= icmpv6_sysctl_table,
#endif
};
