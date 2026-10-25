# Deep-Dive Notes & Q&A

This chapter collects standalone, visual explanations of subtle kernel mechanisms and common questions that come up while reading the main chapters — such as sequence-number boundaries, in-flight packet gauges vs cumulative counters, `TCP_CHRONO_BUSY`, and `TCP_NOTSENT_LOWAT`. Each section is cross-linked from the corresponding chapter in the main text.

---

## 1. Why Don't `write_seq` and `copied_seq` Start at `0`? {#write-seq-copied-seq}

(Related chapter: [Chapter 1: Core data structures](01-data-structures.md))

TCP sequence numbers do not start at byte offset `0`; they start at a cryptographically chosen random **ISN (Initial Sequence Number)** negotiated via SipHash during the 3-way handshake (SYN / SYN-ACK).

### Sender Sequence Boundaries (`snd_una` / `snd_nxt` / `write_seq`)

```text
Send stream (byte sequence number space)
  ... | Sent & ACKed | Sent & UnACKed (In-Flight) | Unsent (queued by sendmsg) |
                     ^                            ^                            ^
                  snd_una                      snd_nxt                     write_seq
```

- **`snd_una`**: Sequence number of the oldest byte that has not yet been acknowledged by the peer. Everything up through `snd_una - 1` has been confirmed delivered.
- **`snd_nxt`**: Sequence number of the next byte to be transmitted onto the wire for the first time. `snd_nxt - snd_una` is the number of transmitted, unacknowledged bytes currently in flight (or in the retransmit queue).
- **`write_seq`**: Sequence number immediately following the last byte appended to the socket write queue by `write()` / `sendmsg()`. `write_seq - snd_nxt` is the number of unsent bytes (`notsent_bytes`) that have never been put on the wire yet.

### Receiver Sequence Boundaries (`copied_seq` / `rcv_nxt`)

```text
Receive stream (byte sequence number space)
  ... | Copied to user via recvmsg | Received in kernel, unread | Not yet received |
                                   ^                            ^
                               copied_seq                    rcv_nxt
```

- **`copied_seq`**: Sequence number of the first byte in the receive queue that the application has not yet read via `read()` / `recvmsg()` (everything up through `copied_seq - 1` has already been copied to user space).
- **`rcv_nxt`**: Sequence number of the next contiguous byte the kernel expects from the peer (everything up through `rcv_nxt - 1` has arrived in order).
- Therefore, **`rcv_nxt - copied_seq`** is the number of unread bytes waiting in the kernel's receive queue (`ioctl(SIOCINQ)`).

---

## 2. What Are `SND.WL1` (`snd_wl1`) and `SND.WL2` (`snd_wl2`) For? {#snd-wl1-wl2}

(Related chapters: [Chapter 1: Core data structures](01-data-structures.md), [Chapter 7: ACK processing and RTT estimation](07-ack-processing.md))

Every incoming TCP header carries the peer's current receive window (`seg.wnd`), which the sender records in `tp->snd_wnd`.
However, packets can be **reordered** in the network. If the sender blindly overwrote `tp->snd_wnd` with every arriving packet's `seg.wnd`, a delayed older packet arriving out of order could overwrite a newer window update with a stale value.

```text
Receiver                                       Sender
--------                                       ------
(1) Sends packet P1:
    seq=500, ack=1000, win=4000 (plenty of room)
         \
          \  (P1 is delayed and overtaken in the network)
           \
(2) Sends packet P2:                           (2') P2 arrives first!
    seq=500, ack=2000, win=0 (buffer full!)    --->  Updates snd_wnd = 0
                                                     Records snd_wl1 = 500, snd_wl2 = 2000
             \
              \-------------------------------> (1') Delayed P1 arrives later!
                                                     seg.seq=500, seg.ack=1000, seg.wnd=4000
                                                     If accepted blindly, snd_wnd would roll
                                                     back to 4000 and overrun the full peer!
```

