# Step 2: Reno-friendly 領域 ($W_{\text{est}}$) と Fast Convergence

## 1. 学習ゴール

このステップでは、Step 1 で実装した純粋な CUBIC の三次関数曲線（$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$）が実ネットワークで直面する 2 つの課題——「新しい競合フローが参入したときに帯域を譲るのが遅い問題」と「短 RTT・小ウィンドウ環境で従来の TCP Reno より遅くなる問題」——を解決する **Fast Convergence（高速収束）** と **Reno-friendly 領域（$W_{\text{est}}$ / `tcp_cwnd`）** を実装します。トラック全体の構成や各ステップの位置づけについては、[CUBIC トラック概要 (`../README.md`)](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

`starter.bpf.c` の設計や `TODO` に取り組む前に、まずは CUBIC がなぜこれら 2 つの補助アルゴリズムを必要とするのか、その直感的な理由と数式、そしてカーネルが提供するデータ構造をゼロから整理しましょう。

### 2.1 純粋な CUBIC 曲線が抱える 2 つの課題

Step 1 で学んだ通り、純粋な CUBIC はパケットロスが発生したときの輻輳ウィンドウ `cwnd` を最大ウィンドウ $W_{\max}$（`ca->last_max_cwnd`）として記録し、`cwnd` を $\beta \approx 0.7$ 倍に減速します。その後、経過時間 $t$ に応じた三次関数

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}, \quad K = \sqrt[3]{\frac{W_{\max} - W_0}{C}}$$

に従って、`t < K`（凹領域）では $W_{\max}$ に向かって駆け上がり、`t = K` 付近の **プラトー（平坦な領域）** でいったん成長を緩めてから、`t > K`（凸領域）で再び未知の帯域を探索します。

この仕組みは長距離・広帯域ネットワーク（LFN: Long Fat Network）で非常に高い性能を発揮しますが、実ネットワークでそのまま使うと以下の 2 つの問題が生じます。

- **課題 1（新しいフローが参入したときの収束の遅さ）**: 既存の CUBIC フローが大きな $W_{\max}$ を維持しているボトルネックに新しいフローが参入すると、1 フローあたりの公平な帯域シェアは以前より小さくなります。しかし既存のフローは「以前の大きな $W_{\max}$」を目指して凹領域で急激にウィンドウを広げようとするため、帯域をなかなか譲らず、公平なシェアへ収束するまでに何度もパケットロスを繰り返してしまいます。
- **課題 2（短 RTT・小 BDP 環境での TCP Reno に対する劣後）**: $W_{\text{cubic}}(t)$ は RTT の長さではなく「実時間 $t$（秒）」の関数であり、特にプラトー（$t \approx K$）付近では傾きがほぼゼロになります。そのため、RTT が短い環境（1 秒間に何十回も RTT が回る環境）や $W_{\max}$ が小さい環境では、「1 RTT ごとに確実にウィンドウを増やす」標準的な TCP Reno（AIMD）よりもウィンドウの成長が遅くなってしまいます。

### 2.2 アルゴリズム 1: Fast Convergence（高速収束）

#### 直感とアイデア

送信側は「新しい競合フローが参入して自分の使える帯域が減った」ことをどうすれば検知できるでしょうか？

鍵になるのは、**今回のパケットロスが起きたときの `cwnd` と、前回のロス時に記録した `last_max_cwnd`（$W_{\max}$）の大小関係**です。

- **$\text{cwnd} \ge \text{last}_{\max,\text{cwnd}}$ のとき**: 前回の最大ウィンドウ $W_{\max}$ まで無事に回復し、さらにその先（凸領域）を探索している途中でロスが起きています。つまりボトルネックの空き帯域は減っておらず、通常通り `last_max_cwnd = cwnd` と記録すれば十分です。
- **$\text{cwnd} < \text{last}_{\max,\text{cwnd}}$ のとき**: 前回の $W_{\max}$ に戻るよりも手前で再びパケットロスが起きています。これは「新しい競合フローが参入したため、自分が使える帯域の上限が前回の $W_{\max}$ より狭くなった」という強いシグナルです。

このとき、もし単純に `last_max_cwnd = cwnd` とだけ更新すると、次のエポックでもこの `cwnd` を目標プラトーとして粘ってしまいます。そこで **Fast Convergence** では、`cwnd < last_max_cwnd` でロスが起きた場合、次の目標プラトー $W_{\max}$ を「ロス直前のウィンドウ $\text{cwnd}$」と「ロス直後の減速ウィンドウ $\beta \cdot \text{cwnd}$」の **中間値（平均）** まで一気に引き下げます。

