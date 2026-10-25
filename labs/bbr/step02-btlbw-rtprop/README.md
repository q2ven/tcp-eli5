# Step 2: BtlBw (最大帯域フィルタ) と RTprop (最小 RTT フィルタ) による BDP 計算

## 1. 学習ゴール

Step 1 では、直近の ACK から得られた配送レート（`rs->delivered / rs->interval_us`）を使って毎回 `sk_pacing_rate` を上書きしました。しかし実際のネットワークでは、ルータのキューにパケットが溜まれば RTT は一時的に膨らみ、アプリケーションの送信が一時停止すれば配送レートは本来の回線容量よりはるかに低い値になります。このステップでは、BBR のネットワーク経路モデルの 2 本柱である **RTprop（往復伝播遅延: 最小 RTT フィルタ）** と **BtlBw（ボトルネック帯域幅: 最大帯域フィルタ）** をソケットのプライベート領域（`icsk_ca_priv`）で追跡し、両者の積である **BDP（帯域遅延積: Bandwidth-Delay Product）** から `snd_cwnd` と `sk_pacing_rate` を決定する仕組みを実装します（トラック全体の概要は [../README.md](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/README.md) を参照してください）。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

コードの設計に入る前に、BBR がどのようにして「ノイズの混じった毎回の ACK サンプル」から「経路の真の物理特性（伝播遅延とボトルネック帯域）」を抽出するのか、そしてカーネルがどのデータ構造を提供しているのかを整理しましょう。

### なぜ「最新のサンプルで毎回上書き」してはいけないのか？

TCP 送信側が ACK を受け取るたびに観測できるのは、その時点の **瞬間的な RTT（`rs->rtt_us`）** と **瞬間的な配送レート（`rs->delivered / rs->interval_us`）** です。しかし、これらをそのまま経路の特性として上書きすると 2 つの深刻な問題が起きます。

1. **キューイング遅延による RTT の膨張**:
   観測される RTT は、光ファイバやルータの物理的な往復伝播遅延（ $\text{RTprop}$ ）と、途中のボトルネックルータのバッファでの待ち時間（ $\text{QueueDelay}_t \ge 0$ ）の和です。
   $$\text{RTT}_t = \text{RTprop} + \text{QueueDelay}_t$$
   待ち時間 $\text{QueueDelay}_t$ は決して負にならないため、バッファが空のときにだけ $\text{RTT}_t = \text{RTprop}$ となり、バッファにパケットが溜まると $\text{RTT}_t$ は必ず $\text{RTprop}$ より大きくなります。したがって、経路の真の伝播遅延 $\text{RTprop}$ を知るには、観測された有効な RTT サンプル（`rs->rtt_us > 0`）の **最小値（min-filter）** を記録し続けなければなりません。

2. **送信データ不足（App-Limited）による配送レートの低下**:
   逆に、ある区間で観測される配送レート $\text{bw}_t = \text{delivered} / \Delta t$ は、物理的なボトルネックリンク容量（ $\text{BtlBw}$ ）を絶対に超えることはできません（ $\text{bw}_t \le \text{BtlBw}$ ）。パケットはボトルネックリンクが送り出せる速度以上では受信側に届かないからです。
   一方で、送信側がパイプ（経路）を満たすほど十分なデータを送っていないとき、配送レートは簡単に $\text{BtlBw}$ を下回ります。したがって、ボトルネック帯域 $\text{BtlBw}$ を推定するには、観測された配送レートの **最大値（max-filter）** を取る必要があります。

### App-Limited（アプリケーション制限）サンプルの不変条件

特に注意が必要なのが、Web サーバのレスポンス間や対話型通信のように、アプリケーションが一時的に少ししかデータを書き込まない **App-Limited（アプリケーション制限）** 状態です。

たとえば $1000\text{ pkts/s}$ の回線で、最初に 10 パケットをバースト送信して $\text{max}_{\text{bw}} = 1000\text{ pkts/s}$ を計測したあと、アプリケーションが $30\text{ ms}$ 休止してから 2 パケットだけを送ったとします。このとき配送区間は $\Delta t = 50\text{ ms}$ に引き伸ばされ、見かけの配送レートは $2 / 50\text{ ms} = 40\text{ pkts/s}$ まで落ち込みます。これはネットワークが遅くなったのではなく、**アプリケーションがデータを送らなかっただけ** です。

