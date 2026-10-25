# BBRv1 Step 5: PROBE_RTT への遷移と最小パイプサイズへのクランプ（cwnd = 4）

## 1. 学習ゴール

本ステップでは、BBRv1 の 4 つ目の状態である `BBR_PROBE_RTT` を BPF で実装し、ボトルネックに常時滞留するキュー（standing queue）や経路変化によって陳腐化した最小 RTT（`min_rtt_us`）を定期的に再計測する仕組みを完成させます。具体的には、最小 RTT フィルタの有効期限（`BBR_MIN_RTT_WIN_US`）の満了判定、突入前の輻輳ウィンドウを `ca->prior_cwnd` に退避して `snd_cwnd` を最小パイプサイズ（`BBR_MIN_PIPE_CWND = 4`）へクランプする処理、そして「インフライト数が 4 パケット以下に落ちてから 200 ms 以上、かつ 1 ラウンド往復完了」を待って元のモードとウィンドウサイズへ復帰する一連のステートマシンを実装します（トラック全体の概要は [../README.md](../README.md) を参照してください）。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

### なぜ `BBR_PROBE_BW` だけでは真の最小 RTT（RTprop）を見失うのか？

BBR はネットワーク経路の帯域遅延積（$\text{BDP} = \text{BtlBw} \times \text{RTprop}$）を推定するために、直近の最大配送レート `max_bw`（$\approx \text{BtlBw}$）と直近の最小 RTT `min_rtt_us`（$\approx \text{RTprop}$）という 2 つの独立した推定量を維持しています。しかし、物理的な往復伝播遅延（$\text{RTprop}$）は「ボトルネックのバッファが完全に空（キュー遅延 $= 0$）の瞬間」にしか観測できません。

定常状態の `BBR_PROBE_BW` では、8 フェーズのゲインサイクル（`[1.25, 0.75, 1, 1, 1, 1, 1, 1]`）を回します。フェーズ 0（ゲイン `1.25`）で自ら余分に積み上げたキュー（$0.25 \times \text{BDP}$）はフェーズ 1（ゲイン `0.75`）で排出されますが、その後の 6 フェーズは常にインフライト数を $1.0 \times \text{BDP}$ に保ち続けます。このため、以下の 2 つの問題が発生します。

1. **複数フロー競合による常時滞留キュー（Standing Queue）**:
   ボトルネックにすでにキューが存在している状態で別フローが合流すると、各フローが $1.0 \times \text{BDP}$ を維持し続ける限り、ボトルネックのキューは一度も 0 パケットになりません。結果として、観測されるすべての RTT サンプルが `真の RTprop + 常時滞留キュー遅延` となり、`min_rtt_us` が過大評価されたまま固定化してしまいます。
2. **経路変更による真の RTprop の増大**:
   ルーティングの切り替えなどで物理的な経路が長くなり、実際の最小 RTT が例えば `10 ms` から `15 ms` に増えたとします。もし `min_rtt_us` を「接続開始からの単純な最小値」として保持し続けると、過去の古い `10 ms` が永遠に残り続け、新しい `15 ms` に更新されません。

### `BBR_PROBE_RTT` の基本アイデアと数理モデル

この問題を解決するため、BBR は最小 RTT フィルタに **有効期限ウィンドウ `BBR_MIN_RTT_WIN_US`**（Linux カーネル `net/ipv4/tcp_bbr.c` では 10 秒、本ラボでは `packetdrill` の実行時間を短縮するため **2 秒**）を設けます。

- **ウィンドウ期間内の更新条件（厳密な不等号 `<`）**:
  新しい ACK から得られた RTT サンプル `rs->rtt_us` が現在の `ca->min_rtt_us` より**厳密に小さい**（`rs->rtt_us < ca->min_rtt_us`）場合、`ca->min_rtt_us = rs->rtt_us` に更新し、更新時刻 `ca->min_rtt_stamp = tp->tcp_mstamp` を記録します。ここで `<=` ではなく厳密な `<` を使う理由は、同じ RTT 値が連続したときにタイムスタンプが延々と更新され、フィルタが永久に失効しなくなるのを防ぐためです。
