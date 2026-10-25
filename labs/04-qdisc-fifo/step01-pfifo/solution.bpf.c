// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: Packet-limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Implements a 4-packet limit FIFO queue using BPF struct_ops (struct Qdisc_ops),
 * bpf_list, bpf_spin_lock, bpf_kptr, and the qdisc watchdog timer.
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BOTTLENECK_NS 2000000ULL /* 2 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_fifo_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
static __u64 next_tx_ns;

SEC("struct_ops/bpf_pfifo_enqueue")
int BPF_PROG(bpf_pfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct skb_node *skbn;
	u32 pkt_len;

	if (sch->q.qlen >= sch->limit) {
		sch->qstats.drops++;
		bpf_qdisc_skb_drop(skb, to_free);
		return NET_XMIT_DROP;
	}

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn) {
		sch->qstats.drops++;
		bpf_qdisc_skb_drop(skb, to_free);
		return NET_XMIT_DROP;
	}

	pkt_len = qdisc_pkt_len(skb);

	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&q_fifo_lock);
	bpf_list_push_back(&q_fifo, &skbn->node);
	bpf_spin_unlock(&q_fifo_lock);

	sch->q.qlen++;
	sch->qstats.backlog += pkt_len;
	return NET_XMIT_SUCCESS;
}

SEC("struct_ops/bpf_pfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_pfifo_dequeue, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct sk_buff *skb = NULL;
	struct skb_node *skbn;
	__u64 now = bpf_ktime_get_ns();

	if (!sch->q.qlen)
		return NULL;

	if (now < next_tx_ns) {
		bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
		return NULL;
	}

	bpf_spin_lock(&q_fifo_lock);
	node = bpf_list_pop_front(&q_fifo);
	bpf_spin_unlock(&q_fifo_lock);
	if (!node)
		return NULL;

	skbn = container_of(node, struct skb_node, node);
	skb = bpf_kptr_xchg(&skbn->skb, NULL);
	bpf_obj_drop(skbn);
	if (!skb)
		return NULL;

	next_tx_ns = now + BOTTLENECK_NS;
	sch->qstats.backlog -= qdisc_pkt_len(skb);
	bpf_qdisc_bstats_update(sch, skb);
	sch->q.qlen--;

	return skb;
}

SEC("struct_ops/bpf_pfifo_init")
int BPF_PROG(bpf_pfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 4;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_pfifo_reset")
void BPF_PROG(bpf_pfifo_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&q_fifo_lock);
		node = bpf_list_pop_front(&q_fifo);
		bpf_spin_unlock(&q_fifo_lock);

		if (!node)
			break;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		if (skb)
			bpf_kfree_skb(skb);
		bpf_obj_drop(skbn);
	}
	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
	next_tx_ns = 0;
}

SEC("struct_ops/bpf_pfifo_destroy")
void BPF_PROG(bpf_pfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_pfifo = {
	.enqueue   = (void *)bpf_pfifo_enqueue,
	.dequeue   = (void *)bpf_pfifo_dequeue,
	.init      = (void *)bpf_pfifo_init,
	.reset     = (void *)bpf_pfifo_reset,
	.destroy   = (void *)bpf_pfifo_destroy,
	.id        = "bpf_pfifo",
};