Linux カーネルの配送レート計測器（`tcp_rate.c`）は、送信ウィンドウやパイプを使い切っていない状態で計測されたサンプルに自動的に `rs->is_app_limited = 1` フラグを立てます。BBR はこのフラグに対して次の不変条件（ルール）を設けています。

- **App-Limited でない通常のサンプル（`!rs->is_app_limited`）**: ボトルネックを試せていたサンプルなので、最大値フィルタに入力します（`bw > ca->max_bw` なら更新）。
- **App-Limited なサンプル（`rs->is_app_limited != 0`）**: 原則として無視します。**ただし、App-Limited でありながら現在の `ca->max_bw` 以上のレートが出た場合（`bw >= ca->max_bw`）は例外として更新を許可します。** パイプを満たしていなくても過去の最大値を超えたのであれば、物理回線が少なくともそれ以上の速度を持つ確かな証拠だからです。

これを 1 つの条件式にまとめると、以下のようになります。

```c
if (!rs->is_app_limited || bw >= ca->max_bw) {
	if (bw > ca->max_bw)
		ca->max_bw = bw;
}
```

```
  配送レート (bw)                             RTT (rtt_us)
      ^                                           ^
1000  |   [Phase 1: 10 pkts / 10 ms]        20 ms |                 [Phase 2: キュー遅延込み]
pkts/s|   *========================> max_bw       |                 * (min-filter で無視)
      |   |                                 10 ms |   [Phase 1]     |
  40  |   |             [Phase 2: App-Limited]    |   *=============+==========> min_rtt_us
pkts/s|   |             * (無視して保持!)         |   |
      +---+-------------+----------> 時間         +---+-------------+----------> 時間
```

### BDP（帯域遅延積）の固定小数点計算と `snd_cwnd`

最小 RTT（`ca->min_rtt_us`）と最大帯域（`ca->max_bw`）が求まれば、パイプをちょうど 100% 満たすのに必要なインフライトパケット数である **BDP（Bandwidth-Delay Product: 帯域遅延積）** が計算できます。

$$\text{BDP (packets)} = \text{BtlBw (packets / }\mu\text{s)} \times \text{RTprop (}\mu\text{s)}$$

BPF / カーネル内では浮動小数点数が使えないため、Linux カーネルの `tcp_bbr.c` と同じ 2 つの固定小数点スケール定数を使います。

- **`BW_SCALE = 24`（`BW_UNIT = 1U << 24 = 16,777,216`）**:
  1 マイクロ秒あたりに届くパケット数（`rs->delivered / rs->interval_us`）は通常 `1` 未満の小さな小数になるため、割り算の前に `BW_UNIT` を掛けて整数化します。
  $$\text{bw} = \left\lfloor \frac{\text{rs}\to\text{delivered} \times \text{BW}_{\text{UNIT}}}{\text{rs}\to\text{interval}_{\text{us}}} \right\rfloor$$
- **`BBR_SCALE = 8`（`BBR_UNIT = 1U << 8 = 256`）**:
  ゲイン（倍率）を表す固定小数点単位です。等倍（ $1.0\times$ ）は `BBR_UNIT`（`256`）で表されます。

この `bw`（単位: $\text{packets}/\mu\text{s} \times 2^{24}$ ）と `ca->min_rtt_us`（単位: $\mu\text{s}$ ）、および `gain`（単位: $2^8$ ）から BDP パケット数を求める式は次の通りです（`bbr_bdp()`）：

1. まず 64 ビット整数で `w = (__u64)bw * ca->min_rtt_us` を計算します（単位は $\text{packets} \times \text{BW}_{\text{UNIT}}$ ）。
2. 次に `gain` を掛けて `BBR_SCALE`（`8` ビット）右シフトし、最後に `BW_UNIT` で **切り上げ除算（ceil）** を行います。
   ```c
   return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
   ```
   ここで単なる切り捨て（`/ BW_UNIT`）ではなく `+ BW_UNIT - 1` を足して **切り上げ（ceil）** にするのには重要な理由があります。もし端数を切り捨てて `snd_cwnd` が真の BDP より 1 パケットでも小さくなると、送信側はパイプを 100% 満たせなくなり、結果としてそれ以上の帯域を計測できなくなる「負のフィードバックループ」に陥るためです。
