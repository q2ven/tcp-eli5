# 16. ハンズオン: BPF CC と packetdrill で作る BBR

リポジトリの `labs/bbr/` ディレクトリにある全 6 ステップのハンズオン教材です。`.cong_control` フックと `sk_pacing_rate` 制御から始め、`BtlBw` / `RTprop` フィルタ、`STARTUP` → `DRAIN` → `PROBE_BW` → `PROBE_RTT`、そして Loss 復帰と `extra_acked` までを 1 ステップずつ実装・検証します。


## Step 1: .cong_control フックと Delivery Rate からの Pacing Rate 計算

### 学習ゴール

このステップでは、BPF `struct_ops`（`struct tcp_congestion_ops`）を用いて BBR（Bottleneck Bandwidth and RTT）の基礎となる **Pacing Rate（送信レート）制御** を実装します。

- Reno や CUBIC が用いる `.cong_avoid` フックと、BBR が用いる `.cong_control` フックの決定的な違いを理解します。
- `.init` フックでソケットの内部ペーシング（`SK_PACING_NEEDED`）を有効化し、初期 `cwnd` と初期 RTT から初期ペーシングレートを設定します。
- `.cong_control` フックに渡される `struct rate_sample` から瞬間配送レート（Delivery Rate）を計算し、固定小数点のゲイン（`BBR_HIGH_GAIN`）を掛けて `sk->sk_pacing_rate` を更新します。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

いきなりコードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

#### Q1. どのフックを使うべきでしょうか？

Reno や CUBIC では `.cong_avoid(sk, ack, acked)` と `.ssthresh(sk)` を中心に実装しました。しかし BBR では `.cong_avoid` を使いません。`struct tcp_congestion_ops` のどのコールバックを使うべきでしょうか？また、そのコールバックを定義するとカーネルの `tcp_cong_control()` の挙動はどう変わるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **使うべきフック**: `.cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)` です。
- **カーネルの挙動の変化**: `tcp_cong_control()` は `icsk_ca_ops->cong_control` が定義されている場合、それを呼び出して即座に `return` します。つまり、カーネル標準の `tcp_cwnd_reduction()`・`tcp_cong_avoid()`・`tcp_update_pacing_rate()` がすべてバイパスされ、輻輳ウィンドウ（`snd_cwnd`）とペーシングレート（`sk_pacing_rate`）の制御が完全に `.cong_control` に委ねられます。
- なお、`tcp_validate_congestion_control()` の登録チェックを通すため、`.ssthresh` と `.undo_cwnd` も必ず定義する必要があります。

</details>

#### Q2. TCP の内部ペーシングを有効化するには何を書き換えますか？

`sk->sk_pacing_rate` にバイト/秒のレートを書き込むだけでは、FQ（Fair Queueing）qdisc を使っていない環境ではパケットがペーシングされません。TCP スタック自身の高分解能タイマー（hrtimer）による内部ペーシングを有効化するには、`.init(sk)` で `struct sock` のどのフィールドに何を設定する必要があるでしょうか？

<details>
<summary>ヒントと答え</summary>

- `sk->sk_pacing_status = SK_PACING_NEEDED;` を設定します。
- BPF の `tcp_congestion_ops` ベリファイア（`bpf_tcp_ca_btf_struct_access()`）は、`struct sock` の `sk_pacing_status` と `sk_pacing_rate` への直接書き込みを許可しています。

</details>

#### Q3. `struct rate_sample` からどのようにバイト/秒の配送レートを計算しますか？

`.cong_control` の第 4 引数 `const struct rate_sample *rs` には、直近の観測区間における配送パケット数 `rs->delivered` と経過時間（マイクロ秒）`rs->interval_us` が格納されています。MSS（1 セグメントあたりのペイロードバイト数 `tp->mss_cache`）を使って、配送レート（bytes/sec）を求める式を整数演算で組み立ててみましょう。

<details>
<summary>ヒントと答え</summary>

- 1 秒は `USEC_PER_SEC`（`1,000,000` マイクロ秒）なので、瞬間配送レート（bytes/sec）は次の式で求まります。
  ```c
  rate = (__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC;
  rate = div64_u64(rate, (__u64)rs->interval_us);
  ```
- 先に分子を 64 ビット整数（`__u64`）で掛け算してから `rs->interval_us` で割ることで、整数除算による桁落ち（truncation）を防ぎます。
- STARTUP フェーズのペーシングゲイン（`2 / ln(2) ~= 2.885`）を固定小数点（`BBR_UNIT = 1024`, `BBR_HIGH_GAIN = 2885`）で掛けるには、`(rate * BBR_HIGH_GAIN) >> BBR_SCALE` を計算します。

</details>

#### Q4. どのようなエッジケース（無効なサンプルやゼロ除算）に注意すべきでしょうか？

すべての ACK で有効な `rate_sample` が得られるとは限りません。また、`.init(sk)` が呼ばれた時点でまだ RTT サンプル（`tp->srtt_us`）が存在しない場合もあります。どの値をチェックしてガードを入れるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- `.cong_control` では、`rs->delivered < 0 || rs->interval_us <= 0` のサンプルは無効な観測値であるため、何もせず `return` します（これにより `rs->interval_us == 0` によるゼロ除算も防げます）。
- `.init` では、`tp->srtt_us`（1/8 マイクロ秒単位の平滑化 RTT）が `0` の場合、デフォルトとして 1 ミリ秒（`USEC_PER_MSEC = 1000` us）を使います。さらに `tp->srtt_us >> 3` が `0` になる極端なケースにも備えて `if (!rtt_us) rtt_us = 1;` でゼロ除算を防止します。

</details>

---

### 実装ステップと解説

#### 1. 固定小数点定数の定義

BPF プログラム内では浮動小数点演算が使えないため、BBR のゲイン計算には 2 のべき乗を基準とした固定小数点演算を用います。

- `BBR_SCALE = 10`（`BBR_UNIT = 1 << 10 = 1024`）を `1.0x` の基準とします。
- STARTUP フェーズのゲイン $2 / \ln(2) \approx 2.885$ は、`BBR_HIGH_GAIN = 2885`（$2885 / 1024 \approx 2.817$）として表現します。

#### 2. `bpf_bbr1_init()` — ペーシングの有効化と初期レート計算

コネクション確立時に呼ばれる `bpf_bbr1_init()` では、最初の ACK から `rate_sample` が届くまでの間にバースト送信が起きないよう、初期 `snd_cwnd` と初期 RTT から初期ペーシングレートを設定します。

- `tp->srtt_us` は 3-way ハンドシェイクの SYN-ACK RTT から計測されており、実際のマイクロ秒値を得るには 3 ビット右シフト（`tp->srtt_us >> 3`）します。未計測（`0`）の場合は 1 ms（`USEC_PER_MSEC`）を仮定します。
- 初期帯域幅 `tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC / rtt_us` に `BBR_HIGH_GAIN` を掛けて `sk->sk_pacing_rate` に書き込み、`sk->sk_pacing_status = SK_PACING_NEEDED` で内部ペーシングを有効化します。

#### 3. `bpf_bbr1_cong_control()` — Delivery Rate からの Pacing Rate 更新

ACK ごとに呼ばれる `bpf_bbr1_cong_control()` では、カーネルの `tcp_rate_gen()` が計測した `struct rate_sample *rs` を読み取ります。

1. `rs->delivered < 0 || rs->interval_us <= 0` の場合は無効サンプルとして無視します。
2. 配送バイト数 `(__u64)rs->delivered * tp->mss_cache` に `USEC_PER_SEC` を掛け、`div64_u64()` で `rs->interval_us` で割ることで、瞬間配送レート（bytes/sec）を求めます。
3. `(rate * BBR_HIGH_GAIN) >> BBR_SCALE` により STARTUP ゲインを適用し、`sk->sk_pacing_rate` を更新します。

#### 4. `bpf_bbr1_ssthresh()` と `bpf_bbr1_undo_cwnd()`

カーネルの `tcp_validate_congestion_control()` は、`.ssthresh` と `.undo_cwnd` が未定義の `tcp_congestion_ops` を `-EINVAL` で拒否します。このステップではまだロス回復時の `cwnd` 保存を扱わないため、それぞれ現在の `tp->snd_ssthresh` と `tp->snd_cwnd` をそのまま返します。

---

### packetdrill テストの解説

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step01-pacing-rate/test.pkt) は以下のシナリオで `bpf_bbr1` の動作を検証します。

1. **3-way ハンドシェイク（RTT = 100 ms, MSS = 1000 B）**:
   - クライアントが `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>` で SYN を送り、12 バイトの TCP タイムスタンプオプションを差し引いたペイロード MSS（`tp->mss_cache`）は `1000` バイトになります。
   - SYN-ACK に対する ACK を `+0.10`（100 ms 後）に返すことで、`tp->srtt_us >> 3` は `100,000` us（100 ms）になります。
   - `bpf_bbr1_init()` により、初期ペーシングレートは `10 * 1000 B / 0.1 s * (2885 / 1024) = 281,738 B/s` に設定されます（`200000 < tcpi_pacing_rate < 400000` をアサート）。
2. **10 セグメント（10,000 バイト）の送信と 10 ms 後の ACK**:
   - サーバーが `write(4, ..., 10000)` を実行して 10 セグメントを送信し、`+0.01`（10 ms 後）にクライアントが `ack 10001` を返します。
   - この ACK により `rs->delivered = 10`、`rs->interval_us = 10000`（10 ms）の `rate_sample` が生成され、配送レートは `10,000 B / 0.01 s = 1,000,000 B/s`（1 MB/s）となります。
   - `bpf_bbr1_cong_control()` がゲイン `2885 / 1024` を適用し、`tcpi_pacing_rate` が約 `2,817,382 B/s`（`> 500000` かつ `2000000 < tcpi_pacing_rate < 3500000`）に更新されたことをアサートします。

---

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step01-pacing-rate starter
## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step01-pacing-rate solution
```


### `labs/bbr/step01-pacing-rate/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1 (Starter): .cong_control hook and Pacing Rate calculation
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BBR_SCALE	10
#define BBR_UNIT	(1 << BBR_SCALE)	/* 1024 */
#define BBR_HIGH_GAIN	2885			/* ~2.885x (2/ln(2)) STARTUP gain */

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 rate;

	if (!rtt_us)
		rtt_us = 1;

	/* Initialize pacing rate: high_gain * init_cwnd * mss / initial_rtt */
	rate = (__u64)tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, rtt_us);
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);

	(void)tp;
	/*
	 * TODO:
	 * 1. Check if the rate sample is valid:
	 *    if (rs->delivered < 0 || rs->interval_us <= 0) return;
	 * 2. Calculate instantaneous delivery rate in bytes/sec:
	 *    rate = (u64)rs->delivered * tp->mss_cache * USEC_PER_SEC / rs->interval_us
	 *    (Hint: use div64_u64(rate, (__u64)rs->interval_us))
	 * 3. Multiply by BBR_HIGH_GAIN and shift right by BBR_SCALE:
	 *    rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;
	 * 4. Write the result to sk->sk_pacing_rate.
	 */
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr1 = {
	.name		= "bpf_bbr1",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr1_init,
	.cong_control	= (void *)bpf_bbr1_cong_control,
	.ssthresh	= (void *)bpf_bbr1_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr1_undo_cwnd,
};
```


### `labs/bbr/step01-pacing-rate/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 1: Verify .cong_control hook and pacing rate calculation from rate_sample
0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr1`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial RTT = 100 ms (so initial pacing rate is ~281 KB/s)
// MSS = 1012 -> payload MSS = 1000 bytes (with 12-byte TCP timestamps)
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.10 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 200 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Verify initial cwnd=10 and initial pacing rate (~281,738 B/s from 100 ms SYN RTT)
+0.00 %{
assert tcpi_snd_cwnd == 10, f"expected cwnd 10, got {tcpi_snd_cwnd}"
assert 200000 < tcpi_pacing_rate < 400000, f"expected initial pacing_rate ~281KB/s, got {tcpi_pacing_rate}"
}%

// Send 10 segments (10,000 bytes) and ACK after 10 ms (+0.010):
// Delivery rate = 10 * 1000 B / 0.010 s = 1,000,000 B/s
// Pacing rate = 1,000,000 * 2885 / 1024 = ~2,817,382 B/s
+0.00 write(4, ..., 10000) = 10000
+0.00 > P. 1:10001(10000) ack 1 <nop,nop,TS val 200 ecr 200>
+0.01 < . 1:1(0) ack 10001 win 257 <nop,nop,TS val 210 ecr 200>

+0.00 %{
assert tcpi_pacing_rate > 500000, f"expected pacing_rate > 500000, got {tcpi_pacing_rate}"
assert 2000000 < tcpi_pacing_rate < 3500000, f"expected pacing_rate ~2.82MB/s, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step01-pacing-rate/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: .cong_control hook and Pacing Rate calculation from Delivery Rate
 *
 * Unlike loss-based congestion control (Reno/CUBIC) which uses .cong_avoid,
 * BBR uses .cong_control(sk, ack, flag, rs) to receive a struct rate_sample
 * on every ACK and directly drive sk->sk_pacing_rate.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BBR_SCALE	10
#define BBR_UNIT	(1 << BBR_SCALE)	/* 1024 */
#define BBR_HIGH_GAIN	2885			/* ~2.885x (2/ln(2)) STARTUP gain */

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 rate;

	if (!rtt_us)
		rtt_us = 1;

	/* Initialize pacing rate: high_gain * init_cwnd * mss / initial_rtt */
	rate = (__u64)tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, rtt_us);
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u64 rate;

	/* Ignore invalid delivery rate samples */
	if (rs->delivered < 0 || rs->interval_us <= 0)
		return;

	/* Instantaneous delivery rate (bytes/sec):
	 *   bw = delivered * mss * USEC_PER_SEC / interval_us
	 */
	rate = (__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, (__u64)rs->interval_us);

	/* Apply STARTUP pacing gain (~2.885x) */
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr1 = {
	.name		= "bpf_bbr1",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr1_init,
	.cong_control	= (void *)bpf_bbr1_cong_control,
	.ssthresh	= (void *)bpf_bbr1_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr1_undo_cwnd,
};
```

