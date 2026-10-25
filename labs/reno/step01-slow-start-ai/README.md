# Step 1: 最小の BPF CC 骨格と Slow Start + Reno AI

## 1. 学習ゴール

このステップでは、Linux カーネルのプラガブル輻輳制御インターフェースである `struct tcp_congestion_ops` を BPF の `struct_ops` 機能で実装し、すべての TCP 輻輳制御アルゴリズムの原点である **TCP Reno（Slow Start・Additive Increase・Multiplicative Decrease）** をゼロから組み立てます。カーネル内の `tcp_slow_start()` や `tcp_cong_avoid_ai()` といった既存ヘルパー関数には頼らず、純粋な BPF コードとして 1 行ずつ記述することで、ACK が到着するたびに輻輳ウィンドウがどのように成長・遷移するかを完全に理解します。

- トラック全体の構成とステップ一覧は [Reno トラック README](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

設計チェックポイントに進む前に、TCP Reno のアルゴリズムと、Linux カーネルが提供するデータ構造・フックの仕組みを基礎から押さえましょう。

### 2.1 なぜ輻輳制御（Congestion Control）が必要なのか？

送信側ホストがネットワークの空き容量を無視して大量のパケットを一気に送り込むと、途中のルータのバッファがあふれてパケットロスが連鎖し、ネットワーク全体のスループットが崩壊します（輻輳崩壊 / Congestion Collapse）。しかし、インターネット上の経路が今どれだけ空いているかは、送信側には事前には分かりません。

そこで TCP は、**「同時にネットワーク上に送り出してよい未確認（in-flight）データの量」** を制限する送信側の自主規制ウィンドウ **輻輳ウィンドウ（`cwnd`: Congestion Window、単位は MSS セグメント数）** を持ち、相手からの ACK（確認応答）を手がかりに `cwnd` を動的に増減させます。

### 2.2 TCP Reno の 2 つの成長フェーズ：Slow Start と Congestion Avoidance

TCP Reno（RFC 5681）は、スロースタート閾値 **`ssthresh`（Slow Start Threshold）** を境界として、`cwnd` の増やし方を 2 段階に切り替えます。

```
cwnd (segments)
  ^
  |                        /  <- Congestion Avoidance (Additive Increase)
23|....................../..     1 RTT あたり +1 cwnd（線形増加）
22|-----------------> *---       ssthresh = 22 で成長モードを切り替え
  |                 /
20|...............*
  |              /            <- Slow Start
  |             /                1 ACK セグメントあたり +1 cwnd
  |            /                 （1 RTT ごとに 2 倍の指数関数的増加）
10|----------*
  +----------------------------> 時間 (RTT)
   RTT 0   RTT 1   RTT 2   RTT 3
```

1. **Slow Start（スロースタート：`cwnd < ssthresh` のとき）**
   - **直感**: 通信開始直後は経路の容量がまったく分からないため、小さな初期ウィンドウ（Linux のデフォルトは `10` セグメント）からスタートし、ACK が返ってくるたびに素早く（指数関数的に）探索幅を広げます。
   - **数式とルール**: 新しいデータを確認する ACK が届き、`acked` セグメント分の到達が確認されるたびに、その分だけ `cwnd` をそのまま増やします。
     $$\text{cwnd} \leftarrow \text{cwnd} + \text{acked}$$
   - `cwnd` 個のセグメントを送ってそれらすべてに ACK が返ってくると（＝1 RTT 経過すると）、`cwnd` は $\text{cwnd} + \text{cwnd} = 2 \times \text{cwnd}$ となり、**1 RTT ごとに 2 倍** に成長します。
   - **境界クランプ**: Slow Start で増やせる上限は `ssthresh` までです。もし 1 つの ACK（複数セグメントをまとめて確認する Stretch ACK など）によって `cwnd + acked` が `ssthresh` を超える場合は、まず `cwnd` を `ssthresh` まで引き上げ、**使い切らなかった残りの `acked` セグメント分はそのまま次の Congestion Avoidance フェーズへ引き継ぎます**。

2. **Congestion Avoidance（輻輳回避 / Reno Additive Increase：`cwnd >= ssthresh` のとき）**
   - **直感**: `ssthresh`（過去にロスが起きた付近、または安全な目標値）に達した後は、2 倍ずつ増やすとすぐにバッファがあふれてしまいます。そこで「1 RTT かけて現在の `cwnd` 個のセグメントすべてが無事に ACK されたら、ようやく `cwnd` を `+1` セグメントだけ増やす」という慎重な線形増加（**Additive Increase: 加算的増加**）に切り替えます。
   - **数式と整数演算による表現**: 理想的には、1 セグメントが ACK されるごとに `cwnd` を $\frac{1}{\text{cwnd}}$ ずつ増やしたい（`acked` セグメントなら $\text{cwnd} \leftarrow \text{cwnd} + \frac{\text{acked}}{\text{cwnd}}$）のですが、Linux カーネルや BPF プログラム内では浮動小数点演算（`float` / `double`）は使えません。
   - そこで、分数 $\frac{\text{cnt}}{\text{cwnd}}$ の分子にあたる **整数カウンタ `snd_cwnd_cnt`** を用意し、以下の整数ルールで計算します。
     1. ACK が届くたびに分子を貯める：`snd_cwnd_cnt += acked`
     2. 分子が分母（`snd_cwnd`）以上に達したら（`snd_cwnd_cnt >= snd_cwnd`）、`snd_cwnd_cnt -= snd_cwnd` として分母を引き、`snd_cwnd` を `+1` する。

### 2.3 具体数値トレース（`cwnd = 10`, `ssthresh = 22` の場合）

このラボの `test.pkt` で実際に動く数値をそのまま追ってみましょう。

| ラウンド | 到着前の状態 | イベント（受信した ACK） | 内部計算のステップ | 更新後の状態 |
| :--- | :--- | :--- | :--- | :--- |
| **初期状態** | — | 3-way ハンドシェイク完了 | `bpf_reno1_init()` で `snd_cwnd_cnt = 0`, `snd_ssthresh = 22` に初期化 | `cwnd = 10`<br>`ssthresh = 22`<br>`cnt = 0` |
| **Round 1**<br>(純粋な Slow Start) | `cwnd = 10`<br>`ssthresh = 22`<br>`cnt = 0` | 10 セグメント送信し、`acked = 10` の ACK を受信 | `cwnd < ssthresh` なので Slow Start。<br>`10 + 10 = 20 <= 22` のため、10 セグメントすべてを Slow Start で消費（残り `acked = 0`）。 | `cwnd = 20`<br>`ssthresh = 22`<br>`cnt = 0` |
| **Round 2**<br>(境界またぎ) | `cwnd = 20`<br>`ssthresh = 22`<br>`cnt = 0` | 20 セグメント送信し、`acked = 20` の ACK を受信 | 1. Slow Start で `ssthresh` (22) まで `22 - 20 = 2` セグメント分だけ成長（`cwnd = 22`）。<br>2. 残りの `acked = 20 - 2 = 18` セグメントを Congestion Avoidance へ引き継ぎ、`cnt = 0 + 18 = 18`。<br>3. `cnt (18) < cwnd (22)` なので `+1` はまだ起きない。 | `cwnd = 22`<br>`ssthresh = 22`<br>`cnt = 18` |
| **Round 3**<br>(Reno AI) | `cwnd = 22`<br>`ssthresh = 22`<br>`cnt = 18` | 22 セグメント送信し、`acked = 22` の ACK を受信 | `cwnd >= ssthresh` なので最初から Congestion Avoidance。<br>1. `cnt = 18 + 22 = 40`。<br>2. `cnt (40) >= cwnd (22)` なので `cnt = 40 - 22 = 18` とし、`cwnd` を `22 + 1 = 23` に増やす。 | `cwnd = 23`<br>`ssthresh = 22`<br>`cnt = 18` |

### 2.4 Cwnd Validation（パイプを使い切っていないときのガード条件）

もし `cwnd = 10` あるのに、アプリケーションが毎 RTT 1 セグメントずつしかデータを書き込まない（Application-limited な）状態だったらどうなるでしょうか？
ネットワークの空き容量を実際に 10 セグメント分試していないにもかかわらず、ACK が返るたびに `cwnd` を 11, 12, ... と増やし続けてしまうと、後でアプリが突然大量のデータを書き込んだ瞬間に巨大なバースト送信が発生し、激しいパケットロスを引き起こします（RFC 7661）。

そのため、ACK 受信時に `cwnd` を増やす処理（`.cong_avoid()`）では、**「現在の送信が本当に `cwnd` によって律速されている（パイプを `cwnd` いっぱいまで使い切っている）か？」** を最初に確認し、律速されていなければ何もせずにリターンしなければなりません。

### 2.5 ロス検知時の Multiplicative Decrease と誤検知の Undo

TCP Reno では、パケットロスを検知したとき、およびそのロス検知が誤り（パケットの順序逆転などによる Spurious Loss）だったと判明したときに以下のルールを適用します。

- **Multiplicative Decrease（乗算的減少：`.ssthresh()`）**: パケットロスが起きたということは、現在の `cwnd` は経路容量を超えていたことを意味します。Reno は新しい `ssthresh` を現在の `cwnd` の半分（$\lfloor \text{cwnd} / 2 \rfloor$、ビットシフトで `snd_cwnd >> 1U`）に引き下げます。ただし RFC 5681 の規定により、ウィンドウが完全に詰まらないよう最小値 `2U` を下回らないようにします：
  $$\text{ssthresh} = \max(\lfloor \text{cwnd} / 2 \rfloor, 2)$$
- **ウィンドウ縮小の取り消し（`.undo_cwnd()`）**: タイムアウトや重複 ACK で一度 `cwnd` を減らした後に、DSACK 等によって「実はパケットはロスしておらず遅れて届いただけだった」と判明した場合、カーネルはロス突入前に保存しておいた `prior_cwnd` と現在の `snd_cwnd` の大きい方（$\max(\text{snd}_{\text{cwnd}}, \text{prior}_{\text{cwnd}})$）へウィンドウを復元します。

### 2.6 カーネルがすでに提供している構造体・フィールド・ヘルパー一覧

BPF で輻輳制御を書くとき、「どの変数を自分で定義すべきで、どの変数が最初からカーネルのソケット構造体に入っているか」を知っておく必要があります。

#### (A) `struct tcp_congestion_ops` の主要フック（BPF `struct_ops` で実装する関数）

| フック名 | シグネチャ | 呼ばれるタイミングと役割 |
| :--- | :--- | :--- |
| `.init` | `void (*init)(struct sock *sk)` | コネクション確立時（または `TCP_CONGESTION` 切り替え時）に状態変数を初期化する。 |
| `.ssthresh` | `__u32 (*ssthresh)(struct sock *sk)` | ロス検知時に呼ばれ、新しい `snd_ssthresh` の値を計算して返す。 |
| `.cong_avoid` | `void (*cong_avoid)(struct sock *sk, __u32 ack, __u32 acked)` | 新規データを確認する ACK 到着時に呼ばれる。`ack` は ACK 番号、`acked` は今回新たに ACK されたセグメント数。 |
| `.undo_cwnd` | `__u32 (*undo_cwnd)(struct sock *sk)` | ロス検知が誤検知（Spurious）と判明した際に呼ばれ、復元すべき `snd_cwnd` の値を返す。 |

#### (B) `struct tcp_sock`（`struct tcp_sock *tp = tcp_sk(sk)` で取得）が標準で持つフィールド

Linux のすべての TCP ソケット（`struct tcp_sock`）には、**Reno 用の状態変数が最初から標準フィールドとして組み込まれています**：

- `tp->snd_cwnd` (`__u32`): 現在の輻輳ウィンドウ（セグメント単位）。
- `tp->snd_ssthresh` (`__u32`): 現在のスロースタート閾値（セグメント単位）。
- `tp->snd_cwnd_cnt` (`__u32`): Congestion Avoidance（Additive Increase）で ACK されたセグメント数を蓄積する線形カウンタ（分数 $\frac{\text{cnt}}{\text{cwnd}}$ の分子）。
- `tp->snd_cwnd_clamp` (`__u32`): `tp->snd_cwnd` の許容上限値（`snd_cwnd` を増やす際は常に `min(new_cwnd, tp->snd_cwnd_clamp)` でクランプする）。
- `tp->prior_cwnd` (`__u32`): ロス回復に入る直前にカーネルが自動で退避してくれた `snd_cwnd` の値。

#### (C) アルゴリズム固有の Private 領域 `icsk_ca_priv` とは？

- `struct inet_connection_sock`（`tcp_sock` の親構造体）には、`__u64 icsk_ca_priv[...]` という 104 バイトの空きメモリ領域が用意されており、`inet_csk_ca(sk)` ヘルパーで任意のカスタム構造体ポインタとしてアクセスできます。
- CUBIC（3 次関数の起点時刻や `W_max` などを保持）や BBR（ボトルネック帯域フィルタなどを保持）のように、`struct tcp_sock` の標準フィールドだけでは足りない複雑なアルゴリズムは、この `icsk_ca_priv` に独自の構造体を置いて状態を管理します。
- 一方、**TCP Reno が必要とする変数（`snd_cwnd`, `snd_ssthresh`, `snd_cwnd_cnt`, `prior_cwnd`）はすべて上記のとおり `struct tcp_sock` 自体に最初から存在します**。

#### (D) `bpf_cc_helpers.h` で提供されているヘルパー関数

- `tcp_sk(sk)`: `struct sock *sk` を `struct tcp_sock *tp` に変換します。
- `tcp_is_cwnd_limited(sk)`: 現在の送信が `snd_cwnd` によって律速されている（パイプを使い切っている）なら `true`、アプリケーション律速なら `false` を返します。
- `tcp_in_slow_start(tp)`: `tp->snd_cwnd < tp->snd_ssthresh` のとき `true` を返します。
- `min(a, b)` / `max(a, b)`: 2 つの値の最小値・最大値を返します。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の解説を踏まえて、`starter.bpf.c` の TODO を埋めるための 4 つの設計の問いを自分の頭で考えてみましょう。すべての答えはセクション 2 の内容から導き出せます。

### Q1. 状態管理の設計：Reno には独自の private 構造体（`icsk_ca_priv`）を定義する必要があるでしょうか？

セクション 2.6 (B)・(C) で見た `struct tcp_sock` の標準フィールドと `icsk_ca_priv` の役割を思い出してください。Reno の Slow Start、Additive Increase、Multiplicative Decrease、Undo を実装するために、新しいカスタム構造体を `inet_csk_ca(sk)` に定義する必要はあるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.2〜2.5 の数式に登場した変数（現在のウィンドウ、閾値、AI の分子カウンタ、ロス前のウィンドウ）が `struct tcp_sock` にすでに揃っているか確認しましょう。
- **答え**: **独自の `icsk_ca_priv` 構造体は一切不要です。**
  `struct tcp_sock` が標準で持っている以下の 5 つのフィールドだけで TCP Reno は完全に実装できます。
  - `tp->snd_cwnd`: 現在の輻輳ウィンドウ
  - `tp->snd_ssthresh`: Slow Start から Congestion Avoidance へ切り替わる閾値
  - `tp->snd_cwnd_cnt`: Additive Increase で `acked` を貯める分子カウンタ
  - `tp->snd_cwnd_clamp`: `snd_cwnd` の上限クランプ値
  - `tp->prior_cwnd`: Undo 時に参照するロス前の `snd_cwnd`

</details>

### Q2. ガード条件の設計：`bpf_reno1_cong_avoid()` の先頭で最初にチェックすべき条件は何ですか？

セクション 2.4（Cwnd Validation）とセクション 2.6 (D) のヘルパー一覧を踏まえると、`bpf_reno1_cong_avoid(sk, ack, acked)` に入ってすぐ、Slow Start や Additive Increase の計算を行う前にどの関数を呼んで早期リターンすべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: アプリケーションがデータを少ししか送っておらず、`snd_cwnd` を使い切っていないときにウィンドウが膨張するのを防ぐヘルパー関数はどれでしょうか？
- **答え**: 関数の先頭で **`if (!tcp_is_cwnd_limited(sk)) return;`** を実行します。これにより、送信パイプが実際に `snd_cwnd` で律速されているときだけウィンドウを成長させます（`starter.bpf.c` の **TODO 2**）。

</details>

### Q3. 境界条件の設計：Slow Start 中に `tp->snd_ssthresh` をまたぐ ACK が届いたとき、C コードはどう書くべきですか？

セクション 2.3 の Round 2（`tp->snd_cwnd = 20`, `tp->snd_ssthresh = 22` のときに `acked = 20` が到着するケース）を C コードに落とし込みます。
`if (tcp_in_slow_start(tp))` ブロックの中で、
1. `tp->snd_ssthresh` を超えない新しいウィンドウ `cwnd` をどう計算しますか？
2. Slow Start で使った分を `acked` からどう差し引きますか？
3. どんなときにその場で `return` し、どんなときに後ろの Congestion Avoidance へフォールスルーさせますか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `tp->snd_cwnd + acked` と `tp->snd_ssthresh` の小さい方を `cwnd` とすれば、Slow Start で実際に増えた量は `cwnd - tp->snd_cwnd` になります。
- **答え**: 以下のように 4 行で綺麗に表現できます（`starter.bpf.c` の **TODO 3**）。
  ```c
  if (tcp_in_slow_start(tp)) {
      __u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

      acked -= cwnd - tp->snd_cwnd;
      tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
      if (!acked)
          return;
  }
  ```
  - Round 1（`cwnd = 10, ssthresh = 22, acked = 10`）では、`cwnd = min(20, 22) = 20` となり、`acked -= 20 - 10` で `acked` が `0` になるため `if (!acked) return;` で終了します。
  - Round 2（`cwnd = 20, ssthresh = 22, acked = 20`）では、`cwnd = min(40, 22) = 22` となり、`acked -= 22 - 20` で `acked` が `18` 残るため、`return` せずに直後の Congestion Avoidance 処理へそのまま進みます。

</details>

### Q4. 整数演算による Additive Increase：残った `acked` を使って `tp->snd_cwnd_cnt` と `tp->snd_cwnd` をどう更新しますか？

セクション 2.2 の「Congestion Avoidance の整数ルール」をそのまま C コードにしてください（`starter.bpf.c` の **TODO 4**）。

<details>
<summary>ヒントと答え</summary>

- **ヒント**: `tp->snd_cwnd_cnt` に `acked` を足し、それが `tp->snd_cwnd` 以上になったら `tp->snd_cwnd` を引いてウィンドウを `+1`（上限 `tp->snd_cwnd_clamp` でクランプ）します。
- **答え**:
  ```c
  tp->snd_cwnd_cnt += acked;
  if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
      tp->snd_cwnd_cnt -= tp->snd_cwnd;
      tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
  }
  ```
  ループ（`while`）を使わずに `if` 文 1 回で処理しているため、BPF verifier のループ境界検査に引っかかることもありません。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/reno/step01-slow-start-ai/solution.bpf.c) の各関数と計算式を順番に確認します。