$$W_{\max} = \text{cwnd} \times \frac{1 + \beta}{2}$$

$\beta \approx 0.7$ のとき $\frac{1 + 0.7}{2} = 0.85$ となるため、$W_{\max}$ を `cwnd` の約 $85\%$ に先回りして下げることで、次のエポックではより低いウィンドウで早めにプラトーに入り、新規参入フローへ素早く帯域を譲ることができます。

```text
cwnd
  ^
20|      * (Loss #1: cwnd=20 -> last_max_cwnd = 20, ssthresh = 14)
  |     /
14|    /   * (Loss #2: cwnd=14 < last_max_cwnd=20 -> 新規フロー参入を検知！)
  |   /   /  Fast Convergence なし: 次エポックのプラトー W_max = 14
11|  /   /   Fast Convergence あり: 次エポックのプラトー W_max = 14 * (1 + 0.7) / 2 = 11
  | /   /                         (低い W_max=11 で早めにプラトーに入り帯域を譲る)
 9|/   +-----> ssthresh = 9 から新エポック開始
  +---------------------------------------------------------------------> 時間 t
```

#### 固定小数点での数式と具体例

`starter.bpf.c` / `solution.bpf.c` では、浮動小数点を使わずに `BICTCP_BETA_SCALE = 1024` を分母とする固定小数点で $\beta$ を表しています（`beta = 717`、すなわち $717 / 1024 \approx 0.7002$）。したがって、$\frac{1 + \beta}{2}$ を掛ける整数演算は以下の式になります。

$$\text{last}_{\max,\text{cwnd}} = \left\lfloor \frac{\text{cwnd} \times (\text{BETA}_{\text{SCALE}} + \text{beta})}{2 \times \text{BETA}_{\text{SCALE}}} \right\rfloor = \left\lfloor \frac{\text{cwnd} \times (1024 + 717)}{2048} \right\rfloor = \left\lfloor \frac{\text{cwnd} \times 1741}{2048} \right\rfloor$$

なお、`ssthresh` 自体の戻り値は Fast Convergence の有無にかかわらず、常に通常の乗法的減少 $\max(\lfloor \text{cwnd} \times 717 / 1024 \rfloor, 2)$ です。

- **数値例（`test.pkt` のシナリオ）**:
  1. 1 回目のロスが `cwnd = 20` で発生（初期状態 `last_max_cwnd = 0`）:
     `20 < 0` は偽なので、`last_max_cwnd = 20`、`ssthresh = (20 * 717) / 1024 = 14` となります。
  2. 20 に回復する前の `cwnd = 14` で 2 回目のロスが発生:
     `cwnd (14) < last_max_cwnd (20)` が真となり、Fast Convergence が発動します。
     - `last_max_cwnd = (14 * 1741) / 2048 = 24374 / 2048 = 11`（Fast Convergence がなければ `14`）
     - `ssthresh = (14 * 717) / 1024 = 9`

### 2.3 アルゴリズム 2: Reno-friendly 領域（$W_{\text{est}}$ / `tcp_cwnd`）

#### 直感とアイデア

短 RTT 環境やプラトー付近で CUBIC が TCP Reno に負けないようにするため、CUBIC は内部で **「もし同じ経路で TCP Reno 相当の AIMD フローが走っていたら、今ごろウィンドウはいくつになっているか？」という推定値 $W_{\text{est}}$（コード上は `ca->tcp_cwnd`）** を並行して計算します。そして毎回、三次関数の目標値 $W_{\text{cubic}}(t)$ と $W_{\text{est}}$ のうち **大きい方** のペースでウィンドウを増やします。

$$W_{\text{target}} = \max\left(W_{\text{cubic}}(t),\, W_{\text{est}}\right)$$

#### なぜ $W_{\text{est}}$ は「1 RTT あたり +1」ではなく「約 1.875 RTT あたり +1」なのか？

ここで非常に重要な疑問が生まれます。標準の TCP Reno は「ロス時に $\beta_{\text{reno}} = 0.5$ 倍に減らし、1 RTT ごとに $\alpha_{\text{reno}} = +1$ セグメント増やす」アルゴリズムです。では、CUBIC の $W_{\text{est}}$ も 1 RTT ごとに $+1$ 増やせばよいのでしょうか？

