# Step 1: パケット数制限 FIFO Qdisc と Watchdog レート制御

[English](README.en.md)

## 1. 学習ゴール

このステップでは、Linux カーネルのトラフィック制御インターフェース `struct Qdisc_ops` を BPF `struct_ops` で実装し、最も基本的かつ重要なキューイング規律である **Packet-limited FIFO（pfifo）** をゼロから組み立てます。

キューのパケット数上限（`sch->limit = 4`）によるテールダンプ（Tail-drop）と、高精度タイマー（Watchdog）を用いた 2 ms ごとのボトルネックレート制御（ペーシング）を純粋な BPF コードとして実装することで、パケットのエンキュー・デキューのライフサイクルと、BPF 特有のデータ構造・ロック規約を完全に理解します。

- トラック全体の構成と背景知識は [Qdisc FIFO トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

設計チェックポイントに進む前に、FIFO キューの動作原理と、Linux カーネルが提供する Qdisc のフック・データ構造の仕組みを整理しましょう。

### 2.1 FIFO（First-In, First-Out）とパケット数制限（pfifo）

FIFO（先入れ先出し）は、到着した順序のままパケットをバッファリングし、到着順に送信側ハードウェア（NIC）へ引き渡す最もシンプルなキューイングアルゴリズムです。

キューの収容能力を「パケットの個数（Packet count）」で制限する方式を **pfifo（Packet FIFO）** と呼びます。
Linux カーネルの標準 Qdisc では `pfifo` や `pfifo_head_drop` がこれに該当します。このステップでは、キューの最大収容パケット数を `sch->limit = 4` に設定します。

### 2.2 テールダンプ（Tail-drop）のメカニズム

現在キュー内に滞留しているパケット数（`sch->q.qlen`）が上限 `sch->limit` に達している状態で、さらに新しいパケットが上流から届いた場合、キューにこれ以上格納する余地がありません。

このとき、**新しく到着したパケット自身を即座に破棄する** 方式を **テールダンプ（Tail-drop: 末尾破棄）** と呼びます。
テールダンプが発生した際、Qdisc は以下の処理を行います：
1. ドロップ統計カウンタ `sch->qstats.drops` をインクリメントする。
2. カーネルヘルパー `bpf_qdisc_skb_drop(skb, to_free)` を呼び出してパケットメモリを解放する。
3. カーネルへ `NET_XMIT_DROP`（`0x01`）を返して送信パケットの破棄を通知する。

### 2.3 ボトルネックレートと Watchdog タイマーによるペーシング

現実のネットワークでは、ルータや NIC の回線速度（リンク帯域）には物理的な上限があります。たとえば 1 パケットあたり 2 ms の送出時間を要するボトルネック回線（ $\text{BOTTLENECK NS} = 2,000,000\text{ ns}$ ）をシミュレートする場合、Qdisc はパケットを 2 ms に 1 個のペースで順次 NIC に引き渡さなければなりません。

Linux の Qdisc では、これを **Watchdog タイマー**（高精度ハードウェアタイマー `hrtimer` 連携）で実現します。

```text
時刻 t = 0 ms:
  P1 が届く -> キューが空なので直ちにデキューされて送信! 次の送信可能時刻 next_tx_ns = 2 ms に設定。
  P2..P5 が届く -> キューに格納 (qlen = 4)。
  P6 が届く -> qlen (4) >= limit (4) なのでテールダンプ!

時刻 t = 2 ms: Watchdog タイマー発火!
  P2 をデキューして送信。next_tx_ns = 4 ms に更新。

時刻 t = 4 ms: Watchdog タイマー発火!
  P3 をデキューして送信。next_tx_ns = 6 ms に更新。

時刻 t = 6 ms: Watchdog タイマー発火!
  P4 をデキューして送信。next_tx_ns = 8 ms に更新。

時刻 t = 8 ms: Watchdog タイマー発火!
  P5 をデキューして送信。next_tx_ns = 10 ms に更新。
```

### 2.4 具体数値トレース（6 パケットバースト送信時のキュー推移）

このラボの `test.pkt` で実際に検証する数値を時系列で追ってみましょう（初期設定: `sch->limit = 4`, `BOTTLENECK_NS = 2 ms`）。

| 時刻 | 発生イベント | キューの状態 (`qlen`) | バックログ (`backlog`) | 送出されるパケット | 備考 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **0.000s** | 接続確立・初期化 | `0` | `0` B | — | `bpf_pfifo_init()` で `sch->limit = 4`, `next_tx_ns = 0` |
| **+0.000s** | `write(4, ..., 6000)` (P1..P6 到着) | `0` | `0` B | **P1** (`1:1001`) | P1 はキューを経由せず直ちに送出。`next_tx_ns = now + 2ms` |
| **+0.000s** | P2..P5 のエンキュー | `4` | ~4000 B | — | P2, P3, P4, P5 がキューに蓄積（`qlen == 4 == limit`） |
| **+0.000s** | P6 のエンキュー試行 | `4` | ~4000 B | — | `qlen >= limit` のため **P6 はテールダンプ**（`drops++`） |
| **+0.002s** | Watchdog タイマー発火 | `3` | ~3000 B | **P2** (`1001:2001`) | P2 をデキュー。`next_tx_ns = now + 2ms` に更新 |
| **+0.004s** | Watchdog タイマー発火 | `2` | ~2000 B | **P3** (`2001:3001`) | P3 をデキュー。`next_tx_ns = now + 2ms` に更新 |
| **+0.006s** | Watchdog タイマー発火 | `1` | ~1000 B | **P4** (`3001:4001`) | P4 をデキュー。`next_tx_ns = now + 2ms` に更新 |
| **+0.008s** | Watchdog タイマー発火 | `0` | `0` B | **P5** (`4001:5001`) | P5 をデキュー。キューが空になる |

### 2.5 カーネルが提供する Qdisc 構造体・フィールド・ヘルパー一覧

#### (A) `struct Qdisc` の主要フィールド

- `sch->limit` (`__u32`): キューが保持できる最大パケット数。`.init` フックで設定します。
- `sch->q.qlen` (`__u32`): 現在キュー内に格納されているパケット数。エンキュー成功時に `+1`、デキュー時に `-1` します。
- `sch->qstats.drops` (`__u32`): ドロップされたパケットの累積数。
- `sch->qstats.backlog` (`__u32`): 現在キュー内に滞留している全パケットのバイト数合計。
- `sch->flags`: Qdisc の動作フラグ（カーネルが管理）。

#### (B) 主要な BPF ヘルパーおよび kfuncs

- `qdisc_pkt_len(skb)`: パケットの送信バイト長（L2/L3/L4 ヘッダ＋ペイロード）を取得します。
- `bpf_qdisc_skb_drop(skb, to_free)`: パケットを破棄リストへ追加し、安全にドロップします。
- `bpf_qdisc_bstats_update(sch, skb)`: デキューされたパケットのバイト数・パケット数を Qdisc の送信統計（bstats）へ加算します。
- `bpf_qdisc_watchdog_schedule(sch, expire, delta_ns)`: Watchdog タイマーを指定時刻 `expire`（ナノ秒）にセットします。
- `bpf_kfree_skb(skb)`: `.reset` フックなどでパケットを直接解放します。
- `bpf_obj_new(type)` / `bpf_obj_drop(ptr)`: BPF メモリプーラからオブジェクトを安全に確保・解放します。
- `bpf_kptr_xchg(dst, src)`: ポインタのアトミックな交換を行い、所有権を移動します。
- `bpf_list_push_back(head, node)` / `bpf_list_pop_front(head)`: BPF 侵入型リストへの追加・取り出しを行います。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の内容を踏まえて、`starter.bpf.c` の TODO を埋めるための 4 つの設計の問いを自分の頭で考えてみましょう。

### Q1. メモリ所有権の設計：なぜ `bpf_kptr_xchg` を呼ぶ「前」に `qdisc_pkt_len(skb)` を取得しなければならないのか？

`bpf_pfifo_enqueue()` では、ノード `skbn` に `skb` を格納する前に `u32 pkt_len = qdisc_pkt_len(skb);` を呼んでいます。もし `bpf_kptr_xchg(&skbn->skb, skb)` を呼んだ後に `qdisc_pkt_len(skb)` を読もうとするとどうなるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `bpf_kptr_xchg(&skbn->skb, skb)` を実行すると、ローカル変数 `skb` の所有権はどう変化するでしょうか？
- **答え**: **BPF verifier によってメモリアクセス違反として拒否されます。**
  `bpf_kptr_xchg(&skbn->skb, skb)` が成功すると、ポインタ `skb` の所有権はノード `skbn` へ完全に移動し、関数内のローカル変数 `skb` は無効化（NULL または古い値）されます。所有権を手放した後のポインタを逆参照することは BPF verifier によって禁じられているため、必ず `bpf_kptr_xchg` の前に `pkt_len` を読み出しておく必要があります。

</details>

### Q2. ロック規約の設計：なぜ `bpf_spin_lock` を保持したまま `bpf_kptr_xchg` や `bpf_obj_drop` を呼んではいけないのか？

デキュー処理（`bpf_pfifo_dequeue`）において、リストからノードを取り出す操作（`bpf_list_pop_front`）と、ノードから `skb` を取り出してノードを解放する操作（`bpf_kptr_xchg`, `bpf_obj_drop`）の順序はどうあるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: Linux カーネルの BPF verifier における `bpf_spin_lock` のクリティカルセクションのルールを確認してください。
- **答え**: **`bpf_spin_lock` の保持区間内では、リスト操作（`bpf_list_pop_front` 等）のみを行い、直ちに `bpf_spin_unlock` してから `bpf_kptr_xchg` と `bpf_obj_drop` を呼ぶ必要があります。**
  BPF スピンロックはデッドロック防止とスリープ不可保証のため、ロック保持中に呼べるヘルパー・kfunc が極めて厳しく制限されています。メモリ解放（`bpf_obj_drop`）や kptr 交換（`bpf_kptr_xchg`）をロック保持中に呼ぶと、verifier がプログラムのロードを拒否します。

</details>

### Q3. テールダンプの設計：キューが満杯（`sch->q.qlen >= sch->limit`）のとき、何を更新して何を返すべきか？

`bpf_pfifo_enqueue()` でキュー上限に達した場合の正しい処理手順はどうなりますか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: 統計カウンタの更新、パケットの安全な破棄、カーネルへの戻り値の 3 点に注目してください。
- **答え**:
  ```c
  if (sch->q.qlen >= sch->limit) {
      sch->qstats.drops++;
      bpf_qdisc_skb_drop(skb, to_free);
      return NET_XMIT_DROP;
  }
  ```
  `sch->qstats.drops` をインクリメントし、`bpf_qdisc_skb_drop(skb, to_free)` でパケットをカーネルの解放リストに登録し、`NET_XMIT_DROP` を返します。

</details>

### Q4. デキュー時のレート制御：送出時刻がまだ訪れていない場合、Watchdog タイマーをどうセットしてリターンすべきか？

現在時刻 `now` が次回の送信予定時刻 `next_tx_ns` より小さい（`now < next_tx_ns`）場合、`.dequeue` はどのような処理を行うべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: パケットを送出せずに、指定時刻にカーネルから再度 `.dequeue` を呼んでもらうための kfunc は何でしょうか？
- **答え**:
  ```c
  if (now < next_tx_ns) {
      bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
      return NULL;
  }
  ```
  `bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0)` を呼び出し、カーネルに `next_tx_ns` でのタイマー割り込みを予約して `NULL` を返します。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step01-pfifo/solution.bpf.c) の各関数と実装内容を確認します。

