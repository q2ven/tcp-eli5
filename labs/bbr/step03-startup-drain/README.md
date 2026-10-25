# Step 3: STARTUP から DRAIN への遷移（満杯パイプ検出）

## 学習ゴール

このステップでは、BBR のステートマシンにおける **STARTUP（指数的な帯域探索）** から **DRAIN（STARTUP で作ったキューの排出）**、そして定常状態である **PROBE_BW（推定帯域での巡航）** への状態遷移を実装します。

- BBR の 3 つの基本モード（`BBR_STARTUP`、`BBR_DRAIN`、`BBR_PROBE_BW`）と、それぞれの `pacing_gain` / `cwnd_gain` の役割を理解します。
- パケット時間（`tp->delivered` と `ca->next_rtt_delivered`）に基づくラウンド（Round-Trip）計数の仕組みを実装します。
- `bbr_check_full_bw_reached()` を実装し、アプリ制限（`rs->is_app_limited`）でないラウンドが 3 回連続して 25% 以上（`1.25x`）の帯域成長を見せなかったときに「パイプが満杯になった（`full_bw_reached`）」と判定します。
- `bbr_check_drain()` を実装し、パイプ満杯検出時に `BBR_STARTUP` から `BBR_DRAIN` へ遷移させ、さらにインフライトパケット数（`tcp_packets_in_flight()`）が `1.0x BDP` 以下まで減った時点で `BBR_PROBE_BW` へ遷移させます。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. ロスを見ずにどうやって「パイプが満杯になった」と知るのでしょうか？

Reno や CUBIC のスロースタートは、パケットロスが起きるか（または HyStart で RTT 増加を検知するか）まで `cwnd` を倍増させ続けます。一方、パケットロスを輻輳シグナルとして使わない BBR は、`BBR_STARTUP`（ゲイン $2 / \ln(2) \approx 2.89\text{x}$）をいつ終えるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **BtlBw（推定ボトルネック帯域）の頭打ち（Plateau）を監視します**。
- パイプがまだ空いている間は、送信レートを毎ラウンド約 2 倍に増やせば、ACK から観測される配送レート（`ca->max_bw`）も毎ラウンド大きく伸びます。
- しかし、送信レートがボトルネック帯域に達すると、それ以上速く送ってもルータのキューが膨らむ（RTT が伸びる）だけで、**配送レート（`ca->max_bw`）は伸びなくなります**。
- BBR は「アプリ制限でないラウンドにおいて、`ca->max_bw` が基準値 `ca->full_bw` から **25% 以上（`bbr_full_bw_thresh = 5/4 = 1.25x`）伸びないラウンドが 3 回連続（`bbr_full_bw_cnt = 3`）** したら、パイプが満杯になった」と判定します。

</details>

### Q2. 時間（クロック）ではなく「パケット数」でラウンドを数えるにはどうしますか？

「3 ラウンド連続で帯域が 25% 伸びなかった」と判定するには、1 ラウンド（1 往復）の区切りを正確に知る必要があります。タイマーや時刻（`tcp_mstamp`）を使わずに、`tp->delivered`（累積配送パケット数）と `rs->prior_delivered` を使って 1 ラウンドの終了を検出するにはどうすればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- ラウンド開始時点の `tp->delivered` を `ca->next_rtt_delivered` に記録しておきます。
- 各パケット送信時、カーネルの `tcp_rate_skb_sent()` はその時点の `tp->delivered` を `skb` にスナップショットします。そのパケットが ACK されると、スナップショットされた値が `rs->prior_delivered` として返ってきます。
- したがって、`!before(rs->prior_delivered, ca->next_rtt_delivered)`（または 1 ウィンドウあたり 1 ACK の場合は `tp->delivered >= ca->next_rtt_delivered`）が成立した瞬間、「前のラウンド開始以降に送信したパケットの ACK が戻ってきた＝1 ラウンドが一周した」と分かります。
- その時点で `ca->next_rtt_delivered = tp->delivered;` に更新し、`ca->round_start = 1` を立てます。

</details>

### Q3. なぜ `BBR_STARTUP` から直接 `BBR_PROBE_BW` に行かず、`BBR_DRAIN` を挟むのでしょうか？