### 4.1 `bpf_reno1_init()` — ソケットの初期化

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->snd_cwnd_cnt = 0;
	tp->snd_ssthresh = 22;
}
```

- TCP コネクション確立時に呼ばれ、Additive Increase 用の端数カウンタ `tp->snd_cwnd_cnt` を `0` に初期化します。
- 通常のカーネル既定値では初期 `snd_ssthresh` は無限大（`TCP_INFINITE_SSTHRESH`）ですが、このステップのテスト（`test.pkt`）ではパケットロスを起こさずに「純粋な Slow Start（`10 -> 20`）」「`snd_ssthresh` 境界でのクランプと余剰 `acked` の引き継ぎ（`20 -> 22`）」「Reno Additive Increase（`22 -> 23`）」の 3 段階を連続して検証できるよう、初期 `tp->snd_ssthresh` を `22` セグメントに設定しています。

### 4.2 `bpf_reno1_ssthresh()` — ロス検知時の Multiplicative Decrease（TODO 1）

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}
```

- パケットロス検知時に新しい `snd_ssthresh` を返します。
- 整数右シフト `tp->snd_cwnd >> 1U` で $\lfloor \text{snd}_{\text{cwnd}} / 2 \rfloor$ を計算し、RFC 5681 の下限値 `2U` と `max()` をとります。