答えは **No** です。なぜなら、CUBIC はロス時にウィンドウを $0.5$ 倍ではなく **$\beta \approx 0.7$ 倍にしか減らさない** からです。もし $0.7 W_{\max}$ という高い開始点からスタートして 1 RTT ごとに $+1$ ずつ増やしてしまうと、同じボトルネックを共有する標準 TCP Reno（$0.5 W_{\max}$ からスタートして 1 RTT ごとに $+1$）よりも常にウィンドウが大きくなり、Reno フローから帯域を奪ってしまいます。

AIMD($\alpha, \beta$) フローが標準 TCP Reno($1, 0.5$) と **同じ平均ウィンドウ（公平なスループット）** になるための 1 RTT あたりの増加幅 $\alpha$ は、定常状態のノコギリ波の平均面積から次式で導かれます。

$$\alpha = \frac{3(1 - \beta)}{1 + \beta}$$

ここに CUBIC の $\beta = 717 / 1024 \approx 0.7$ を代入すると、以下のようになります。

$$\alpha = \frac{3 \times (1024 - 717)}{1024 + 717} = \frac{3 \times 307}{1741} = \frac{921}{1741} \approx 0.529 \text{ [セグメント / RTT]}$$

つまり、1 RTT あたり約 $0.529$ セグメントずつ増やせば TCP Reno と同じ平均スループットになります。逆数をとると、**$W_{\text{est}}$（`ca->tcp_cwnd`）を $+1$ セグメント増やすのに必要な RTT 数 $\frac{1}{\alpha}$** は次の通りです。

$$\frac{1}{\alpha} = \frac{1 + \beta}{3(1 - \beta)} = \frac{1741}{921} \approx 1.890 \text{ [RTT]}$$

#### 固定小数点定数 `beta_scale` と ACK カウントによる更新規則

1 RTT の間にはおよそ `cwnd` 個のセグメントが ACK されるため、`ca->tcp_cwnd` を $+1$ 増やすのに必要な累積 ACK セグメント数 `delta` は $\text{cwnd} \times \frac{1}{\alpha}$ 個です。

カーネルおよび `starter.bpf.c` では、$\frac{1}{\alpha}$ を 8 倍（$= 2^3$ スケール）した整数定数 `beta_scale` をあらかじめ定義しています。

```c
static const __u32 beta_scale = 8 * (BICTCP_BETA_SCALE + beta) / 3
				/ (BICTCP_BETA_SCALE - beta);
```

`BICTCP_BETA_SCALE = 1024`、`beta = 717` を代入すると、整数除算により `beta_scale = 4642 / 307 = 15`（実数換算で $15 / 8 = 1.875$）となります。したがって、`ca->tcp_cwnd` を $+1$ 進めるのに必要な ACK 数 `delta` は、8 で割る右シフト `>> 3` を使って次のように求まります。

$$\text{delta} = \left\lfloor \frac{\text{cwnd} \times \text{beta}_{\text{scale}}}{8} \right\rfloor = ((\text{cwnd} \times \text{beta}_{\text{scale}}) \gg 3)$$

`bictcp_update(sk, cwnd, acked)` における Reno-friendly 領域の状態遷移ルールは以下の 4 ステップです。

1. **ACK セグメント数の累積**:
   `bictcp_update()` が呼ばれるたびに、まず冒頭で `ca->ack_cnt += acked;` として ACK されたセグメント数を加算します（なお、新エポック開始時 `ca->epoch_start == 0` には、すでに用意されている初期化処理によって `ca->ack_cnt = acked;` および `ca->tcp_cwnd = cwnd;` にリセットされます。また、同一時刻 `now_us == ca->last_time` に複数の ACK が到着した場合は、三次関数の再計算を省いて `goto tcp_friendliness;` へジャンプし、この累積した `ca->ack_cnt` を使って `ca->tcp_cwnd` を更新します）。
2. **`ca->tcp_cwnd` の更新（BPF 検証器に対応した $O(1)$ 演算）**:
   `tcp_friendliness:` ラベルで `delta = (cwnd * beta_scale) >> 3;` を計算し、`ca->ack_cnt` に溜まった ACK 数を `delta` で割った商の分だけ `ca->tcp_cwnd` を増やし、余りを `ca->ack_cnt` に残します。
   - **重要（BPF Verifier の制約）**: Linux カーネル本体の `net/ipv4/tcp_cubic.c` では `while (ca->ack_cnt > delta) { ca->ack_cnt -= delta; ca->tcp_cwnd++; }` というループで書かれていますが、BPF プログラムでは実行時の変数に依存する上限不明の `while` ループは **BPF 検証器（Verifier）が停止性を証明できずロード時にリジェクト** します。そのため、BPF ではゼロ除算を防ぐ `if (delta)` チェックを入れた上で、整数の除算 `/` と剰余 `%` を用いて $O(1)$ で計算します。
     ```c
     delta = (cwnd * beta_scale) >> 3;
     if (delta) {
             ca->tcp_cwnd += ca->ack_cnt / delta;
             ca->ack_cnt %= delta;
     }
     ```
