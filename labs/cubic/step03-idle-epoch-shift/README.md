# Step 3: アイドル後の Epoch シフト (`CA_EVENT_TX_START`)

## 1. 学習ゴール

Step 2 までで、CUBIC の 3 次関数曲線・Fast Convergence・Reno-friendly 領域をすべて備えた輻輳回避（Congestion Avoidance）が完成しました。しかし、ここまでの実装には **アプリケーションが送信を一時停止（アイドル）した後に再開すると、`snd_cwnd` が一瞬で急膨張して深刻なパケットバーストを引き起こす** という実ネットワーク上の大きな落とし穴が残っています。

このステップでは、なぜ「時間ベース」の CUBIC がアイドル期間に弱いのかを基礎から理解し、TCP コアが「未確認データゼロ（`in_flight == 0`）からの送信再開」を通知する `.cwnd_event_tx_start` フック（`CA_EVENT_TX_START` イベント）を使って、エポック開始時刻 `ca->epoch_start` をアイドル期間分だけ未来へ平行移動（シフト）させる仕組みを実装します。

- トラック全体の概要と各ステップの一覧は [CUBIC トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

### 2.1 なぜアイドル期間が CUBIC の窓サイズを暴走させるのか？

従来の TCP Reno（AIMD）は、「到着した ACK の数」だけを数えて `snd_cwnd` を進めます。したがって、アプリケーションがデータを送らずにアイドル状態（ネットワーク上の未確認パケット数 `in_flight == 0`）になっている間は ACK も届かず、Reno の「時計」は自然に停止します。

一方、CUBIC は輻輳回避（Congestion Avoidance）における目標ウィンドウ `bic_target`（ $W_{\text{cubic}}(t)$ ）を、**現在のエポックが始まってからの実経過時間 $t$** の 3 次関数として計算します。

$$W_{\text{cubic}}(t) = C (t - K)^3 + W_{\max}, \quad t = t_{\text{now}} - t_{\\text{epoch}} + d_{\min}$$

ここで、もし Congestion Avoidance の途中でアプリケーションが 2 秒間データを送らずにアイドル状態になり、2 秒後に送信を再開したら何が起きるでしょうか？

`ca->epoch_start` がアイドル前の古い時刻のまま放置されていると、再開後の最初の ACK が届いた瞬間に、経過時間 $t = \text{jiffies}_{32} - \text{ca}\to\text{epoch}_{\text{start}}$ が **アイドルしていた 2 秒分を一気にジャンプ** します。すると $(t - K)^3$ が急増して `bic_target` が跳ね上がり、その間ネットワークの空き帯域を一切プローブしていなかったにもかかわらず、アイドル明けのたった 1 RTT で `snd_cwnd` が最大 1.5 倍（`ca->cnt = 2`）まで急膨張してしまいます。このような理由から、[RFC 9438 Section 5.8](https://www.rfc-editor.org/rfc/rfc9438#section-5.8) は「経過時間 $t$ にアプリケーション制限（アイドル）期間を含めてはならない（MUST NOT）」と規定しています。

### 2.2 解決策：曲線の「リセット」ではなく時間軸方向の「平行移動（シフト）」

アイドル期間 $\Delta_{\text{idle}}$ を経過時間 $t$ から除外するには、送信を再開した瞬間にエポック開始時刻 `ca->epoch_start` をアイドル期間 $\Delta_{\text{idle}}$ の分だけ未来へずらします（平行移動）。

$$t_{\\text{epoch}} \leftarrow t_{\\text{epoch}} + \Delta_{\text{idle}}, \quad \text{ただし } \Delta_{\text{idle}} = t_{\text{now}} - t_{\\text{last}}$$

これにより、再開後の経過時間は次のように計算され、アイドルしていた時間 $\Delta_{\text{idle}}$ がきれいに対消滅します。

$$t_{\text{after}} = t_{\text{now}} - (t_{\\text{epoch}} + \Delta_{\text{idle}}) = (t_{\\text{last}} + \Delta_{\text{idle}}) - t_{\\text{epoch}} - \Delta_{\text{idle}} = t_{\\text{last}} - t_{\\text{epoch}}$$

```text
  cwnd
   ^
20 |               ........---'''      ........---'''
   |            .-'                 .-'
15 |         .-'  <-- 2.0s idle -->'  (再開後も t の位置と傾きを維持)
14 |       .'     [epoch_start を +2.0s シフト]
   +-------+---------------+---------------+-------------> 時間 (jiffies)
       epoch_start     lsndtime          now (再開)
```

ここで重要なのは、**`ca->epoch_start = 0` にリセットして新しいエポックを始め直すのではなく、`ca->epoch_start += delta` でシフトする** という点です。
CUBIC の凹領域（ $t < K$ ）は「エポック開始直後が最も傾きが急で、プラトー（ $W_{\max}$ ）に近づくほど傾きが緩やかになる」形をしています。もしアイドルからの再開のたびに `ca->epoch_start = 0` にリセットして $K$ を再計算してしまうと、すでに $W_{\max}$ 手前の平坦部まで達していたフローであっても、アイドルのたびに「ロス直後のような急勾配の立ち上がり」を最初からやり直すことになってしまいます。`ca->epoch_start += delta` で平行移動すれば、**3 次関数の曲線形状（`ca->bic_K` と `ca->bic_origin_point`）と曲線上の現在位置をそのまま保った状態** で時計だけを一時停止・再開できます。

### 2.3 エッジケース 1：まだエポックが始まっていない場合（`ca->epoch_start == 0`）

CUBIC では、`ca->epoch_start == 0` を「現在アクティブな Congestion Avoidance エポックが存在しない（Slow Start 中、またはロス発生直後でまだ Congestion Avoidance の ACK を受けていない）」ことを表す **番兵（センチネル値）** として使っています。
もし `ca->epoch_start == 0` のときに `delta` を足してしまうと、`ca->epoch_start` が非ゼロの適当な時刻に化けてしまい、次の `bictcp_update()` で `ca->bic_K` や `ca->bic_origin_point` の初期化がスキップされて壊れてしまいます。そのため、シフト処理は必ず **`ca->epoch_start != 0` かつ `delta > 0` のときだけ** 実行します。

### 2.4 エッジケース 2：シフト後の `ca->epoch_start` が現在時刻 `now` より未来になる現象

一見すると、`delta = now - tp->lsndtime` を足しているだけなので `ca->epoch_start + delta` が現在時刻 `now` を超えることはないように思えます。しかし、カーネル内のタイムスタンプが記録されるタイミングの違いにより、**`ca->epoch_start + delta > now`（未来の時刻）になるケース** が実在します。

- `tp->lsndtime` は **データパケットを送信した時刻** に記録されます。
- `ca->epoch_start` は **ACK を受信して `bictcp_update()` が走った時刻** に記録されます。

次のタイムラインを考えてみましょう。

1. 時刻 $T_0$ に、Fast Recovery 終了前の最後のデータパケットを送信します（この時点で `tp->lsndtime` $= T_0$ ）。
2. 時刻 $T_1$ （ $T_1 > T_0$ 、たとえば 1 RTT 後）に累積 ACK が届いて Recovery を抜け、同じ ACK または直後の ACK で `bictcp_update()` が呼ばれて新エポックが開始されます（`ca->epoch_start` $= T_1$ ）。このときアプリケーションに送信待ちデータがなければパケットは送信されないため、`tp->lsndtime` は $T_0$ のまま更新されません。
3. そのままアイドル状態が続き、時刻 $T_2$ （`now` $= T_2$ ）にアプリケーションが新しいデータを送信します。

このとき $\Delta_{\text{idle}} = \text{now} - \text{lsndtime} = T_2 - T_0$ なので、`ca->epoch_start` に $\Delta_{\text{idle}}$ を足すと次のようになります。

$$\text{ca}\to\text{epoch}_{\text{start}} + \Delta_{\text{idle}} = T_1 + (T_2 - T_0) = T_2 + (T_1 - T_0) > T_2 \ (\text{now})$$

なんと、`ca->epoch_start` が現在時刻 `now`（ $T_2$ ）よりも $T_1 - T_0$ だけ **未来の時刻** になってしまいます！

もし `ca->epoch_start` が未来の時刻のまま同じ jiffy 内に ACK が届いて `bictcp_update()` が走ると、174 行目の経過時間計算で致命的なバグが起きます。

```c
t = (__s32)(tcp_jiffies32 - ca->epoch_start) * USEC_PER_JIFFY;
```

`tcp_jiffies32 < ca->epoch_start` のため、`(__s32)(tcp_jiffies32 - ca->epoch_start)` は **負の値** になります。それに `USEC_PER_JIFFY`（unsigned）を掛けて 64 ビット符号なし整数 `__u64 t` に代入すると、負の値が符号拡張されて **約 $2^{64}$ という天文学的な正の数にアンダーフロー（ラップアラウンド）** し、3 次関数の計算が完全に崩壊します。

これを防ぐため、`ca->epoch_start += delta` の直後に「もし `ca->epoch_start` が `now` より未来（`after(ca->epoch_start, now)`）になったら、`ca->epoch_start = now` にクランプ（頭打ち）する」というガードが不可欠です。

### 2.5 具体例（`test.pkt` の数値で計算）

RTT = $20\text{ ms}$ （ $d_{\min} = 20{,}000\ \mu\text{s}$ ）、`cwnd = 20` でロスが起きて `last_max_cwnd` ($W_{\max}$) $= 20$ 、`ssthresh` $= 14$ になったケース（ $K \approx 2.46\text{ s}$ ）を考えます。

- Recovery 完了直後の Congestion Avoidance 開始時点で `cwnd` は $14 \to 15$ に成長し、さらに 1 RTT（ $20\text{ ms}$ ）送受信した時点で経過時間は約 $40\text{ ms}$ 、`cwnd = 15` です。
- ここで **2.0 秒間アイドル** し、再開後に 15 セグメントを送信して $20\text{ ms}$ 後に ACK を受け取ります。
  - **Epoch シフトあり（正常）**: 2.0 秒のアイドル分だけ `ca->epoch_start` が後ろへシフトされるため、ACK 到着時の実質経過時間 $t$ は約 $60\text{ ms}$ + $d_{\min}$ にとどまります。 $t \ll K$ なので `bic_target` は $15$ のままで、`cwnd` は **$15$ を維持** します。
  - **Epoch シフトなし（バグ）**: `ca->epoch_start` が古いままなので、ACK 到着時に $t \approx 2.06\text{ s}$ まで一気に進みます。すると $t$ が $K = 2.46\text{ s}$ に急接近するため `bic_target` が $W_{\max} = 20$ へ跳ね上がり、`ca->cnt = 15 / (20 - 15) = 3` となって、15 セグメントの ACK 一発で `cwnd` が **$15 \to 20$ へ急騰** してしまいます。

### 2.6 カーネルと `bpf_cc_helpers.h` が提供する構造体・フィールド・ヘルパー

セクション 3 の設計問題に取り組む前に、このステップで使うカーネルの仕組みと構造体フィールドを整理しておきましょう（なお、コード内の関数・構造体プレフィックスは歴史的経緯により `bpf_cubic5` となっています）。

| 要素 | 型 / シグネチャ | 役割と重要な性質 |
| :--- | :--- | :--- |
| `.cwnd_event_tx_start` | `void (*)(struct sock *sk)` (`struct tcp_congestion_ops` メンバ) | TCP 送信パスの `tcp_event_data_sent()` において、パケット送信時に **ネットワーク上の未確認パケット数がゼロ（`tcp_packets_in_flight(tp) == 0`）** だった場合に呼び出される専用コールバックです（`CA_EVENT_TX_START` イベント）。 |
| `tcp_sk(sk)->lsndtime` | `__u32` (`struct tcp_sock` メンバ) | 最後にデータパケットを送信した時刻（32 ビット jiffies）です。**重要**: カーネルの `tcp_event_data_sent()` は、`if (tcp_packets_in_flight(tp) == 0) tcp_ca_event(sk, CA_EVENT_TX_START);` を呼んだ **直後の行** で `tp->lsndtime = now;` に更新します。そのため、`.cwnd_event_tx_start()` が実行されている最中は、`tp->lsndtime` にはまだ **「アイドルに入る直前に最後のパケットを送った時刻」** が残っています。 |
| `tcp_jiffies32` | `((__u32)bpf_jiffies64())` | 現在のカーネル時刻（jiffies の下位 32 ビット）です。`tp->lsndtime` や `ca->epoch_start` と同じ 32 ビット jiffies 単位で比較・加減算できます。 |
| `after(seq2, seq1)` | `bool` (`bpf_cc_helpers.h`) | 32 ビット符号なし整数同士をラップアラウンド安全に比較するマクロで、`(__s32)(seq1 - seq2) < 0`（つまり `seq2` が `seq1` より未来である）のとき `true` を返します。`after(ca->epoch_start, now)` で「`ca->epoch_start` が現在時刻 `now` より未来か」を判定できます。 |
| `inet_csk_ca(sk)` | `struct bpf_bictcp *` (`icsk_ca_priv`) | ソケットごとの輻輳制御プライベート領域（`struct inet_connection_sock` の `icsk_ca_priv`）です。`ca->epoch_start`（`__u32`）に現在のエポック開始時刻（jiffies、未開始時は `0`）を保持しています。 |

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の解説を踏まえて、`starter.bpf.c` の TODO を埋めるための 4 つの設計チェックポイントに答えてみましょう。

### Q1. アイドルからの送信再開を検知するフックの選択

ネットワーク上の未確認データがゼロ（`in_flight == 0`）の状態からアプリケーションが送信を再開した瞬間に処理を行いたい場合、`struct tcp_congestion_ops` のどのコールバックを使えばよいでしょうか？

<details>
<summary>ヒントと答え</summary>

セクション 2.6 の表の通り、`struct tcp_congestion_ops` の **`.cwnd_event_tx_start`** フックを使います。
TCP 送信パスの `tcp_event_data_sent()` は、`tcp_packets_in_flight(tp) == 0` の状態でデータパケットを送信するときに `CA_EVENT_TX_START` イベントを発火させ、`.cwnd_event_tx_start(sk)` を呼び出します。`starter.bpf.c` では `bpf_cubic5_cwnd_event_tx_start()` がすでに `.cwnd_event_tx_start` に登録されているため、この関数の中身を実装します。

</details>

### Q2. 新しい変数を追加せずにアイドル期間 `delta` を求める方法

`struct bpf_bictcp` に「最後に送信した時刻」を保存する独自フィールドを追加しなくても、カーネルの `struct tcp_sock` が持っているフィールドからアイドル期間 `delta`（jiffies 単位）を計算できます。どのフィールドを使い、なぜ `.cwnd_event_tx_start` の中で古い時刻が読み取れるのでしょうか？

<details>
<summary>ヒントと答え</summary>

`tcp_sk(sk)->lsndtime` を使います。
セクション 2.6 で解説した通り、カーネルの `tcp_event_data_sent()` は次の順序で処理を行います。

```c
if (tcp_packets_in_flight(tp) == 0)
	tcp_ca_event(sk, CA_EVENT_TX_START);

tp->lsndtime = now;
```

`tp->lsndtime` が現在時刻 `now` に上書きされる **直前** に `.cwnd_event_tx_start(sk)` が呼ばれるため、コールバック内では `tcp_sk(sk)->lsndtime` に「アイドルに入る前の最後の送信時刻」がそのまま残っています。したがって、現在時刻 `now = tcp_jiffies32` との差分を符号付き 32 ビット整数として

```c
delta = now - tcp_sk(sk)->lsndtime;
```

と計算するだけで、アイドルしていた期間（jiffies）を正確に取得できます。

</details>

### Q3. `ca->epoch_start` をシフトする条件と、リセット（`= 0`）しない理由

計算した `delta` を使って `ca->epoch_start` を更新するとき、(1) なぜ `ca->epoch_start = 0` にリセットするのではなく `ca->epoch_start += delta` でシフトするのか、(2) なぜ更新前に `if (ca->epoch_start && delta > 0)` をチェックする必要があるのかを説明してください。

<details>
<summary>ヒントと答え</summary>

1. **リセットではなくシフトする理由（セクション 2.2）**:
   `ca->epoch_start = 0` にリセットすると、次の ACK で `bictcp_update()` が `ca->bic_K` を再計算して新しいエポックを最初からやり直してしまいます。すると、 $W_{\max}$ 手前の緩やかなプラトー付近まで到達していたフローでも、アイドルを挟むたびにエポック初期の急勾配な増加を繰り返してしまいます。`ca->epoch_start += delta` でシフトすれば、3 次関数の曲線形状と現在位置を保ったままアイドル期間だけを時計から除外できます。
2. **`if (ca->epoch_start && delta > 0)` をチェックする理由（セクション 2.3）**:
   `ca->epoch_start == 0` は「まだ Congestion Avoidance エポックが始まっていない（Slow Start 中またはロス直後）」ことを示す番兵です。このときに `delta` を足して非ゼロにしてしまうと、`bictcp_update()` がエポック初期化（`ca->bic_K` と `ca->bic_origin_point` の設定）をスキップして壊れてしまいます。また、時刻の逆転や同一 jiffy での呼び出しを除外するため `delta > 0` も合わせて確認します。

</details>

### Q4. 未来の時刻への飛び出しを防ぐクランプ処理

`ca->epoch_start += delta;` の直後に、なぜ `if (after(ca->epoch_start, now)) ca->epoch_start = now;` というクランプ処理が必要なのでしょうか？クランプしないと `bictcp_update()` のどの計算がどう壊れるでしょうか？

<details>
<summary>ヒントと答え</summary>

セクション 2.4 で見た通り、`tp->lsndtime` は **パケット送信時**（ $T_0$ ）に記録されるのに対し、`ca->epoch_start` は **ACK 受信時**（ $T_1 > T_0$ ）に記録されます。もし時刻 $T_0$ の送信後に追加送信がないまま時刻 $T_1$ の ACK でエポックが始まり、時刻 $T_2$ （`now`）までアイドルだった場合、

$$\text{ca}\to\text{epoch}_{\text{start}} + \text{delta} = T_1 + (T_2 - T_0) = T_2 + (T_1 - T_0) > \text{now}$$

となり、`ca->epoch_start` が現在時刻 `now` より未来になってしまいます。
もし未来の `ca->epoch_start` のまま `bictcp_update()` が呼ばれて `t = (__s32)(tcp_jiffies32 - ca->epoch_start) * USEC_PER_JIFFY;` が実行されると、`(__s32)(tcp_jiffies32 - ca->epoch_start)` が負の値になり、`__u64 t` への代入時に約 $2^{64}$ へラップアラウンドして 3 次関数が暴走します。これを防ぐために `after(ca->epoch_start, now)` であれば `ca->epoch_start = now` に丸めます。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

Step 2 までのコード（`bictcp_update()`、`bpf_cubic5_cong_avoid()`、`bpf_cubic5_recalc_ssthresh()`、`bpf_cubic5_state()`、`bpf_cubic5_acked()`、`bpf_cubic5_undo_cwnd()`）はすべてそのまま引き継ぎ、`bpf_cubic5_cwnd_event_tx_start()` を完成させて `struct tcp_congestion_ops bpf_cubic5` に登録します。

### 4.1 `bpf_cubic5_cwnd_event_tx_start()` の実装

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

1. **現在時刻とアイドル期間の計算**:
   - `now = tcp_jiffies32` で現在の 32 ビット jiffies を取得します。
   - `delta = now - tcp_sk(sk)->lsndtime` で、アイドルに入る前の最後の送信時刻からの経過 jiffies を符号付き 32 ビット整数 `__s32` として求めます。
2. **エポック開始時刻のシフト**:
   - `if (ca->epoch_start && delta > 0)` により、すでに Congestion Avoidance エポックが進行中（`ca->epoch_start != 0`）であり、かつ 1 jiffy 以上アイドルしていた場合のみ、`ca->epoch_start += delta;` でエポック開始時刻を後ろへシフトします。
3. **未来時刻のクランプ**:
   - `if (after(ca->epoch_start, now)) ca->epoch_start = now;` により、`ca->epoch_start` が現在時刻 `now` を追い越した場合に `now` で頭打ちにし、`bictcp_update()` における `tcp_jiffies32 - ca->epoch_start` の負数アンダーフローを確実に防ぎます。

### 4.2 `bictcp_update()` における経過時間 `t` の計算との連携

`bpf_cubic5_cwnd_event_tx_start()` が `ca->epoch_start` をシフトしておくことで、その後に ACK が到着して `bictcp_update()` が呼ばれた際、174〜177 行目の固定小数点計算は特別な分岐を一切追加することなくそのまま正しく動作します。

```c
	t = (__s32)(tcp_jiffies32 - ca->epoch_start) * USEC_PER_JIFFY;
	t += ca->delay_min;
	t <<= BICTCP_HZ;
	t /= USEC_PER_SEC;
```

- `tcp_jiffies32 - ca->epoch_start` からアイドル期間 `delta` が差し引かれた状態になるため、`t`（ $1/1024$ 秒単位）には純粋にデータを送信していたアクティブな期間と最小 RTT `ca->delay_min` だけが反映されます。

### 4.3 `struct tcp_congestion_ops bpf_cubic5` への登録

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

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### 5.1 `test.pkt` のシナリオ解説

`test.pkt` は、RTT = $20\text{ ms}$ の環境で以下の 5 つのフェーズを順番に実行し、2 秒間のアイドル後に `snd_cwnd` が暴走しないことを検証します。

1. **接続確立と sysctl 設定（8〜20 行目）**:
   - `net.ipv4.tcp_congestion_control=bpf_cubic5` を設定します。
   - また、カーネル標準の「アイドル後に Slow Start からやり直す機能（RFC 2861）」が介入しないよう、`net.ipv4.tcp_slow_start_after_idle=0` に設定した上で 3-way ハンドシェイクを行います（初期 `cwnd = 10`、`delay_min = 20 ms`）。
2. **Slow Start で `cwnd = 20` まで成長（22〜28 行目）**:
   - 10 セグメント（10,000 バイト）を送信して ACK を受け取り、`tcpi_snd_cwnd == 20` に成長させます。
3. **パケットロスと Fast Recovery から Congestion Avoidance への遷移（30〜46 行目）**:
   - 20 セグメント（20,000 バイト）を送信し、先頭セグメントのロスと 3 つの SACK によって Fast Recovery に入ります。
   - `bpf_cubic5_recalc_ssthresh()` が呼ばれ、`ca->last_max_cwnd = 20`、`snd_ssthresh = (20 * 717) / 1024 = 14`、`ca->epoch_start = 0` が設定されます。
   - 再送後の累積 ACK（`ack 30001`）で Recovery を完了すると、`tcp_complete_cwr()` が `snd_cwnd = ssthresh = 14` にして `TCP_CA_Open` に戻し、同じ ACK の処理内で `bpf_cubic5_cong_avoid()` が呼ばれます。ここで `bictcp_update()` が `ca->epoch_start = tcp_jiffies32` と `ca->bic_K = 2524`（約 $2.46$ 秒）を初期化し、`bic_target = 15`（`ca->cnt = 14`）により `tcpi_snd_cwnd` が `14` から `15` に増えます。
4. **アイドル前の Congestion Avoidance で 1 RTT 送信（48〜54 行目）**:
   - 15 セグメント（15,000 バイト）を送信して $20\text{ ms}$ 後に ACK を受け取り、`tcpi_snd_cwnd == 15` であることを確認します。この ACK でネットワーク上の未確認データはゼロ（`in_flight == 0`）になります。
5. **2.0 秒間のアイドル後に 15 セグメントを送信・ACK（56〜67 行目）**:
   - `+2.000` 秒間何も送らずに待機した後、`write(4, ..., 15000)` で 15 セグメントを送信し、 $20\text{ ms}$ 後に ACK を受け取って `assert tcpi_snd_cwnd <= 15` を検証します。
   - **`starter.bpf.c` が失敗する理由**: `bpf_cubic5_cwnd_event_tx_start()` が何もしないため、`ca->epoch_start` が 2 秒前のまま残ります。ACK 到着時に経過時間 `t` が一気に約 $2.06$ 秒まで進み、`bic_target` が `W_max = 20` に跳ね上がって `ca->cnt = 15 / (20 - 15) = 3` となるため、15 セグメントの ACK 一発で `tcpi_snd_cwnd` が **`20` 以上** に急騰し、`assert tcpi_snd_cwnd <= 15` で FAIL します。
   - **`solution.bpf.c` がパスする理由**: 2 秒後の最初のセグメント送信時に `bpf_cubic5_cwnd_event_tx_start()` が `ca->epoch_start` を 2 秒分シフトします。そのため ACK 到着時の `t` は約 $60\text{ ms}$ にとどまり、`bic_target = 15 <= cwnd (15)`（`ca->cnt = 100 * 15 = 1500`）となって `tcpi_snd_cwnd` は **`15` のまま**（`<= 15`）で PASS します。

### 5.2 テストの実行方法

```sh
../../run_test.sh cubic/step03-idle-epoch-shift starter
../../run_test.sh cubic/step03-idle-epoch-shift solution
```
