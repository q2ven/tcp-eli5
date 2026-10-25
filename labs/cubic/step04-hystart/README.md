# Step 4: HyStart（ACK Train と Delay 増加による Slow Start 早期脱出）

## 学習ゴール

Step 3 までで、CUBIC の 3 次関数ウィンドウ曲線・Fast Convergence・Reno-friendly 領域・アイドル後の Epoch シフトを備えた輻輳回避（Congestion Avoidance）を実装しました（トラック全体の構成は [CUBIC トラック概要](../README.md) を参照してください）。
しかし、ここまでの実装には **接続開始直後の初期 Slow Start でボトルネックルータのバッファを溢れさせてしまう** という大きな課題が残っています。このステップでは、パケットロスが起きる前にネットワークの飽和兆候（ACK 列の詰まりと RTT の増加）を検知して Slow Start から Congestion Avoidance へ安全に着陸させる **HyStart（Hybrid Slow Start）** を `.pkts_acked` フック（`bpf_cubic6_acked()` と `hystart_update()`）に実装します。

---

## アルゴリズムと背景の解説（まずここを読む）

### 1. なぜ初期 Slow Start で大事故が起きるのか？（Slow Start オーバーシュート問題）

TCP コネクションが確立された直後、送信側はネットワークの空き帯域（帯域遅延積: BDP = Bandwidth-Delay Product）を知らないため、**Slow Start** フェーズから送信を開始します。
初期状態の Slow Start 閾値 `tp->snd_ssthresh` は事実上の無限大である `TCP_INFINITE_SSTHRESH`（`0x7fffffff`）に設定されており、`tp->snd_cwnd < tp->snd_ssthresh` が成り立つ間、カーネルの `tcp_slow_start()` は到着した ACK のパケット数分だけ `tp->snd_cwnd` を加算します。つまり、**1 RTT ごとに `snd_cwnd` が 2 倍（$$10 \to 20 \to 40 \to 80 \dots$$）へ指数関数的に膨張**します。

ここに深刻な落とし穴があります。

- あるラウンドで `snd_cwnd` がちょうどネットワークの BDP（パイプの容量 $$W_{\text{BDP}}$$）に達したとします。
- しかしその時点ではまだパケットロスは起きていないため、Slow Start は止まりません。そのラウンドの $$W_{\text{BDP}}$$ 個のパケットに対する ACK が返ってくる間に、`snd_cwnd` は **$$2 \times W_{\text{BDP}}$$ まで倍増** してしまいます。
- もしボトルネックルータのバッファ容量が $$W_{\text{BDP}}$$ 未満（浅いバッファのスイッチなど）であれば、パイプに入りきらない最大 $$W_{\text{BDP}}$$ 個ものパケットが 1 RTT のうちに一斉に破棄（テールドロップ）されます。
- これほど大量のバーストロスが起きると、SACK による Fast Recovery でも回復しきれずに再送タイムアウト（RTO）へ陥り、`snd_cwnd = 1` にリセットされて通信が長時間ストールしてしまいます。

**HyStart（Hybrid Slow Start）の基本アイデア**:
「パケットロスが起きるまで倍増を続ける」のではなく、初期 Slow Start 中に返ってくる ACK から **「パイプが満杯になった兆候」を 1 RTT（ラウンド）単位で監視し、ロスが起きる前に `tp->snd_ssthresh = tp->snd_cwnd` に引き下げて Congestion Avoidance へソフトランディングする** のが HyStart です。

---

### 2. ラウンド（1 RTT のフライト）の定義と境界管理

HyStart は 1 RTT ごとの「ラウンド」単位で測定を行います。シーケンス番号空間を使ってラウンドの区切りを次のように追跡します。

1. **ラウンド開始時の記録（`hystart_reset()`）**:
   新しいラウンドが始まる瞬間、その時点で次に送信する予定のシーケンス番号 `tp->snd_nxt` をラウンド終端 `ca->end_seq` として記録し、現在時刻 `now_us = tp->tcp_mstamp`（マイクロ秒）を `ca->round_start` と `ca->last_ack` に保存します。また、そのラウンド内の最小 RTT `ca->curr_rtt` を `~0U`（`0xffffffff`）、サンプル数 `ca->sample_cnt` を `0` に初期化します。
