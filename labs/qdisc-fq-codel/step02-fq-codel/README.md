# Step 2: 完全な FQ-CoDel（フロー分離と 2 リスト DRR + フロー単位 CoDel）

[English](README.en.md)

## 1. 学習ゴール

このステップでは、Step 1 で実装した CoDel AQM に **フロー分類（Flow Classification）** と **2 つの循環リスト（`new_flows` / `old_flows`）による Deficit Round Robin (DRR)** を統合し、**FQ-CoDel（RFC 8290）** をゼロから完成させます。

- トラック全体の背景や Bufferbloat のメカニズムについては [トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説

### 2.1 なぜ CoDel だけでは不十分なのか？

Step 1 の単一キュー CoDel では、持続的な滞留キューを検知して Head-Drop を行い、Bufferbloat を解消できることを確認しました。

しかし、単一の FIFO キューである限り、**「ヘビーなダウンロードフロー」と「対話型の軽量フロー（SSH、DNS、Web、ゲーム、VoIP）」が同じキューに相乗り** してしまいます。
- バルクフローが大量のパケットを送り込んでいる最中に届いた SSH のキー入力パケットは、バルクフローの後ろに並ばされます。
- また、CoDel がパケットを落とす際、たまたま運悪く対話型フローのパケットが先頭にあれば、それが巻き添えで落とされてしまう危険性もあります。

### 2.2 FQ-CoDel の 3 本柱

FQ-CoDel は、以下の 3 つの技術を組み合わせることでこの問題を完璧に解決します：

```text
パケット到着
   |
   +---> [1] フロー分類 (IP 5-tuple ハッシュ) でバケットへ振り分け
           |
           +---> [2] 2 リスト DRR: 疎なフロー (new_flows) を最優先デキュー!
           |
           +---> [3] フロー単位 CoDel: 滞留しているバルクフローだけを選択的 Head-Drop!
```

1. **フロー分類（Flow Classification）**:
   - パケットのソケットハッシュ（`sk->__sk_common.skc_hash`）または 5-tuple ハッシュ（`bpf_skb_get_hash(skb)`）を用いて、パケットを独立したフローキューに分類します。
2. **2 つのリストによる DRR スケジューリング（`new_flows` と `old_flows`）**:
   - **`new_flows`**: 直前までアイドル（休止）状態だったフローや、新しく到着したフローが入るリスト。
   - **`old_flows`**: クレジット（`quantum`）を使い切ったフローが入るリスト。
   - デキュー時は **常に `new_flows` を優先して処理** します。対話型通信のような「たまにしかパケットを送らない疎なフロー（Sparse Flow）」は、常に `new_flows` で即座に送信されるため、**待ち時間ゼロ・遅延ゼロ・パケットロスゼロ** でバルクフローを追い越します！
3. **フロー単位の CoDel AQM**:
   - `first_above_time` や `dropping` などの CoDel 状態変数を **フロー構造体（`struct fq_codel_flow`）ごとに独立して保持** します。
   - バッファを埋め尽くしているバルクフローだけが `TARGET_NS` を超過して Head-Drop され、他のフローには一切悪影響を及ぼしません。

---

## 3. コード構造と主要データ構造

### `struct bpf_fq_codel_flow`（フロー構造体）

```c
struct bpf_fq_codel_flow {
	s64 credit;                 /* DRR 送信可能残高 (バイト数, s64) */
	u32 qlen;                   /* フロー内のパケット数 */
	u64 age;                    /* 0: リスト接続中, 1: 休止中 (detached) */
	u64 first_above_time;       /* CoDel: TARGET 超過観測タイマー */
	u32 drop_count;             /* CoDel: 本フローのドロップ数 */
	bool dropping;              /* CoDel: ドロップモード中か */
	struct bpf_list_node list_node; /* new_flows / old_flows 用ノード */
	struct bpf_list_head queue __contains(skb_node, node); /* フローのパケットキュー */
	struct bpf_spin_lock lock;  /* パケットキューのロック */
	struct bpf_refcount refcount; /* BPF オブジェクト参照カウント */
};
```

### 2 リスト DRR とグローバル状態

- `new_flows`: 疎なフローや新規フローを管理するリスト。最優先でデキューされます。
- `old_flows`: クレジットを使い切ったバルクフローがラウンドロビンするリスト。
- `next_tx_ns`: ボトルネックリンクの送信レート制御用タイマー（`BOTTLENECK_NS = 4,000,000ULL`）。

---

## 3.5 設計チェックポイント（理解した仕組みをコードに落とし込む）

`starter.bpf.c` の TODO を実装する前に、以下の 3 つの設計の問いを考えてみましょう。

### Q1. 疎なフローの優遇：なぜ `new_flows` と `old_flows` の 2 つのリストが必要なのか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: もし全フローを 1 つのラウンドロビンリストで管理すると、新しく届いた 1 パケットだけの通信はどう扱われるでしょうか？
- **答え**: **たまにしかパケットを送らない対話型通信（Sparse Flow）を、大量転送中のバルクフロー（`old_flows`）より常に先にデキューさせるためです。**
  休止（detached）状態から到着したフローは `new_flows` に入り、デキュー処理は `new_flows` が空になるまで `old_flows` を処理しません。これにより対話型パケットは待ち時間ゼロでバルクフローを追い越せます。

</details>

### Q2. クレジットと降格：フローが `new_flows` から `old_flows` へ移動するのはどのタイミングか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: パケットを送信するたびに `flow->credit -= pkt_len` が行われます。
- **答え**: **パケット送信によって `flow->credit <= 0` となり、初期クレジットを使い切ったタイミングです。**
  1 パケットしか送らない疎なフローは `credit` が残っている間にキューが空になり休止状態へ戻りますが、連続してパケットを送り続けるバルクフローは `credit <= 0` となって `flow->credit += QUANTUM` を加算されたうえで `old_flows` の末尾へ降格されます。

</details>

### Q3. フロー単位の CoDel 隔離：なぜ `first_above_time` と `dropping` をグローバル変数ではなく `struct bpf_fq_codel_flow` のメンバとして持つのか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: もし CoDel の状態変数を Qdisc 全体で 1 つだけ共有したら、バルクフローのせいで何が起きるでしょうか？
- **答え**: **滞留キューを作っている悪質なバルクフローだけを選択的に Head-Drop し、他の健全なフローを巻き添えにしないためです。**
  各フロー構造体が独自の `first_above_time` と `dropping` を持つことで、疎なフロー（Flow B）は滞在時間が短く保たれ、ドロップゼロで通信を継続できます。

</details>

---

## 4. 実装手順（TODO リスト）

[`starter.bpf.c`](starter.bpf.c) 内の `bpf_fq_codel_dequeue()` にある 3 つの TODO を実装します。

### TODO 1: 2 リスト DRR によるフロー選択

`new_flows` が空でなければ `new_flows` から先頭フローを取り出し、空であれば `old_flows` から取り出します。

```c
bpf_spin_lock(&new_flows_lock);
node = bpf_list_pop_front(&new_flows);
if (node) {
	new_flow_cnt--;
	is_new = true;
}
bpf_spin_unlock(&new_flows_lock);

if (!node) {
	bpf_spin_lock(&old_flows_lock);
	node = bpf_list_pop_front(&old_flows);
	if (node) {
		old_flow_cnt--;
		is_new = false;
	}
	bpf_spin_unlock(&old_flows_lock);
}

if (!node)
	return NULL;
```

### TODO 2: クレジット検査と DRR ラウンド制御

取り出したフローの `credit <= 0` の場合、そのラウンドでの送信権を使い切っています。
`credit += QUANTUM` を加算し、フローを `old_flows` の末尾へ移動させて `return NULL;` します（次回の dequeue で再試行されます）。

```c
if (flow->credit <= 0) {
	flow->credit += QUANTUM;
	bpf_spin_lock(&old_flows_lock);
	bpf_list_push_back(&old_flows, &flow->list_node);
	old_flow_cnt++;
	bpf_spin_unlock(&old_flows_lock);
	bpf_fq_codel_flow_drop(flow);
	return NULL;
}
```

### TODO 3: フロー単位の CoDel AQM 状態マシン

フロー内のパケットキューからパケットを取り出し、フローごとの CoDel 判定を行います。

```c
sojourn = now - enqueue_time;
if (sojourn < TARGET_NS) {
	flow->first_above_time = 0;
	flow->dropping = false;
} else {
	if (flow->first_above_time == 0) {
		flow->first_above_time = now + INTERVAL_NS;
	} else if (now >= flow->first_above_time) {
		flow->dropping = true;
		flow->drop_count++;
		sch->qstats.backlog -= qdisc_pkt_len(skb);
		sch->q.qlen--;
		sch->qstats.drops++;
		bpf_kfree_skb(skb);
		skb = NULL;

		/* フロー内キューから直後のパケットを即座に取り出す */
		bpf_spin_lock(&flow->lock);
		node = bpf_list_pop_front(&flow->queue);
		bpf_spin_unlock(&flow->lock);
		if (!node)
			goto finish_flow;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		flow->qlen--;
		bpf_obj_drop(skbn);
		if (!skb)
			goto finish_flow;
	}
}
```

---

## 5. テスト検証と Packetdrill
 
テストスクリプト [`test.pkt`](test.pkt) は、FQ-CoDel の真価を証明する以下の実戦的シナリオを検証します：
 
1. **Flow A（バルク転送）**:
   - ソケット 3 から 1,000 バイトの UDP パケットを 8 個一気に送信（$$t = 0$$）。
   - 最初のパケット（P1）が即座に送出された後、クレジットを使い切って（$$1000 - 1028 \le 0$$）`old_flows` へ降格され、残り 7 パケットがキューに溜まります。
2. **Flow B（対話型疎フロー / Sparse Flow）**:
   - Flow A のパケットがキューに並んでいる最中（$$t = 2\text{ ms}$$）に、ソケット 4 から 1 パケット（500 バイト）だけ送信。
   - Flow B はアイドル状態から到着したため、`new_flows` に登録されます。
3. **疎フローの優先デキュー**:
   - $$t = 4\text{ ms}$$ のボトルネックスロットにおいて、FQ-CoDel は `old_flows` に並んでいる Flow A のパケットを差し置いて、**`new_flows` にある Flow B のパケットを最優先で送信** します！
4. **バルクフローの隔離と Head-Drop**:
   - $$t = 8\text{ ms} \sim 28\text{ ms}$$ にかけて Flow A のパケット（P2〜P7）が順次送出されます。
   - $$t = 32\text{ ms}$$ において、滞留キューにより Sojourn Time が `TARGET_NS (5 ms)` を超え続け、`INTERVAL_NS (20 ms)` 経過後に **Flow A のパケット（P8）だけが Head-Drop** されます。
   - 疎フローである Flow B にはパケットロスも遅延も一切発生しません！

### テスト実行

```sh
# starter が失敗することを確認
./run_test.sh qdisc-fq-codel/step02-fq-codel starter

# 実装後、テストをパスさせる
./run_test.sh qdisc-fq-codel/step02-fq-codel solution
```

全テストにパスしたら、Track 6 完了です！お疲れ様でした！