### 4.1 データ構造とグローバル状態

```c
#define BOTTLENECK_NS 2000000ULL /* 2 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_fifo_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
static __u64 next_tx_ns;
```

- `skb_node`: パケットポインタ `skb`（`__kptr`）と、侵入型リンクリスト用の `node` を持つ構造体です。
- `private(A)`: BPF データセクション `.data.A` に配置され、同一 BTF 名前空間内でスピンロックとリストヘッドを安全に関連付けます。
- `next_tx_ns`: 次回パケットを送出してよい最短のカーネル時刻（ナノ秒）。

### 4.2 `bpf_pfifo_init()` — Qdisc 初期化

```c
SEC("struct_ops/bpf_pfifo_init")
int BPF_PROG(bpf_pfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 4;
	next_tx_ns = 0;
	return 0;
}
```

- Qdisc 作成時に呼ばれ、キューのパケット数上限を `sch->limit = 4` に設定し、`next_tx_ns` を `0` に初期化します。

### 4.3 `bpf_pfifo_enqueue()` — エンキューとテールダンプ（TODO 1〜4）

```c
SEC("struct_ops/bpf_pfifo_enqueue")
int BPF_PROG(bpf_pfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	struct skb_node *skbn;
	u32 pkt_len;

	if (sch->q.qlen >= sch->limit) {
		sch->qstats.drops++;
		bpf_qdisc_skb_drop(skb, to_free);
		return NET_XMIT_DROP;
	}

	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn) {
		sch->qstats.drops++;
		bpf_qdisc_skb_drop(skb, to_free);
		return NET_XMIT_DROP;
	}

	pkt_len = qdisc_pkt_len(skb);

	skb = bpf_kptr_xchg(&skbn->skb, skb);
	if (skb)
		bpf_qdisc_skb_drop(skb, to_free);

	bpf_spin_lock(&q_fifo_lock);
	bpf_list_push_back(&q_fifo, &skbn->node);
	bpf_spin_unlock(&q_fifo_lock);

	sch->q.qlen++;
	sch->qstats.backlog += pkt_len;
	return NET_XMIT_SUCCESS;
}
```

