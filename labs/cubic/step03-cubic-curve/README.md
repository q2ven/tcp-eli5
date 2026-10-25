# Step 3: 固定小数点の立方根と W_cubic(t) 曲線（凹領域・凸領域）

## 学習ゴール

このステップでは、Linux カーネルのデフォルト輻輳制御アルゴリズムである **CUBIC**（[net/ipv4/tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c)）の中核となる 3 次関数ウィンドウ成長曲線 $W_{\text{cubic}}(t)$ を BPF `struct_ops`（`bpf_cubic3`）で実装します。

具体的には以下の 3 点を習得します。

- **乗法的減少（$\beta = 0.7$）**: パケットロス発生時に `snd_cwnd` を Reno の半分（$0.5 \times \text{cwnd}$）ではなく約 $0.7$ 倍（`717 / 1024`）に減速させ、ロス直前のウィンドウサイズを $W_{\max}$（`last_max_cwnd`）として記憶する方法
- **固定小数点の立方根 `cubic_root()`**: 浮動小数点演算が使えないカーネル / BPF 環境において、`fls64()`・64 エントリのルックアップテーブル `v[]`・1 回のニュートン＝ラフソン法反復を組み合わせて 64 ビット整数の立方根を誤差約 $0.195\%$ で高速に近似するアルゴリズムと、BPF 検証器（verifier）の配列境界チェックを通すためのガード
- **3 次関数曲線 $W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$**:
  - **凹領域（Concave region, $t < K$）**: $W_{\max}$ に向かって急速に回復し、変曲点に近づくにつれて増加ペースを緩める
  - **プラトー（Plateau, $t \approx K$）**: $W_{\max}$ 付近でウィンドウ成長をほぼ停止（`ca->cnt = 100 * cwnd`）してネットワークを安定させる
  - **凸領域（Convex region, $t > K$）**: 再び慎重に増加を始め、時間とともに加速しながら未知の帯域を探索（Probing）する

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/starter.bpf.c) や [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/solution.bpf.c) を開く前に、以下の 4 つの設計上の問いを自分の頭で考えてみましょう。

### Q1. 曲線の計算に必要な状態（State Struct）は何か？

$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$ を ACK ごとに評価し、`tcp_cong_avoid_ai()` に渡す `cnt`（何パケット ACK されたら `snd_cwnd` を $+1$ するか）を求めるには、ソケットごとのプライベート領域 `inet_csk_ca(sk)` にどのようなフィールドを保持する必要があるでしょうか？また、毎回 `cubic_root()` を再計算しないためには何をキャッシュすべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- ロス発生時の最大ウィンドウ $W_{\max}$ を記録する `last_max_cwnd` と、現在のエポックにおける曲線の原点 `bic_origin_point` が必要です。
- 輻輳回避エポックの開始時刻 `epoch_start` と、エポック開始時に 1 回だけ `cubic_root()` で計算した変曲点までの時間 `bic_K` をキャッシュしておけば、以降の ACK では立方根を再計算せずに $t - K$ の 3 乗だけで目標ウィンドウを計算できます。
- さらに、経過時間 $t$ に加算する最小 RTT `delay_min`、計算結果の増加インターバル `cnt`、および同一 `cwnd`・短時間での無駄な再計算を省くための `last_cwnd` と `last_time` を `struct bpf_cubic3_ca` に保持します。

</details>

### Q2. なぜ経過時間 `t` に `delay_min`（最小 RTT）を足すのか？

エポック開始時刻 `epoch_start` は「Fast Recovery を抜けて輻輳回避の最初の ACK を受け取った時刻」に記録されます。しかし実際のウィンドウ削減は 1 RTT 前のロス検知時に起きています。もし $t = \text{now} - \text{epoch\_start}$ のまま計算すると何が起きるでしょうか？

<details>
<summary>ヒントと答え</summary>

- `epoch_start` が記録される最初の輻輳回避 ACK の時点では、`now - epoch_start == 0` です。もし `delay_min` を足さないと、最初の RTT の目標ウィンドウが $W_{\text{cubic}}(0) = \text{cwnd}$ のままになり、`bic_target == cwnd` となって最初の 1 RTT の成長が止まってしまいます。
- $t = (\text{now} - \text{epoch\_start}) + \text{delay\_min}$ とすることで、「1 RTT 後（次のウィンドウ分が到着する時点）の目標ウィンドウサイズ」を先読みして `bic_target` を算出し、そこから逆算して現在の RTT で必要な増加ペース `cnt = cwnd / (bic_target - cwnd)` を正しく決定できます。

</details>

### Q3. 符号なし整数（`__u64`）だけで凹領域（$t < K$）と凸領域（$t > K$）を計算するには？

