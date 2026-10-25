# Step 4: PROBE_BW の 8 相ゲインサイクル（帯域探索とキュー排出）

## 学習ゴール

このステップでは、定常状態である **PROBE_BW** モードにおける 8 相のペーシングゲインサイクル（`bbr_pacing_gain[]`）と、各フェーズの遷移判定ロジックを実装します。

- 定常状態で利用可能な帯域の変化に追従し、かつ恒常的なバッファ溜まり（バッファブロート）を防ぐための「8 相ゲインサイクル（`1.25x` → `0.75x` → `1.0x` × 6）」の構造と役割を理解します。
- `bbr_is_next_cycle_phase()` を実装し、巡航フェーズ・探索フェーズ・排出フェーズそれぞれの終了条件（最小 RTT 経過、目標インフライト量到達、ロス発生、キュー排出完了）の違いを理解します。
- `bbr_advance_cycle_phase()` と `bbr_update_cycle_phase()` を実装し、フェーズインデックス（`ca->cycle_idx`）の進行とフェーズ開始時刻（`ca->cycle_mstamp`）の記録を行います。
- `bbr_update_gains()` で `ca->cycle_idx` に応じた `bbr_pacing_gain[]` の設定を行います。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

コードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. 定常状態で常に 1.0x で送り続けず、なぜ 8 相のゲインサイクル（1.25x → 0.75x → 1.0x × 6）を繰り返すのでしょうか？

`BBR_STARTUP` と `BBR_DRAIN` を経てボトルネック帯域（`ca->max_bw`）と最小 RTT（`ca->min_rtt_us`）を正しく計測できた後、なぜずっと `1.0x` のペーシングレートで送り続けてはいけないのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ネットワークの利用可能帯域は時間とともに変動するためです**。
- 他のフローが終了して帯域が空いた場合、送信側が現在の推定帯域（`1.0x`）のまま送り続けていては、新しく空いた帯域の存在に永遠に気づくことができません。
- そこで BBR は、約 8 RTT ごとに一時的にペーシングレートを `1.25x`（+25%）に引き上げて余剰帯域の探索（**Phase 0: 探索フェーズ**）を行います。
- 探索によって余剰帯域がなかった場合、ボトルネックルータに `0.25x BDP` のキューが一時的に溜まります。これを放置すると遅延が増大したままになるため、次のラウンドでペーシングレートを `0.75x`（-25%）に落として過剰キューを即座に排出（**Phase 1: 排出フェーズ**）します。
- その後、推定帯域どおりの `1.0x` で 6 ラウンド巡航（**Phases 2..7: 巡航フェーズ**）し、低遅延・高スループットを維持します。

</details>

### Q2. 探索フェーズ（Phase 0: 1.25x）を終了して排出フェーズへ進む条件は何でしょうか？

探索フェーズ（`ca->pacing_gain > BBR_UNIT`）の終了判定において、単に「1 RTT（`min_rtt_us`）が経過したか」だけでなく、インフライト量（`rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)`）やパケットロス（`rs->losses`）を判定条件に含めるのはなぜでしょうか？

<details>
<summary>ヒントと答え</summary>

- 送信側がアプリ制限（送信バッファ不足）やウィンドウ制限によって十分なパケットを送信できていない場合、1 RTT が経過してもパイプには `1.25x BDP` のデータが実際に注入されていない可能性があります。
- その状態で探索を打ち切ってしまうと、ネットワークに帯域の余力があるかどうかを正しくテストできません。
- したがって、探索フェーズは「少なくとも 1 RTT が経過していること（`is_full_length`）」に加えて、「実際に `1.25x BDP` のインフライトパケットを送り込めたこと（`rs->prior_in_flight >= bbr_inflight(1.25x)`）」または「バッファ溢れによるロスが発生したこと（`rs->losses`）」のいずれかが確認されるまで維持されます。

</details>

### Q3. 排出フェーズ（Phase 1: 0.75x）は 1 RTT 経過を待たずに終了することが許可されている（OR 条件）のはなぜでしょうか？

探索フェーズとは対照的に、排出フェーズ（`ca->pacing_gain < BBR_UNIT`）では `is_full_length || rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)` という OR 条件になっています。なぜ 1 RTT 待たずに早期終了してよいのでしょうか？

<details>
<summary>ヒントと答え</summary>

- 排出フェーズの唯一の目的は、「Phase 0 で注入した余分なキューをボトルネックから抜くこと」です。
- ACK が戻ってきた時点でインフライトパケット数が `1.0x BDP`（`bbr_inflight(sk, ca->max_bw, BBR_UNIT)`）以下まで落ちていれば、ボトルネックの余分なキューはすでに完全に抜け切ったと判断できます。
- キューが抜けた後も `0.75x` で低速送信を続けると、パイプを不必要に過小利用（アンダーラン）してしまうため、目標インフライト量に達した瞬間に即座に排出フェーズを終了し、`1.0x` の巡航フェーズへ移行します。

</details>

### Q4. 実際の BBR では PROBE_BW 開始時に `cycle_idx` をランダムに選ぶのに対し、この演習では 0 から始めるのはなぜでしょうか？また、ランダム化にはどんな目的があるのでしょうか？

製品版の BBR では、`BBR_DRAIN` から `BBR_PROBE_BW` に入る際、`ca->cycle_idx = CYCLE_LEN - 1 - prandom_u32_max(bbr_cycle_rand)` のように巡航フェーズ（2〜7）の間でランダムなオフセットを選びます。なぜランダム化が必要なのでしょうか？

<details>
<summary>ヒントと答え</summary>

- **複数フロー間の探索タイミングの同期（Synchronization）を防ぐためです**。
- もし同じボトルネックを共有するすべての BBR フローが同時に Phase 0（`1.25x`）に入ると、全フローが一斉に 25% 増しのトラフィックを送り出し、大規模なキュー膨張やパケットロスを引き起こしてしまいます。
- 巡航フェーズの開始インデックスをランダムに散らす（脱同期化する）ことで、各フローが異なるタイミングで順番に帯域探索を行うようになり、ネットワーク全体として安定した利用率と低遅延が保たれます。
- 本演習では、packetdrill による決定論的で再現性のあるテスト（Phase 0 → Phase 1 → Phase 2 の遷移検証）を行うため、意図的に `cycle_idx = 0` から固定で開始しています。

</details>

---

## 実装ステップと解説

### 1. `bbr_pacing_gain[]` 配列

BBR の定常状態（`BBR_PROBE_BW`）では、8 相のゲインテーブルを循環します。

```c
#define CYCLE_LEN 8

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

### 2. `bbr_is_next_cycle_phase()` — 各フェーズの終了判定

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

### 3. `bbr_advance_cycle_phase()` と `bbr_update_cycle_phase()` — フェーズの進行

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

### 4. `bbr_update_gains()` でのフェーズゲイン反映

```c
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
```

`BBR_PROBE_BW` において、現在のフェーズインデックス `ca->cycle_idx` に応じたゲインを `bbr_pacing_gain[]` から取得して `ca->pacing_gain` に設定します。`ca->cwnd_gain` は固定で `bbr_cwnd_gain`（`2.0x`）を用います。

---

## packetdrill テストの解説

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

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step04-probe-bw starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step04-probe-bw solution
```