2. **ラウンド完了の判定**:
   ACK が到着して未確認シーケンス番号 `tp->snd_una` が `ca->end_seq` を追い越した瞬間（`after(tp->snd_una, ca->end_seq)` が真になった瞬間）、ラウンド開始時に飛んでいた 1 RTT 分のパケットがすべて相手に届いたことが分かります。そこで直ちに `hystart_reset()` を呼んで次のラウンドを開始します。
3. **最小ウィンドウ条件（`HYSTART_LOW_WINDOW = 16`）と順序の注意点**:
   `snd_cwnd` がまだ小さいうち（`tp->snd_cwnd < 16`）はサンプル数が少なく誤検知しやすいため、HyStart の検出器は動かしません。
   **ただし、`after(tp->snd_una, ca->end_seq)` によるラウンド更新だけは、`tp->snd_cwnd < 16` のチェックより必ず先に行わなければなりません。**
   もし `snd_cwnd < 16` で先に `return` してしまうと、`snd_cwnd` が 10 から 16 以上に増えるまでの間 `ca->end_seq` が一度も更新されず、接続開始時の古い `round_start` や `end_seq` が残ったまま `snd_cwnd >= 16` の最初の検出処理が走ってしまうためです（Linux カーネルのコミット `25c1a9ca53db` で修正された歴史的バグです）。

---

### 3. HyStart の 2 つの独立した検出器

HyStart は特性の異なる 2 つの検出器を併用し、**どちらか一方でも条件を満たしたら** `ca->found = 1` を立てて `tp->snd_ssthresh = tp->snd_cwnd` に設定します。

```text
  [送信側]                   [ボトルネックルータ]                 [受信側]
  ラウンド開始 (round_start)
  パケット群をバースト送信 ──► パイプに詰まり、キューが溜まり始める ──► データ到着
                                                                      │
  ◄────────────────────────── 密な ACK 列 (ACK Train) が戻る ─────────┘
  |◄── ACK 間隔 <= 2ms ──►|
  |◄────── トレイン長 >= delay_min / 2 なら [Detector 1 発火!]
  |
  |  8 サンプル以上の最小 RTT (curr_rtt) >= delay_min + 閾値 なら [Detector 2 発火!]
```

#### 検出器 1: ACK Train 検出（`HYSTART_ACK_TRAIN`）

- **直感的な仕組み**:
  Slow Start 中に送信側が 1 ラウンド分のパケット群を送出すると、それらのパケットはボトルネック回線の帯域幅に応じた間隔で順番に受信側へ届き、その応答である ACK も密に連なった「列車（ACK Train）」となって送信側へ戻ってきます。
  この ACK Train の先頭（`ca->round_start`）から末尾までの継続時間は、1 ラウンド分のデータがボトルネックを通過するのに要した時間を表します。往復の最小伝搬遅延 `ca->delay_min` のうち片道分はおおよそ $$\text{delay}_{\min} / 2$$ であるため、**途切れない ACK Train がラウンド開始から $$\text{delay}_{\min} / 2$$ 以上続いたなら、送出したデータ量がすでに片道パイプ容量を埋め尽くしている** と判断できます。
- **判定ルールと数式**:
  1. 直前の ACK からの経過時間が `HYSTART_ACK_DELTA_US`（$$2000\ \mu\text{s} = 2\text{ ms}$$）以内かどうかを符号付き 32 ビット差分で判定します：
     ```c
     if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US)
     ```
  2. $$2\text{ ms}$$ 以内であれば同じ ACK Train が継続しているとみなし、`ca->last_ack = now_us` に更新します（もし $$2\text{ ms}$$ より大きく空いた場合は `ca->last_ack` が更新されないため、そのラウンドでは以降の ACK でも差が広がり続け、ACK Train 検出は自動的に無効化されます）。
  3. そのままラウンド開始からの経過時間が `ca->delay_min` の半分（右 1 ビットシフト `ca->delay_min >> 1`）以上に達したかを判定します：
     ```c
     if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
         ca->found = 1;
         tp->snd_ssthresh = tp->snd_cwnd;
     }
     ```