3. **`ca->cnt` のクランプ（増加ペースの引き上げ）**:
   `bpf_cubic4_cong_avoid_ai(tp, ca->cnt, acked)` は、「`ca->cnt` 個のセグメントが ACK されるごとに `tp->snd_cwnd` を $+1$ 増やす」関数です。つまり **`ca->cnt` が小さいほど `cwnd` は速く成長します**。
   もし `ca->tcp_cwnd > cwnd`（Reno 推定ウィンドウの方が現在の `cwnd` を追い越している状態）であれば、1 RTT（`cwnd` 個の ACK）あたり `ca->tcp_cwnd - cwnd` セグメント増やす必要があるため、1 セグメント増やすのに必要な ACK 間隔 `max_cnt` は次式になります。
   $$\text{max}_{\text{cnt}} = \left\lfloor \frac{\text{cwnd}}{\text{ca}\to\text{tcp}_{\text{cwnd}} - \text{cwnd}} \right\rfloor$$
   三次関数が計算した `ca->cnt` がこの `max_cnt` より大きい（＝三次関数の方が成長が遅い）場合、`if (ca->cnt > max_cnt) ca->cnt = max_cnt;` として `ca->cnt` を引き下げ、Reno 相当の成長速度を保証します。
4. **最小値 `2U` の保証**:
   最後に既存の `ca->cnt = max(ca->cnt, 2U);` によって、輻輳回避フェーズで少なくとも 2 ACK に 1 回の増加ペースを超えないよう下限を設定します。

- **数値例（`test.pkt` のプラトー突破シナリオ）**:
  - 2 回目のロス回復後、`cwnd = 9` からエポックが開始し（`tcp_cwnd = 9`）、Flight 1（9 ACKs）と Flight 2（10 ACKs）を経て `cwnd = 11`（$= \text{last}_{\max,\text{cwnd}}$）のプラトーに到達します（この時点で `tcp_cwnd = 10`、`ack_cnt = 1`）。
  - `cwnd = 11` のプラトーでは、$t < K \approx 1.71\text{ s}$ の間、純粋な三次関数は `bic_target = 11 == cwnd` となるため `ca->cnt = 100 * 11 = 1100` を返し、1100 パケット ACK されるまで `cwnd` が増えない「ほぼ停止状態」になります。
  - 一方、Reno-friendly 領域では `cwnd = 11` のとき `delta = (11 * 15) >> 3 = 165 >> 3 = 20` です。11 パケットのフライトを 4 回（計 44 ACKs）送ると、`ack_cnt` が 20 を 2 回超えて `ca->tcp_cwnd` が `10 -> 11 -> 12` へと進みます。
  - `ca->tcp_cwnd (12) > cwnd (11)` となった瞬間、`max_cnt = 11 / (12 - 11) = 11` が計算され、`ca->cnt` は `1100` から `11` にクランプされます。その結果、プラトーの最中であっても 11 パケットの ACK で `cwnd` が `12` へと成長します。

### 2.4 カーネルと `starter.bpf.c` が提供する構造体・フィールド・ヘルパー

このステップで使うすべての構造体メンバとヘルパー関数は以下の通りです（すべて `vmlinux.h`、`bpf_cc_helpers.h`、`starter.bpf.c` で定義済みです）。

