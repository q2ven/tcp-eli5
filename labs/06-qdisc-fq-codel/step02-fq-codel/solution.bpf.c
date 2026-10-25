// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define ENOMEM 12

#define QUANTUM 1000
#define INITIAL_QUANTUM 1000
#define TARGET_NS 5000000ULL
#define INTERVAL_NS 20000000ULL
#define BOTTLENECK_NS 4000000ULL

#define NUM_FLOWS 1024

struct skb_node {
	__u64 enqueue_time;
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

struct bpf_fq_codel_flow {
	s64 credit;
	u32 qlen;
	u64 age;
	u64 first_above_time;
	u32 drop_count;
	bool dropping;
	struct bpf_list_node list_node;
	struct bpf_list_head queue __contains(skb_node, node);
	struct bpf_spin_lock lock;
	struct bpf_refcount refcount;
};

struct fq_stashed_flow {
	struct bpf_fq_codel_flow __kptr *flow;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u64);
	__type(value, struct fq_stashed_flow);
	__uint(max_entries, NUM_FLOWS);
} flows SEC(".maps");

private(A) struct bpf_spin_lock new_flows_lock;
private(A) struct bpf_list_head new_flows __contains(bpf_fq_codel_flow, list_node);

private(B) struct bpf_spin_lock old_flows_lock;
private(B) struct bpf_list_head old_flows __contains(bpf_fq_codel_flow, list_node);

private(C) u32 new_flow_cnt;
private(C) u32 old_flow_cnt;
private(C) u64 next_tx_ns;

static void bpf_kptr_xchg_back(void *map_val, void *ptr)
{
	void *ret;

	ret = bpf_kptr_xchg(map_val, ptr);
	if (ret)
		bpf_obj_drop(ret);
}

static u64 fq_codel_classify(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;

	if (sk && sk->__sk_common.skc_hash)
		return (u64)sk->__sk_common.skc_hash;

	u64 hash = (u64)bpf_skb_get_hash(skb);
	if (!hash)
		hash = 1;
	return hash;
}

static int fq_codel_new_flow(struct fq_stashed_flow **sflow, u64 hash)
{
	struct fq_stashed_flow tmp = {};
	struct bpf_fq_codel_flow *flow;
	int ret;

	flow = bpf_obj_new(typeof(*flow));
	if (!flow)
		return -ENOMEM;

	flow->credit = INITIAL_QUANTUM;
	flow->qlen = 0;
	flow->age = 1;
	flow->first_above_time = 0;
	flow->drop_count = 0;
	flow->dropping = false;

	ret = bpf_map_update_elem(&flows, &hash, &tmp, 0);
	if (ret < 0) {
		bpf_obj_drop(flow);
		return ret;
	}

	*sflow = bpf_map_lookup_elem(&flows, &hash);
	if (!*sflow) {
		bpf_obj_drop(flow);
		return -ENOMEM;
	}

	bpf_kptr_xchg_back(&(*sflow)->flow, flow);
	return 0;
}

SEC("struct_ops/bpf_fq_codel_enqueue")
int BPF_PROG(bpf_fq_codel_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct bpf_fq_codel_flow *flow = NULL, *flow_copy;
	struct fq_stashed_flow *sflow;
	struct skb_node *skbn;
	u32 pkt_len;
	u64 hash;

	if (sch->q.qlen >= sch->limit)
		goto drop;

	hash = fq_codel_classify(skb);
	sflow = bpf_map_lookup_elem(&flows, &hash);
	if (!sflow) {
		if (fq_codel_new_flow(&sflow, hash) < 0)
			goto drop;
	}

	flow = bpf_kptr_xchg(&sflow->flow, flow);
	if (!flow)
		goto drop;

	if (flow->age != 0) {
		/* Flow is detached (empty). Attach to new_flows with fresh credit */
		flow_copy = bpf_refcount_acquire(flow);
		flow_copy->credit = INITIAL_QUANTUM;
		flow_copy->age = 0;

		bpf_spin_lock(&new_flows_lock);
		bpf_list_push_back(&new_flows, &flow_copy->list_node);
		bpf_spin_unlock(&new_flows_lock);
		new_flow_cnt++;
	}

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn) {
		bpf_kptr_xchg_back(&sflow->flow, flow);
		goto drop;
	}

	skbn->enqueue_time = bpf_ktime_get_ns();
	pkt_len = qdisc_pkt_len(skb);

	sch->qstats.backlog += pkt_len;
	sch->q.qlen++;
	flow->qlen++;

	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&flow->lock);
	bpf_list_push_back(&flow->queue, &skbn->node);
	bpf_spin_unlock(&flow->lock);

	bpf_kptr_xchg_back(&sflow->flow, flow);
	return NET_XMIT_SUCCESS;