- **ウィンドウ失効（`filter_expired`）の判定**:
  最後に `ca->min_rtt_us` を更新してから `BBR_MIN_RTT_WIN_US` を超えて時間が経過した場合、フィルタが失効したと判定します。
  $$\text{filter\_expired} \iff \text{tp->tcp\_mstamp} > \text{ca->min\_rtt\_stamp} + \text{BBR\_MIN\_RTT\_WIN\_US}$$
  フィルタ失効時、受信側の遅延 ACK タイマーによる水増しサンプルでない限り（`!rs->is_ack_delayed`）、現在の `rs->rtt_us`（たとえ以前の `min_rtt_us` より大きくても）で `ca->min_rtt_us` と `ca->min_rtt_stamp` を上書きし、`BBR_PROBE_RTT` モードへ遷移します。

### なぜ `cwnd = 4` に絞り、「200 ms かつ 1 ラウンド」待つのか？

`BBR_PROBE_RTT` に入ると、BBR は現在の輻輳ウィンドウを `ca->prior_cwnd` に退避した上で、`pacing_gain = 1.0`（`BBR_UNIT`）、`cwnd_gain = 1.0`（`BBR_UNIT`）に設定し、輻輳ウィンドウ `tp->snd_cwnd` を最小パイプサイズ **`BBR_MIN_PIPE_CWND = 4` パケット**に強制クランプします。

- **なぜ 1 パケットではなく 4 パケットなのか？**:
  標準的な TCP 受信側は遅延 ACK（Delayed ACK：2 パケットごとに 1 回 ACK を返す仕組み）を使用します。もし `snd_cwnd` を 1〜2 パケットまで絞ってしまうと、受信側で最大 40〜200 ms の Delayed ACK タイマー待ちが発生して ACK クロックが止まってしまいます。`BBR_MIN_PIPE_CWND = 4` パケットを確保すれば、少なくとも 2 回の即時 ACK トリガーが発生し、パイプを失速させずにキューを排出できます。
- **なぜ `tp->app_limited` をセットするのか？**:
  `snd_cwnd = 4` に絞っている間は、ネットワークの帯域上限ではなくウィンドウ制限によって送信レートが極端に低くなります。この期間の低い配送レートサンプルで最大帯域 `ca->max_bw` が汚染されないよう、ソケットをアプリケーション制限状態（`tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1`）としてマークします（`0` は「制限なし」を意味するため、万一和が `0` になる場合は `1` にフォールバックします）。
- **滞在期間の開始条件と 2 つの完了条件**:
  `tp->snd_cwnd = 4` に設定した直後は、それ以前に送信した数十〜数百個のパケットがまだネットワーク内を飛行中です（`tcp_packets_in_flight(tp) > 4`）。それらが受信側に届いてパイプから抜け、**実際にインフライト数が 4 パケット以下（`tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`）に減少した瞬間**に初めて、200 ms の滞在タイマーとパケット往復ラウンドの計測を開始します。
  - **条件 A（時間条件：200 ms 経過）**: `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US`（`200 ms`）を設定し、`tp->tcp_mstamp > ca->probe_rtt_done_stamp` となるまで待ちます。複数の競合フローが少しずつ異なるタイミングで `BBR_PROBE_RTT` に入っても、200 ms 維持することで互いの排出期間が重なりやすくなります。なお、カーネル標準の 10 秒周期で 200 ms 滞在する場合、帯域利用のオーバーヘッドは $\frac{200\text{ ms}}{10\text{ s}} = 2\%$ に抑えられます。
  - **条件 B（ラウンド条件：1 ラウンド完了）**: インフライトが 4 以下になった時点の `tp->delivered` を `ca->next_rtt_delivered` に記録し、`ca->probe_rtt_round_done = 0` とします。その後、1 往復分のパケットが配送されて `ca->round_start` が立った時点で `ca->probe_rtt_round_done = 1` とします。これは RTT が 200 ms を超える長遅延経路（衛星回線など）において、「キューが空になった状態で送ったパケットの ACK」が確実に返ってくるまで待つためです。
