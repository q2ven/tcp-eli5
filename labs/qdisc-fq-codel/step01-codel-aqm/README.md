# Step 1: 単一キューにおける CoDel (Controlled Delay) AQM

[English](README.en.md)

## 1. 学習ゴール

このステップでは、Linux カーネルの Qdisc インターフェースである `struct Qdisc_ops` を BPF の `struct_ops` 機能で実装し、**Bufferbloat を解決する CoDel（RFC 8289）能動的キュー管理（AQM: Active Queue Management）** の根幹アルゴリズムをゼロから組み立てます。

- トラック全体の背景や Bufferbloat のメカニズムについては [トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説

### 2.1 キュー長ではなく「パケット滞在時間（Sojourn Time）」を測る

従来の FIFO キューにおける Tail-Drop（末尾破棄）や RED（Random Early Detection）は、キューにたまっている「パケット数」や「バイト数」に基づいてパケットを破棄していました。

しかし、キュー長に基づく制御には根本的な欠陥があります。
- $10\text{ Gbps}$ の超高速回線では、 $100$ パケットのキューはわずか $0.12\text{ ms}$ で捌けます。
- 一方、 $1\text{ Mbps}$ の低速回線では、同じ $100$ パケットのキューを捌くのに $1.2\text{ 秒}$ もかかります。

同じパケット数であっても、回線帯域によって遅延は桁違いに異なります。そこで CoDel は、キューの長さではなく **「パケットがキューに並んでいた時間（滞在時間: Sojourn Time）」** を直接計測して判断します。

$$\text{sojourn} = t_{\text{dequeue}} - t_{\text{enqueue}}$$

### 2.2 良いキュー（Good Queue）と悪いキュー（Bad Queue）の判別

- **良いキュー（一過性のバースト）**:
  - Web ページの読み込みやアプリの初期通信などで、瞬間的にパケットがまとまって到着します。
  - 回線が健全であれば、バッファで受け止めたパケットはすぐに掃き出され、Sojourn Time は一時的に上昇してもすぐに $0$ に戻ります。
- **悪いキュー（持続的な滞留キュー / Standing Queue）**:
  - 送信側 TCP が巨大なウィンドウでパケットを流し込み続け、ボトルネックリンクの処理能力を超えてキューが常時満杯のまま推移します。
  - Sojourn Time が長期間にわたって高い値を維持し続けます。

CoDel は、**「`INTERVAL`（観測ウィンドウ）の間、最低滞在時間がずっと `TARGET` を超え続けているか？」** を監視することで、一時的なバーストを見逃し、有害な滞留キューだけを正確に特定します。

### 2.3 CoDel の状態マシン

CoDel は以下の 2 つの主要パラメータで動作します：
- **`TARGET_NS`**: 目標滞在時間（標準 5 ms = $5,000,000\text{ ns}$）。
- **`INTERVAL_NS`**: 観測ウィンドウ（標準 100 ms、本ラボではテスト時間短縮のため $20\text{ ms} = 20,000,000\text{ ns}$）。

```mermaid
flowchart TD
    Start["パケットをキューから取り出す (bpf_codel_dequeue)"] --> Calc["sojourn = now - enqueue_time を計算"]
    Calc --> CondTarget{"sojourn < TARGET_NS ?"}

    CondTarget -->|Yes: 正常 (Good Queue)| Reset["first_above_time = 0\ndropping = false\n(正常送信へ)"]
    CondTarget -->|No: 遅延過大| CheckTimer{"first_above_time == 0 ?"}

    CheckTimer -->|Yes: 初めて TARGET を超過| ArmTimer["first_above_time = now + INTERVAL_NS\n(即座には落とさず監視開始)"]
    CheckTimer -->|No: 既に監視中| CheckExpire{"now >= first_above_time ?"}

    CheckExpire -->|No: まだ INTERVAL 経過前| NormalSend["一過性の可能性あり: パケット送信"]
    CheckExpire -->|Yes: INTERVAL の間ずっと遅延過大!| DropState["dropping = true\n先頭パケットを Head-Drop!\n次パケットを即座にデキューして送信"]

    Reset --> SendWire["NIC / 回線へパケット送出"]
    ArmTimer --> SendWire
    NormalSend --> SendWire
    DropState --> SendWire
```

1. **`sojourn < TARGET_NS` の場合**:
   - キュー遅延は健全です。`first_above_time = 0` および `dropping = false` にリセットし、パケットを通常送出します。
2. **`sojourn >= TARGET_NS` の場合**:
   - `first_above_time == 0`（初回超過）であれば、`first_above_time = now + INTERVAL_NS` に設定し、監視タイマーを開始します（このパケットはまだ落としません）。
   - `now >= first_above_time` に達した場合、`INTERVAL_NS` の間ずっと滞留キューが解消しなかったことを意味します。
   - `dropping = true` に移行し、この滞留パケットを `bpf_kfree_skb(skb)` で **Head-Drop（先頭破棄）** します。
   - 直後にキューから次のパケットを取り出し、送信側に最速でパケットロス（SACK / 重複 ACK）を通知します。

---

## 3. コード構造と主要データ構造

### `struct skb_node` と BPF リンクリスト

```c
struct skb_node {
	__u64 enqueue_time;
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock codel_lock;
private(A) struct bpf_list_head codel_queue __contains(skb_node, node);
```

- 各パケットは BPF の `bpf_list_head` / `bpf_list_node` による侵入型リンクリストで FIFO 管理されます。
- エンキュー時に `skbn->enqueue_time = bpf_ktime_get_ns();` を打刻し、デキュー時に滞在時間を算出します。

### グローバル状態変数

- `next_tx_ns`: ボトルネックリンクの送信完了予定時刻（`BOTTLENECK_NS = 4,000,000ULL`）。パケット送信間隔をレート制御し、擬似的にボトルネックリンクを作ります。
- `first_above_time`: Sojourn Time が `TARGET_NS` を超えたときに設定される満了予定時刻（`now + INTERVAL_NS`）。
- `dropping`: 現在 Head-Drop 状態に入っているかを示すフラグ。
- `drop_count`: Head-Drop されたパケット数。

---

## 3.5 設計チェックポイント（理解した仕組みをコードに落とし込む）

`starter.bpf.c` の TODO を実装する前に、以下の 3 つの設計の問いを自分の頭で考えてみましょう。

### Q1. 滞在時間（Sojourn Time）の測定：なぜキュー長ではなく `now - enqueue_time` を測るのか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: 回線速度が 10 Gbps のときと 1 Mbps のときで、同じ 100 パケットのキューが引き起こす遅延はどう変わるでしょうか？
- **答え**: **キューのパケット数が同じでも、回線のドレイン速度によって実際の待ち時間は数千倍異なるためです。**
  エンキュー時に `skbn->enqueue_time = bpf_ktime_get_ns()` を記録し、デキュー時に `sojourn = now - enqueue_time` を計算することで、回線速度に依存せず純粋なキューイング遅延をナノ秒単位で評価できます。

</details>

### Q2. 観測ウィンドウ（`INTERVAL_NS`）：なぜ `sojourn >= TARGET_NS` になった瞬間に即座にドロップしないのか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: 一過性のバースト（Good Queue）と持続的な滞留キュー（Bad Queue）の違いを思い出してください。
- **答え**: **一時的なバーストトラフィックを誤ってドロップしてしまうのを防ぐためです。**
  初めて `sojourn >= TARGET_NS` になったときは `first_above_time = now + INTERVAL_NS` をセットするだけにとどめ、`INTERVAL_NS` の間ずっと `sojourn >= TARGET_NS` が継続した（`now >= first_above_time` に到達した）ときに初めて持続的な滞留キューと判定して Head-Drop を発動します。

</details>

### Q3. Head-Drop の実装：先頭パケットを `bpf_kfree_skb(skb)` で破棄した後、なぜ直ちに次のパケットをポップして返すのか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: もしパケットを落として `return NULL;` してしまったら、回線（NIC）と受信側 TCP はどうなるでしょうか？
- **答え**: **回線を遊ばせず（Work-conserving）、かつ受信側に後続パケットを即座に届けて SACK / 重複 ACK による高速なロス通知を発生させるためです。**
  先頭パケットを破棄した直後に 2 番目の新鮮なパケットをポップして送信することで、送信側 TCP は 1 RTT 以内に輻輳を検知できます。

</details>

---

## 4. 実装手順（TODO リスト）

[`starter.bpf.c`](starter.bpf.c) 内の `bpf_codel_dequeue()` にある 2 つの TODO を実装します。

### TODO 1: ボトルネック送信レート制御（Watchdog タイマー）

ボトルネックリンクの帯域制限を再現するため、前回の送信完了予定時刻 `next_tx_ns` より前にデキューが呼ばれた場合は送信を延期します。

```c
now = bpf_ktime_get_ns();
if (now < next_tx_ns) {
	bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	return NULL;
}
```

### TODO 2: CoDel AQM 状態マシンの実装

パケットをリンクリストから取り出した後、滞在時間 `sojourn = now - enqueue_time` を計算し、CoDel のドロップ判定を行います。

```c
sojourn = now - enqueue_time;
if (sojourn < TARGET_NS) {
	first_above_time = 0;
	dropping = false;
} else {
	if (first_above_time == 0) {
		first_above_time = now + INTERVAL_NS;
	} else if (now >= first_above_time) {
		dropping = true;
		drop_count++;
		sch->qstats.backlog -= qdisc_pkt_len(skb);
		sch->q.qlen--;
		sch->qstats.drops++;
		bpf_kfree_skb(skb);
		skb = NULL;

		/* 次のパケットを即座に取り出して送信 */
		bpf_spin_lock(&codel_lock);
		node = bpf_list_pop_front(&codel_queue);
		bpf_spin_unlock(&codel_lock);
		if (!node)
			return NULL;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		bpf_obj_drop(skbn);
		if (!skb)
			return NULL;
	}
}
```

---

## 5. テスト検証と Packetdrill

テストスクリプト [`test.pkt`](test.pkt) は以下のシナリオを自動実行します：

1. `tc qdisc replace dev tun0 root bpf_codel` で本 Qdisc を装着。
2. TCP コネクションを確立し、送信側から一気に 10 個のセグメント（各 1000 バイト、合計 10 KB）を送信。
3. ボトルネックレート（1 パケットあたり 4 ms）により、キュー内にパケットが溜まり始めます。
4. 最初の数パケットは滞在時間が短いため正常送信されますが、やがて滞在時間が `TARGET_NS (5 ms)` を超え、`INTERVAL_NS (20 ms)` が経過した時点で CoDel が滞留パケットを Head-Drop します。
5. 直後のパケットが受信側に届き、受信側から SACK（例: `8001:10001`）が返ることで、送信側 TCP は最速でパケットロスを検知してウィンドウを縮小します。

### テスト実行

```sh
# starter が失敗することを確認
./run_test.sh qdisc-fq-codel/step01-codel-aqm starter

# 実装後、テストをパスさせる
./run_test.sh qdisc-fq-codel/step01-codel-aqm solution
```

テストに合格したら、次は複数フローの公平性と対話型通信の超低遅延化を実現する [`step02-fq-codel/`](../step02-fq-codel/README.md) に進みましょう！
