# Step 1: 固定小数点の立方根と W_cubic(t) 曲線（凹領域・プラトー・凸領域）

## 1. 学習ゴール

このステップでは、Linux カーネルのデフォルト輻輳制御アルゴリズムである **TCP CUBIC**（[net/ipv4/tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c)）の心臓部である **乗法的減少（$$\beta \approx 0.7$$）**、**固定小数点の立方根 `cubic_root()`**、および **3 次関数ウィンドウ成長曲線 $$W_{\text{cubic}}(t)$$** を BPF `struct_ops`（`bpf_cubic3`）としてゼロから実装します。トラック全体の構成や背景については [CUBIC トラック概要 (`../README.md`)](../README.md) もあわせて参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

設計チェックポイントやコードに進む前に、**「なぜ Reno では高帯域ネットワークを使い切れないのか」「CUBIC はどのような数式と固定小数点演算で動くのか」「カーネルの `struct tcp_sock` とソケット固有領域 `icsk_ca_priv` に何が用意されているのか」** を一から整理します。

### 2.1 なぜ TCP Reno では不十分なのか？（Long Fat Network の問題）

TCP Reno（AIMD）は、パケットロス発生時に `snd_cwnd` を半分（$$0.5$$ 倍）に減らし、その後は **1 RTT あたり $$+1$$ セグメント** ずつ直線的にウィンドウを回復させます。

しかし、帯域幅が大きく遅延も長いネットワーク（**LFN: Long Fat Network**）では、この「1 RTT に $$+1$$」というペースが致命的なボトルネックになります。
- たとえば **帯域 10 Gbps・RTT 100 ms** の回線では、パイプを満たすのに必要なウィンドウサイズ（BDP）は約 $$83,333$$ セグメントです。
- たった 1 パケットのロスでウィンドウが半分の $$41,666$$ に落ちると、元の $$83,333$$ に戻るまでに **41,666 RTT ＝ 約 69 分** もかかってしまいます。
- また、Reno は「1 RTT ごとに $$+1$$」増えるため、RTT が短いフローほど急速にウィンドウを広げ、RTT が長いフローが帯域を奪われる **RTT 不公平性（RTT unfairness）** という弱点も抱えています。

### 2.2 CUBIC の基本アイデア：実時間 $$t$$ の 3 次関数で $$W_{\max}$$ を目指す

CUBIC は、Reno の問題を次の 2 つのアイデアで解決します。

1. **緩やかな乗法的減少（$$\beta \approx 0.7$$）と $$W_{\max}$$ の記憶**:
   パケットロスが発生したとき、ロス直前の `snd_cwnd` を **「前回ネットワークが飽和した上限の目安 $$W_{\max}$$（`last_max_cwnd`）」** として記録し、ウィンドウを Reno の半分（$$0.5$$ 倍）ではなく **約 $$0.7$$ 倍**（$$\beta = 717 / 1024 \approx 0.7002$$）にだけ縮小します。
2. **ACK 数ではなく「経過時間 $$t$$」に基づく 3 次関数 $$W_{\text{cubic}}(t)$$**:
   Fast Recovery を抜けて輻輳回避（Congestion Avoidance）エポックが始まってからの経過時間を $$t$$ とし、変曲点（傾きが $$0$$ になる平坦な点）を $$(t = K, W = W_{\max})$$ に置いた **3 次関数** で目標ウィンドウを定めます。

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$$

ここで $$C = 0.4$$ はスケーリング定数、$$K$$ は「エポック開始時点（$$t = 0$$）のウィンドウサイズ $$\text{cwnd}_{\text{epoch}}$$ から、原点 $$W_{\max}$$ に到達するまでにかかる時間」です。$$W_{\text{cubic}}(0) = \text{cwnd}_{\text{epoch}}$$ を代入すると、$$K$$ は次の立方根（3 乗根）で求まります。

$$K = \sqrt[3]{\frac{W_{\max} - \text{cwnd}_{\text{epoch}}}{C}}$$

```text
  cwnd
   ^
   |                                            /  <-- (3) 凸領域 (Convex: t > K)
   |                                          /        新しい空き帯域を慎重に探索し加速
W_max (20) +.................................*.........
   |                            . '   ' .
   |                        . '          <-- (2) プラトー (Plateau: t ~= K)
   |                      /                  W_max 付近で傾きをほぼ 0 にしロス再発を防ぐ
   |                    /
   |                   /  <-- (1) 凹領域 (Concave: t < K)
cwnd_epoch (14) +     *       最初は一気に駆け上がり、W_max に近づくほど減速
   |
   +------------------+----------------------+---------> 経過時間 t
                    t = 0                  t = K
                 (エポック開始)           (変曲点)
```