</details>

---

## Step 2: BtlBw (最大帯域フィルタ) と RTprop (最小 RTT) による BDP 計算

### 学習ゴール

このステップでは、Step 1 のペーシングレート制御を発展させ、BBR の中核モデルである **BtlBw（ボトルネック帯域幅: 最大帯域フィルタ）** と **RTprop（往復伝播遅延: 最小 RTT フィルタ）** を追跡し、両者の積である **BDP（帯域遅延積: Bandwidth-Delay Product）** から `snd_cwnd` と `sk_pacing_rate` を決定する仕組みを実装します。

- `inet_csk_ca(sk)` のプライベート領域（`struct bpf_bbr2_ca`）に、フローごとの BBR モデル状態（`min_rtt_us`, `max_bw`, `round_cnt`, `next_rtt_delivered`）を保持する方法を理解します。
- なぜ RTT は **最小値フィルタ（min-filter）** で、配送レート（帯域）は **最大値フィルタ（max-filter）** で追跡するのかという物理的意味を理解します。
- アプリケーションの送信データ不足（`rs->is_app_limited`）による低いレートサンプルが `max_bw` を引き下げてしまう問題と、そのフィルタ条件を理解します。
- 固定小数点単位 `BW_UNIT`（`1 << 24`）で表された `max_bw` と `min_rtt_us` から BDP（パケット数）を計算する `bbr_bdp()` を実装し、`tp->snd_cwnd = max(bdp + 4U, 4U)` を設定します。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書き始める前に、以下の 4 つの問いを自分の頭で考えてみましょう。

#### Q1. なぜ RTT は「最小値」を取り、帯域（Delivery Rate）は「最大値」を取るのでしょうか？

Step 1 のように直近の ACK サンプルで毎回レートや RTT を上書きすると、何が起きるでしょうか？ネットワーク経路の物理的な **往復伝播遅延（RTprop）** と **ボトルネック帯域（BtlBw）** を推定するために、それぞれ最小値・最大値のどちらを記録すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **RTprop（最小 RTT フィルタ）**: 観測される RTT は「物理的な伝播遅延（RTprop）＋ボトルネックキューでの待ち時間（Queueing Delay）」の和です。キューが空のときにだけ真の伝播遅延が観測され、キューが溜まると RTT は必ず大きくなるため、**最小値（min-filter）** を取ることでキュー遅延を除いた `min_rtt_us` を推定できます。
- **BtlBw（最大帯域フィルタ）**: 観測される配送レート（`rs->delivered / rs->interval_us`）は、物理リンクのボトルネック容量を超えることはありませんが、送信量が少ないときや他フローとの一時的な競合で容易にボトルネック容量を下回ります。したがって、達成できた **最大値（max-filter）** を取ることでボトルネック帯域 `max_bw` を推定します。

</details>

#### Q2. アプリケーションが少しずつしかデータを送らない（App-Limited）とき、`max_bw` はどう扱うべきでしょうか？

高帯域で 10 パケットを一気に送って `max_bw` を計測したあと、アプリケーションがしばらく休止し、2 パケットだけをゆっくり送信したとします。このときの `rate_sample`（`rs->is_app_limited == true`）の配送レートは非常に低くなります。どのような条件のときだけ `ca->max_bw` を更新すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- パイプ（ネットワーク経路）を満たすほど送信していない期間（`rs->is_app_limited != 0`）の配送レートは、ネットワークの上限ではなく「アプリケーションがどれだけ書いたか」を表しているに過ぎません。
- そのため、`!rs->is_app_limited || bw >= ca->max_bw` のときだけフィルタに入力し、かつ `bw > ca->max_bw` のときに `ca->max_bw = bw` と更新します（App-Limited であっても、過去の `max_bw` を上回るサンプルが得られた場合は、真の帯域がそれ以上である証拠なので更新を許可します）。

</details>

#### Q3. パケット単位の「ラウンド（Packet-timed Round Trip）」はどうやって数えますか？

今後のステップ（Startup の終了判定や ProbeBW のフェーズ遷移）では、「何 RTT（ラウンド）経過したか」を時計ではなく配送パケット数ベースで数える必要があります。`tp->delivered` と `rs->prior_delivered` を使って、1 ラウンドの完了をどう判定できるでしょうか？

<details>
<summary>ヒントと答え</summary>

- ラウンド開始時点の累積配送パケット数を `ca->next_rtt_delivered = tp->delivered` に記録しておきます。
- その後到着する ACK の `rs->prior_delivered`（その ACK で確認されたパケットが送信された時点での `tp->delivered`）が `ca->next_rtt_delivered` 以上になったとき（`!before(rs->prior_delivered, ca->next_rtt_delivered)`）、そのラウンド開始以降に送ったパケットの ACK が返ってきた＝**1 ラウンド（1 RTT）が経過した** と判定できます。
- そのタイミングで `ca->round_cnt++` し、次の目標値を `ca->next_rtt_delivered = tp->delivered` に進めます。

</details>

#### Q4. `BW_UNIT`（`1 << 24`）でスケールされた `max_bw` と `min_rtt_us` から、どうやって BDP（パケット数）を計算しますか？

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

### 実装ステップと解説

#### 1. ソケットごとの状態構造体 `struct bpf_bbr2_ca`

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

#### 2. `bbr_bdp()` と `bbr_rate_bytes_per_sec()`

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) では、Linux カーネルの `tcp_bbr.c` と同じスケール定数（`BW_SCALE = 24`, `BBR_SCALE = 8`）を使って 2 つのコア計算ヘルパーを定義しています。

- `bbr_rate_bytes_per_sec()`: `bw`（`pkts/us << 24`）に `mss`・`gain`・`USEC_PER_SEC`（1% のペーシングマージン `BBR_PACING_MARGIN_PCT` を差し引いた値）を掛け、`BBR_SCALE` と `BW_SCALE` で右シフトして `bytes/sec` に変換します。
- `bbr_bdp()`: `ca->min_rtt_us` または `bw` が未計測（`0`）の間は安全な初期値 `10U` を返し、計測済みの場合は `ceil(bw * min_rtt_us * gain)` をパケット数として返します。

#### 3. `bpf_bbr2_cong_control()` — BtlBw・RTprop の更新と `snd_cwnd` 設定

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

### packetdrill テストの解説

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

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step02-btlbw-rtprop starter
## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step02-btlbw-rtprop solution
```


### `labs/bbr/step02-btlbw-rtprop/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 2: Bottleneck Bandwidth (BtlBw) and Round-Trip Propagation Time (RtProp) Tracking (Starter Skeleton)
 *
 * TODO 1: Implement min-filter for RtProp (min_rtt_us) instead of blindly overwriting
 * TODO 2: Implement max-filter for BtlBw (max_bw) and respect app-limited rate samples
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

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

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (rate > 0)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	bbr_set_pacing_rate(sk, bw, BBR_UNIT);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

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

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u32 bw, bdp;

	/*
	 * TODO 1: Track path propagation delay (RtProp).
	 * Do not overwrite min_rtt_us with every sample. Instead, track the minimum
	 * observed RTT across non-zero RTT samples.
	 */
	if (rs->rtt_us > 0)
		ca->min_rtt_us = rs->rtt_us;

	if (rs->delivered > 0 && rs->interval_us > 0) {
		if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
			ca->round_cnt++;
			ca->next_rtt_delivered = tp->delivered;
		}

		bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);

		/*
		 * TODO 2: Track bottleneck bandwidth (BtlBw).
		 * A flow cannot accurately measure bandwidth when application-limited,
		 * because it doesn't send fast enough to saturate the pipe.
		 * Ignore app-limited samples unless they exceed the current max_bw estimate,
		 * and track the maximum bandwidth across samples.
		 */
		ca->max_bw = bw;
	}

	bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT);

	bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_ssthresh, struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr2 = {
	.name		= "bpf_bbr2",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr2_init,
	.cong_control	= (void *)bpf_bbr2_cong_control,
	.ssthresh	= (void *)bpf_bbr2_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr2_undo_cwnd,
};
```


### `labs/bbr/step02-btlbw-rtprop/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 2: Track BtlBw and RtProp with app-limited rate sample filtering

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr2`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with 10 ms RTT (0.000 SYN, 0.000 SYN-ACK, 0.010 ACK)
// MSS = 1012 with 12-byte TCP timestamps -> payload MSS = 1000 bytes
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Phase 1: High-bandwidth burst
// write(4, ..., 10000) sends 10 segments (1:10001) at 0.010, ACKed at 0.020.
// rtt_us = 10000, delivered = 10, interval_us = 10000
// bw = 10 pkts / 10 ms = 1000 pkts/s (1,000,000 B/s)
// bdp = 10 pkts
// cwnd = bdp + 4 = 14
// pacing_rate = 99% of 1 MB/s = ~990,000 B/s
+0.00 write(4, ..., 10000) = 10000
+0.00 > P. 1:10001(10000) ack 1 <nop,nop,TS val 110 ecr 110>
+0.01 < . 1:1(0) ack 10001 win 257 <nop,nop,TS val 120 ecr 110>

+0.00 %{
assert tcpi_snd_cwnd == 14, f"expected cwnd 14, got {tcpi_snd_cwnd}"
assert 950000 < tcpi_pacing_rate < 1050000, f"expected pacing_rate ~990KB/s, got {tcpi_pacing_rate}"
}%

// Phase 2: App-limited smaller burst with longer interval and higher RTT
// Send 2 segments (2,000 bytes) at 0.050 (+0.030), ACKed at 0.070 (+0.020).
// rtt_us = 20000, interval_us = 50000, is_app_limited = true.
// BtlBw and RtProp tracking should preserve max_bw = 1000 pkts/s and min_rtt = 10000 us!
+0.03 write(4, ..., 2000) = 2000
+0.00 > P. 10001:12001(2000) ack 1 <nop,nop,TS val 150 ecr 120>
+0.02 < . 1:1(0) ack 12001 win 257 <nop,nop,TS val 170 ecr 150>

+0.00 %{
assert tcpi_snd_cwnd == 14, f"expected cwnd 14 to be preserved, got {tcpi_snd_cwnd}"
assert 950000 < tcpi_pacing_rate < 1050000, f"expected pacing_rate ~990KB/s to be preserved, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step02-btlbw-rtprop/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 2: Bottleneck Bandwidth (BtlBw) and Round-Trip Propagation Time (RtProp) Tracking
 *
 * In BBR, the network path model consists of:
 * - RtProp (min_rtt_us): the two-way propagation delay of the path without queuing.
 *   Tracked via a windowed min-filter over RTT samples.
 * - BtlBw (max_bw): the bottleneck bandwidth available to the flow.
 *   Tracked via a windowed max-filter over rate samples from ACKs.
 *
 * Important BBR invariant:
 * Application-limited (app-limited) rate samples must NEVER decrease max_bw
 * unless the sample is larger than the current max_bw estimate.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

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

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (rate > 0)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	bbr_set_pacing_rate(sk, bw, BBR_UNIT);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

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

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_ssthresh, struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr2 = {
	.name		= "bpf_bbr2",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr2_init,
	.cong_control	= (void *)bpf_bbr2_cong_control,
	.ssthresh	= (void *)bpf_bbr2_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr2_undo_cwnd,
};
```

</details>

---

## Step 3: STARTUP から DRAIN への遷移（満杯パイプ検出）

### 学習ゴール

このステップでは、BBR のステートマシンにおける **STARTUP（指数的な帯域探索）** から **DRAIN（STARTUP で作ったキューの排出）**、そして定常状態である **PROBE_BW（推定帯域での巡航）** への状態遷移を実装します。

- BBR の 3 つの基本モード（`BBR_STARTUP`、`BBR_DRAIN`、`BBR_PROBE_BW`）と、それぞれの `pacing_gain` / `cwnd_gain` の役割を理解します。
- パケット時間（`tp->delivered` と `ca->next_rtt_delivered`）に基づくラウンド（Round-Trip）計数の仕組みを実装します。
- `bbr_check_full_bw_reached()` を実装し、アプリ制限（`rs->is_app_limited`）でないラウンドが 3 回連続して 25% 以上（`1.25x`）の帯域成長を見せなかったときに「パイプが満杯になった（`full_bw_reached`）」と判定します。
- `bbr_check_drain()` を実装し、パイプ満杯検出時に `BBR_STARTUP` から `BBR_DRAIN` へ遷移させ、さらにインフライトパケット数（`tcp_packets_in_flight()`）が `1.0x BDP` 以下まで減った時点で `BBR_PROBE_BW` へ遷移させます。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