- **`icsk_ca_priv` と `struct bpf_cubic4_ca`（`inet_csk_ca(sk)` で取得）**:
  カーネルの各 TCP ソケット（`struct inet_connection_sock`）には、輻輳制御アルゴリズム専用のプライベート領域 `icsk_ca_priv` が用意されています。`inet_csk_ca(sk)` ヘルパーはこれを `struct bpf_cubic4_ca *` にキャストして返します。`starter.bpf.c` ではすでに以下の全フィールドが定義されています。
  - `ca->cnt` (`__u32`): `snd_cwnd` を $+1$ 増やすのに必要な ACK セグメント数（小さいほど成長が速い）。
  - `ca->last_max_cwnd` (`__u32`): 前回のロス時点での最大ウィンドウ $W_{\max}$（Fast Convergence で調整する対象）。
  - `ca->last_cwnd`, `ca->last_time` (`__u32`): 前回 `bictcp_update()` を実行したときの `cwnd` と時刻（$\mu\text{s}$）。`cwnd` が変わらない間、計算頻度を $1/32$ 秒に 1 回へ間引くために使われます。
  - `ca->bic_origin_point`, `ca->bic_K` (`__u32`): 三次関数の原点ウィンドウ $W_{\max}$ と、そこに到達するまでの時間 $K$（単位は `1 / 1024` 秒）。
  - `ca->delay_min` (`__u32`): 観測された最小 RTT（$\mu\text{s}$）。
  - `ca->epoch_start` (`__u32`): 現在の輻輳回避エポックの開始時刻（$\mu\text{s}$）。ロス時に `0` にリセットされ、次の輻輳回避の最初の ACK で現在時刻がセットされます。
  - `ca->ack_cnt` (`__u32`): Reno-friendly 計算用に累積した ACK セグメント数。
  - `ca->tcp_cwnd` (`__u32`): Reno 相当の推定ウィンドウ $W_{\text{est}}$。
- **`struct tcp_sock` の主要フィールド（`tcp_sk(sk)` で取得）**:
  - `tp->snd_cwnd` (`__u32`): 現在の輻輳ウィンドウ（セグメント数）。
  - `tp->snd_ssthresh` (`__u32`): スロースタート閾値（セグメント数）。
  - `tp->snd_cwnd_cnt` (`__u32`): `bpf_cubic4_cong_avoid_ai()` が `ca->cnt` に達するまで ACK 数を数えるカウンタ。
  - `tp->snd_cwnd_clamp` (`__u32`): `snd_cwnd` の上限値。
  - `tp->tcp_mstamp` (`__u64` / `__u32`): パケット送受信時刻（$\mu\text{s}$、`bictcp_clock_us(sk)` が返す値）。
- **定義済みの定数（`starter.bpf.c` 先頭）**:
  - `BICTCP_BETA_SCALE` (`1024`)
  - `beta` (`717` $\approx 0.7 \times 1024$)
  - `beta_scale` (`8 * (BICTCP_BETA_SCALE + beta) / 3 / (BICTCP_BETA_SCALE - beta)` $= 15$)

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の解説を踏まえて、`starter.bpf.c` の 2 つの `TODO` をコードに落とし込むための 4 つの設計問答に答えてみましょう。すべての問いはセクション 2 の内容から導き出すことができます。

### Q1. `bpf_cubic4_ssthresh()` で Fast Convergence を実装するとき、どの変数を比較し、どのような整数式で `ca->last_max_cwnd` を更新しますか？

ロス発生時に呼ばれる `bpf_cubic4_ssthresh()` では、すでに `cwnd = tp->snd_cwnd` と `ca = inet_csk_ca(sk)` が取得されています。`starter.bpf.c` では無条件に `ca->last_max_cwnd = cwnd;` となっている箇所を、セクション 2.2 の式と定数（`BICTCP_BETA_SCALE`、`beta`）を使ってどのように書き換えればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- **判定条件**: ロス発生時の `cwnd` が前回の最大ウィンドウ `ca->last_max_cwnd` より小さいかどうか、すなわち `if (cwnd < ca->last_max_cwnd)` を判定します。
- **更新式**:
  - `cwnd < ca->last_max_cwnd` のときは、$\text{cwnd} \times \frac{1 + \beta}{2}$ を固定小数点定数 `BICTCP_BETA_SCALE`（1024）と `beta`（717）で表した整数演算を行います。
  - そうでないとき（`cwnd >= ca->last_max_cwnd`）は、通常通り `ca->last_max_cwnd = cwnd;` とします。
  ```c
  if (cwnd < ca->last_max_cwnd)
          ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
                              / (2 * BICTCP_BETA_SCALE);
  else
          ca->last_max_cwnd = cwnd;
  ```

</details>

### Q2. `bictcp_update()` で `ca->ack_cnt += acked;` を関数の先頭（レート制限チェックの前）に置く必要があるのはなぜですか？また、新エポック開始時（`ca->epoch_start == 0`）にはどう扱われますか？

`bictcp_update()` の冒頭には、以下のような早期リターンと `goto` があります。

```c
	/* TODO 1 の位置 */

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto tcp_friendliness;
```

