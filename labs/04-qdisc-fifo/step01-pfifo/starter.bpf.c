// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1 (Starter): Packet-limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Fill in the TODOs below to implement a 4-packet limit FIFO queue in BPF.
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
	/* TODO 1: Check queue limit:
	 *   If sch->q.qlen >= sch->limit, increment sch->qstats.drops++,
	 *   drop the packet with bpf_qdisc_skb_drop(skb, to_free),
	 *   and return NET_XMIT_DROP.
	 */

	/* TODO 2: Allocate skb_node object:
	 *   skbn = bpf_obj_new(typeof(*skbn));
	 *   If allocation fails (!skbn), increment sch->qstats.drops++,
	 *   drop the packet with bpf_qdisc_skb_drop(skb, to_free),
	 *   and return NET_XMIT_DROP.
	 */

	/* TODO 3: Read packet length BEFORE kptr exchange:
	 *   u32 pkt_len = qdisc_pkt_len(skb);
	 *
	 *   Store skb into skbn via bpf_kptr_xchg(&skbn->skb, skb).
	 *   If bpf_kptr_xchg returns a non-NULL skb, drop it with
	 *   bpf_qdisc_skb_drop(skb, to_free).
	 */

	/* TODO 4: Enqueue skbn to q_fifo:
	 *   Acquire bpf_spin_lock(&q_fifo_lock), push to back with
	 *   bpf_list_push_back(&q_fifo, &skbn->node), and release lock.
	 *   Increment sch->q.qlen++ and add pkt_len to sch->qstats.backlog.
	 *   Return NET_XMIT_SUCCESS.
	 */

	/* Placeholder: drop all incoming packets */
	sch->qstats.drops++;
	bpf_qdisc_skb_drop(skb, to_free);
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_pfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_pfifo_dequeue, struct Qdisc *sch)
{
	/* TODO 5: Check empty queue:
	 *   If !sch->q.qlen, return NULL.
	 */

	/* TODO 6: Watchdog rate limiter:
	 *   __u64 now = bpf_ktime_get_ns();
	 *   If now < next_tx_ns, schedule watchdog timer with:
	 *     bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	 *   and return NULL.
	 */

	/* TODO 7: Dequeue head node under bpf_spin_lock(&q_fifo_lock):
	 *   Pop from front with bpf_list_pop_front(&q_fifo).
	 *   Remember: NEVER call bpf_kptr_xchg or bpf_obj_drop inside
	 *   bpf_spin_lock! Unlock immediately after popping.
	 */

	/* TODO 8: Extract skb and free skbn:
	 *   struct skb_node *skbn = container_of(node, struct skb_node, node);
	 *   struct sk_buff *skb = bpf_kptr_xchg(&skbn->skb, NULL);
	 *   bpf_obj_drop(skbn);
	 *   If !skb, return NULL.
	 */

	/* TODO 9: Update qdisc stats and schedule next transmission:
	 *   next_tx_ns = now + BOTTLENECK_NS;
	 *   sch->qstats.backlog -= qdisc_pkt_len(skb);
	 *   bpf_qdisc_bstats_update(sch, skb);
	 *   sch->q.qlen--;
	 *   Return skb.
	 */

	return NULL;
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
