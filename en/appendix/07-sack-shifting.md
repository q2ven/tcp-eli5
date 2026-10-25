# SACK Shifting (`tcp_shift_skb_data()` / `skb_shift()`) and Why Linear Payload Is Always 0

**Related sections**: [Chapter 3: `tcp_stream_alloc_skb()`](../03-sendmsg.md), [Chapter 8: Step 6 SACK shifting](../08-loss-recovery.md)

## Why Must TCP Split and Retain SACKed `sk_buff`s Instead of Freeing Them Immediately?

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

## What Is SACK Shifting?

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

## Why Putting 100% of Payload in `frags[]` (Zero Linear Payload) Enables Zero-Copy Shifting

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