この 3 次曲線により、CUBIC は以下の 3 つの領域を 1 本の式で表現します。
- **(1) 凹領域（Concave region, $$t < K$$）**: $$t - K < 0$$ のため $$W_{\text{cubic}}(t) = W_{\max} - C(K - t)^3$$ となります。エポック開始直後は $$|t - K|$$ が大きいため急勾配で $$W_{\max}$$ に向かって一気に回復し、$$t$$ が $$K$$ に近づくにつれて増加ペースを緩めます。
- **(2) プラトー（Plateau, $$t \approx K$$）**: 前回ロスが起きた $$W_{\max}$$ に到達すると、$$|t - K| \approx 0$$ となって 3 次関数の傾きがほぼ $$0$$ になります。ここでしばらく水平に留まることで、リンク容量ギリギリを安全に維持します。
- **(3) 凸領域（Convex / Max-Probing region, $$t > K$$）**: $$W_{\max}$$ でロスが起きなければ「ネットワークの空き帯域が増えた」と判断し、$$W_{\text{cubic}}(t) = W_{\max} + C(t - K)^3$$ で最初はゆっくり、徐々に加速しながら未知の帯域を探索（Probing）します。

### 2.3 「1 RTT 先の目標ウィンドウ」から増加インターバル `ca->cnt` への変換

カーネルの輻輳回避処理は、「`ca->cnt` 個のセグメントが ACK されるごとに `snd_cwnd` を $$+1$$ する」という汎用カウンタ関数 `bpf_cubic3_cong_avoid_ai(tp, ca->cnt, acked)` で動いています。では、連続関数の $$W_{\text{cubic}}(t)$$ をどのように `ca->cnt` に変換するのでしょうか？

1. **1 RTT 先の時刻 $$t = (\text{now} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}$$ を評価する**:
   エポック開始時刻 `epoch_start` は、輻輳回避の最初の ACK を受け取った瞬間に `now_us` で初期化されます。もし $$t = \text{now} - \text{epoch}_{\text{start}}$$ のまま計算すると、最初の ACK 時点で $$t = 0$$ となり、$$W_{\text{cubic}}(0) = \text{cwnd}$$ となって目標ウィンドウが現在の `cwnd` から全く増えません。
   そこで CUBIC は、観測された最小 RTT（`delay_min`）を足した **$$t = (\text{now} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}$$**（「今から 1 RTT 後に到達していたい目標時刻」）における目標ウィンドウ `bic_target` $$= W_{\text{cubic}}(t)$$ を計算します。
2. **`bic_target` から `ca->cnt` を逆算する**:
   現在のウィンドウが `cwnd` のとき、これからの 1 RTT で到着する ACK の総数は約 `cwnd` 個です。1 RTT 後にウィンドウを `bic_target` まで増やす（つまり 1 RTT の間に `bic_target - cwnd` 個だけ `+1` する）には、何個の ACK ごとに `+1` すればよいでしょうか？
   - **`bic_target > cwnd` のとき**:
     $$\text{cnt} = \left\lfloor \frac{\text{cwnd}}{\text{bic}_{\text{target}} - \text{cwnd}} \right\rfloor$$
   - **`bic_target <= cwnd` のとき（$$t \approx K$$ のプラトー）**:
     分母が $$0$$ 以下になるため、非常にゆっくりとした増加（100 RTT で $$+1$$）を表す `ca->cnt = 100 * cwnd` を設定します。
   - **過去のロス履歴がない初期エポック（`last_max_cwnd == 0` かつ `ca->cnt > 20`）**:
     まだ一度もロスを経験していない場合は `ca->cnt = 20` で頭打ちにし、最後に `ca->cnt = max(ca->cnt, 2U)` で最小値 `2`（2 ACK ごとに最大 $$+1$$）にクランプします。

### 2.4 カーネル／BPF での固定小数点演算と整数立方根 `cubic_root()`

Linux カーネルや BPF プログラムでは浮動小数点（`float` / `double`）が使えないため、すべての小数を **2 のべき乗（1024）を分母とする固定小数点整数** で表現します。

