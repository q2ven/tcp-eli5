# `alloc_skb_fclone()` and `struct sk_buff_fclones`: Why TCP Preallocates Twin `sk_buff`s

**Related sections**: [Chapter 3: `tcp_stream_alloc_skb()`](../03-sendmsg.md), [Chapter 4: `__tcp_transmit_skb()`](../04-write-xmit.md)

## Why Does TCP Clone Every `sk_buff` on Transmit?

In a protocol without retransmission (such as UDP), the kernel hands the `struct sk_buff` directly down to the IP layer, Qdisc, and NIC driver, and the driver frees it via `consume_skb()` as soon as TX DMA completes.

TCP cannot let the NIC free the packet on TX completion: **because the packet might be dropped in the network, TCP must keep the packet in its retransmit queue (`tcp_rtx_queue`) until the peer's ACK arrives**. At the same time, the lower layers (IP, Qdisc, NIC driver) overwrite `struct sk_buff` linkage pointers (`next`/`prev`/`rbnode`) and header offsets (`data`/`transport_header`/`network_header`), and free the `struct sk_buff` upon DMA completion.

Therefore, every time `__tcp_transmit_skb()` sends a packet, it calls **`skb_clone()`**:
- **Note**: `skb_clone()` does **not** copy the packet payload (the page fragments or the `skb->head` linear buffer). It merely increments the data reference count (`skb_shinfo(skb)->dataref`) and **duplicates only the ~232-byte `struct sk_buff` metadata descriptor**.
- **Original `sk_buff` (`oskb`)**: Remains in the socket's `tcp_rtx_queue` until ACKed.
- **Cloned `sk_buff` (`skb`)**: Has TCP/IP/Ethernet headers pushed onto it, travels down through IP -> Qdisc -> NIC driver, and is freed when TX DMA completes.

## Why `fclone` (Fast Clone) Exists

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

## The 3-Step Lifecycle: Allocate, Clone, Free

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