なぜ `ca->ack_cnt += acked;` をこれらの `if` 文よりも前に置く必要があるのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **理由 1（`1/32` 秒の間引き中も ACK 数を漏らさず数えるため）**: 三次関数 $W_{\text{cubic}}(t)$ の再計算は `cwnd` が同じであれば $1/32$ 秒（約 31.25 ms）に 1 回で十分ですが、Reno-friendly 領域の `ca->tcp_cwnd` は **到着したすべての ACK セグメント数** を数えて進める必要があります。早期リターン（`return;`）の前に `ca->ack_cnt += acked;` を実行しておくことで、間引かれた呼び出しの分の `acked` も確実に `ca->ack_cnt` に蓄積され、次に `tcp_friendliness:` へ到達したときにまとめて反映されます。
- **理由 2（同一タイムスタンプ `now_us == ca->last_time` での `goto tcp_friendliness` に対応するた め）**: 同じ時刻に複数の ACK が届いた場合、三次関数の計算だけをスキップして `tcp_friendliness:` へ飛びますが、その際にも今回の `acked` が加算されている必要があります。
- **新エポック開始時（`ca->epoch_start == 0`）の扱い**: エポックの最初の呼び出しでは、直後の `if (ca->epoch_start == 0)` ブロック内で `ca->ack_cnt = acked;` と上書き初期化されるため、先頭で加算しても二重カウントにはなりません。

</details>

### Q3. `tcp_friendliness:` ラベルにおいて、BPF 検証器（Verifier）を通過しつつ `ca->tcp_cwnd` と `ca->ack_cnt` を更新するにはどう書けばよいですか？

セクション 2.3 で学んだ通り、1 セグメント増やすのに必要な ACK しきい値は `delta = (cwnd * beta_scale) >> 3;` です。カーネルの `tcp_cubic.c` のような `while (ca->ack_cnt > delta)` ループを使わずに、エッジケース（ゼロ除算）も防ぎながら `ca->tcp_cwnd` と `ca->ack_cnt` を更新するコードを書いてみましょう。

<details>
<summary>ヒントと答え</summary>

- **BPF 検証器への対応とゼロ除算ガード**: 実行時の値に依存する `while` ループの代わりに整数の除算 `/` と剰余 `%` を使います。また、万一 `delta == 0` になった場合のゼロ除算を防ぐため、必ず `if (delta)` でガードします。
  ```c
  delta = (cwnd * beta_scale) >> 3;
  if (delta) {
          ca->tcp_cwnd += ca->ack_cnt / delta;
          ca->ack_cnt %= delta;
  }
  ```

</details>

### Q4. `ca->tcp_cwnd > cwnd` のとき、なぜ `ca->cnt` を「大きくする」のではなく `max_cnt` 以下に「小さくクランプする」のでしょうか？

`ca->tcp_cwnd` が現在の `cwnd` を上回ったとき、Reno 相当の成長速度を保証するために `ca->cnt` をどのように更新すべきか、その理由とともにコードを示してください。

<details>
<summary>ヒントと答え</summary>

- **理由**: `ca->cnt` は「`cwnd` を $+1$ 増やすために必要な ACK セグメント数（更新間隔）」を表します。たとえばプラトー付近では三次関数が `ca->cnt = 100 * cwnd`（ほとんど増やさない）を設定します。したがって、ウィンドウの成長ペースを **速くする** には、必要な ACK 間隔 `ca->cnt` を **小さくする（上から抑える）** 必要があります。
- **コード**: 1 RTT（`cwnd` 個の ACK）で `ca->tcp_cwnd - cwnd` 個増やすための間隔 `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` を求め、`ca->cnt > max_cnt` なら `ca->cnt = max_cnt` に引き下げます。
  ```c
  if (ca->tcp_cwnd > cwnd) {
          max_cnt = cwnd / (ca->tcp_cwnd - cwnd);
          if (ca->cnt > max_cnt)
                  ca->cnt = max_cnt;
  }
  ```

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

`solution.bpf.c` では、Step 1 の純粋な CUBIC 実装に対して以下の 2 つの関数を拡張しています。それぞれの処理と固定小数点演算を確認しましょう。