#### 検出器 2: Delay 増加検出（`HYSTART_DELAY`）

- **直感的な仕組み**:
  ACK 間引きや GRO/LRO によって ACK Train が途切れる環境でも、in-flight パケット数が BDP を超えてボトルネックルータのバッファにキューが溜まり始めれば、必ず **RTT（往復遅延）が最小 RTT（`ca->delay_min`）より長くなります**。
  ただし、たった 1 個の ACK の RTT だけを見て判定すると、OS のスケジューリング遅延や NIC の割り込み遅延（ジッタ）で誤発火してしまいます。そこで、**同じラウンド内で最低 `HYSTART_MIN_SAMPLES`（8 個）の RTT サンプルを集め、その 8 個以上のサンプルの中で最も小さかった RTT（`ca->curr_rtt`）でさえも基準値より伸びているか** を検査します。8 個の最小値を取ることで、一過性のジッタ（上振れノイズ）を確実に除去できます。
- **判定ルールと数式**:
  1. 今回の ACK の RTT `delay`（マイクロ秒）で、現在のラウンドの最小 RTT `ca->curr_rtt` を更新します：
     ```c
     if (ca->curr_rtt > delay)
         ca->curr_rtt = delay;
     ```
  2. サンプル数 `ca->sample_cnt` を `HYSTART_MIN_SAMPLES`（`8`）に達するまでカウントアップします：
     ```c
     if (ca->sample_cnt < HYSTART_MIN_SAMPLES)
         ca->sample_cnt++;
     ```
  3. `ca->sample_cnt >= HYSTART_MIN_SAMPLES`（8 サンプル以上収集済み）になったら、`ca->curr_rtt` が最小 RTT `ca->delay_min` に動的閾値 $$\Delta_{\text{thresh}}$$ を足した値以上かを判定します：
     $$\Delta_{\text{thresh}} = \text{clamp}\left(\frac{\text{delay}_{\min}}{8},\ 4000\ \mu\text{s},\ 16000\ \mu\text{s}\right)$$
     ```c
     if (ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)) {
         ca->found = 1;
         tp->snd_ssthresh = tp->snd_cwnd;
     }
     ```
  - **なぜ $$\text{delay}_{\min} \gg 3$$（$$12.5\%$$）を $$[4\text{ ms}, 16\text{ ms}]$$ にクランプするのか？**:
    - 基本は最小 RTT の $$1/8$$（`ca->delay_min >> 3`）を許容キューイング遅延とします。
    - **下限 `HYSTART_DELAY_MIN = 4000U`（$$4\text{ ms}$$）**: データセンターや LAN など $$\text{delay}_{\min} \le 1\text{ ms}$$ の超低遅延環境では、$$\text{delay}_{\min} / 8 \approx 125\ \mu\text{s}$$ となり、TSO/GRO のパケット集約だけで簡単に超えてしまいます。最低 $$4\text{ ms}$$ の余裕を持たせることで早すぎる Slow Start 脱出を防ぎます。
    - **上限 `HYSTART_DELAY_MAX = 16000U`（$$16\text{ ms}$$）**: 衛星回線や大陸間回線など $$\text{delay}_{\min} = 200\text{ ms}$$ を超える長遅延環境では、$$\text{delay}_{\min} / 8 = 25\text{ ms}$$ 以上もの深いキューが溜まるまで待ってしまうため、上限を $$16\text{ ms}$$ に抑えます。

---

### 4. 具体例で追う数値トレース（Delay 増加検出）

最小 RTT が $$20\text{ ms}$$（`delay_min = 20000` $$\mu\text{s}$$）の回線で、Slow Start がどのように止まるかを数値で確認しましょう。