### 4.3 `bpf_reno1_cong_avoid()` — Slow Start と Reno Additive Increase（TODO 2〜4）

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

- **TODO 2（Cwnd Validation）**: `!tcp_is_cwnd_limited(sk)` でパイプを使い切っていない場合は即座にリターンします。
- **TODO 3（Slow Start と境界の引き継ぎ）**: `tcp_in_slow_start(tp)` が真の間、`tp->snd_ssthresh` まで `tp->snd_cwnd` を増やし、Slow Start で消費したセグメント数（`cwnd - tp->snd_cwnd`）を `acked` から減算します。`acked` が残っていればそのまま下の Congestion Avoidance にフォールスルーします。
- **TODO 4（Reno Additive Increase）**: `tp->snd_cwnd_cnt` に `acked` を加算し、`tp->snd_cwnd` 以上に達したら `tp->snd_cwnd` を引いてウィンドウを `+1` します。

### 4.4 `bpf_reno1_undo_cwnd()` — 誤検知時のウィンドウ復元（TODO 5）

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_undo_cwnd, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}
```

- ロス検知が誤り（Spurious）だったと分かったときに呼ばれ、`tp->snd_cwnd` とロス突入前の `tp->prior_cwnd` の大きい方を返してウィンドウを元に戻します（次の Step 2 で実際にパケットトレースを流してテストします）。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### 5.1 [`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/reno/step01-slow-start-ai/test.pkt) のシナリオ解説

