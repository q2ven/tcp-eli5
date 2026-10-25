# Step 1: Packet-Limited FIFO Qdisc and Watchdog Rate Pacing

[日本語](README.md)

## 1. Goal

In this step, you will implement the Linux kernel's traffic control interface `struct Qdisc_ops` using BPF `struct_ops`, building the most fundamental queueing discipline from scratch: the **Packet-limited FIFO (pfifo)**.

By implementing tail-dropping under a queue capacity limit (`sch->limit = 4`) and bottleneck rate pacing (2 ms per packet) driven by high-resolution watchdog timers, you will master the full lifecycle of packet enqueueing/dequeuing as well as BPF-specific memory ownership and locking invariants.

- For the overall track structure and background concepts, refer to the [Qdisc FIFO Track README](../README.en.md).

---

## 2. How the Algorithm Works (Read This First)

Before diving into the design checkpoints, let's explore how a FIFO queue functions in the kernel and how the Qdisc hooks and data structures operate.

### 2.1 FIFO (First-In, First-Out) and Packet Limits (pfifo)

FIFO is the simplest possible queueing model: packets are buffered in the exact order they arrive and dispatched to the transmission hardware (NIC) in that same order.

When queue capacity is measured in total packet count, the discipline is known as **pfifo (Packet FIFO)**.
In standard Linux, this corresponds to `pfifo` or `pfifo_head_drop`. In this step, we configure the maximum capacity to `sch->limit = 4` packets.

### 2.2 The Tail-Drop Mechanism

When the number of packets currently buffered in the queue (`sch->q.qlen`) reaches `sch->limit`, the buffer has no room for newly arriving packets.

The simplest policy is **Tail-drop**: **immediately drop the newly arriving packet itself**.
When a tail-drop occurs, the Qdisc must:
1. Increment the drop statistics counter `sch->qstats.drops++`.
2. Call the kernel helper `bpf_qdisc_skb_drop(skb, to_free)` to release the packet memory safely.
3. Return `NET_XMIT_DROP` (`0x01`) to inform the network stack that transmission failed.

### 2.3 Bottleneck Serialization Rate and Watchdog Rate Pacing

In real networks, physical links have finite transmission bandwidth. For instance, simulating a bottleneck link that requires 2 ms of serialization delay per packet ($\text{BOTTLENECK NS} = 2,000,000\text{ ns}$) requires the Qdisc to pace packet departures such that at most one packet is released to the NIC every 2 ms.

Linux Qdiscs achieve this via the **Watchdog timer**, backed by high-resolution hardware timers (`hrtimer`):

```text
Time t = 0 ms:
  P1 arrives -> Queue is empty; P1 is transmitted immediately! next_tx_ns set to 2 ms.
  P2..P5 arrive -> Buffered in FIFO (qlen = 4).
  P6 arrives -> qlen (4) >= limit (4) -> Tail-dropped!

Time t = 2 ms: Watchdog timer expires!
  Dequeue P2 and transmit. next_tx_ns updated to 4 ms.

Time t = 4 ms: Watchdog timer expires!
  Dequeue P3 and transmit. next_tx_ns updated to 6 ms.

Time t = 6 ms: Watchdog timer expires!
  Dequeue P4 and transmit. next_tx_ns updated to 8 ms.

Time t = 8 ms: Watchdog timer expires!
  Dequeue P5 and transmit. next_tx_ns updated to 10 ms.
```

### 2.4 Concrete Numerical Trace (6-Packet Burst Sequence)

Here is the exact numerical timeline verified by `test.pkt` in this step (`sch->limit = 4`, `BOTTLENECK_NS = 2 ms`):