#### Q1. ロスを見ずにどうやって「パイプが満杯になった」と知るのでしょうか？

Reno や CUBIC のスロースタートは、パケットロスが起きるか（または HyStart で RTT 増加を検知するか）まで `cwnd` を倍増させ続けます。一方、パケットロスを輻輳シグナルとして使わない BBR は、`BBR_STARTUP`（ゲイン $2 / \ln(2) \approx 2.89\text{x}$）をいつ終えるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **BtlBw（推定ボトルネック帯域）の頭打ち（Plateau）を監視します**。
- パイプがまだ空いている間は、送信レートを毎ラウンド約 2 倍に増やせば、ACK から観測される配送レート（`ca->max_bw`）も毎ラウンド大きく伸びます。
- しかし、送信レートがボトルネック帯域に達すると、それ以上速く送ってもルータのキューが膨らむ（RTT が伸びる）だけで、**配送レート（`ca->max_bw`）は伸びなくなります**。
- BBR は「アプリ制限でないラウンドにおいて、`ca->max_bw` が基準値 `ca->full_bw` から **25% 以上（`bbr_full_bw_thresh = 5/4 = 1.25x`）伸びないラウンドが 3 回連続（`bbr_full_bw_cnt = 3`）** したら、パイプが満杯になった」と判定します。

</details>

#### Q2. 時間（クロック）ではなく「パケット数」でラウンドを数えるにはどうしますか？

「3 ラウンド連続で帯域が 25% 伸びなかった」と判定するには、1 ラウンド（1 往復）の区切りを正確に知る必要があります。タイマーや時刻（`tcp_mstamp`）を使わずに、`tp->delivered`（累積配送パケット数）と `rs->prior_delivered` を使って 1 ラウンドの終了を検出するにはどうすればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- ラウンド開始時点の `tp->delivered` を `ca->next_rtt_delivered` に記録しておきます。
- 各パケット送信時、カーネルの `tcp_rate_skb_sent()` はその時点の `tp->delivered` を `skb` にスナップショットします。そのパケットが ACK されると、スナップショットされた値が `rs->prior_delivered` として返ってきます。
- したがって、`!before(rs->prior_delivered, ca->next_rtt_delivered)`（または 1 ウィンドウあたり 1 ACK の場合は `tp->delivered >= ca->next_rtt_delivered`）が成立した瞬間、「前のラウンド開始以降に送信したパケットの ACK が戻ってきた＝1 ラウンドが一周した」と分かります。
- その時点で `ca->next_rtt_delivered = tp->delivered;` に更新し、`ca->round_start = 1` を立てます。

</details>

#### Q3. なぜ `BBR_STARTUP` から直接 `BBR_PROBE_BW` に行かず、`BBR_DRAIN` を挟むのでしょうか？

`BBR_STARTUP` で 3 ラウンド連続して帯域が伸びなかったと分かった時点で、ネットワーク内のキューにはどれくらいの余分なパケットが溜まっているでしょうか？また、それを素早く抜くには `pacing_gain` と `cwnd_gain` をどう設定すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- `BBR_STARTUP` では `bbr_high_gain`（約 `2.89x`）で送信し、かつ帯域頭打ちを検出するまでに 3 ラウンド見守るため、STARTUP 終了時点ではインフライト量が約 `2.89x BDP` まで膨らみ、ボトルネックに約 `1.89x BDP` もの過剰キューが作られています。
- そのため、すぐ巡航モード（`1.0x BtlBw`）に入ると過剰キューが抜けないまま残ってしまいます。
- そこでまず `BBR_DRAIN` に入り、`pacing_gain` を `bbr_high_gain` の逆数である `bbr_drain_gain = BBR_UNIT * 1000 / 2885`（`88 / 256`、約 `0.35x`）に落としてキューを排出します。一方、`cwnd_gain` は送信が突然止まらないよう `bbr_high_gain` を維持し、ペーシング主導で滑らかに減速させます。
- `tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)`（インフライト量が `1.0x BDP` 以下）になった時点でキューが抜けたと判断し、`BBR_PROBE_BW`（`pacing_gain = 1.0x`、`cwnd_gain = 2.0x`）へ遷移します。

</details>

#### Q4. `bbr_set_pacing_rate()` で `ca->full_bw_reached` をチェックするのはなぜですか？

`bbr_set_pacing_rate()` には `if (ca->full_bw_reached || rate > sk->sk_pacing_rate)` という条件があります。なぜ `ca->full_bw_reached` が `0` の間はペーシングレートを下げることを禁止し、`1` になったら下げることを許可するのでしょうか？

<details>
<summary>ヒントと答え</summary>

- `BBR_STARTUP` の初期段階では、`cwnd` がまだ小さかったり一時的にアプリ制限になったりして、本来のボトルネック帯域より小さいレートサンプルしか得られないことがあります。パイプを満たす前（`!ca->full_bw_reached`）にペーシングレートを下げてしまうと、STARTUP の加速が止まってしまいます。
- 一方、パイプが満杯になった後（`ca->full_bw_reached == 1`）は、`BBR_DRAIN`（約 `0.35x`）や `BBR_PROBE_BW`（`1.0x`）へ遷移した際に、`2.89x BtlBw` から低いペーシングレートへ引き下げる必要があるため、無条件で `sk->sk_pacing_rate = rate` の更新を許可します。

</details>

---

### 実装ステップと解説

#### 1. モードと固定小数点定数

`BBR_SCALE = 8`（`BBR_UNIT = 256` が `1.0x`）の固定小数点で各ゲインとしきい値を定義します。

- `bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1 = 739`（約 `2.89x`）: `BBR_STARTUP` の `pacing_gain` および `cwnd_gain` です。
- `bbr_drain_gain = BBR_UNIT * 1000 / 2885 = 88`（約 `0.35x`）: `BBR_DRAIN` の `pacing_gain` です。
- `bbr_cwnd_gain = BBR_UNIT * 2 = 512`（`2.0x`）: `BBR_PROBE_BW` の `cwnd_gain` です。
- `bbr_full_bw_thresh = BBR_UNIT * 5 / 4 = 320`（`1.25x`）: STARTUP 継続に必要な帯域成長率（+25%）です。
- `bbr_full_bw_cnt = 3`: 25% 以上の成長が見られないラウンドが 3 回続いたらパイプ満杯と判定します。

#### 2. `bbr_check_full_bw_reached()` — 満杯パイプの検出

ラウンドの区切り（`ca->round_start == 1`）かつアプリ制限でない（`!rs->is_app_limited`）サンプルが届いたときに呼ばれます。

1. `bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE` で、基準帯域 `ca->full_bw` の 1.25 倍を計算します。
2. `ca->max_bw >= bw_thresh` であれば、帯域はまだ 25% 以上伸びているため、`ca->full_bw = ca->max_bw` に更新して `ca->full_bw_cnt = 0` にリセットします。
3. 伸びていなければ `++ca->full_bw_cnt` を進め、`bbr_full_bw_cnt`（`3`）に達したら `ca->full_bw_reached = 1` を立てます。

#### 3. `bbr_check_drain()` — `BBR_DRAIN` と `BBR_PROBE_BW` への遷移

1. `ca->mode == BBR_STARTUP && ca->full_bw_reached` が成り立ったら、`ca->mode = BBR_DRAIN` に遷移し、観測用に `tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT)`（`1.0x BDP`）を記録します。
2. 続いて（`else if` ではなく独立した `if` で fall-through させることで、すでにインフライトが少ない場合は同じ ACK 内で即座に）`ca->mode == BBR_DRAIN && tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)` を判定し、条件を満たせば `ca->mode = BBR_PROBE_BW` に遷移します。

#### 4. `bbr_update_gains()` と `bbr_set_cwnd()`

- `bbr_update_gains()` は現在の `ca->mode` に応じて `ca->pacing_gain` と `ca->cwnd_gain` を設定します。
- `bbr_set_cwnd()` は `target_cwnd = bbr_bdp(sk, bw, gain)` を計算し、`ca->full_bw_reached` が立った後は `cwnd = min(cwnd + acked, target_cwnd)` で `target_cwnd`（`BBR_PROBE_BW` では `2.0x BDP`）へ収束させます。

---

### packetdrill テストの解説

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

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step03-startup-drain starter
## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step03-startup-drain solution
```


### `labs/bbr/step03-startup-drain/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 3: STARTUP to DRAIN and PROBE_BW Transition (Starter Skeleton)
 *
 * TODO 1: Implement bbr_check_full_bw_reached() to detect 3 consecutive
 *         non-app-limited rounds without >= 25% bandwidth growth.
 * TODO 2: Implement bbr_check_drain() to transition from BBR_STARTUP to
 *         BBR_DRAIN, and from BBR_DRAIN to BBR_PROBE_BW once in-flight <= BDP.
 * TODO 3: Implement bbr_update_gains() to set pacing_gain and cwnd_gain
 *         for BBR_STARTUP, BBR_DRAIN, and BBR_PROBE_BW.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1U << BW_SCALE)
#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET	4U
#define BBR_PACING_MARGIN_PCT	1U

enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* steady-state: pace around estimated bw */
};

static const __u32 bbr_high_gain      = BBR_UNIT * 2885 / 1000 + 1; /* 739 (~2.89x) */
static const __u32 bbr_drain_gain     = BBR_UNIT * 1000 / 2885;     /* 88 (~0.35x) */
static const __u32 bbr_cwnd_gain      = BBR_UNIT * 2;               /* 512 (2.0x) */
static const __u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;           /* 320 (1.25x) */
static const __u32 bbr_full_bw_cnt    = 3U;

