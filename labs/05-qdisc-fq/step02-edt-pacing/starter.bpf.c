// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

#define ENOMEM 12

char _license[] SEC("license") = "GPL";

#define QUANTUM 1500
#define INITIAL_QUANTUM 2000
#define HORIZON_NS 2000000000ULL

#define NUM_FLOWS 1024

struct skb_node {
	u64 tstamp;
	struct sk_buff __kptr *skb;
	struct bpf_rb_node node;
};

struct fq_flow_node {
	s64 credit;
	u32 qlen;
	u64 age;
	u64 time_next_packet;
	struct bpf_list_node list_node;
	struct bpf_rb_node rb_node;
	struct bpf_rb_root queue __contains(skb_node, node);
	struct bpf_spin_lock lock;
	struct bpf_refcount refcount;
};

struct unset_throttled_flows_ctx {
	bool unset_all;
	u64 now;
};

struct fq_stashed_flow {
	struct fq_flow_node __kptr *flow;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u64);
	__type(value, struct fq_stashed_flow);
	__uint(max_entries, NUM_FLOWS);
} fq_flows SEC(".maps");

private(A) struct bpf_spin_lock fq_delayed_lock;
private(A) struct bpf_rb_root fq_delayed __contains(fq_flow_node, rb_node);

private(B) struct bpf_spin_lock fq_new_flows_lock;
private(B) struct bpf_list_head fq_new_flows __contains(fq_flow_node, list_node);

private(C) struct bpf_spin_lock fq_old_flows_lock;
private(C) struct bpf_list_head fq_old_flows __contains(fq_flow_node, list_node);

private(D) u32 fq_new_flow_cnt;
private(D) u32 fq_old_flow_cnt;
private(D) u64 time_next_delayed_flow;

static void bpf_kptr_xchg_back(void *map_val, void *ptr)
{
	void *ret;

	ret = bpf_kptr_xchg(map_val, ptr);
	if (ret)
		bpf_obj_drop(ret);
}

static bool skbn_tstamp_less(struct bpf_rb_node *a, const struct bpf_rb_node *b)
{
	struct skb_node *skbn_a;
	struct skb_node *skbn_b;

	skbn_a = container_of(a, struct skb_node, node);
	skbn_b = container_of(b, struct skb_node, node);

	return skbn_a->tstamp < skbn_b->tstamp;
}

static bool fn_time_next_packet_less(struct bpf_rb_node *a, const struct bpf_rb_node *b)
{
	struct fq_flow_node *flow_a;
	struct fq_flow_node *flow_b;

	flow_a = container_of(a, struct fq_flow_node, rb_node);
	flow_b = container_of(b, struct fq_flow_node, rb_node);

	return flow_a->time_next_packet < flow_b->time_next_packet;
}

static u64 fq_classify(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;

	if (sk && sk->__sk_common.skc_hash)
		return (u64)sk->__sk_common.skc_hash;

	u64 hash = (u64)bpf_skb_get_hash(skb);
	if (!hash)
		hash = 1;
	return hash;
}

static int fq_new_flow(struct fq_stashed_flow **sflow, u64 hash)
{
	struct fq_stashed_flow tmp = {};
	struct fq_flow_node *flow;
	int ret;

	flow = bpf_obj_new(typeof(*flow));
	if (!flow)
		return -ENOMEM;

	flow->credit = INITIAL_QUANTUM;
	flow->qlen = 0;
	flow->age = 1;
	flow->time_next_packet = 0;

	ret = bpf_map_update_elem(&fq_flows, &hash, &tmp, 0);
	if (ret < 0) {
		bpf_obj_drop(flow);
		return ret;
	}

	*sflow = bpf_map_lookup_elem(&fq_flows, &hash);
	if (!*sflow) {
		bpf_obj_drop(flow);
		return -ENOMEM;
	}

	bpf_kptr_xchg_back(&(*sflow)->flow, flow);
	return 0;
}