- **閾値の計算**:
  `delay_min >> 3` は $$20000 / 8 = 2500\ \mu\text{s}$$（$$2.5\text{ ms}$$）です。これを `clamp(2500, 4000U, 16000U)` に通すと下限の **`4000` $$\mu\text{s}$$（$$4\text{ ms}$$）** が選ばれます。
  したがって、Delay 増加検出の発火ラインは `20000 + 4000 = 24000` $$\mu\text{s}$$（**$$24\text{ ms}$$**）となります。
- **Round 1（`snd_cwnd: 10 -> 20`）**:
  初期ウィンドウ 10 パケットを送信し、$$20\text{ ms}$$ 後に累積 ACK が返ります。`ca->delay_min = 20000` が記録され、Slow Start によって `snd_cwnd` は `10` から `20`（$$\ge 16$$）へ倍増し、次の 20 パケットが送信されます。
- **Round 2（`snd_cwnd >= 20`、バッファにキューが溜まり RTT が $$35\text{ ms}$$ に増加）**:
  - 1 個目の ACK が到着（`delay = 35000` $$\mu\text{s}$$）：`after(tp->snd_una, ca->end_seq)` が真となり、`hystart_reset()` で新ラウンドが開始されます。`ca->curr_rtt = 35000`、`ca->sample_cnt = 1` になります。
  - 2〜7 個目の ACK が到着（`delay = 35000` $$\mu\text{s}$$）：`ca->sample_cnt` が `2, 3, ..., 7` と増えますが、まだ 8 サンプル未満なので発火しません（`snd_cwnd` は ACK ごとに $$+1$$ ずつ増えていきます）。
  - **8 個目のサンプルとなる ACK が到着**（`delay = 35000` $$\mu\text{s}$$）：`ca->sample_cnt` が `8`（`HYSTART_MIN_SAMPLES`）に到達します。`ca->curr_rtt`（$$35000\ \mu\text{s}$$）$$\ge$$ `24000` $$\mu\text{s}$$ が成立するため、**その場で `ca->found = 1`、`tp->snd_ssthresh = tp->snd_cwnd`（28）に設定され、パケットロス 0 で Slow Start を脱出します！**

---

### 5. カーネルと `bpf_cc_helpers.h` が提供する構造体・フィールド・ヘルパー一覧

コードを書く前に、このステップでアクセスするカーネル側の構造体メンバと既存ヘルパーを整理しておきましょう。

1. **`struct tcp_sock *tp = tcp_sk(sk)`（TCP ソケット本体）**:
   - `tp->snd_cwnd`（`__u32`）: 現在の輻輳ウィンドウサイズ（パケット数）。
   - `tp->snd_ssthresh`（`__u32`）: Slow Start 閾値。初期値は `TCP_INFINITE_SSTHRESH`（`0x7fffffff`）。`tp->snd_ssthresh = tp->snd_cwnd` を代入すると、`tcp_in_slow_start(tp)`（`tp->snd_cwnd < tp->snd_ssthresh`）が偽になり Congestion Avoidance へ移行します。
   - `tp->snd_una`（`__u32`）: まだ ACK されていない最古のシーケンス番号。
   - `tp->snd_nxt`（`__u32`）: 次に送信するシーケンス番号（ラウンド開始時に `ca->end_seq` へコピーします）。
   - `tp->tcp_mstamp`（`__u64` / 下位 32 ビットを `__u32 now_us` として使用）: TCP スタックが管理するマイクロ秒単位の現在タイムスタンプ。
2. **`const struct ack_sample *sample`（`.pkts_acked` フックの第 2 引数）**:
   - `sample->rtt_us`（`__s32`）: この ACK で計測された RTT（マイクロ秒）。RTT が計測できなかった場合は負値（`< 0`）が入ります。
