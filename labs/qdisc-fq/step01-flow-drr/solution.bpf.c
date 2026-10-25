// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

#define ENOMEM 12

char _license[] SEC("license") = "GPL";

#define QUANTUM 1500
#define INITIAL_QUANTUM 2000
#define BOTTLENECK_NS 2000000ULL

#define NUM_FLOWS 1024

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

struct fq_flow_node {
	s64 credit;
	u32 qlen;
	u64 age;
	struct bpf_list_node list_node;
	struct bpf_list_head queue __contains(skb_node, node);
	struct bpf_spin_lock lock;
	struct bpf_refcount refcount;
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

private(A) struct bpf_spin_lock fq_new_flows_lock;
private(A) struct bpf_list_head fq_new_flows __contains(fq_flow_node, list_node);

private(B) struct bpf_spin_lock fq_old_flows_lock;
private(B) struct bpf_list_head fq_old_flows __contains(fq_flow_node, list_node);

private(C) u32 fq_new_flow_cnt;
private(C) u32 fq_old_flow_cnt;
private(C) u64 next_tx_ns;

static void bpf_kptr_xchg_back(void *map_val, void *ptr)
{
	void *ret;

	ret = bpf_kptr_xchg(map_val, ptr);
	if (ret)
		bpf_obj_drop(ret);
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

SEC("struct_ops/bpf_fq_drr_enqueue")
int BPF_PROG(bpf_fq_drr_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct fq_flow_node *flow = NULL, *flow_copy;
	struct fq_stashed_flow *sflow;
	struct skb_node *skbn;
	u64 hash;

	if (sch->q.qlen >= sch->limit)
		goto drop;

	hash = fq_classify(skb);
	sflow = bpf_map_lookup_elem(&fq_flows, &hash);
	if (!sflow) {
		if (fq_new_flow(&sflow, hash) < 0)
			goto drop;
	}

	flow = bpf_kptr_xchg(&sflow->flow, flow);
	if (!flow)
		goto drop;

	if (flow->age != 0) {
		/* Flow is detached */
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

	sch->qstats.backlog += qdisc_pkt_len(skb);
	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&flow->lock);
	bpf_list_push_back(&flow->queue, &skbn->node);
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

SEC("struct_ops/bpf_fq_drr_dequeue")
struct sk_buff *BPF_PROG(bpf_fq_drr_dequeue, struct Qdisc *sch)
{
	struct bpf_list_head *head;
	struct bpf_spin_lock *lock;
	struct bpf_list_node *node;
	struct fq_flow_node *flow;
	struct sk_buff *skb = NULL;
	struct skb_node *skbn;
	u64 now;
	int i;

	if (!sch->q.qlen)
		return NULL;

	now = bpf_ktime_get_ns();
	if (now < next_tx_ns) {
		bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
		return NULL;
	}

	bpf_for(i, 0, sch->limit) {
		bool from_new = false;

		if (fq_new_flow_cnt) {
			head = &fq_new_flows;
			lock = &fq_new_flows_lock;
			from_new = true;
		} else if (fq_old_flow_cnt) {
			head = &fq_old_flows;
			lock = &fq_old_flows_lock;
			from_new = false;
		} else {
			break;
		}

		bpf_spin_lock(lock);
		node = bpf_list_pop_front(head);
		bpf_spin_unlock(lock);
		if (!node)
			break;

		if (from_new)
			fq_new_flow_cnt--;
		else
			fq_old_flow_cnt--;

		flow = container_of(node, struct fq_flow_node, list_node);

		if (flow->credit <= 0) {
			flow->credit += QUANTUM;
			bpf_spin_lock(&fq_old_flows_lock);
			bpf_list_push_back(&fq_old_flows, &flow->list_node);
			bpf_spin_unlock(&fq_old_flows_lock);
			fq_old_flow_cnt++;
			continue;
		}

		bpf_spin_lock(&flow->lock);
		node = bpf_list_pop_front(&flow->queue);
		bpf_spin_unlock(&flow->lock);

		if (!node) {
			if (from_new && fq_old_flow_cnt > 0) {
				bpf_spin_lock(&fq_old_flows_lock);
				bpf_list_push_back(&fq_old_flows, &flow->list_node);
				bpf_spin_unlock(&fq_old_flows_lock);
				fq_old_flow_cnt++;
			} else {
				flow->age = bpf_jiffies64() ?: 1;
				bpf_obj_drop(flow);
			}
			continue;
		}

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		bpf_obj_drop(skbn);

		if (!skb) {
			flow->age = bpf_jiffies64() ?: 1;
			bpf_obj_drop(flow);
			continue;
		}

		flow->credit -= qdisc_pkt_len(skb);
		flow->qlen--;

		if (flow->credit <= 0) {
			flow->credit += QUANTUM;
			bpf_spin_lock(&fq_old_flows_lock);
			bpf_list_push_back(&fq_old_flows, &flow->list_node);
			bpf_spin_unlock(&fq_old_flows_lock);
			fq_old_flow_cnt++;
		} else {
			bpf_spin_lock(lock);
			bpf_list_push_front(head, &flow->list_node);
			bpf_spin_unlock(lock);
			if (from_new)
				fq_new_flow_cnt++;
			else
				fq_old_flow_cnt++;
		}

		next_tx_ns = now + BOTTLENECK_NS;
		sch->q.qlen--;
		sch->qstats.backlog -= qdisc_pkt_len(skb);
		bpf_qdisc_bstats_update(sch, skb);
		return skb;
	}

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

SEC("struct_ops/bpf_fq_drr_reset")
void BPF_PROG(bpf_fq_drr_reset, struct Qdisc *sch)
{
	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
	next_tx_ns = 0;

	bpf_loop(NUM_FLOWS, fq_remove_flows_in_list, NULL, 0);
	fq_new_flow_cnt = 0;
	fq_old_flow_cnt = 0;

	bpf_for_each_map_elem(&fq_flows, fq_remove_map_flow, NULL, 0);
}

SEC("struct_ops/bpf_fq_drr_init")
int BPF_PROG(bpf_fq_drr_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 1000;
	fq_new_flow_cnt = 0;
	fq_old_flow_cnt = 0;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_fq_drr_destroy")
void BPF_PROG(bpf_fq_drr_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops fq_drr = {
	.enqueue   = (void *)bpf_fq_drr_enqueue,
	.dequeue   = (void *)bpf_fq_drr_dequeue,
	.reset     = (void *)bpf_fq_drr_reset,
	.init      = (void *)bpf_fq_drr_init,
	.destroy   = (void *)bpf_fq_drr_destroy,
	.id        = "bpf_fq_drr",
};
