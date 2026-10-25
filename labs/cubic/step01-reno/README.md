# Step 1: 最小の BPF CC 骨格と Slow Start + Reno AI

## 学習ゴール

このステップでは、Linux カーネルの `struct tcp_congestion_ops` を BPF の `struct_ops` 機能で実装し、すべての輻輳制御アルゴリズムの基礎となる **TCP Reno（Slow Start と Additive Increase）** をゼロから組み立てます。

カーネル内の `tcp_slow_start()` や `tcp_cong_avoid_ai()` といった `extern __ksym` 関数には頼らず、純粋な BPF コードとして 1 行ずつ記述することで、`tp->snd_cwnd`・`tp->snd_ssthresh`・`tp->snd_cwnd_cnt` が ACK 到着ごとにどのように更新されるかを完全に可視化します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

`starter.bpf.c` や `solution.bpf.c` を開く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. 状態管理の設計：Reno には独自の private 構造体（`icsk_ca_priv`）が必要でしょうか？

CUBIC や BBR ではソケットごとの独自状態（`inet_csk_ca(sk)`）を使いますが、Reno の Slow Start と Additive Increase（1 RTT あたり `+1 cwnd`）を実装するために必要な変数は、`struct tcp_sock` の中にすでに用意されているでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `struct tcp_sock` には現在の輻輳ウィンドウ `snd_cwnd`、スロースタート閾値 `snd_ssthresh` に加えて、1 パケット未満の端数クレジットを蓄積するカウンタが存在します。
- **答え**: 独自の `icsk_ca_priv` 構造体は不要です。`struct tcp_sock` が持つ以下のフィールドだけで完結します。
  - `tp->snd_cwnd`: 現在の輻輳ウィンドウ（セグメント単位）
  - `tp->snd_ssthresh`: Slow Start から Congestion Avoidance へ移行する閾値
  - `tp->snd_cwnd_cnt`: Congestion Avoidance 中に ACK されたセグメント数を数える線形増加カウンタ
  - `tp->prior_cwnd`: ロス検知前の `snd_cwnd` の退避値（Undo 用）

</details>

### Q2. フックの選択とガード条件：アプリがデータを少ししか送っていないときでも `snd_cwnd` を増やしてよいでしょうか？

ACK が届くたびに呼ばれる `.cong_avoid(sk, ack, acked)` フックで、無条件に `tp->snd_cwnd` を増やすと何が起きるでしょうか？たとえば `snd_cwnd = 10` なのにアプリが毎 RTT 1 セグメントしか送信しない場合を考えてみてください。

<details>
<summary>ヒントと答え</summary>

- **ヒント**: ネットワークの空き容量を実際に試していないのに `snd_cwnd` だけが増え続けると、突然バースト送信が発生した際に大崩壊を起こします（RFC 7661 / Cwnd Validation）。
- **答え**: 送信が本当に `snd_cwnd` によって律速されている（パイプを `snd_cwnd` いっぱいまで使い切っている）ときだけウィンドウを増やす必要があります。そのため、`.cong_avoid()` の先頭で必ず `!tcp_is_cwnd_limited(sk)` をチェックし、律速されていなければ即座に `return` します。

</details>

### Q3. 境界条件の設計：Slow Start 中に `snd_ssthresh` をまたぐ Stretch ACK が届いたら？

現在 `tp->snd_cwnd = 20`、`tp->snd_ssthresh = 22` だとします。ここで 20 セグメント分を一括で ACK する Stretch ACK（`acked = 20`）が届いたとき、`snd_cwnd` と `snd_cwnd_cnt` はそれぞれいくつになるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `snd_ssthresh`（22）までは Slow Start で 1 ACK あたり +1 増えますが、`snd_ssthresh` に達した瞬間に残りの `acked` はどこへ行くべきでしょうか？
- **答え**:
  - まず Slow Start として `22 - 20 = 2` セグメント分の `acked` を使い、`tp->snd_cwnd` を `22`（`snd_ssthresh`）まで引き上げます。
  - まだ使われていない残りの `acked = 20 - 2 = 18` セグメント分は捨てずに、そのまま Congestion Avoidance（Additive Increase）の処理へフォールスルーさせ、`tp->snd_cwnd_cnt += 18` として蓄積します。
  - もし `snd_ssthresh` に達していない（`acked` をすべて Slow Start で使い切った）場合は、`if (!acked) return;` で早期リターンします。

</details>

### Q4. 浮動小数点なしの Additive Increase：BPF で `cwnd += acked / cwnd` をどう表現する？

BPF プログラム内では浮動小数点演算（`float` / `double`）が使えません。1 セグメント ACK されるごとに `snd_cwnd` を `1 / snd_cwnd` ずつ増やす（＝1 RTT で `snd_cwnd` 個の ACK が届いたら `+1` する）には、整数演算だけでどう実装すればよいでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: 分母が `tp->snd_cwnd` である分数 `tp->snd_cwnd_cnt / tp->snd_cwnd` の分子だけを整数変数 `tp->snd_cwnd_cnt` に貯めていきます。
- **答え**:
  - ACK のたびに `tp->snd_cwnd_cnt += acked` と分子を加算します。
  - `tp->snd_cwnd_cnt >= tp->snd_cwnd`（分子が分母以上に達した＝1 RTT 分の ACK が集まった）タイミングで、`tp->snd_cwnd_cnt -= tp->snd_cwnd` と引き算し、`tp->snd_cwnd` を `+1`（上限 `tp->snd_cwnd_clamp` でクランプ）します。

</details>

---

## 実装ステップと解説

