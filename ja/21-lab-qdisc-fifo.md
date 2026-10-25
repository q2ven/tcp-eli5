# 21. ハンズオン (4): BPF Qdisc と packetdrill で作る FIFO / 優先度スケジューラ


このトラックでは、Linux カーネルのネットワークパケット送出調停メカニズムである **Qdisc（Queueing Discipline: キューイング規律）** を、BPF `struct_ops`（`struct Qdisc_ops`）を使ってゼロから実装します。

これまで学んだ TCP 輻輳制御（Reno / CUBIC / BBR）が「送信側ソケットがネットワークへ送り出してよい未確認データ量（`cwnd`）やペーシングレート」を決めるアルゴリズムであったのに対し、Qdisc は **「複数のソケットから吐き出されたパケットを、物理 NIC（送信リング）へ渡す直前にどのような順序・速度で調停し、バッファが溢れたときにどのパケットを落とすか」** を司るトラフィック制御（Traffic Control: TC）の中核です。

---

## 1. そもそも Qdisc（キューイング規律）とは何か？

Linux の送信パケット処理フローにおいて、Qdisc は **レイヤ 4（TCP/UDP トランスポート層）と レイヤ 2/1（NIC デバイスドライバ・送信リング）の境界** に位置します。

```text
+--------------------------------------------------------------+
| アプリケーション層 (write / sendmsg)                         |
+--------------------------------------------------------------+
                              |
                              v
+--------------------------------------------------------------+
| トランスポート層 (TCP / UDP)                                 |
| - ソケット送信バッファ (sk_write_queue)                      |
| - 輻輳制御 (cwnd / pacing rate の決定)                       |
+--------------------------------------------------------------+
                              |
                              v dev_queue_xmit()
+--------------------------------------------------------------+
| トラフィック制御層 (Qdisc: struct Qdisc / struct Qdisc_ops)   |
| - パケットのキューイングとバッファ管理 (.enqueue)            |
| - パケットの並べ替え・帯域制限・送出調停 (.dequeue)          |
+--------------------------------------------------------------+
                              |
                              v sch_direct_xmit()
+--------------------------------------------------------------+
| NIC ドライバ & 物理ハードウェア (TX Ring Buffer / DMA)       |
+--------------------------------------------------------------+
```

### なぜ Qdisc が必要なのか？

- **速度差の吸収**: アプリケーションや TCP スタックがパケットを生成する速度は、物理回線のリンク速度や NIC 送信リングの空き状況と必ずしも一致しません。Qdisc は一時的なトラフィックのスパイクをキューに蓄積し、回線容量を超えないペースで順次 NIC に引き渡します。
- **スケジューリングと優先度制御（QoS）**: DNS クエリや TCP SYN、SSH といった遅延に敏感なパケットを、バルクファイル転送などの大量パケットより優先して送出できます。
- **バッファブロート対策と輻輳シグナル**: キューが満杯になったときにどのパケットを破棄（テールダンプ / ヘッドドロップ）するか、あるいは ECN マークを打つかを決定します。

---

## 2. Linux カーネルの Qdisc アーキテクチャと BPF `struct_ops`

Linux カーネル 6.x 以降では、従来 C 言語でカーネルモジュールとして書かれていた Qdisc を、安全な BPF プログラム（`struct_ops`）としてロード・アタッチできるようになりました。

`struct Qdisc_ops` が要求する主要な 5 つのフック関数を理解しましょう。

