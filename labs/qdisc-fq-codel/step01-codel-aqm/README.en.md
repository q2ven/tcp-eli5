# Step 1: Single-Queue CoDel (Controlled Delay) AQM

[日本語](README.md)

## 1. Learning Goals

In this step, you will implement Linux kernel `struct Qdisc_ops` using BPF `struct_ops` to build the foundational active queue management (AQM) algorithm of **CoDel (RFC 8289)** from scratch.

- For general background on Bufferbloat and queuing dynamics, refer to the [Track README](../README.en.md).

---

## 2. Algorithm and Background

### 2.1 Measuring Sojourn Time Instead of Queue Length

Traditional queue management schemes like Tail-Drop or RED (Random Early Detection) drop packets based on instantaneous queue occupancy (packet or byte counts).

However, queue length is an unreliable indicator of delay:
- On a 10 Gbps link, a queue of 100 packets drains in just 0.12 ms.
- On a 1 Mbps link, that same 100-packet queue takes 1.2 seconds to drain.

Because identical packet counts produce vastly different delays across different link speeds, CoDel measures the **time packets actually spend waiting in the queue (sojourn time)**:

$$\text{sojourn} = t_{\text{dequeue}} - t_{\text{enqueue}}$$

### 2.2 Distinguishing Good Queues from Bad Queues

- **Good Queue (Transient Burst)**:
  - During web browsing or bursty application activity, bursts of packets arrive simultaneously.
  - On healthy networks, these packets are absorbed into buffers and drained rapidly. While sojourn time spikes temporarily, it quickly falls back to zero.
- **Bad Queue (Persistent Standing Queue)**:
  - When a TCP sender continuously drives large amounts of traffic exceeding bottleneck bandwidth, the queue never drains.
  - Sojourn time remains elevated over an extended duration.

CoDel determines whether a standing queue exists by tracking whether **the minimum sojourn time over an observation window (`INTERVAL`) has persistently exceeded a target delay (`TARGET`)**.

### 2.3 CoDel State Transitions

CoDel is governed by two primary parameters:
- **`TARGET_NS`**: Target sojourn time (standard 5 ms = $$5,000,000\text{ ns}$$).
- **`INTERVAL_NS`**: Sliding observation window (standard 100 ms, shortened to $$20\text{ ms} = 20,000,000\text{ ns}$$ in this lab).

```mermaid
flowchart TD
    Start["Pop packet from queue (bpf_codel_dequeue)"] --> Calc["Compute sojourn = now - enqueue_time"]
    Calc --> CondTarget{"sojourn < TARGET_NS ?"}

    CondTarget -->|Yes: Normal (Good Queue)| Reset["first_above_time = 0\ndropping = false\n(Proceed to send)"]
    CondTarget -->|No: High delay| CheckTimer{"first_above_time == 0 ?"}

    CheckTimer -->|Yes: First time exceeding TARGET| ArmTimer["first_above_time = now + INTERVAL_NS\n(Start observation window)"]
    CheckTimer -->|No: Timer running| CheckExpire{"now >= first_above_time ?"}

    CheckExpire -->|No: INTERVAL not yet elapsed| NormalSend["Possibly transient: send packet"]
    CheckExpire -->|Yes: Delay above TARGET for entire INTERVAL!| DropState["dropping = true\nHead-Drop this stale packet!\nImmediately dequeue and send next packet"]

    Reset --> SendWire["Transmit packet to wire"]
    ArmTimer --> SendWire
    NormalSend --> SendWire
    DropState --> SendWire
```

1. **When `sojourn < TARGET_NS`**:
   - Queuing delay is healthy. Reset `first_above_time = 0` and `dropping = false`, and transmit normally.
2. **When `sojourn >= TARGET_NS`**:
   - If `first_above_time == 0` (initial excursion), set `first_above_time = now + INTERVAL_NS` to start observing without dropping.
   - If `now >= first_above_time`, the queue has failed to drain during the entire interval, confirming a persistent standing queue.
   - Transition to `dropping = true`, drop this head packet via `bpf_kfree_skb(skb)` (**Head-Drop**), and dequeue the next packet to notify the sender via SACK/duplicate ACKs immediately.

---

## 3. Code Architecture and Invariants

### `struct skb_node` and BPF Linked List

```c
struct skb_node {
	__u64 enqueue_time;
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock codel_lock;
private(A) struct bpf_list_head codel_queue __contains(skb_node, node);
```

- Packets are queued in an intrusive linked list (`bpf_list_head` / `bpf_list_node`).
- Enqueue stamps `skbn->enqueue_time = bpf_ktime_get_ns();` and dequeue evaluates queue latency.

### State Variables