3. **`icsk_ca_priv` と `struct bictcp *ca = inet_csk_ca(sk)`**:
   - 各ソケットの `struct inet_connection_sock` には、輻輳制御アルゴリズム専用のプライベートメモリ領域 `icsk_ca_priv` が用意されており、`inet_csk_ca(sk)` でポインタを取得できます。
   - 本ステップではカーネルの `vmlinux.h` に定義されている `struct bictcp` をそのまま使用します。HyStart で使うフィールドは以下の通りです：
     - `ca->delay_min`（`__u32`）: コネクション確立以降に観測された最小 RTT（$$\mu\text{s}$$）。
     - `ca->round_start`（`__u32`）: 現在のラウンドが始まった時刻（$$\mu\text{s}$$）。
     - `ca->last_ack`（`__u32`）: 現在の ACK Train で直近の ACK を受信した時刻（$$\mu\text{s}$$）。
     - `ca->end_seq`（`__u32`）: 現在のラウンドの終端シーケンス番号。
     - `ca->curr_rtt`（`__u32`）: 現在のラウンド内で観測された最小 RTT（$$\mu\text{s}$$、ラウンド開始時は `~0U`）。
     - `ca->sample_cnt`（`__u32`）: 現在のラウンド内で収集した RTT サンプル数。
     - `ca->found`（`__u8`）: HyStart が Slow Start 脱出を検知済みかを示すフラグ（未検知なら `0`、検知したら `1`）。
4. **`bpf_cc_helpers.h` のインラインヘルパー**:
   - `after(seq2, seq1)`: 32 ビットシーケンス番号のラップアラウンドを考慮して `seq2` が `seq1` より後（`(__s32)(seq1 - seq2) < 0`）かを判定します。
   - `clamp(val, lo, hi)`: `val` を `[lo, hi]` の範囲に収めます。
   - `tcp_in_slow_start(tp)`: `tp->snd_cwnd < tp->snd_ssthresh` を返します。

---

## 設計チェックポイント（理解した仕組みをコードに落とし込む）

上の解説を踏まえて、`starter.bpf.c` の `hystart_update()` にある `TODO` をどのようにコードへ落とし込むか、4 つの設計ポイントを考えてみましょう。

### Q1. `bpf_cubic6_acked()` はどのような条件のときだけ `hystart_update()` を呼び出すか？

`bpf_cubic6_acked()`（既に `starter.bpf.c` に実装済み）を見ると、`ca->delay_min` を更新した後、3 つの条件がすべて成り立つときだけ `hystart_update(sk, delay)` を呼んでいます。その 3 つの条件とは何で、なぜそれぞれが必要なのでしょうか？

<details>
<summary>ヒントと答え</summary>

`bpf_cubic6_acked()` の呼び出し条件は以下の 3 つです：

```c
if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH &&
    tcp_in_slow_start(tp) && !ca->found)
	hystart_update(sk, delay);
```

- `tp->snd_ssthresh == TCP_INFINITE_SSTHRESH`: コネクション開始直後の **初期 Slow Start** であることを確認します（一度ロスを経験した後の再 Slow Start では、すでに過去の `snd_ssthresh` が分かっているため HyStart は使いません）。
- `tcp_in_slow_start(tp)`: 現在 `tp->snd_cwnd < tp->snd_ssthresh` の Slow Start 中であることを確認します。
- `!ca->found`: まだ HyStart の検出器が発火していないことを確認します。

</details>

### Q2. `hystart_update()` の先頭で行うラウンド境界チェックとウィンドウサイズチェックの正しい順序は？

`hystart_update(struct sock *sk, __u32 delay)` の冒頭では、「新しいラウンドに入ったかの判定（`hystart_reset(sk)`）」と「ウィンドウサイズが `HYSTART_LOW_WINDOW`（16）未満なら何もしない早期リターン」の 2 つを行います。この 2 つの `if` 文はどう並べるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

必ず **ラウンド境界の判定（`after(tp->snd_una, ca->end_seq)`）を先** に書き、その後に `tp->snd_cwnd < HYSTART_LOW_WINDOW` のチェックを置きます。

