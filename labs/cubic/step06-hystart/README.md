# Step 6: HyStart (ACK Train と Delay 増加による Slow Start 早期脱出)

## 学習ゴール

Step 5 までで、3 次関数（CUBIC 曲線）・Fast Convergence・Reno-friendly 領域・アイドル明けの Epoch シフトを備えた完全な輻輳回避（Congestion Avoidance）を実装しました。
しかし、ここまでの CUBIC には **最初の Slow Start でネットワークのバッファをパンクさせてしまう** という深刻な課題があります。

従来の Reno や標準 TCP の Slow Start は、1 RTT ごとに `snd_cwnd` を倍々に増やします（指数関数的増加）。この倍々ゲームは、ネットワークの帯域遅延積（BDP）を超えても止まらず、スイッチやルータのボトルネックバッファが溢れて **パケット破棄（ロス）が起きるまで加速を続けます**。
特に高 BDP（広帯域・長遅延）回線やバッファが浅いスイッチでは、Slow Start の末期に in-flight パケット数が BDP の最大 2 倍に達し、大量のパケットが一度にドロップして過酷なロス回復（複数タイムアウトや長い PRR）に陥ってしまいます。

**HyStart（Hybrid Slow Start）** は、パケットロスが起きる **前** にネットワーク飽和の兆候を検知し、安全に Slow Start を終了して Congestion Avoidance にソフトランディングさせるアルゴリズムです。
本ステップでは、HyStart の 2 つの中核検出器である **ACK Train 検出** と **Delay 増加（Delay Increase）検出** を `struct tcp_congestion_ops` の `.pkts_acked` フックに BPF で実装します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

`starter.bpf.c` を開く前に、以下の 4 つの問いを自分の頭で考えてみてください。

### Q1. なぜ最初の Slow Start をパケットロスまで続けさせてはいけないのか？（Slow Start オーバーシュート問題）

Slow Start は 1 RTT で窓が 2 倍になります。なぜ「ロスが起きたら半減（または 0.7 倍）にして CA に入る」という従来のやり方ではダメなのでしょうか？

<details>
<summary>ヒントと答え</summary>

Slow Start 中、送信側は ACK 1 個を受信するたびに 2 個の新しいパケットを送信します。
ボトルネックのパイプ（BDP）が満杯になった瞬間の窓サイズを $W$ とすると、次の 1 RTT で送信側は $2W$ 個のパケットを送り込もうとします。
もしルータのバッファ容量が $W$ より小さければ、溢れた $W$ 個近くのパケットが一斉に破棄されます（テールドロップ）。

これほどの大量バーストロスが起きると、SACK による Fast Recovery でも修復しきれず、RTO（再送タイムアウト）が発生して `cwnd = 1` まで叩き落とされます。結果として、接続開始直後に長い通信断とスループット低下を招きます。
HyStart は「バッファが溢れる前（キューが溜まり始めた段階）」で `ssthresh = cwnd` に設定して Slow Start を止めることで、**ロス 0 で Congestion Avoidance に移行する** ことを目指します。

</details>

### Q2. なぜ 2 つの独立した検出器（ACK Train と Delay Increase）を組み合わせるのか？

HyStart には「ACK Train」と「Delay 増加」の 2 つの検出器があります。どちらか一方だけでは不十分なのはなぜでしょうか？

<details>
<summary>ヒントと答え</summary>

2 つの検出器は互いの弱点を補い合っています。

- **ACK Train 検出**: ボトルネック帯域が飽和すると、ボトルネックルータはパケットを回線速度（ボトルネック帯域幅）の間隔で順次送出します。その結果、受信側から戻ってくる ACK 列もボトルネック間隔でぎっしり詰まった「トレイン（列車）」になります。この密な ACK 列がラウンド開始から $\text{delay\_min} / 2$ 以上続けば、送信したデータが BDP に達したと判定できます。しかし、低帯域回線や ACK 間引き・統合（LRO/GRO、ACK フィルタリング）がある環境では、ACK の間隔が乱れてトレインが途切れることがあります。
- **Delay 増加検出**: ボトルネックバッファにパケットが並び始めると、往復時間（RTT）が伸長します。ラウンド内の最小 RTT が接続の最小 RTT（`delay_min`）より閾値以上大きくなれば、キューイング遅延の発生を確実に検知できます。しかし、無線リンク（Wi-Fi/セルラー）の伝搬ジッタや一時的なスケジューリング遅延による誤検知のリスクがあります。

このように、帯域飽和の「流量シグナル（ACK 間隔）」とバッファ蓄積の「遅延シグナル（RTT 伸長）」を両面から監視することで、多様なネットワーク環境で高精度な判定を実現しています。

</details>

### Q3. なぜラウンド境界の判定（`after(tp->snd_una, ca->end_seq)`）を `cwnd < 16` のチェックより前に置くのか？

