# Step 2: Complete FQ-CoDel (Flow Isolation and 2-List DRR + Per-Flow CoDel)

[日本語](README.md)

## 1. Learning Goals

In this step, you will integrate **Flow Classification** and **Deficit Round Robin (DRR) with two circular lists (`new_flows` and `old_flows`)** into the CoDel AQM from Step 1, completing the full **FQ-CoDel (RFC 8290)** algorithm from scratch.

- For general background on Bufferbloat and queuing dynamics, refer to the [Track README](../README.en.md).

---

## 2. Algorithm and Background

### 2.1 Why CoDel Alone Is Not Sufficient

In Step 1, we observed that single-queue CoDel successfully mitigates Bufferbloat by detecting persistent standing queues and dropping stale packets from the head.

However, in a single FIFO queue, **bulk download flows and interactive lightweight flows (SSH keystrokes, DNS queries, web requests, gaming packets, VoIP) share the same queue**:
- When a bulk transfer saturates the bottleneck link, a newly arriving SSH keystroke packet must wait behind thousands of bytes of bulk data.
- Furthermore, when CoDel triggers a head-drop, an interactive packet might be collateral damage if it happens to be at the head of the shared queue.

### 2.2 The Three Pillars of FQ-CoDel

FQ-CoDel resolves these limitations through three complementary mechanisms:

```text
Packet Arrival
   |
   +---> [1] Flow Classification (IP 5-tuple hash) maps to flow bucket
           |
           +---> [2] 2-List DRR: Sparse flows (new_flows) dequeued with top priority!
           |
           +---> [3] Per-Flow CoDel: Surgical head-drops exclusively on bloated bulk flows!
```

1. **Flow Classification**:
   - Packets are hashed by socket (`sk->__sk_common.skc_hash`) or 5-tuple (`bpf_skb_get_hash(skb)`), isolating each TCP connection into its own queue.
2. **Deficit Round Robin (DRR) Scheduling with Two Lists**:
   - **`new_flows`**: Hosts idle flows that just received packets or newly created flows.
   - **`old_flows`**: Hosts bulk flows that have exhausted their round deficit credit (`quantum = 1500` bytes).
   - The scheduler **always serves `new_flows` before `old_flows`**. Sparse interactive flows (like SSH, DNS, or gaming) arrive intermittently when their flow queue is empty, so they enter `new_flows` and are dequeued immediately—experiencing **zero queue latency (`sojourn < TARGET`) and zero packet loss**!
3. **Per-Flow CoDel AQM**:
   - CoDel state variables (`first_above_time`, `dropping`) are maintained **independently inside each `struct fq_codel_flow`**.
   - Only the bulk flow filling the buffer suffers head-drops. Sparse and well-behaved flows are completely shielded.

---

## 3. Code Architecture and Invariants

### `struct bpf_fq_codel_flow` (Flow Control Block)

```c
struct bpf_fq_codel_flow {
	s64 credit;                 /* DRR credit allowance (bytes, s64) */
	u32 qlen;                   /* Number of packets currently queued */
	u64 age;                    /* 0: active on a list, 1: idle/detached */
	u64 first_above_time;       /* CoDel: observation window expiration */
	u32 drop_count;             /* CoDel: packets dropped in this flow */
	bool dropping;              /* CoDel: dropping state flag */
	struct bpf_list_node list_node; /* Node for new_flows / old_flows */
	struct bpf_list_head queue __contains(skb_node, node); /* Flow skb queue */
	struct bpf_spin_lock lock;  /* Flow queue spinlock */
	struct bpf_refcount refcount; /* BPF refcount for lifecycle management */
};
```

### 2-List DRR State

- `new_flows`: Intrusive list of sparse/new flows. Dequeued with strict priority over `old_flows`.
- `old_flows`: Intrusive list of bulk flows sharing bandwidth round-robin.
- `next_tx_ns`: Pacing timestamp enforcing bottleneck serialization rate (`BOTTLENECK_NS = 4,000,000ULL`).

---

## 3.5 Socratic Design Checkpoints

Before implementing the TODOs in `starter.bpf.c`, reason through these three design questions:

### Q1. Sparse Flow Prioritization: Why maintain two separate lists (`new_flows` and `old_flows`) instead of a single round-robin list?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Consider how a newly arriving single-packet SSH or DNS flow is scheduled when bulk flows are active.
- **Answer**: **To guarantee that intermittent, latency-sensitive sparse flows always jump ahead of backlogged bulk flows.**
  Idle flows entering the active state are placed on `new_flows`, and `.dequeue()` always drains `new_flows` before servicing `old_flows`.

</details>