static int fq_unset_throttled_flows(u32 index, struct unset_throttled_flows_ctx *ctx)
{
	struct bpf_rb_node *node = NULL;
	struct fq_flow_node *flow;

	bpf_spin_lock(&fq_delayed_lock);

	node = bpf_rbtree_first(&fq_delayed);
	if (!node) {
		bpf_spin_unlock(&fq_delayed_lock);
		return 1;
	}

	flow = container_of(node, struct fq_flow_node, rb_node);
	if (!ctx->unset_all && flow->time_next_packet > ctx->now) {
		time_next_delayed_flow = flow->time_next_packet;
		bpf_spin_unlock(&fq_delayed_lock);
		return 1;
	}

	node = bpf_rbtree_remove(&fq_delayed, &flow->rb_node);
	bpf_spin_unlock(&fq_delayed_lock);

	if (!node)
		return 1;

	flow = container_of(node, struct fq_flow_node, rb_node);
	flow->age = 0;
	bpf_spin_lock(&fq_old_flows_lock);
	bpf_list_push_back(&fq_old_flows, &flow->list_node);
	bpf_spin_unlock(&fq_old_flows_lock);
	fq_old_flow_cnt++;

	return 0;
}

static void fq_flow_set_throttled(struct fq_flow_node *flow)
{
	flow->age = ~0ULL;

	if (time_next_delayed_flow > flow->time_next_packet)
		time_next_delayed_flow = flow->time_next_packet;

	bpf_spin_lock(&fq_delayed_lock);
	bpf_rbtree_add(&fq_delayed, &flow->rb_node, fn_time_next_packet_less);
	bpf_spin_unlock(&fq_delayed_lock);
}

/* TODO 3: Check throttled flows
 * If time_next_delayed_flow > now, none of the delayed flows are ready, so return.
 * Otherwise:
 * 1. Reset time_next_delayed_flow = ~0ULL.
 * 2. Loop over delayed flows using bpf_loop(NUM_FLOWS, fq_unset_throttled_flows, &ctx, 0)
 *    to pop flows whose time_next_packet <= now and move them to fq_old_flows.
 */
static void fq_check_throttled(u64 now)
{
	/* [YOUR CODE HERE] */
}

SEC("struct_ops/bpf_fq_edt_enqueue")
int BPF_PROG(bpf_fq_edt_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct fq_flow_node *flow = NULL, *flow_copy;
	struct fq_stashed_flow *sflow;
	struct sock *sk = skb->sk;
	u64 now, time_to_send;
	struct skb_node *skbn;
	u64 rate = 0;
	u64 hash;

	if (sch->q.qlen >= sch->limit)
		goto drop;

	now = bpf_ktime_get_ns();

	hash = fq_classify(skb);
	sflow = bpf_map_lookup_elem(&fq_flows, &hash);
	if (!sflow) {
		if (fq_new_flow(&sflow, hash) < 0)
			goto drop;
	}

	flow = bpf_kptr_xchg(&sflow->flow, flow);
	if (!flow)
		goto drop;

	if (sk)
		rate = sk->sk_max_pacing_rate;

	/* TODO 1: EDT time_to_send and pacing rate calculation
	 * 1. If skb->tstamp != 0, use skb->tstamp as time_to_send.
	 * 2. Else if rate > 0 && rate != ~0ULL:
	 *      time_to_send = (flow->time_next_packet > now) ? flow->time_next_packet : now;
	 * 3. Else:
	 *      time_to_send = now;
	 */
	time_to_send = now; /* [REPLACE WITH YOUR CODE] */

	/* TODO 2: Horizon check
	 * If (s64)time_to_send > (s64)(now + HORIZON_NS):
	 *   Release flow back to sflow->flow, drop packet, and return NET_XMIT_DROP.
	 */
	/* [YOUR CODE HERE] */

	if (rate > 0 && rate != ~0ULL)
		flow->time_next_packet = time_to_send + (u64)qdisc_pkt_len(skb) * NSEC_PER_SEC / rate;

	if (flow->age != 0 && flow->age != ~0ULL) {
		flow_copy = bpf_refcount_acquire(flow);
		flow_copy->credit = INITIAL_QUANTUM;
		flow_copy->age = 0;
		bpf_spin_lock(&fq_new_flows_lock);
		bpf_list_push_back(&fq_new_flows, &flow_copy->list_node);
		bpf_spin_unlock(&fq_new_flows_lock);
		fq_new_flow_cnt++;
	}

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn) {
		bpf_kptr_xchg_back(&sflow->flow, flow);
		goto drop;
	}

	skbn->tstamp = skb->tstamp = time_to_send;
	sch->qstats.backlog += qdisc_pkt_len(skb);

	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&flow->lock);
	bpf_rbtree_add(&flow->queue, &skbn->node, skbn_tstamp_less);
	bpf_spin_unlock(&flow->lock);

	flow->qlen++;
	bpf_kptr_xchg_back(&sflow->flow, flow);

	sch->q.qlen++;
	return NET_XMIT_SUCCESS;