| 定数・変数 | 値 / 定義 | 数学的な意味 |
| :--- | :--- | :--- |
| `BICTCP_BETA_SCALE` | `1024` | $$\beta$$ の分母（$$2^{10}$$） |
| `beta` | `717` | 乗法的減少係数 $$\beta = 717 / 1024 \approx 0.7002$$ |
| `BICTCP_HZ` | `10` | 時間 $$t, K$$ の単位スケール：$$1\text{ 秒} = 2^{10} = 1024\text{ 単位}$$（約 $$0.976\text{ ms}$$ 刻み） |
| `bic_scale` | `41` | スケーリング基準値（`cube_rtt_scale = bic_scale * 10 = 410`） |
| `cube_rtt_scale` | `410` | $$C \times 1024 = 0.4 \times 1024 \approx 410$$（すなわち $$C = 410 / 2^{10}$$） |
| `cube_factor` | `(1ULL << 40) / 410` ($$= 2,681,735,677$$) | $$K$$ を `BICTCP_HZ`（$$2^{10}$$）単位で求めるための係数：$$\frac{(2^{10})^3}{C} = \frac{2^{30}}{410 / 2^{10}} = \frac{2^{40}}{410}$$ |

この単位系を使うと、各計算はすべて整数シフトと乗除算だけで書けます。
- **変曲点までの時間 `bic_K`（単位：`BICTCP_HZ`）**:
  $$K_{\text{hz}} = \sqrt[3]{\text{cube}_{\text{factor}} \times (W_{\max} - \text{cwnd})}$$
- **経過時間 `t`（マイクロ秒 $$\to$$ `BICTCP_HZ` 単位）**:
  $$\text{t} = \frac{((\text{now}_{\text{us}} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}) \ll 10}{1,000,000}$$
- **原点からの差分 `delta` $$= C |t - K|^3$$**:
  符号なし整数 `__u64` で安全に計算するため、まず絶対値 $$\text{offs} = |t - \text{bic}_{K}|$$ を取り、
  $$\text{delta} = (\text{cube}_{\text{rtt,scale}} \times \text{offs}^3) \gg (10 + 3 \times 10)$$
  （$$C = 410 / 2^{10}$$ と $$\text{offs}^3$$ の時間スケール $$(2^{10})^3 = 2^{30}$$ の合計 $$40$$ ビット分を右シフトで戻します。）
  $$t < \text{bic}_{K}$$（凹領域）なら `bic_target = bic_origin_point - delta`、$$t \ge \text{bic}_{K}$$（凸領域）なら `bic_target = bic_origin_point + delta` となります。

#### 固定小数点の立方根 `cubic_root(__u64 a)` の仕組み

整数 $$a$$ の立方根 $$\lfloor \sqrt[3]{a} \rfloor$$ を高速に求めるため、[tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c) と同じ 3 ステップのアルゴリズムを使います。
1. **ビット幅を 3 で割る（指数部の計算）**:
   `fls64(a)` で $$a$$ の最上位セットビット位置（$$1 \sim 64$$）を求め、`b = ((fls64(a) * 84) >> 8) - 1` を計算します。$$84 / 256 \approx 1/3$$ なので、これにより $$a$$ を「3 ビットずつのブロック何個分か」を表すシフト量 `b` が求まります（$$a < 64$$ の小さい値はテーブルから直接 `((__u32)v[(__u32)a] + 35) >> 6` を返します）。
2. **64 要素のルックアップテーブル `v[]` による初期推定値**:
   `shift = a >> (b * 3)` によって $$a$$ の上位 6 ビット（$$0 \sim 63$$）を取り出し、事前計算された配列 `v[shift]` を引いて `x = (((__u32)v[shift] + 10) << b) >> 6` で初期近似値 $$x_0 \approx \sqrt[3]{a}$$ を作ります。
   （※ **BPF Verifier 上の重要ルール**: 数学的には `shift < 64` ですが、BPF 検証器は変数シフト `a >> (b * 3)` の上限を自動証明できないため、`v[shift]` の直前に必ず `if (shift >= 64) return 0;` を入れて配列境界内であることを検証器に保証します。）
3. **ニュートン＝ラフソン法（Newton-Raphson）の 1 回反復**:
   方程式 $$f(x) = x^3 - a = 0$$ の解を 1 回反復式 $$x_{1} = \frac{1}{3}\left(2x_0 + \frac{a}{x_0^2}\right)$$ で精密化します。整数演算では `x * (x - 1)` で分母を近似し、$$1/3 \approx 341 / 1024$$ を使って次のように書きます（平均誤差わずか約 $$0.195\%$$）：
   ```c
   x = (2 * x + (__u32)div64_u64(a, (__u64)x * (__u64)(x - 1)));
   x = ((x * 341) >> 10);
   ```

### 2.5 具体的な数値例（`test.pkt` のパラメータでの追跡）