- **モード脱出と `prior_cwnd` の復元**:
  条件 A と条件 B の**両方**が満たされると（`bbr_check_probe_rtt_done()`）、`ca->min_rtt_stamp = tp->tcp_mstamp` に更新して直後の再突入を防ぎ、`tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)` で突入前のウィンドウサイズを一気に復元した上で、`bbr_reset_mode()` により元のモード（`full_bw_reached` が真なら `BBR_PROBE_BW`、偽なら `BBR_STARTUP`）へ復帰します。

```
[BBR_STARTUP / BBR_PROBE_BW] (cwnd = 20, min_rtt_us = 10 ms)
       │
       │  2.0 秒経過 (tp->tcp_mstamp > min_rtt_stamp + BBR_MIN_RTT_WIN_US)
       ▼
  filter_expired == true
  ・bbr_save_cwnd() で prior_cwnd = 20 を退避
  ・ca->mode = BBR_PROBE_RTT, ca->probe_rtt_done_stamp = 0
  ・bbr_set_cwnd() の末尾で tp->snd_cwnd = min(cwnd, 4) にクランプ
       │
       ▼
[BBR_PROBE_RTT: パイプ排出待ち]
  ・tp->app_limited を設定し max_bw の低下を防止
  ・tcp_packets_in_flight(tp) <= 4 になった瞬間にタイマーとラウンド計測を開始:
      probe_rtt_done_stamp = tp->tcp_mstamp + 200 ms
      probe_rtt_round_done = 0
      next_rtt_delivered   = tp->delivered
       │
       │  (1) round_start == 1 により probe_rtt_round_done = 1 (1 ラウンド完了)
       │  (2) tp->tcp_mstamp > probe_rtt_done_stamp (200 ms 経過)
       ▼
[bbr_check_probe_rtt_done()]
  ・ca->min_rtt_stamp = tp->tcp_mstamp
  ・tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)  (cwnd = 20 に即座に復元)
  ・bbr_reset_mode() で BBR_STARTUP または BBR_PROBE_BW へ復帰
```

### 具体例（`test.pkt` の数値で追うタイムライン）

1. **`t = 0.010 s`（接続確立）**: 初期 RTT サンプル `10 ms`（`10,000 us`）を観測し、`ca->min_rtt_us = 10000`、`ca->min_rtt_stamp = t_0` に記録されます。初期 `snd_cwnd` は `10` です。
2. **`t = 0.020 s`（データ 10 パケット送信・ACK 受信）**: `BBR_STARTUP` 中に 10 パケットが ACK され、`snd_cwnd` は `10 + 10 = 20` に成長します。RTT は `10 ms`（`< 10 ms` ではない）なので `min_rtt_stamp` は `t_0` のままです。
3. **`t = 2.135 s`（`+2.1 s` 経過後に RTT = 15 ms の ACK 到着）**: `tp->tcp_mstamp - ca->min_rtt_stamp` が約 `2.125 s` となり、`BBR_MIN_RTT_WIN_US`（`2.0 s`）を超過するため `filter_expired = true` になります。
   - `!rs->is_ack_delayed` なので `ca->min_rtt_us = 15000`（15 ms）、`ca->min_rtt_stamp = tp->tcp_mstamp` に更新されます。
   - `bbr_save_cwnd()` が `ca->prior_cwnd = 20` を保存し、`ca->mode = BBR_PROBE_RTT` に遷移します。
   - インフライト数は `0 <= 4` なので即座に `ca->probe_rtt_done_stamp = tp->tcp_mstamp + 200 ms` がセットされます。
   - `bbr_set_cwnd()` の末尾で `tp->snd_cwnd` が `4`（`BBR_MIN_PIPE_CWND`）にクランプされます。
4. **`t = 2.395 s`（さらに `+0.25 s` 経過後に 1 パケット送信・RTT = 10 ms で ACK 到着）**:
   - `rs->rtt_us = 10000 < ca->min_rtt_us (15000)` のため、`ca->min_rtt_us` が真の最小 RTT である `10000`（10 ms）に再更新されます。
   - 新たな 1 パケットの往復により `ca->round_start = 1` となり、`ca->probe_rtt_round_done = 1` になります。
   - `tp->tcp_mstamp > ca->probe_rtt_done_stamp`（250 ms > 200 ms）も満たされているため `bbr_check_probe_rtt_done()` が実行され、`tp->snd_cwnd` は `max(4, 20) = 20` に復元され、元のモードへ戻ります。

