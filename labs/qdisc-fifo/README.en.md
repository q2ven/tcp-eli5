# Track 4: Introduction to BPF Qdiscs — FIFO Queueing and Priority Scheduling

[日本語](README.md)

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