`hystart_update()` では、`after(tp->snd_una, ca->end_seq)` によるラウンドのリセットを、`tp->snd_cwnd < HYSTART_LOW_WINDOW`（16）のチェックより **前** に実行します。かつての Linux カーネルでこの順序が逆だった際、どのようなバグが発生していたでしょうか？

<details>
<summary>ヒントと答え</summary>

Linux カーネルのコミット `25c1a9ca53db` で修正された有名なバグです。
もし `cwnd < 16` のチェックをラウンドリセットより前に置くと、接続開始時の `cwnd`（初期窓 10 など）が 16 未満の間は `hystart_update()` が即座にリターンしてしまい、ラウンド境界 `end_seq` が一切更新されません。

その結果、`cwnd` が 16 に達した最初のラウンドにおいて、`end_seq` が接続開始直後の古いシーケンス番号のまま残ってしまいます。これによってラウンドが異常に引き伸ばされ、古いタイムスタンプや RTT サンプルが混入して、意図しないタイミングで誤検出が起きる原因となっていました。
`cwnd` の大きさにかかわらず毎回の ACK で正しくラウンドを進め、`cwnd >= 16` に達したラウンドからクリーンな状態で検出器を走らせるために、ラウンド更新を先頭に配置する必要があります。

</details>

### Q4. Delay 増加検出で、なぜ「8 サンプル」を集め、閾値に `clamp(delay_min >> 3, 4ms, 16ms)` を使うのか？

Delay 増加検出では、1 サンプルでも RTT が伸びたら即座に判定するのではなく、最低 8 サンプル（`HYSTART_MIN_SAMPLES = 8`）の最小値 `curr_rtt` を監視し、基準値 `delay_min` に動的なクッションを加えます。なぜ固定値ではなくこのような設計になっているのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **最低 8 サンプルの最小値（`curr_rtt`）**: 単一の ACK パケットは OS のタスクスイッチや NIC の割り込み統合（Interrupt Moderation）による瞬間的なジッタを含みます。8 個以上のサンプルのうちの「最小値」を取ることで、一過性の外れ値ノイズを除去し、真のルータキュー遅延を抽出します。
- **`delay_min >> 3`（12.5% の動的閾値）**: RTT が 100 ms の長距離回線では、12.5% は 12.5 ms です。高遅延回線ではキューイング遅延の絶対値も大きくなりやすいため、比率で閾値を広げることで早すぎる脱出（アンダーシュート）を防ぎます。
- **`clamp(..., 4000U, 16000U)`（4 ms〜16 ms クランプ）**:
  - **下限 4 ms**: データセンターや LAN などの超低 RTT（1 ms 以下）では、`1 ms >> 3 = 125 us` となり、GRO/TSO のパケット集約遅延だけで簡単に閾値を超えて誤検出してしまいます。そのため最低でも 4 ms のマージンを保証します。
  - **上限 16 ms**: 衛星通信など超高 RTT（600 ms）では、`600 ms >> 3 = 75 ms` にもなり、75 ms もバッファにキューが溜まるのを待っていてはバッファブロートやパケットロスを防げません。そのため上限を 16 ms に抑えています。
  （この 4 ms / 16 ms / 8 サンプルの定数は、のちに標準化された RFC 9406 HyStart++ にもそのまま取り入れられました）。

</details>

---

## 実装ステップと解説 (`solution.bpf.c`)

### 1. 定数とデータ構造

```c
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	4000U	/* 4 ms */
#define HYSTART_DELAY_MAX	16000U	/* 16 ms */
#define HYSTART_LOW_WINDOW	16
#define HYSTART_ACK_DELTA_US	2000U	/* 2 ms: トレインとみなす ACK 間隔の上限 */
```

`struct bictcp` の HyStart 関連フィールド（`vmlinux.h` 既定）：
- `round_start`: 現在のラウンドが開始された時刻（マイクロ秒）
- `end_seq`: 現在のラウンドの終端シーケンス番号（`tp->snd_nxt`）
- `last_ack`: 直近のトレインに属する ACK を受信した時刻（マイクロ秒）
- `curr_rtt`: 現在のラウンドで観測された最小 RTT（マイクロ秒、初期値 `~0U`）
- `sample_cnt`: 現在のラウンドで収集した有効 RTT サンプル数
- `found`: HyStart の脱出条件が満たされたかを示すフラグ（1: 検出済み）

### 2. `hystart_reset()`: ラウンドの初期化

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

- `now_us`: カーネルの TCP タイムスタンプ `tp->tcp_mstamp`（マイクロ秒単位）。
- `end_seq = tp->snd_nxt`: 現在送信済みの最後尾パケットが ACK されたら、送信側が送った 1 RTT 分のデータがすべて相手に届いたことになります。
- `curr_rtt = ~0U`: 無限大（`0xFFFFFFFF`）で初期化することで、最初のサンプルとの比較で「未設定なら」の条件分岐を省略できます。

