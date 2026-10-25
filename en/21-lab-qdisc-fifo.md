# 21. Hands-on (4): Building FIFO and Priority Qdiscs with BPF and packetdrill


In this track, you will build Linux kernel **Qdiscs (Queueing Disciplines)** from scratch using BPF `struct_ops` (`struct Qdisc_ops`).

While the TCP congestion control algorithms explored in earlier tracks (Reno / CUBIC / BBR) determine the maximum amount of unacknowledged data in flight (`cwnd`) or the sender-side pacing rate for an individual socket, a Qdisc operates at the **Traffic Control (TC)** layer right above the physical NIC. It governs **how packets originating from multiple sockets are buffered, ordered, paced, and dropped before being submitted to the hardware transmit ring**.

---

## 1. What Is a Qdisc (Queueing Discipline)?

In the Linux network output path, a Qdisc sits at the boundary between **Layer 4 (Transport layer: TCP/UDP)** and **Layer 2/1 (Device driver / TX ring buffer / NIC hardware)**.

```text
+--------------------------------------------------------------+
| Application Layer (write / sendmsg)                          |
+--------------------------------------------------------------+
                              |
                              v
+--------------------------------------------------------------+
| Transport Layer (TCP / UDP)                                  |
| - Socket write queue (sk_write_queue)                        |
| - Congestion Control (determines cwnd & pacing rate)         |
+--------------------------------------------------------------+
                              |
                              v dev_queue_xmit()
+--------------------------------------------------------------+
| Traffic Control Layer (Qdisc: struct Qdisc / struct Qdisc_ops) |
| - Packet buffering and tail-drop management (.enqueue)       |
| - Packet scheduling, pacing, and dequeuing (.dequeue)        |
+--------------------------------------------------------------+
                              |
                              v sch_direct_xmit()
+--------------------------------------------------------------+
| NIC Driver & Hardware (TX Ring Buffer / DMA)                 |
+--------------------------------------------------------------+
```

### Why Do We Need Qdiscs?

- **Absorbing Rate Discrepancies**: Applications and TCP stacks can generate packet bursts much faster than the physical wire rate or the NIC TX ring can accept. The Qdisc temporarily stores these bursts and feeds them into the NIC at link speed.
- **Scheduling and Quality of Service (QoS)**: Latency-sensitive traffic (such as DNS requests, TCP SYNs, SSH keystrokes, or VoIP) can be scheduled ahead of bulky file transfers.
- **Bufferbloat Mitigation and Congestion Signals**: When queues become full, the Qdisc decides whether to drop packets from the tail, drop from the head, or inject ECN marks to signal upcoming congestion.

---

## 2. Linux Kernel Qdisc Architecture & BPF `struct_ops`

Starting with Linux 6.x, developers can write programmable Qdiscs safely using BPF `struct_ops` instead of compiling out-of-tree kernel C modules.

A BPF Qdisc implements five core hooks in `struct Qdisc_ops`:

| Hook Name | Signature | Role & Trigger |
| :--- | :--- | :--- |
| `.enqueue` | `int (*enqueue)(struct sk_buff *skb, struct Qdisc *sch, struct bpf_sk_buff_ptr *to_free)` | Invoked whenever an upstream layer passes a packet via `dev_queue_xmit()`. Returns `NET_XMIT_SUCCESS` if buffered, or calls `bpf_qdisc_skb_drop(skb, to_free)` and returns `NET_XMIT_DROP` when queue limits are exceeded. |
| `.dequeue` | `struct sk_buff *(*dequeue)(struct Qdisc *sch)` | Invoked by the kernel when the NIC TX ring is ready or when a watchdog timer expires. Returns the next `skb` to transmit, or `NULL` if no packets are ready or if transmission is throttled. |
| `.init` | `int (*init)(struct Qdisc *sch, struct nlattr *opt, struct netlink_ext_ack *extack)` | Invoked upon attaching the Qdisc to an interface (`tc qdisc add/replace`). Initialises queue boundaries (`sch->limit`) and state variables. |
| `.reset` | `void (*reset)(struct Qdisc *sch)` | Invoked when the interface goes down or is reset. Flushes all packets currently buffered in the queue using `bpf_kfree_skb(skb)` and resets statistics. |
| `.destroy` | `void (*destroy)(struct Qdisc *sch)` | Invoked when the Qdisc is completely detached and destroyed. Cleans up dynamically allocated memory and resources. |