```c
if (after(tp->snd_una, ca->end_seq))
	hystart_reset(sk);

if (tp->snd_cwnd < HYSTART_LOW_WINDOW)
	return;
```

先にラウンドを更新しておくことで、`snd_cwnd` が 10 から 16 に成長するまでの間も 1 RTT ごとに `ca->end_seq` と `ca->round_start` が正しく前進し、`snd_cwnd >= 16` に到達したラウンドを正確な開始時刻・シーケンス番号で測定できます。

</details>

### Q3. Detector 1（`HYSTART_ACK_TRAIN`）は C コードでどう記述するか？

現在時刻 `__u32 now_us = tp->tcp_mstamp;` を使って、「直前の ACK からの間隔が `HYSTART_ACK_DELTA_US`（`2000U`）以内なら `ca->last_ack` を更新し、さらにラウンド開始時刻 `ca->round_start` からの経過時間が `ca->delay_min` の半分以上なら Slow Start を終了する」というロジックを C コードで組み立ててみましょう。

<details>
<summary>ヒントと答え</summary>

32 ビットタイムスタンプの引き算は、ラップアラウンドに対処するため `(__s32)` にキャストして比較します。`ca->delay_min` の半分は右ビットシフト `ca->delay_min >> 1` で計算できます。

```c
/* Detector 1: HYSTART_ACK_TRAIN */
if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US) {
	ca->last_ack = now_us;
	if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
		ca->found = 1;
		tp->snd_ssthresh = tp->snd_cwnd;
	}
}
```

</details>

### Q4. Detector 2（`HYSTART_DELAY`）は C コードでどう記述するか？

引数で渡された今回の RTT `delay`（$$\mu\text{s}$$）を使って、「ラウンド内の最小 RTT `ca->curr_rtt` を更新し、サンプル数 `ca->sample_cnt` を最大 `HYSTART_MIN_SAMPLES`（8）まで増やし、8 サンプル以上集まった状態で `ca->curr_rtt` が閾値以上なら Slow Start を終了する」というロジックを C コードで組み立ててみましょう。

<details>
<summary>ヒントと答え</summary>

`ca->curr_rtt` は `hystart_reset()` で `~0U`（最大値）に初期化されているため、単純な `if (ca->curr_rtt > delay)` で最小値を追跡できます。閾値のクランプには `bpf_cc_helpers.h` の `clamp()` マクロを使います。

```c
/* Detector 2: HYSTART_DELAY */
if (ca->curr_rtt > delay)
	ca->curr_rtt = delay;

if (ca->sample_cnt < HYSTART_MIN_SAMPLES)
	ca->sample_cnt++;

if (ca->sample_cnt >= HYSTART_MIN_SAMPLES) {
	if (ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)) {
		ca->found = 1;
		tp->snd_ssthresh = tp->snd_cwnd;
	}
}
```

</details>

---

## 実装ステップと解説 (`solution.bpf.c`)

`solution.bpf.c` における HyStart 関連の実装を関数ごとに確認します。

### 1. HyStart の定数定義

```c
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	4000U
#define HYSTART_DELAY_MAX	16000U
#define HYSTART_LOW_WINDOW	16
#define HYSTART_ACK_DELTA_US	2000U
```

- `HYSTART_LOW_WINDOW`（`16`）: HyStart の検出器を有効化する最小の `snd_cwnd`。
- `HYSTART_ACK_DELTA_US`（`2000U` = $$2\text{ ms}$$）: 同じ ACK Train に属するとみなす連続 ACK 間の最大間隔。
- `HYSTART_MIN_SAMPLES`（`8`）: Delay 増加検出を行うために必要なラウンド内の最小 RTT サンプル数。
- `HYSTART_DELAY_MIN`（`4000U` = $$4\text{ ms}$$）/ `HYSTART_DELAY_MAX`（`16000U` = $$16\text{ ms}$$）: 許容キューイング遅延 `delay_min >> 3` の下限と上限。

### 2. `hystart_reset()` — 新しいラウンドの開始

