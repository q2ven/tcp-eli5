// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define TARGET_NS	5000000ULL	/* 5 ms target sojourn time */
#define INTERVAL_NS	20000000ULL	/* 20 ms sliding minimum window */
#define BOTTLENECK_NS	4000000ULL	/* 4 ms bottleneck serialization rate */

struct skb_node {
	__u64 enqueue_time;
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock codel_lock;
private(A) struct bpf_list_head codel_queue __contains(skb_node, node);

static __u64 next_tx_ns;
static __u64 first_above_time;
static __u32 drop_count;
static bool dropping;

SEC("struct_ops/bpf_codel_enqueue")
int BPF_PROG(bpf_codel_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct skb_node *skbn;
	u32 pkt_len;

	if (sch->q.qlen >= sch->limit)
		goto drop;

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn)
		goto drop;

	/* Stamp arrival timestamp to measure sojourn time in dequeue */
	skbn->enqueue_time = bpf_ktime_get_ns();
	pkt_len = qdisc_pkt_len(skb);

	sch->q.qlen++;
	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&codel_lock);
	bpf_list_push_back(&codel_queue, &skbn->node);
	bpf_spin_unlock(&codel_lock);

	sch->qstats.backlog += pkt_len;
	return NET_XMIT_SUCCESS;

drop:
	bpf_qdisc_skb_drop(skb, to_free);
	sch->qstats.drops++;
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_codel_dequeue")
struct sk_buff *BPF_PROG(bpf_codel_dequeue, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct sk_buff *skb = NULL;
	struct skb_node *skbn;
	u64 now, enqueue_time, sojourn;

	if (!sch->q.qlen)
		return NULL;

	now = bpf_ktime_get_ns();

	/* TODO 1: Enforce bottleneck link rate (BOTTLENECK_NS).
	 * If now < next_tx_ns, schedule the watchdog timer:
	 *   bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	 * and return NULL.
	 */

	bpf_spin_lock(&codel_lock);
	node = bpf_list_pop_front(&codel_queue);
	bpf_spin_unlock(&codel_lock);
	if (!node)
		return NULL;

	skbn = container_of(node, struct skb_node, node);
	skb = bpf_kptr_xchg(&skbn->skb, skb);
	enqueue_time = skbn->enqueue_time;
	bpf_obj_drop(skbn);
	if (!skb)
		return NULL;

	/* TODO 2: CoDel Active Queue Management (AQM) state machine.
	 *
	 * Compute packet sojourn time:
	 *   sojourn = now - enqueue_time;
	 *
	 * 1. If sojourn < TARGET_NS (healthy delay):
	 *    - Reset first_above_time = 0;
	 *    - Reset dropping = false;
	 *
	 * 2. Else (sojourn >= TARGET_NS, standing queue delay detected):
	 *    - If first_above_time == 0:
	 *        first_above_time = now + INTERVAL_NS;
	 *    - Else if now >= first_above_time:
	 *        // Persistent bufferbloat! Enter dropping state and head-drop
	 *        // this stale packet, then dequeue the next fresher packet.
	 *        dropping = true;
	 *        drop_count++;
	 *        sch->qstats.backlog -= qdisc_pkt_len(skb);
	 *        sch->q.qlen--;
	 *        sch->qstats.drops++;
	 *        bpf_kfree_skb(skb);
	 *        skb = NULL;
	 *
	 *        bpf_spin_lock(&codel_lock);
	 *        node = bpf_list_pop_front(&codel_queue);
	 *        bpf_spin_unlock(&codel_lock);
	 *        if (!node)
	 *            return NULL;
	 *
	 *        skbn = container_of(node, struct skb_node, node);
	 *        skb = bpf_kptr_xchg(&skbn->skb, skb);
	 *        bpf_obj_drop(skbn);
	 *        if (!skb)
	 *            return NULL;
	 */

	next_tx_ns = now + BOTTLENECK_NS;
	sch->qstats.backlog -= qdisc_pkt_len(skb);
	bpf_qdisc_bstats_update(sch, skb);
	sch->q.qlen--;

	return skb;
}

SEC("struct_ops/bpf_codel_init")
int BPF_PROG(bpf_codel_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 1000;
	next_tx_ns = 0;
	first_above_time = 0;
	drop_count = 0;
	dropping = false;
	return 0;
}

SEC("struct_ops/bpf_codel_reset")
void BPF_PROG(bpf_codel_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&codel_lock);
		node = bpf_list_pop_front(&codel_queue);
		bpf_spin_unlock(&codel_lock);

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
	first_above_time = 0;
	drop_count = 0;
	dropping = false;
}

SEC("struct_ops")
void BPF_PROG(bpf_codel_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops codel = {
	.enqueue   = (void *)bpf_codel_enqueue,
	.dequeue   = (void *)bpf_codel_dequeue,
	.init      = (void *)bpf_codel_init,
	.reset     = (void *)bpf_codel_reset,
	.destroy   = (void *)bpf_codel_destroy,
	.id        = "bpf_codel",
};