RTT = $$100\text{ ms}$$（`delay_min = 100,000` usec）の接続で、`cwnd = 20` のときにパケットロスが発生したケースを実際に計算してみましょう。

1. **ロス発生時（`bpf_cubic3_ssthresh()`）**:
   - $$W_{\max}$$ として `last_max_cwnd = 20` を保存し、`epoch_start = 0` にリセットします。
   - 新しい `ssthresh`（＝回復後の初期 `cwnd`）は $$\lfloor 20 \times 717 / 1024 \rfloor = \lfloor 14.003 \rfloor = 14$$ になります。
2. **エポック開始時（Recovery 完了直後の Flight 1, $$\text{cwnd} = 14$$）**:
   - `bic_origin_point = 20`
   - `bic_K = cubic_root(2,681,735,677 * (20 - 14)) = cubic_root(16,090,414,062) = 2523`（秒に直すと $$2523 / 1024 \approx 2.46\text{ 秒}$$）
3. **経過時間ごとの `bic_target` と `ca->cnt` の推移**:

| フライト | 経過時間 $$\text{now} - \text{epoch}_{\text{start}}$$ | 先読み時刻 $$t$$ (`BICTCP_HZ`) | $$\text{offs} = \|t - 2523\|$$ | $$\text{delta} = C \cdot \text{offs}^3$$ | 領域と `bic_target` | `ca->cnt` | 1 RTT 後の `cwnd` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Flight 1** | $$0.0\text{ s}$$ | $$102$$ ($$0.1\text{ s}$$) | $$2421$$ | $$5$$ | 凹領域：$$20 - 5 = 15$$ | $$14 / (15 - 14) = 14$$ | $$14 \to 15$$ |
| **Flight 2** | $$0.6\text{ s}$$ | $$716$$ ($$0.7\text{ s}$$) | $$1807$$ | $$2$$ | 凹領域：$$20 - 2 = 18$$ | $$15 / (18 - 15) = 5$$ | $$15 \to 18$$（$$+3$$ 急伸） |
| **Flight 3** | $$1.8\text{ s}$$ | $$1945$$ ($$1.9\text{ s}$$) | $$578$$ | $$0$$ | 原点到達：$$20 - 0 = 20$$ | $$18 / (20 - 18) = 9$$ | $$18 \to 20$$（$$W_{\max}$$） |
| **Flight 4** | $$2.1\text{ s}$$ | $$2252$$ ($$2.2\text{ s}$$) | $$271$$ | $$0$$ | プラトー：$$20 \le \text{cwnd}$$ | $$100 \times 20 = 2000$$ | $$20 \to 20$$（水平維持） |
| **Flight 5** | $$4.0\text{ s}$$ | $$4198$$ ($$4.1\text{ s}$$) | $$1675$$ | $$1$$ | 凸領域：$$20 + 1 = 21$$ | $$20 / (21 - 20) = 20$$ | $$20 \to 21$$（再加速） |

### 2.6 カーネルが提供するフィールドとソケット固有領域 `icsk_ca_priv`

BPF で CUBIC を実装するにあたり、**カーネル側ですでに用意されている構造体フィールド**と、**私たちが `icsk_ca_priv`（`struct bpf_cubic3_ca`）に独自定義して管理するフィールド**は以下の通りです。

1. **`struct tcp_sock *tp = tcp_sk(sk)`（カーネルが管理する TCP 状態）**:
   - `tp->snd_cwnd`: 現在の輻輳ウィンドウ（セグメント単位）
   - `tp->snd_ssthresh`: スロースタート閾値（`tcp_in_slow_start(tp)` は `tp->snd_cwnd < tp->snd_ssthresh` を判定）
   - `tp->snd_cwnd_cnt`: `bpf_cubic3_cong_avoid_ai()` が使う線形増加カウンタ（ACK されたセグメント数を蓄積し、`w` に達したら `snd_cwnd` を増やす）
   - `tp->snd_cwnd_clamp`: `snd_cwnd` の上限値
   - `tp->prior_cwnd`: ロス回復に入る前の `snd_cwnd`（誤検知時の `.undo_cwnd` で使用）
   - `tp->tcp_mstamp`: パケット送受信時に更新されるマイクロ秒単位の単調増加タイムスタンプ（`bictcp_clock_us(sk)` で取得）
2. **`const struct ack_sample *sample`（`.pkts_acked` フックの引数）**:
   - `sample->rtt_us`: 今回の ACK で計測された RTT（マイクロ秒単位。再送等で計測不能な場合は負の値 `< 0`）
