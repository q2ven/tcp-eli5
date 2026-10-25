# Step 2: 2-Band Priority + Byte-Limited FIFO Qdisc

[日本語](README.md)

## 1. Goal

In this step, you will extend the basic FIFO from Step 1 into a **2-Band Priority + Byte-limited FIFO Qdisc (`bpf_bfifo`)** combining a byte-based backlog limit (bfifo) with strict priority scheduling.

You will enforce a byte ceiling (`BYTE_LIMIT = 3500` bytes) on normal bulk traffic to prevent bufferbloat while allowing high-priority control packets (`SO_PRIORITY = 7` / `TC_PRIO_CONTROL`) to bypass the byte limit and jump ahead of already-queued bulk packets, eliminating head-of-line blocking.

- For the overall track structure and background concepts, refer to the [Qdisc FIFO Track README](../README.en.md).

---

## 2. How the Algorithm Works (Read This First)

Before looking at the design checkpoints, let's explore why packet limits alone are insufficient and how priority scheduling reduces latency for critical packets.

### 2.1 Why Packet Limits (pfifo) Are Insufficient: The Need for Byte Limits (bfifo)

The packet-count limit (`sch->limit`) used in Step 1 has a significant shortcoming: **packets vary widely in physical size**:

- Pure TCP ACKs / SYNs: ~**64 bytes**
- Standard MTU TCP data packets: ~**1500 bytes** (over 23 times larger!)

If a queue capacity is set to 1000 packets, buffering 1000 small ACKs consumes only 64 KB of memory and drains in negligible time. But buffering 1000 full-sized data packets consumes 1.5 MB of memory and introduces 23 times more queueing delay!

To solve this, **bfifo (Byte FIFO)** limits queue capacity by the **total bytes currently buffered (Backlog)** rather than packet count.
In this step, we enforce `BYTE_LIMIT = 3500` bytes on normal traffic, dropping packets if `sch->qstats.backlog + pkt_len > BYTE_LIMIT`.

### 2.2 2-Band Strict Priority Scheduling

Treating all packets equally causes **Head-of-Line (HoL) Blocking**: latency-sensitive packets (DNS queries, TCP SYNs, SSH keystrokes) stuck behind a bulk file transfer suffer hundreds of milliseconds of bufferbloat delay.

**Priority Queueing (PRIO)** solves this by maintaining separate priority queues:
- **`q_prio` (Priority Queue)**: Holds control and high-priority packets. Bypasses `BYTE_LIMIT` (bounded only by total packet limit `sch->limit = 8`).
- **`q_normal` (Normal Queue)**: Holds bulk traffic, subject to `BYTE_LIMIT = 3500` bytes.

```text
Packet Arrival (.enqueue)
        |
        +---> is_prio == true  ---> Push to q_prio (bypasses BYTE_LIMIT)
        |
        +---> is_prio == false ---> backlog + pkt_len > 3500 ?
                                        ├── Yes -> Tail-drop
                                        └── No  -> Push to q_normal

Packet Departure (.dequeue)
        |
        +---> Any packet in q_prio?
                  ├── Yes -> Pop from q_prio and transmit! (Priority preemption)
                  └── No  -> Pop from q_normal and transmit
```

### 2.3 Concrete Numerical Trace (Flow A vs Priority Flow B Preemption)

Let's trace the exact scenario tested by `test.pkt` (`BYTE_LIMIT = 3500` bytes, `BOTTLENECK_NS = 4 ms`):

1. **Normal Flow A (`fd = 4`) bursts 5000 bytes (5 segments)**:
   - Each segment has `pkt_len` ~ 1066 bytes (MSS 1000 + 66 bytes headers).
   - **Seg 1** (`1:1001`): Queue empty at arrival -> Transmitted immediately (`t = 0 ms`), `next_tx_ns = 4 ms`.
   - **Seg 2** (`1001:2001`): Queued in `q_normal`, `backlog = 1066` ($\le 3500$).
   - **Seg 3** (`2001:3001`): Queued in `q_normal`, `backlog = 2132` ($\le 3500$).
   - **Seg 4** (`3001:4001`): Queued in `q_normal`, `backlog = 3198` ($\le 3500$).
   - **Seg 5** (`4001:5001`): $3198 + 1066 = 4264 > 3500$ -> **Tail-dropped due to `BYTE_LIMIT`!**