| Time | Event | Queue Length (`qlen`) | Backlog (`backlog`) | Transmitted Packet | Details |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0.000s** | Handshake & Init | `0` | `0` B | — | `bpf_pfifo_init()` sets `sch->limit = 4`, `next_tx_ns = 0` |
| **+0.000s** | `write(4, ..., 6000)` (P1..P6 arrive) | `0` | `0` B | **P1** (`1:1001`) | P1 bypasses queue and transmits immediately. `next_tx_ns = now + 2ms` |
| **+0.000s** | P2..P5 Enqueue | `4` | ~4000 B | — | P2, P3, P4, P5 fill the queue (`qlen == 4 == limit`) |
| **+0.000s** | P6 Enqueue attempt | `4` | ~4000 B | — | `qlen >= limit` -> **P6 is tail-dropped** (`drops++`) |
| **+0.002s** | Watchdog timer fires | `3` | ~3000 B | **P2** (`1001:2001`) | Dequeue P2. `next_tx_ns = now + 2ms` |
| **+0.004s** | Watchdog timer fires | `2` | ~2000 B | **P3** (`2001:3001`) | Dequeue P3. `next_tx_ns = now + 2ms` |
| **+0.006s** | Watchdog timer fires | `1` | ~1000 B | **P4** (`3001:4001`) | Dequeue P4. `next_tx_ns = now + 2ms` |
| **+0.008s** | Watchdog timer fires | `0` | `0` B | **P5** (`4001:5001`) | Dequeue P5. Queue becomes empty |

### 2.5 Kernel Structs, Fields, and Helpers

#### (A) Core Fields in `struct Qdisc`

- `sch->limit` (`__u32`): Maximum packet capacity of the queue. Set during `.init`.
- `sch->q.qlen` (`__u32`): Current number of packets buffered. Incremented on enqueue, decremented on dequeue.
- `sch->qstats.drops` (`__u32`): Cumulative number of dropped packets.
- `sch->qstats.backlog` (`__u32`): Total number of bytes currently stored in the queue.
- `sch->flags`: Qdisc operational flags maintained by the kernel.

#### (B) Essential BPF Helpers and kfuncs

- `qdisc_pkt_len(skb)`: Returns total packet transmission length (L2/L3/L4 headers + payload).
- `bpf_qdisc_skb_drop(skb, to_free)`: Adds the packet to the deferred release list for safe freeing.
- `bpf_qdisc_bstats_update(sch, skb)`: Records transmitted packet and byte counters in Qdisc stats.
- `bpf_qdisc_watchdog_schedule(sch, expire, delta_ns)`: Arms the watchdog hrtimer to fire at timestamp `expire` (in nanoseconds).
- `bpf_kfree_skb(skb)`: Directly releases a packet during `.reset`.
- `bpf_obj_new(type)` / `bpf_obj_drop(ptr)`: Allocates and frees objects from BPF memory pools.
- `bpf_kptr_xchg(dst, src)`: Atomically swaps kernel pointers to transfer memory ownership.
- `bpf_list_push_back(head, node)` / `bpf_list_pop_front(head)`: Inserts/removes items in an intrusive BPF linked list.

---

## 3. Socratic Design Checkpoints

Before implementing the code in `starter.bpf.c`, reason through these four design questions based on the principles explained in Section 2.

### Q1. Memory Ownership: Why must `qdisc_pkt_len(skb)` be called BEFORE `bpf_kptr_xchg`?

In `bpf_pfifo_enqueue()`, we invoke `u32 pkt_len = qdisc_pkt_len(skb);` before storing `skb` into `skbn`. What happens if you try to read `qdisc_pkt_len(skb)` after executing `bpf_kptr_xchg(&skbn->skb, skb)`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: What happens to the local pointer variable `skb` after `bpf_kptr_xchg` succeeds?
- **Answer**: **The BPF verifier will reject the program with a memory invalidation error.**
  Once `bpf_kptr_xchg(&skbn->skb, skb)` executes, ownership of the `skb` is transferred into the node, and the local variable `skb` in the caller's frame is invalidated. Attempting to dereference an invalidated pointer is strictly illegal in BPF.

</details>

### Q2. Locking Invariants: Why must you never call `bpf_kptr_xchg` or `bpf_obj_drop` while holding `bpf_spin_lock`?

In `.dequeue()`, why do we pop the node from the list under `bpf_spin_lock`, immediately unlock, and only then extract `skb` and drop `skbn`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Consider the rules enforced by the BPF verifier regarding spinlock critical sections.
- **Answer**: **BPF spinlock critical sections must be extremely short and cannot perform memory allocations, memory deallocations (`bpf_obj_drop`), or kptr swaps.**
  Holding a spinlock while freeing memory or calling complex kfuncs can violate lock ordering and non-sleepable requirements. Violating this will cause verification to fail immediately.

</details>

### Q3. Tail-Drop Policy: What should `.enqueue()` do when `sch->q.qlen >= sch->limit`?

