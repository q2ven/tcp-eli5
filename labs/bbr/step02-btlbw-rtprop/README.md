# Step 2: BtlBw (最大帯域フィルタ) と RTprop (最小 RTT) による BDP 計算

## 学習ゴール

このステップでは、Step 1 のペーシングレート制御を発展させ、BBR の中核モデルである **BtlBw（ボトルネック帯域幅: 最大帯域フィルタ）** と **RTprop（往復伝播遅延: 最小 RTT フィルタ）** を追跡し、両者の積である **BDP（帯域遅延積: Bandwidth-Delay Product）** から `snd_cwnd` と `sk_pacing_rate` を決定する仕組みを実装します。

- `inet_csk_ca(sk)` のプライベート領域（`struct bpf_bbr2_ca`）に、フローごとの BBR モデル状態（`min_rtt_us`, `max_bw`, `round_cnt`, `next_rtt_delivered`）を保持する方法を理解します。
- なぜ RTT は **最小値フィルタ（min-filter）** で、配送レート（帯域）は **最大値フィルタ（max-filter）** で追跡するのかという物理的意味を理解します。
- アプリケーションの送信データ不足（`rs->is_app_limited`）による低いレートサンプルが `max_bw` を引き下げてしまう問題と、そのフィルタ条件を理解します。
- 固定小数点単位 `BW_UNIT`（`1 << 24`）で表された `max_bw` と `min_rtt_us` から BDP（パケット数）を計算する `bbr_bdp()` を実装し、`tp->snd_cwnd = max(bdp + 4U, 4U)` を設定します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書き始める前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. なぜ RTT は「最小値」を取り、帯域（Delivery Rate）は「最大値」を取るのでしょうか？

Step 1 のように直近の ACK サンプルで毎回レートや RTT を上書きすると、何が起きるでしょうか？ネットワーク経路の物理的な **往復伝播遅延（RTprop）** と **ボトルネック帯域（BtlBw）** を推定するために、それぞれ最小値・最大値のどちらを記録すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **RTprop（最小 RTT フィルタ）**: 観測される RTT は「物理的な伝播遅延（RTprop）＋ボトルネックキューでの待ち時間（Queueing Delay）」の和です。キューが空のときにだけ真の伝播遅延が観測され、キューが溜まると RTT は必ず大きくなるため、**最小値（min-filter）** を取ることでキュー遅延を除いた `min_rtt_us` を推定できます。
- **BtlBw（最大帯域フィルタ）**: 観測される配送レート（`rs->delivered / rs->interval_us`）は、物理リンクのボトルネック容量を超えることはありませんが、送信量が少ないときや他フローとの一時的な競合で容易にボトルネック容量を下回ります。したがって、達成できた **最大値（max-filter）** を取ることでボトルネック帯域 `max_bw` を推定します。

</details>

### Q2. アプリケーションが少しずつしかデータを送らない（App-Limited）とき、`max_bw` はどう扱うべきでしょうか？

高帯域で 10 パケットを一気に送って `max_bw` を計測したあと、アプリケーションがしばらく休止し、2 パケットだけをゆっくり送信したとします。このときの `rate_sample`（`rs->is_app_limited == true`）の配送レートは非常に低くなります。どのような条件のときだけ `ca->max_bw` を更新すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- パイプ（ネットワーク経路）を満たすほど送信していない期間（`rs->is_app_limited != 0`）の配送レートは、ネットワークの上限ではなく「アプリケーションがどれだけ書いたか」を表しているに過ぎません。
- そのため、`!rs->is_app_limited || bw >= ca->max_bw` のときだけフィルタに入力し、かつ `bw > ca->max_bw` のときに `ca->max_bw = bw` と更新します（App-Limited であっても、過去の `max_bw` を上回るサンプルが得られた場合は、真の帯域がそれ以上である証拠なので更新を許可します）。

</details>

### Q3. パケット単位の「ラウンド（Packet-timed Round Trip）」はどうやって数えますか？