2. **Immediately (`t = 0 ms`), Priority Flow B (`fd = 6`, `SO_PRIORITY = 7`) sends 500 bytes (1 segment)**:
   - `is_prio` is true -> Bypasses `BYTE_LIMIT` and enters `q_prio`.
3. **Preemption in Departure Order**:
   - **`t = 0 ms`**: Flow A Seg 1 is transmitting on wire.
   - **`t = 4 ms`**: Watchdog timer expires. `.dequeue` checks `q_prio` first -> **Flow B's packet (`1:501`) is transmitted BEFORE Flow A's queued Seg 2–4!**
   - **`t = 8 ms`**: `q_prio` is now empty. Dequeue serves Flow A Seg 2 (`1001:2001`) from `q_normal`.
   - **`t = 12 ms`**: Dequeue serves Flow A Seg 3 (`2001:3001`).
   - **`t = 16 ms`**: Dequeue serves Flow A Seg 4 (`3001:4001`).

| Time | Event | `q_prio` | `q_normal` | Transmitted Packet | Details |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0.000s** | Flow A burst (5 pkts) | Empty | Seg 2, 3, 4 (3198 B) | **Flow A Seg 1** (`1:1001`) | Seg 5 tail-dropped |
| **+0.000s** | Flow B send (500 B, prio) | Prio 1 (566 B) | Seg 2, 3, 4 (3198 B) | — | Flow B joins `q_prio` |
| **+0.004s** | Watchdog timer fires | Empty | Seg 2, 3, 4 (3198 B) | **Flow B** (`1:501`) | **Jumps ahead of Flow A!** |
| **+0.008s** | Watchdog timer fires | Empty | Seg 3, 4 (2132 B) | **Flow A Seg 2** (`1001:2001`) | Serviced from `q_normal` |
| **+0.012s** | Watchdog timer fires | Empty | Seg 4 (1066 B) | **Flow A Seg 3** (`2001:3001`) | Serviced from `q_normal` |
| **+0.016s** | Watchdog timer fires | Empty | Empty (0 B) | **Flow A Seg 4** (`3001:4001`) | Both queues empty |

### 2.4 Kernel Structs, Fields, and Helpers

- `TC_PRIO_CONTROL`: Kernel priority class constant (`7`), configured on a socket via `setsockopt(fd, SOL_SOCKET, SO_PRIORITY, [7], 4)`.
- `TC_PRIO_MAX`: Priority bitmask (`15`).
- `skb->priority`: Packet priority propagated from the sending socket. Tested via `(skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL`.
- `private(A) struct bpf_spin_lock q_lock`: Protects multiple list heads (`q_prio` and `q_normal`) inside the same section atomically.

---

## 3. Socratic Design Checkpoints

### Q1. Byte-Limit Calculation: Why check `backlog + pkt_len > BYTE_LIMIT`?

Why do we add `pkt_len` to `sch->qstats.backlog` rather than merely checking `sch->qstats.backlog > BYTE_LIMIT`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Consider the backlog size *after* the packet is accepted.
- **Answer**: **To test the post-admission backlog against the limit.**
  Checking only `backlog > BYTE_LIMIT` would allow an incoming 1500-byte packet to be accepted even when the queue already sits at 3499 bytes, bloating total backlog to 4999 bytes and violating the 3500-byte limit.

</details>

### Q2. Priority Masking: Why apply `& TC_PRIO_MAX`?

Why mask with `& TC_PRIO_MAX` rather than directly checking `skb->priority == TC_PRIO_CONTROL`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Does `skb->priority` contain bits beyond the basic priority class?
- **Answer**: **To strip away higher-order traffic-class or queue-mapping bits.**
  In the Linux TC subsystem, only the lower 4 bits (0–15) encode the standard priority band.