```c
static __always_inline void hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 now_us = tp->tcp_mstamp;

	ca->round_start = ca->last_ack = now_us;
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0U;
	ca->sample_cnt = 0;
}
```

- ソケット初期化時（`bpf_cubic6_init()`）、RTO ロス時（`bpf_cubic6_state()` で `new_state == TCP_CA_Loss` のとき）、および各ラウンドの切り替わり時（`hystart_update()` 内）に呼ばれます。
- `ca->round_start` と `ca->last_ack` を現在の `tp->tcp_mstamp` で揃え、`ca->end_seq = tp->snd_nxt` でこのラウンドの終端シーケンス番号をマークします。
- `ca->curr_rtt = ~0U` とすることで、次の最初の RTT サンプルで確実に `ca->curr_rtt = delay` へ更新されます。

### 3. `hystart_update()` — ラウンド管理と 2 つの検出器

```c
static __always_inline void hystart_update(struct sock *sk, __u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 now_us = tp->tcp_mstamp;

	if (after(tp->snd_una, ca->end_seq))
		hystart_reset(sk);

	if (tp->snd_cwnd < HYSTART_LOW_WINDOW)
		return;

	/* Detector 1: HYSTART_ACK_TRAIN */
	if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US) {
		ca->last_ack = now_us;
		if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
			ca->found = 1;
			tp->snd_ssthresh = tp->snd_cwnd;
		}
	}

	/* Detector 2: HYSTART_DELAY */
	if (ca->curr_rtt > delay)
		ca->curr_rtt = delay;

	if (ca->sample_cnt < HYSTART_MIN_SAMPLES)
		ca->sample_cnt++;

	if (ca->sample_cnt >= HYSTART_MIN_SAMPLES) {
		if (ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)) {
			ca->found = 1;
			tp->snd_ssthresh = tp->snd_cwnd;
		}
	}
}
```

- まず `after(tp->snd_una, ca->end_seq)` で前ラウンドのパケットがすべて ACK されたかを確認し、真であれば `hystart_reset(sk)` で新しいラウンドを開始します。
- 次に `tp->snd_cwnd < HYSTART_LOW_WINDOW`（16）であれば早期リターンします。
- 続く Detector 1（`HYSTART_ACK_TRAIN`）と Detector 2（`HYSTART_DELAY`）のいずれかで飽和条件が成立すると、`ca->found = 1` と `tp->snd_ssthresh = tp->snd_cwnd` を設定します。

### 4. `bpf_cubic6_acked()` — `.pkts_acked` フックからの呼び出し

```c
SEC("struct_ops")
void BPF_PROG(bpf_cubic6_acked, struct sock *sk, const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 delay;

	if (sample->rtt_us < 0)
		return;

	delay = sample->rtt_us ? (__u32)sample->rtt_us : 1U;

	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH &&
	    tcp_in_slow_start(tp) && !ca->found)
		hystart_update(sk, delay);
}
```

- TCP コアは ACK 受信時に `.pkts_acked` を `.cong_avoid` より **前** に呼び出します。
- そのため、`hystart_update()` が `tp->snd_ssthresh = tp->snd_cwnd` に引き下げると、直後に呼ばれる `bpf_cubic6_cong_avoid()` では `tcp_in_slow_start(tp)` が即座に偽となり、その ACK からシームレスに CUBIC の Congestion Avoidance（`bictcp_update()` + `tcp_cong_avoid_ai()`）へ切り替わります。

---

## `packetdrill` テストの解説 (`test.pkt`) と実行方法

### `test.pkt` のシナリオ解説

`test.pkt` は、初期 Slow Start 中に RTT が $$20\text{ ms}$$ から $$35\text{ ms}$$ へ増加したとき、パケットロスなしで Slow Start を早期脱出できるかを検証します。

1. **接続確立（0.000s 〜 0.020s）**:
   - `net.ipv4.tcp_congestion_control=bpf_cubic6` を設定し、MSS 1012 バイトで 3-way ハンドシェイクを行います。
   - `bpf_cubic6_init()` により `hystart_reset()` が呼ばれ、`end_seq = 1` が設定されます。
