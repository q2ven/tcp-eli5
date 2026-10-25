# Step 2: Earliest Departure Time (EDT) ペーシングとホライゾンチェック

[English](README.en.md)

## 1. 学習ゴール

このステップでは、赤黒木（`bpf_rb_root`）を活用して、現代の Linux カーネルの核心的ペーシング機構である **Earliest Departure Time (EDT) ペーシング** と **ホライゾンチェック（Horizon Drop）** を BPF `struct_ops`（`struct Qdisc_ops`）として実装します。

- トラック全体の概要は [FQ トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

### 2.1 なぜパケットペーシングが必要なのか？

TCP は ACK を受信するたびに新しいデータを送信します。受信側が複数のパケットをまとめて ACK（Delayed ACK や LRO/GRO）すると、送信側は 10〜30 パケットを **NIC の最大回線速度（ワイヤレート）で一気に連射（バースト送信）** してしまいます。

```text
従来のバースト送信:
||||||||||||                ||||||||||||                ||||||||||||
[-- 10連射パケット --]      [-- 10連射パケット --]      [-- 10連射パケット --]
(スイッチのバッファが瞬間的に溢れて自己パケットロス！)

EDT ペーシング送信:
|   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |
(RTT 全体に均等に散らばるため、バッファを消費せず低遅延を維持！)
```

バースト送信が途中のボトルネックスイッチに突入すると、瞬間的にスイッチのバッファが満杯になってパケットロスが発生します。
そこで、パケット同士の間隔を均等に空けて **小川のようにサラサラと流す（Pacing）** 仕組みが不可欠です。

### 2.2 Earliest Departure Time (EDT) モデル

Linux 4.20 で導入された **EDT モデル** では、ソケットが設定した目標レート（`sk->sk_max_pacing_rate`）に基づき、各パケットに「このパケットは未来の時刻 $$t$$ に送信せよ」という **最早出発時刻（`skb->tstamp`）** をスタンプします。

パケット送信予定時刻 `time_to_send` は次のように計算されます：

1. **明示的な `skb->tstamp` がある場合**: TCP レイヤ（TCP Pacing や BBR）がすでに計算したタイムスタンプ `skb->tstamp` をそのまま使用します。
2. **ソケットの `sk_max_pacing_rate` が有効な場合**:
   現在のフローの次回送出時刻 `flow->time_next_packet` と現在時刻 `now` の大きい方を基準とします：
   $$\text{time to send} = \max(\text{flow} \to \text{next packet time}, \text{now})$$
   そして、このパケットを送るのに必要な時間幅（$$\Delta t$$）を足して、次回のパケットのために `time_next_packet` を進めます：
   $$\text{flow} \to \text{next packet time} = \text{time to send} + \frac{\text{packet len} \times 10^9}{\text{rate}}$$
3. **上記以外**: 即座に送出可能（`time_to_send = now`）とします。

### 2.3 遅延フロー管理（`fq_delayed`）と Watchdog タイマー

Dequeue 時にフローの先頭パケットを取り出した際、そのパケットの `time_to_send` が現在時刻 `now` より未来（`now < time_to_send`）だった場合：

1. **フローをスロットル（Throttled）状態にする**: `flow->age = ~0ULL` とマークし、赤黒木 **`fq_delayed`** に挿入します（キーは `flow->time_next_packet`）。
2. **Watchdog タイマーのセット**: 最も早く解禁される時刻（`time_next_delayed_flow`）をカーネルのタイマー機構 `bpf_qdisc_watchdog_schedule()` に登録します。
3. **タイマー発火と復帰**: タイマーが発火して次回 Dequeue が呼ばれると、`fq_check_throttled(now)` が実行され、現在時刻を過ぎたフローが `fq_delayed` から取り出されて `fq_old_flows` に復帰します。

### 2.4 ホライゾンチェック（Horizon Check）

もしアプリケーションの誤動作や設定ミスにより、`setsockopt(..., SO_MAX_PACING_RATE)` に極端に小さい値（例: 100 バイト/秒）が設定されたり、遥か未来のタイムスタンプが付けられたらどうなるでしょうか？

1000 バイトのパケット 1 つで送出間隔が 10 秒以上も伸びてしまい、キューの中に何秒・何分もパケットが滞留し続けることになります。これはメモリの枯渇やタイムアウトを招きます。

そこで FQ は **ホライゾン（`HORIZON_NS = 2` 秒）** を設定し、現在時刻より 2 秒以上先の未来に送信予定となる異常パケットは、キューに入れずに即座にドロップ（`NET_XMIT_DROP`）します。

---

## 3. 具体数値トレース（本ステップの `test.pkt` シナリオ）

設定値：`SO_MAX_PACING_RATE = 200,000` バイト/秒（200 KB/s）、パケット長 $$\approx 1066$$ バイト。
パケットあたりの所要時間：
$$\Delta t = \frac{1066 \times 10^9}{200,000} = 5,330,000 \text{ ns} \approx 5.33 \text{ ms}$$

| イベント | 送出時刻 | 状態遷移とタイマー |
| :--- | :--- | :--- |
| `write(4, ..., 3000)` (3 セグメント) | **+0.000** | Pkt 1 (1:1001) の目標時刻は `now` なので **即座に送出**！<br>`time_next_packet` は `now + 5.33 ms` へ進む。 |
| Pkt 2 の検査 | — | Pkt 2 の目標時刻は `+5.33 ms`（未来）。<br>フローは `fq_delayed` へ退避され、タイマーがセットされる。 |
| タイマー発火 (+5.33 ms) | **~ +0.005** | Pkt 2 (1001:2001) が **Watchdog によって送出**！<br>`time_next_packet` は `now + 10.66 ms` へ進む。 |
| タイマー発火 (+10.66 ms) | **~ +0.010** | Pkt 3 (2001:3001) が **Watchdog によって送出**！ |
| `SO_MAX_PACING_RATE = 100` B/s に設定 | — | 1000B パケットで $$\Delta t = \frac{1066 \times 10^9}{100} > 10$$ 秒！ |
| `write(4, ..., 2000)` (2 セグメント) | **+0.000** | Pkt 1 (3001:4001) が送出され、`time_next_packet` は `now + 10.66s` へ進む。 |
| Pkt 2 の Enqueue | — | Pkt 2 の目標時刻は `now + 10.66s` となり、**`now + 2s` のホライゾンを超過**！<br>`bpf_qdisc_skb_drop()` で **即座にドロップ**！ |

---

## 4. 設計チェックポイント

### Q1. `time_to_send` の計算優先順位はどうなっていますか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `skb->tstamp` がすでに設定されているケース、ソケットのペーシングレートがあるケース、どちらもないケースの 3 通りがあります。
- **答え**:
  1. `skb->tstamp != 0` の場合はその値を優先します。
  2. そうでなく `rate > 0 && rate != ~0ULL` の場合は `max(flow->time_next_packet, now)` を用います。
  3. それ以外は `now` を使用します。

</details>

### Q2. ホライゾンドロップの判定式と後始末はどう記述しますか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: 符号付き 64 ビット整数（`s64`）で比較し、マップから取り出していた `flow` のポインタを忘れずに戻します。
- **答え**:
  ```c
  if ((s64)time_to_send > (s64)(now + HORIZON_NS)) {
      bpf_kptr_xchg_back(&sflow->flow, flow);
      goto drop;
  }
  ```
  `drop` ラベルでは `sch->qstats.drops++` を加算し、`bpf_qdisc_skb_drop(skb, to_free)` を呼び出して `NET_XMIT_DROP` を返します。

</details>

---

## 5. テストと動作確認

```sh
cd /usr/local/google/home/kuniyu/tcp-eli5/labs

# 1. 未実装の starter が失敗することを確認する
./run_test.sh qdisc-fq/step02-edt-pacing starter

# 2. 模範解答 (solution) が PASS することを確認する
./run_test.sh qdisc-fq/step02-edt-pacing solution
```