- `next_tx_ns`: Target completion time for the current transmission (`BOTTLENECK_NS = 4,000,000ULL`). Paces dequeue to simulate a bottleneck link.
- `first_above_time`: Expiration timestamp (`now + INTERVAL_NS`) set when sojourn time first crosses `TARGET_NS`.
- `dropping`: Boolean flag indicating whether the AQM is currently in the dropping state.
- `drop_count`: Counter tracking the number of head-dropped packets.

---

## 3.5 Socratic Design Checkpoints

Before implementing the TODOs in `starter.bpf.c`, reason through these three design questions:

### Q1. Measuring Sojourn Time: Why measure `now - enqueue_time` instead of queue length (`qlen`)?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Compare the queuing delay of 100 packets on a 10 Gbps link versus a 1 Mbps link.
- **Answer**: **Queue length does not directly translate to queuing delay because link drain rates vary by orders of magnitude.**
  By stamping `skbn->enqueue_time = bpf_ktime_get_ns()` at enqueue and computing `sojourn = now - enqueue_time` at dequeue, CoDel directly measures actual packet waiting time independent of link bandwidth.

</details>

### Q2. Observation Window (`INTERVAL_NS`): Why not drop immediately as soon as `sojourn >= TARGET_NS`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Recall the distinction between a transient burst (Good Queue) and a persistent standing queue (Bad Queue).
- **Answer**: **To avoid penalizing short-lived, transient packet bursts that buffers are meant to absorb.**
  When `sojourn >= TARGET_NS` is first observed (`first_above_time == 0`), CoDel sets `first_above_time = now + INTERVAL_NS` and lets the packet pass. It only enters `dropping = true` and performs a head-drop if `sojourn >= TARGET_NS` persists until `now >= first_above_time`.

</details>

### Q3. Head-Drop Mechanics: After freeing the stale head packet with `bpf_kfree_skb(skb)`, why must `.dequeue()` immediately pop and return the next packet?

<details>
<summary>Hint and Answer</summary>

- **Hint**: What happens to link utilization and receiver SACK generation if `.dequeue()` returns `NULL` instead?
- **Answer**: **To remain work-conserving and immediately deliver the subsequent packet so the receiver generates a SACK / duplicate ACK without delay.**
  Transmitting the fresher second packet right away notifies the TCP sender of the drop within 1 RTT.

</details>

---

## 4. Implementation Steps (TODO List)

Implement the two TODOs in `bpf_codel_dequeue()` inside [`starter.bpf.c`](starter.bpf.c).

### TODO 1: Bottleneck Link Rate Control (Watchdog Timer)

Check if the current time is earlier than `next_tx_ns`. If so, schedule the watchdog timer and return `NULL`:

```c
now = bpf_ktime_get_ns();
if (now < next_tx_ns) {
	bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	return NULL;
}
```

### TODO 2: CoDel AQM State Machine

After popping the packet from the linked list, calculate `sojourn = now - enqueue_time` and apply CoDel dropping logic:

```c
sojourn = now - enqueue_time;
if (sojourn < TARGET_NS) {
	first_above_time = 0;
	dropping = false;
} else {
	if (first_above_time == 0) {
		first_above_time = now + INTERVAL_NS;
	} else if (now >= first_above_time) {
		dropping = true;
		drop_count++;
		sch->qstats.backlog -= qdisc_pkt_len(skb);
		sch->q.qlen--;
		sch->qstats.drops++;
		bpf_kfree_skb(skb);
		skb = NULL;

		/* Immediately dequeue and transmit the next packet */
		bpf_spin_lock(&codel_lock);
		node = bpf_list_pop_front(&codel_queue);
		bpf_spin_unlock(&codel_lock);
		if (!node)
			return NULL;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		bpf_obj_drop(skbn);
		if (!skb)
			return NULL;
	}
}
```

---

## 5. Verification with Packetdrill

The test script [`test.pkt`](test.pkt) automatically tests:
1. Installing the qdisc: `tc qdisc replace dev tun0 root bpf_codel`.
2. Establishing a TCP connection and transmitting 10 segments (1000 bytes each, 10 KB total).
3. Bottleneck serialization pacing (4 ms per packet) causes packets to queue up.
4. As queue sojourn exceeds `TARGET_NS (5 ms)` and sustains past `INTERVAL_NS (20 ms)`, CoDel head-drops a stale packet.
5. The receiver immediately detects the gap and returns SACK `8001:10001`, prompting the sender to throttle its congestion window.

### Running Tests

```sh
# Verify starter fails (as it lacks drop logic)
./run_test.sh qdisc-fq-codel/step01-codel-aqm starter

# Verify solution passes
./run_test.sh qdisc-fq-codel/step01-codel-aqm solution
```

After completing Step 1, advance to [`step02-fq-codel/`](../step02-fq-codel/README.en.md) to implement multi-flow isolation and sparse flow prioritization!