3. **`inet_csk_ca(sk)` / `icsk_ca_priv`（私たちが定義する `struct bpf_cubic3_ca`）**:
   - `struct inet_connection_sock` の末尾には、各輻輳制御アルゴリズムが自由に使えるプライベートメモリ領域 `icsk_ca_priv` が用意されており、`inet_csk_ca(sk)` ヘルパーでポインタを取得できます。
   - [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c) には、すでに以下の `struct bpf_cubic3_ca` が定義されています：
     - `cnt`: 計算された増加インターバル（何パケット ACK されたら `snd_cwnd` を $$+1$$ するか）
     - `last_max_cwnd`: 前回のロス発生時のウィンドウサイズ $$W_{\max}$$
     - `last_cwnd`, `last_time`: 短時間（1/32 秒以内）に同じ `cwnd` で `bictcp_update()` が重複実行されるのを防ぐキャッシュ
     - `bic_origin_point`: 現在のエポックにおける曲線の原点（通常は `last_max_cwnd`）
     - `bic_K`: エポック開始から原点 `bic_origin_point` に到達するまでの時間 $$K$$（`BICTCP_HZ` 単位）
     - `delay_min`: `.pkts_acked` フックで記録した最小 RTT（マイクロ秒単位）
     - `epoch_start`: 現在の輻輳回避エポックの開始時刻（マイクロ秒単位。`0` のときはエポック未開始を表す）

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の仕組みを踏まえて、[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c) の 3 つの `TODO` を書く前に以下の 4 つの設計の問いを考えてみましょう。

### Q1. なぜ `bic_K` は ACK のたびではなく `ca->epoch_start == 0` のときだけ計算するのか？

`bictcp_update()` は輻輳回避中に ACK が届くたびに呼ばれますが、`cubic_root()` を使った `ca->bic_K` の計算は `if (ca->epoch_start == 0)` のブロック内で 1 回しか行いません。なぜ毎回計算しなくてよいのでしょうか？また、`ca->epoch_start` はどのタイミングで `0` にリセットされるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.2 の式 $$K = \sqrt[3]{(W_{\max} - \text{cwnd}_{\text{epoch}}) / C}$$ に含まれる変数が、同一エポック内で変化するかどうかを考えてみましょう。
- **答え**:
  - 変曲点までの時間 $$K$$ は、「エポック開始時点の初期ウィンドウ $$\text{cwnd}_{\text{epoch}}$$」と「目標の原点 $$W_{\max}$$（`last_max_cwnd`）」の差だけで決まる定数です。同じ輻輳回避エポックが続いている間は曲線自体が変わらないため、エポック開始時（`ca->epoch_start == 0`）に 1 回だけ `cubic_root()` を計算して `ca->bic_K` と `ca->bic_origin_point` にキャッシュしておけば、以降の ACK では引き算と 3 乗算だけで $$W_{\text{cubic}}(t)$$ を高速に評価できます。
  - 新しいエポックへのリセットは、パケットロス検知時（`bpf_cubic3_ssthresh()` で `ca->epoch_start = 0;`）および RTO タイムアウト時（`bpf_cubic3_set_state()` の `TCP_CA_Loss` で `bictcp_reset(ca)`）に行われます。また、もしエポック開始時にすでに `ca->last_max_cwnd <= cwnd` であれば、減速後のウィンドウよりも過去の $$W_{\max}$$ の方が小さい（または未設定 `0`）ため、`ca->bic_K = 0`、`ca->bic_origin_point = cwnd` として即座に凸領域（探索）からスタートします。

</details>

### Q2. なぜ経過時間 `t` を計算するときに `ca->delay_min`（最小 RTT）を足すのか？

`bictcp_update()` では、経過時間 `t` を `now_us - ca->epoch_start` ではなく `(now_us - ca->epoch_start) + ca->delay_min` として計算します。もし `ca->delay_min` を足さなかった場合、エポック開始直後の Flight 1（`now_us == ca->epoch_start`）で `bic_target` と `ca->cnt` はどうなってしまうでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.3 とセクション 2.5 の数値例で、$$t = 0$$ のときの $$W_{\text{cubic}}(0)$$ の値を思い出してみましょう。
- **答え**:
  - エポックの最初の ACK では `ca->epoch_start = now_us` が代入されるため、`now_us - ca->epoch_start == 0` です。もし `delay_min` を足さないと $$t = 0$$ となり、定義により $$W_{\text{cubic}}(0) = \text{cwnd}$$ となって `bic_target == cwnd` になります。
  - すると `bic_target <= cwnd` のプラトー分岐に入ってしまい、最初の 1 RTT の間 `ca->cnt = 100 * cwnd` となってウィンドウ成長が止まってしまいます。
  - `ca->delay_min`（1 RTT）を加算することで、「1 RTT 後に達成すべき目標ウィンドウ `bic_target`」を先読みし、現在の RTT 中に到着する `cwnd` 個の ACK でちょうど `bic_target` に到達するようなペース `ca->cnt = cwnd / (bic_target - cwnd)` を正しく算出できます。