| フック名 | シグネチャ | 役割と呼び出しタイミング |
| :--- | :--- | :--- |
| `.enqueue` | `int (*enqueue)(struct sk_buff *skb, struct Qdisc *sch, struct bpf_sk_buff_ptr *to_free)` | 上流（`dev_queue_xmit()`）からパケットが送られてきたときに呼ばれます。キューに格納できた場合は `NET_XMIT_SUCCESS` を返し、バッファ上限等で破棄する場合は `bpf_qdisc_skb_drop(skb, to_free)` を呼んで `NET_XMIT_DROP` を返します。 |
| `.dequeue` | `struct sk_buff *(*dequeue)(struct Qdisc *sch)` | NIC の送信リングが空いたとき、または watchdog タイマーが発火したときにカーネルから呼ばれます。キューから次に送出すべき `skb` を 1 つ取り出して返します。送出可能なパケットがない（またはレート制限中）の場合は `NULL` を返します。 |
| `.init` | `int (*init)(struct Qdisc *sch, struct nlattr *opt, struct netlink_ext_ack *extack)` | `tc qdisc add/replace` で Qdisc がネットワークデバイスにアタッチされた直後に呼ばれます。キューの長さ上限（`sch->limit`）や状態変数を初期化します。 |
| `.reset` | `void (*reset)(struct Qdisc *sch)` | デバイスがダウンした際や Qdisc がリセットされる際に呼ばれます。現在キュー内に滞留しているすべてのパケットを取り出して `bpf_kfree_skb(skb)` で解放し、カウンタをリセットします。 |
| `.destroy` | `void (*destroy)(struct Qdisc *sch)` | Qdisc が完全に削除・アンロードされる際に呼ばれます。動的に確保したリソースを解放します。 |

---

## 3. BPF で Qdisc を安全に実装するためのプリミティブ

BPF 内でパケット（`struct sk_buff`）を安全にキューイングするために、カーネルは最新の BPF 型システムとヘルパー（kfunc）を提供しています。

### 3.1 BPF `kptr`（`__kptr`）と所有権の移動

パケット `skb` はカーネルメモリ上に存在し、BPF プログラムが自由にキューへ保存できるようにするため、`__kptr` アノテーションと `bpf_kptr_xchg()` による **所有権（Ownership）の移動** を行います。

```c
struct skb_node {
    struct sk_buff __kptr *skb;
    struct bpf_list_node node;
};
```

- `skb = bpf_kptr_xchg(&skbn->skb, skb);`: 入力された `skb` の所有権をノード `skbn` に移します。もし以前のポインタが存在した場合は戻り値として返されるため、`if (skb) bpf_qdisc_skb_drop(skb, to_free);` で解放します。
- `skb = bpf_kptr_xchg(&skbn->skb, NULL);`: デキュー時にノードから `skb` の所有権を取り出します。

### 3.2 BPF リスト（`bpf_list_head` / `bpf_list_node`）とスピンロック

パケットを保持するキューは、侵入型リンクリスト（Intrusive Linked List）である `struct bpf_list_head` を使います。

```c
private(A) struct bpf_spin_lock q_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
```

- **スピンロックの厳格な制約**: BPF verifier はロック保持区間を極めて厳密に検査します。**`bpf_spin_lock` を保持した状態で `bpf_kptr_xchg()` や `bpf_obj_drop()`、メモリ割り当てを行うことは固く禁止されています**。ロック内で行う操作は `bpf_list_push_back()` や `bpf_list_pop_front()` のリスト操作のみにとどめ、ロックを解除した直後に `bpf_kptr_xchg()` を呼び出す必要があります。

### 3.3 高精度レート制限（Pacing）と Watchdog タイマー

回線帯域をシミュレートしたりパケットの送出間隔を制御するために、Qdisc はハードウェア高精度タイマー（hrtimer）と連動する **Watchdog タイマー** を備えています。

```c
__u64 now = bpf_ktime_get_ns();
if (now < next_tx_ns) {
    bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
    return NULL;
}
```

- 送出予定時刻 `next_tx_ns` より現在時刻 `now` が前である場合、`bpf_qdisc_watchdog_schedule()` でカーネルにタイマー発火を依頼し、`.dequeue` は `NULL` を返します。
- 指定時刻が訪れると、カーネルが自動的に Qdisc の `.dequeue` を再実行してパケットを送り出します。

---

## 4. トラックの構成と学習のロードマップ

このトラックは以下の 2 ステップで構成されています。

