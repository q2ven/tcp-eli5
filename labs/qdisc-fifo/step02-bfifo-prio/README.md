# Step 2: 2 バンド優先度付きバイト数制限 FIFO Qdisc

[English](README.en.md)

## 1. 学習ゴール

このステップでは、Step 1 の基本 FIFO を発展させ、バイト数制限（Byte-limited FIFO: bfifo）と 2 バンドの優先度付きスケジューリング（Strict Priority Scheduling）を組み合わせた **2-Band Priority + Byte-limited FIFO Qdisc（`bpf_bfifo`）** を実装します。

通常トラフィックに対してバイト数上限（`BYTE_LIMIT = 3500` バイト）を課してバッファ溢れを防ぎつつ、制御トラフィックや優先パケット（`SO_PRIORITY = 7` / `TC_PRIO_CONTROL`）に対してはバイト制限をバイパスさせ、キュー内で待機中の通常パケットを追い越して最優先でデキュー（先頭送出）される仕組みをゼロから構築します。

- トラック全体の構成と背景知識は [Qdisc FIFO トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

設計チェックポイントに進む前に、なぜパケット数制限だけでは不十分なのか、そして優先度制御がどのように遅延を劇的に削減するかを理解しましょう。

### 2.1 なぜパケット数制限（pfifo）だけでは不十分なのか？（バイト数制限の必要性）

Step 1 で扱ったパケット数制限（`sch->limit`）には、ネットワーク工学上の大きな弱点があります。それは **「パケット 1 個あたりのサイズが大きく異なる」** という点です。

- 単なる TCP ACK パケットや SYN パケット: 約 **64 バイト**
- 通常の TCP データパケット: 約 **1500 バイト**（23 倍以上のサイズ）

もし `sch->limit = 1000` パケットと設定した場合、小さな ACK パケットばかりなら 64 KB のメモリしか消費しませんが、最大長データパケットばかりなら 1.5 MB ものメモリを消費し、回線の占有時間（キューイング遅延）も 23 倍に膨らんでしまいます。

そこで、キューの滞留限界をパケット個数ではなく **「キュー内に溜まっている合計バイト数（Backlog）」** で制限する方式を **bfifo（Byte FIFO）** と呼びます。
本ステップでは、通常パケットに対するバイト上限を `BYTE_LIMIT = 3500` バイトに設定し、`sch->qstats.backlog + pkt_len > BYTE_LIMIT` となった場合にパケットを破棄します。

### 2.2 2 バンド厳格優先スケジューリング（Strict Priority Scheduling）

インターネット通信では、すべてのパケットを平等に扱うと深刻な問題（Head-of-Line Blocking: 先頭ブロッキング）が発生します。
たとえば、動画ストリーミングや大容量ファイル転送でキューが満杯になっているとき、DNS クエリや SSH のキーストローク、TCP の接続確立パケット（SYN）がその末尾に並ばされると、数百ミリ秒もの通信遅延（ラグ）が発生してしまいます。

これを解決するのが **優先度付きキューイング（Priority Queueing / PRIO）** です。
本ステップでは、2 つの独立したキューを用意します：
- **`q_prio`（優先キュー）**: 優先フラグが付与された制御パケットを格納します。バイト数制限（`BYTE_LIMIT`）をバイパスし、全パケット上限（`sch->limit = 8`）まで格納できます。
- **`q_normal`（通常キュー）**: 通常のトラフィックを格納します。`BYTE_LIMIT = 3500` バイトを超えるとテールダンプされます。

```text
パケット到着 (.enqueue)
        |
        +---> is_prio == true  ---> q_prio に格納 (BYTE_LIMIT バイパス)
        |
        +---> is_prio == false ---> backlog + pkt_len > 3500 ?
                                        ├── Yes -> ドロップ (Tail-drop)
                                        └── No  -> q_normal に格納

パケット送出 (.dequeue)
        |
        +---> q_prio にパケットがあるか？
                  ├── Yes -> q_prio からポップして送出! (優先割り込み)
                  └── No  -> q_normal からポップして送出
```

### 2.3 具体数値トレース（通常フロー A と優先フロー B の追い越し）

このステップの `test.pkt` で検証する具体的なシナリオ（`BYTE_LIMIT = 3500` バイト、`BOTTLENECK_NS = 4 ms`）を追ってみましょう。

1. **通常フロー A (`fd = 4`) が 5000 バイト（5 セグメント）を一気にバースト送信**:
   - 各セグメントのパケット長 `pkt_len` は約 1066 バイト（MSS 1000 + ヘッダ 66 バイト）。
   - **Seg 1** (`1:1001`): 到着時キューが空のため即時デキュー（`t = 0 ms`）、`next_tx_ns = 4 ms` に設定。
   - **Seg 2** (`1001:2001`): `q_normal` に格納。`backlog = 1066`（$\le 3500$）。
   - **Seg 3** (`2001:3001`): `q_normal` に格納。`backlog = 2132`（$\le 3500$）。
   - **Seg 4** (`3001:4001`): `q_normal` に格納。`backlog = 3198`（$\le 3500$）。
   - **Seg 5** (`4001:5001`): $3198 + 1066 = 4264 > 3500$。**`BYTE_LIMIT` 超過によりテールダンプ！**
2. **直後（`t = 0 ms`）、優先フロー B (`fd = 6`, `SO_PRIORITY = 7`) が 500 バイト（1 セグメント）を送信**:
   - `is_prio` が真のため `BYTE_LIMIT` をバイパスし、`q_prio` に格納される。
3. **送出順序の逆転（優先割り込み）**:
   - **`t = 0 ms`**: フロー A の Seg 1 が送出中。
   - **`t = 4 ms`**: Watchdog タイマーが発火。.dequeue は `q_prio` を最優先で確認するため、後から届いた **フロー B のパケット（`1:501`）が、先に待っていたフロー A の Seg 2〜4 を差し置いて真っ先に送出される！**
   - **`t = 8 ms`**: `q_prio` が空になったため、`q_normal` からフロー A の Seg 2 (`1001:2001`) が送出される。
   - **`t = 12 ms`**: フロー A の Seg 3 (`2001:3001`) が送出される。
   - **`t = 16 ms`**: フロー A の Seg 4 (`3001:4001`) が送出される。

| 時刻 | 実行イベント | `q_prio` | `q_normal` | 送出パケット | 備考 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0.000s** | フロー A バースト (5 パケット) | 空 | Seg 2, 3, 4 (3198 B) | **Flow A Seg 1** (`1:1001`) | Seg 5 は `BYTE_LIMIT` 超過でドロップ |
| **+0.000s** | フロー B 送信 (500 B, prio) | Prio 1 (566 B) | Seg 2, 3, 4 (3198 B) | — | フロー B が `q_prio` に割り込み |
| **+0.004s** | Watchdog タイマー発火 | 空 | Seg 2, 3, 4 (3198 B) | **Flow B** (`1:501`) | **フロー A を追い越して最優先送出！** |
| **+0.008s** | Watchdog タイマー発火 | 空 | Seg 3, 4 (2132 B) | **Flow A Seg 2** (`1001:2001`) | 通常キューから送出 |
| **+0.012s** | Watchdog タイマー発火 | 空 | Seg 4 (1066 B) | **Flow A Seg 3** (`2001:3001`) | 通常キューから送出 |
| **+0.016s** | Watchdog タイマー発火 | 空 | 空 (0 B) | **Flow A Seg 4** (`3001:4001`) | 全キューが空になる |

### 2.4 カーネル構造体・フィールド・ヘルパー一覧

- `TC_PRIO_CONTROL`: カーネルで定義された最高優先度クラス（値 `7`）。ソケットの `setsockopt(fd, SOL_SOCKET, SO_PRIORITY, [7], 4)` で設定できます。
- `TC_PRIO_MAX`: 優先度マスク（値 `15`）。
- `skb->priority`: ソケットからパケットに引き継がれた優先度フィールド。`(skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL` で制御優先度パケットかどうかを判定できます。
- `private(A) struct bpf_spin_lock q_lock`: 1 つのスピンロックで同一セクション内の複数リスト（`q_prio` と `q_normal`）をアトミックに保護できます。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

### Q1. バイト数制限の計算：なぜ `sch->qstats.backlog + pkt_len > BYTE_LIMIT` と判定するのか？

`bpf_bfifo_enqueue()` でバイト数制限をチェックする際、なぜ現在のバックログ `sch->qstats.backlog` だけでなく `+ pkt_len` を足して比較する必要があるのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: パケットがキューに入った後の状態を想像してください。
- **答え**: **このパケットを受け入れた結果として上限を超えるかどうか（事後サイズ）を事前に検査するためです。**
  もし `sch->qstats.backlog > BYTE_LIMIT` と判定してしまうと、上限ギリギリ（例: 3400 バイト）のときに 1500 バイトのパケットが到着すると合計 4900 バイトまで膨らんでしまい、`BYTE_LIMIT = 3500` の厳格な上限を守れなくなってしまいます。

</details>

### Q2. 優先度判定の設計：なぜビットマスク `& TC_PRIO_MAX` をとるのか？

`skb->priority` を直接比較せず、`(skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL` とマスクをかけるのはなぜでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: Linux カーネルにおける `skb->priority` の上位ビットの使われ方を考慮してください。
- **答え**: **上位ビットにサブクラスやトラフィッククラス情報が含まれている場合でも、標準優先度（0〜15）を正しく抽出するためです。**
  カーネルの TC サブシステムでは、`TC_PRIO_MAX`（`15` / `0xf`）でマスクした下位 4 ビットが基本優先度バンドを表す規約となっています。

</details>

### Q3. デキューの優先度判定：優先キューと通常キューの調停はどう書くべきか？

`.dequeue` で「まず `q_prio` を試し、空なら `q_normal` を試す」処理を、BPF のスピンロック下で最もシンプルかつエレガントに書くにはどうすればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `bpf_list_pop_front(&q_prio)` の結果が NULL だった場合に次のポップを呼び出します。
- **答え**:
  ```c
  bpf_spin_lock(&q_lock);
  node = bpf_list_pop_front(&q_prio);
  if (!node)
      node = bpf_list_pop_front(&q_normal);
  bpf_spin_unlock(&q_lock);
  ```
  このように書けば、1 回のロック保持区間内で最優先ノードを確実に取得でき、コードもわずか 5 行で安全に完結します。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step02-bfifo-prio/solution.bpf.c) の主要部分を確認します。