</details>

### Q3. 符号なし整数（`__u64`）だけで凹領域（$$t < K$$）と凸領域（$$t \ge K$$）の両方を安全に計算するには？

数式 $$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$$ では、$$t < K$$ のとき $$(t - K)^3$$ が負になり、$$t > K$$ のとき正になります。`t` と `ca->bic_K` が符号なし整数であるとき、アンダーフローを起こさずに `bic_target` を求めるにはどう条件分岐を組めばよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.4 の通り、先に絶対値 $$\text{offs} = |t - K|$$ を求めてから 3 乗の差分 `delta` を作り、最後に原点から引くか足すかを切り替えます。
- **答え**:
  1. まず `t < ca->bic_K` かどうかで分岐し、常に非負の距離 `offs` を計算します：
     ```c
     if (t < ca->bic_K)
         offs = ca->bic_K - t;
     else
         offs = t - ca->bic_K;
     ```
  2. 次に非負の 3 次項 `delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ);` を計算します。
  3. 最後に、凹領域（`t < ca->bic_K`）なら `bic_target = ca->bic_origin_point - delta;`、凸領域（`t >= ca->bic_K`）なら `bic_target = ca->bic_origin_point + delta;` とします。これにより符号付きオーバーフローや負数の右シフト問題を完全に回避できます。

</details>

### Q4. `cubic_root()` で `v[shift]` を参照する直前に `if (shift >= 64) return 0;` が必要な理由は？

`cubic_root(__u64 a)` では、`b = fls64(a); b = ((b * 84) >> 8) - 1; shift = (a >> (b * 3));` によって $$a$$ の上位 6 ビットを抽出します。アルゴリズム上は `shift` は必ず `0` 以上 `63` 以下になりますが、境界チェックを書かずに `v[shift]` にアクセスすると BPF プログラムのロードが失敗します。なぜでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.4 の BPF Verifier のルールを思い出してください。検証器は `fls64(a)` の戻り値と変数シフト `a >> (b * 3)` の関係を代数的に証明できるでしょうか？
- **答え**:
  - BPF 検証器（verifier）はレジスタごとの値の範囲（min/max bounds）を追跡しますが、`fls64(a)` のビット探索ロジックと `a >> (b * 3)` の組み合わせから「結果の `shift` が必ず 64 未満になる」という数学的性質までは推論できません。
  - そのため、要素数 64 の静的配列 `v[]`（`.rodata` マップ）に対して境界外読み取り（Out-of-Bounds access）の恐れがあると判定し、ロードを拒否します。
  - `if (shift >= 64) return 0;` を直前に置くことで、その後のパスにおける `shift` の上限が `63` 以下であることが検証器に明示され、安全にロードできるようになります。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/solution.bpf.c) の各関数と固定小数点式の実装を順番に見ていきましょう。

### 4.1 `cubic_root(__u64 a)` — テーブル参照とニュートン法による整数立方根（TODO 1）

```c
static __always_inline __u32 cubic_root(__u64 a)
{
	__u32 x, b, shift;

	if (a < 64)
		return ((__u32)v[(__u32)a] + 35) >> 6;

	b = fls64(a);
	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	/* Bounds check required by the BPF verifier for v[shift] */
	if (shift >= 64)
		return 0;

	x = ((__u32)(((__u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration:
	 *   x_{k+1} = (2 * x_k + a / x_k^2) / 3
	 * Note: 341 / 1024 ~= 1 / 3.
	 */
	x = (2 * x + (__u32)div64_u64(a, (__u64)x * (__u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}
```

- **$$a < 64$$ の高速パス**: $$a$$ が 6 ビット以内に収まる場合はシフトが不要なため、テーブル `v[a]` から直接丸め付きの値を返します。
- **指数ブロック `b` と上位 6 ビット `shift` の抽出**: `fls64(a)` から `b = ((b * 84) >> 8) - 1` を求め、`shift = a >> (b * 3)` で $$a$$ を `[0, 63]` に正規化します。
- **Verifier ガードと初期推定値**: `if (shift >= 64) return 0;` で BPF 検証器に境界を保証したうえで、`v[shift]` を参照し `<< b` で元のスケールに戻して初期近似値 `x` を得ます。
- **ニュートン＝ラフソン反復**: `div64_u64(a, (__u64)x * (__u64)(x - 1))` で $$a / x^2$$ を計算し、`2 * x` と足して `* 341 >> 10`（$$\approx \div 3$$）することで、1 回の反復で高精度な立方根に収束させます。