| ステップ | ディレクトリ | 実装する内容 |
| :--- | :--- | :--- |
| **Step 1** | [`step01-pfifo/`](step01-pfifo/README.md) | **Packet-limited FIFO（pfifo）**: パケット数上限（`sch->limit = 4`）とボトルネックレート制御（2 ms ペーシング）を持つ単一キュー FIFO を実装し、BPF kptr、リンクリスト、スピンロック、Watchdog タイマーの基礎を完全制覇します。 |
| **Step 2** | [`step02-bfifo-prio/`](step02-bfifo-prio/README.md) | **2-Band Priority + Byte-limited FIFO**: バイト数上限（`BYTE_LIMIT = 3500`）と優先度判定（`SO_PRIORITY` / `TC_PRIO_CONTROL`）を組み合わせた 2 バンド優先度付きキューを実装し、優先パケットによる通常トラフィックの追い越し（Head-of-Line Blocking 回避）を検証します。 |

### 実行コマンド

`labs/` ディレクトリ（`/usr/local/google/home/kuniyu/tcp-eli5/labs`）から `run_test.sh` を使ってビルド・テストを実行できます。

```sh
# Step 1: starter が失敗することを確認し、solution を検証する
./run_test.sh qdisc-fifo/step01-pfifo starter
./run_test.sh qdisc-fifo/step01-pfifo solution

# Step 2: 優先度制御キューの検証
./run_test.sh qdisc-fifo/step02-bfifo-prio starter
./run_test.sh qdisc-fifo/step02-bfifo-prio solution
```

それでは、[`step01-pfifo/README.md`](step01-pfifo/README.md) に進み、最初の BPF Qdisc を組み立てていきましょう！


---

## Step 1: パケット数制限 FIFO Qdisc と Watchdog レート制御


## 1. 学習ゴール

このステップでは、Linux カーネルのトラフィック制御インターフェース `struct Qdisc_ops` を BPF `struct_ops` で実装し、最も基本的かつ重要なキューイング規律である **Packet-limited FIFO（pfifo）** をゼロから組み立てます。

キューのパケット数上限（`sch->limit = 4`）によるテールダンプ（Tail-drop）と、高精度タイマー（Watchdog）を用いた 2 ms ごとのボトルネックレート制御（ペーシング）を純粋な BPF コードとして実装することで、パケットのエンキュー・デキューのライフサイクルと、BPF 特有のデータ構造・ロック規約を完全に理解します。