### 4.1 データ構造の定義

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

- 同一の `private(A)` セクションに `q_prio` と `q_normal` の 2 つのリストヘッドを定義し、1 つの `q_lock` で両方を安全に保護します。

### 4.2 `bpf_bfifo_enqueue()` — 優先度振り分けとバイト制限（TODO 1〜5）

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

- **TODO 2 & 3**: `is_prio` を判定し、通常パケット（`!is_prio`）かつ `backlog + pkt_len > BYTE_LIMIT` の場合は直ちに `goto drop;` します。
- **TODO 5**: `is_prio` が真なら `q_prio` へ、偽なら `q_normal` へ振り分けて `push_back` します。

### 4.3 `bpf_bfifo_dequeue()` — 優先キュー先行デキュー（TODO 6〜9）

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

- `q_prio` にパケットが存在する間はそちらを最優先で排出し、`q_prio` が空になった時だけ `q_normal` からパケットを取り出します。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### 5.1 [`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step02-bfifo-prio/test.pkt) のシナリオ解説

1. **通常フロー A (`fd = 4`) と優先フロー B (`fd = 6`) の並行確立**:
   - フロー A は通常のソケットとして接続。
   - フロー B は `setsockopt(6, SOL_SOCKET, SO_PRIORITY, [7], 4)` で `TC_PRIO_CONTROL = 7` を設定。
2. **フロー A のバーストとバイト制限ドロップ（29 行目）**:
   - `write(4, ..., 5000)` により、Seg 1 が即時送出され、Seg 2〜4 が `q_normal` に蓄積。Seg 5 は 3500 バイト上限を超えてドロップされます。
3. **フロー B の割り込み送信と追い越し送出（33〜39 行目）**:
   - `write(6, ..., 500)` でフロー B が 1 パケット送信。
   - `+0.004` 秒（4 ms 後）に、キュー内で待機していたフロー A の Seg 2 よりも前に **フロー B のパケット (`1:501`) がデキューされる** ことを検証します。
4. **通常トラフィックの再開とドロップ確認（42〜48 行目）**:
   - フロー B の送出完了後、残っていたフロー A の Seg 2, 3, 4 が 4 ms 間隔で順次送出され、最後に `tc -s qdisc show dev tun0` でドロップの発生を確認します。

### 5.2 テストの実行コマンド

```sh
# starter の失敗を確認する
../../run_test.sh qdisc-fifo/step02-bfifo-prio starter

# solution（模範解答）の合格を確認する
../../run_test.sh qdisc-fifo/step02-bfifo-prio solution
```