### 4.2 `bictcp_update()` — $$W_{\text{cubic}}(t)$$ 曲線と `ca->cnt` の算出（TODO 2）

```c
static __always_inline void bictcp_update(struct sock *sk, __u32 cwnd, __u32 acked)
{
	struct bpf_cubic3_ca *ca = inet_csk_ca(sk);
	__u32 now_us = bictcp_clock_us(sk);
	__u32 delta, bic_target;
	__u64 offs, t;

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto out;

	ca->last_cwnd = cwnd;
	ca->last_time = now_us;

	if (ca->epoch_start == 0) {
		ca->epoch_start = now_us;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K = cubic_root(cube_factor *
					       (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	t = (__u64)((__s32)(now_us - ca->epoch_start) + ca->delay_min);
	t <<= BICTCP_HZ;
	t /= USEC_PER_SEC;

	if (t < ca->bic_K)
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* delta = C * |t - K|^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ);
	if (t < ca->bic_K)	/* Concave region: below origin W_max */
		bic_target = ca->bic_origin_point - delta;
	else			/* Convex region: above origin W_max */
		bic_target = ca->bic_origin_point + delta;

	/* Convert target window into ACK count interval ca->cnt */
	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = 100 * cwnd;	/* Very small increment at plateau */

	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;

out:
	ca->cnt = max(ca->cnt, 2U);
}
```

- **レート制限ガード**: 同じ `cwnd` のまま $$1/32\text{ 秒}$$（$$31.25\text{ ms}$$）以内に到着した ACK では再計算をスキップし、CPU 負荷を抑えます。
- **エポック初期化**: `ca->epoch_start == 0` のとき、`ca->epoch_start = now_us` を記録し、`ca->last_max_cwnd > cwnd` であれば `cubic_root(cube_factor * (ca->last_max_cwnd - cwnd))` で変曲点までの時間 `ca->bic_K` を求めます。
- **3 次曲線の評価**: `(now_us - ca->epoch_start) + ca->delay_min` を `BICTCP_HZ` 単位の `t` に変換し、`offs = |t - ca->bic_K|` から `delta` を計算して凹領域（`bic_origin_point - delta`）または凸領域（`bic_origin_point + delta`）の `bic_target` を得ます。
- **`ca->cnt` の決定**: `bic_target > cwnd` なら `cwnd / (bic_target - cwnd)`、プラトー（`bic_target <= cwnd`）なら `100 * cwnd` とし、最後に `max(ca->cnt, 2U)` で下限を保証します。