`test.pkt` はセクション 2.3 の数値トレースと完全に一致する 3 ラウンドのパケット交換を行い、各段階の `tcpi_snd_cwnd` を Python アサーションで検証します。

1. **MSS = 1000 バイトのコネクション確立（8〜22 行目）**:
   - クライアントからの SYN パケットで `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>` を指定しています。
   - TCP Timestamps オプション（`nop,nop,TS` で 12 バイト）が有効なため、1 セグメントあたりの実効データ長 `tp->mss_cache` は `1012 - 12 = 1000` バイトちょうどになります（10,000 バイトの書き込み＝10 セグメント）。
   - `accept()` 直後に、初期状態 `tcpi_snd_cwnd == 10` および `tcpi_snd_ssthresh == 22` を検証します。
2. **Round 1：純粋な Slow Start（24〜31 行目、`cwnd: 10 -> 20`）**:
   - `write(4, ..., 10000)` で 10 セグメントを送信します。`snd_cwnd == 10` をすべて使い切るため、`tp->is_cwnd_limited` が `true` になります。
   - `ack 10001`（`acked = 10`）を受信すると、Slow Start によって `tcpi_snd_cwnd == 20` に倍増することをアサートします。
3. **Round 2：`snd_ssthresh` 境界でのクランプと余剰クレジットの引き継ぎ（33〜43 行目、`cwnd: 20 -> 22`）**:
   - `write(4, ..., 20000)` で 20 セグメントを送信して `snd_cwnd == 20` を使い切ります。
   - `ack 30001`（`acked = 20`）を受信すると、Slow Start で `2` セグメント分使って `tcpi_snd_cwnd == 22` に到達し、残りの `18` セグメント分が `tp->snd_cwnd_cnt = 18` に蓄えられることを検証します。