What are the three required steps when the queue capacity limit is hit?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Think about statistics, memory management, and return code.
- **Answer**:
  ```c
  if (sch->q.qlen >= sch->limit) {
      sch->qstats.drops++;
      bpf_qdisc_skb_drop(skb, to_free);
      return NET_XMIT_DROP;
  }
  ```
  Increment `sch->qstats.drops`, drop the packet safely using `bpf_qdisc_skb_drop(skb, to_free)`, and return `NET_XMIT_DROP`.

</details>

### Q4. Rate Limiting in Dequeue: How should `.dequeue()` behave when the current time is earlier than `next_tx_ns`?

What should `.dequeue()` do when `now < next_tx_ns`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: How do we tell the kernel to wake this Qdisc back up at the exact right moment?
- **Answer**:
  ```c
  if (now < next_tx_ns) {
      bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
      return NULL;
  }
  ```
  Call `bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0)` to register an hrtimer wakeup for `next_tx_ns` and return `NULL`.

</details>

---

## 4. Implementation Details (`solution.bpf.c`)

Let's review the key sections of [`solution.bpf.c`](solution.bpf.c).

### 4.1 Data Structures & State Variables

```c
#define BOTTLENECK_NS 2000000ULL /* 2 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_fifo_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
static __u64 next_tx_ns;
```

- `skb_node`: Wraps `skb` with `__kptr` alongside an intrusive `bpf_list_node`.
- `private(A)`: Associates the spinlock and list head in the same BTF data section `.data.A`.
- `next_tx_ns`: Earliest nanosecond timestamp at which the next packet may be transmitted.

### 4.2 `bpf_pfifo_init()` — Initialisation

```c
SEC("struct_ops/bpf_pfifo_init")
int BPF_PROG(bpf_pfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 4;
	next_tx_ns = 0;
	return 0;
}
```

- Initialises `sch->limit = 4` and clears `next_tx_ns`.

### 4.3 `bpf_pfifo_enqueue()` — Enqueue & Tail Drop (TODO 1–4)

```c
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
```

- Reads `pkt_len` before transferring `skb` ownership.
- Inserts `skbn` into the list under `q_fifo_lock` and updates backlog statistics.

### 4.4 `bpf_pfifo_dequeue()` — Dequeue & Pacing (TODO 5–9)

```c
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
```

- Enforces pacing by scheduling the watchdog if `now < next_tx_ns`.
- Pops from list under lock, extracts `skb` and drops `skbn` outside the lock.
- Advances `next_tx_ns` by `BOTTLENECK_NS` (2 ms) and decrements `qlen`.

### 4.5 `bpf_pfifo_reset()` — Queue Flushing

```c
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
```

- Drains any lingering packets using `bpf_for` and `bpf_kfree_skb()`.

---

## 5. Packetdrill Test Analysis (`test.pkt`) & Verification

### 5.1 Test Flow Walkthrough

[`test.pkt`](test.pkt) executes the following sequence:

1. **Setup**: Replaces the root Qdisc on `tun0` with `bpf_pfifo` and disables TSO/GSO with `ethtool -K tun0 tso off gso off`.
2. **Handshake**: Completes passive TCP handshake with MSS = 1000 bytes.
3. **Burst Write**: Invokes `write(4, ..., 6000)` to send 6 segments simultaneously (`cwnd = 10`):
   - Segment 1 (`1:1001`) departs immediately at `+0.00`.
   - Segments 2..5 (`1001:2001`, `2001:3001`, `3001:4001`, `4001:5001`) queue into FIFO and depart spaced by 2 ms (`+0.002`).
   - Segment 6 (`5001:6001`) is tail-dropped because `sch->q.qlen >= 4`.
4. **Drop Verification**: Runs `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"` to confirm that the drop was recorded by the kernel.

### 5.2 Why `starter.bpf.c` Fails vs `solution.bpf.c` Passes

- **`starter.bpf.c` fails**: `.enqueue()` unconditionally drops every packet and `.dequeue()` returns `NULL`, causing the handshake to fail.
- **`solution.bpf.c` passes**: Correctly implements queue management, tail-dropping, and watchdog-driven pacing.

### 5.3 Test Execution Commands

```sh
# Verify starter failure
../../run_test.sh 04-qdisc-fifo/step01-pfifo starter

# Verify solution success
../../run_test.sh 04-qdisc-fifo/step01-pfifo solution
```
