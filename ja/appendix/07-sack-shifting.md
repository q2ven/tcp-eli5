# SACK shifting（`tcp_shift_skb_data()` / `skb_shift()`）と「線形領域のペイロードを 0 にする理由」

**関連箇所**: [第 3 章: `tcp_stream_alloc_skb()`](../03-sendmsg.md)、[第 8 章: Step 6 SACK shifting](../08-loss-recovery.md)

## なぜ SACK されたデータをその場で解放せず、分割してまで保持し続けるのか？

SACK の処理を読むと、まず **「相手に届いたと SACK で報告されたのなら、なぜその場で解放（free）せず、わざわざ `sk_buff` を分割してまで再送キューに残しておくのか？」** という疑問が湧きます。理由は 2 つあります。

1. **なぜ SACK されたデータをすぐ解放してはいけないのか？ → SACK Reneging（受信側による SACK のちゃぶ台返し）があるため**:
   - 累積 ACK（`ack_seq` / `snd_una`）は「受信側が先頭から順番どおり受け取り、確定した」という **取り消し不能な約束** です。
   - 一方、SACK（Selective ACK）は「先頭の #1 がまだ届いていないので、先に届いた #2〜#4 を **受信側の Out-of-Order キュー（第 6 章の `out_of_order_queue`）に一時的に預かっている**」というだけの **仮の報告（Advisory / Non-binding）** に過ぎません（[RFC 2018 §4](https://www.rfc-editor.org/rfc/rfc2018#section-4)）。#1 が届くまで、受信側は #2〜#4 をアプリケーション（`recvmsg()`）に渡せません。
   - もし #1 を待っている間に受信側が深刻なメモリ不足に陥ると、受信側は **`out_of_order_queue` に預かっていた SACK 済みパケット（#2〜#4）を破棄して捨てること** が RFC 2018 で明確に許されています（第 6 章・第 10 章の `tcp_prune_ofo_queue()`）。これを **SACK Reneging（SACK の撤回・反故）** と呼びます。
   - もし送信側が「SACK されたから」といって #2〜#4 のメモリを解放してしまっていたら、受信側がメモリ不足で OOO キューを捨てた瞬間に、**送信側にも受信側にも #2〜#4 のデータが存在しなくなり、永久に再送できなくなってしまいます**。
   - 実際、やっと #1 が届いたのに受信側から `ack_seq` が進まず RTO（再送タイムアウト）が発火すると、送信側の `tcp_timeout_mark_lost()`（[net/ipv4/tcp_input.c#L2524](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2524)）は「先頭（`snd_una`）の `sk_buff` に `TCPCB_SACKED_ACKED` が付いたままタイムアウトした＝受信側が SACK Reneging した（`is_reneg = true`）」と検知し、**全 `sk_buff` の `TCPCB_SACKED_ACKED` フラグを剥がして（`sacked &= ~TCPCB_SACKED_ACKED`）、SACK 済みだったデータも最初から再送し直します**。
   - したがって、**累積 ACK（`snd_una`）が追い付くまでは、SACK されたデータも絶対に再送キューから解放してはならない** という鉄則があります。

2. **なぜ 1 個の `sk_buff` の一部が SACK されたときに分割（`tcp_fragment()`）しなければならないのか？**:
   - スコアボードの状態フラグ（`TCP_SKB_CB(skb)->sacked` の `S` = SACK 済み、`L` = ロス、`R` = 再送中）も、再送関数 `tcp_retransmit_skb()` が送り直す単位も、すべて **`struct sk_buff` 1 個単位** だからです（1 個の `sk_buff` の中にセグメントごとの状態ビット配列があるわけではありません）。
   - たとえば 10 セグメント（#1〜#10）入った TSO `sk_buff` のうち「#1 がロス、#2〜#4 が SACK 済み」になったとき、もしこの `sk_buff` を分割しなければ：
     - 1 個の `sk_buff` に「ロス（`L`）」と「SACK 済み（`S`）」を同時に付けることはできず、
     - #1 を再送するときに **#1 だけを送ることができず、すでに届いている #2〜#10 まで含めて 10 セグメント丸ごと再送する** ことになってしまいます。
   - そのため、「SACK されていない穴（#1）」と「SACK された範囲（#2〜#4）」の境界では、必ず別の `sk_buff` に切り分ける必要があります。

## そもそも「SACK shifting」とは何をしているのか？

TSO（TCP Segmentation Offload）を使うと、TCP はたとえば 10 セグメント（10 MSS）分のペイロードをまとめた 1 個の大きな `sk_buff` を作って送信し、NIC がワイヤに出す直前に 10 個のパケット（#1〜#10）に分割します。

ここで **1 番目のパケット（#1）だけが途中でロス** し、残りの #2〜#10 が相手に届いたとします。受信側はパケットが到着するたびに SACK（Selective ACK）ブロックで「どこまで届いたか」を報告してきます。