### カーネルがすでに提供している構造体フィールドとヘルパー

BPF プログラムで Step 5 を実装するにあたり、カーネル側（`struct tcp_sock`、`struct rate_sample`、`struct inet_connection_sock`）および `bpf_cc_helpers.h` が提供する要素は以下の通りです。

- **`icsk_ca_priv` と `inet_csk_ca(sk)`**:
  `struct inet_connection_sock` 内の `icsk_ca_priv` は、各輻輳制御アルゴリズムが自由に使えるソケットごとのプライベートメモリ領域です。本ラボでは `inet_csk_ca(sk)` ヘルパーを使って `struct bbr_ca *ca` としてアクセスし、`min_rtt_us`、`min_rtt_stamp`、`probe_rtt_done_stamp`、`prior_cwnd`、`probe_rtt_round_done` などの BBR 独自状態を保持します。
- **`struct tcp_sock`（`tp = tcp_sk(sk)` で取得）**:
  - `tp->tcp_mstamp`: パケット送受信時に更新されるマイクロ秒単位の現在時刻タイムスタンプ。`min_rtt_stamp` や `probe_rtt_done_stamp` との比較に使います。
  - `tp->delivered`: これまでに受信側へ届けられた総パケット数。パケット単位のラウンド判定（`next_rtt_delivered`）や `tp->app_limited` の算出に使います。
  - `tp->delivered_mstamp`: 直近の配送タイムスタンプ（`BBR_PROBE_BW` 復帰時の `cycle_mstamp` 初期化に使用）。
  - `tp->snd_cwnd` / `tp->snd_cwnd_clamp`: 現在の輻輳ウィンドウ（パケット数）とその上限値。
  - `tp->app_limited`: 0 以外の値がセットされている間、カーネルのレート計測器に「現在はアプリケーション制限（または意図的なウィンドウ制限）中である」と伝えるインデックス値。
- **ヘルパー関数 `tcp_packets_in_flight(tp)`**:
  `tp->packets_out - tp->left_out + tp->retrans_out` を計算し、現在ネットワーク内に飛行中（未確認）のパケット数を返します。
- **`struct rate_sample`（`.cong_control` コールバックの引数 `rs`）**:
  - `rs->rtt_us`: この ACK で計測された RTT（マイクロ秒、符号付き 32 ビット整数。無効な場合は負値 `< 0`）。
  - `rs->is_ack_delayed`: 受信側の Delayed ACK によって遅延した ACK である場合に真となるフラグ。
  - `rs->is_app_limited`: サンプル計測区間が `tp->app_limited` 中であった場合に真となるフラグ。
  - `rs->acked_sacked`: この ACK で新たに確認応答されたパケット数。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の仕組みを踏まえて、`starter.bpf.c` の TODO を実装する前に以下の 4 つの設計ポイントを確認しましょう。

### Q1. `bbr_update_min_rtt()` で `min_rtt_us` を更新する 2 つの条件は何か？
`rs->rtt_us >= 0`（有効な RTT サンプル）であるとき、`ca->min_rtt_us` と `ca->min_rtt_stamp` を更新すべき条件式はどう書くべきでしょうか？ また、なぜ `<=` ではなく `<` を使い、フィルタ失効時には `!rs->is_ack_delayed` をチェックするのでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
ウィンドウ期間内に「これまでの最小値を下回るサンプルが来たとき」と、ウィンドウが期限切れ（`filter_expired`）になって「古い最小値を捨てて現在の RTT を採用するとき」の 2 パターンがあります。

