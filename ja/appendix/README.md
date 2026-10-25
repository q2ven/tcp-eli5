# 補足ノート・ Q&A 一覧

本編を読み進めるなかで疑問に浮かびやすい「なぜこのフィールドやフラグが存在するのか？」「このカウンタはどう増減するのか？」といったトピックを、1 テーマ 1 ファイルで図解とともにまとめた補足ノート集です。本編の各章からも各ファイルへ直接リンクしています。

- [1. `write_seq` や `copied_seq` はなぜ `0` から始まらないのか？](01-write-seq-copied-seq.md)
- [2. `SND.WL1` (`snd_wl1`) と `SND.WL2` (`snd_wl2`) は何のためにあるのか？](02-snd-wl1-wl2.md)
- [3. `packets_out` / `sacked_out` / `lost_out` / `retrans_out` は増え続けるフィールドか？](03-packets-out-lifecycle.md)
- [4. `enum tcp_chrono` と `TCP_CHRONO_BUSY` は何を測っているのか？](04-tcp-chrono-busy.md)
- [5. `TCP_NOTSENT_LOWAT` (`notsent_lowat`) と `tcp_stream_memory_free()`](05-tcp-notsent-lowat.md)
- [6. `alloc_skb_fclone()` と `struct sk_buff_fclones`：なぜ最初から双子の `sk_buff` を確保するのか](06-skb-fclone.md)
- [7. SACK shifting（`tcp_shift_skb_data()` / `skb_shift()`）と「線形領域のペイロードを 0 にする理由」](07-sack-shifting.md)
- [8. `tp->nonagle` の 3 つのビットと `TCP_NAGLE_PUSH` の役割](08-tcp-nagle-push.md)
- [9. SWS（Silly Window Syndrome / 愚かなウィンドウ症候群）と回避策](09-sws-avoidance.md)
- [10. `PF_MEMALLOC` / `pfmemalloc` / `SOCK_MEMALLOC` とは？](10-pfmemalloc.md)
- [11. MTU Probing（ `tcp_mtu_probe()` / PLPMTUD ）とは？](11-mtu-probing.md)