To prevent stale window updates, TCP (RFC 9293 §3.3) records **the `seq` (`snd_wl1`) and `ack` (`snd_wl2`) of the segment that last updated `snd_wnd`**, and `tcp_may_update_window()` only updates `snd_wnd` when the incoming segment is provably newer:

1. **`after(seq, snd_wl1)`**: The peer has advanced its own send sequence number (`seq > snd_wl1`).
2. **`seq == snd_wl1 && after(ack, snd_wl2)`**: The peer's `seq` is unchanged, but it is acknowledging newer data from us (`ack > snd_wl2`).
3. **`seq == snd_wl1 && ack == snd_wl2 && nwin > snd_wnd`**: Both `seq` and `ack` are identical, and the peer is advertising a larger window (a pure Window Update ACK after the peer application read data).

In the diagram above, when the delayed P1 arrives with `seq == 500` and `ack (1000) < snd_wl2 (2000)`, `tcp_may_update_window()` recognizes it as stale and ignores its `win=4000`.

---

## 3. Do `packets_out`, `sacked_out`, `lost_out`, and `retrans_out` Grow Forever? {#packets-out-sacked-out-lifecycle}

(Related chapters: [Chapter 1: Core data structures](01-data-structures.md), [Chapter 7: ACK processing and RTT estimation](07-ack-processing.md), [Chapter 8: Loss detection and recovery](08-loss-recovery.md))

**No — none of these four fields are monotonic cumulative counters.**
All four are **instantaneous gauges** describing the current breakdown of unacknowledged packets sitting in the retransmit queue between `snd_una` and `snd_nxt`. As cumulative ACKs advance `snd_una` and free `sk_buff`s from the retransmit queue, **all four return to `0`**.

```text
|<------------------------ packets_out (e.g. 10 packets) ----------------------->|
+------------------+------------------+------------------+-----------------------+
|   lost_out (2)   |  sacked_out (3)  |       ...        |                  ...  |
|  "Marked lost"   | "Arrived (SACK)" | (Still flying: 5)|                       |
+------------------+------------------+------------------+-----------------------+
         |
         +---> Retransmitted & currently in flight: retrans_out (e.g. 1)
```

$$\text{Packets In Flight} = \text{packets}_{\text{out}} - (\text{sacked}_{\text{out}} + \text{lost}_{\text{out}}) + \text{retrans}_{\text{out}}$$

| Field | Meaning | Incremented (`+`) when | Decremented (`-`) when |
| :--- | :--- | :--- | :--- |
| **`packets_out`** | Total original data packets between `snd_una` and `snd_nxt` | New data is transmitted (`tcp_event_new_data_sent()`) | Cumulative ACK advances `snd_una` and frees `sk_buff`s from the rtx queue (`tcp_clean_rtx_queue()`) |
| **`sacked_out`** | Subset of `packets_out` selectively acknowledged via SACK blocks (or estimated via Reno dupacks) | New SACK blocks are tagged on the rtx queue (`tcp_sacktag_one()`) | **Cumulative ACK (`snd_una`) advances past the SACKed `sk_buff` and frees it** (or RTO clears SACK state) |
| **`lost_out`** | Subset of `packets_out` deemed lost by RACK or SACK reordering threshold | Packets are marked lost (`tcp_mark_skb_lost()`, or all un-SACKed packets on RTO) | **Cumulative ACK covers and frees the lost `sk_buff`** (or loss recovery is undone) |
| **`retrans_out`** | Subset of `lost_out` that has been retransmitted and is currently in flight | A lost `sk_buff` is retransmitted (`__tcp_retransmit_skb()`) | **Cumulative ACK frees the retransmitted `sk_buff`**, or the retransmission itself is deemed lost again |