$(t - K)^3$ は $t < K$ のとき負、$t > K$ のとき正になります。カーネルや BPF でオーバーフローや符号拡張のバグを避けつつ、64 ビット符号なし整数 `__u64` で安全に計算するにはどう書けばよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- まず絶対値 $\text{offs} = |t - K|$ を分岐で求めます（`t < ca->bic_K ? ca->bic_K - t : t - ca->bic_K`）。
- 次に非負の差分 $\Delta = C \cdot \text{offs}^3$（`delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ)`）を計算します。
- 最後に、`t < ca->bic_K`（凹領域）なら `bic_target = ca->bic_origin_point - delta`、`t >= ca->bic_K`（凸領域）なら `bic_target = ca->bic_origin_point + delta` とすることで、すべて符号なし整数演算で綺麗に表現できます。

</details>

### Q4. BPF 検証器（Verifier）が `v[shift]` のテーブル参照を拒否するのはなぜか？

`cubic_root(__u64 a)` では、`b = fls64(a); b = ((b * 84) >> 8) - 1; shift = a >> (b * 3);` によって $a$ の上位 6 ビットを取り出し、64 要素の配列 `v[shift]` を参照します。数学的には `0 <= shift < 64` が保証されていますが、そのまま `v[shift]` と書くと BPF 検証器にロードを拒否されます。なぜでしょうか？どう修正すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- BPF 検証器は `fls64(a)` と変数シフト `a >> (b * 3)` の組み合わせから「`shift` が必ず 64 未満になる」という数学的不変条件を自動証明できません。そのため、64 バイトの `.rodata` マップ `v[]` に対する境界外アクセス（Out-of-Bounds access）の可能性があると判断してプログラムを拒否します。
- `v[shift]` にアクセスする直前に明示的な境界チェック `if (shift >= 64) return 0;` を入れることで、検証器に `shift` の上限（`[0, 63]`）を教え、安全性を証明させることができます。

</details>

---

## 実装ステップと解説

### 1. 固定小数点定数と単位系（`BICTCP_HZ`）

CUBIC のウィンドウ曲線は次の式で定義されます（$C = 0.4, \beta = 0.7$）。

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}, \quad K = \sqrt[3]{\frac{W_{\max} - \text{cwnd}}{C}}$$

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/solution.bpf.c) では、カーネルの [tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c) と同じ固定小数点スケールを使用します。

- `BICTCP_BETA_SCALE = 1024`、`beta = 717`: $\beta = 717 / 1024 \approx 0.7002$
- `BICTCP_HZ = 10`: 時間の単位を $1\text{ 秒} = 2^{10} = 1024$ 単位（約 $0.976\text{ ms}$ 刻み）で表します。
- `bic_scale = 41`、`cube_rtt_scale = bic_scale * 10 = 410`: $C = 410 / 1024 \approx 0.4004$。
- `cube_factor = (1ULL << (10 + 3 * BICTCP_HZ)) / (bic_scale * 10)`:
  $K$ を `BICTCP_HZ`（$2^{10}$）単位で求めるには、根号の中身に $(2^{10})^3 = 2^{30}$ を掛ける必要があります。さらに $C = 410 / 2^{10}$ で割るため、全体として $2^{40} / 410 = 2,681,735,677$ が `cube_factor` となります。

### 2. `fls64()` と `cubic_root()`

1. **ビット幅の特定**: `fls64(a)` で最上位の `1` ビット位置（$1 \sim 64$）を求め、`b = ((b * 84) >> 8) - 1` により $\lfloor (\text{fls64}(a) - 1) / 3 \rfloor$（3 ビットずつのブロック数、なぜなら $84 / 256 \approx 1/3$）を算出します。
2. **テーブル参照による初期推定値**: `shift = a >> (b * 3)` で上位 6 ビット（$0 \sim 63$）を取り出し、事前計算されたテーブル `v[shift]` から初期近似値 `x = (((v[shift] + 10) << b) >> 6)` を得ます。このとき BPF 検証器を通すため `if (shift >= 64) return 0;` を必ず挟みます。
3. **ニュートン＝ラフソン法の 1 回反復**:
   $f(x) = x^3 - a = 0$ に対する反復式 $x_{k+1} = \frac{1}{3}\left(2x_k + \frac{a}{x_k^2}\right)$ を整数演算 `x = (2 * x + div64_u64(a, x * (x - 1)))` と `* 341 >> 10`（$341 / 1024 \approx 1/3$）で計算します。

### 3. `bpf_cubic3_ssthresh()` — 乗法的減少（$\beta \approx 0.7$）

パケットロス検知時に呼ばれる `.ssthresh` フック `bpf_cubic3_ssthresh()` では、以下を行います。

- `ca->epoch_start = 0;` にリセットし、Recovery 終了後の最初の輻輳回避 ACK で新しい CUBIC エポックが開始されるようにします。
- ロス発生時の `tp->snd_cwnd` を `ca->last_max_cwnd`（$W_{\max}$）に保存します（Fast Convergence は Step 4 で実装します）。
- 新しい `snd_ssthresh` として `max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U)` を返します。たとえば `cwnd = 20` のとき、`20 * 717 / 1024 = 14` となります（Reno の `10` より緩やかな減少）。

### 4. `bictcp_update()` — 凹領域・プラトー・凸領域の目標計算

輻輳回避フェーズで呼ばれる `bictcp_update()` の流れは以下の通りです。