---

## 3. Primitives for Safe BPF Qdisc Development

To safely manage kernel packet structures (`struct sk_buff`) inside BPF programs, the kernel provides specialized kfuncs and type tags:

### 3.1 BPF `kptr` (`__kptr`) and Ownership Transfer

Because `skb` pointers represent kernel memory, BPF programs use the `__kptr` type tag and `bpf_kptr_xchg()` to manage **safe ownership transfer**:

```c
struct skb_node {
    struct sk_buff __kptr *skb;
    struct bpf_list_node node;
};
```

- `skb = bpf_kptr_xchg(&skbn->skb, skb);`: Transfers ownership of the inbound `skb` into the allocated node `skbn`. If a previous pointer occupied the slot, the old pointer is returned and must be dropped with `bpf_qdisc_skb_drop(skb, to_free)`.
- `skb = bpf_kptr_xchg(&skbn->skb, NULL);`: Extracts ownership of `skb` out of the node during dequeue.

### 3.2 BPF Intrusive Lists & Spin Lock Rules

Packet queues are maintained using intrusive linked lists (`struct bpf_list_head` / `struct bpf_list_node`):

```c
private(A) struct bpf_spin_lock q_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
```

- **Strict Locking Invariants**: The BPF verifier strictly inspects code holding a `bpf_spin_lock`. **Calling `bpf_kptr_xchg()`, `bpf_obj_drop()`, or performing heap allocations while holding a `bpf_spin_lock` is strictly forbidden**. You must only perform list operations (`bpf_list_push_back()` / `bpf_list_pop_front()`) under the lock, and release the lock immediately before calling `bpf_kptr_xchg()`.

### 3.3 Precise Pacing with the Watchdog Timer

To enforce bottleneck bandwidth limits or pace packet departures, the Qdisc leverages high-resolution hardware timers (hrtimers) via the **Watchdog timer**:

```c
__u64 now = bpf_ktime_get_ns();
if (now < next_tx_ns) {
    bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
    return NULL;
}
```

- When the current timestamp `now` is earlier than the next allowable transmission time `next_tx_ns`, calling `bpf_qdisc_watchdog_schedule()` programs the kernel timer and `.dequeue` returns `NULL`.
- When the timer fires, the kernel re-enters `.dequeue()` automatically.

---

## 4. Track Outline & Roadmap

This track is divided into two progressive steps:

| Step | Directory | Description |
| :--- | :--- | :--- |
| **Step 1** | [`step01-pfifo/`](step01-pfifo/README.en.md) | **Packet-limited FIFO (pfifo)**: Implement a single-queue FIFO with a 4-packet limit (`sch->limit = 4`) and 2 ms bottleneck rate pacing, mastering BPF kptrs, linked lists, spin locks, and the watchdog timer. |
| **Step 2** | [`step02-bfifo-prio/`](step02-bfifo-prio/README.en.md) | **2-Band Priority + Byte-limited FIFO**: Build a 2-band priority queue enforcing a byte limit (`BYTE_LIMIT = 3500`) on normal traffic while allowing high-priority traffic (`SO_PRIORITY` / `TC_PRIO_CONTROL`) to jump ahead of queued bulk packets. |

### Running the Labs

From the `labs/` directory (`/usr/local/google/home/kuniyu/tcp-eli5/labs`), run:

```sh
# Step 1: verify starter fails, then test solution
./run_test.sh qdisc-fifo/step01-pfifo starter
./run_test.sh qdisc-fifo/step01-pfifo solution

# Step 2: verify starter fails, then test solution
./run_test.sh qdisc-fifo/step02-bfifo-prio starter
./run_test.sh qdisc-fifo/step02-bfifo-prio solution
```

Now proceed to [`step01-pfifo/README.en.md`](step01-pfifo/README.en.md) ([Japanese](step01-pfifo/README.md)) to build your first BPF Qdisc!


---

## Step 1: Packet-Limited FIFO Qdisc and Watchdog Rate Pacing