### 4.3 `bpf_cubic3_ssthresh()` — $$\beta \approx 0.7$$ の乗法的減少（TODO 3）

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_cubic3_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_cubic3_ca *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;
	ca->last_max_cwnd = tp->snd_cwnd;

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}
```

- パケットロス検知時に `ca->epoch_start = 0` にリセットし、現在の `tp->snd_cwnd` を `ca->last_max_cwnd`（$$W_{\max}$$）として記憶します（Fast Convergence と Reno-friendly 領域は次の Step 2 で追加します）。
- 新しい `snd_ssthresh` として `(tp->snd_cwnd * 717) / 1024`（最小値 `2U`）を返します。

### 4.4 その他のコールバック関数（初期提供済み）

- `bpf_cubic3_init()`: コネクション開始時に `bictcp_reset(ca)` でプライベート構造体をゼロクリアします。
- `bpf_cubic3_cong_avoid()`: `tcp_is_cwnd_limited(sk)` を確認し、Slow Start 中は `bpf_cubic3_slow_start()` で `snd_ssthresh` まで伸ばし、残りの `acked` に対して `bictcp_update()` と `bpf_cubic3_cong_avoid_ai(tp, ca->cnt, acked)` を実行します。
- `bpf_cubic3_set_state()`: RTO タイムアウト（`TCP_CA_Loss`）に入った際、`bictcp_reset(ca)` で状態をリセットします。
- `bpf_cubic3_acked()`: 各 ACK の `sample->rtt_us` から最小 RTT `ca->delay_min` を更新します（ただし Fast Recovery 終了直後の 1 秒間は遅延サンプルが不安定なため除外します）。
- `bpf_cubic3_undo_cwnd()`: 誤検知によるウィンドウ縮小を取り消す際、`max(tp->snd_cwnd, tp->prior_cwnd)` を返します。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### 5.1 `test.pkt` のシナリオと検証内容

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/test.pkt) は、実効 MSS = 1000 バイト・RTT = 100 ms（`delay_min = 100,000` usec）のコネクション上で、セクション 2.5 で計算した通りの 7 ステップを順番にアサートします。

1. **3-way ハンドシェイクと Slow Start（`cwnd: 10 -> 20`）**:
   10 セグメント（`10000` バイト）を送信し、100 ms 後の ACK で `tcpi_snd_cwnd == 20` に倍増するとともに、`bpf_cubic3_acked()` で `delay_min = 100 ms` が記録されます。
2. **パケットロス発生と $$\beta = 0.7$$ の検証（`cwnd = 20 -> ssthresh = 14`）**:
   4 セグメント（`10001:14001`）を送信し、先頭セグメント `10001:11001` がドロップして 3 個の SACK が到着することで Fast Recovery（`TCP_CA_Recovery`）に入ります。
   - **なぜ `starter.bpf.c` は失敗するのか？**: [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c) の `bpf_cubic3_ssthresh()` は Reno の半分（`tp->snd_cwnd >> 1U = 10`）を返すため、42 行目の `assert tcpi_snd_ssthresh == 14` で即座に失敗します。また、`bictcp_update()` も `ca->cnt = cwnd` のままなので、仮に `ssthresh` だけを直しても以降の凹領域・プラトーのアサートで失敗します。
   - **なぜ `solution.bpf.c` はパスするのか？**: `20 * 717 / 1024 = 14` により `tcpi_snd_ssthresh == 14` となり、Recovery 完了後の ACK（46 行目）で `tcpi_snd_cwnd == 14`、$$W_{\max} = 20$$ から輻輳回避エポックが始まります。
3. **Flight 1（エポック開始・凹領域の立ち上がり, $$t_{\text{elapsed}} = 0.0\text{ s}$$）**:
   `14000` バイト（14 セグメント）を送信して ACK を受けると、`bic_K = 2523`、`bic_target = 15`、`ca->cnt = 14` が計算され、`tcpi_snd_cwnd == 15` になります。
4. **Flight 2（凹領域の急成長, $$t_{\text{elapsed}} = 0.6\text{ s}$$）**:
   0.5 秒待ってから 15 セグメントを送信し、0.1 秒後に ACK を受け取ります（合計経過時間 $$0.6\text{ s}$$）。`bic_target = 18`、`ca->cnt = 15 / (18 - 15) = 5` となり、15 個の ACK で一気に $$+3$$ 増えて `tcpi_snd_cwnd == 18` にジャンプします。
5. **Flight 3（原点 $$W_{\max} = 20$$ への到達, $$t_{\text{elapsed}} = 1.8\text{ s}$$）**:
   18 セグメントを送信して ACK を受けると、変曲点 $$K \approx 2.46\text{ s}$$ に近づいて `delta = 0`、`bic_target = 20`、`ca->cnt = 18 / (20 - 18) = 9` となり、`tcpi_snd_cwnd == 20`（$$W_{\max}$$）に到達します。
6. **Flight 4（変曲点 $$t \approx K$$ でのプラトー検証, $$t_{\text{elapsed}} = 2.1\text{ s}$$）**:
   `cwnd = 20` かつ `bic_target = 20` となるため、`bic_target <= cwnd` のプラトー分岐に入り `ca->cnt = 100 * 20 = 2000` になります。20 セグメントすべてが ACK されても、`tcpi_snd_cwnd == 20` のままピタリと水平に維持されることを確認します。
7. **Flight 5（凸領域 $$t > K$$ での帯域探索, $$t_{\text{elapsed}} = 4.0\text{ s}$$）**:
   経過時間が $$K \approx 2.46\text{ s}$$ を超えると凸領域に入り、`delta = 1`、`bic_target = 20 + 1 = 21`、`ca->cnt = 20` となって、20 セグメントの ACK 後に `tcpi_snd_cwnd > 20` へ再び増加し始めることを確認します。

### 5.2 テストの実行方法

`labs/` の各ステップディレクトリ（またはリポジトリ内の任意の場所）から以下のコマンドを実行してください。

```sh
../../run_test.sh cubic/step01-cubic-curve starter
../../run_test.sh cubic/step01-cubic-curve solution
```