3. 未計測時（`ca->min_rtt_us == 0` または `bw == 0`）は、ゼロ除算や極端な縮小を防ぐため安全な初期ウィンドウ `10U` パケットを返します。
4. 算出された `bdp` に対し、受信側の遅延 ACK（Delayed ACK）や TSO/GSO 送信の量子化誤差を吸収するためのマージンとして `4` パケットを加え、最小ターゲット `BBR_CWND_MIN_TARGET`（`4U`）との最大値を `tp->snd_cwnd` に設定します。
   $$\text{snd}_{\text{cwnd}} = \max(\text{bdp} + 4, 4)$$

### パケットタイムド・ラウンド（`round_cnt`）の数え方

後のステップ（Step 3 の Startup 終了判定や Step 4 の ProbeBW ゲインサイクル）では、「何ラウンドトリップ（RTT）経過したか」を時計の秒数ではなく **パケットの往復回数（Packet-timed Round Trip）** で数えます。このステップでその土台も一緒に実装します。

1. ラウンド開始時点の累積配送パケット数を `ca->next_rtt_delivered = tp->delivered` に記録しておきます。
2. カーネルは各パケットを送信するとき、その時点の `tp->delivered` をパケットのメタデータに記録しています。そのパケットに対する ACK が返ってくると、送信時点での `tp->delivered` の値が `rs->prior_delivered` に入っています。
3. したがって、到着した ACK の `rs->prior_delivered` が `ca->next_rtt_delivered` 以上になった瞬間（32 ビット符号なし整数のラップアラウンドを考慮して `!before(rs->prior_delivered, ca->next_rtt_delivered)` が真になった瞬間）、「このラウンドが始まった後に送信されたパケットが 1 往復して ACK された＝**1 ラウンドが完了した**」と判定できます。
4. 1 ラウンド完了したら `ca->round_cnt++` でカウンタを 1 増やし、次のラウンドの終了境界を現在の `ca->next_rtt_delivered = tp->delivered` に更新します。

### 具体例（`test.pkt` の数値で計算してみよう）

MSS = $1000\text{ Bytes}$ の経路で、以下の 2 つのフェーズが順に起きたときの計算を追ってみましょう。

- **Phase 1（フルスピードの 10 パケット送信）**:
  - 10 パケット（ $10,000\text{ Bytes}$ ）を送信し、 $10\text{ ms}$ （ $10,000\ \mu\text{s}$ ）後に ACK が到着。
  - `rs->rtt_us = 10000`, `rs->delivered = 10`, `rs->interval_us = 10000`, `rs->is_app_limited = 0`。
  - `ca->min_rtt_us` は初回（`0`）なので `10000` に更新。
  - $\text{bw} = \lfloor 10 \times 16,777,216 / 10000 \rfloor = 16,777$ （約 $0.001\text{ pkts/}\mu\text{s} = 1000\text{ pkts/s}$ ）。
  - `!rs->is_app_limited` かつ `bw > ca->max_bw` なので、`ca->max_bw = 16777` に更新。
  - `bbr_bdp()` では $w = 16,777 \times 10000 = 167,770,000$ 。等倍ゲイン（`gain = BBR_UNIT`）で切り上げ除算すると $\lceil 167,770,000 / 16,777,216 \rceil = 10\text{ パケット}$ 。
  - よって `tp->snd_cwnd = max(10 + 4, 4) = 14`、`sk_pacing_rate` は約 $990,000\text{ Bytes/sec}$ （ $1\text{ MB/s}$ から 1% マージンを引いた値）になります。