2. **Round 1: 最小 RTT `delay_min = 20 ms` の確立と `cwnd: 10 -> 20`（0.020s 〜 0.040s）**:
   - アプリケーションが `write(4, ..., 30360)` で 30 セグメント（$$30 \times 1012 = 30360$$ バイト）を書き込みます。初期 `snd_cwnd = 10` のため、まず 10 セグメント（シーケンス番号 `1:10121`）が送信されます。
   - `+.020`（$$20\text{ ms}$$ 後）に `ack 10121` が返ります。
     - `bpf_cubic6_acked()` で `delay_min = 20000`（$$20\text{ ms}$$）が記録されます。
     - `hystart_update()` の冒頭で `after(10121, 1)` が真になるため、`hystart_reset()` が呼ばれて `end_seq = 10121` に更新されます（この時点ではまだ `snd_cwnd == 10 < 16` のため検出器はスキップされます）。
     - 直後の `bpf_cubic6_cong_avoid()` で `snd_cwnd` が `10` から `20` に倍増し、残りの 20 セグメント（`10121:30361`）が送信されます。
     - `tcpi_snd_cwnd == 20` かつ `tcpi_snd_ssthresh == 0x7fffffff` であることをアサートします。
3. **Round 2: キューイング遅延（$$35\text{ ms}$$）の発生と 8 サンプル目の HyStart 発火（0.075s）**:
   - `+.035`（$$35\text{ ms}$$ 後）に Round 2 の 1 個目の ACK（`ack 11133`）が到着します。
     - `after(11133, 10121)` が真となり、`hystart_reset()` で新しいラウンドが開始されます（`end_seq = 30361`、`sample_cnt = 0`、`curr_rtt = ~0U`）。
     - `snd_cwnd` は `20`（$$\ge 16$$）なので検出器が動き始め、`curr_rtt = 35000`、`sample_cnt = 1` になります。まだ 1 サンプル目なので `tcpi_snd_ssthresh == 0x7fffffff` のままです。
   - 続いて 7 個の 1-MSS ACK（`ack 12145` 〜 `ack 18217`）が `+0.000` で連続到着し、`sample_cnt` が `2` から `8` まで増加します（なお、`+.035` の最初の ACK で `round_start` が現在時刻にリセットされた後、後続の ACK はすべて `+0.000` で到着するため `now_us - round_start == 0 < 10000` となり、ACK Train 検出器はまだ発火しません）。
   - 8 サンプル目に達した `ack 18217` および続く `ack 19229` の時点で、`sample_cnt >= 8` かつ `curr_rtt`（$$35\text{ ms}$$）$$\ge$$ `delay_min + 4 ms`（$$20\text{ ms} + 4\text{ ms} = 24\text{ ms}$$）が成立し、**Detector 2（`HYSTART_DELAY`）が発火** します。
   - `tp->snd_ssthresh = tp->snd_cwnd` が実行され、`assert tcpi_snd_ssthresh == tcpi_snd_cwnd` と `assert tcpi_snd_ssthresh < 0x7fffffff` が PASS します。

### なぜ `starter.bpf.c` は失敗し、`solution.bpf.c` は PASS するのか？

- **`starter.bpf.c`**: `hystart_update()` が空のスタブになっているため、Round 2 で $$35\text{ ms}$$ の遅延サンプルが 8 個以上届いても `tp->snd_ssthresh` が更新されず初期値 `0x7fffffff` のまま残り、49 行目の `assert tcpi_snd_ssthresh == tcpi_snd_cwnd` で FAIL します。
- **`solution.bpf.c`**: Round 2 の 8 サンプル目で Delay 増加検出器が `ca->found = 1` と `tp->snd_ssthresh = tp->snd_cwnd` を設定するため、すべてのアサートに PASS します。

### テストの実行コマンド

```sh
../../run_test.sh cubic/step04-hystart starter
../../run_test.sh cubic/step04-hystart solution
```