## 1. Goal

In this step, you will implement the Linux kernel's traffic control interface `struct Qdisc_ops` using BPF `struct_ops`, building the most fundamental queueing discipline from scratch: the **Packet-limited FIFO (pfifo)**.

By implementing tail-dropping under a queue capacity limit (`sch->limit = 4`) and bottleneck rate pacing (2 ms per packet) driven by high-resolution watchdog timers, you will master the full lifecycle of packet enqueueing/dequeuing as well as BPF-specific memory ownership and locking invariants.

- For the overall track structure and background concepts, refer to the [Qdisc FIFO Track README](#).

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

In real networks, physical links have finite transmission bandwidth. For instance, simulating a bottleneck link that requires 2 ms of serialization delay per packet ($$\text{BOTTLENECK NS} = 2,000,000\text{ ns}$$) requires the Qdisc to pace packet departures such that at most one packet is released to the NIC every 2 ms.

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

Let's review the key sections of [`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step01-pfifo/solution.bpf.c).

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

[`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step01-pfifo/test.pkt) executes the following sequence:

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
../../run_test.sh qdisc-fifo/step01-pfifo starter

# Verify solution success
../../run_test.sh qdisc-fifo/step01-pfifo solution
```


### `labs/qdisc-fifo/step01-pfifo/starter.bpf.c` (Starter skeleton)

```c
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
```

### `labs/qdisc-fifo/step01-pfifo/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 1: Test packet-limited FIFO (sch->limit = 4) and 2 ms watchdog rate pacing

0.00 `tc qdisc replace dev tun0 root bpf_pfifo`
0.00 `ethtool -K tun0 tso off gso off`

// Establish a passive TCP connection.
// MSS = 1012 with 12-byte TCP Timestamps option -> tp->mss_cache == 1000 bytes.
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// write(4, ..., 6000) bursts 6 segments while cwnd = 10:
// - Packet 1 (1:1001) dequeues immediately at +0.00, scheduling next_tx_ns = now + 2ms.
// - Packets 2..5 (1001:2001, 2001:3001, 3001:4001, 4001:5001) enter the 4-packet FIFO.
// - Packet 6 (5001:6001) exceeds sch->limit = 4 and is tail-dropped!
// - Watchdog timer fires every 2 ms (+0.002) to dequeue Packets 2..5.
+0.00 write(4, ..., 6000) = 6000
+0.00 > . 1:1001(1000) ack 1
+0.002 > . 1001:2001(1000) ack 1
+0.002 > . 2001:3001(1000) ack 1
+0.002 > . 3001:4001(1000) ack 1
+0.002 > . 4001:5001(1000) ack 1

// Verify that bpf_pfifo recorded at least 1 tail drop
+0.01 `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"`
```

<details>
<summary>Reference solution: <code>labs/qdisc-fifo/step01-pfifo/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: Packet-limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Implements a 4-packet limit FIFO queue using BPF struct_ops (struct Qdisc_ops),
 * bpf_list, bpf_spin_lock, bpf_kptr, and the qdisc watchdog timer.
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
```

</details>



---

## Step 2: 2-Band Priority + Byte-Limited FIFO Qdisc


## 1. Goal

In this step, you will extend the basic FIFO from Step 1 into a **2-Band Priority + Byte-limited FIFO Qdisc (`bpf_bfifo`)** combining a byte-based backlog limit (bfifo) with strict priority scheduling.

You will enforce a byte ceiling (`BYTE_LIMIT = 3500` bytes) on normal bulk traffic to prevent bufferbloat while allowing high-priority control packets (`SO_PRIORITY = 7` / `TC_PRIO_CONTROL`) to bypass the byte limit and jump ahead of already-queued bulk packets, eliminating head-of-line blocking.

- For the overall track structure and background concepts, refer to the [Qdisc FIFO Track README](#).

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
   - **Seg 2** (`1001:2001`): Queued in `q_normal`, `backlog = 1066` ($$\le 3500$$).
   - **Seg 3** (`2001:3001`): Queued in `q_normal`, `backlog = 2132` ($$\le 3500$$).
   - **Seg 4** (`3001:4001`): Queued in `q_normal`, `backlog = 3198` ($$\le 3500$$).
   - **Seg 5** (`4001:5001`): $$3198 + 1066 = 4264 > 3500$$ -> **Tail-dropped due to `BYTE_LIMIT`!**
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

Review [`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step02-bfifo-prio/solution.bpf.c).

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

[`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step02-bfifo-prio/test.pkt) verifies:

1. **Dual Flow Setup**: Establishes Flow A (`fd = 4`, standard priority) and Flow B (`fd = 6`, `SO_PRIORITY = 7`).
2. **Flow A Burst & Byte-Limit Tail-Drop (Line 29)**: `write(4, ..., 5000)` sends 5 segments. Segments 1–4 are admitted; Segment 5 exceeds `BYTE_LIMIT = 3500` and is dropped.
3. **Priority Flow B Preemption (Lines 33–39)**: `write(6, ..., 500)` puts a high-priority packet into `q_prio`. At `+0.004` (4 ms later), Flow B's 500-byte packet is dequeued **BEFORE Flow A's remaining queued segments (1001:2001, 2001:3001, 3001:4001)**.
4. **Resumption of Bulk Flow & Drop Check (Lines 42–48)**: Flow A segments drain sequentially, and `tc -s qdisc show dev tun0` confirms `dropped >= 1`.

### 5.2 Test Execution Commands

```sh
# Verify starter failure
../../run_test.sh qdisc-fifo/step02-bfifo-prio starter

# Verify solution success
../../run_test.sh qdisc-fifo/step02-bfifo-prio solution
```


### `labs/qdisc-fifo/step02-bfifo-prio/starter.bpf.c` (Starter skeleton)

```c
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
```

### `labs/qdisc-fifo/step02-bfifo-prio/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 2: Test 2-band priority + byte-limited FIFO

0.00 `tc qdisc replace dev tun0 root bpf_bfifo`
0.00 `ethtool -K tun0 tso off gso off`

// Setup Flow A listener (fd 3)
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEPORT, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// Handshake Flow A
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Setup Flow B listener (fd 5)
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 5
+0.00 setsockopt(5, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 setsockopt(5, SOL_SOCKET, SO_REUSEPORT, [1], 4) = 0
+0.00 bind(5, ..., ...) = 0
+0.00 listen(5, 1) = 0

// Handshake Flow B
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(5, ..., ...) = 6
+0.00 setsockopt(6, SOL_SOCKET, SO_PRIORITY, [7], 4) = 0

// Flow A bursts 5000 bytes (5 segments):
// - Segment 1 (1:1001) dequeues immediately at t = 0.
// - Segments 2, 3, 4 (1001:2001, 2001:3001, 3001:4001) queue into q_normal.
// - Segment 5 (4001:5001) exceeds BYTE_LIMIT = 3500 and is dropped.
+0.00 write(4, ..., 5000) = 5000

// Flow B sends 500 bytes (1 segment) with SO_PRIORITY = 7:
// Enqueued into q_prio, bypassing BYTE_LIMIT.
+0.00 write(6, ..., 500) = 500

// Segment 1 of Flow A departs immediately at +0.00:
+0.00 > . 1:1001(1000) ack 1

// At +0.004 (4 ms later), Flow B's high-priority packet is dequeued BEFORE Flow A's queued segments!
+0.004 > P. 1:501(500) ack 1

// Subsequent dequeue cycles serve Flow A's remaining segments from q_normal:
+0.004 > . 1001:2001(1000) ack 1
+0.004 > . 2001:3001(1000) ack 1
+0.004 > . 3001:4001(1000) ack 1

// Verify that Segment 5 was tail-dropped by bpf_bfifo
+0.01 `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"`
```

<details>
<summary>Reference solution: <code>labs/qdisc-fifo/step02-bfifo-prio/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 2: 2-Band Priority + Byte-Limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Implements a 2-band priority queue:
 * - High priority (TC_PRIO_CONTROL): enqueued to q_prio, bypasses BYTE_LIMIT
 * - Normal priority: enqueued to q_normal, subject to BYTE_LIMIT (3500 bytes)
 * Dequeue always serves q_prio before q_normal, paced at BOTTLENECK_NS (4 ms).
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
```

</details>
