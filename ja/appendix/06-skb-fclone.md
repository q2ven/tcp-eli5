# `alloc_skb_fclone()` と `struct sk_buff_fclones`：なぜ最初から双子の `sk_buff` を確保するのか

**関連箇所**: [第 3 章: `tcp_stream_alloc_skb()`](../03-sendmsg.md)、[第 4 章: `__tcp_transmit_skb()`](../04-write-xmit.md)

## なぜ TCP は送信のたびに `sk_buff` を clone するのか？

UDP のように再送を行わないプロトコルでは、作った `struct sk_buff` をそのまま IP 層 → Qdisc → NIC ドライバへ渡し、NIC の DMA 送信が終わった時点でドライバが `consume_skb()` を呼んで解放してしまいます。

しかし TCP では、**送信したパケットがネットワークの途中でロスするかもしれないため、相手から ACK が返ってくるまで送信済みデータを再送キュー（`tcp_rtx_queue`）に保管し続けなければなりません**。一方で、下位層（IP・Qdisc・NIC ドライバ）に渡した `struct sk_buff` は、キューのポインタ（`next` / `prev` / `rbnode`）やヘッダポインタ（`data` / `transport_header` / `network_header`）が書き換えられ、DMA 送信完了と同時に解放されてしまいます。

そのため TCP は、`__tcp_transmit_skb()` でパケットを送り出すたびに **`skb_clone()`** を呼びます。
- **注意**: `skb_clone()` はパケットのデータ本体（ペイロードのページ群や `skb->head` の線形バッファ）は一切コピーしません。データ領域の参照カウント（`skb_shinfo(skb)->dataref`）を `+1` するだけで、**管理用の構造体である `struct sk_buff`（約 232 バイト）だけをもう 1 個複製** します。
- **原本の `sk_buff`（`oskb`）**: TCP ソケットの `tcp_rtx_queue` に居残り続け、ACK が来たら解放されます。
- **クローンされた `sk_buff`（`skb`）**: TCP/IP/Ethernet ヘッダを書き込まれて IP 層 → Qdisc → NIC ドライバへ旅立ち、NIC の DMA 送信が終わったら解放されます。

## 通常の `skb_clone()` の無駄と `fclone`（Fast Clone）の発想

通常の `alloc_skb()` で確保した `sk_buff` に対して `skb_clone()` を呼ぶと、クローン用の `struct sk_buff` を作るためにスラブアロケータ（`kmem_cache_alloc(skbuff_cache)`）をもう一度呼ぶ必要があり、解放時も `kmem_cache_free()` が 2 回（NIC 送信完了時と ACK 到着時）走ります。

しかし、TCP の送信パス（`tcp_stream_alloc_skb()`）で作る `sk_buff` は、**「あとで `__tcp_transmit_skb()` で 100% 確実に 1 回以上 `skb_clone()` される」ことが最初から分かっています**。

そこで `alloc_skb_fclone()`（[include/linux/skbuff.h#L1437](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/linux/skbuff.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1437)）は、最初から **`struct sk_buff` 2 個分（`skb1` と `skb2`）と参照カウンタ `fclone_ref` が 1 塊になった構造体 `struct sk_buff_fclones`** を専用スラブキャッシュ（`skbuff_fclone_cache`）から一括で確保します（[include/linux/skbuff.h#L1401](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/linux/skbuff.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1401)）。

```c
/* Layout of fast clones : [skb1][skb2][fclone_ref] */
struct sk_buff_fclones {
	struct sk_buff	skb1;

	struct sk_buff	skb2;

	refcount_t	fclone_ref;
};
```

```
skbuff_fclone_cache の 1 スラブオブジェクト (struct sk_buff_fclones):
+---------------------------+---------------------------+------------+
| skb1 (SKB_FCLONE_ORIG)    | skb2 (SKB_FCLONE_CLONE)   | fclone_ref |
| TCP 再送キューに残る原本  | 下位層・NIC に渡す予備席  |   1 -> 2   |
+---------------------------+---------------------------+------------+
              |                           |
              +-------------+-------------+
                            v
      同じデータバッファ (skb->head / skb_shared_info / page frags) を共有
```

## 確保・送信（clone）・解放の 3 ステップ

1. **確保時（`alloc_skb_fclone()` → `__alloc_skb()`）**:
   - `skbuff_fclone_cache` から `struct sk_buff_fclones` を 1 個取り出します。
   - `skb1.fclone = SKB_FCLONE_ORIG`、`fclone_ref = 1` に初期化し、`&fclones->skb1` を返します。隣にある `skb2` はまだ空席のまま隣に待機しています。
2. **送信時（`__tcp_transmit_skb()` → `skb_clone()`、[net/core/skbuff.c#L2091](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2091)）**:
   ```c
   if (skb->fclone == SKB_FCLONE_ORIG &&
       refcount_read(&fclones->fclone_ref) == 1) {
   	n = &fclones->skb2;
   	refcount_set(&fclones->fclone_ref, 2);
   	n->fclone = SKB_FCLONE_CLONE;
   }
   ```
   - `skb1` が `SKB_FCLONE_ORIG` で、かつ `fclone_ref == 1`（隣の `skb2` が未使用）であれば、**スラブアロケータを一切呼ばずに、すぐ隣のアドレス `&fclones->skb2` をポインタ計算だけで取り出し**、`fclone_ref = 2` にして `skb1` のフィールドをコピーします。
3. **解放時（`kfree_skbmem()`、[net/core/skbuff.c#L1130](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/core/skbuff.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1130)）**:
   - 通常は数マイクロ秒後に NIC の DMA 送信が先に終わり、ドライバがクローン側（`skb2`）を解放します。このとき `refcount_dec_and_test(&fclones->fclone_ref)` で `fclone_ref` が `2 → 1` に減るだけで、スラブ解放は行われません（ちなみに、このあとタイムアウト等で **再送** が必要になった場合、`fclone_ref` が `1` に戻っているため、同じ `skb2` をもう一度再利用してクローンできます）。
   - その後、相手から ACK が届いて TCP が原本（`skb1`）を解放するとき、`kfree_skbmem()` は `skb->fclone == SKB_FCLONE_ORIG` かつ `refcount_read(&fclones->fclone_ref) == 1` を見て、アトミック減算すら省いて即座に `kmem_cache_free(skbuff_fclone_cache, fclones)` を 1 回だけ呼び、ペアごとスラブキャッシュに返却します。
