/*
 * IPv6 raw table, a port of the IPv4 raw table to IPv6
 *
 * Copyright (C) 2003 Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 */
#include <linux/module.h>
#include <linux/netfilter_ipv6/ip6_tables.h>

#define RAW_VALID_HOOKS ((1 << NF_INET_PRE_ROUTING) | (1 << NF_INET_LOCAL_OUT))

static const struct xt_table packet_raw = {
	.name = "raw",
	.valid_hooks = RAW_VALID_HOOKS,
	.me = THIS_MODULE,
	.af = NFPROTO_IPV6,
};

/* The work comes in here from netfilter.c. */
static unsigned int
ip6t_pre_routing_hook(unsigned int hook,
	 struct sk_buff *skb,
	 const struct net_device *in,
	 const struct net_device *out,
	 int (*okfn)(struct sk_buff *))
{
	return ip6t_do_table(skb, hook, in, out,
			     dev_net(in)->ipv6.ip6table_raw);
}

static unsigned int
ip6t_local_out_hook(unsigned int hook,
	 struct sk_buff *skb,
	 const struct net_device *in,
	 const struct net_device *out,
	 int (*okfn)(struct sk_buff *))
{
	return ip6t_do_table(skb, hook, in, out,
			     dev_net(out)->ipv6.ip6table_raw);
}

static struct nf_hook_ops ip6t_ops[] __read_mostly = {
	{
	  .hook = ip6t_pre_routing_hook,
	  .pf = NFPROTO_IPV6,
	  .hooknum = NF_INET_PRE_ROUTING,
	  .priority = NF_IP6_PRI_FIRST,
	  .owner = THIS_MODULE,
	},
	{
	  .hook = ip6t_local_out_hook,
	  .pf = NFPROTO_IPV6,
	  .hooknum = NF_INET_LOCAL_OUT,
	  .priority = NF_IP6_PRI_FIRST,
	  .owner = THIS_MODULE,
	},
};

static int __net_init ip6table_raw_net_init(struct net *net)
{
	struct ip6t_replace *repl;

	repl = ip6t_alloc_initial_table(&packet_raw);
	if (repl == NULL)
		return -ENOMEM;
	net->ipv6.ip6table_raw =
		ip6t_register_table(net, &packet_raw, repl);
	kfree(repl);
	if (IS_ERR(net->ipv6.ip6table_raw))
		return PTR_ERR(net->ipv6.ip6table_raw);
	return 0;
}

static void __net_exit ip6table_raw_net_exit(struct net *net)
{
	ip6t_unregister_table(net, net->ipv6.ip6table_raw);
}

static struct pernet_operations ip6table_raw_net_ops = {
	.init = ip6table_raw_net_init,
	.exit = ip6table_raw_net_exit,
};

static int __init ip6table_raw_init(void)
{
	int ret;

	ret = register_pernet_subsys(&ip6table_raw_net_ops);
	if (ret < 0)
		return ret;

	/* Register hooks */
	ret = nf_register_hooks(ip6t_ops, ARRAY_SIZE(ip6t_ops));
	if (ret < 0)
		goto cleanup_table;

	return ret;

 cleanup_table:
	unregister_pernet_subsys(&ip6table_raw_net_ops);
	return ret;
}

static void __exit ip6table_raw_fini(void)
{
	nf_unregister_hooks(ip6t_ops, ARRAY_SIZE(ip6t_ops));
	unregister_pernet_subsys(&ip6table_raw_net_ops);
}

module_init(ip6table_raw_init);
module_exit(ip6table_raw_fini);
MODULE_LICENSE("GPL");
