# Step 4: Reno-friendly 領域 ($W_{\text{est}}$) と Fast Convergence

## 学習ゴール

Step 3 で実装した純粋な CUBIC の三次関数曲線（$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$）には、実ネットワークでそのまま使うと 2 つの課題があります。

- **課題 1（新しいフローが参入したときの収束の遅さ）**: 既存の CUBIC フローが大きな $W_{\max}$ を維持しているところに新しいフローが参入すると、ボトルネックの公平なシェアが下がります。しかし三次関数は前回の $W_{\max}$ 付近でプラトー（平坦）になるため、既存のフローがなかなか帯域を譲らず、収束に時間がかかります。
- **課題 2（短 RTT・小 BDP 環境での TCP Reno に対する劣後）**: CUBIC の三次関数は実時間 $t$ に依存するため、RTT が短い環境や $W_{\max}$ が小さい環境では、1 RTT ごとに `cwnd` を 1 ずつ増やす従来の TCP Reno（AIMD）よりも増加ペースが遅くなる領域が存在します。

このステップでは、上記 2 つの課題を解決する **Fast Convergence（高速収束）** と **Reno-friendly 領域（$W_{\text{est}}$ / `tcp_cwnd`）** を実装し、`bpf_cubic4` として完成させます。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

`starter.bpf.c` や `solution.bpf.c` を開く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. 新しいフローが参入したことを、送信側はどのフック・どの条件で検知できるでしょうか？

パケットロスが発生したとき、現在の `cwnd` と前回のロス時の最大ウィンドウ `ca->last_max_cwnd`（$W_{\max}$）を比較すると、ネットワークの状況について何が言えるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **着目するフック**: パケットロス検知時に呼ばれる `.ssthresh`（`bpf_cubic4_ssthresh()`）です。
- **判定条件**: 前回のロス後に $W_{\max}$ まで回復しきれないうちに再びロスが起きた場合、すなわち **`cwnd < ca->last_max_cwnd`** のとき、「新しい競合フローが参入して自分が使える帯域の上限が下がった」と推測できます。
- **Fast Convergence のアクション**: 通常なら `ca->last_max_cwnd = cwnd` とするところを、あえてさらに低い値に引き下げることで、次のエポックのプラトー（目標点 $W_{\max}$）を低くし、素早く帯域を譲ります。

</details>

### Q2. Fast Convergence では `last_max_cwnd` をどの値に引き下げるべきでしょうか？

ロス発生時のウィンドウを $W$、乗法的減少係数を $\beta \approx 0.7$（固定小数点では `beta = 717`、`BICTCP_BETA_SCALE = 1024`）とします。減速前の $W$ と減速後の $\beta W$ の「中間値」に $W_{\max}$ を設定するには、整数演算でどのような式になりますか？

<details>
<summary>ヒントと答え</summary>

- 減速前のウィンドウ $W$（$= 1.0 W$）と減速後のウィンドウ $\beta W$ の平均（中点）は以下の通りです。
  $$W_{\max} = W \times \frac{1 + \beta}{2} \approx 0.85 W$$
- `BICTCP_BETA_SCALE = 1024`、`beta = 717` を用いた整数演算では、以下のように計算します。
  ```c
  if (cwnd < ca->last_max_cwnd)
          ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
                              / (2 * BICTCP_BETA_SCALE);
  else
          ca->last_max_cwnd = cwnd;
  ```
- たとえば 1 回目のロスが `cwnd = 20` で起きて `last_max_cwnd = 20`、`ssthresh = 14` となり、20 に戻る前の `cwnd = 14` で 2 回目のロスが起きた場合、`14 < 20` なので Fast Convergence が発動し、`last_max_cwnd = (14 * 1741) / 2048 = 11` に引き下げられます。

</details>

### Q3. なぜ CUBIC が模倣する「Reno 相当ウィンドウ `ca->tcp_cwnd`」は、1 RTT あたり +1 ではなく約 1.875 RTT あたり +1 のペースで増やすのでしょうか？