- **Which fields DO grow monotonically over the life of a connection?**
  - **`tp->delivered`** (total packets delivered to the peer, used by BBR's delivery-rate estimator), **`tp->delivered_ce`** (total ECN CE-marked packets delivered), and **`tp->total_retrans`** (lifetime cumulative retransmissions).

---

## 4. What Do `enum tcp_chrono` and `TCP_CHRONO_BUSY` Measure? {#tcp-chrono-busy}

(Related chapter: [Chapter 3: Sending (1): tcp_sendmsg and the write queue](03-sendmsg.md))

When a TCP connection achieves lower throughput than expected in production, there are four possible culprits:
1. The application itself is idle (not writing data).
2. The network path is limiting transmission via `snd_cwnd` or pacing.
3. The peer's receive window (`snd_wnd` / `rwnd`) is throttling the sender.
4. The local send buffer (`SO_SNDBUF` / `sk_sndbuf`) is full, blocking `sendmsg()` even though `cwnd` and `rwnd` have room.

`tcp_chrono_start()` and `tcp_chrono_stop()` (`enum tcp_chrono`) act as a microsecond stopwatch attributing active send time across these states:

```text
Priority order (higher-numbered state overrides lower-numbered state):
  TCP_CHRONO_UNSPEC (0)        : Idle (both write queue and rtx queue are empty)
    ↓
  TCP_CHRONO_BUSY (1)          : At least 1 byte is queued or in flight (total active send time)
    ↓
  TCP_CHRONO_RWND_LIMITED (2)  : Throttled by peer receive window (snd_wnd)
    ↓
  TCP_CHRONO_SNDBUF_LIMITED (3): Throttled because local send buffer (sk_sndbuf) is full
```

- The moment `tcp_sendmsg_locked()` queues the very first `sk_buff` onto a completely empty socket (`tcp_skb_entail()`), it calls `tcp_chrono_start(sk, TCP_CHRONO_BUSY)` to start the active-transmission stopwatch.
- Once all queued and in-flight data is acknowledged and the queues become empty again, `tcp_chrono_stop(sk, TCP_CHRONO_BUSY)` stops the clock.
- The accumulated durations in `tp->chrono_stat[]` are exposed via `ss -ti` (`busy:120ms rwnd_limited:30ms(25.0%) sndbuf_limited:60ms(50.0%)`) and `struct tcp_info` (`tcpi_busy_time`, `tcpi_rwnd_limited`, `tcpi_sndbuf_limited`).

---

## 5. `TCP_NOTSENT_LOWAT` (`notsent_lowat`) and `tcp_stream_memory_free()` {#tcp-notsent-lowat}

(Related chapter: [Chapter 3: Sending (1): tcp_sendmsg and the write queue](03-sendmsg.md))

### Why Do We Need `TCP_NOTSENT_LOWAT` in Addition to `sk_sndbuf`?

A TCP socket's send buffer (`sk_wmem_queued`, capped by `sk_sndbuf`) holds **two distinct categories of data separated by `snd_nxt`**:

```text
|<------------------------ sk_wmem_queued (limit: sk_sndbuf) ---------------------->|
+----------------------------------------+------------------------------------------+
| (A) Sent but unacknowledged data       | (B) Unsent data never put on the wire    |
|     (In-Flight: must keep for retrans) |     yet (notsent_bytes)                  |
+----------------------------------------+------------------------------------------+
^                                        ^                                          ^
snd_una                                  snd_nxt                                    write_seq
```

- **(A) Sent but unacknowledged (`snd_nxt - snd_una`)**:
  - Data that has already been transmitted onto the wire but not yet acknowledged by the peer. The kernel must retain these `sk_buff`s in case retransmission is needed.
  - On high-BDP paths, (A) must grow to several megabytes to keep the pipe full, so Linux's send-buffer autotuning (`tcp_moderate_sndbuf()`) automatically expands `sk_sndbuf` to multiple megabytes (e.g. 4 MB).
- **(B) Unsent data (`write_seq - snd_nxt` = `notsent_bytes`)**:
  - Data that the application has passed to `sendmsg()`, waiting in the kernel queue for cwnd or pacing to allow it onto the wire for the first time.

If only `sk_sndbuf` (say, 4 MB) caps the queue and (A) is currently 500 KB, a non-blocking `epoll` loop will pour **3.5 MB of unsent data (B)** into the kernel.
Once bytes are handed to `sendmsg()`, the application can no longer cancel or reorder them. If an HTTP/2 or video server queues 3.5 MB of a low-priority image and immediately receives a high-priority API request or stream cancellation, that urgent response is stuck behind 3.5 MB of already-queued bytes (application-layer Head-of-Line blocking / bufferbloat).

Setting **`TCP_NOTSENT_LOWAT`** (default `UINT_MAX`, unlimited) to e.g. **`16 KB`** solves this:
1. **(A) In-flight data** can still use the full multi-megabyte `sk_sndbuf` (preserving 100% of high-BDP throughput), while
2. **(B) Unsent data (`write_seq - snd_nxt`) is capped at 16 KB**.
3. As packets leave on the wire and `snd_nxt` advances, the kernel wakes the application **just-in-time** to supply the highest-priority stream's next 16 KB (commit `c9bee3b7fdec`).

### Why Is an Upper Limit Called `LOWAT`, and Why Does `<< wake` Halve It?

```c
bool tcp_stream_memory_free(const struct sock *sk, int wake)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 notsent_bytes = READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_nxt);

	return (notsent_bytes << wake) < tcp_notsent_lowat(tp);
}
```

- **Why `LOWAT` (Low Watermark)**: From `poll()` / `epoll`'s perspective, it is the low watermark below which unsent bytes (B) must drop before the socket becomes writable (`EPOLLOUT`).
- **Why `<< wake` halves the threshold (Hysteresis)**:
  - **During `sendmsg()` (`wake = 0`)**: `notsent_bytes < lowat` (the application can write until unsent bytes reach e.g. 16 KB).
  - **When checking whether to emit `EPOLLOUT` (`wake = 1`)**: `(notsent_bytes << 1) < lowat`, i.e. **`notsent_bytes < lowat / 2` (do not wake the application until unsent bytes drop below half the limit, e.g. 8 KB)**.
  - Without this hysteresis, transmitting a single 1.4 KB packet from a 16 KB queue would immediately trigger `EPOLLOUT` at 14.6 KB, waking the application on every single packet just to write 1.4 KB and hit `-EAGAIN` again (commits `8ba3c9d1c6d7`, `a74f0fa082b7`).

---

## 6. `alloc_skb_fclone()` and `struct sk_buff_fclones`: Why TCP Preallocates Twin `sk_buff`s {#skb-fclone}

**Related sections**: [Chapter 3: `tcp_stream_alloc_skb()`](03-sendmsg.md), [Chapter 4: `__tcp_transmit_skb()`](04-write-xmit.md)

### Why Does TCP Clone Every `sk_buff` on Transmit?

In a protocol without retransmission (such as UDP), the kernel hands the `struct sk_buff` directly down to the IP layer, Qdisc, and NIC driver, and the driver frees it via `consume_skb()` as soon as TX DMA completes.

TCP cannot let the NIC free the packet on TX completion: **because the packet might be dropped in the network, TCP must keep the packet in its retransmit queue (`tcp_rtx_queue`) until the peer's ACK arrives**. At the same time, the lower layers (IP, Qdisc, NIC driver) overwrite `struct sk_buff` linkage pointers (`next`/`prev`/`rbnode`) and header offsets (`data`/`transport_header`/`network_header`), and free the `struct sk_buff` upon DMA completion.

Therefore, every time `__tcp_transmit_skb()` sends a packet, it calls **`skb_clone()`**:
- **Note**: `skb_clone()` does **not** copy the packet payload (the page fragments or the `skb->head` linear buffer). It merely increments the data reference count (`skb_shinfo(skb)->dataref`) and **duplicates only the ~232-byte `struct sk_buff` metadata descriptor**.
- **Original `sk_buff` (`oskb`)**: Remains in the socket's `tcp_rtx_queue` until ACKed.
- **Cloned `sk_buff` (`skb`)**: Has TCP/IP/Ethernet headers pushed onto it, travels down through IP -> Qdisc -> NIC driver, and is freed when TX DMA completes.

### Why `fclone` (Fast Clone) Exists

Calling `skb_clone()` on a normal `alloc_skb()` buffer requires a second slab allocation (`kmem_cache_alloc(skbuff_cache)`) for the clone's `struct sk_buff`, and two separate `kmem_cache_free()` calls later.

However, for every `sk_buff` allocated in the TCP transmit path (`tcp_stream_alloc_skb()`), **we know with 100% certainty upfront that `__tcp_transmit_skb()` will clone it at least once**.

Therefore, `alloc_skb_fclone()` ([include/linux/skbuff.h#L1437](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/linux/skbuff.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1437)) allocates a **`struct sk_buff_fclones` containing two adjacent `struct sk_buff`s (`skb1` and `skb2`) plus a shared `fclone_ref` counter** in a single slab object from `skbuff_fclone_cache` ([include/linux/skbuff.h#L1401](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/linux/skbuff.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1401)):

```c
/* Layout of fast clones : [skb1][skb2][fclone_ref] */
struct sk_buff_fclones {
	struct sk_buff	skb1;

	struct sk_buff	skb2;

	refcount_t	fclone_ref;
};
```

```
One slab object in skbuff_fclone_cache (struct sk_buff_fclones):
+---------------------------+---------------------------+------------+
| skb1 (SKB_FCLONE_ORIG)    | skb2 (SKB_FCLONE_CLONE)   | fclone_ref |
| Kept in TCP rtx queue     | Preallocated twin for TX  |   1 -> 2   |
+---------------------------+---------------------------+------------+
              |                           |
              +-------------+-------------+
                            v
      Share the same data buffer (skb->head / skb_shared_info / page frags)
```

### The 3-Step Lifecycle: Allocate, Clone, Free

1. **Allocation (`alloc_skb_fclone()` -> `__alloc_skb()`)**:
   - Allocates one `struct sk_buff_fclones` from `skbuff_fclone_cache`.
   - Initialises `skb1.fclone = SKB_FCLONE_ORIG` and `fclone_ref = 1`, returning `&fclones->skb1`. The adjacent `skb2` sits unused right next to `skb1`.
2. **Transmission (`__tcp_transmit_skb()` -> `skb_clone()`, [net/core/skbuff.c#L2091](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2091))**:
   ```c
   if (skb->fclone == SKB_FCLONE_ORIG &&
       refcount_read(&fclones->fclone_ref) == 1) {
   	n = &fclones->skb2;
   	refcount_set(&fclones->fclone_ref, 2);
   	n->fclone = SKB_FCLONE_CLONE;
   }
   ```
   - Because `skb1` has `SKB_FCLONE_ORIG` and `fclone_ref == 1` (`skb2` is free), **`skb_clone()` skips the slab allocator completely, grabs the adjacent `&fclones->skb2` via pointer arithmetic**, sets `fclone_ref = 2`, and copies the `struct sk_buff` fields into `skb2`.
3. **Deallocation (`kfree_skbmem()`, [net/core/skbuff.c#L1130](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1130))**:
   - Typically, NIC TX DMA completes first (within microseconds) and frees `skb2`. `refcount_dec_and_test(&fclones->fclone_ref)` decrements `fclone_ref` from `2` back to `1` without touching the slab allocator (if the packet later needs retransmission, `fclone_ref` is `1` again, so `skb2` can even be reused for the retransmission!).
   - When the peer's ACK later arrives and TCP frees `skb1`, `kfree_skbmem()` sees `skb->fclone == SKB_FCLONE_ORIG` and `refcount_read(&fclones->fclone_ref) == 1`, bypasses the atomic decrement via `goto fastpath`, and frees the entire `struct sk_buff_fclones` pair back to `skbuff_fclone_cache` in a single call.

---

## 7. SACK Shifting (`tcp_shift_skb_data()` / `skb_shift()`) and Why Linear Payload Is Always 0 {#sack-shifting}

**Related sections**: [Chapter 3: `tcp_stream_alloc_skb()`](03-sendmsg.md), [Chapter 8: Step 6 SACK shifting](08-loss-recovery.md)

### Why Must TCP Split and Retain SACKed `sk_buff`s Instead of Freeing Them Immediately?

A natural question is: **"If a SACK block tells the sender that those bytes already reached the receiver, why doesn't the sender just free them immediately instead of splitting `sk_buff`s and keeping SACKed data in the retransmit queue?"** There are two fundamental reasons:

1. **Why SACKed data cannot be freed until `snd_una` advances: SACK Reneging ([RFC 2018 §4](https://www.rfc-editor.org/rfc/rfc2018#section-4))**:
   - A cumulative ACK (`ack_seq` / `snd_una`) is an **irrevocable promise** that the receiver has received all bytes in order up to `ack_seq`.
   - A SACK block, by contrast, is strictly **advisory (non-binding)**: because packet `#1` is still missing, the receiver is merely holding packets `#2..#4` in its kernel **out-of-order queue (`out_of_order_queue`, Chapter 6)** and cannot deliver them to the application (`recvmsg()`) yet.
   - If the receiver runs into severe memory pressure while waiting for `#1`, RFC 2018 explicitly allows the receiver to **discard previously SACKed packets from its out-of-order queue** (`tcp_prune_ofo_queue()` in Chapters 6 & 10). This is called **SACK Reneging**.
   - If the sender had already freed `#2..#4` upon receiving the SACK, and the receiver subsequently discarded its out-of-order queue under memory pressure, **those bytes would exist neither on the sender nor on the receiver—causing permanent data loss**.
   - Indeed, if an RTO fires while the head of the retransmit queue (`snd_una`) is marked `TCPCB_SACKED_ACKED`, `tcp_timeout_mark_lost()` ([net/ipv4/tcp_input.c#L2524](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2524)) detects SACK reneging (`is_reneg = true`), **clears `TCPCB_SACKED_ACKED` across all `sk_buff`s in the retransmit queue**, and marks them lost so they can be retransmitted.
   - Therefore, **the sender must never free any data from `tcp_rtx_queue` until the cumulative ACK (`snd_una`) covers it**.

2. **Why a partially SACKed TSO `sk_buff` must be split (`tcp_fragment()`)**:
   - Both scoreboard state flags (`TCP_SKB_CB(skb)->sacked`: `S` = SACKed, `L` = Lost, `R` = Retransmitted) and retransmissions (`tcp_retransmit_skb()`) operate at the granularity of **an entire `struct sk_buff`** (there is no per-MSS bitmap inside a TSO `sk_buff`).
   - If a 10-segment TSO `sk_buff` (`#1..#10`) has `#1` lost and `#2..#4` SACKed, keeping them in a single `sk_buff` would make it impossible to mark `#1` as `L` and `#2..#4` as `S`, and impossible to retransmit `#1` alone without redundantly retransmitting `#2..#10`.
   - Thus, the boundary between an un-SACKed hole (`#1`) and a SACKed range (`#2..#4`) **must** be split into separate `sk_buff`s.

### What Is SACK Shifting?

With TSO (TCP Segmentation Offload), TCP builds a single large `sk_buff` containing, say, 10 segments (10 MSS) of payload, and the NIC splits it into 10 wire packets (#1..#10).

Suppose **packet #1 is lost** in the network while packets #2..#10 arrive at the receiver. As each packet arrives, the receiver sends SACK (Selective ACK) blocks reporting the delivered range:

- **Without SACK shifting (fragmentation via `tcp_fragment()`)**:
  - When the first SACK reports `#2..#4`, TCP splits the 10-MSS `sk_buff` into three separate `sk_buff`s: `#1` (unacked), `#2..#4` (SACKed), and `#5..#10` (unsacked).
  - When the next SACK reports `#2..#6`, TCP splits `#5..#10` again into `#5..#6` (SACKed) and `#7..#10` (unsacked).
  - What started as a single TSO `sk_buff` shatters into many tiny 1–2 MSS `sk_buff`s inside the retransmit rbtree, inflating both memory overhead and rbtree traversal cost.

- **With SACK shifting (`tcp_shift_skb_data()`, [net/ipv4/tcp_input.c#L1954](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1954))**:
  - Instead of splitting `#5..#6` into a separate `sk_buff`, TCP **shifts (moves) the newly SACKed bytes (`#5..#6`) from the front of `skb` (`#5..#10`) into the tail of the immediately preceding already-SACKed `prev` (`#2..#4`)**:

```
[Before SACK for #5..#6]
  prev (SACKed: #2..#4)           skb (Unsacked: #5..#10)
+--------------------------+   +----------------------------------+
| seq: #2..#4              |   | seq: #5..#10                     |
+--------------------------+   +----------------------------------+

[When SACK for #2..#6 arrives: SACK shifting]
  prev (SACKed: #2..#6)                 skb (Unsacked: #7..#10)
+----------------------------------+   +--------------------------+
| seq: #2..#6 (absorbs #5..#6!)    |   | seq: #7..#10 (shrunk)    |
+----------------------------------+   +--------------------------+
```

When `#7..#10` are subsequently SACKed, `prev` absorbs the remainder of `skb` and the empty `skb` is freed (`TCPSackMerged`), keeping the entire contiguous SACKed range in a single `sk_buff`.

### Why Putting 100% of Payload in `frags[]` (Zero Linear Payload) Enables Zero-Copy Shifting

An `sk_buff` can store bytes in two places (Chapter 1):

1. **The linear buffer (`skb->head` .. `skb->tail`, length `skb_headlen(skb)`)**:
   - A single contiguous `kmalloc` buffer allocated alongside the TCP/IP header headroom.
   - Moving payload out of `skb->head` into another `sk_buff` (`prev`) would require **`memcpy()`ing the payload bytes**. Consequently, `skb_can_shift()` ([net/ipv4/tcp_input.c#L1931](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1931)) and `skb_shift()` ([net/core/skbuff.c#L4327](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n4327)) **refuse to shift any `sk_buff` that has even 1 byte of linear payload (`skb_headlen(skb) > 0`)**:
   ```c
   /* Shifting pages past head area doesn't work */
   static int skb_can_shift(const struct sk_buff *skb)
   {
   	return !skb_headlen(skb) && skb_is_nonlinear(skb);
   }
   ```
2. **The page fragment array (`skb_shinfo(skb)->frags[]`)**:
   - The payload bytes live in separate pages (such as `current->task_frag`'s 32 KB page), while the `sk_buff` holds only an array of `(page, offset, size)` descriptors (`skb_frag_t`).
   - When 100% of the payload lives in `frags[]`, shifting `shiftlen` bytes from `skb` to `prev` (`skb_shift(prev, skb, shiftlen)`) **never touches the payload bytes at all—it only updates `(page, offset, size)` descriptors in `frags[]` (zero-copy)**.
   - Even better, because `tcp_sendmsg_locked()` appends sequential `sk_buff`s into the same 32 KB `task_frag` page, `prev`'s last frag and `skb`'s first frag usually point to **contiguous offsets of the exact same page**. In that common case, `skb_shift()` simply adds `shiftlen` to `prev`'s frag `size` and advances `skb`'s frag `offset` by `shiftlen` ([net/core/skbuff.c#L4369-L4371](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n4369)).

This is why `tcp_stream_alloc_skb()` in Chapter 3 allocates `MAX_TCP_HEADER` of linear headroom only and places all payload into page fragments (commits `3613b3dbd1ad`, `5882efff88aa`): so every TX `sk_buff` satisfies `!skb_headlen(skb)` and supports zero-copy SACK shifting.