- **Phase 2（ $30\text{ ms}$ 休止後の App-Limited な 2 パケット送信＋キュー遅延）**:
  - アプリが $30\text{ ms}$ 休止したあと 2 パケットだけを送信し、別トラフィックのキュー遅延により $20\text{ ms}$ かかって ACK が到着。
  - `rs->rtt_us = 20000`, `rs->delivered = 2`, `rs->interval_us = 50000`, `rs->is_app_limited = 1`。
  - **RTprop フィルタ**: `rs->rtt_us`（`20000`）は `ca->min_rtt_us`（`10000`）より大きいため無視され、`ca->min_rtt_us = 10000` が維持されます。
  - **BtlBw フィルタ**: 今回の $\text{bw} = \lfloor 2 \times 16,777,216 / 50000 \rfloor = 671$ （ $40\text{ pkts/s}$ ）ですが、`rs->is_app_limited == 1` かつ `bw < ca->max_bw` なので無視され、`ca->max_bw = 16777` が維持されます。
  - その結果、Phase 2 の後も `tp->snd_cwnd == 14` と `sk_pacing_rate`（約 $990\text{ KB/s}$ ）が崩れずに保たれます。

### カーネルが提供するフィールドとヘルパー（このステップで使うもの）

コードを書く前に、カーネル（および [bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h)）がすでに用意している構造体フィールドとヘルパー関数を確認しておきましょう。

- **`inet_csk(sk)->icsk_ca_priv` と `inet_csk_ca(sk)`**:
  - 各 TCP ソケット（`struct inet_connection_sock`）には、輻輳制御アルゴリズムがソケットごとの独自変数を保存できるバイト配列 `icsk_ca_priv` が組み込まれています。
  - `inet_csk_ca(sk)` を呼ぶと `icsk_ca_priv` の先頭ポインタ（`void *`）が返るため、`struct bpf_bbr2_ca *ca = inet_csk_ca(sk);` のように自分の構造体ポインタへキャストして読み書きできます。
- **`struct rate_sample *rs`（`cong_control` コールバックの第 4 引数）**:
  - `rs->rtt_us`（`__s32`）: 今回の ACK で計測された RTT（マイクロ秒）。RTT が計測できなかった ACK では `-1`（負の値）または `0` になります。
  - `rs->delivered`（`__s32`）: 測定区間内に受信側へ届けられたパケット数。
  - `rs->interval_us`（`__s32`）: そのパケット群が配送されるのに要した時間（マイクロ秒）。
  - `rs->prior_delivered`（`__u32`）: 今回 ACK されたパケットが送信された時点での `tp->delivered` のスナップショット値。
  - `rs->is_app_limited`（`bool`）: 測定区間中に送信側がアプリケーション制限（データ不足）を経験していた場合に真（`true` / `1`）となります。
- **`struct tcp_sock *tp`（`tcp_sk(sk)` で取得）**:
  - `tp->delivered`（`__u32`）: コネクション開始以降に受信側へ届けられた累積パケット数。
  - `tp->snd_cwnd`（`__u32`）: 送信輻輳ウィンドウ（パケット単位）。
  - `tp->snd_ssthresh`（`__u32`）: スロースタート閾値（BBR では従来のスロースタートを使わないため `TCP_INFINITE_SSTHRESH` に固定します）。
  - `tp->srtt_us`（`__u32`）: 8 倍にスケールされた平滑化 RTT（`tp->srtt_us >> 3` がマイクロ秒単位の SRTT）。
  - `tp->mss_cache`（`__u32`）: 現在の MSS（バイト単位）。
- **`struct sock *sk`**:
  - `sk->sk_pacing_status`: `SK_PACING_NEEDED` を設定することでカーネルの TCP ペーシングを有効化します。
  - `sk->sk_pacing_rate`（`unsigned long`）: 送信ペーシングレート（Bytes/sec）。
  - `sk->sk_max_pacing_rate`（`unsigned long`）: アプリケーションが `SO_MAX_PACING_RATE` で設定した上限レート。
- **ヘルパー関数（[bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h)）**:
  - `before(seq1, seq2)`: 32 ビット符号なし整数のラップアラウンドを考慮して `seq1 < seq2` を判定します。したがって `!before(a, b)` は `a >= b` を意味します。
  - `div64_u64(dividend, divisor)`: 64 ビット符号なし整数同士の安全な除算（`divisor == 0` のときは `0` を返す）を行います。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の仕組みを踏まえて、[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c) の TODO を埋めるための設計を 4 つの問いで確認しましょう。すべての答えはセクション 2 の内容から導き出せます。

