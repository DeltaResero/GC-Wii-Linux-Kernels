/*
 * ip_nat_proto_gre.c - Version 2.0
 *
 * NAT protocol helper module for GRE.
 *
 * GRE is a generic encapsulation protocol, which is generally not very
 * suited for NAT, as it has no protocol-specific part as port numbers.
 *
 * It has an optional key field, which may help us distinguishing two 
 * connections between the same two hosts.
 *
 * GRE is defined in RFC 1701 and RFC 1702, as well as RFC 2784 
 *
 * PPTP is built on top of a modified version of GRE, and has a mandatory
 * field called "CallID", which serves us for the same purpose as the key
 * field in plain GRE.
 *
 * Documentation about PPTP can be found in RFC 2637
 *
 * (C) 2000-2005 by Harald Welte <laforge@gnumonks.org>
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/ip.h>
#include <linux/netfilter_ipv4/ip_nat.h>
#include <linux/netfilter_ipv4/ip_nat_rule.h>
#include <linux/netfilter_ipv4/ip_nat_protocol.h>
#include <linux/netfilter_ipv4/ip_conntrack_proto_gre.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harald Welte <laforge@gnumonks.org>");
MODULE_DESCRIPTION("Netfilter NAT protocol helper module for GRE");

#if 0
#define DEBUGP(format, args...) printk(KERN_DEBUG "%s:%s: " format, __FILE__, \
				       __FUNCTION__, ## args)
#else
#define DEBUGP(x, args...)
#endif

/* is key in given range between min and max */
static int
gre_in_range(const struct ip_conntrack_tuple *tuple,
	     enum ip_nat_manip_type maniptype,
	     const union ip_conntrack_manip_proto *min,
	     const union ip_conntrack_manip_proto *max)
{
	u_int32_t key;

	if (maniptype == IP_NAT_MANIP_SRC)
		key = tuple->src.u.gre.key;
	else
		key = tuple->dst.u.gre.key;

	return ntohl(key) >= ntohl(min->gre.key)
		&& ntohl(key) <= ntohl(max->gre.key);
}

/* generate unique tuple ... */
static int 
gre_unique_tuple(struct ip_conntrack_tuple *tuple,
		 const struct ip_nat_range *range,
		 enum ip_nat_manip_type maniptype,
		 const struct ip_conntrack *conntrack)
{
	static u_int16_t key;
	u_int16_t *keyptr;
	unsigned int min, i, range_size;

	if (maniptype == IP_NAT_MANIP_SRC)
		keyptr = &tuple->src.u.gre.key;
	else
		keyptr = &tuple->dst.u.gre.key;

	if (!(range->flags & IP_NAT_RANGE_PROTO_SPECIFIED)) {
		DEBUGP("%p: NATing GRE PPTP\n", conntrack);
		min = 1;
		range_size = 0xffff;
	} else {
		min = ntohl(range->min.gre.key);
		range_size = ntohl(range->max.gre.key) - min + 1;
	}

	DEBUGP("min = %u, range_size = %u\n", min, range_size); 

	for (i = 0; i < range_size; i++, key++) {
		*keyptr = htonl(min + key % range_size);
		if (!ip_nat_used_tuple(tuple, conntrack))
			return 1;
	}

	DEBUGP("%p: no NAT mapping\n", conntrack);

	return 0;
}

/* manipulate a GRE packet according to maniptype */
static int
gre_manip_pkt(struct sk_buff **pskb,
	      unsigned int iphdroff,
	      const struct ip_conntrack_tuple *tuple,
	      enum ip_nat_manip_type maniptype)
{
	struct gre_hdr *greh;
	struct gre_hdr_pptp *pgreh;
	struct iphdr *iph = (struct iphdr *)((*pskb)->data + iphdroff);
	unsigned int hdroff = iphdroff + iph->ihl*4;

	/* pgreh includes two optional 32bit fields which are not required
	 * to be there.  That's where the magic '8' comes from */
	if (!skb_make_writable(pskb, hdroff + sizeof(*pgreh)-8))
		return 0;

	greh = (void *)(*pskb)->data + hdroff;
	pgreh = (struct gre_hdr_pptp *) greh;

	/* we only have destination manip of a packet, since 'source key' 
	 * is not present in the packet itself */
	if (maniptype == IP_NAT_MANIP_DST) {
		/* key manipulation is always dest */
		switch (greh->version) {
		case 0:
			if (!greh->key) {
				DEBUGP("can't nat GRE w/o key\n");
				break;
			}
			if (greh->csum) {
				/* FIXME: Never tested this code... */
				*(gre_csum(greh)) = 
					ip_nat_cheat_check(~*(gre_key(greh)),
							tuple->dst.u.gre.key,
							*(gre_csum(greh)));
			}
			*(gre_key(greh)) = tuple->dst.u.gre.key;
			break;
		case GRE_VERSION_PPTP:
			DEBUGP("call_id -> 0x%04x\n", 
				ntohs(tuple->dst.u.gre.key));
			pgreh->call_id = tuple->dst.u.gre.key;
			break;
		default:
			DEBUGP("can't nat unknown GRE version\n");
			return 0;
			break;
		}
	}
	return 1;
}

/* print out a nat tuple */
static unsigned int 
gre_print(char *buffer, 
	  const struct ip_conntrack_tuple *match,
	  const struct ip_conntrack_tuple *mask)
{
	unsigned int len = 0;

	if (mask->src.u.gre.key)
		len += sprintf(buffer + len, "srckey=0x%x ", 
				ntohl(match->src.u.gre.key));

	if (mask->dst.u.gre.key)
		len += sprintf(buffer + len, "dstkey=0x%x ",
				ntohl(match->src.u.gre.key));

	return len;
}

/* print a range of keys */
static unsigned int 
gre_print_range(char *buffer, const struct ip_nat_range *range)
{
	if (range->min.gre.key != 0 
	    || range->max.gre.key != 0xFFFF) {
		if (range->min.gre.key == range->max.gre.key)
			return sprintf(buffer, "key 0x%x ",
					ntohl(range->min.gre.key));
		else
			return sprintf(buffer, "keys 0x%u-0x%u ",
					ntohl(range->min.gre.key),
					ntohl(range->max.gre.key));
	} else
		return 0;
}

/* nat helper struct */
static struct ip_nat_protocol gre = { 
	.name		= "GRE", 
	.protonum	= IPPROTO_GRE,
	.manip_pkt	= gre_manip_pkt,
	.in_range	= gre_in_range,
	.unique_tuple	= gre_unique_tuple,
	.print		= gre_print,
	.print_range	= gre_print_range,
#if defined(CONFIG_IP_NF_CONNTRACK_NETLINK) || \
    defined(CONFIG_IP_NF_CONNTRACK_NETLINK_MODULE)
	.range_to_nfattr	= ip_nat_port_range_to_nfattr,
	.nfattr_to_range	= ip_nat_port_nfattr_to_range,
#endif
};
				  
int __init ip_nat_proto_gre_init(void)
{
	return ip_nat_protocol_register(&gre);
}

void __exit ip_nat_proto_gre_fini(void)
{
	ip_nat_protocol_unregister(&gre);
}