### 3. `hystart_update()`: 2 つの検出器

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

- **ラウンド境界チェック**: `after(tp->snd_una, ca->end_seq)` が真（前ラウンドの `end_seq` を超える新しい ACK が届いた）なら、`hystart_reset(sk)` を呼び出して新ラウンドを開始します。
- **下限チェック**: `tp->snd_cwnd < HYSTART_LOW_WINDOW`（16）なら早期リターンします。
- **Detector 1（ACK Train）**:
  - 前回の ACK から 2 ms 以内（`now_us - ca->last_ack <= 2000`）であればトレインの継続とみなして `last_ack = now_us` に更新します。2 ms を超えて隙間が空いた場合、そのラウンドでは `last_ack` が更新されなくなり、トレイン検出は失効します。
  - トレインの継続時間（`now_us - ca->round_start`）が `ca->delay_min >> 1`（min RTT の半分）に達したら、`ca->found = 1; tp->snd_ssthresh = tp->snd_cwnd;` で Slow Start を即座に終了します。
- **Detector 2（Delay Increase）**:
  - 届いた RTT サンプル `delay` で `ca->curr_rtt` を更新します。
  - サンプル数が 8 個に達したら、`ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, 4000U, 16000U)` を判定します。
  - キューイング遅延の閾値を超えていれば `ca->found = 1; tp->snd_ssthresh = tp->snd_cwnd;` で Slow Start を終了します。

### 4. `bpf_cubic6_acked()`: フックからの呼び出し

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

- 初期 Slow Start 中（`tp->snd_ssthresh == TCP_INFINITE_SSTHRESH` かつ `tcp_in_slow_start(tp)` かつ `!ca->found`）のときのみ `hystart_update(sk, delay)` を呼び出します。
- HyStart が一度 `ca->found = 1` にセットして `snd_ssthresh = snd_cwnd` に下げると、`tcp_in_slow_start(tp)` が偽になり、以降の ACK では `hystart_update()` が呼ばれなくなります。

---

## packetdrill テストの解説 (`test.pkt`)

`test.pkt` では、Delay 増加検出器が Slow Start をロスなしで早期終了させる振る舞いを精密に検証します。

```text
0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_cubic6`
```

1. **3 ウェイハンドシェイク**:
   - MSS 1012、受信ウィンドウ 65535 で接続を確立します。
2. **Round 1 (基準 delay_min の確立と窓成長)**:
   - 送信側が 30 セグメント（30360 バイト）を `write()` します。
   - 初期窓 `cwnd = 10` のため、最初の 10 セグメント（1:10121）が送信されます。
   - 20 ms 後（`+.020`）に ACK 10121 が返ります。これにより `delay_min = 20000`（20 ms）が記録されます。
   - Slow Start により `cwnd` は 10 から 20（`>= 16`）に倍増し、残りの 20 セグメント（10121:30361）が一気に送信されます。
   - `assert tcpi_snd_cwnd == 20`、`assert tcpi_snd_ssthresh == 0x7fffffff`。
3. **Round 2 (遅延増加による HyStart 発動)**:
   - 35 ms 後（`+.035`、ネットワークキューによる遅延インフレーション）に Round 2 の最初の ACK（ack 11133）が到着します。
   - `after(11133, 10121)` により新ラウンドが開始され、`sample_cnt = 0`、`round_start` が更新されます。
   - 続く 8 個の ACK（ack 12145〜19229）がバーストで到着（`+0.000`）します。
   - 9 個目の ACK（ack 19229）が届いた時点で `sample_cnt >= 8` となり、ラウンドの最小 RTT（35 ms）が閾値 `delay_min + 4ms`（20 ms + 4 ms = 24 ms）を上回っているため、Delay 増加検出器が発火します！
   - `tp->snd_ssthresh = tp->snd_cwnd`（28）が実行され、`tcpi_snd_ssthresh == tcpi_snd_cwnd` および `tcpi_snd_ssthresh < 0x7fffffff` が成立します。
   - パケットロスは 1 つも発生していません！

---

## 実行方法

### 1. starter（スケルトン）がアサートで失敗することを確認

```sh
../../run_test.sh cubic/step06-hystart starter
```

`starter.bpf.c` では `hystart_update()` が空のスタブであるため、`ssthresh` は初期値 `0x7fffffff` のまま下がりません。`test.pkt` のアサートで失敗します。

### 2. 模範解答（solution）を実行して PASS することを確認

```sh
../../run_test.sh cubic/step06-hystart solution
```

HyStart が正しく発動し、`=== [PASS] cubic/step06-hystart (solution) ===` と表示されます。