drop:
	bpf_qdisc_skb_drop(skb, to_free);
	sch->qstats.drops++;
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_fq_edt_dequeue")
struct sk_buff *BPF_PROG(bpf_fq_edt_dequeue, struct Qdisc *sch)
{
	/* TODO 4: EDT Dequeue and throttling
	 * 1. Read now = bpf_ktime_get_ns() and call fq_check_throttled(now).
	 * 2. Loop through active flows (bpf_for(i, 0, sch->limit)).
	 * 3. Inspect earliest packet in flow->queue (bpf_rbtree_first(&flow->queue)).
	 * 4. If now < skbn->tstamp:
	 *      set flow->time_next_packet = skbn->tstamp;
	 *      call fq_flow_set_throttled(flow);
	 *      continue loop.
	 * 5. If packet can depart:
	 *      pop from rbtree, deduct credit, update backlog, and return skb.
	 * 6. If no packet dequeued and time_next_delayed_flow != ~0ULL:
	 *      call bpf_qdisc_watchdog_schedule(sch, time_next_delayed_flow, 0);
	 */

	/* [YOUR CODE HERE] */
	return NULL;
}

static int fq_remove_flows_in_list(u32 index, void *ctx)
{
	struct bpf_list_node *node;
	struct fq_flow_node *flow;

	bpf_spin_lock(&fq_new_flows_lock);
	node = bpf_list_pop_front(&fq_new_flows);
	bpf_spin_unlock(&fq_new_flows_lock);
	if (!node) {
		bpf_spin_lock(&fq_old_flows_lock);
		node = bpf_list_pop_front(&fq_old_flows);
		bpf_spin_unlock(&fq_old_flows_lock);
		if (!node)
			return 1;
	}

	flow = container_of(node, struct fq_flow_node, list_node);
	bpf_obj_drop(flow);
	return 0;
}

static int fq_remove_map_flow(struct bpf_map *map, u64 *key,
			      struct fq_stashed_flow *sflow, void *ctx)
{
	bpf_map_delete_elem(map, key);
	return 0;
}

SEC("struct_ops/bpf_fq_edt_reset")
void BPF_PROG(bpf_fq_edt_reset, struct Qdisc *sch)
{
	struct unset_throttled_flows_ctx utf_ctx = {
		.unset_all = true,
		.now = 0,
	};

	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
	time_next_delayed_flow = ~0ULL;

	bpf_loop(NUM_FLOWS, fq_unset_throttled_flows, &utf_ctx, 0);

	bpf_loop(NUM_FLOWS, fq_remove_flows_in_list, NULL, 0);
	fq_new_flow_cnt = 0;
	fq_old_flow_cnt = 0;

	bpf_for_each_map_elem(&fq_flows, fq_remove_map_flow, NULL, 0);
}

SEC("struct_ops/bpf_fq_edt_init")
int BPF_PROG(bpf_fq_edt_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 1000;
	fq_new_flow_cnt = 0;
	fq_old_flow_cnt = 0;
	time_next_delayed_flow = ~0ULL;
	return 0;
}

SEC("struct_ops/bpf_fq_edt_destroy")
void BPF_PROG(bpf_fq_edt_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops fq_edt = {
	.enqueue   = (void *)bpf_fq_edt_enqueue,
	.dequeue   = (void *)bpf_fq_edt_dequeue,
	.reset     = (void *)bpf_fq_edt_reset,
	.init      = (void *)bpf_fq_edt_init,
	.destroy   = (void *)bpf_fq_edt_destroy,
	.id        = "bpf_fq_edt",
};