1. **エポック初期化（`ca->epoch_start == 0`）**:
   - `ca->epoch_start = now_us;` を記録します。
   - `ca->last_max_cwnd <= cwnd` なら既に過去の最大ウィンドウを超えているため `ca->bic_K = 0`、`ca->bic_origin_point = cwnd` とします。
   - `ca->last_max_cwnd > cwnd` なら、`ca->bic_K = cubic_root(cube_factor * (ca->last_max_cwnd - cwnd))` で $W_{\max}$ に到達するまでの時間 $K$ を計算し、`ca->bic_origin_point = ca->last_max_cwnd` とします。
2. **経過時間 $t$ と三次項 $\Delta$ の計算**:
   - マイクロ秒単位の経過時間 `(now_us - ca->epoch_start) + ca->delay_min` を `<< BICTCP_HZ` して `USEC_PER_SEC` で割り、`BICTCP_HZ` 単位の `t` に変換します。
   - `offs = |t - ca->bic_K|` から `delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ)` を求めます。
   - `t < ca->bic_K`（**凹領域**）なら `bic_target = ca->bic_origin_point - delta`、`t >= ca->bic_K`（**凸領域**）なら `bic_target = ca->bic_origin_point + delta` とします。
3. **`ca->cnt` への変換**:
   - `bic_target > cwnd` のときは、1 RTT（`cwnd` 個の ACK）あたり `bic_target - cwnd` だけ増やすため、`ca->cnt = cwnd / (bic_target - cwnd)` とします。
   - `bic_target <= cwnd`（$W_{\max}$ のプラトー到達時）は、`ca->cnt = 100 * cwnd` として成長をほぼ止めます。
   - 最後に `ca->cnt = max(ca->cnt, 2U)` で上限成長率をクランプし、`bpf_cubic3_cong_avoid_ai()` に渡します。

---

## packetdrill テストの解説

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/test.pkt) では、RTT = 100ms（`delay_min = 100,000` usec）のコネクション上で、CUBIC の 3 つの領域すべてを順番に検証します。

1. **Slow Start（`cwnd: 10 -> 20`）**:
   初期ウィンドウ 10 セグメント（10,000 バイト）を送信し、ACK により `tcpi_snd_cwnd == 20` に到達するとともに、`bpf_cubic3_acked()` で `delay_min = 100ms` が記録されます。
2. **パケットロスと $\beta = 0.7$ の検証**:
   `cwnd = 20` で 4 セグメント送信し、先頭セグメントのロスと 3 個の SACK により Fast Recovery に遷移します。このとき `bpf_cubic3_ssthresh()` が呼ばれ、`tcpi_snd_ssthresh == 14`（`20 * 717 / 1024 = 14`）および `last_max_cwnd = 20` になることをアサートします（[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/starter.bpf.c) は Reno の `10` を返すためここで失敗します）。
3. **Flight 1（エポック開始・凹領域の立ち上がり, $t_{\text{elapsed}} = 0.0\text{ s}$）**:
   Recovery 完了後、`cwnd = 14` から開始します。`bic_K = cubic_root(cube_factor * 6) = 2523`（約 $2.46\text{ s}$）、`t = 102`、`delta = 5` より `bic_target = 20 - 5 = 15`、`ca->cnt = 14 / (15 - 14) = 14` となり、14 パケットの ACK で `tcpi_snd_cwnd == 15` に増えます。
4. **Flight 2（凹領域の急成長, $t_{\text{elapsed}} = 0.6\text{ s}$）**:
   `delta = 2`、`bic_target = 20 - 2 = 18` となり、`ca->cnt = 15 / (18 - 15) = 5` に短縮されます。15 パケットの ACK により、1 RTT で一気に `15 / 5 = +3` 増加し、`tcpi_snd_cwnd == 18` になります。
5. **Flight 3（原点 $W_{\max} = 20$ への到達, $t_{\text{elapsed}} = 1.8\text{ s}$）**:
   変曲点 $K \approx 2.46\text{ s}$ に近づき `delta = 0`、`bic_target = 20` となります。`ca->cnt = 18 / (20 - 18) = 9` により、18 パケットの ACK で `tcpi_snd_cwnd == 20`（$W_{\max}$）に到達します。
6. **Flight 4（変曲点 $t \approx K$ のプラトー検証, $t_{\text{elapsed}} = 2.1\text{ s}$）**:
   `cwnd = 20` かつ `bic_target = 20` となるため、`bic_target <= cwnd` の分岐に入り `ca->cnt = 100 * cwnd = 2000` が設定されます。20 パケットすべてが ACK されても `tcpi_snd_cwnd == 20` のまま水平に保たれることを確認します。
7. **Flight 5（凸領域 $t > K$ での帯域探索, $t_{\text{elapsed}} = 4.0\text{ s}$）**:
   時間が $K$ を大きく超えて `t > bic_K` かつ `delta = 1` となり、凸領域の `bic_target = 20 + 1 = 21 > cwnd` に入ります。20 パケットの ACK によって `tcpi_snd_cwnd > 20` へ再び増加し始めることを確認します。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh cubic/step03-cubic-curve starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh cubic/step03-cubic-curve solution
```