**答え**:
条件式は以下のように記述します。
```c
filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
if (rs->rtt_us >= 0 &&
    ((__u32)rs->rtt_us < ca->min_rtt_us ||
     (filter_expired && !rs->is_ack_delayed))) {
	ca->min_rtt_us = rs->rtt_us;
	ca->min_rtt_stamp = tp->tcp_mstamp;
}
```
- `(__u32)rs->rtt_us < ca->min_rtt_us` で厳密な不等号 `<` を使うのは、同一の RTT サンプルが連続した際に `ca->min_rtt_stamp` がリセットされ続け、`filter_expired` が永遠に真にならなくなるのを防ぐためです。
- `filter_expired && !rs->is_ack_delayed` では、ウィンドウ失効時に新しい基準 RTT を取り込む際、受信側の Delayed ACK タイマーによる遅延が含まれたサンプル（`rs->is_ack_delayed`）を除外することで、`min_rtt_us` の過大評価を防ぎます。
</details>

---

### Q2. `filter_expired` 時に `BBR_PROBE_RTT` へ遷移する際、なぜ `probe_rtt_done_stamp` をすぐには設定せず `0` にリセットするのか？
`filter_expired && ca->mode != BBR_PROBE_RTT` が成立した瞬間、すぐに `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` としないのはなぜでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
モードを切り替えた瞬間には、まだネットワークのパイプ内に大量の飛行中パケット（`tcp_packets_in_flight(tp) > 4`）が残っている可能性があります。

**答え**:
`BBR_PROBE_RTT` に入った時点では `bbr_save_cwnd(sk)` で現在の `snd_cwnd` を `ca->prior_cwnd` に退避し、`ca->mode = BBR_PROBE_RTT`、`ca->probe_rtt_done_stamp = 0`（タイマー未開始）に初期化します。
その後、飛行中パケットが実際に抜けて `tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`（4 パケット以下）になったことを検知してから初めて、`ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` をセットします。もし突入と同時に 200 ms タイマーを開始してしまうと、パイプ内の既存パケットが抜け切る前にタイマーが満了し、キューが空にならないまま `BBR_PROBE_RTT` を終了してしまう恐れがあるためです。
</details>

---

### Q3. `BBR_PROBE_RTT` 滞在中に `tp->app_limited` を設定し、`probe_rtt_round_done` と `probe_rtt_done_stamp` の両方を待つにはどう実装するか？
`ca->mode == BBR_PROBE_RTT` のブロック内で、毎回の ACK ごとに行うべき処理と、`bbr_check_probe_rtt_done()` を呼ぶタイミングを整理してみましょう。

<details>
<summary>ヒントと答え</summary>

**ヒント**:
1. まず `tp->app_limited` を非ゼロに更新して帯域推定値 `max_bw` を保護します。
2. `ca->probe_rtt_done_stamp == 0`（タイマー未開始）なら、`tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND` かどうかを調べます。
3. すでに `ca->probe_rtt_done_stamp != 0`（タイマー開始済み）なら、`ca->round_start` でラウンド完了フラグを立て、フラグが立っていれば時間満了をチェックします。

**答え**:
1. 帯域フィルタ保護のため、毎回 `tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;` をセットします。
2. `!ca->probe_rtt_done_stamp && tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND` のとき、タイマーと新ラウンドを開始します：
   - `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;`
   - `ca->probe_rtt_round_done = 0;`
   - `ca->next_rtt_delivered = tp->delivered;`
3. `else if (ca->probe_rtt_done_stamp)` のとき：
   - `if (ca->round_start)` ならば `ca->probe_rtt_round_done = 1;` とします。
   - `if (ca->probe_rtt_round_done)` ならば `bbr_check_probe_rtt_done(sk);` を呼び出し、`tp->tcp_mstamp > ca->probe_rtt_done_stamp` であれば `ca->min_rtt_stamp = tp->tcp_mstamp` の更新、`tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)` の復元、および `bbr_reset_mode(sk)` によるモード復帰を行います。
</details>

---

### Q4. `bbr_set_cwnd()` のどこで `BBR_MIN_PIPE_CWND` へのクランプを行うべきか？
`bbr_set_cwnd()` には `if (!acked) goto done;` という分岐や、`target_cwnd` に向かって `cwnd` を増減させる処理があります。`BBR_PROBE_RTT` のウィンドウ制限（4 パケット）は関数のどの位置に書くべきでしょうか？