標準の TCP Reno はロス時に `cwnd` を $0.5$ 倍に減らし、1 RTT ごとに $+1$ 増やします。一方、CUBIC はロス時に `cwnd` を $\beta \approx 0.7$ 倍にしか減らしません。もし CUBIC がロス後に $0.7 W_{\max}$ からスタートして 1 RTT ごとに $+1$ 増やしたら、TCP Reno と比べて平均スループットはどうなってしまうでしょうか？

<details>
<summary>ヒントと答え</summary>

- もし $\beta = 0.7$ のまま 1 RTT ごとに $+1$（傾き $\alpha = 1$）で増やすと、TCP Reno（$\beta_{\text{reno}} = 0.5, \alpha_{\text{reno}} = 1$）よりも常にウィンドウが大きくなり、Reno フローから帯域を奪ってしまいます（Reno に対して不公平になります）。
- AIMD($\alpha, \beta$) が TCP Reno と同じ平均ウィンドウを持つための条件は、以下の式で与えられます。
  $$\alpha = \frac{3(1 - \beta)}{1 + \beta}$$
- $\beta = 717 / 1024 \approx 0.7$ を代入すると $\alpha \approx 0.529$（1 RTT あたり約 $0.53$ パケット増加）、つまり **1 パケット増やすのに必要な RTT 数（$\frac{1}{\alpha}$）** は以下のようになります。
  $$\frac{1}{\alpha} = \frac{1 + \beta}{3(1 - \beta)} \approx 1.889\text{ RTT}$$
- カーネル実装では、これを 8 倍した固定小数点定数 `beta_scale` として事前計算しています。
  ```c
  beta_scale = 8 * (BICTCP_BETA_SCALE + beta) / 3 / (BICTCP_BETA_SCALE - beta); /* = 15 */
  ```
  そして `delta = (cwnd * beta_scale) >> 3`（約 $1.875 \times \text{cwnd}$ 個の ACK）ごとに `ca->tcp_cwnd` を 1 増やします。

</details>

### Q4. カーネルの `tcp_cubic.c` にある `while (ca->ack_cnt > delta)` ループを BPF でそのまま書くと何が起きるでしょうか？

Linux カーネル本体の `tcp_cubic.c` では、`ca->tcp_cwnd` の更新に `while (ca->ack_cnt > delta) { ca->ack_cnt -= delta; ca->tcp_cwnd++; }` というループを使っています。BPF プログラムではこれをどう書き換えるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **BPF 検証器（Verifier）の制約**: 実行時の変数 `ca->ack_cnt` と `delta` に依存する上限不明の `while` ループは、BPF 検証器が停止性を証明できずリジェクトする原因になります。
- **割り算と剰余への置き換え**: `delta > 0` を確認した上で、整数の除算と剰余を使って $O(1)$ で計算します。
  ```c
  delta = (cwnd * beta_scale) >> 3;
  if (delta) {
          ca->tcp_cwnd += ca->ack_cnt / delta;
          ca->ack_cnt %= delta;
  }
  ```
- そして `ca->tcp_cwnd > cwnd`（三次関数よりも Reno 推定ウィンドウの方が先行している）なら、`max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` を計算し、`ca->cnt > max_cnt` のとき `ca->cnt = max_cnt` にクランプして増加ペースを引き上げます。

</details>

---

## 実装ステップと解説

`solution.bpf.c` では、Step 3 の実装に以下の 2 箇所を追加しています。

### 1. `bpf_cubic4_ssthresh()` における Fast Convergence

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

- ロス発生時に `ca->epoch_start = 0` として現在の CUBIC エポックを終了します。
- `cwnd < ca->last_max_cwnd` が成り立つ場合、前回の最大ウィンドウ $W_{\max}$ に到達する前に再びロスが起きたことを意味します。このとき `ca->last_max_cwnd` を `cwnd * (1024 + 717) / 2048`（約 $0.85 \times \text{cwnd}$）に引き下げます。
- これにより、回復後の新しいエポックで設定される `ca->bic_origin_point`（$= W_{\max}$）と `ca->bic_K` が小さくなり、低いウィンドウ領域で早めにプラトーへ入って新規参入フローに帯域を譲ることができます。

### 2. `bictcp_update()` における Reno-friendly 領域（`tcp_friendliness`）

```c
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
```

