# Step 5: アイドル後の Epoch シフト (`CA_EVENT_TX_START`)

## 学習ゴール

Step 4 までで、CUBIC の 3 次関数曲線・Fast Convergence・Reno-friendly 領域をすべて備えた輻輳回避（Congestion Avoidance）が完成しました。しかし、ここまでの実装には **実ネットワークで深刻なバーストを引き起こす落とし穴** が 1 つ残っています。

Reno は「到着した ACK の数」だけで `snd_cwnd` を進めるため、アプリケーションが送信を一時停止（アイドル）している間は `snd_cwnd` の時計も自然に止まります。一方、CUBIC は **最後の輻輳イベントからの経過時間 `t = tcp_jiffies32 - ca->epoch_start`** の 3 次関数で `bic_target` を計算します。

もし Congestion Avoidance 中にアプリケーションが 2〜5 秒間データを送らずにアイドル状態になり、その後送信を再開したらどうなるでしょうか？
`ca->epoch_start` が昔の時刻のまま放置されていると、再開後の最初の ACK で経過時間 `t` が一気に数秒分進み、`t - K` の 3 乗が爆発して `bic_target` が跳ね上がります。その結果、アイドル明けのたった 1 RTT で `snd_cwnd` が最大 1.5 倍（`ca->cnt = 2`）まで急膨張してしまいます（[RFC 9438 §5.8](https://www.rfc-editor.org/rfc/rfc9438#section-5.8) が「経過時間 $t$ にアプリケーション制限期間を含めてはならない（MUST NOT）」と規定している理由です）。

このステップでは、`struct tcp_congestion_ops` の `.cwnd_event_tx_start` フック（`CA_EVENT_TX_START` イベント）を実装し、`in_flight == 0` のアイドル状態から送信を再開した瞬間に `ca->epoch_start` をアイドル時間分だけ未来へ平行移動（シフト）させる方法を学びます。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

`starter.bpf.c` を開く前に、以下の 4 つの問いを自分の頭で考えてみてください。

### Q1. どのフックで「アイドルからの送信再開」を検知すべきか？

TCP コアが「現在ネットワーク上に未確認データがない状態（`tcp_packets_in_flight(tp) == 0`）から新しくデータパケットを送信する瞬間」に輻輳制御モジュールへ通知する仕組みは何でしょうか？`struct tcp_congestion_ops` のどのメンバ関数を使えばよいでしょうか？

<details>
<summary>ヒントと答え</summary>

送信パスの `tcp_event_data_sent()`（[net/ipv4/tcp_output.c#L179](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n179)）は、パケット送信時に `tcp_packets_in_flight(tp) == 0` であれば `tcp_ca_event(sk, CA_EVENT_TX_START)` を呼び出します。

`include/net/tcp.h` の `tcp_ca_event()`（[include/net/tcp.h#L1491](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/net/tcp.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1491)）は、`CA_EVENT_TX_START` イベントを専用の `.cwnd_event_tx_start(sk)` フックへディスパッチします（コミット `d1e59a469737` で間接分岐予測の最適化のため `.cwnd_event(sk, ev)` から分離されました）。したがって、`struct tcp_congestion_ops` の `.cwnd_event_tx_start` にハンドラを登録します。

</details>

### Q2. アイドルしていた期間 `delta` をどのフィールドから計算するか？

`struct bpf_bictcp` に新しいタイムスタンプ変数を追加しなくても、`struct tcp_sock` がすでに記録している「最後にデータパケットを送信した時刻」を使ってアイドル期間 `delta`（jiffies 単位）を計算できます。どのフィールドを使い、なぜ `.cwnd_event_tx_start` が呼ばれた時点でまだ古い値が残っているのでしょうか？

<details>
<summary>ヒントと答え</summary>

`tcp_sk(sk)->lsndtime` を使います。`tcp_event_data_sent()` の実装は次の順序になっています。

```c
if (tcp_packets_in_flight(tp) == 0)
	tcp_ca_event(sk, CA_EVENT_TX_START);

tp->lsndtime = now;
```

`tp->lsndtime = now` で現在時刻に上書きする **直前** に `CA_EVENT_TX_START` が発火するため、`.cwnd_event_tx_start(sk)` の中では `tp->lsndtime` は「アイドルに入る直前に最後のデータを送信した時刻（jiffies）」を保持しています。
よって、`__s32 delta = (__s32)(now - tcp_sk(sk)->lsndtime);` でアイドル期間（jiffies）を正確に求められます。

</details>

### Q3. なぜ `ca->epoch_start = 0` にリセットせず、`ca->epoch_start += delta` でシフトするのか？

アイドル明けに経過時間 `t` が爆発するのを防ぐだけなら、`ca->epoch_start = 0` にリセットして新しい Epoch を始め直すという手も思いつきます。なぜリセットではなく `ca->epoch_start += delta` で曲線を時間方向に平行移動させるのでしょうか？

<details>
<summary>ヒントと答え</summary>

もし `ca->epoch_start = 0` にリセットしてしまうと、アイドル明けの最初の ACK で `bictcp_update()` が `ca->bic_K` を再計算して新しい Epoch を最初からやり直します。
CUBIC の凹（concave）領域は「Epoch 開始直後が最も傾きが急で、`W_max` に近づくほど緩やかになる」形をしています。`W_max` 手前の平坦部まで達していたフローがアイドルに入るたびに `epoch_start = 0` にリセットされると、毎回ロス直後のような急勾配の立ち上がりを繰り返してしまいます。
`ca->epoch_start += delta` でアイドル時間分だけ開始時刻を後ろにずらせば、**3 次関数の形と現在位置を保ったままアイドル期間だけを時計から除外** できます（コミット `30927520dbae`）。

</details>

### Q4. なぜ `ca->epoch_start` が現在時刻 `now` より未来になるエッジケースが起きるのか？

`ca->epoch_start += delta` の直後に、`if (after(ca->epoch_start, now)) ca->epoch_start = now;` というクランプ処理が必要です。`delta = now - tp->lsndtime` を足しているだけなのに、なぜ `ca->epoch_start + delta > now`（未来の時刻）になることがあるのでしょうか？また、未来になると何が壊れるでしょうか？

<details>
<summary>ヒントと答え</summary>

`tp->lsndtime` は **送信時** に記録されるのに対し、`ca->epoch_start` は **ACK 受信時**（`bictcp_update()`）に記録されるからです。
たとえば次の順序を考えてみます。

1. 時刻 `T0` に Recovery 終了前の最後のデータを送信する（`tp->lsndtime = T0`）。
2. 時刻 `T1`（`T1 > T0`）に ACK が届いて Recovery を抜け、Congestion Avoidance の最初の ACK で `ca->epoch_start = T1` が記録される（このとき追加の送信データはなく `lsndtime` は `T0` のまま）。
3. そのままアイドルになり、時刻 `T2` に送信を再開する。

このとき `delta = T2 - T0` なので、`ca->epoch_start + delta = T1 + (T2 - T0) = T2 + (T1 - T0) > T2 (now)` となり、`ca->epoch_start` が現在時刻 `now` より `T1 - T0` だけ未来になってしまいます。
もし未来の `ca->epoch_start` のまま同じ jiffy（時刻 `T2`）に ACK が届くと、`bictcp_update()` の `(__s32)(tcp_jiffies32 - ca->epoch_start)` が **負の値** になり、`__u64 t` への代入時に符号拡張されて約 $2^{64}$ という巨大な値に化け、3 次関数が完全に暴走します（コミット `c2e7204d180f`）。これを防ぐために `after(ca->epoch_start, now)` なら `now` にクランプします。

</details>

---

## 実装ステップと解説 (`solution.bpf.c`)

Step 4 までのコード（`bictcp_update()`、`bpf_cubic5_cong_avoid()`、`bpf_cubic5_recalc_ssthresh()`、`bpf_cubic5_state()`、`bpf_cubic5_acked()`、`bpf_cubic5_undo_cwnd()`）はそのまま引き継ぎ、新たに `bpf_cubic5_cwnd_event_tx_start()` を実装して `struct tcp_congestion_ops bpf_cubic5` に登録します。

### 1. `bpf_cubic5_cwnd_event_tx_start()`

```c
SEC("struct_ops")
void BPF_PROG(bpf_cubic5_cwnd_event_tx_start, struct sock *sk)
{
	struct bpf_bictcp *ca = inet_csk_ca(sk);
	__u32 now = tcp_jiffies32;
	__s32 delta;

	delta = now - tcp_sk(sk)->lsndtime;

	/* We were application limited (idle) for a while.
	 * Shift epoch_start to keep cwnd growth to cubic curve.
	 */
	if (ca->epoch_start && delta > 0) {
		ca->epoch_start += delta;
		if (after(ca->epoch_start, now))
			ca->epoch_start = now;
	}
}
```

- `now = tcp_jiffies32`: `bpf_jiffies64()` の下位 32 ビットを取得します。
- `delta = now - tcp_sk(sk)->lsndtime`: 最後のデータ送信からの経過 jiffies を符号付き 32 ビット整数 `__s32` として計算します。
- `if (ca->epoch_start && delta > 0)`:
  - `ca->epoch_start != 0` のチェックにより、Slow Start 中やロス直後でまだ Epoch が始まっていない場合は何もしません（もし `epoch_start == 0` に `delta` を足してしまうと、番兵である `0` が壊れて意図しない時刻から Epoch が始まったことになってしまいます）。
  - `delta > 0` のときだけ `ca->epoch_start += delta;` で Epoch 開始時刻をアイドル期間分だけ後ろへシフトします。
- `if (after(ca->epoch_start, now)) ca->epoch_start = now;`: `bpf_cc_helpers.h` の `after(seq2, seq1)`（`(__s32)(seq1 - seq2) < 0`）を使い、`ca->epoch_start` が `now` を追い越して未来の時刻になった場合は `now` に丸めます。

### 2. `struct tcp_congestion_ops bpf_cubic5` への登録

```c
SEC(".struct_ops")
struct tcp_congestion_ops bpf_cubic5 = {
	.init			= (void *)bpf_cubic5_init,
	.ssthresh		= (void *)bpf_cubic5_recalc_ssthresh,
	.cong_avoid		= (void *)bpf_cubic5_cong_avoid,
	.set_state		= (void *)bpf_cubic5_state,
	.undo_cwnd		= (void *)bpf_cubic5_undo_cwnd,
	.cwnd_event_tx_start	= (void *)bpf_cubic5_cwnd_event_tx_start,
	.pkts_acked		= (void *)bpf_cubic5_acked,
	.flags			= TCP_CONG_NON_RESTRICTED,
	.name			= "bpf_cubic5",
};
```

---

## `packetdrill` テストの解説 (`test.pkt`)

`test.pkt` は以下の 5 フェーズでアイドル後の Epoch シフトの有無を検証します（RTT = 20 ms）。

1. **接続確立と sysctl 設定**:
   - `net.ipv4.tcp_congestion_control=bpf_cubic5` と `net.ipv4.tcp_slow_start_after_idle=0` を設定し、3-way ハンドシェイクで接続を確立します（初期 `cwnd = 10`、`delay_min = 20 ms`）。
2. **Slow Start で `cwnd = 20` まで成長**:
   - 10 セグメント（10,000 バイト）を送信して ACK を受け取り、`tcpi_snd_cwnd == 20` にします。
3. **パケットロスと Fast Recovery で Congestion Avoidance へ遷移**:
   - 20 セグメントを送信し、先頭セグメントのロスと 3 つの SACK で Fast Recovery に入ります。
   - `bpf_cubic5_recalc_ssthresh()` により `last_max_cwnd = 20`、`snd_ssthresh = (20 * 717) / 1024 = 14`、`epoch_start = 0` が設定されます。
   - 累積 ACK（`ack 30001`）で Recovery を抜けると、`tcp_complete_cwr()` が `snd_cwnd = ssthresh = 14` に設定して `TCP_CA_Open` に遷移し、同じ ACK の後半で `bpf_cubic5_cong_avoid()` が呼ばれます。ここで `bictcp_update()` が `ca->epoch_start = tcp_jiffies32` と `ca->bic_K = 2524`（約 2.46 秒）を初期化し、`bic_target = 15`（`ca->cnt = 14`）により `tcpi_snd_cwnd` は `14` から `15` に成長します。
4. **Congestion Avoidance で 1 RTT 分（15 セグメント）を送信・ACK**:
   - アイドルに入る前に 15 セグメント（15,000 バイト）を送信して ACK を受け取り、`tcpi_snd_cwnd == 15` であることを確認します。
5. **2.0 秒間のアイドル後に 15 セグメントを送信・ACK**:
   - `+2.000` 秒間何も送らずに待機した後（`in_flight == 0`）、`write(4, ..., 15000)` で 15 セグメントを送信します。
   - **模範解答（`solution.bpf.c`）の場合**: 最初のセグメント送信時に `bpf_cubic5_cwnd_event_tx_start()` が呼ばれ、`ca->epoch_start` が 2 秒分シフトされます。20 ms 後の ACK 時点でも経過時間 `t` は約 60 ms にとどまり、`bic_target = 15 <= cwnd (15)` のため `ca->cnt = 100 * 15 = 1500`（Reno-friendly 側でも `ca->tcp_cwnd == 15`）となり、`tcpi_snd_cwnd` は **`15` のまま**（`<= 15`）で PASS します。
   - **未実装スケルトン（`starter.bpf.c`）の場合**: `ca->epoch_start` がシフトされないため、2 秒後の ACK 時点で `t` が一気に約 2.06 秒まで進みます。すると `bic_target` が `W_max = 20` に跳ね上がり、`ca->cnt = 15 / (20 - 15) = 3` となって 15 セグメントの ACK 一発で `tcpi_snd_cwnd` が **`20` 以上**へ急騰し、`assert tcpi_snd_cwnd <= 15` で FAIL します。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh cubic/step05-idle-epoch-shift starter

# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh cubic/step05-idle-epoch-shift solution
```
