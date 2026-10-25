# 目次

- [はじめに](README.md)

## 準備

- [1. 主要データ構造](01-data-structures.md)
- [2. コネクションの一生を早回しで](02-connection-lifecycle.md)

## データ転送の fast path

- [3. 送信 (1): tcp_sendmsg と送信キュー](03-sendmsg.md)
- [4. 送信 (2): tcp_write_xmit からワイヤまで](04-write-xmit.md)
- [5. 受信 (1): tcp_v4_rcv と header prediction](05-rcv-established.md)
- [6. 受信 (2): データキューイングと tcp_recvmsg](06-data-queue-recvmsg.md)
- [7. ACK 処理と RTT 推定](07-ack-processing.md)
- [8. ロス検出と回復](08-loss-recovery.md)
- [9. タイマー](09-timers.md)
- [10. ウィンドウとメモリ管理](10-window-memory.md)

## 輻輳制御

- [11. 輻輳制御フレームワークと Reno](11-cc-framework-reno.md)
- [12. CUBIC](12-cubic.md)
- [13. BBR](13-bbr.md)
- [14. ECN と DCTCP](14-ecn-dctcp.md)

## パケットスケジューリング (Qdisc)

- [15. パケットスケジューリング (Qdisc) の基礎と FIFO / MQ](15-qdisc-framework-fifo.md)
- [16. Fair Queueing (sch_fq) と EDT (Earliest Departure Time) Pacing](16-qdisc-fq-edt.md)
- [17. AQM (CoDel / FQ-CoDel / RED) と Network Emulation (NetEm)](17-qdisc-aqm-codel-red-netem.md)

## BPF CC / Qdisc ハンズオン

- [ハンズオン全体の概要と進め方](lab-overview.md)
- [18. ハンズオン (1): BPF CC と packetdrill で作る TCP Reno](18-lab-reno.md)
- [19. ハンズオン (2): BPF CC と packetdrill で作る TCP CUBIC](19-lab-cubic.md)
- [20. ハンズオン (3): BPF CC と packetdrill で作る TCP BBR](20-lab-bbr.md)
- [21. ハンズオン (4): BPF Qdisc で作る FIFO と PRIO](21-lab-qdisc-fifo.md)
- [22. ハンズオン (5): BPF Qdisc で作る FQ と EDT Pacing](22-lab-qdisc-fq.md)
- [23. ハンズオン (6): BPF Qdisc で作る CoDel と FQ-CoDel](23-lab-qdisc-fq-codel.md)

## 付録

- [補足ノート・ Q&A 一覧](appendix/README.md)
  - [A1. write_seq や copied_seq はなぜ 0 から始まらないのか？](appendix/01-write-seq-copied-seq.md)
  - [A2. SND.WL1 (snd_wl1) と SND.WL2 (snd_wl2) は何のためにあるのか？](appendix/02-snd-wl1-wl2.md)
  - [A3. packets_out / sacked_out / lost_out / retrans_out は増え続けるフィールドか？](appendix/03-packets-out-lifecycle.md)
  - [A4. enum tcp_chrono と TCP_CHRONO_BUSY は何を測っているのか？](appendix/04-tcp-chrono-busy.md)
  - [A5. TCP_NOTSENT_LOWAT (notsent_lowat) と tcp_stream_memory_free()](appendix/05-tcp-notsent-lowat.md)
  - [A6. alloc_skb_fclone() と struct sk_buff_fclones：なぜ最初から双子の sk_buff を確保するのか](appendix/06-skb-fclone.md)
  - [A7. SACK shifting（tcp_shift_skb_data() / skb_shift()）と「線形領域のペイロードを 0 にする理由」](appendix/07-sack-shifting.md)
  - [A8. tp->nonagle の 3 つのビットと TCP_NAGLE_PUSH の役割](appendix/08-tcp-nagle-push.md)
  - [A9. SWS（Silly Window Syndrome / 愚かなウィンドウ症候群）と回避策](appendix/09-sws-avoidance.md)
  - [A10. PF_MEMALLOC / pfmemalloc / SOCK_MEMALLOC とは？](appendix/10-pfmemalloc.md)