- **TODO 1**: `sch->q.qlen >= sch->limit` のとき即座にテールダンプします。
- **TODO 2**: `bpf_obj_new` でノードを確保し、失敗時はドロップします。
- **TODO 3**: `bpf_kptr_xchg` の前に `qdisc_pkt_len(skb)` を取得し、`skb` の所有権をノードへ移します。
- **TODO 4**: スピンロックを取得してリストの末尾（`push_back`）へ追加し、`sch->q.qlen++` と `sch->qstats.backlog += pkt_len` を更新します。

### 4.4 `bpf_pfifo_dequeue()` — デキューと Watchdog レート制御（TODO 5〜9）

```c
SEC("struct_ops/bpf_pfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_pfifo_dequeue, struct Qdisc *sch)
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

	bpf_spin_lock(&q_fifo_lock);
	node = bpf_list_pop_front(&q_fifo);
	bpf_spin_unlock(&q_fifo_lock);
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

- **TODO 5 & 6**: 空キュー判定および Watchdog タイマーによるレート制御。`now < next_tx_ns` ならタイマーを予約して `NULL` を返します。
- **TODO 7 & 8**: スピンロック下でリスト先頭からノードを取り出し（`pop_front`）、ロック解除後に `skb` を取り出してノードを解放（`bpf_obj_drop`）します。
- **TODO 9**: 次回送出可能時刻を `now + BOTTLENECK_NS`（2 ms 後）にセットし、統計と `sch->q.qlen--` を更新して `skb` を返します。

### 4.5 `bpf_pfifo_reset()` — キューの安全なフラッシュ

```c
SEC("struct_ops/bpf_pfifo_reset")
void BPF_PROG(bpf_pfifo_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&q_fifo_lock);
		node = bpf_list_pop_front(&q_fifo);
		bpf_spin_unlock(&q_fifo_lock);

		if (!node)
			break;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		if (skb)
			bpf_kfree_skb(skb);
		bpf_obj_drop(skbn);
	}
	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
	next_tx_ns = 0;
}
```

- `bpf_for` 反復子を使ってキュー内の全ノードを取り出し、`bpf_kfree_skb(skb)` でパケットを解放してキュー長とバックログをゼロにリセットします。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### 5.1 [`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/qdisc-fifo/step01-pfifo/test.pkt) のシナリオ解説

