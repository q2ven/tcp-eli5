# `packets_out` / `sacked_out` / `lost_out` / `retrans_out` は増え続けるフィールドか？

（関連章: [第 1 章: 主要データ構造](../01-data-structures.md)、[第 7 章: ACK 処理と RTT 推定](../07-ack-processing.md)、[第 8 章: ロス検出と回復](../08-loss-recovery.md)）

**いいえ、一生増え続ける累積カウンタではありません。**
この 4 つはすべて **「いま現在 `snd_una` 〜 `snd_nxt` の間（再送キュー / rtx queue）に残っている未確認パケットの内訳」を表す現在値（ゲージ）** です。相手から累積 ACK が返ってきて `snd_una` が進み、ソケットがすべて ACK されれば **全員 `0` に戻ります**。

```text
|<------------------------ packets_out (例: 10 パケット) ------------------------>|
+------------------+------------------+------------------+------------------------+
|   lost_out (2)   |  sacked_out (3)  |       ...        |                   ...  |
| 「抜けたと判定」 | 「SACK で到着済」| (まだ飛行中: 5)  |                        |
+------------------+------------------+------------------+------------------------+
         |
         +---> うち再送済みの個数が retrans_out (例: 1)
```

$$\text{In-Flight パケット数} = \text{packets}_{\text{out}} - (\text{sacked}_{\text{out}} + \text{lost}_{\text{out}}) + \text{retrans}_{\text{out}}$$

| フィールド | 意味 | いつ増えるか（`+`） | いつ減るか（`-`） |
| :--- | :--- | :--- | :--- |
| **`packets_out`** | `snd_una` 〜 `snd_nxt` の間にある元データの総パケット数 | 新規データ送信時（`tcp_event_new_data_sent()`） | 累積 ACK で `snd_una` が進み rtx queue から `sk_buff` が解放されたとき（`tcp_clean_rtx_queue()`）、または FIN 離脱時 |
| **`sacked_out`** | `packets_out` のうち、SACK ブロック（または Reno の dupack 推定）で「相手には届いている」と分かっているパケット数 | 新たな SACK ブロックが報告されたとき（`tcp_sacktag_one()`） | **累積 ACK（`snd_una`）がその SACK 済みパケットを追い越して `sk_buff` が解放されたとき**（または RTO で SACK 情報がリセットされたとき） |
| **`lost_out`** | `packets_out` のうち、RACK や SACK 閾値によって「途中で落ちた」と判定されたパケット数 | ロス判定時（`tcp_mark_skb_lost()`、RTO 時は全未 SACK 分） | **累積 ACK でそのロス区間が回収されて `sk_buff` が解放されたとき**（または Undo 時） |
| **`retrans_out`** | `lost_out` のうち、すでに再送パケットを送り出して現在ワイヤ上を飛んでいる数 | 再送パケット送信時（`__tcp_retransmit_skb()`） | **累積 ACK で再送済みの `sk_buff` が解放されたとき**、または再送パケット自体が再びロスしたと判定されたとき |

- **逆に「一生増え続ける累積カウンタ」はどれか？**
  - コネクション開始からの累計で単調増加し続けるのは **`tp->delivered`**（相手に届いた累計パケット数、BBR の帯域計算などで使用）、**`tp->delivered_ce`**（ECN CE マーク付きの累計パケット数）、**`tp->total_retrans`**（コネクション全体の累計再送数）です。