struct bpf_bbr3_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
	__u32 full_bw;
	__u32 full_bw_cnt;
	__u32 mode;
	__u32 pacing_gain;
	__u32 cwnd_gain;
	__u8  round_start;
	__u8  full_bw_reached;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->round_cnt++;
		ca->round_start = 1;
	}

	bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw) {
		if (bw > ca->max_bw)
			ca->max_bw = bw;
	}
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	/*
	 * TODO 1: Detect when STARTUP has filled the pipe.
	 * - Return early if ca->full_bw_reached, !ca->round_start, or rs->is_app_limited.
	 * - Compute bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE.
	 * - If ca->max_bw >= bw_thresh:
	 *     update ca->full_bw = ca->max_bw, reset ca->full_bw_cnt = 0, and return.
	 * - Otherwise increment ca->full_bw_cnt, and when it reaches bbr_full_bw_cnt (3),
	 *     set ca->full_bw_reached = 1.
	 */
	(void)sk;
	(void)rs;
	(void)bbr_full_bw_thresh;
	(void)bbr_full_bw_cnt;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	/*
	 * TODO 2: Transition through BBR_DRAIN to BBR_PROBE_BW.
	 * - If ca->mode == BBR_STARTUP and ca->full_bw_reached:
	 *     set ca->mode = BBR_DRAIN and tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT).
	 * - If ca->mode == BBR_DRAIN and tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT):
	 *     set ca->mode = BBR_PROBE_BW.
	 */
	(void)sk;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	/*
	 * TODO 3: Set ca->pacing_gain and ca->cwnd_gain based on ca->mode:
	 * - BBR_STARTUP:  pacing_gain = bbr_high_gain,  cwnd_gain = bbr_high_gain
	 * - BBR_DRAIN:    pacing_gain = bbr_drain_gain, cwnd_gain = bbr_high_gain
	 * - BBR_PROBE_BW: pacing_gain = BBR_UNIT,       cwnd_gain = bbr_cwnd_gain
	 */
	(void)bbr_drain_gain;
	(void)bbr_cwnd_gain;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked,
					 __u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		return;

	target_cwnd = bbr_bdp(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < 10U)
		cwnd = cwnd + acked;

	cwnd = max(cwnd, BBR_CWND_MIN_TARGET);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
	ca->round_start = 0;
	ca->full_bw_reached = 0;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us > 0 &&
	    (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	bbr_update_bw(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_undo_cwnd, struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr3 = {
	.name		= "bpf_bbr3",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr3_init,
	.cong_control	= (void *)bpf_bbr3_cong_control,
	.ssthresh	= (void *)bpf_bbr3_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr3_undo_cwnd,
};
```


### `labs/bbr/step03-startup-drain/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 3: BBR_STARTUP to BBR_DRAIN / BBR_PROBE_BW transition after 3 plateau rounds

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr3`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial 100 ms RTT so initial pacing rate is ~141 KB/s
// MSS = 1000 bytes, receive window = 1,000 bytes (1 segment per RTT -> BtlBw = 100 KB/s at 10 ms RTT)
+0.00 < S 0:0(0) win 1000 <mss 1000,sackOK,nop,nop>
+0.00 > S. 0:0(0) ack 1 <...>
+0.10 < . 1:1(0) ack 1 win 1000
+0.00 accept(3, ..., ...) = 4

// Queue 5,000 bytes (5 segments) in one write() so the write queue stays
// non-empty across rounds (avoiding app-limited rate samples after warmup).
+0.00 write(4, ..., 5000) = 5000

// Warmup round (1:1001): clears initial app_limited state upon ACK at +0.01
+0.00 > P. 1:1001(1000) ack 1
+0.01 < . 1:1(0) ack 1001 win 1000

// Round 1 (1001:2001): non-app-limited, sets full_bw = BtlBw (~100 KB/s), full_bw_cnt = 0
// In BBR_STARTUP, pacing_gain = bbr_high_gain (~2.89x) -> tcpi_pacing_rate ~= 286 KB/s
+0.00 > P. 1001:2001(1000) ack 1
+0.01 < . 1:1(0) ack 2001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 1 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
assert tcpi_snd_ssthresh == 0x7fffffff, f"Round 1 STARTUP: expected infinite ssthresh, got {tcpi_snd_ssthresh}"
}%

// Round 2 (2001:3001): 0% bw growth (< 25%) -> full_bw_cnt = 1 (still in BBR_STARTUP)
+0.00 > P. 2001:3001(1000) ack 1
+0.01 < . 1:1(0) ack 3001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 2 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 3 (3001:4001): 0% bw growth (< 25%) -> full_bw_cnt = 2 (still in BBR_STARTUP)
+0.00 > P. 3001:4001(1000) ack 1
+0.01 < . 1:1(0) ack 4001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 3 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 4 (4001:5001): 0% bw growth (< 25%) -> full_bw_cnt = 3 -> full_bw_reached = 1!
// Transitions BBR_STARTUP -> BBR_DRAIN -> BBR_PROBE_BW (since in_flight == 0 <= BDP).
// Pacing rate drops from ~2.89x BtlBw (~286 KB/s) to ~1.0x BtlBw (~99 KB/s)!
+0.00 > P. 4001:5001(1000) ack 1
+0.01 < . 1:1(0) ack 5001 win 1000
+0.00 %{
assert 90000 < tcpi_pacing_rate < 110000, f"Round 4 exited STARTUP: expected ~99KB/s, got {tcpi_pacing_rate}"
assert 1 <= tcpi_snd_ssthresh <= 4, f"Round 4 exited STARTUP: expected ssthresh in [1, 4], got {tcpi_snd_ssthresh}"
assert tcpi_snd_cwnd == 4, f"Round 4 PROBE_BW: expected cwnd == 4, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step03-startup-drain/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 3: STARTUP to DRAIN and PROBE_BW Transition (Full Pipe Detection)
 *
 * BBR starts in BBR_STARTUP with a high pacing/cwnd gain (2/ln(2) ~= 2.89x) to
 * double its sending rate each round and quickly discover bottleneck bandwidth.
 *
 * When estimated bottleneck bandwidth (max_bw) stops growing by at least 25%
 * (bbr_full_bw_thresh = 1.25x) for 3 consecutive non-app-limited rounds
 * (bbr_full_bw_cnt = 3), BBR estimates that the pipe is full:
 * 1. Transition from BBR_STARTUP to BBR_DRAIN (pacing_gain = 1/high_gain ~= 0.35x,
 *    cwnd_gain = high_gain ~= 2.89x) to drain the queue created during STARTUP.
 * 2. Once packets in flight fall to <= 1.0x BDP, transition from BBR_DRAIN to
 *    BBR_PROBE_BW (pacing_gain = 1.0x, cwnd_gain = 2.0x).
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1U << BW_SCALE)
#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET	4U
#define BBR_PACING_MARGIN_PCT	1U

enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* steady-state: pace around estimated bw */
};

static const __u32 bbr_high_gain      = BBR_UNIT * 2885 / 1000 + 1; /* 739 (~2.89x) */
static const __u32 bbr_drain_gain     = BBR_UNIT * 1000 / 2885;     /* 88 (~0.35x) */
static const __u32 bbr_cwnd_gain      = BBR_UNIT * 2;               /* 512 (2.0x) */
static const __u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;           /* 320 (1.25x) */
static const __u32 bbr_full_bw_cnt    = 3U;

struct bpf_bbr3_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
	__u32 full_bw;
	__u32 full_bw_cnt;
	__u32 mode;
	__u32 pacing_gain;
	__u32 cwnd_gain;
	__u8  round_start;
	__u8  full_bw_reached;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->round_cnt++;
		ca->round_start = 1;
	}

	bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw) {
		if (bw > ca->max_bw)
			ca->max_bw = bw;
	}
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT))
		ca->mode = BBR_PROBE_BW;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked,
					 __u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		return;

	target_cwnd = bbr_bdp(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < 10U)
		cwnd = cwnd + acked;

	cwnd = max(cwnd, BBR_CWND_MIN_TARGET);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
	ca->round_start = 0;
	ca->full_bw_reached = 0;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us > 0 &&
	    (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	bbr_update_bw(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_undo_cwnd, struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr3 = {
	.name		= "bpf_bbr3",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr3_init,
	.cong_control	= (void *)bpf_bbr3_cong_control,
	.ssthresh	= (void *)bpf_bbr3_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr3_undo_cwnd,
};
```

</details>

---

## Step 4: PROBE_BW の 8 相ゲインサイクル（帯域探索とキュー排出）

### 学習ゴール

このステップでは、定常状態である **PROBE_BW** モードにおける 8 相のペーシングゲインサイクル（`bbr_pacing_gain[]`）と、各フェーズの遷移判定ロジックを実装します。

- 定常状態で利用可能な帯域の変化に追従し、かつ恒常的なバッファ溜まり（バッファブロート）を防ぐための「8 相ゲインサイクル（`1.25x` → `0.75x` → `1.0x` × 6）」の構造と役割を理解します。
- `bbr_is_next_cycle_phase()` を実装し、巡航フェーズ・探索フェーズ・排出フェーズそれぞれの終了条件（最小 RTT 経過、目標インフライト量到達、ロス発生、キュー排出完了）の違いを理解します。
- `bbr_advance_cycle_phase()` と `bbr_update_cycle_phase()` を実装し、フェーズインデックス（`ca->cycle_idx`）の進行とフェーズ開始時刻（`ca->cycle_mstamp`）の記録を行います。
- `bbr_update_gains()` で `ca->cycle_idx` に応じた `bbr_pacing_gain[]` の設定を行います。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

#### Q1. 定常状態で常に 1.0x で送り続けず、なぜ 8 相のゲインサイクル（1.25x → 0.75x → 1.0x × 6）を繰り返すのでしょうか？

`BBR_STARTUP` と `BBR_DRAIN` を経てボトルネック帯域（`ca->max_bw`）と最小 RTT（`ca->min_rtt_us`）を正しく計測できた後、なぜずっと `1.0x` のペーシングレートで送り続けてはいけないのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ネットワークの利用可能帯域は時間とともに変動するためです**。
- 他のフローが終了して帯域が空いた場合、送信側が現在の推定帯域（`1.0x`）のまま送り続けていては、新しく空いた帯域の存在に永遠に気づくことができません。
- そこで BBR は、約 8 RTT ごとに一時的にペーシングレートを `1.25x`（+25%）に引き上げて余剰帯域の探索（**Phase 0: 探索フェーズ**）を行います。
- 探索によって余剰帯域がなかった場合、ボトルネックルータに `0.25x BDP` のキューが一時的に溜まります。これを放置すると遅延が増大したままになるため、次のラウンドでペーシングレートを `0.75x`（-25%）に落として過剰キューを即座に排出（**Phase 1: 排出フェーズ**）します。
- その後、推定帯域どおりの `1.0x` で 6 ラウンド巡航（**Phases 2..7: 巡航フェーズ**）し、低遅延・高スループットを維持します。

</details>

#### Q2. 探索フェーズ（Phase 0: 1.25x）を終了して排出フェーズへ進む条件は何でしょうか？

探索フェーズ（`ca->pacing_gain > BBR_UNIT`）の終了判定において、単に「1 RTT（`min_rtt_us`）が経過したか」だけでなく、インフライト量（`rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)`）やパケットロス（`rs->losses`）を判定条件に含めるのはなぜでしょうか？

<details>
<summary>ヒントと答え</summary>

- 送信側がアプリ制限（送信バッファ不足）やウィンドウ制限によって十分なパケットを送信できていない場合、1 RTT が経過してもパイプには `1.25x BDP` のデータが実際に注入されていない可能性があります。
- その状態で探索を打ち切ってしまうと、ネットワークに帯域の余力があるかどうかを正しくテストできません。
- したがって、探索フェーズは「少なくとも 1 RTT が経過していること（`is_full_length`）」に加えて、「実際に `1.25x BDP` のインフライトパケットを送り込めたこと（`rs->prior_in_flight >= bbr_inflight(1.25x)`）」または「バッファ溢れによるロスが発生したこと（`rs->losses`）」のいずれかが確認されるまで維持されます。

</details>

#### Q3. 排出フェーズ（Phase 1: 0.75x）は 1 RTT 経過を待たずに終了することが許可されている（OR 条件）のはなぜでしょうか？

探索フェーズとは対照的に、排出フェーズ（`ca->pacing_gain < BBR_UNIT`）では `is_full_length || rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)` という OR 条件になっています。なぜ 1 RTT 待たずに早期終了してよいのでしょうか？

<details>
<summary>ヒントと答え</summary>

- 排出フェーズの唯一の目的は、「Phase 0 で注入した余分なキューをボトルネックから抜くこと」です。
- ACK が戻ってきた時点でインフライトパケット数が `1.0x BDP`（`bbr_inflight(sk, ca->max_bw, BBR_UNIT)`）以下まで落ちていれば、ボトルネックの余分なキューはすでに完全に抜け切ったと判断できます。
- キューが抜けた後も `0.75x` で低速送信を続けると、パイプを不必要に過小利用（アンダーラン）してしまうため、目標インフライト量に達した瞬間に即座に排出フェーズを終了し、`1.0x` の巡航フェーズへ移行します。

</details>

#### Q4. 実際の BBR では PROBE_BW 開始時に `cycle_idx` をランダムに選ぶのに対し、この演習では 0 から始めるのはなぜでしょうか？また、ランダム化にはどんな目的があるのでしょうか？

製品版の BBR では、`BBR_DRAIN` から `BBR_PROBE_BW` に入る際、`ca->cycle_idx = CYCLE_LEN - 1 - prandom_u32_max(bbr_cycle_rand)` のように巡航フェーズ（2〜7）の間でランダムなオフセットを選びます。なぜランダム化が必要なのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **複数フロー間の探索タイミングの同期（Synchronization）を防ぐためです**。
- もし同じボトルネックを共有するすべての BBR フローが同時に Phase 0（`1.25x`）に入ると、全フローが一斉に 25% 増しのトラフィックを送り出し、大規模なキュー膨張やパケットロスを引き起こしてしまいます。
- 巡航フェーズの開始インデックスをランダムに散らす（脱同期化する）ことで、各フローが異なるタイミングで順番に帯域探索を行うようになり、ネットワーク全体として安定した利用率と低遅延が保たれます。
- 本演習では、packetdrill による決定論的で再現性のあるテスト（Phase 0 → Phase 1 → Phase 2 の遷移検証）を行うため、意図的に `cycle_idx = 0` から固定で開始しています。

</details>

---

### 実装ステップと解説

#### 1. `bbr_pacing_gain[]` 配列

BBR の定常状態（`BBR_PROBE_BW`）では、8 相のゲインテーブルを循環します。

```c
##define CYCLE_LEN 8

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4, /* Phase 0: 1.25x (探索) */
	BBR_UNIT * 3 / 4, /* Phase 1: 0.75x (排出) */
	BBR_UNIT,         /* Phase 2: 1.0x (巡航) */
	BBR_UNIT,         /* Phase 3: 1.0x (巡航) */
	BBR_UNIT,         /* Phase 4: 1.0x (巡航) */
	BBR_UNIT,         /* Phase 5: 1.0x (巡航) */
	BBR_UNIT,         /* Phase 6: 1.0x (巡航) */
	BBR_UNIT,         /* Phase 7: 1.0x (巡航) */
};
```

- **Phase 0（1.25x）**: 送信レートを 25% 引き上げ、より大きな帯域が利用可能かプローブします。
- **Phase 1（0.75x）**: 送信レートを 25% 引き下げ、Phase 0 でルータに溜まったキューを排出します。
- **Phases 2..7（1.0x）**: 推定帯域どおりのレートで 6 ラウンド巡航し、キューを溜めずに高効率通信を継続します。

#### 2. `bbr_is_next_cycle_phase()` — 各フェーズの終了判定

現在のフェーズが終了して次のフェーズへ進むべきかを判定します。

```c
static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length = (tp->tcp_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;

	if (ca->pacing_gain == BBR_UNIT)
		return is_full_length;

	if (ca->pacing_gain > BBR_UNIT)
		return is_full_length &&
			(rs->losses ||
			 rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain));

	return is_full_length ||
		rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT);
}
```

1. **基本時間（`is_full_length`）**: フェーズ開始時刻（`ca->cycle_mstamp`）からの経過時間が `ca->min_rtt_us` を超えているかを判定します。
2. **巡航フェーズ（`ca->pacing_gain == BBR_UNIT`）**: 1 RTT が経過していれば次フェーズへ進みます（`is_full_length`）。
3. **探索フェーズ（`ca->pacing_gain > BBR_UNIT`）**: 1 RTT が経過し、かつパケットロスが発生したか、またはインフライトパケット数が `1.25x BDP` に達していれば次フェーズへ進みます。
4. **排出フェーズ（`ca->pacing_gain < BBR_UNIT`）**: 1 RTT が経過したか、あるいはインフライトパケット数が `1.0x BDP` 以下まで下がってキューが抜け切っていれば次フェーズへ進みます。

#### 3. `bbr_advance_cycle_phase()` と `bbr_update_cycle_phase()` — フェーズの進行

```c
static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->tcp_mstamp;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
		bbr_advance_cycle_phase(sk);
}
```

- `bbr_advance_cycle_phase()` はフェーズ番号 `ca->cycle_idx` を 1 つ進め、`& (CYCLE_LEN - 1)`（`& 7`）で 0〜7 の範囲にラップアラウンドさせます。同時に `ca->cycle_mstamp` に現在の `tp->tcp_mstamp` を記録します。
- `bbr_update_cycle_phase()` は `ca->mode == BBR_PROBE_BW` かつ `bbr_is_next_cycle_phase()` が真のときに `bbr_advance_cycle_phase()` を呼び出します。

#### 4. `bbr_update_gains()` でのフェーズゲイン反映

```c
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
```

`BBR_PROBE_BW` において、現在のフェーズインデックス `ca->cycle_idx` に応じたゲインを `bbr_pacing_gain[]` から取得して `ca->pacing_gain` に設定します。`ca->cwnd_gain` は固定で `bbr_cwnd_gain`（`2.0x`）を用います。

---

### packetdrill テストの解説

[test.pkt](test.pkt) は、`BBR_STARTUP` を抜けて `BBR_PROBE_BW` に入った後の Phase 0（探索: `1.25x`）→ Phase 1（排出: `0.75x`）→ Phase 2（巡航: `1.0x`）のフェーズ遷移を検証します。

1. **STARTUP 終了と PROBE_BW Phase 0 突入（Round 4）**:
   - 4 ラウンドにわたる帯域頭打ちにより、`full_bw_reached` が立ち、`BBR_STARTUP` → `BBR_DRAIN` → `BBR_PROBE_BW`（`ca->cycle_idx = 0`）へ遷移します。
   - Phase 0 のゲインは `5/4`（`1.25x`）です。ボトルネック帯域約 100 KB/s に対し、1% のペーシングマージンを引いた約 `123 KB/s`（`115000 < tcpi_pacing_rate < 135000`）が設定されていることを確認します。
   - 受信ウィンドウを 2,000 バイト（2 セグメント）に広げて Phase 0 での探索を許可します。
2. **Phase 0 → Phase 1 への遷移**:
   - 10 ms 間隔で 2 セグメント（`6001:7001` と `7001:8001`）を送信し、インフライトパケット数を 2（`prior_in_flight >= bbr_inflight(5/4) == 2`）に到達させます。
   - 20 ms 経過後（`> min_rtt_us = 10 ms`）にまとめて ACK を受信することで、`is_full_length` とインフライト目標を同時に満たし、Phase 1（排出フェーズ）へ進めます。
   - Phase 1 のゲインは `3/4`（`0.75x`）となり、ペーシングレートが約 `74 KB/s`（`65000 < tcpi_pacing_rate < 85000`）へ低下したことをアサートします。
3. **Phase 1 → Phase 2 への遷移**:
   - 1 セグメント（`8001:9001`）を送信し、15 ms 後（`> min_rtt_us = 10 ms`、かつ `prior_in_flight == 1 <= BDP`）に ACK を受信します。
   - キューが排出されて条件を満たしたため、Phase 2（巡航フェーズ、`cycle_idx = 2`）へ遷移します。
   - Phase 2 のゲインは `1.0x` となり、ペーシングレートが約 `99 KB/s`（`90000 < tcpi_pacing_rate < 110000`）に戻ったことをアサートします。

---

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step04-probe-bw starter
## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step04-probe-bw solution
```


### `labs/bbr/step04-probe-bw/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 4: PROBE_BW phase cycling (8-phase gain cycle)
 *
 * Steady-state bandwidth probing using an 8-phase gain cycle:
 *   Phase 0: 1.25x (probe for more available bandwidth)
 *   Phase 1: 0.75x (drain excess queue created in phase 0)
 *   Phases 2..7: 1.0x (cruise at estimated bandwidth)
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE			24
#define BW_UNIT				(1ULL << BW_SCALE)

#define BBR_SCALE			8
#define BBR_UNIT			(1 << BBR_SCALE)

#define CYCLE_LEN			8
#define TCP_INIT_CWND			10

static const int bbr_pacing_margin_percent = 1;
static const int bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain = BBR_UNIT * 2;
static const int bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const int bbr_full_bw_cnt = 3;
static const int bbr_cwnd_min_target = 4;

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
};

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
};