4. **Round 3：Reno Additive Increase（45〜53 行目、`cwnd: 22 -> 23`）**:
   - `write(4, ..., 22000)` で 22 セグメントを送信し、`ack 52001`（`acked = 22`）を受信します。
   - Congestion Avoidance により `tp->snd_cwnd_cnt` が `18 + 22 = 40 >= 22` となり、`tcpi_snd_cwnd == 23` に `+1` されることを検証します。

### 5.2 なぜ `starter.bpf.c` は失敗し、`solution.bpf.c` はパスするのか？

- **`starter.bpf.c` が失敗する理由**: `starter.bpf.c` の `bpf_reno1_cong_avoid()` はまだ空（何もしない状態）なので、Round 1 で `ack 10001` を受信しても `tp->snd_cwnd` が初期値 `10` のまま変化せず、30 行目の `assert tcpi_snd_cwnd == 20`（`expected cwnd 20 after Slow Start, got 10`）で即座に失敗します。
- **`solution.bpf.c` がパスする理由**: Slow Start・`snd_ssthresh` 境界での余剰 `acked` の引き継ぎ・Additive Increase のすべてが実装されているため、Round 1 (`20`)、Round 2 (`22`)、Round 3 (`23`) のすべてのアサーションを通過します。

### 5.3 テストの実行コマンド

以下のコマンドで、`starter` の失敗確認と `solution`（または自分で編集した `starter`）のテストを実行できます。

```sh
../../run_test.sh reno/step01-slow-start-ai starter
../../run_test.sh reno/step01-slow-start-ai solution
```