<details>
<summary>ヒントと答え</summary>

**ヒント**:
`acked == 0` で `goto done;` にジャンプした場合でも、`BBR_PROBE_RTT` に遷移した直後のパケット送信前には必ず `tp->snd_cwnd` が 4 以下に制限されている必要があります。

**答え**:
関数の末尾にある `done:` ラベルの後、`tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);` の直後に配置します。
```c
done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
```
これにより、通常の `cwnd` 計算ロジックや `acked == 0` の早期リターン経路にかかわらず、`ca->mode == BBR_PROBE_RTT` である限り最終的な `tp->snd_cwnd` は確実に `BBR_MIN_PIPE_CWND`（4）以下にクランプされます。
</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

`solution.bpf.c` における各関数と固定小数点演算・状態遷移の実装を詳しく見ていきます。

### 1. 定数定義と固定小数点スケール

```c
#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_MIN_PIPE_CWND	4U
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)
```

- 帯域 `max_bw` は `BW_SCALE = 24`（`1 << 24` スケールのパケット/マイクロ秒）、各種ゲインは `BBR_SCALE = 8`（`BBR_UNIT = 256` が `1.0` 倍に対応）の固定小数点数で表現されます。
- `bbr_bdp()` では `w = (__u64)bw * ca->min_rtt_us` に `gain` を掛けて `BBR_SCALE` と `BW_SCALE` で右シフト（切り上げ補正 `+ BW_UNIT - 1` 付き）することで BDP パケット数を算出します。
- `BBR_MIN_RTT_WIN_US` は `2ULL * USEC_PER_SEC`（2,000,000 マイクロ秒 = 2 秒）、`BBR_PROBE_RTT_MODE_US` は `200ULL * USEC_PER_MSEC`（200,000 マイクロ秒 = 200 ミリ秒）です。

### 2. `bbr_save_cwnd()` と `bbr_reset_mode()`

```c
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
```

- `bbr_save_cwnd()` は `BBR_PROBE_RTT` 突入時（およびロス検知時の `bpf_bbr5_ssthresh()`）に呼ばれ、`BBR_PROBE_RTT` 以外のモードであれば現在の `tp->snd_cwnd` をそのまま `ca->prior_cwnd` に保存し、すでに `BBR_PROBE_RTT` 中であれば `max(ca->prior_cwnd, tp->snd_cwnd)` で小さい値 (4) による上書きを防ぎます。
- `bbr_reset_mode()` は `BBR_PROBE_RTT` 完了時に呼ばれ、`ca->full_bw_reached` に応じて `BBR_STARTUP` または `BBR_PROBE_BW` に復帰させます。