### 4.1 `bpf_cubic4_ssthresh()` — Fast Convergence と乗法的減少

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_cubic4_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_cubic4_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd;

	ca->epoch_start = 0;

	/* W_max and Fast Convergence */
	if (cwnd < ca->last_max_cwnd)
		ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
				    / (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = cwnd;

	return max((cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}
```

- `ca->epoch_start = 0;`: パケットロスが発生したため、現在の輻輳回避エポックを終了します。回復後に再び `bpf_cubic4_cong_avoid()` が呼ばれた際、`bictcp_update()` 内の `if (ca->epoch_start == 0)` 分岐が実行され、新しい `ca->last_max_cwnd` を基に `ca->bic_K` と `ca->bic_origin_point` が再計算されます。
- `if (cwnd < ca->last_max_cwnd)`: 前回の最大ウィンドウ $W_{\max}$ に到達する前にロスが発生した場合、`(cwnd * (1024 + 717)) / 2048` によって `ca->last_max_cwnd` を約 $0.85 \times \text{cwnd}$ に引き下げます。
- `return max((cwnd * beta) / BICTCP_BETA_SCALE, 2U);`: 新しいスロースタート閾値 `snd_ssthresh` として $\max(\lfloor 0.7 \times \text{cwnd} \rfloor, 2)$ を返します。

### 4.2 `bictcp_update()` — エポック初期化、三次関数、および Reno-friendly 領域

```c
static __always_inline void bictcp_update(struct sock *sk, __u32 cwnd, __u32 acked)
{
	struct bpf_cubic4_ca *ca = inet_csk_ca(sk);
	__u32 now_us = bictcp_clock_us(sk);
	__u32 delta, bic_target, max_cnt;
	__u64 offs, t;

	ca->ack_cnt += acked;	/* Count the number of ACKed segments */

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = now_us;

	if (ca->epoch_start == 0) {
		ca->epoch_start = now_us;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K = cubic_root(cube_factor *
					       (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}
	/* ... (中略: 三次関数による bic_target と ca->cnt の算出) ... */

tcp_friendliness:
	delta = (cwnd * beta_scale) >> 3;
	if (delta) {
		ca->tcp_cwnd += ca->ack_cnt / delta;
		ca->ack_cnt %= delta;
	}

	if (ca->tcp_cwnd > cwnd) {
		max_cnt = cwnd / (ca->tcp_cwnd - cwnd);
		if (ca->cnt > max_cnt)
			ca->cnt = max_cnt;
	}

	ca->cnt = max(ca->cnt, 2U);
}
```

- **冒頭の `ca->ack_cnt += acked;`**: 到着した ACK セグメント数を毎回加算します。
- **エポック開始時（`ca->epoch_start == 0`）**: `ca->ack_cnt = acked;`、`ca->tcp_cwnd = cwnd;` として、Reno 推定ウィンドウ $W_{\text{est}}$ をエポック開始時点の `cwnd` からスタートさせます。また、Fast Convergence で引き下げられた `ca->last_max_cwnd` を使って `ca->bic_K` と `ca->bic_origin_point` を設定します。
- **`tcp_friendliness:` ブロック**:
  - `delta = (cwnd * beta_scale) >> 3;` で、`ca->tcp_cwnd` を 1 増やすのに必要な ACK 数（$\approx 1.875 \times \text{cwnd}$）を計算します。
  - `ca->tcp_cwnd += ca->ack_cnt / delta;` と `ca->ack_cnt %= delta;` により、BPF 検証器に優しいループなしの形（$O(1)$）で `ca->tcp_cwnd` を進めます。
  - `ca->tcp_cwnd > cwnd` のとき、`max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` を上限として `ca->cnt` をクランプします。これにより、三次関数がプラトーで `ca->cnt = 100 * cwnd` を出していても、`ca->tcp_cwnd` が追いついた時点で Reno 相当のペースでウィンドウが再び成長し始めます。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

`test.pkt` は、RTT = 100 ms、MSS = 1000 バイト（TS オプション込みで 1012 バイト）のコネクション上で **Fast Convergence** と **Reno-friendly 領域** の両方を順番に検証します。

### `test.pkt` のタイムラインと検証内容

1. **Slow Start（`0.000s 〜 0.200s`、`cwnd: 10 -> 20`）**:
   - 3-way ハンドシェイク完了後、初期ウィンドウ 10 セグメント（10,000 バイト）を送信し、0.1 秒後の ACK で `cwnd == 20` に成長するとともに最小 RTT `ca->delay_min = 100,000 us`（100 ms）が記録されます。
2. **1 回目のパケットロス（`0.200s 〜 0.400s`、`cwnd = 20`）**:
   - 4 セグメント送信し、先頭セグメントが欠落して 3 個の SACK を受信することで Recovery に遷移します。
   - この時点では `ca->last_max_cwnd == 0` なので、`cwnd (20) < last_max_cwnd (0)` は偽となり、`ca->last_max_cwnd = 20`、`ssthresh = (20 * 717) / 1024 = 14` が設定されます。Recovery 完了後、`cwnd == 14` となります。
3. **2 回目のパケットロス（`0.400s 〜 0.600s`、`cwnd = 14 < last_max_cwnd = 20`）— Fast Convergence の発動**:
   - `cwnd` が前回の最大値 `20` に戻る前の `cwnd = 14` で再びロスが発生します。
   - `ssthresh` は `(14 * 717) / 1024 = 9` となり、Recovery 完了後の `cwnd` は `9` になります。
   - **`solution.bpf.c`（Fast Convergence あり）**: `14 < 20` が成り立つため、`ca->last_max_cwnd = (14 * 1741) / 2048 = 11` に引き下げられます。
   - **`starter.bpf.c`（Fast Convergence 未実装）**: 無条件に `ca->last_max_cwnd = 14` となります。
4. **Congestion Avoidance での Fast Convergence 検証（Flight 1 & Flight 2）**:
   - **Flight 1（エポック開始、`t_elapsed = 0.0s`）**: `cwnd = 9` で 9 セグメント送信・ACK されます。エポック開始時の `bic_target` は `10`、`ca->cnt = 9 / (10 - 9) = 9` となり、9 個の ACK で `cwnd` は `9 -> 10` に増えます。
   - **Flight 2（`t_elapsed = 0.8s`、99 行目のアサート `assert tcpi_snd_cwnd == 11`）**: 0.7 秒待ってから 10 セグメント送信し、0.1 秒後に ACK を受け取ります（エポック開始から 0.8 秒経過）。
     - **`solution.bpf.c` が PASS する理由**: Fast Convergence により `last_max_cwnd = 11`（`bic_K = 1750` $\approx 1.71\text{ s}$）となっているため、0.8 秒時点では `delta = 0` で `bic_target` がすでにプラトーの `11` に到達しています。したがって `ca->cnt = 10 / (11 - 10) = 10` となり、10 セグメントの ACK で `cwnd` はちょうど `11` になってアサートに PASS します。
     - **`starter.bpf.c` が FAIL する理由**: Fast Convergence がないため `last_max_cwnd = 14` のままになっており、0.8 秒時点の `bic_target` が `13` になります。すると `ca->cnt = 10 / (13 - 10) = 3` という急激な増加レートになり、10 セグメントの ACK で `cwnd` が `13` まで跳ね上がって `expected cwnd 11 (Fast Convergence W_max=11), got 13` でアサートに失敗します。
5. **プラトー（`cwnd = 11 == last_max_cwnd`）での Reno-friendly 領域の検証（Flights 3〜6）**:
   - `cwnd = 11` に達した後、11 セグメントのフライトを 4 回連続で送受信します（経過時間はエポック開始から 0.9s, 1.0s, 1.1s, 1.2s）。
   - この間、まだ $t < K \approx 1.71\text{ s}$ なので、純粋な三次関数だけでは `bic_target == 11 == cwnd` のプラトーにとどまり、`ca->cnt = 100 * 11 = 1100` となって `cwnd` は `11` から一切増えません（もし Fast Convergence だけを実装して Reno-friendly 領域を未実装のままにすると、最後の 125 行目のアサート `assert tcpi_snd_cwnd > 11` で `got 11` となり FAIL します）。
   - **`solution.bpf.c` が PASS する理由**: Reno-friendly 領域が有効なため、`delta = (11 * 15) >> 3 = 20` ACK ごとに `ca->tcp_cwnd` が `10 -> 11 -> 12` と着実に増加します。Flight 6 で `ca->tcp_cwnd` が `12`（`> cwnd = 11`）に達した瞬間に `max_cnt = 11 / (12 - 11) = 11` で `ca->cnt` が `1100` から `11` にクランプされ、`cwnd` が `12`（`> 11`）へと成長して最終アサートに PASS します。

### 実行コマンド

`labs/` ディレクトリから見ても、このステップのディレクトリ（`cubic/step02-reno-friendly-fast-conv`）から相対パスで実行する場合も、以下のコマンドでテストできます。

```sh
../../run_test.sh cubic/step02-reno-friendly-fast-conv starter
../../run_test.sh cubic/step02-reno-friendly-fast-conv solution
```