`BBR_STARTUP` で 3 ラウンド連続して帯域が伸びなかったと分かった時点で、ネットワーク内のキューにはどれくらいの余分なパケットが溜まっているでしょうか？また、それを素早く抜くには `pacing_gain` と `cwnd_gain` をどう設定すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- `BBR_STARTUP` では `bbr_high_gain`（約 `2.89x`）で送信し、かつ帯域頭打ちを検出するまでに 3 ラウンド見守るため、STARTUP 終了時点ではインフライト量が約 `2.89x BDP` まで膨らみ、ボトルネックに約 `1.89x BDP` もの過剰キューが作られています。
- そのため、すぐ巡航モード（`1.0x BtlBw`）に入ると過剰キューが抜けないまま残ってしまいます。
- そこでまず `BBR_DRAIN` に入り、`pacing_gain` を `bbr_high_gain` の逆数である `bbr_drain_gain = BBR_UNIT * 1000 / 2885`（`88 / 256`、約 `0.35x`）に落としてキューを排出します。一方、`cwnd_gain` は送信が突然止まらないよう `bbr_high_gain` を維持し、ペーシング主導で滑らかに減速させます。
- `tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)`（インフライト量が `1.0x BDP` 以下）になった時点でキューが抜けたと判断し、`BBR_PROBE_BW`（`pacing_gain = 1.0x`、`cwnd_gain = 2.0x`）へ遷移します。

</details>

### Q4. `bbr_set_pacing_rate()` で `ca->full_bw_reached` をチェックするのはなぜですか？

`bbr_set_pacing_rate()` には `if (ca->full_bw_reached || rate > sk->sk_pacing_rate)` という条件があります。なぜ `ca->full_bw_reached` が `0` の間はペーシングレートを下げることを禁止し、`1` になったら下げることを許可するのでしょうか？

<details>
<summary>ヒントと答え</summary>

- `BBR_STARTUP` の初期段階では、`cwnd` がまだ小さかったり一時的にアプリ制限になったりして、本来のボトルネック帯域より小さいレートサンプルしか得られないことがあります。パイプを満たす前（`!ca->full_bw_reached`）にペーシングレートを下げてしまうと、STARTUP の加速が止まってしまいます。
- 一方、パイプが満杯になった後（`ca->full_bw_reached == 1`）は、`BBR_DRAIN`（約 `0.35x`）や `BBR_PROBE_BW`（`1.0x`）へ遷移した際に、`2.89x BtlBw` から低いペーシングレートへ引き下げる必要があるため、無条件で `sk->sk_pacing_rate = rate` の更新を許可します。

</details>

---

## 実装ステップと解説

### 1. モードと固定小数点定数

`BBR_SCALE = 8`（`BBR_UNIT = 256` が `1.0x`）の固定小数点で各ゲインとしきい値を定義します。

- `bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1 = 739`（約 `2.89x`）: `BBR_STARTUP` の `pacing_gain` および `cwnd_gain` です。
- `bbr_drain_gain = BBR_UNIT * 1000 / 2885 = 88`（約 `0.35x`）: `BBR_DRAIN` の `pacing_gain` です。
- `bbr_cwnd_gain = BBR_UNIT * 2 = 512`（`2.0x`）: `BBR_PROBE_BW` の `cwnd_gain` です。
- `bbr_full_bw_thresh = BBR_UNIT * 5 / 4 = 320`（`1.25x`）: STARTUP 継続に必要な帯域成長率（+25%）です。
- `bbr_full_bw_cnt = 3`: 25% 以上の成長が見られないラウンドが 3 回続いたらパイプ満杯と判定します。

### 2. `bbr_check_full_bw_reached()` — 満杯パイプの検出

ラウンドの区切り（`ca->round_start == 1`）かつアプリ制限でない（`!rs->is_app_limited`）サンプルが届いたときに呼ばれます。