### 3. `bbr_check_probe_rtt_done()` と `bbr_update_min_rtt()`

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
```

- `bpf_bbr5_cong_control()` では最初に `bbr_update_bw()` が呼ばれて `ca->round_start` が更新された後、`bbr_update_min_rtt()` が呼ばれます。
- `ca->probe_rtt_done_stamp` をセットしたまさにその ACK では `if (!ca->probe_rtt_done_stamp && ...)` の分岐に入り、次の ACK 以降で `else if (ca->probe_rtt_done_stamp)` の分岐に入って `ca->round_start` による 1 ラウンド完了と `bbr_check_probe_rtt_done()` が判定されます。
- `bbr_check_probe_rtt_done()` で `tp->snd_cwnd` を `ca->prior_cwnd` に復元し `bbr_reset_mode()` で `ca->mode` を戻すため、直後に呼ばれる `bbr_update_gains()` と `bbr_set_cwnd()` でもクランプは解除され、復元された `snd_cwnd` が維持されます。

### 4. `bbr_update_gains()` と `bbr_set_cwnd()`

```c
static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	// ...
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
```

- `BBR_PROBE_RTT` 中は `pacing_gain = BBR_UNIT`（1.0 倍）、`cwnd_gain = BBR_UNIT`（1.0 倍）となります。
- `bbr_set_cwnd()` の最終行で `if (ca->mode == BBR_PROBE_RTT)` をチェックし、`tp->snd_cwnd` を `BBR_MIN_PIPE_CWND`（4）以下にクランプします。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### `test.pkt` のパケットトレースと検証内容

`test.pkt` は以下の 3 段階で `BBR_PROBE_RTT` の突入・クランプ・脱出を検証します。

1. **接続確立と初期ウィンドウ成長（12〜23 行目）**:
   ```
   +0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
   +0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
   +0.010 < . 1:1(0) ack 1 win 512
   ...
   +0.000 write(4, ..., 10000) = 10000
   +0.000 > P. 1:10001(10000) ack 1
   +0.010 < . 1:1(0) ack 10001 win 512
   +0.000 %{
   assert tcpi_snd_cwnd == 20, f"expected cwnd == 20, got {tcpi_snd_cwnd}"
   }%
   ```
   - 3-way ハンドシェイクで `RTT = 10 ms` が計測され、`min_rtt_us = 10000` が記録されます。
   - 10 セグメント（10,000 バイト、`mss = 1000`）を送信して 10 ms 後に ACK を受け取ると、`BBR_STARTUP` において `snd_cwnd` は `10 + 10 = 20` に増加します。

2. **`+2.100 s` 経過によるフィルタ失効と `cwnd = 4` クランプの検証（27〜34 行目）**:
   ```
   +2.100 write(4, ..., 1000) = 1000
   +0.000 > P. 10001:11001(1000) ack 1
   +0.015 < . 1:1(0) ack 11001 win 512

   +0.000 %{
   assert tcpi_snd_cwnd == 4, f"expected cwnd == 4 in PROBE_RTT, got {tcpi_snd_cwnd}"
   }%
   ```
   - `+2.100` 秒待機してから 1 セグメント送信し、意図的に元の `10 ms` より大きい `15 ms`（`+0.015`）で ACK を返します。
   - `BBR_MIN_RTT_WIN_US`（2 秒）を超過しているため `filter_expired` が真となり、`prior_cwnd = 20` を保存して `BBR_PROBE_RTT` に突入し、`tcpi_snd_cwnd == 4` にクランプされることをアサートします。

3. **`+0.250 s` 経過と 1 ラウンド完了による `prior_cwnd` 復元の検証（38〜45 行目）**:
   ```
   +0.250 write(4, ..., 1000) = 1000
   +0.000 > P. 11001:12001(1000) ack 1
   +0.010 < . 1:1(0) ack 12001 win 512

   +0.000 %{
   assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
   }%
   ```
   - `BBR_PROBE_RTT_MODE_US`（200 ms）を超える `+0.250` 秒待機後に 1 セグメントを送信し、`RTT = 10 ms` で ACK を受け取ります。
   - これにより「200 ms 経過」と「インフライト $\le 4$ での 1 ラウンド完了」の両方が満たされて `BBR_PROBE_RTT` を脱出し、`prior_cwnd`（20）が復元された上でこの ACK の 1 セグメント分が加算され、`tcpi_snd_cwnd >= 20` となることをアサートします。

### なぜ `starter.bpf.c` は失敗し、`solution.bpf.c` はパスするのか？

- `starter.bpf.c` では `bbr_update_min_rtt()` 内の `filter_expired` 判定と `BBR_PROBE_RTT` への状態遷移、および `bbr_set_cwnd()` 末尾の `BBR_MIN_PIPE_CWND` クランプが未実装（TODO）となっています。そのため、`+2.100 s` 経過後も `BBR_STARTUP` のまま `snd_cwnd` が `21` に増えてしまい、33 行目の `assert tcpi_snd_cwnd == 4` で失敗します。
- `solution.bpf.c`（または TODO をすべて実装した `starter.bpf.c`）では、2.1 秒後のフィルタ失効で `snd_cwnd` が `4` にクランプされ、さらに 0.25 秒後の ACK で `prior_cwnd`（20）以上に復元されるため、すべてのアサートをパスします。

### テストの実行コマンド

以下のコマンドで `starter`（失敗の確認）と `solution`（パスの確認）を実行できます。

```sh
../../run_test.sh bbr/step05-probe-rtt starter
../../run_test.sh bbr/step05-probe-rtt solution
```