- `bictcp_update()` の冒頭で `ca->ack_cnt += acked` により ACK されたセグメント数を累積します（新エポック開始時は `ca->ack_cnt = acked`、`ca->tcp_cwnd = cwnd` で初期化されます）。
- 同一 jiffy 内に複数の ACK が到着した場合は、三次関数の再計算をスキップして `goto tcp_friendliness;` にジャンプし、ACK カウンタによる `ca->tcp_cwnd` の更新のみを行います。
- `delta = (cwnd * beta_scale) >> 3` ごとに `ca->tcp_cwnd` を進めます。
- もし `ca->tcp_cwnd > cwnd` であれば、CUBIC の三次関数が算出する `ca->cnt`（特にプラトー付近では `100 * cwnd` という非常に大きな値になります）を `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` で上から抑え（`ca->cnt = min(ca->cnt, max_cnt)`）、Reno 相当の成長速度を保証します。

---

## packetdrill テストの解説 (`test.pkt`)

`test.pkt` は、**Fast Convergence** と **Reno-friendly 領域** の両方が正しく機能しているかを段階的に検証します。

1. **Slow Start（`cwnd: 10 -> 20`）**:
   - 初期ウィンドウ 10 セグメント（10,000 バイト）を送信し、ACK を受けて `cwnd == 20` に到達します。
2. **1 回目のロス（`cwnd = 20`）**:
   - 20 セグメント送信中に 3 個の SACK を受け取って Recovery に入ります。
   - この時点では初回のロス（`ca->last_max_cwnd == 0`）なので、`ca->last_max_cwnd = 20`、`ssthresh = (20 * 717) / 1024 = 14` となり、Recovery 完了後に `cwnd == 14` になります。
3. **2 回目のロス（`cwnd = 14 < last_max_cwnd = 20`）— Fast Convergence の発動**:
   - `cwnd` が 20 に戻る前の `cwnd = 14` で再び 3 個の SACK を受け取ります。
   - `ssthresh` は `(14 * 717) / 1024 = 9` になります。
   - **Fast Convergence あり（`solution`）**: `14 < 20` なので `ca->last_max_cwnd = (14 * 1741) / 2048 = 11` に引き下げられます。
   - **Fast Convergence なし（`starter`）**: `ca->last_max_cwnd = 14` になります。
4. **2 回目の回復後の Congestion Avoidance（`cwnd = 9`）— Fast Convergence の検証**:
   - **Flight 1（エポック開始、`t_elapsed = 0.0s`）**: `cwnd = 9` で 9 セグメント送信・ACK され、`bic_target = 10`（`cnt = 9`）により `cwnd` が `9 -> 10` に増加します。
   - **Flight 2（`t_elapsed = 0.8s`）**: 10 セグメント送信・ACK されます。
     - `solution`（`last_max_cwnd = 11`）では、目標値 `bic_target` がすでにプラトーの `11` に達しているため、`cnt = 10 / (11 - 10) = 10` となり、10 セグメントの ACK で `cwnd` はちょうど `11` になります。
     - 一方 `starter`（`last_max_cwnd = 14`）では、0.8 秒時点の `bic_target` が `13` と高いため、`cnt = 10 / (13 - 10) = 3` となってしまい、10 セグメントの ACK で `cwnd` が `13`（`!= 11`）へ飛び出してアサートに失敗します。
5. **プラトー（`cwnd = 11 == last_max_cwnd`）での Reno-friendly 領域の検証**:
   - `cwnd = 11` に達すると、`t < bic_K`（約 1.71 秒）の間は `bic_target == 11 == cwnd` となるため、純粋な三次関数だけでは `ca->cnt = 100 * 11 = 1100` となり、`cwnd` の成長がほぼ止まります。
   - しかし Reno-friendly 領域を実装していると、`delta = (11 * 15) >> 3 = 20` ACK ごとに `ca->tcp_cwnd` が `9 -> 10 -> 11 -> 12` と増加します。`ca->tcp_cwnd == 12 > cwnd (11)` になった瞬間に `max_cnt = 11 / (12 - 11) = 11` で `ca->cnt` がクランプされ、`cwnd > 11` へと成長します。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh cubic/step04-reno-friendly-fast-conv starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh cubic/step04-reno-friendly-fast-conv solution
```