### Q1. 初回サンプル（`ca->min_rtt_us == 0`）も考慮した `RTprop` 最小値フィルタの条件式はどう書くべきでしょうか？

`bpf_bbr2_init()` では `ca->min_rtt_us = 0;` に初期化されています。もし `bpf_bbr2_cong_control()` で単純に `if (rs->rtt_us > 0 && rs->rtt_us < ca->min_rtt_us)` とだけ書いてしまうと、何が起きるでしょうか？また、型比較（`rs->rtt_us` は符号付き `__s32`、`ca->min_rtt_us` は符号なし `__u32`）も含めて正しい条件式はどうなりますか？

<details>
<summary>ヒントと答え</summary>

- `ca->min_rtt_us` の初期値は `0` なので、単に `rs->rtt_us < ca->min_rtt_us` と書くと、正の `rs->rtt_us` は絶対に `0` より小さくならず、`ca->min_rtt_us` が永遠に `0` のまま更新されなくなってしまいます。
- したがって、有効な RTT サンプル（`rs->rtt_us > 0`）が得られたら、「まだ未計測（`ca->min_rtt_us == 0`）であるか、または現在の `ca->min_rtt_us` より小さい場合」に更新する必要があります。
  ```c
  if (rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
  	ca->min_rtt_us = rs->rtt_us;
  ```

</details>

### Q2. `starter.bpf.c` の TODO 2 で `ca->max_bw` を更新する C コードはどう書くべきでしょうか？

[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c) では `ca->max_bw = bw;` と無条件に上書きしています。セクション 2 で学んだ「App-Limited サンプルの不変条件」と「最大値フィルタ」の両方を満たすには、この部分をどのような `if` 文に書き換えればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- セクション 2 の通り、サンプルが App-Limited でない（`!rs->is_app_limited`）か、あるいは App-Limited であっても現在の最大帯域以上（`bw >= ca->max_bw`）である場合にのみ最大値フィルタを通し、`bw > ca->max_bw` であれば `ca->max_bw` を更新します。
  ```c
  if (!rs->is_app_limited || bw >= ca->max_bw) {
  	if (bw > ca->max_bw)
  		ca->max_bw = bw;
  }
  ```
- なお、論理的には `if (bw > ca->max_bw)` の 1 行だけでも「`bw <= ca->max_bw` のサンプル（App-Limited かどうかを問わず）は `max_bw` を引き下げない」という同じ結果になりますが、Linux カーネルの `bbr_update_bw()`（将来のステップでウィンドウ付き max-filter に拡張される箇所）の構造に合わせて、まず `if (!rs->is_app_limited || bw >= ca->max_bw)` で App-Limited サンプルをフィルタする形を明示しています。

</details>

### Q3. パケットタイムド・ラウンド（`ca->round_cnt`）はいつ、どのように進めますか？

`bpf_bbr2_init()` で `ca->next_rtt_delivered = tp->delivered;` と初期化したあと、`bpf_bbr2_cong_control()` で有効な配送サンプル（`rs->delivered > 0 && rs->interval_us > 0`）が届いたときに 1 ラウンドの完了を判定して次のラウンドを開始するコードはどうなりますか？

<details>
<summary>ヒントと答え</summary>

- `rs->prior_delivered`（ACK されたパケットの送信時点の `tp->delivered`）が `ca->next_rtt_delivered` に追いついたかどうかを `!before(rs->prior_delivered, ca->next_rtt_delivered)` で判定します。
- 条件を満たしたら `ca->round_cnt++` でラウンド数を増やし、次のラウンドの終了境界を現在の `tp->delivered` に設定します。
  ```c
  if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
  	ca->round_cnt++;
  	ca->next_rtt_delivered = tp->delivered;
  }
  ```

</details>

### Q4. `bbr_bdp()` が未初期化時のガードと切り上げ除算（ceil）を行っているのはなぜですか？

`bbr_bdp()` の実装を見ると、先頭で `if (!ca->min_rtt_us || !bw) return 10U;` をチェックし、後半で `(((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT` を計算しています。この 2 つの処理がそれぞれ防いでいるエッジケースは何でしょうか？

