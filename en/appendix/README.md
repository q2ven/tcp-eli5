# Deep-Dive Notes & Q&A

Standalone, visual explanations of subtle kernel mechanisms and common questions that come up while reading the main chapters — one topic per file, cross-linked directly from the main text.

- [1. Why Don't `write_seq` and `copied_seq` Start at `0`?](01-write-seq-copied-seq.md)
- [2. What Are `SND.WL1` (`snd_wl1`) and `SND.WL2` (`snd_wl2`) For?](02-snd-wl1-wl2.md)
- [3. Do `packets_out`, `sacked_out`, `lost_out`, and `retrans_out` Grow Forever?](03-packets-out-lifecycle.md)
- [4. What Do `enum tcp_chrono` and `TCP_CHRONO_BUSY` Measure?](04-tcp-chrono-busy.md)
- [5. `TCP_NOTSENT_LOWAT` (`notsent_lowat`) and `tcp_stream_memory_free()`](05-tcp-notsent-lowat.md)
- [6. `alloc_skb_fclone()` and `struct sk_buff_fclones`: Why TCP Preallocates Twin `sk_buff`s](06-skb-fclone.md)
- [7. SACK Shifting (`tcp_shift_skb_data()` / `skb_shift()`) and Why Linear Payload Is Always 0](07-sack-shifting.md)
- [8. The 3 Bits of `tp->nonagle` and the Role of `TCP_NAGLE_PUSH`](08-tcp-nagle-push.md)
- [9. SWS (Silly Window Syndrome) and Its Avoidance](09-sws-avoidance.md)
- [10. What Are `PF_MEMALLOC`, `pfmemalloc`, and `SOCK_MEMALLOC`?](10-pfmemalloc.md)
