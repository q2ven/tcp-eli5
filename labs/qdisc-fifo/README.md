# トラック 4: BPF Qdisc 入門 — FIFO キューイングと優先度制御

[English](README.en.md)

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