### Q2. Credit Exhaustion and Demotion: When does a flow transition from `new_flows` to `old_flows`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Track what happens to `flow->credit` as packets are transmitted.
- **Answer**: **When a flow's `credit` drops to `<= 0` after transmitting packets.**
  A sparse flow empties its queue before exhausting its credit and detaches cleanly, whereas a bulk flow with more packets queued exhausts its credit, receives `flow->credit += QUANTUM`, and is appended to the tail of `old_flows`.

</details>

### Q3. Per-Flow CoDel Isolation: Why store `first_above_time` and `dropping` inside `struct bpf_fq_codel_flow` rather than as global variables?

<details>
<summary>Hint and Answer</summary>

- **Hint**: What would happen to a healthy flow if CoDel state were shared globally across all flows?
- **Answer**: **To isolate AQM drops strictly to the specific flow building a standing queue.**
  With per-flow CoDel state, only the congested bulk flow (Flow A) crosses `TARGET_NS` for `INTERVAL_NS` and undergoes head-dropping, while sparse flows (Flow B) experience low sojourn times and zero drops.

</details>

---

## 4. Implementation Steps (TODO List)

Implement the three TODOs inside `bpf_fq_codel_dequeue()` in [`starter.bpf.c`](starter.bpf.c).

### TODO 1: Flow Selection via 2-List DRR

Check `new_flows` first; if empty, fall back to `old_flows`:

```c
bpf_spin_lock(&new_flows_lock);
node = bpf_list_pop_front(&new_flows);
if (node) {
	new_flow_cnt--;
	is_new = true;
}
bpf_spin_unlock(&new_flows_lock);

if (!node) {
	bpf_spin_lock(&old_flows_lock);
	node = bpf_list_pop_front(&old_flows);
	if (node) {
		old_flow_cnt--;
		is_new = false;
	}
	bpf_spin_unlock(&old_flows_lock);
}

if (!node)
	return NULL;
```

### TODO 2: Deficit Credit Check and Flow Rotation

If `flow->credit <= 0`, replenish credit with `QUANTUM`, rotate the flow to the tail of `old_flows`, and return `NULL` (the next dequeue will pick up the next flow):

```c
if (flow->credit <= 0) {
	flow->credit += QUANTUM;
	bpf_spin_lock(&old_flows_lock);
	bpf_list_push_back(&old_flows, &flow->list_node);
	old_flow_cnt++;
	bpf_spin_unlock(&old_flows_lock);
	bpf_fq_codel_flow_drop(flow);
	return NULL;
}
```

### TODO 3: Per-Flow CoDel AQM State Machine

Pop the packet from the flow's internal queue and evaluate per-flow CoDel metrics:

```c
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
		sch->qstats.backlog -= qdisc_pkt_len(skb);
		sch->q.qlen--;
		sch->qstats.drops++;
		bpf_kfree_skb(skb);
		skb = NULL;

		/* Immediately dequeue the next packet from this flow */
		bpf_spin_lock(&flow->lock);
		node = bpf_list_pop_front(&flow->queue);
		bpf_spin_unlock(&flow->lock);
		if (!node)
			goto finish_flow;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		flow->qlen--;
		bpf_obj_drop(skbn);
		if (!skb)
			goto finish_flow;
	}
}
```

---

## 5. Verification with Packetdrill

The test script [`test.pkt`](test.pkt) demonstrates the real-world power of FQ-CoDel:

1. **Flow A (Bulk Transfer)**:
   - Socket 3 transmits 8 UDP packets (1,000 bytes each) at $t = 0$.
   - Packet 1 departs immediately at $t = 0$. Flow A exhausts its credit ($1000 - 1028 \le 0$) and is demoted to `old_flows`, queuing the remaining 7 packets behind the 4 ms bottleneck rate.
2. **Flow B (Sparse Interactive Traffic)**:
   - While Flow A's queue is backlogged, socket 4 transmits a single 500-byte UDP packet at $t = 2\text{ ms}$.
   - Because Flow B arrived from an idle state, it enters `new_flows`.
3. **Sparse Flow Prioritization**:
   - At the next bottleneck slot ($t = 4\text{ ms}$), FQ-CoDel checks `new_flows` first and **transmits Flow B's packet ahead of Flow A's queued packets**!
4. **Isolated CoDel Head-Drops**:
   - Flow A continues transmitting packets P2 through P7 from $t = 8\text{ ms} \sim 28\text{ ms}$.
   - At $t = 32\text{ ms}$, because Flow A's sojourn time exceeds `TARGET_NS (5 ms)` for over `INTERVAL_NS (20 ms)`, **Flow A's packet P8 is surgically head-dropped**.
   - Sparse Flow B experiences zero packet loss and zero queuing delay!

### Running Tests

```sh
# Verify starter fails
./run_test.sh qdisc-fq-codel/step02-fq-codel starter

# Verify solution passes
./run_test.sh qdisc-fq-codel/step02-fq-codel solution
```

Once all tests pass, congratulations on completing Track 6!