</details>

### Q3. Strict Priority Dequeue Logic: How to pop `q_prio` before `q_normal`?

How do you implement "try `q_prio` first, then fall back to `q_normal`" under a single BPF spinlock?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Check if the first `bpf_list_pop_front()` returned NULL.
- **Answer**:
  ```c
  bpf_spin_lock(&q_lock);
  node = bpf_list_pop_front(&q_prio);
  if (!node)
      node = bpf_list_pop_front(&q_normal);
  bpf_spin_unlock(&q_lock);
  ```
  This cleanly evaluates both queues in strict priority order within a single 5-line critical section.

</details>

---

## 4. Implementation Details (`solution.bpf.c`)

Review [`solution.bpf.c`](solution.bpf.c).

### 4.1 Data Structures & Two Priority Bands

```c
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
```

### 4.2 `bpf_bfifo_enqueue()` — Priority Demux & Byte Limiting (TODO 1–5)

```c
SEC("struct_ops/bpf_bfifo_enqueue")
int BPF_PROG(bpf_bfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct skb_node *skbn;
	bool is_prio;
	u32 pkt_len;

	if (sch->q.qlen >= sch->limit)
		goto drop;

	is_prio = (skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL;
	pkt_len = qdisc_pkt_len(skb);

	if (!is_prio && (sch->qstats.backlog + pkt_len > BYTE_LIMIT))
		goto drop;

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn)
		goto drop;

	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&q_lock);
	if (is_prio)
		bpf_list_push_back(&q_prio, &skbn->node);
	else
		bpf_list_push_back(&q_normal, &skbn->node);
	bpf_spin_unlock(&q_lock);

	sch->q.qlen++;
	sch->qstats.backlog += pkt_len;
	return NET_XMIT_SUCCESS;

drop:
	sch->qstats.drops++;
	bpf_qdisc_skb_drop(skb, to_free);
	return NET_XMIT_DROP;
}
```

- Normal packets exceeding `BYTE_LIMIT` are dropped.
- High-priority packets bypass `BYTE_LIMIT` and enter `q_prio`.

### 4.3 `bpf_bfifo_dequeue()` — Strict Priority Scheduling (TODO 6–9)

```c
SEC("struct_ops/bpf_bfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_bfifo_dequeue, struct Qdisc *sch)
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

	bpf_spin_lock(&q_lock);
	node = bpf_list_pop_front(&q_prio);
	if (!node)
		node = bpf_list_pop_front(&q_normal);
	bpf_spin_unlock(&q_lock);
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
```

- Always pops from `q_prio` first, only serving `q_normal` when `q_prio` is completely drained.

---

## 5. Packetdrill Test Analysis (`test.pkt`) & Verification

### 5.1 Test Flow Walkthrough

[`test.pkt`](test.pkt) verifies:

1. **Dual Flow Setup**: Establishes Flow A (`fd = 4`, standard priority) and Flow B (`fd = 6`, `SO_PRIORITY = 7`).
2. **Flow A Burst & Byte-Limit Tail-Drop (Line 29)**: `write(4, ..., 5000)` sends 5 segments. Segments 1–4 are admitted; Segment 5 exceeds `BYTE_LIMIT = 3500` and is dropped.
3. **Priority Flow B Preemption (Lines 33–39)**: `write(6, ..., 500)` puts a high-priority packet into `q_prio`. At `+0.004` (4 ms later), Flow B's 500-byte packet is dequeued **BEFORE Flow A's remaining queued segments (1001:2001, 2001:3001, 3001:4001)**.
4. **Resumption of Bulk Flow & Drop Check (Lines 42–48)**: Flow A segments drain sequentially, and `tc -s qdisc show dev tun0` confirms `dropped >= 1`.

### 5.2 Test Execution Commands

```sh
# Verify starter failure
../../run_test.sh 04-qdisc-fifo/step02-bfifo-prio starter

# Verify solution success
../../run_test.sh 04-qdisc-fifo/step02-bfifo-prio solution
```