struct bbr_ca {
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT seen */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	return bbr_bdp(sk, bw, gain);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	/*
	 * TODO 1: PROBE_BW のフェーズ終了条件を判定する
	 *
	 * 1. フェーズ開始からの経過時間 (tp->tcp_mstamp - ca->cycle_mstamp) が
	 *    ca->min_rtt_us を超えているか (is_full_length) を判定する。
	 * 2. ca->pacing_gain == BBR_UNIT (巡航フェーズ 2..7) の場合:
	 *    is_full_length を返す (経過時間のみで判定)。
	 * 3. ca->pacing_gain > BBR_UNIT (探索フェーズ 0, 1.25x) の場合:
	 *    is_full_length かつ、ロス発生 (rs->losses) または
	 *    rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)
	 *    に達していれば true を返す。
	 * 4. ca->pacing_gain < BBR_UNIT (排出フェーズ 1, 0.75x) の場合:
	 *    is_full_length または、キューが抜けて
	 *    rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)
	 *    に下がっていれば true を返す。
	 */
	(void)sk;
	(void)rs;
	return false;
}

static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	/*
	 * TODO 2: 次のフェーズへ進める
	 *
	 * 1. ca->cycle_idx を 1 つ進め、CYCLE_LEN (8) でラップさせる (& (CYCLE_LEN - 1))。
	 * 2. ca->cycle_mstamp に現在のタイムスタンプ tp->tcp_mstamp を記録する。
	 */
	(void)sk;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	/*
	 * TODO 3: PROBE_BW モードにおけるフェーズ更新
	 *
	 * ca->mode == BBR_PROBE_BW かつ bbr_is_next_cycle_phase(sk, rs) が真なら
	 * bbr_advance_cycle_phase(sk) を呼び出す。
	 */
	(void)sk;
	(void)rs;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->tcp_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rs->rtt_us;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		/*
		 * TODO 4: PROBE_BW での pacing_gain を bbr_pacing_gain[] から設定する
		 *
		 * ca->cycle_idx (0..7) に応じて bbr_pacing_gain[ca->cycle_idx & 7] を設定する。
		 * starter では暫定的にフラットな BBR_UNIT (1.0x) を設定している。
		 */
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, (__u32)bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_main, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_sndbuf_expand, struct sock *sk)
{
	return 3;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr4 = {
	.init		= (void *)bpf_bbr4_init,
	.cong_control	= (void *)bpf_bbr4_main,
	.sndbuf_expand	= (void *)bpf_bbr4_sndbuf_expand,
	.undo_cwnd	= (void *)bpf_bbr4_undo_cwnd,
	.ssthresh	= (void *)bpf_bbr4_ssthresh,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr4",
};
```


### `labs/bbr/step04-probe-bw/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 4: PROBE_BW 8-phase gain cycle (5/4 -> 3/4 -> 1.0)

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr4`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial 100 ms RTT and win 2000 on the ACK so
// tp->max_window = 2000 and tp->mss_cache = 1000 from the start.
+0.00 < S 0:0(0) win 2000 <mss 1000,sackOK,nop,nop>
+0.00 > S. 0:0(0) ack 1 <...>
+0.10 < . 1:1(0) ack 1 win 2000
+0.00 accept(3, ..., ...) = 4

// Send 1 segment (1:1001) and ACK with win 1000 to set snd_wnd = 1000
// (1 segment per 10 ms RTT -> BtlBw = 100 KB/s) while keeping max_window = 2000.
+0.00 write(4, ..., 1000) = 1000
+0.00 > P. 1:1001(1000) ack 1
+0.01 < . 1:1(0) ack 1001 win 1000

// Queue 5,000 bytes (5 segments) in one write() so the write queue stays
// non-empty across STARTUP rounds (avoiding app-limited rate samples after warmup).
+0.01 write(4, ..., 5000) = 5000

// Warmup round (1001:2001): clears app_limited state
+0.00 > . 1001:2001(1000) ack 1
+0.01 < . 1:1(0) ack 2001 win 1000

// Round 1 (2001:3001): non-app-limited, sets full_bw = BtlBw (~100 KB/s), full_bw_cnt = 0
+0.00 > . 2001:3001(1000) ack 1
+0.01 < . 1:1(0) ack 3001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 1 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 2 (3001:4001): 0% bw growth (< 25%) -> full_bw_cnt = 1
+0.00 > . 3001:4001(1000) ack 1
+0.01 < . 1:1(0) ack 4001 win 1000

// Round 3 (4001:5001): 0% bw growth (< 25%) -> full_bw_cnt = 2
+0.00 > . 4001:5001(1000) ack 1
+0.01 < . 1:1(0) ack 5001 win 1000

// Round 4 (5001:6001): 0% bw growth (< 25%) -> full_bw_cnt = 3 -> full_bw_reached = 1!
// Transitions BBR_STARTUP -> BBR_DRAIN -> BBR_PROBE_BW (cycle_idx = 0, pacing_gain = 5/4).
// Pacing rate is 1.25x BtlBw (* 0.99 margin) ~= 123 KB/s!
// Open receive window back to 2000 bytes (2 segments) for Phase 0 probing.
+0.00 > P. 5001:6001(1000) ack 1
+0.01 < . 1:1(0) ack 6001 win 2000
+0.00 %{
assert 115000 < tcpi_pacing_rate < 135000, f"PROBE_BW Phase 0 (5/4 gain): expected ~123KB/s, got {tcpi_pacing_rate}"
assert tcpi_snd_cwnd == 4, f"PROBE_BW Phase 0: expected cwnd == 4, got {tcpi_snd_cwnd}"
}%

// Phase 0 -> Phase 1 transition:
// Send 2 segments spaced by 10 ms so prior_in_flight == 2 >= bbr_inflight(5/4) == 2,
// and ACK after 10 ms (total 2 segments over 20 ms = 100 KB/s, elapsed > min_rtt_us).
// Transitions to Phase 1 (cycle_idx = 1, pacing_gain = 3/4 -> ~74 KB/s)!
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 6001:7001(1000) ack 1
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 7001:8001(1000) ack 1
+0.01 < . 1:1(0) ack 8001 win 2000
+0.00 %{
assert 65000 < tcpi_pacing_rate < 85000, f"PROBE_BW Phase 1 (3/4 gain): expected ~74KB/s, got {tcpi_pacing_rate}"
}%

// Phase 1 -> Phase 2 transition:
// Send 1 segment and ACK after 15 ms (> min_rtt_us = 10 ms, prior_in_flight == 1 <= BDP).
// Transitions to Phase 2 (cycle_idx = 2, pacing_gain = 1.0 -> ~99 KB/s)!
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 8001:9001(1000) ack 1
+0.015 < . 1:1(0) ack 9001 win 2000
+0.00 %{
assert 90000 < tcpi_pacing_rate < 110000, f"PROBE_BW Phase 2 (1.0 gain): expected ~99KB/s, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step04-probe-bw/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 4: PROBE_BW phase cycling (8-phase gain cycle)
 *
 * Steady-state bandwidth probing using an 8-phase gain cycle:
 *   Phase 0: 1.25x (probe for more available bandwidth)
 *   Phase 1: 0.75x (drain excess queue created in phase 0)
 *   Phases 2..7: 1.0x (cruise at estimated bandwidth)
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE			24
#define BW_UNIT				(1ULL << BW_SCALE)

#define BBR_SCALE			8
#define BBR_UNIT			(1 << BBR_SCALE)

#define CYCLE_LEN			8
#define TCP_INIT_CWND			10

static const int bbr_pacing_margin_percent = 1;
static const int bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain = BBR_UNIT * 2;
static const int bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const int bbr_full_bw_cnt = 3;
static const int bbr_cwnd_min_target = 4;

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
};

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
};

struct bbr_ca {
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT seen */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	return bbr_bdp(sk, bw, gain);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length = (tp->tcp_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;

	if (ca->pacing_gain == BBR_UNIT)
		return is_full_length;

	if (ca->pacing_gain > BBR_UNIT)
		return is_full_length &&
			(rs->losses ||
			 rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain));

	return is_full_length ||
		rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT);
}

static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->tcp_mstamp;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
		bbr_advance_cycle_phase(sk);
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->tcp_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rs->rtt_us;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, (__u32)bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_main, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);


}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_sndbuf_expand, struct sock *sk)
{
	return 3;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr4 = {
	.init		= (void *)bpf_bbr4_init,
	.cong_control	= (void *)bpf_bbr4_main,
	.sndbuf_expand	= (void *)bpf_bbr4_sndbuf_expand,
	.undo_cwnd	= (void *)bpf_bbr4_undo_cwnd,
	.ssthresh	= (void *)bpf_bbr4_ssthresh,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr4",
};
```

</details>

---

## BBR Step 5: PROBE_RTT への遷移とキューの排出（cwnd = 4）

### 学習ゴール

- BBR がなぜ定期的に `BBR_PROBE_RTT` に入り、インフライトを最小パイプサイズ（`BBR_MIN_PIPE_CWND = 4`）まで絞り込むのか、その必然性と制御フローを理解します。
- ボトルネックのバッファにパケットが滞留し続ける「常時滞留キュー（standing queue）」と、推定伝播遅延 `min_rtt_us`（RTprop）の陳腐化を解消する仕組みを学びます。
- `BBR_MIN_RTT_WIN_US`（カーネル標準では 10 秒、本ラボでは 2 秒に短縮）によるウィンドウ有効期限の判定ロジックを実装します。
- `BBR_PROBE_RTT` モードにおいて、パケット滞留を確実に抜くための 2 つの完了条件（200 ms 以上の滞在、かつインフライト <= 4 でのパケット往復 1 ラウンド完了）を BPF で実装します。
- モード突入前の輻輳ウィンドウを `ca->prior_cwnd` に退避し、脱出時に復元するメカニズム、および `bbr_set_cwnd()` でのクランプ処理を理解します。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

#### Q1. なぜ `BBR_PROBE_BW` だけでは `min_rtt_us` が過大評価されたまま膠着してしまうのか？
`BBR_PROBE_BW` では、ゲインサイクル（1.25 で帯域を探り、0.75 で排出し、残り 6 フェーズを 1.0 で維持）によって一時的なキューを解消しようとします。しかし、他のフローが後から合流してきた場合や、ネットワークの経路変更などで本当の最小伝播遅延（RTprop）が変化した場合、`BBR_PROBE_BW` の定常状態だけではなぜ真の最小 RTT を計測し直せないのでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
`BBR_PROBE_BW` のフェーズ 1（pacing gain 0.75）は、自フローがフェーズ 0 で積んだ余剰キュー（0.25 × BDP）を抜くだけです。ボトルネックを共有する他フローがキューを作っている場合、自フローがインフライトを BDP（1.0 倍）までしか下げなければ、ボトルネックキューは完全に空（0 パケット）にはなりません。

**答え**:
`BBR_PROBE_BW` は常にインフライトを 1.0 × BDP 以上（探り時は 1.25 × BDP）に保つため、ボトルネックに常時滞留するキュー（standing queue）が存在すると、どのパケットもそのキュー遅延を被ってしまい、測定される RTT サンプルは常に `真の RTprop + キュー遅延` となってしまいます。
特に複数フローが競合している環境では、どのフローも「キューが空になった瞬間」を観測できません。そのため、全フローが定期的に自発的にインフライトを限界まで削り、ボトルネックキューを強制的に空に排出する特別なモード（`BBR_PROBE_RTT`）が不可欠となります。
</details>

---

#### Q2. `min_rtt` の有効期限と `BBR_PROBE_RTT` の滞在タイマーを管理するために、どのような状態変数が必要か？
`struct bbr_ca` には、最小 RTT のタイムスタンプや `BBR_PROBE_RTT` の状態を追跡するためにどのようなフィールドを用意する必要があるでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
- 最後に最小 RTT を更新した時刻
- `BBR_PROBE_RTT` に入った後、いつまでそのモードを維持すべきかの終了時刻
- `BBR_PROBE_RTT` 中にインフライトが 4 パケット以下になった状態で 1 ラウンド経過したかを示すフラグ
- `BBR_PROBE_RTT` を抜けた後に元の送信規模へ戻すための退避変数

**答え**:
以下の状態変数を使用します：
- `ca->min_rtt_stamp`: 最後に `min_rtt_us` が更新された時刻（`tp->tcp_mstamp`）。この時刻から `BBR_MIN_RTT_WIN_US` が経過するとフィルタ失効と判定されます。
- `ca->probe_rtt_done_stamp`: `BBR_PROBE_RTT` モードを終了してよい時刻（usec）。突入直後は `0`（タイマー未開始）とし、インフライトが `BBR_MIN_PIPE_CWND` 以下に落ちた瞬間に `tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` にセットされます。
- `ca->probe_rtt_round_done`: インフライトが 4 パケット以下の状態でパケット往復が 1 ラウンド完了したかどうかを記録するフラグ。
- `ca->prior_cwnd`: `BBR_PROBE_RTT` に入る直前の `tp->snd_cwnd`。モード脱出時に以前の良好な cwnd を復元するために使用します。
</details>

---

#### Q3. なぜ `BBR_PROBE_RTT` は「200 ms 経過」だけでなく、「インフライト <= 4 の状態でのパケット往復 1 ラウンド完了」の両方を必須とするのか？
単にタイマーで 200 ms 待つだけでは不十分な理由は何でしょうか？ また、なぜ「インフライトが 4 以下になってから」カウントを開始する必要があるのでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
`snd_cwnd = 4` に設定した瞬間、すでに送信済みのフライト中パケット（数十〜数百パケット）は一瞬では消えません。パイプから抜けるには時間がかかります。また、高遅延リンク（衛星通信など RTT が 500 ms ある環境）では 200 ms の間にパケットが 1 往復すらできません。

**答え**:
1. **キューが実際に抜けるまでの遅延**: `snd_cwnd = 4` に絞っても、ネットワーク内に残っていた古いパケットが受信側に届いて ACK が返ってくるまで、キューは空になりません。そのため、インフライトが実際に `BBR_MIN_PIPE_CWND`（4）以下に減ったことを確認してからタイマー（200 ms）を開始する必要があります。
2. **長 RTT リンクへの対応**: RTT が 200 ms より大きい長遅延リンクでは、200 ms だけ待っても「インフライトが 4 の状態で送ったパケット」の ACK（つまりキューが空の状態で計測された真の最小 RTT サンプル）が送信元に届きません。
したがって、`max(200 ms, 1 ラウンド)`、すなわち「少なくとも 200 ms 経過」かつ「インフライト <= 4 での往復が 1 ラウンド完了（`probe_rtt_round_done`）」の両方を満たして初めてモードを脱出します。
</details>

---

#### Q4. なぜ `bbr_save_cwnd()` で `prior_cwnd` を退避し、`bbr_set_cwnd()` では関数の末尾で `BBR_MIN_PIPE_CWND` にクランプするのか？
`BBR_PROBE_RTT` 中の `snd_cwnd` の設定方法と、モード終了時の復元方法について考えてみましょう。通常計算の途中で `snd_cwnd = 4` を上書きするのではなく、なぜ関数の末尾でクランプするのでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
`bbr_set_cwnd()` の通常のロジック（BDP 計算、スロースタート時の加算、`snd_cwnd_clamp` など）が実行された後で、最終的に `BBR_PROBE_RTT` の制約（4 パケット）を強制する必要があります。また、脱出時には即座に元の送信規模（数十〜数百パケット）に戻せないと、帯域プローブをゼロからやり直すことになってしまいます。

**答え**:
- **`bbr_save_cwnd()`**: `BBR_PROBE_RTT` に突入する直前の正常なパイプ規模（`tp->snd_cwnd`）を `ca->prior_cwnd` に退避します。これにより、200 ms のプローブ終了後に即座に元の送信規模へジャンプでき、無駄なスロースタートのオーバーヘッドを回避できます。
- **末尾クランプ**: `bbr_set_cwnd()` では、ACK 処理に伴う通常の cwnd 計算（`target_cwnd = bbr_inflight()` など）を行った後、関数の出口（`done:` ラベル以降）で `ca->mode == BBR_PROBE_RTT` の場合に `tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND)` を適用します。これにより、ACK 受信時の通常計算ロジックを破壊することなく、送信ウィンドウを確実に 4 パケット以下に抑え込みます。
</details>

---

### 実装ステップと解説

#### 1. 定数と数学的背景

Linux カーネルの `net/ipv4/tcp_bbr.c` では以下の定数が定義されています：

- `bbr_min_rtt_win_sec = 10` (10 秒)
- `bbr_probe_rtt_mode_ms = 200` (200 ミリ秒)
- `bbr_cwnd_min_target = 4` (4 パケット)

**2% のスループットペナルティの設計根拠**:
定常状態の BBR フローは、10 秒ごとに 200 ms だけ `BBR_PROBE_RTT` に入ります：
$$
\frac{200\text{ ms}}{10\text{ s}} = \frac{0.2\text{ s}}{10\text{ s}} = 2\%
$$
つまり、スループットの理論的な低下を約 2% のオーバーヘッドに抑えながら、定期的にボトルネックキューを完全に排出し、真の RTprop を再計測します。

> [!NOTE]
> 本ラボでは `packetdrill` テストを約 2.5 秒で高速に完了させるため、`BBR_MIN_RTT_WIN_US` を `2 * USEC_PER_SEC`（2 秒）に設定しています。論理構造やステートマシンは Linux カーネルと全く同一です。

なぜ最小パイプサイズ（`BBR_MIN_PIPE_CWND`）は **4 パケット** なのでしょうか？
TCP では遅延 ACK（delayed ACK）により、受信側は通常 2 パケットに 1 回 ACK を返します。もしウィンドウを 1 や 2 パケットにしてしまうと、受信側の delayed ACK タイマー（最大 40〜200 ms）の満了待ちが発生してパイプが失速します。4 パケットあれば、常に少なくとも 2 回の ACK トリガーが発生するため、パイプを途切れさせることなくスムーズに送受信を維持できます。

---

#### 2. `bbr_update_min_rtt()`: 最小 RTT の追跡と PROBE_RTT 制御

```c
static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	// 1. 最後に min_rtt を更新してから 2 秒（カーネルでは 10 秒）経過したか判定
	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;

	// 2. より小さい RTT サンプルが得られたか、あるいはウィンドウが失効した場合は更新
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	// 3. ウィンドウが失効し、まだ PROBE_RTT でなければ PROBE_RTT に突入
	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	// 4. PROBE_RTT モード中の滞在・脱出判定
	if (ca->mode == BBR_PROBE_RTT) {
		// 低レートのサンプルが max_bw を汚染しないよう app_limited 扱いにする
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;

		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			// インフライトが 4 以下になった瞬間に 200 ms タイマーを開始
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}
```

- **`<` による比較**: `(__u32)rs->rtt_us < ca->min_rtt_us` で厳密な不等号を使うのは重要です。同じ値のサンプルでタイムスタンプを更新し続けると、永遠にフィルタが失効しなくなってしまうためです。
- **delayed ACK の除外**: ウィンドウ失効時の更新において、`!rs->is_ack_delayed` を確認します。遅延 ACK の遅延時間を含んだ RTT を最小 RTT として採用してしまうと、RTprop を大幅に過大評価してしまうためです。
- **`tp->app_limited` のマーキング**: cwnd = 4 に絞っている間のレートサンプルは極めて小さな値になります。これが `ca->max_bw` を下方に更新しないよう、アプリ制限（app-limited）扱いにして帯域フィルタを保護します。

---

#### 3. `bbr_check_probe_rtt_done()` と `bbr_reset_mode()`: 脱出と復元

```c
static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}
```

- `tp->tcp_mstamp > ca->probe_rtt_done_stamp` により、200 ms の滞在を満たしたことを確認します。
- `ca->min_rtt_stamp = tp->tcp_mstamp` により、たとえ新しい最小 RTT が見つからなかった場合でも、直後に再び `BBR_PROBE_RTT` に入ってしまうのを防ぎます（次のプローブは再びウィンドウ期間後になります）。
- `tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)` により、退避していた元の送信ウィンドウへ復元します。
- `bbr_reset_mode()` により、パイプが満たされていれば `BBR_PROBE_BW`（サイクルインデックス 0）へ復帰します。

---

#### 4. `bbr_set_cwnd()` でのクランプ

```c
static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	// ... 通常の cwnd 計算 ...
done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}
```

通常の ACK 処理による cwnd 更新が終わった後、`BBR_PROBE_RTT` 中であれば `tp->snd_cwnd` を `BBR_MIN_PIPE_CWND`（4）以下に強制クランプします。

---

### packetdrill テストの解説 (`test.pkt`)

`test.pkt` では、以下のシナリオをミリ秒単位で検証します：

1. **接続確立とウィンドウ成長**:
   - 初期 RTT = 10 ms で接続を確立します。
   - 10 パケット（10000 バイト）を送信し、ACK を受信することで `snd_cwnd` が 10 から 20 へ成長します。
   - `assert tcpi_snd_cwnd == 20` で確認します。

2. **2.1 秒経過とフィルタ失効**:
   - `+2.100` 秒進めます（`BBR_MIN_RTT_WIN_US = 2` 秒を超過）。
   - 1 パケット送信し、RTT = 15 ms（> 10 ms）で ACK を返します。
   - より小さい RTT サンプルは得られず、フィルタウィンドウが失効したため、BBR は `BBR_PROBE_RTT` へ突入します。
   - `assert tcpi_snd_cwnd == 4` により、cwnd が 4 にクランプされたことを検証します。

3. **0.25 秒経過と PROBE_RTT 脱出**:
   - `+0.250` 秒進めます（`BBR_PROBE_RTT_MODE_US = 200` ms を超過）。
   - 1 パケット送信し、RTT = 10 ms で ACK を返します。これにより 1 ラウンドが完了します（`probe_rtt_round_done = 1`）。
   - 滞在時間（200 ms）と 1 ラウンド完了の両方が満たされたため、BBR は `BBR_PROBE_RTT` を脱出します。
   - `assert tcpi_snd_cwnd >= 20` により、退避されていた `prior_cwnd`（20）以上に復元されたことを検証します。

---

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step05-probe-rtt starter
## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step05-probe-rtt solution
```

- `starter.bpf.c` では `filter_expired` や `BBR_PROBE_RTT` への遷移、`snd_cwnd` のクランプが未実装のため、`test.pkt` の 2.1 秒経過後のアサート（`assert tcpi_snd_cwnd == 4`）で失敗します。
- `solution.bpf.c`（または `TODO` 実装後の `starter.bpf.c`）では全ての検証アサートをパスし、`=== [PASS] bbr/step05-probe-rtt (solution) ===` が出力されます。


### `labs/bbr/step05-probe-rtt/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 5: Transition to PROBE_RTT & BBR_MIN_PIPE_CWND (cwnd = 4)
 *
 * Periodically drains the bottleneck queue when min_rtt_us has not been
 * refreshed within BBR_MIN_RTT_WIN_US.
 *
 * Note: Linux kernel tcp_bbr.c uses bbr_min_rtt_win_sec = 10 seconds.
 * In this lab we set BBR_MIN_RTT_WIN_US = 2 * USEC_PER_SEC (2 seconds)
 * so packetdrill tests complete in ~2.5 seconds instead of waiting 10s.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

/*
 * Kernel tcp_bbr.c uses 10 seconds (bbr_min_rtt_win_sec = 10).
 * We use 2 seconds in this lab so packetdrill runs quickly.
 */
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	/*
	 * TODO: Implement BBR_PROBE_RTT logic and min_rtt window expiration:
	 * 1. Compute filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US.
	 * 2. Update ca->min_rtt_us and ca->min_rtt_stamp when rs->rtt_us >= 0 and
	 *    either ((__u32)rs->rtt_us < ca->min_rtt_us) or (filter_expired && !rs->is_ack_delayed).
	 * 3. When filter_expired && ca->mode != BBR_PROBE_RTT, save cwnd (bbr_save_cwnd(sk)),
	 *    set ca->mode = BBR_PROBE_RTT, and reset ca->probe_rtt_done_stamp = 0.
	 * 4. While ca->mode == BBR_PROBE_RTT, maintain the probe phase:
	 *    - Set tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
	 *    - If !ca->probe_rtt_done_stamp && tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND:
	 *        ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
	 *        ca->probe_rtt_round_done = 0;
	 *        ca->next_rtt_delivered = tp->delivered;
	 *    - Else if ca->probe_rtt_done_stamp:
	 *        if ca->round_start: ca->probe_rtt_round_done = 1;
	 *        if ca->probe_rtt_round_done && tp->tcp_mstamp > ca->probe_rtt_done_stamp:
	 *            ca->min_rtt_stamp = tp->tcp_mstamp;
	 *            tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	 *            bbr_reset_mode(sk);
	 */
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	/* TODO: If ca->mode == BBR_PROBE_RTT, clamp tp->snd_cwnd to BBR_MIN_PIPE_CWND (4). */
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr5 = {
	.init		= (void *)bpf_bbr5_init,
	.cong_control	= (void *)bpf_bbr5_cong_control,
	.ssthresh	= (void *)bpf_bbr5_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr5_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr5",
};
```


### `labs/bbr/step05-probe-rtt/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 5: BBR_PROBE_RTT transition, clamping cwnd to 4, and restoring prior_cwnd

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr5`

// Establish a connection with base RTT = 10 ms
+0.000 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.000 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.000 bind(3, ..., ...) = 0
+0.000 listen(3, 1) = 0

+0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
+0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
+0.010 < . 1:1(0) ack 1 win 512
+0.000 accept(3, ..., ...) = 4

// Send 10 segments at RTT = 10 ms to grow cwnd from 10 to 20
+0.000 write(4, ..., 10000) = 10000
+0.000 > P. 1:10001(10000) ack 1
+0.010 < . 1:1(0) ack 10001 win 512
+0.000 %{
assert tcpi_snd_cwnd == 20, f"expected cwnd == 20, got {tcpi_snd_cwnd}"
}%

// Advance +2.1s (> BBR_MIN_RTT_WIN_US = 2s) and ACK with RTT = 15 ms (> 10 ms)
// so min_rtt_us is not refreshed early and the filter window expires.
+2.100 write(4, ..., 1000) = 1000
+0.000 > P. 10001:11001(1000) ack 1
+0.015 < . 1:1(0) ack 11001 win 512

// BBR must enter BBR_PROBE_RTT and clamp cwnd to BBR_MIN_PIPE_CWND (4)!
+0.000 %{
assert tcpi_snd_cwnd == 4, f"expected cwnd == 4 in PROBE_RTT, got {tcpi_snd_cwnd}"
}%

// Wait +0.25s (> BBR_PROBE_RTT_MODE_US = 200 ms) with in_flight <= 4
// and complete 1 packet-timed round trip at RTT = 10 ms.
+0.250 write(4, ..., 1000) = 1000
+0.000 > P. 11001:12001(1000) ack 1
+0.010 < . 1:1(0) ack 12001 win 512

// BBR exits BBR_PROBE_RTT and restores cwnd >= prior_cwnd (20)!
+0.000 %{
assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step05-probe-rtt/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 5: Transition to PROBE_RTT & BBR_MIN_PIPE_CWND (cwnd = 4)
 *
 * Periodically drains the bottleneck queue when min_rtt_us has not been
 * refreshed within BBR_MIN_RTT_WIN_US.
 *
 * Note: Linux kernel tcp_bbr.c uses bbr_min_rtt_win_sec = 10 seconds.
 * In this lab we set BBR_MIN_RTT_WIN_US = 2 * USEC_PER_SEC (2 seconds)
 * so packetdrill tests complete in ~2.5 seconds instead of waiting 10s.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

/*
 * Kernel tcp_bbr.c uses 10 seconds (bbr_min_rtt_win_sec = 10).
 * We use 2 seconds in this lab so packetdrill runs quickly.
 */
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr5 = {
	.init		= (void *)bpf_bbr5_init,
	.cong_control	= (void *)bpf_bbr5_cong_control,
	.ssthresh	= (void *)bpf_bbr5_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr5_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr5",
};
```

</details>

---

## Step 6: Loss 時の cwnd 保存・復元と ACK 集約 (extra_acked) 補償

このステップでは、BBRv1 のパケットロス時の挙動（Packet Conservation と `prior_cwnd` の復元）および、Wi-Fi や LRO/GRO 環境で生じる ACK 集約（ACK Aggregation）への適応アルゴリズムを実装します。

---

### 学習ゴール

- **BBR と Reno/CUBIC のロス応答の違いの理解**: なぜ BBR はパケットロスが発生しても `cwnd` を半減させず、送信レート（ペーシングレート）を変更しないのかを理解します。
- **Recovery 第 1 ラウンドのパケット保存則（Packet Conservation）**: ロス検出直後に一時的に「1 個 ACK されたら 1 個送る」保存則（Packet Conservation）を適用し、安全にパイプをドレインする方法を学びます。
- **Fast Recovery 終了後の `prior_cwnd` 復元**: ロス復旧が完了した際、事前退避した `prior_cwnd` へ安全にウィンドウを復元する仕組みを習得します。
- **ACK 集約（`extra_acked`）の推定と補正**: バースト ACK やストレッチ ACK によって生じる一時的な cwnd 制約によるスループット低下を防ぐため、超過 ACK 数を計測して `target_cwnd` に上乗せする機構を理解します。

---

### 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

#### Q1. なぜ BBR は Reno や CUBIC のようにロス検出で `cwnd` やペーシングレートを永続的に半減させないのでしょうか？
- ヒント: BBR のネットワークモデル（BtlBw と RTprop）において、パケットロスはどのように扱われているでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: Reno や CUBIC はパケットロスを「バッファ溢れ＝輻輳」の直接的な証拠とみなしてウィンドウを 50%〜30% 削減します。しかし現代のネットワーク（特に浅いバッファのスイッチや無線リンク）では、輻輳とは無関係なランダムロスや一時的なバッファ競合が日常的に発生します。
- BBR はボトルネック容量を物理量（`BtlBw` と `RTprop`）として直接計測・モデル化しています。パケットロスが発生したからといって回線の帯域（`BtlBw`）や伝搬遅延（`RTprop`）自体が縮むわけではありません。そのため、BBR はパケットロスを理由に永続的に帯域推定（`max_bw`）やペーシングレートを引き下げることはせず、ロスしたパケット数を一時的に差し引くにとどめます。
</details>

---

#### Q2. Fast Recovery 突入時の第 1 ラウンドで「パケット保存則（Packet Conservation）」が必要な理由と、終了後に `prior_cwnd` を復元する仕組みはどうなっていますか？
- ヒント: パケットロスが起きた瞬間、ネットワーク内の実フライト数はどうなっていますか？ また、復旧完了後に `cwnd` が切り詰められたままだと何が起こるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: ロスが検知された瞬間、パイプ内には実際に欠損したパケットが存在しており、過剰な再送やバースト送信を行うとさらなるロスを招きます。そこで BBR は Recovery 突入直後の 1 ラウンドトリップ（RTT）の間、`cwnd = tcp_packets_in_flight(tp) + max(acked, 1)` に設定し、「ACK が届いてネットワークからパケットが抜けた分だけ新しく送信を許可する」パケット保存則（Packet Conservation）を適用します。
- そして Recovery から通常の `TCP_CA_Open` に復帰した際は、Recovery 前に退避していた `ca->prior_cwnd` を使って `cwnd = max(cwnd, ca->prior_cwnd)` と復元します。これにより、不要なスループット低下を防ぎ、即座に本来の BDP 目標ウィンドウへ戻ることができます。
</details>

---

#### Q3. なぜ `.set_state` フックでは `TCP_CA_Recovery` ではなく `TCP_CA_Loss`（RTO 発生時）のみを拾って状態を更新するのでしょうか？
- ヒント: Fast Recovery（SACK/dupACK）と RTO（タイムアウト）の重大度の違いは何でしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: 3 重重複 ACK などによる Fast Recovery（`TCP_CA_Recovery`）は、ACK が継続的に返ってきており、パイプが完全に止まっているわけではありません。そのため、通常の `.cong_control` フック内の `rate_sample` から状態遷移とパケット保存則をシームレスに処理できます。
- 一方、RTO タイムアウト（`TCP_CA_Loss`）はネットワークが完全に停止したかパイプが完全に空になったことを意味します。このときは ACK が届かないため `.cong_control` は呼ばれず、カーネルのタイマーパスから `.set_state` が直接呼び出されます。BBR は `.set_state` で `new_state == TCP_CA_Loss` を検知したときのみ、`ca->full_bw = 0` や `ca->round_start = 1` をリセットしてモデルを安全に再初期化します。
</details>

---

#### Q4. Wi-Fi や GRO（Generic Receive Offload）における ACK 集約（ACK Aggregation）とは何であり、BBR はどのように `extra_acked` を計算して補正しているでしょうか？
- ヒント: 本来 1 パケットごとに分散して届くはずの ACK が、受信側や中継機器でまとめられて一度にドッと届くと、送信側のレートやウィンドウ計算にどのような影響が出るでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: 無線 LAN のフレームアグリゲーションや NIC の LRO/GRO により、複数の TCP ACK が 1 つにまとめられてバースト的に受信される現象を ACK 集約と呼びます。ACK が集約されると、ACK が来ない間は送信が一時的に止まり、集約 ACK 到着時に大量のデータが一気に ACK されます。
- `cwnd` が純粋な BDP（`BtlBw * RTprop`）ぴったりに設定されていると、集約によって ACK が間欠的になった際に送信側が cwnd 制限にかかり、パイプが枯渇してスループットが低下します。
- そこで BBR はサンプリングエポック内の経過時間 `epoch_us` から期待される ACK 数 `expected_acked = max_bw * epoch_us` を計算し、実際に届いた ACK 数との差分 `extra_acked = ack_epoch_acked - expected_acked` を計測します。この `extra_acked`（直近 5 RTT の最大値、上限は BDP）を `target_cwnd` に上乗せすることで、ACK 集約環境でも途切れなくペーシング送信を継続できます。
</details>

---

### 実装ステップと解説

#### 1. `struct bbr_ca` の拡張
ACK 集約とロス復旧を追跡するために、以下のメンバを追加します：
```c
struct bbr_ca {
	...
	__u64 epoch_mstamp;		/* ACK 集約サンプリングエポック開始時刻 */
	__u32 ack_epoch_acked;		/* エポック内で (S)ACK された累積パケット数 */
	__u32 extra_acked;		/* エポック内の最大超過 ACK パケット数 */
	__u8  extra_acked_win_rtts;	/* extra_acked の経過 RTT 数（5 RTT で減衰） */
	__u8  prev_ca_state;		/* 直前の ACK 処理時の CA 状態 */
	__u8  packet_conservation;	/* Recovery 第 1 ラウンドのパケット保存則フラグ */
};
```

#### 2. `bbr_save_cwnd()` による事前 cwnd 退避
Recovery や PROBE_RTT に突入する直前の健全な cwnd を保存します：
```c
static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}
```

#### 3. `bbr_set_cwnd_to_recover_or_restore()` によるロス制御
- パケットロス発生時は、ロス分のみを `cwnd` から減算します（下限 1）。
- `TCP_CA_Recovery` 突入時は `packet_conservation = 1` とし、`cwnd = tcp_packets_in_flight(tp) + max(acked, 1)` を強制します。
- `TCP_CA_Recovery` から抜けた際は、`ca->prior_cwnd` まで `cwnd` を復元します。

#### 4. `bbr_update_ack_aggregation()` による集約推定
- 1 ラウンドごとに `extra_acked_win_rtts` をインクリメントし、5 RTT が経過したら `extra_acked` をリセットして古い推定値を減衰させます。
- エポック内の実際の ACK 数が期待 ACK 数を超えた場合、その差分を `extra_acked` の最大値として記録します。
- `bbr_set_cwnd()` 内で `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));` を加算します。

---

### packetdrill テストの解説 (`test.pkt`)

`test.pkt` は以下のシナリオを自動検証します：

1. **初期スロースタート**:
   - 10 パケット（10,000 バイト）を送信し、10 ms RTT で ACK されることで `tcpi_snd_cwnd` が 10 から 20 以上に成長することを確認します。
2. **Fast Recovery のトリガー**:
   - 4 パケット（10001:14001）を送信後、先頭の 1 パケットがドロップし、後続 3 パケットに対する SACK 重複 ACK を受信します。
   - Linux TCP が Fast Recovery（`TCP_CA_Recovery`）に移行し、高速再送（`10001:11001`）を行うことを検証します。
3. **パケット保存則の検証**:
   - 再送直前の時点でパイプ内のフライト数が 0 となり、`max(acked, 1) == 1` であるため、`tcpi_snd_cwnd` が一時的に 1 に設定されていることをアサートします。
4. **prior_cwnd の復元検証**:
   - 全データに対する累積 ACK（`ack 14001`）が到着して Recovery から `TCP_CA_Open` に戻った際、Reno/CUBIC のように cwnd が半減（10 付近）されるのではなく、ロス前の健全な `tcpi_snd_cwnd >= 20` に復元されていることをアサートします。

---

### 実行方法

```sh
## 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step06-loss-ack-aggregation starter

## 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```


### `labs/bbr/step06-loss-ack-aggregation/starter.bpf.c`（穴埋めスケルトン）

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 6 (Starter): Loss Recovery, Packet Conservation, & ACK Aggregation
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

#define BBR_EXTRA_ACKED_WIN_RTTS	5U
#define BBR_ACK_EPOCH_ACKED_RESET_THRESH (1U << 20)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT or Recovery */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* use packet conservation in 1st round of Recovery? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	/*
	 * TODO: Save cwnd before Recovery and PROBE_RTT:
	 * If ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT,
	 * save ca->prior_cwnd = tp->snd_cwnd.
	 * Otherwise keep ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd).
	 */
	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
		ca->packet_conservation = 0;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_ack_aggregation(struct sock *sk,
						       const struct rate_sample *rs)
{
	(void)sk;
	(void)rs;
	/*
	 * TODO: ACK aggregation estimation:
	 * 1. If rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0, return.
	 * 2. On round_start: increment ca->extra_acked_win_rtts.
	 *    If ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS, reset both
	 *    extra_acked_win_rtts = 0 and extra_acked = 0.
	 * 3. Compute epoch_us = tp->delivered_mstamp - ca->epoch_mstamp.
	 *    expected_acked = ((u64)ca->max_bw * epoch_us) >> BW_SCALE.
	 * 4. If ca->ack_epoch_acked <= expected_acked or
	 *    ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH:
	 *    Reset ca->ack_epoch_acked = 0, ca->epoch_mstamp = tp->delivered_mstamp, expected_acked = 0.
	 * 5. ca->ack_epoch_acked += rs->acked_sacked.
	 *    extra_acked = min(ca->ack_epoch_acked - expected_acked, tp->snd_cwnd).
	 *    If extra_acked > ca->extra_acked: ca->extra_acked = extra_acked.
	 */
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, __u32 acked, __u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd;

	(void)ca;
	(void)acked;
	/*
	 * TODO: Implement BBR Loss Recovery and cwnd restoration:
	 * 1. Subtract rs->losses from cwnd (floor 1).
	 * 2. On entry to TCP_CA_Recovery (state == Recovery && prev_state != Recovery):
	 *    enable packet_conservation = 1, set next_rtt_delivered = tp->delivered,
	 *    and cwnd = packets_in_flight + max(acked, 1).
	 * 3. On exit from Recovery (prev_state >= Recovery && state < Recovery):
	 *    restore cwnd = max(cwnd, ca->prior_cwnd), clear packet_conservation = 0.
	 * 4. Update ca->prev_ca_state = state.
	 * 5. If ca->packet_conservation is active, set *new_cwnd = max(cwnd, packets_in_flight + max(acked, 1))
	 *    and return true (packet conservation mode).
	 *
	 * Placeholder before TODO is completed: halve cwnd on loss like Reno/CUBIC.
	 */
	if (rs->losses > 0)
		cwnd = max(cwnd >> 1U, 2U);

	*new_cwnd = cwnd;
	return false;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	/*
	 * TODO: Add extra_acked compensation for ACK aggregation:
	 * target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));
	 */

	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->prev_ca_state = TCP_CA_Open;
	ca->packet_conservation = 0;
	ca->epoch_mstamp = tp->tcp_mstamp;
	ca->ack_epoch_acked = 0;
	ca->extra_acked = 0;
	ca->extra_acked_win_rtts = 0;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_ack_aggregation(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_ssthresh, struct sock *sk)
{
	/*
	 * TODO: Save cwnd via bbr_save_cwnd(sk) before entering loss recovery,
	 * and return tcp_sk(sk)->snd_ssthresh without halving cwnd.
	 */
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_undo_cwnd, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	/*
	 * TODO: Restore snd_cwnd to max(tp->snd_cwnd, ca->prior_cwnd).
	 */
	return tp->snd_cwnd;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_set_state, struct sock *sk, __u8 new_state)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	(void)ca;
	(void)new_state;
	/*
	 * TODO: On new_state == TCP_CA_Loss:
	 *   ca->prev_ca_state = TCP_CA_Loss;
	 *   ca->full_bw = 0;
	 *   ca->round_start = 1;
	 */
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr6 = {
	.init		= (void *)bpf_bbr6_init,
	.cong_control	= (void *)bpf_bbr6_cong_control,
	.ssthresh	= (void *)bpf_bbr6_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr6_undo_cwnd,
	.set_state	= (void *)bpf_bbr6_set_state,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr6",
};
```


### `labs/bbr/step06-loss-ack-aggregation/test.pkt`（packetdrill テスト）

```python
// SPDX-License-Identifier: GPL-2.0
// Step 6: Loss Recovery, Packet Conservation (cwnd = 1), & Prior cwnd Restoration

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr6`

// Establish a connection with base RTT = 10 ms
+0.000 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.000 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.000 bind(3, ..., ...) = 0
+0.000 listen(3, 1) = 0

+0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
+0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
+0.010 < . 1:1(0) ack 1 win 512
+0.000 accept(3, ..., ...) = 4

// Send 10 segments at RTT = 10 ms to grow cwnd from 10 to 20
+0.000 write(4, ..., 10000) = 10000
+0.000 > P. 1:10001(10000) ack 1
+0.010 < . 1:1(0) ack 10001 win 512
+0.000 %{
assert tcpi_snd_cwnd >= 20, f"expected cwnd >= 20, got {tcpi_snd_cwnd}"
assert tcpi_ca_state == TCP_CA_Open, f"expected CA_Open, got {tcpi_ca_state}"
}%

// Send 4 segments (10001:14001)
+0.000 write(4, ..., 4000) = 4000
+0.000 > P. 10001:14001(4000) ack 1

// Receive 3 SACK duplicate ACKs to trigger Fast Recovery
+0.010 < . 1:1(0) ack 10001 win 512 <sack 11001:12001,nop,nop>
+0.000 < . 1:1(0) ack 10001 win 512 <sack 11001:13001,nop,nop>
+0.000 < . 1:1(0) ack 10001 win 512 <sack 11001:14001,nop,nop>

// Fast retransmit of the lost segment (10001:11001)
+0.000 > . 10001:11001(1000) ack 1

// Verify packet conservation in the 1st round of Recovery:
// in_flight was 0, max(acked, 1) = 1, so cwnd = 1!
+0.000 %{
assert tcpi_ca_state == TCP_CA_Recovery, f"expected CA_Recovery, got {tcpi_ca_state}"
assert tcpi_snd_cwnd == 1, f"expected cwnd == 1 under packet conservation, got {tcpi_snd_cwnd}"
}%

// Cumulative ACK acknowledging all 14,000 bytes exits Recovery
+0.010 < . 1:1(0) ack 14001 win 512

// BBR exits Recovery back to TCP_CA_Open and restores cwnd to pre-recovery prior_cwnd (>= 20)
+0.000 %{
assert tcpi_ca_state == TCP_CA_Open, f"expected CA_Open, got {tcpi_ca_state}"
assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>模範解答: <code>labs/bbr/step06-loss-ack-aggregation/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 6: Loss Recovery, Packet Conservation, & ACK Aggregation
 *
 * Implements:
 * 1. Packet conservation during the 1st round trip of TCP_CA_Recovery.
 * 2. Saving and restoring prior_cwnd across Loss Recovery without halving cwnd.
 * 3. ACK aggregation estimation (extra_acked) with windowed decay over
 *    BBR_EXTRA_ACKED_WIN_RTTS round trips.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

#define BBR_EXTRA_ACKED_WIN_RTTS	5U
#define BBR_ACK_EPOCH_ACKED_RESET_THRESH (1U << 20)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT or Recovery */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* use packet conservation in 1st round of Recovery? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
		ca->packet_conservation = 0;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_ack_aggregation(struct sock *sk,
						       const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 epoch_us, expected_acked, extra_acked;

	if (rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (ca->round_start) {
		ca->extra_acked_win_rtts = min(0x1FU, ca->extra_acked_win_rtts + 1U);
		if (ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS) {
			ca->extra_acked_win_rtts = 0;
			ca->extra_acked = 0;
		}
	}

	epoch_us = tp->delivered_mstamp - ca->epoch_mstamp;
	expected_acked = ((__u64)ca->max_bw * epoch_us) >> BW_SCALE;

	if (ca->ack_epoch_acked <= expected_acked ||
	    ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH) {
		ca->ack_epoch_acked = 0;
		ca->epoch_mstamp = tp->delivered_mstamp;
		expected_acked = 0;
	}

	ca->ack_epoch_acked += rs->acked_sacked;
	extra_acked = ca->ack_epoch_acked - expected_acked;
	extra_acked = min(extra_acked, tp->snd_cwnd);
	if (extra_acked > ca->extra_acked)
		ca->extra_acked = extra_acked;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, __u32 acked, __u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u8 prev_state = ca->prev_ca_state;
	__u8 state = inet_csk(sk)->icsk_ca_state;
	__u32 cwnd = tp->snd_cwnd;

	if (rs->losses > 0)
		cwnd = max((__s32)cwnd - rs->losses, 1);

	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		ca->packet_conservation = 1;
		ca->next_rtt_delivered = tp->delivered;
		cwnd = tcp_packets_in_flight(tp) + max(acked, 1U);
	} else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
		cwnd = max(cwnd, ca->prior_cwnd);
		ca->packet_conservation = 0;
	}
	ca->prev_ca_state = state;

	if (ca->packet_conservation) {
		*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U));
		return true;
	}
	*new_cwnd = cwnd;
	return false;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));

	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->prev_ca_state = TCP_CA_Open;
	ca->packet_conservation = 0;
	ca->epoch_mstamp = tp->tcp_mstamp;
	ca->ack_epoch_acked = 0;
	ca->extra_acked = 0;
	ca->extra_acked_win_rtts = 0;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_ack_aggregation(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_undo_cwnd, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return max(tp->snd_cwnd, ca->prior_cwnd);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_set_state, struct sock *sk, __u8 new_state)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (new_state == TCP_CA_Loss) {
		ca->prev_ca_state = TCP_CA_Loss;
		ca->full_bw = 0;
		ca->round_start = 1;
	}
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr6 = {
	.init		= (void *)bpf_bbr6_init,
	.cong_control	= (void *)bpf_bbr6_cong_control,
	.ssthresh	= (void *)bpf_bbr6_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr6_undo_cwnd,
	.set_state	= (void *)bpf_bbr6_set_state,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr6",
};
```

</details>

---