今後のステップ（Startup の終了判定や ProbeBW のフェーズ遷移）では、「何 RTT（ラウンド）経過したか」を時計ではなく配送パケット数ベースで数える必要があります。`tp->delivered` と `rs->prior_delivered` を使って、1 ラウンドの完了をどう判定できるでしょうか？

<details>
<summary>ヒントと答え</summary>

- ラウンド開始時点の累積配送パケット数を `ca->next_rtt_delivered = tp->delivered` に記録しておきます。
- その後到着する ACK の `rs->prior_delivered`（その ACK で確認されたパケットが送信された時点での `tp->delivered`）が `ca->next_rtt_delivered` 以上になったとき（`!before(rs->prior_delivered, ca->next_rtt_delivered)`）、そのラウンド開始以降に送ったパケットの ACK が返ってきた＝**1 ラウンド（1 RTT）が経過した** と判定できます。
- そのタイミングで `ca->round_cnt++` し、次の目標値を `ca->next_rtt_delivered = tp->delivered` に進めます。

</details>

### Q4. `BW_UNIT`（`1 << 24`）でスケールされた `max_bw` と `min_rtt_us` から、どうやって BDP（パケット数）を計算しますか？

`rs->delivered`（パケット数）を `rs->interval_us`（マイクロ秒）で割ると通常は `1` 未満の小数になってしまいます。そこで Linux の `tcp_bbr.c` と同様に `BW_SCALE = 24`（`BW_UNIT = 1 << 24`）を掛けてから割った値を `ca->max_bw` とします。この `ca->max_bw` と `ca->min_rtt_us`、そして `gain`（`BBR_UNIT = 1 << 8`）から、切り上げ（ceil）込みの BDP パケット数を求める整数式を書いてみましょう。

<details>
<summary>ヒントと答え</summary>

- まず 64 ビット整数で `w = (__u64)bw * ca->min_rtt_us;` を計算します（この時点で `w` は `BDP * BW_UNIT` に相当します）。
- 次に `gain`（`BBR_SCALE = 8`）を掛けてシフトし、`BW_UNIT` で切り上げ除算（`+ BW_UNIT - 1` してから `/ BW_UNIT`）を行います：
  ```c
  bdp = (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
  ```
- 切り上げ（ceil）にする理由は、端数の切り捨てによって `snd_cwnd` が真の BDP より小さくなり、パイプを 100% 満たせなくなる負のフィードバックループを防ぐためです。

</details>

---

## 実装ステップと解説

### 1. ソケットごとの状態構造体 `struct bpf_bbr2_ca`

`inet_csk(sk)->icsk_ca_priv` には各輻輳制御アルゴリズムが自由に使えるプライベート領域が用意されており、[bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h) の `inet_csk_ca(sk)` ヘルパーでポインタを取得できます。

```c
struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};
```

- `min_rtt_us`: 観測された最小 RTT（RTprop、単位: マイクロ秒）。未初期化時は `0` です。
- `max_bw`: 観測された最大配送レート（BtlBw、単位: `packets / usec << BW_SCALE`）。
- `round_cnt`: パケット基準で計測したラウンドトリップ回数。
- `next_rtt_delivered`: 現在のラウンドが終了する `tp->delivered` の境界値。

### 2. `bbr_bdp()` と `bbr_rate_bytes_per_sec()`

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) では、Linux カーネルの `tcp_bbr.c` と同じスケール定数（`BW_SCALE = 24`, `BBR_SCALE = 8`）を使って 2 つのコア計算ヘルパーを定義しています。

- `bbr_rate_bytes_per_sec()`: `bw`（`pkts/us << 24`）に `mss`・`gain`・`USEC_PER_SEC`（1% のペーシングマージン `BBR_PACING_MARGIN_PCT` を差し引いた値）を掛け、`BBR_SCALE` と `BW_SCALE` で右シフトして `bytes/sec` に変換します。
- `bbr_bdp()`: `ca->min_rtt_us` または `bw` が未計測（`0`）の間は安全な初期値 `10U` を返し、計測済みの場合は `ceil(bw * min_rtt_us * gain)` をパケット数として返します。