1. `bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE` で、基準帯域 `ca->full_bw` の 1.25 倍を計算します。
2. `ca->max_bw >= bw_thresh` であれば、帯域はまだ 25% 以上伸びているため、`ca->full_bw = ca->max_bw` に更新して `ca->full_bw_cnt = 0` にリセットします。
3. 伸びていなければ `++ca->full_bw_cnt` を進め、`bbr_full_bw_cnt`（`3`）に達したら `ca->full_bw_reached = 1` を立てます。

### 3. `bbr_check_drain()` — `BBR_DRAIN` と `BBR_PROBE_BW` への遷移

1. `ca->mode == BBR_STARTUP && ca->full_bw_reached` が成り立ったら、`ca->mode = BBR_DRAIN` に遷移し、観測用に `tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT)`（`1.0x BDP`）を記録します。
2. 続いて（`else if` ではなく独立した `if` で fall-through させることで、すでにインフライトが少ない場合は同じ ACK 内で即座に）`ca->mode == BBR_DRAIN && tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)` を判定し、条件を満たせば `ca->mode = BBR_PROBE_BW` に遷移します。

### 4. `bbr_update_gains()` と `bbr_set_cwnd()`

- `bbr_update_gains()` は現在の `ca->mode` に応じて `ca->pacing_gain` と `ca->cwnd_gain` を設定します。
- `bbr_set_cwnd()` は `target_cwnd = bbr_bdp(sk, bw, gain)` を計算し、`ca->full_bw_reached` が立った後は `cwnd = min(cwnd + acked, target_cwnd)` で `target_cwnd`（`BBR_PROBE_BW` では `2.0x BDP`）へ収束させます。

---

## packetdrill テストの解説

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step03-startup-drain/test.pkt) は、一定レート（1 セグメント / 10 ms = `100 KB/s`）のボトルネック経路で接続を走らせ、4 ラウンド後に `BBR_STARTUP` を抜けることを検証します。

1. **接続確立と 5,000 バイトの一括書き込み**:
   - `MSS = 1000` バイト、`RTT = 10 ms`、受信ウィンドウ `win 1000`（1 セグメント）で 3-way ハンドシェイクを行います。
   - `write(4, ..., 5000)` で 5 セグメント分を一度に送信キューへ積むことで、ウォームアップ後の Round 1〜4 で送信キューが空にならず、`rs->is_app_limited == 0` の状態で毎ラウンド 1 セグメント（1,000 バイト）ずつ送信されます。
2. **Round 1（`1001:2001`）**:
   - `ca->max_bw`（約 `100 KB/s`）が `ca->full_bw` に記録され、`ca->full_bw_cnt = 0` となります。
   - モードは `BBR_STARTUP`（`pacing_gain = 739/256 ~= 2.89x`）であり、`tcpi_pacing_rate` が約 `286 KB/s`（`250000 < tcpi_pacing_rate < 320000`）、`tcpi_snd_ssthresh` が初期値 `0x7fffffff` であることをアサートします。
3. **Round 2・Round 3（`2001:3001`、`3001:4001`）**:
   - ボトルネック帯域が `100 KB/s` で一定のため、帯域成長率は 0%（`< 25%`）となり、`ca->full_bw_cnt` が `1` → `2` と増加します。まだ 3 ラウンド未満なので `BBR_STARTUP` に留まります。
4. **Round 4（`4001:5001`）**:
   - 3 ラウンド連続で +25% 未満の成長となったため、`ca->full_bw_cnt` が `3` に達して `ca->full_bw_reached = 1` となります。
   - `bbr_check_drain()` により `BBR_STARTUP` → `BBR_DRAIN`（`tp->snd_ssthresh = bbr_bdp(...)`）へ遷移し、さらにインフライト数が `0 <= BDP` であるため `BBR_PROBE_BW`（`pacing_gain = 1.0x`、`cwnd_gain = 2.0x`）へ遷移します。
   - その結果、`tcpi_pacing_rate` が約 `99 KB/s`（`90000 < tcpi_pacing_rate < 110000`）に低下し、`1 <= tcpi_snd_ssthresh <= 4`、`tcpi_snd_cwnd == 4`（最小ターゲット `bbr_cwnd_min_target = 4`）になったことをアサートします。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step03-startup-drain starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step03-startup-drain solution
```