[`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno/solution.bpf.c) の各関数を順に見ていきます。

### 1. `bpf_reno1_init()` — ソケットの初期化

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->snd_cwnd_cnt = 0;
	tp->snd_ssthresh = 22;
}
```

- TCP コネクション確立時（または `setsockopt(TCP_CONGESTION)` 時）に呼ばれます。
- 線形増加用の端数カウンタ `tp->snd_cwnd_cnt` を `0` にリセットします。
- 本ハンズオンの `test.pkt` では、パケットロスを発生させずに「純粋な Slow Start（`10 -> 20`）」「`snd_ssthresh` 境界でのクランプと余剰 `acked` の引き継ぎ（`20 -> 22`）」「Reno Additive Increase（`22 -> 23`）」の 3 段階を連続して観察できるよう、初期 `tp->snd_ssthresh` を `22` に設定しています。

### 2. `bpf_reno1_ssthresh()` — ロス検知時の閾値計算

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}
```

- パケットロスを検知して Fast Recovery や RTO に入る際、新しい `snd_ssthresh` を決定するために呼ばれます。
- Reno の Multiplicative Decrease（乗算的減少）に従い、現在の `tp->snd_cwnd` を半分（`>> 1U`）にします。ただし RFC 5681 の規定により最小値 `2U` を下回らないよう `max(..., 2U)` で保護します。

### 3. `bpf_reno1_cong_avoid()` — Slow Start と Reno Additive Increase

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		__u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

		acked -= cwnd - tp->snd_cwnd;
		tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
		if (!acked)
			return;
	}

	tp->snd_cwnd_cnt += acked;
	if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
		tp->snd_cwnd_cnt -= tp->snd_cwnd;
		tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
	}
}
```

- **Cwnd-limited チェック**: `tcp_is_cwnd_limited(sk)` が偽（アプリが `snd_cwnd` を使い切っていない）なら何もしません。
- **Slow Start フェーズ**: `tcp_in_slow_start(tp)`（すなわち `tp->snd_cwnd < tp->snd_ssthresh`）の間は、`tp->snd_cwnd + acked` を `tp->snd_ssthresh` でクランプした新しい `cwnd` を求めます。Slow Start で消費した分（`cwnd - tp->snd_cwnd`）を `acked` から差し引き、`acked` が `0` になればここで終了します。
- **Congestion Avoidance（Reno AI）フェーズ**: 残った `acked` を `tp->snd_cwnd_cnt` に加算し、`tp->snd_cwnd` 以上に達したら `tp->snd_cwnd` を引いてウィンドウサイズを `+1` します。
- **BPF Verifier のポイント**: ループを使わずに 1 回の条件分岐で更新しているため、BPF verifier の命令数・ループ解析に引っかかることなく安全に検証を通過します。

### 4. `bpf_reno1_undo_cwnd()` — 誤検知時のウィンドウ復元

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_undo_cwnd, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}
```

- DSACK や Eifel 検知によって「先ほどのロス検知はパケット並べ替え等による誤検知（Spurious Retransmission）だった」と判明した際に呼ばれ、縮小前の `tp->prior_cwnd` へ `snd_cwnd` を戻します（Step 2 で詳しくテストします）。

---

## packetdrill テストの解説

[`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno/test.pkt) では、以下のシナリオを実行して `bpf_reno1` の動作を検証しています。

1. **MSS = 1000 バイトのコネクション確立**:
   - クライアントからの SYN パケットで `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>` を指定します。
   - TCP Timestamps オプション（`nop,nop,TS` で 12 バイト）が有効なため、実効ペイロード長 `tp->mss_cache` は `1012 - 12 = 1000` バイトちょうどになり、`1000` バイト単位でセグメント数を直感的に計算できます。
   - 3-way ハンドシェイク直後、初期ウィンドウ `tcpi_snd_cwnd == 10` と `tcpi_snd_ssthresh == 22` をアサートします。
2. **フェーズ 1：純粋な Slow Start（`cwnd: 10 -> 20`）**:
   - `write(4, ..., 10000)` で 10 セグメント（10,000 バイト）を送信します。`snd_cwnd == 10` を使い切るため、`tp->is_cwnd_limited` が `true` になります。
   - `ack 10001`（`acked = 10`）を受信すると、Slow Start により `tcpi_snd_cwnd` が `10 + 10 = 20` に倍増することを確認します。
3. **フェーズ 2：`snd_ssthresh` 境界クランプと余剰クレジットの引き継ぎ（`cwnd: 20 -> 22`）**:
   - `write(4, ..., 20000)` で 20 セグメント（20,000 バイト）を送信して再び `snd_cwnd == 20` を使い切ります。
   - `ack 30001`（`acked = 20`）を受信すると、`snd_ssthresh == 22` でクランプされるため Slow Start で `2` セグメント分だけ消費されて `tcpi_snd_cwnd == 22` になり、残りの `18` セグメント分が `tp->snd_cwnd_cnt = 18` に蓄えられます。
4. **フェーズ 3：Reno Additive Increase（`cwnd: 22 -> 23`）**:
   - `write(4, ..., 22000)` で 22 セグメント（22,000 バイト）を送信し、`ack 52001`（`acked = 22`）を受信します。
   - Congestion Avoidance により `tp->snd_cwnd_cnt` が `18 + 22 = 40 >= 22` となり、`tcpi_snd_cwnd` が `+1` されて `23` になることを確認します。

---

## 実行方法

`/usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno` ディレクトリ（または `labs/` ルート）から以下のコマンドを実行します。

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh cubic/step01-reno starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh cubic/step01-reno solution
```
