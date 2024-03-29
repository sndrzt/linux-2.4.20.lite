/* IP tables module for matching the value of the IPv4 and TCP ECN bits
 *
 * ipt_ecn.c,v 1.3 2002/05/29 15:09:00 laforge Exp
 *
 * (C) 2002 by Harald Welte <laforge@gnumonks.org>
 *
 * This software is distributed under the terms GNU GPL v2
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>

#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4/ipt_ecn_.h>

MODULE_AUTHOR("Harald Welte <laforge@gnumonks.org>");
MODULE_DESCRIPTION("IP tables ECN matching module");
MODULE_LICENSE("GPL");

static inline int match_ip(const struct sk_buff *skb,
			   const struct iphdr *iph,
			   const struct ipt_ecn_info *einfo)
{
	return ((iph->tos&IPT_ECN_IP_MASK) == einfo->ip_ect);
}

static inline int match_tcp(const struct sk_buff *skb,
			    const struct iphdr *iph,
			    const struct ipt_ecn_info *einfo)
{
	struct tcphdr *tcph = (void *)iph + iph->ihl*4;

	if (einfo->operation & IPT_ECN_OP_MATCH_ECE) {
		if (einfo->invert & IPT_ECN_OP_MATCH_ECE) {
			if (tcph->ece == 1)
				return 0;
		} else {
			if (tcph->ece == 0)
				return 0;
		}
	}

	if (einfo->operation & IPT_ECN_OP_MATCH_CWR) {
		if (einfo->invert & IPT_ECN_OP_MATCH_CWR) {
			if (tcph->cwr == 1)
				return 0;
		} else {
			if (tcph->cwr == 0)
				return 0;
		}
	}

	return 1;
}

static int match(const struct sk_buff *skb, const struct net_device *in,
		 const struct net_device *out, const void *matchinfo,
		 int offset, const void *hdr, u_int16_t datalen,
		 int *hotdrop)
{
	const struct ipt_ecn_info *info = matchinfo;
	const struct iphdr *iph = skb->nh.iph;

	if (info->operation & IPT_ECN_OP_MATCH_IP)
		if (!match_ip(skb, iph, info))
			return 0;

	if (info->operation & (IPT_ECN_OP_MATCH_ECE|IPT_ECN_OP_MATCH_CWR)) {
		if (iph->protocol != IPPROTO_TCP)
			return 0;
		if (!match_tcp(skb, iph, info))
			return 0;
	}

	return 1;
}

static int checkentry(const char *tablename, const struct ipt_ip *ip,
		      void *matchinfo, unsigned int matchsize,
		      unsigned int hook_mask)
{
	const struct ipt_ecn_info *info = matchinfo;

	if (matchsize != IPT_ALIGN(sizeof(struct ipt_ecn_info)))
		return 0;

	if (info->operation & IPT_ECN_OP_MATCH_MASK)
		return 0;

	if (info->invert & IPT_ECN_OP_MATCH_MASK)
		return 0;

	if (info->operation & (IPT_ECN_OP_MATCH_ECE|IPT_ECN_OP_MATCH_CWR)
	    && ip->proto != IPPROTO_TCP) {
		printk(KERN_WARNING "ipt_ecn: can't match TCP bits in rule for"
		       " non-tcp packets\n");
		return 0;
	}

	return 1;
}

static struct ipt_match ecn_match = { { NULL, NULL }, "ecn", &match,
		&checkentry, NULL, THIS_MODULE };

static int __init init(void)
{
	return ipt_register_match(&ecn_match);
}

static void __exit fini(void)
{
	ipt_unregister_match(&ecn_match);
}

module_init(init);
module_exit(fini);