`test.pkt` は、セクション 2.4 の数値トレースと完全に一致する 6 パケットのバースト送信を行い、キューイング、レート制御、テールダンプを厳密に検証します。

1. **Qdisc のアタッチとオフロード無効化（4〜5 行目）**:
   - `tc qdisc replace dev tun0 root bpf_pfifo`: 作成した BPF Qdisc を `tun0` の root Qdisc に設定します。
   - `ethtool -K tun0 tso off gso off`: TSO/GSO によるパケット集約を無効化し、TCP が 1 パケット（MSS = 1000 バイト）ごとに Qdisc へ引き渡すようにします。
2. **MSS = 1000 バイトの TCP コネクション確立（8〜16 行目）**:
   - 3-way ハンドシェイクを行い、初期 `cwnd = 10` の接続を確立します。
3. **6 パケットバースト送信とレート制御・テールダンプ検証（18〜30 行目）**:
   - `write(4, ..., 6000)` により、TCP は 6 つのセグメントを一気に送出します。
   - Packet 1 (`1:1001`) は `+0.00` 秒で即座に NIC へ送出されます。
   - Packets 2〜5 は Qdisc 内にバッファリングされ、Watchdog タイマーによって 2 ms 間隔（`+0.002`）で順番に送出されます。
   - Packet 6 (`5001:6001`) は `sch->limit = 4` に引っかかり、テールダンプされます。
4. **ドロップ統計の検証（33 行目）**:
   - `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"` を実行し、Qdisc が実際にパケットをドロップしたことを検証します。

### 5.2 なぜ `starter.bpf.c` は失敗し、`solution.bpf.c` はパスするのか？

- **`starter.bpf.c` が失敗する理由**: `starter.bpf.c` の `.enqueue` はすべてのパケットを即座にドロップ（`NET_XMIT_DROP`）し、`.dequeue` は常に `NULL` を返すため、ハンドシェイクの SYN/ACK すら送信できず、パケット送出の照合で即座にテスト失敗となります。
- **`solution.bpf.c` がパスする理由**: パケットのエンキュー、2 ms 間隔の Watchdog デキュー、4 パケット上限でのテールダンプがすべて正しく動作するため、`test.pkt` のすべてのタイムライン検証を通過します。

### 5.3 テストの実行コマンド

```sh
# starter の失敗を確認する
../../run_test.sh qdisc-fifo/step01-pfifo starter

# solution（模範解答）の合格を確認する
../../run_test.sh qdisc-fifo/step01-pfifo solution
```