drop:
	bpf_qdisc_skb_drop(skb, to_free);
	sch->qstats.drops++;
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_fq_codel_dequeue")
struct sk_buff *BPF_PROG(bpf_fq_codel_dequeue, struct Qdisc *sch)
{
	struct bpf_list_node *node, *skb_node_ptr;
	struct bpf_fq_codel_flow *flow;
	struct sk_buff *skb = NULL;
	struct skb_node *skbn;
	u64 now, enqueue_time, sojourn;
	bool from_new;
	int i;

	if (!sch->q.qlen)
		return NULL;

	now = bpf_ktime_get_ns();
	if (now < next_tx_ns) {
		bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
		return NULL;
	}

	bpf_for(i, 0, sch->limit) {
		from_new = false;
		node = NULL;

		if (new_flow_cnt > 0) {
			bpf_spin_lock(&new_flows_lock);
			node = bpf_list_pop_front(&new_flows);
			bpf_spin_unlock(&new_flows_lock);
			if (node) {
				new_flow_cnt--;
				from_new = true;
			}
		}

		if (!node && old_flow_cnt > 0) {
			bpf_spin_lock(&old_flows_lock);
			node = bpf_list_pop_front(&old_flows);
			bpf_spin_unlock(&old_flows_lock);
			if (node)
				old_flow_cnt--;
		}

		if (!node)
			break;

		flow = container_of(node, struct bpf_fq_codel_flow, list_node);

		if (flow->credit <= 0) {
			flow->credit += QUANTUM;
			bpf_spin_lock(&old_flows_lock);
			bpf_list_push_back(&old_flows, &flow->list_node);
			bpf_spin_unlock(&old_flows_lock);
			old_flow_cnt++;
			continue;
		}

		/* Pop front packet from this flow */
		bpf_spin_lock(&flow->lock);
		skb_node_ptr = bpf_list_pop_front(&flow->queue);
		bpf_spin_unlock(&flow->lock);

		if (!skb_node_ptr) {
			/* Flow is empty */
			if (from_new && old_flow_cnt > 0) {
				/* RFC 8290: Empty new_flow can transition to old_flows */
				bpf_spin_lock(&old_flows_lock);
				bpf_list_push_back(&old_flows, &flow->list_node);
				bpf_spin_unlock(&old_flows_lock);
				old_flow_cnt++;
			} else {
				/* Detach flow and release list ref */
				flow->age = 1;
				bpf_obj_drop(flow);
			}
			continue;
		}

		skbn = container_of(skb_node_ptr, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		enqueue_time = skbn->enqueue_time;
		bpf_obj_drop(skbn);

		if (!skb) {
			flow->age = 1;
			bpf_obj_drop(flow);
			continue;
		}

		/* Apply CoDel AQM to this flow */
		sojourn = now - enqueue_time;
		if (sojourn < TARGET_NS) {
			flow->first_above_time = 0;
			flow->dropping = false;
		} else {
			if (flow->first_above_time == 0) {
				flow->first_above_time = now + INTERVAL_NS;
			} else if (now >= flow->first_above_time) {
				flow->dropping = true;
				flow->drop_count++;
				flow->first_above_time = now + INTERVAL_NS;
				sch->qstats.backlog -= qdisc_pkt_len(skb);
				sch->q.qlen--;
				flow->qlen--;
				sch->qstats.drops++;
				bpf_kfree_skb(skb);
				skb = NULL;

				/* Head-drop: pop the next fresher packet from flow */
				bpf_spin_lock(&flow->lock);
				skb_node_ptr = bpf_list_pop_front(&flow->queue);
				bpf_spin_unlock(&flow->lock);

				if (skb_node_ptr) {
					skbn = container_of(skb_node_ptr, struct skb_node, node);
					skb = bpf_kptr_xchg(&skbn->skb, skb);
					bpf_obj_drop(skbn);
				}
			}
		}

		if (skb) {
			u32 len = qdisc_pkt_len(skb);

			flow->credit -= len;
			flow->qlen--;
			sch->qstats.backlog -= len;
			sch->q.qlen--;
			bpf_qdisc_bstats_update(sch, skb);

			/* Re-queue flow */
			if (flow->credit <= 0) {
				flow->credit += QUANTUM;
				bpf_spin_lock(&old_flows_lock);
				bpf_list_push_back(&old_flows, &flow->list_node);
				bpf_spin_unlock(&old_flows_lock);
				old_flow_cnt++;
			} else if (from_new) {
				bpf_spin_lock(&new_flows_lock);
				bpf_list_push_front(&new_flows, &flow->list_node);
				bpf_spin_unlock(&new_flows_lock);
				new_flow_cnt++;
			} else {
				bpf_spin_lock(&old_flows_lock);
				bpf_list_push_front(&old_flows, &flow->list_node);
				bpf_spin_unlock(&old_flows_lock);
				old_flow_cnt++;
			}

			next_tx_ns = now + BOTTLENECK_NS;
			return skb;
		}

		/* If packet was dropped, rotate flow to old_flows */
		if (flow->credit <= 0)
			flow->credit += QUANTUM;
		bpf_spin_lock(&old_flows_lock);
		bpf_list_push_back(&old_flows, &flow->list_node);
		bpf_spin_unlock(&old_flows_lock);
		old_flow_cnt++;
	}

	return NULL;
}

static int fq_codel_remove_flows_in_list(u32 index, void *ctx)
{
	struct bpf_list_node *node;
	struct bpf_fq_codel_flow *flow;

	bpf_spin_lock(&new_flows_lock);
	node = bpf_list_pop_front(&new_flows);
	bpf_spin_unlock(&new_flows_lock);
	if (!node) {
		bpf_spin_lock(&old_flows_lock);
		node = bpf_list_pop_front(&old_flows);
		bpf_spin_unlock(&old_flows_lock);
		if (!node)
			return 1;
	}

	flow = container_of(node, struct bpf_fq_codel_flow, list_node);
	bpf_obj_drop(flow);
	return 0;
}

SEC("struct_ops/bpf_fq_codel_init")
int BPF_PROG(bpf_fq_codel_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 1000;
	new_flow_cnt = 0;
	old_flow_cnt = 0;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_fq_codel_reset")
void BPF_PROG(bpf_fq_codel_reset, struct Qdisc *sch)
{
	bpf_loop(NUM_FLOWS, fq_codel_remove_flows_in_list, NULL, 0);
	new_flow_cnt = 0;
	old_flow_cnt = 0;
	next_tx_ns = 0;
	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
}

SEC("struct_ops")
void BPF_PROG(bpf_fq_codel_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops fq_codel = {
	.enqueue   = (void *)bpf_fq_codel_enqueue,
	.dequeue   = (void *)bpf_fq_codel_dequeue,
	.init      = (void *)bpf_fq_codel_init,
	.reset     = (void *)bpf_fq_codel_reset,
	.destroy   = (void *)bpf_fq_codel_destroy,
	.id        = "bpf_fq_codel",
};
