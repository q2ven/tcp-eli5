# BBR Step 5: PROBE_RTT への遷移とキューの排出（cwnd = 4）

## 学習ゴール

- BBR がなぜ定期的に `BBR_PROBE_RTT` に入り、インフライトを最小パイプサイズ（`BBR_MIN_PIPE_CWND = 4`）まで絞り込むのか、その必然性と制御フローを理解します。
- ボトルネックのバッファにパケットが滞留し続ける「常時滞留キュー（standing queue）」と、推定伝播遅延 `min_rtt_us`（RTprop）の陳腐化を解消する仕組みを学びます。
- `BBR_MIN_RTT_WIN_US`（カーネル標準では 10 秒、本ラボでは 2 秒に短縮）によるウィンドウ有効期限の判定ロジックを実装します。
- `BBR_PROBE_RTT` モードにおいて、パケット滞留を確実に抜くための 2 つの完了条件（200 ms 以上の滞在、かつインフライト <= 4 でのパケット往復 1 ラウンド完了）を BPF で実装します。
- モード突入前の輻輳ウィンドウを `ca->prior_cwnd` に退避し、脱出時に復元するメカニズム、および `bbr_set_cwnd()` でのクランプ処理を理解します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

### Q1. なぜ `BBR_PROBE_BW` だけでは `min_rtt_us` が過大評価されたまま膠着してしまうのか？
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

### Q2. `min_rtt` の有効期限と `BBR_PROBE_RTT` の滞在タイマーを管理するために、どのような状態変数が必要か？
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

### Q3. なぜ `BBR_PROBE_RTT` は「200 ms 経過」だけでなく、「インフライト <= 4 の状態でのパケット往復 1 ラウンド完了」の両方を必須とするのか？
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

### Q4. なぜ `bbr_save_cwnd()` で `prior_cwnd` を退避し、`bbr_set_cwnd()` では関数の末尾で `BBR_MIN_PIPE_CWND` にクランプするのか？
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

## 実装ステップと解説

### 1. 定数と数学的背景

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

### 2. `bbr_update_min_rtt()`: 最小 RTT の追跡と PROBE_RTT 制御

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

### 3. `bbr_check_probe_rtt_done()` と `bbr_reset_mode()`: 脱出と復元

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

### 4. `bbr_set_cwnd()` でのクランプ

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

## packetdrill テストの解説 (`test.pkt`)

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

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step05-probe-rtt starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step05-probe-rtt solution
```

- `starter.bpf.c` では `filter_expired` や `BBR_PROBE_RTT` への遷移、`snd_cwnd` のクランプが未実装のため、`test.pkt` の 2.1 秒経過後のアサート（`assert tcpi_snd_cwnd == 4`）で失敗します。
- `solution.bpf.c`（または `TODO` 実装後の `starter.bpf.c`）では全ての検証アサートをパスし、`=== [PASS] bbr/step05-probe-rtt (solution) ===` が出力されます。