### 3. `bpf_bbr2_cong_control()` — BtlBw・RTprop の更新と `snd_cwnd` 設定

ACK ごとに呼ばれる `bpf_bbr2_cong_control()` は以下の 4 ステップで動作します。

1. **RTprop（`min_rtt_us`）の最小値フィルタ更新**:
   - `rs->rtt_us > 0` であり、かつ `ca->min_rtt_us == 0`（初回）または `rs->rtt_us < (__s32)ca->min_rtt_us` のときだけ `ca->min_rtt_us = rs->rtt_us` に更新します。
2. **パケットタイムド・ラウンドの進行と BtlBw（`max_bw`）の最大値フィルタ更新**:
   - `rs->delivered > 0 && rs->interval_us > 0` のとき、`!before(rs->prior_delivered, ca->next_rtt_delivered)` であれば `ca->round_cnt++` し、`ca->next_rtt_delivered = tp->delivered` に更新します。
   - `bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us)` でサンプル帯域を計算します。
   - `!rs->is_app_limited || bw >= ca->max_bw` を満たし、かつ `bw > ca->max_bw` であれば `ca->max_bw = bw` を更新します。
3. **ペーシングレートの更新**:
   - `bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT)` により、`ca->max_bw`（`1.0x` ゲイン）から算出したレートを `sk->sk_pacing_rate` に設定します。
4. **BDP に基づく `snd_cwnd` の設定**:
   - `bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT)` を計算し、遅延 ACK や TSO/GSO 量子化を見込んだ最低 4 パケットの余裕を加えて `tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET)` に設定します。

---

## packetdrill テストの解説

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/test.pkt) は、高帯域バーストのあとに「App-Limited かつキュー遅延で RTT が膨らんだ低レートサンプル」を注入し、`bpf_bbr2` が真の `BtlBw` と `RTprop` を保持し続けることを検証します。

1. **3-way ハンドシェイク（RTT = 10 ms, ペイロード MSS = 1000 B）**:
   - `0.00` で SYN / SYN-ACK、`+0.01`（10 ms 後）に ACK が到着し、コネクションが確立されます。
2. **Phase 1: 高帯域バースト（10 セグメント / 10 ms）**:
   - `write(4, ..., 10000)` で 10 セグメント（`1:10001(10000)`）を送信し、`+0.01`（10 ms 後）に ACK を受け取ります。
   - このサンプルは `rs->rtt_us = 10000`（10 ms）、`rs->delivered = 10`、`rs->interval_us = 10000`（10 ms）となります。
   - したがって `ca->min_rtt_us = 10000`、`ca->max_bw` は `1000 pkts/sec`（`1,000,000 B/s`）となり、`bdp = 10` パケット、`tcpi_snd_cwnd = 10 + 4 = 14`、`tcpi_pacing_rate` は約 `990,000 B/s`（1% マージン差し引き後）になることをアサートします。
3. **Phase 2: App-Limited な小バースト＋膨張した RTT（2 セグメント / interval 50 ms, RTT 20 ms）**:
   - 30 ms 待機したあと（`+0.03`）、`write(4, ..., 2000)` で 2 セグメントのみを送信し、20 ms 後（`+0.02`）に ACK を返します。
   - このサンプルでは `rs->rtt_us = 20000`（20 ms に膨張）、`rs->delivered = 2`、`rs->interval_us = 50000`（40 pkts/sec に低下）となります。
   - [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c) のように毎回 `min_rtt_us` と `max_bw` を上書きしてしまうと、レートが 1/25 に激減して `tcpi_snd_cwnd` と `tcpi_pacing_rate` が崩壊し、アサートに失敗します。
   - 一方、[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) では最小 RTT（10 ms）と最大帯域（1000 pkts/sec）がフィルタによって維持されるため、Phase 2 の ACK 後も `tcpi_snd_cwnd == 14` と `tcpi_pacing_rate`（約 `990 KB/s`）がそのまま保たれます。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step02-btlbw-rtprop starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step02-btlbw-rtprop solution
```