<details>
<summary>ヒントと答え</summary>

- **未初期化ガード（`!ca->min_rtt_us || !bw`）**: コネクション直後や最初の有効な配送サンプルが届く前は `min_rtt_us` または `max_bw` が `0` です。そのまま掛け算すると `bdp = 0` となり、`snd_cwnd` が最小値 `4` パケットに張り付いて初期バーストが送れなくなるため、標準の初期ウィンドウ相当である `10U` パケットを返します。
- **切り上げ除算（`+ BW_UNIT - 1`）**: 整数除算の切り捨てによって `snd_cwnd` が真の BDP を下回ると、パイプを 100% 満たすだけのパケットを送り出せなくなり、それ以上の帯域サンプルも得られなくなるデッドロック（負のフィードバックループ）を防ぐためです。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) の各関数と固定小数点演算の流れを順に解説します。

### 1. スケール定数とソケット状態構造体 `struct bpf_bbr2_ca`

```c
#define BW_SCALE 24
#define BW_UNIT (1U << BW_SCALE)
#define BBR_SCALE 8
#define BBR_UNIT (1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET 4U
#define BBR_PACING_MARGIN_PCT 1U

struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};
```

- `BW_UNIT`（`1 << 24`）は帯域（ $\text{packets}/\mu\text{s}$ ）のスケール、`BBR_UNIT`（`1 << 8`）はゲインのスケールです。
- `struct bpf_bbr2_ca` は `inet_csk_ca(sk)` を通じて各ソケットの `icsk_ca_priv` 内に配置されます。

### 2. ペーシングレート変換ヘルパー `bbr_rate_bytes_per_sec()` と `bbr_set_pacing_rate()`

```c
static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}
```

- 引数 `rate` には `BW_UNIT` スケールの帯域（ $\text{packets}/\mu\text{s} \times 2^{24}$ ）が渡されます。
- これに `mss`（Bytes/packet）と `gain`（`>> BBR_SCALE`）、さらに 1 秒あたりのマイクロ秒数 `USEC_PER_SEC` から 1% のペーシングマージン（`BBR_PACING_MARGIN_PCT = 1U`）を差し引いた係数（ $990,000$ ）を掛け、最後に `>> BW_SCALE` でスケールを戻すことで、1 秒あたりのバイト数（`Bytes/sec`）に変換します。
- `bbr_set_pacing_rate()` はこの値を `sk->sk_max_pacing_rate` でクランプした上で `sk->sk_pacing_rate` に書き込みます。

### 3. BDP 計算ヘルパー `bbr_bdp()`

```c
static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}
```

- `ca->min_rtt_us` または `bw` が未計測（`0`）の間は `10U` を返し、両方揃っている場合は `w = (__u64)bw * ca->min_rtt_us` に `gain` を適用して `BW_UNIT` で切り上げ除算した BDP パケット数を返します。

### 4. 初期化 `bpf_bbr2_init()` と ACK ごとの更新 `bpf_bbr2_cong_control()`

```c
SEC("struct_ops")
void BPF_PROG(bpf_bbr2_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}
```

- コネクション初期化時に `ca` の各フィールドをゼロ初期化し、`ca->next_rtt_delivered = tp->delivered` で最初のラウンドの基準点を記録します。また、`bbr_init_pacing_rate_from_rtt()` で初期 `snd_cwnd` と `srtt_us` から最初のペーシングレートを設定します。

続いて、`bpf_bbr2_cong_control()` で ACK ごとにモデルと制御値を更新します。

```c
SEC("struct_ops")
void BPF_PROG(bpf_bbr2_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u32 bw, bdp;

	if (rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	if (rs->delivered > 0 && rs->interval_us > 0) {
		if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
			ca->round_cnt++;
			ca->next_rtt_delivered = tp->delivered;
		}

		bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);

		if (!rs->is_app_limited || bw >= ca->max_bw) {
			if (bw > ca->max_bw)
				ca->max_bw = bw;
		}
	}

	bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT);

	bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET);
}
```

