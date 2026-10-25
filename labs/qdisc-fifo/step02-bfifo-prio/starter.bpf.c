// SPDX-License-Identifier: GPL-2.0
/*
 * Step 2 (Starter): 2-Band Priority + Byte-Limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Fill in the TODOs below to implement a 2-band priority queue in BPF.
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BYTE_LIMIT	3500		/* 3500 bytes limit for normal queue */
#define BOTTLENECK_NS	4000000ULL	/* 4 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_lock;
private(A) struct bpf_list_head q_prio __contains(skb_node, node);
private(A) struct bpf_list_head q_normal __contains(skb_node, node);
static __u64 next_tx_ns;

SEC("struct_ops/bpf_bfifo_enqueue")
int BPF_PROG(bpf_bfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	/* TODO 1: Check total packet limit:
	 *   If sch->q.qlen >= sch->limit, goto drop.
	 */

	/* TODO 2: Identify priority and measure packet length:
	 *   bool is_prio = (skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL;
	 *   u32 pkt_len = qdisc_pkt_len(skb);
	 */

	/* TODO 3: Enforce BYTE_LIMIT for normal traffic:
	 *   If !is_prio and (sch->qstats.backlog + pkt_len > BYTE_LIMIT),
	 *   goto drop.
	 */

	/* TODO 4: Allocate skb_node and store skb:
	 *   skbn = bpf_obj_new(typeof(*skbn));
	 *   if (!skbn) goto drop;
	 *   skb = bpf_kptr_xchg(&skbn->skb, skb);
	 *   if (skb) bpf_qdisc_skb_drop(skb, to_free);
	 */

	/* TODO 5: Enqueue to q_prio or q_normal:
	 *   bpf_spin_lock(&q_lock);
	 *   if (is_prio)
	 *       bpf_list_push_back(&q_prio, &skbn->node);
	 *   else
	 *       bpf_list_push_back(&q_normal, &skbn->node);
	 *   bpf_spin_unlock(&q_lock);
	 *   sch->q.qlen++;
	 *   sch->qstats.backlog += pkt_len;
	 *   return NET_XMIT_SUCCESS;
	 */

	/* Placeholder: drop all incoming packets */
	sch->qstats.drops++;
	bpf_qdisc_skb_drop(skb, to_free);
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_bfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_bfifo_dequeue, struct Qdisc *sch)
{
	/* TODO 6: Check empty queue and watchdog timer:
	 *   if (!sch->q.qlen) return NULL;
	 *   __u64 now = bpf_ktime_get_ns();
	 *   if (now < next_tx_ns) {
	 *       bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	 *       return NULL;
	 *   }
	 */

	/* TODO 7: Dequeue highest priority packet available:
	 *   bpf_spin_lock(&q_lock);
	 *   node = bpf_list_pop_front(&q_prio);
	 *   if (!node)
	 *       node = bpf_list_pop_front(&q_normal);
	 *   bpf_spin_unlock(&q_lock);
	 *   if (!node) return NULL;
	 */

	/* TODO 8: Extract skb and free node:
	 *   skbn = container_of(node, struct skb_node, node);
	 *   skb = bpf_kptr_xchg(&skbn->skb, NULL);
	 *   bpf_obj_drop(skbn);
	 *   if (!skb) return NULL;
	 */

	/* TODO 9: Update statistics and schedule next transmit:
	 *   next_tx_ns = now + BOTTLENECK_NS;
	 *   sch->qstats.backlog -= qdisc_pkt_len(skb);
	 *   bpf_qdisc_bstats_update(sch, skb);
	 *   sch->q.qlen--;
	 *   return skb;
	 */

	return NULL;
}

SEC("struct_ops/bpf_bfifo_init")
int BPF_PROG(bpf_bfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 8;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_bfifo_reset")
void BPF_PROG(bpf_bfifo_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&q_lock);
		node = bpf_list_pop_front(&q_prio);
		if (!node)
			node = bpf_list_pop_front(&q_normal);
		bpf_spin_unlock(&q_lock);

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

SEC("struct_ops/bpf_bfifo_destroy")
void BPF_PROG(bpf_bfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_bfifo = {
	.enqueue   = (void *)bpf_bfifo_enqueue,
	.dequeue   = (void *)bpf_bfifo_dequeue,
	.init      = (void *)bpf_bfifo_init,
	.reset     = (void *)bpf_bfifo_reset,
	.destroy   = (void *)bpf_bfifo_destroy,
	.id        = "bpf_bfifo",
};