- トラック全体の構成と背景知識は [Qdisc FIFO トラック README](#) を参照してください。

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


### `labs/qdisc-fifo/step01-pfifo/starter.bpf.c` (穴埋めスケルトン)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1 (Starter): Packet-limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Fill in the TODOs below to implement a 4-packet limit FIFO queue in BPF.
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BOTTLENECK_NS 2000000ULL /* 2 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_fifo_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
static __u64 next_tx_ns;

SEC("struct_ops/bpf_pfifo_enqueue")
int BPF_PROG(bpf_pfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	/* TODO 1: Check queue limit:
	 *   If sch->q.qlen >= sch->limit, increment sch->qstats.drops++,
	 *   drop the packet with bpf_qdisc_skb_drop(skb, to_free),
	 *   and return NET_XMIT_DROP.
	 */

	/* TODO 2: Allocate skb_node object:
	 *   skbn = bpf_obj_new(typeof(*skbn));
	 *   If allocation fails (!skbn), increment sch->qstats.drops++,
	 *   drop the packet with bpf_qdisc_skb_drop(skb, to_free),
	 *   and return NET_XMIT_DROP.
	 */

	/* TODO 3: Read packet length BEFORE kptr exchange:
	 *   u32 pkt_len = qdisc_pkt_len(skb);
	 *
	 *   Store skb into skbn via bpf_kptr_xchg(&skbn->skb, skb).
	 *   If bpf_kptr_xchg returns a non-NULL skb, drop it with
	 *   bpf_qdisc_skb_drop(skb, to_free).
	 */

	/* TODO 4: Enqueue skbn to q_fifo:
	 *   Acquire bpf_spin_lock(&q_fifo_lock), push to back with
	 *   bpf_list_push_back(&q_fifo, &skbn->node), and release lock.
	 *   Increment sch->q.qlen++ and add pkt_len to sch->qstats.backlog.
	 *   Return NET_XMIT_SUCCESS.
	 */

	/* Placeholder: drop all incoming packets */
	sch->qstats.drops++;
	bpf_qdisc_skb_drop(skb, to_free);
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_pfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_pfifo_dequeue, struct Qdisc *sch)
{
	/* TODO 5: Check empty queue:
	 *   If !sch->q.qlen, return NULL.
	 */

	/* TODO 6: Watchdog rate limiter:
	 *   __u64 now = bpf_ktime_get_ns();
	 *   If now < next_tx_ns, schedule watchdog timer with:
	 *     bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	 *   and return NULL.
	 */

	/* TODO 7: Dequeue head node under bpf_spin_lock(&q_fifo_lock):
	 *   Pop from front with bpf_list_pop_front(&q_fifo).
	 *   Remember: NEVER call bpf_kptr_xchg or bpf_obj_drop inside
	 *   bpf_spin_lock! Unlock immediately after popping.
	 */

	/* TODO 8: Extract skb and free skbn:
	 *   struct skb_node *skbn = container_of(node, struct skb_node, node);
	 *   struct sk_buff *skb = bpf_kptr_xchg(&skbn->skb, NULL);
	 *   bpf_obj_drop(skbn);
	 *   If !skb, return NULL.
	 */

	/* TODO 9: Update qdisc stats and schedule next transmission:
	 *   next_tx_ns = now + BOTTLENECK_NS;
	 *   sch->qstats.backlog -= qdisc_pkt_len(skb);
	 *   bpf_qdisc_bstats_update(sch, skb);
	 *   sch->q.qlen--;
	 *   Return skb.
	 */

	return NULL;
}

SEC("struct_ops/bpf_pfifo_init")
int BPF_PROG(bpf_pfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 4;
	next_tx_ns = 0;
	return 0;
}

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