- **SACK shifting がなかった時代（`tcp_fragment()` による細切れ化）**:
  - 最初の SACK で「#2〜#4 が届いた」と来ると、10 MSS の塊だった `sk_buff` を「#1（未着）」「#2〜#4（SACK 済み）」「#5〜#10（未確定）」の 3 個の `sk_buff` に分割（`tcp_fragment()`）します。
  - 次の SACK で「#2〜#6 が届いた」と来ると、後半の「#5〜#10」をさらに「#5〜#6（SACK 済み）」と「#7〜#10（未確定）」に分割します。
  - これが続くと、**もともと 1 個だった TSO `sk_buff` が再送キュー（rbtree）の中で 1〜2 MSS ずつの細かい `sk_buff` にバラバラに断片化** し、メモリ（`sk_buff` ごとの管理オーバーヘッド）もキュー走査の CPU コストも激増してしまいます。

- **SACK shifting（`tcp_shift_skb_data()`、[net/ipv4/tcp_input.c#L1954](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1954)）の動き**:
  - 「#2〜#4（すでに SACK 済みの直前の `sk_buff` = `prev`）」のすぐ後ろにある `skb`（#5〜#10）のうち、新しく SACK された「#5〜#6」のデータを、**新しい `sk_buff` に切り出すのではなく、直前の `prev` の末尾へ引っ越し（shift / 吸収）させます**。

```
[SACK 到着前]
  prev (SACK 済み: #2..#4)        skb (未確定: #5..#10)
+--------------------------+   +----------------------------------+
| seq: #2..#4              |   | seq: #5..#10                     |
+--------------------------+   +----------------------------------+

[SACK で #2..#6 が通知されたとき: SACK shifting]
  prev (SACK 済み: #2..#6)        skb (未確定: #7..#10)
+----------------------------------+   +--------------------------+
| seq: #2..#6 (#5..#6 を吸収!)     |   | seq: #7..#10 (先頭が縮む)|
+----------------------------------+   +--------------------------+
```

さらに最後（#7〜#10）まで SACK されれば、`skb` の中身はすべて `prev` に吸い込まれ、空になった `skb` 自体は解放（merge）されます。これにより、**SACK 済みの連続領域は常に 1 個の大きな `sk_buff` にまとまった状態** を保てます。

## なぜ「ペイロードを線形領域（`skb->head`）に入れず、すべて `frags[]`（ページ frag）に入れる」とコピーなしで shift できるのか？

`sk_buff` がデータを持つ場所には、次の 2 種類があります（第 1 章）。

1. **線形領域（`skb->head` 〜 `skb->tail`、長さは `skb_headlen(skb)`）**:
   - `alloc_skb_fclone()` のときに TCP/IP ヘッダ用の領域と一続きで `kmalloc` された 1 枚の連続メモリです。
   - もしここにペイロード（データ本体）が入っていると、そのデータを隣の `sk_buff`（`prev`）に移すには **`memcpy()` でバイト列をコピーするしかありません**。そのためカーネルの `skb_can_shift()`（[net/ipv4/tcp_input.c#L1931](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_input.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1931)）および `skb_shift()`（[net/core/skbuff.c#L4327](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n4327)）は、**線形領域に 1 バイトでもデータが入っている `sk_buff`（`skb_headlen(skb) > 0`）の shift を即座に拒否** します。
   ```c
   /* Shifting pages past head area doesn't work */
   static int skb_can_shift(const struct sk_buff *skb)
   {
   	return !skb_headlen(skb) && skb_is_nonlinear(skb);
   }
   ```
2. **ページ frag 配列（`skb_shinfo(skb)->frags[]`）**:
   - データ本体は別ページ（`current->task_frag` の 32 KB ページなど）にあり、`sk_buff` 側は **「どのページの、何バイト目（`offset`）から、何バイト分（`size`）」という 3 つ組の配列（`skb_frag_t`）** を持っているだけです。
   - したがって、ペイロードが 100% `frags[]` に入っていれば、`skb` から `prev` へ `shiftlen` バイト移す処理（`skb_shift(prev, skb, shiftlen)`）は、**データ本体には一切触れず、`frags[]` 配列の `(page, offset, size)` を書き換えるだけ（ゼロコピー）** で完結します。
   - 特に `tcp_sendmsg_locked()` では同じ 32 KB の `task_frag` ページに順番に書き込んでいるため、`prev` の末尾 frag と `skb` の先頭 frag は **同じページの連続したオフセット** を指していることがほとんどです。その場合はさらに単純で、ページ参照すら増やさず **`prev` 側の frag の `size` に `+shiftlen` し、`skb` 側の frag の `offset` に `+shiftlen`、`size` に `-shiftlen` するだけ** でデータの引っ越しが終わります（[net/core/skbuff.c#L4369-L4371](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n4369)）。

第 3 章の `tcp_stream_alloc_skb()` が `alloc_skb_fclone(MAX_TCP_HEADER, gfp)` で「ヘッダ用の余白（ヘッドルーム）だけ確保し、ペイロードは 1 バイトも線形領域に入れずすべてページ frag に入れる」よう統一された（コミット `3613b3dbd1ad`、`5882efff88aa`）のは、すべての送信 `sk_buff` でこのゼロコピー SACK shifting（`!skb_headlen(skb)`）を常に効かせるためです。