- **TODO 1 対応箇所**: `rs->rtt_us > 0` のとき、初回または最小値更新時のみ `ca->min_rtt_us` を更新します。
- **TODO 2 対応箇所**: `rs->delivered > 0 && rs->interval_us > 0` のとき、ラウンド進行判定を行った上で固定小数点帯域 `bw` を算出し、`!rs->is_app_limited || bw >= ca->max_bw` のガードを通して `ca->max_bw` の最大値を保持します。
- 最後に `ca->max_bw` と `BBR_UNIT`（等倍ゲイン）から `sk_pacing_rate` と `bdp` を計算し、`tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET)` を設定します。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/test.pkt) は、高帯域バーストのあとに「App-Limited かつ RTT が膨張した低レートサンプル」を入力し、フィルタの有無による挙動の差を検証します。

### `test.pkt` のパケットトレース解説

1. **3-way ハンドシェイク（14〜17 行目、RTT = $10\text{ ms}$, ペイロード MSS = $1000\text{ Bytes}$ ）**:
   - 時刻 `0.00` に SYN / SYN-ACK を交換し、`+0.01`（ $10\text{ ms}$ 後）に ACK が到着してコネクションが確立されます。TCP タイムスタンプオプション（12 バイト）込みで `mss 1012` が指定されているため、ペイロード 1 セグメントあたりちょうど $1000\text{ Bytes}$ になります。
2. **Phase 1: 高帯域バーストと 1 回目のアサート（26〜33 行目）**:
   - `write(4, ..., 10000)` で 10 セグメント（`1:10001(10000)`）を送信し、`+0.01`（ $10\text{ ms}$ 後）にまとめて ACK（`ack 10001`）を受け取ります。
   - このサンプルは `rs->rtt_us = 10000`（ $10\text{ ms}$ ）、`rs->delivered = 10`、`rs->interval_us = 10000`（ $10\text{ ms}$ ）です。
   - `min_rtt_us = 10000`、`max_bw` は $1000\text{ pkts/s}$ （ $1,000,000\text{ Bytes/s}$ ）となり、 $\text{BDP} = 10$ パケット、`tcpi_snd_cwnd == 14`（ $10 + 4$ ）、`tcpi_pacing_rate` が約 $990\text{ KB/s}$ （`950000 < tcpi_pacing_rate < 1050000`）になることを検証します。
3. **Phase 2: App-Limited な小バースト＋膨張した RTT と 2 回目のアサート（39〜46 行目）**:
   - $30\text{ ms}$ のアイドル時間（`+0.03`）を空けたあと、`write(4, ..., 2000)` で 2 セグメント（ $2000\text{ Bytes}$ ）だけを送信し、`+0.02`（ $20\text{ ms}$ 後）に ACK（`ack 12001`）を受け取ります。
   - このサンプルでは `rs->rtt_us = 20000`（ $20\text{ ms}$ に倍増）、`rs->delivered = 2`、`rs->interval_us = 50000`（ $40\text{ pkts/s}$ に激減）、`rs->is_app_limited = true` となります。
   - **なぜ `starter.bpf.c` は失敗するのか**: [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c) は毎回 `ca->min_rtt_us = rs->rtt_us` と `ca->max_bw = bw` をそのまま上書きしてしまうため、Phase 2 の ACK で `max_bw` が $1/25$ （ $40\text{ pkts/s}$ ）に崩壊し、`tcpi_snd_cwnd` が `5`（ $\text{BDP } 1 + 4$ ）、`tcpi_pacing_rate` が約 $39.6\text{ KB/s}$ に落ちて 44〜45 行目のアサートに失敗します。
   - **なぜ `solution.bpf.c` はパスするのか**: [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) では最小値フィルタが `min_rtt_us = 10000` を守り、App-Limited ガード付き最大値フィルタが `max_bw = 1000 pkts/s` を守るため、Phase 2 の後も `tcpi_snd_cwnd == 14` と `tcpi_pacing_rate`（約 $990\text{ KB/s}$ ）が維持されてテストをパスします。

### テストの実行コマンド

以下のコマンドで、`starter` の失敗と `solution` の成功を確認できます。

```sh
../../run_test.sh bbr/step02-btlbw-rtprop starter
../../run_test.sh bbr/step02-btlbw-rtprop solution
```