SEC("struct_ops/bpf_pfifo_destroy")
void BPF_PROG(bpf_pfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_pfifo = {
	.enqueue   = (void *)bpf_pfifo_enqueue,
	.dequeue   = (void *)bpf_pfifo_dequeue,
	.init      = (void *)bpf_pfifo_init,
	.reset     = (void *)bpf_pfifo_reset,
	.destroy   = (void *)bpf_pfifo_destroy,
	.id        = "bpf_pfifo",
};
```

### `labs/qdisc-fifo/step01-pfifo/test.pkt` (packetdrill テスト)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 1: Test packet-limited FIFO (sch->limit = 4) and 2 ms watchdog rate pacing

0.00 `tc qdisc replace dev tun0 root bpf_pfifo`
0.00 `ethtool -K tun0 tso off gso off`

// Establish a passive TCP connection.
// MSS = 1012 with 12-byte TCP Timestamps option -> tp->mss_cache == 1000 bytes.
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// write(4, ..., 6000) bursts 6 segments while cwnd = 10:
// - Packet 1 (1:1001) dequeues immediately at +0.00, scheduling next_tx_ns = now + 2ms.
// - Packets 2..5 (1001:2001, 2001:3001, 3001:4001, 4001:5001) enter the 4-packet FIFO.
// - Packet 6 (5001:6001) exceeds sch->limit = 4 and is tail-dropped!
// - Watchdog timer fires every 2 ms (+0.002) to dequeue Packets 2..5.
+0.00 write(4, ..., 6000) = 6000
+0.00 > . 1:1001(1000) ack 1
+0.002 > . 1001:2001(1000) ack 1
+0.002 > . 2001:3001(1000) ack 1
+0.002 > . 3001:4001(1000) ack 1
+0.002 > . 4001:5001(1000) ack 1

// Verify that bpf_pfifo recorded at least 1 tail drop
+0.01 `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"`
```

<details>
<summary>模範解答: <code>labs/qdisc-fifo/step01-pfifo/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: Packet-limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Implements a 4-packet limit FIFO queue using BPF struct_ops (struct Qdisc_ops),
 * bpf_list, bpf_spin_lock, bpf_kptr, and the qdisc watchdog timer.
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BOTTLENECK_NS 2000000ULL /* 2 ms per packet */

struct skb_node {
	struct sk_buff __kptr *skb;
	struct bpf_list_node node;
};

private(A) struct bpf_spin_lock q_fifo_lock;
private(A) struct bpf_list_head q_fifo __contains(skb_node, node);
static __u64 next_tx_ns;

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

SEC("struct_ops/bpf_pfifo_init")
int BPF_PROG(bpf_pfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 4;
	next_tx_ns = 0;
	return 0;
}

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

SEC("struct_ops/bpf_pfifo_destroy")
void BPF_PROG(bpf_pfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_pfifo = {
	.enqueue   = (void *)bpf_pfifo_enqueue,
	.dequeue   = (void *)bpf_pfifo_dequeue,
	.init      = (void *)bpf_pfifo_init,
	.reset     = (void *)bpf_pfifo_reset,
	.destroy   = (void *)bpf_pfifo_destroy,
	.id        = "bpf_pfifo",
};
```

</details>



---

## Step 2: 2 バンド優先度付きバイト数制限 FIFO Qdisc


## 1. 学習ゴール

このステップでは、Step 1 の基本 FIFO を発展させ、バイト数制限（Byte-limited FIFO: bfifo）と 2 バンドの優先度付きスケジューリング（Strict Priority Scheduling）を組み合わせた **2-Band Priority + Byte-limited FIFO Qdisc（`bpf_bfifo`）** を実装します。

通常トラフィックに対してバイト数上限（`BYTE_LIMIT = 3500` バイト）を課してバッファ溢れを防ぎつつ、制御トラフィックや優先パケット（`SO_PRIORITY = 7` / `TC_PRIO_CONTROL`）に対してはバイト制限をバイパスさせ、キュー内で待機中の通常パケットを追い越して最優先でデキュー（先頭送出）される仕組みをゼロから構築します。

- トラック全体の構成と背景知識は [Qdisc FIFO トラック README](#) を参照してください。

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
   - **Seg 2** (`1001:2001`): `q_normal` に格納。`backlog = 1066`（ $\le 3500$ ）。
   - **Seg 3** (`2001:3001`): `q_normal` に格納。`backlog = 2132`（ $\le 3500$ ）。
   - **Seg 4** (`3001:4001`): `q_normal` に格納。`backlog = 3198`（ $\le 3500$ ）。
   - **Seg 5** (`4001:5001`): $3198 + 1066 = 4264 > 3500$ 。**`BYTE_LIMIT` 超過によりテールダンプ！**
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


### `labs/qdisc-fifo/step02-bfifo-prio/starter.bpf.c` (穴埋めスケルトン)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 2 (Starter): 2-Band Priority + Byte-Limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Fill in the TODOs below to implement a 2-band priority queue in BPF.
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

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

SEC("struct_ops/bpf_bfifo_enqueue")
int BPF_PROG(bpf_bfifo_enqueue, struct sk_buff *skb, struct Qdisc *sch,
	     struct bpf_sk_buff_ptr *to_free)
{
	/* TODO 1: Check total packet limit:
	 *   If sch->q.qlen >= sch->limit, goto drop.
	 */

	/* TODO 2: Identify priority and measure packet length:
	 *   bool is_prio = (skb->priority & TC_PRIO_MAX) == TC_PRIO_CONTROL;
	 *   u32 pkt_len = qdisc_pkt_len(skb);
	 */

	/* TODO 3: Enforce BYTE_LIMIT for normal traffic:
	 *   If !is_prio and (sch->qstats.backlog + pkt_len > BYTE_LIMIT),
	 *   goto drop.
	 */

	/* TODO 4: Allocate skb_node and store skb:
	 *   skbn = bpf_obj_new(typeof(*skbn));
	 *   if (!skbn) goto drop;
	 *   skb = bpf_kptr_xchg(&skbn->skb, skb);
	 *   if (skb) bpf_qdisc_skb_drop(skb, to_free);
	 */

	/* TODO 5: Enqueue to q_prio or q_normal:
	 *   bpf_spin_lock(&q_lock);
	 *   if (is_prio)
	 *       bpf_list_push_back(&q_prio, &skbn->node);
	 *   else
	 *       bpf_list_push_back(&q_normal, &skbn->node);
	 *   bpf_spin_unlock(&q_lock);
	 *   sch->q.qlen++;
	 *   sch->qstats.backlog += pkt_len;
	 *   return NET_XMIT_SUCCESS;
	 */

	/* Placeholder: drop all incoming packets */
	sch->qstats.drops++;
	bpf_qdisc_skb_drop(skb, to_free);
	return NET_XMIT_DROP;
}

SEC("struct_ops/bpf_bfifo_dequeue")
struct sk_buff *BPF_PROG(bpf_bfifo_dequeue, struct Qdisc *sch)
{
	/* TODO 6: Check empty queue and watchdog timer:
	 *   if (!sch->q.qlen) return NULL;
	 *   __u64 now = bpf_ktime_get_ns();
	 *   if (now < next_tx_ns) {
	 *       bpf_qdisc_watchdog_schedule(sch, next_tx_ns, 0);
	 *       return NULL;
	 *   }
	 */

	/* TODO 7: Dequeue highest priority packet available:
	 *   bpf_spin_lock(&q_lock);
	 *   node = bpf_list_pop_front(&q_prio);
	 *   if (!node)
	 *       node = bpf_list_pop_front(&q_normal);
	 *   bpf_spin_unlock(&q_lock);
	 *   if (!node) return NULL;
	 */

	/* TODO 8: Extract skb and free node:
	 *   skbn = container_of(node, struct skb_node, node);
	 *   skb = bpf_kptr_xchg(&skbn->skb, NULL);
	 *   bpf_obj_drop(skbn);
	 *   if (!skb) return NULL;
	 */

	/* TODO 9: Update statistics and schedule next transmit:
	 *   next_tx_ns = now + BOTTLENECK_NS;
	 *   sch->qstats.backlog -= qdisc_pkt_len(skb);
	 *   bpf_qdisc_bstats_update(sch, skb);
	 *   sch->q.qlen--;
	 *   return skb;
	 */

	return NULL;
}

SEC("struct_ops/bpf_bfifo_init")
int BPF_PROG(bpf_bfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 8;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_bfifo_reset")
void BPF_PROG(bpf_bfifo_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&q_lock);
		node = bpf_list_pop_front(&q_prio);
		if (!node)
			node = bpf_list_pop_front(&q_normal);
		bpf_spin_unlock(&q_lock);

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

SEC("struct_ops/bpf_bfifo_destroy")
void BPF_PROG(bpf_bfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_bfifo = {
	.enqueue   = (void *)bpf_bfifo_enqueue,
	.dequeue   = (void *)bpf_bfifo_dequeue,
	.init      = (void *)bpf_bfifo_init,
	.reset     = (void *)bpf_bfifo_reset,
	.destroy   = (void *)bpf_bfifo_destroy,
	.id        = "bpf_bfifo",
};
```

### `labs/qdisc-fifo/step02-bfifo-prio/test.pkt` (packetdrill テスト)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 2: Test 2-band priority + byte-limited FIFO

0.00 `tc qdisc replace dev tun0 root bpf_bfifo`
0.00 `ethtool -K tun0 tso off gso off`

// Setup Flow A listener (fd 3)
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEPORT, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// Handshake Flow A
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Setup Flow B listener (fd 5)
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 5
+0.00 setsockopt(5, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 setsockopt(5, SOL_SOCKET, SO_REUSEPORT, [1], 4) = 0
+0.00 bind(5, ..., ...) = 0
+0.00 listen(5, 1) = 0

// Handshake Flow B
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(5, ..., ...) = 6
+0.00 setsockopt(6, SOL_SOCKET, SO_PRIORITY, [7], 4) = 0

// Flow A bursts 5000 bytes (5 segments):
// - Segment 1 (1:1001) dequeues immediately at t = 0.
// - Segments 2, 3, 4 (1001:2001, 2001:3001, 3001:4001) queue into q_normal.
// - Segment 5 (4001:5001) exceeds BYTE_LIMIT = 3500 and is dropped.
+0.00 write(4, ..., 5000) = 5000

// Flow B sends 500 bytes (1 segment) with SO_PRIORITY = 7:
// Enqueued into q_prio, bypassing BYTE_LIMIT.
+0.00 write(6, ..., 500) = 500

// Segment 1 of Flow A departs immediately at +0.00:
+0.00 > . 1:1001(1000) ack 1

// At +0.004 (4 ms later), Flow B's high-priority packet is dequeued BEFORE Flow A's queued segments!
+0.004 > P. 1:501(500) ack 1

// Subsequent dequeue cycles serve Flow A's remaining segments from q_normal:
+0.004 > . 1001:2001(1000) ack 1
+0.004 > . 2001:3001(1000) ack 1
+0.004 > . 3001:4001(1000) ack 1

// Verify that Segment 5 was tail-dropped by bpf_bfifo
+0.01 `tc -s qdisc show dev tun0 | grep -q "dropped [1-9]"`
```

<details>
<summary>模範解答: <code>labs/qdisc-fifo/step02-bfifo-prio/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 2: 2-Band Priority + Byte-Limited FIFO Qdisc with Watchdog Rate Pacing
 *
 * Implements a 2-band priority queue:
 * - High priority (TC_PRIO_CONTROL): enqueued to q_prio, bypasses BYTE_LIMIT
 * - Normal priority: enqueued to q_normal, subject to BYTE_LIMIT (3500 bytes)
 * Dequeue always serves q_prio before q_normal, paced at BOTTLENECK_NS (4 ms).
 */
#include "vmlinux.h"
#include "bpf_qdisc_helpers.h"

char _license[] SEC("license") = "GPL";

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

SEC("struct_ops/bpf_bfifo_init")
int BPF_PROG(bpf_bfifo_init, struct Qdisc *sch, struct nlattr *opt,
	     struct netlink_ext_ack *extack)
{
	sch->limit = 8;
	next_tx_ns = 0;
	return 0;
}

SEC("struct_ops/bpf_bfifo_reset")
void BPF_PROG(bpf_bfifo_reset, struct Qdisc *sch)
{
	struct bpf_list_node *node;
	struct skb_node *skbn;
	int i;

	bpf_for(i, 0, sch->q.qlen) {
		struct sk_buff *skb = NULL;

		bpf_spin_lock(&q_lock);
		node = bpf_list_pop_front(&q_prio);
		if (!node)
			node = bpf_list_pop_front(&q_normal);
		bpf_spin_unlock(&q_lock);

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

SEC("struct_ops/bpf_bfifo_destroy")
void BPF_PROG(bpf_bfifo_destroy, struct Qdisc *sch)
{
}

SEC(".struct_ops")
struct Qdisc_ops bpf_bfifo = {
	.enqueue   = (void *)bpf_bfifo_enqueue,
	.dequeue   = (void *)bpf_bfifo_dequeue,
	.init      = (void *)bpf_bfifo_init,
	.reset     = (void *)bpf_bfifo_reset,
	.destroy   = (void *)bpf_bfifo_destroy,
	.id        = "bpf_bfifo",
};
```

</details>
