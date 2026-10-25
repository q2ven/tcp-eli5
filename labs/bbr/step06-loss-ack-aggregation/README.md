# BBRv1 Step 6: Loss 時の cwnd 保存・復元と ACK 集約 (`extra_acked`) 補償

## 1. 学習ゴール

この最終ステップでは、BBRv1 の **パケットロス発生時の応答（Packet Conservation と `prior_cwnd` の保存・復元）** と、Wi-Fi や LRO/GRO 環境で頻発する **ACK 集約（ACK Aggregation）への適応（`extra_acked`）** を実装し、全 6 ステップの BPF BBRv1 を完成させます。Reno や CUBIC のようなロスベース輻輳制御とは異なり、BBR がなぜロス時に `snd_cwnd` を半減させないのか、またバースト状に返ってくる ACK に対してどのようにウィンドウを下支えするのかを数式とコードの両面から学びます。

- トラック全体の構成と各ステップの一覧については、[../README.md](../README.md) を参照してください。

---

## 2. アルゴリズムと背景の解説（まずここを読む）

コードの設計に入る前に、BBRv1 が扱う 2 つの重要な仕組みと、Linux カーネルが提供するデータ構造・コールバックの仕様を基礎から整理します。

### 2.1 なぜ BBR はロスで `cwnd` を半減させず、どう振る舞うのか？

Reno や CUBIC などの従来アルゴリズムは、「パケットロス＝ボトルネックのバッファ溢れ（輻輳）」とみなして、ロス検出のたびに `snd_cwnd` を 50% や 30% 削減します。しかし現代のネットワーク（浅いバッファのスイッチや無線リンクなど）では、回線帯域を使い切っていなくても一時的なバーストやランダムロスでパケットが欠損することがあります。

BBR はネットワークの物理容量を **最大帯域（`max_bw` = BtlBw）** と **最小伝播遅延（`min_rtt_us` = RTprop）** の積（BDP）として直接モデル化しています。パケットが 1 個ロスしたからといって、物理的な回線帯域や光の伝播遅延が縮むわけではありません。そのため BBR はロスが発生しても `max_bw` やペーシングレートを引き下げず、以下の **3 段階のロス制御** を行います。

1. **ロス直前の健全な `cwnd` の退避（`bbr_save_cwnd()`）**:
   カーネルがロスを検知して Fast Recovery（`TCP_CA_Recovery`）や RTO タイムアウト（`TCP_CA_Loss`）に入る直前、あるいは `BBR_PROBE_RTT` に入る直前に、その時点の正常な `tp->snd_cwnd` を `ca->prior_cwnd` に保存します。
   - すでに Recovery / Loss 中（`ca->prev_ca_state >= TCP_CA_Recovery`）または `BBR_PROBE_RTT` 中（`ca->mode == BBR_PROBE_RTT`）である場合は、`tp->snd_cwnd` がすでに小さく絞られている可能性があるため、単純代入で上書きせず $\text{prior\_cwnd} = \max(\text{prior\_cwnd}, \text{snd\_cwnd})$ で大きい方の値を保持します。
2. **Recovery 第 1 ラウンドのパケット保存則（Packet Conservation）**:
   パケットロスが起きた直後は、ネットワーク内に欠損パケットが存在し不安定な状態です。そのため、今回の ACK で新たに検知されたロスパケット数 `rs->losses` だけ一旦 `cwnd` を減算します（下限 1）：
   $$\text{cwnd} = \max(\text{cwnd} - \text{rs->losses}, 1)$$
   さらに、`TCP_CA_Recovery` に突入した最初の **1 ラウンドトリップ（1 RTT）** の間だけ、`ca->packet_conservation = 1` を有効化し、「ネットワークから抜けた（ACK/SACK された）パケット数 `acked` と同じ数だけ新規送信を許可する」**パケット保存則（Packet Conservation）** を適用します：
   $$\text{cwnd} = \text{tcp\_packets\_in\_flight}(\text{tp}) + \max(\text{acked}, 1)$$
   この 1 ラウンドの終わりを測るため、Recovery 突入時に `ca->next_rtt_delivered = tp->delivered` を記録し、`bbr_update_bw()` で次のラウンド開始（`!before(rs->prior_delivered, ca->next_rtt_delivered)`）が検知された時点で `ca->packet_conservation = 0` に解除します。
3. **Recovery / Loss 脱出時の `prior_cwnd` 復元**:
   欠損パケットの再送が完了し、状態が `TCP_CA_Recovery` 以上から `TCP_CA_Open` などの通常状態（`< TCP_CA_Recovery`）に戻った瞬間（または誤検知による `.undo_cwnd` 呼び出し時）、BBR は退避しておいた `ca->prior_cwnd` を使って即座に元のウィンドウ規模へ復元します：
   $$\text{cwnd} = \max(\text{cwnd}, \text{ca->prior\_cwnd})$$

#### 具体例（数値トレース）

- 通常状態（`TCP_CA_Open`）で `snd_cwnd = 20` のときに 4 パケット送信し、先頭 1 パケットがドロップ、後続 3 パケットが SACK されて Fast Recovery（`TCP_CA_Recovery`）に突入したとします。
- Recovery 突入直前に呼ばれる `.ssthresh` フックで `bbr_save_cwnd()` が走り、`ca->prior_cwnd = 20` が退避されます（`snd_ssthresh` 自体は変更しません）。
- Fast Retransmit 直後の ACK 処理において、ネットワーク内の未確認フライト数 `tcp_packets_in_flight(tp)` が `0`、今回の `acked` が `1` であれば、パケット保存則により `snd_cwnd = 0 + max(1, 1) = 1` に制限されます。
- その後、再送パケットを含む全データの累積 ACK が届いて `TCP_CA_Open` に復帰すると、`snd_cwnd = max(cwnd, prior_cwnd) = 20` へと即座に復元されます（Reno のように `10` に半減されません）。

---

### 2.2 ACK 集約（ACK Aggregation）と `extra_acked` の仕組み

#### なぜ問題が起きるのか（直感）

Wi-Fi のフレームアグリゲーション、モバイル回線、あるいは受信側 NIC の GRO/LRO（Generic/Large Receive Offload）環境では、本来なら一定間隔で滑らかに返ってくるはずの ACK がネットワーク途中でまとめられ、**「しばらく ACK が全く来ない無音期間のあと、数十パケット分の ACK が一気にドッと届く」** という現象（ACK 集約）が頻繁に発生します。

もし `target_cwnd` を純粋な $\text{BDP} \times \text{cwnd\_gain}$ だけに制限していると、ACK が途絶えている無音期間中にインフライト数が `snd_cwnd` の上限に達してしまい、ペーシングタイマーが送信可能であっても **cwnd 制限によって送信がストップ（パイプが枯渇）** してしまいます。

#### アルゴリズムのアイデアと数式

そこで BBR は、「もし `max_bw` の速度で滑らかに ACK が返ってきていたとしたら、エポック開始からの経過時間 `epoch_us` の間に本来届いているはずの期待 ACK 数（`expected_acked`）」を計算し、**実際に届いた累積 ACK 数（`ack_epoch_acked`）がそれをどれだけ上回ってバースト到着したか（`extra_acked`）** を計測します。

```text
累積 ACK 数 (パケット)
  ^
  |                     +--- 実際の累積 ACK (ack_epoch_acked)
  |                     |    ↑
  |                     |    | extra_acked = ack_epoch_acked - expected_acked
  |              +------+    ↓
  |              |     /
  |       +------+    /  <-- 期待される滑らかな ACK 直線 (expected_acked = max_bw * epoch_us)
  |      /           /
  +-----+-----------+---------> 時間 (delivered_mstamp)
  epoch_mstamp
```

各 ACK 到着時（`rs->acked_sacked > 0 && rs->delivered > 0 && rs->interval_us > 0`）の計算手順は以下の通りです。

1. **エポック経過時間と期待 ACK 数の計算**:
   エポック開始時刻 `ca->epoch_mstamp` から現在の配送タイムスタンプ `tp->delivered_mstamp` までの経過時間（マイクロ秒）を求め、`ca->max_bw`（`BW_SCALE = 24` の固定小数点、単位は `pkts/us << 24`）を掛けて期待 ACK 数を算出します：
   $$\text{epoch\_us} = \text{tp->delivered\_mstamp} - \text{ca->epoch\_mstamp}$$
   $$\text{expected\_acked} = (\text{ca->max\_bw} \times \text{epoch\_us}) \gg 24$$
2. **エポックのリセット判定**:
   もしこれまでの累積 ACK 数 `ca->ack_epoch_acked` が期待値 `expected_acked` 以下に落ち着いた場合（`ca->ack_epoch_acked <= expected_acked`）、またはカウンタがオーバーフロー防止閾値 `BBR_ACK_EPOCH_ACKED_RESET_THRESH`（$2^{20}$ パケット）に達する場合は、過去の集約バーストが解消されたとみなして新しいエポックをゼロから開始します：
   - `ca->ack_epoch_acked = 0`
   - `ca->epoch_mstamp = tp->delivered_mstamp`
   - `expected_acked = 0`
3. **超過 ACK 数 `extra_acked` の算出と最大値更新**:
   今回の ACK/SACK 数 `rs->acked_sacked` を加算し、期待値との差分を計算します（ただし現在の `tp->snd_cwnd` を上限とします）：
   $$\text{ack\_epoch\_acked} \mathrel{+}= \text{rs->acked\_sacked}$$
   $$\text{extra\_acked} = \min(\text{ack\_epoch\_acked} - \text{expected\_acked}, \text{tp->snd\_cwnd})$$
   この値が保持中の `ca->extra_acked` より大きければ `ca->extra_acked` を更新します。
4. **5 RTT ウィンドウによる減衰（Windowed Decay）**:
   大昔の単発バーストによって `ca->extra_acked` が永遠に高く維持されるのを防ぐため、`ca->round_start`（1 RTT ラウンドの開始）ごとに世代カウンタ `ca->extra_acked_win_rtts`（上限 `0x1F`）を 1 増やし、`BBR_EXTRA_ACKED_WIN_RTTS`（`5` ラウンド）に達したら `ca->extra_acked_win_rtts = 0` および `ca->extra_acked = 0` にリセットします。
5. **`target_cwnd` への補償加算**:
   `bbr_set_cwnd()` で目標ウィンドウを計算する際、`ca->extra_acked`（ただし暴走を防ぐため $\text{BDP} = \text{bbr\_bdp}(\text{sk}, \text{bw}, \text{gain})$ を上限とする）を上乗せします：
   $$\text{target\_cwnd} = \text{bbr\_inflight}(\text{sk}, \text{bw}, \text{gain}) + \min(\text{ca->extra\_acked}, \text{bbr\_bdp}(\text{sk}, \text{bw}, \text{gain}))$$

---

### 2.3 カーネルが提供する構造体フィールドとコールバック

このステップを実装するにあたり、カーネル側からすでに提供されているフィールドとヘルパーは以下の通りです。

- **`struct inet_connection_sock`（`inet_csk(sk)` で取得）**:
  - `icsk_ca_state` (`__u8`): カーネルの TCP 輻輳状態を表す列挙値です。数値の大小関係が意味を持ちます：
    - `TCP_CA_Open (0)`, `TCP_CA_Disorder (1)`, `TCP_CA_CWR (2)`: ロス復旧中ではない状態（`< TCP_CA_Recovery`）
    - `TCP_CA_Recovery (3)`: SACK / 重複 ACK による Fast Recovery 状態
    - `TCP_CA_Loss (4)`: RTO（再送タイムアウト）による Loss 復旧状態
    したがって、`state >= TCP_CA_Recovery` は「Fast Recovery または RTO Loss の最中」を意味し、`state < TCP_CA_Recovery` は「それらから抜けた状態」を意味します。
  - `icsk_ca_priv`（`inet_csk_ca(sk)` でポインタ取得）: 各ソケット固有の輻輳制御プライベート領域です。ここに独自の `struct bbr_ca` を配置して、`prior_cwnd` や `prev_ca_state`、`packet_conservation`、`extra_acked` などを保持します。
- **`struct tcp_sock`（`tcp_sk(sk)` で取得）**:
  - `tp->snd_cwnd` (`__u32`): 現在の輻輳ウィンドウ（パケット単位）。
  - `tp->snd_cwnd_clamp` (`__u32`): `snd_cwnd` の上限値。
  - `tp->snd_ssthresh` (`__u32`): スロースタート閾値。
  - `tp->delivered` (`__u32`): これまでに受信側へ届けられた累積パケット数。
  - `tp->delivered_mstamp` (`__u64`): 直近の配送が確認された時刻（マイクロ秒）。ACK 集約エポックの時刻計算に使用します。
  - `tp->tcp_mstamp` (`__u64`): ソケットの現在時刻タイムスタンプ（マイクロ秒）。
  - `tcp_packets_in_flight(tp)` (`bpf_cc_helpers.h` のヘルパー関数): 現在ネットワーク内を飛行中の未確認パケット数（`packets_out - (sacked_out + lost_out) + retrans_out`）を返します。
- **`struct rate_sample`（`.cong_control` の引数 `const struct rate_sample *rs`）**:
  - `rs->acked_sacked` (`__u32`): 今回の ACK で新たに ACK または SACK されたパケット数。
  - `rs->losses` (`__s32`): 今回の ACK で新たにロスと判定されたパケット数。
  - `rs->delivered` (`__s32`), `rs->interval_us` (`long`): サンプル区間の配送パケット数と経過時間（マイクロ秒）。これらが `<= 0` の場合は無効サンプルです。
  - `rs->prior_delivered` (`__u32`): 今回 ACK されたパケットが送信された時点の `tp->delivered`（1 RTT ラウンドの完了判定に使用）。
- **`struct tcp_congestion_ops` のフック群**:
  - `.ssthresh(sk)`: カーネルが `TCP_CA_Recovery` や `TCP_CA_Loss` に入る直前に呼び出されます。
  - `.set_state(sk, new_state)`: `icsk_ca_state` が `new_state` に遷移するたびに呼び出されます（特に ACK が届かないタイマー発火時の `TCP_CA_Loss` 遷移を捕捉するのに使われます）。
  - `.undo_cwnd(sk)`: ロス検知が誤り（Spurious Retransmission）だったと判明して巻き戻す際に呼び出されます。

---

## 3. 設計チェックポイント（理解した仕組みをコードに落とし込む）

セクション 2 の仕組みを踏まえて、[starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/starter.bpf.c) の `TODO` を埋めるための設計を 4 つの問いで確認しましょう。

### Q1. `bbr_save_cwnd()` と `bpf_bbr6_ssthresh()` では、いつ・どのように `ca->prior_cwnd` を保存すべきでしょうか？

カーネルが Fast Recovery に入る直前に `bpf_bbr6_ssthresh()` が呼ばれ、また `BBR_PROBE_RTT` に入る直前にも `bbr_save_cwnd()` が呼ばれます。もし `BBR_PROBE_RTT` 中（`snd_cwnd == 4`）にパケットロスが発生して `bbr_save_cwnd()` が再び呼ばれた場合、単純に `ca->prior_cwnd = tp->snd_cwnd` と代入すると何が起きてしまうでしょうか？どのような条件分岐にすべきか考えてみましょう。

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.1 のステップ 1 とセクション 2.3 の `icsk_ca_state` の大小関係を思い出しましょう。`tp->snd_cwnd` が本来の健全なウィンドウサイズを表しているのは、`ca->prev_ca_state` と `ca->mode` がどの状態のときだけでしょうか？
- **答え**:
  - もし `BBR_PROBE_RTT` 中（`snd_cwnd = 4`）や `TCP_CA_Recovery` 中（パケット保存則で `snd_cwnd` が絞られている最中）に単純代入 `ca->prior_cwnd = tp->snd_cwnd` を行うと、せっかく退避していた大きな `prior_cwnd`（たとえば `20`）が `4` や `1` で上書きされて失われてしまいます。
  - そのため、`ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT` のときだけ `ca->prior_cwnd = tp->snd_cwnd;` と保存し、それ以外の状態では `ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);` で既存の退避値を守ります。
  - そして `bpf_bbr6_ssthresh(sk)` では、`bbr_save_cwnd(sk)` を呼び出したうえで、ウィンドウを半減させずに `tp->snd_ssthresh` をそのまま返します。

</details>

---

### Q2. `bbr_set_cwnd_to_recover_or_restore()` で Recovery 突入・維持・脱出をどう実装しますか？

ACK ごとに呼ばれる `bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd)` では、現在の状態 `state = inet_csk(sk)->icsk_ca_state` と前回の状態 `prev_state = ca->prev_ca_state` を比較します。
1. `rs->losses > 0` のとき
2. `TCP_CA_Recovery` に突入した瞬間（`state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery`）
3. Recovery / Loss から通常状態へ抜けた瞬間（`prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery`）
4. `ca->packet_conservation` が有効な間

それぞれで `cwnd` や状態変数をどう更新し、関数の戻り値（`bool`）はどう返すべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.1 のステップ 2・3 をそのまま C の条件分岐に対応させます。符号なしアンダーフローを防ぐため、`cwnd - rs->losses` の減算時は `(__s32)cwnd - rs->losses` とキャストして `1` との `max()` を取ります。
- **答え**:
  1. **ロス減算**: `if (rs->losses > 0) cwnd = max((__s32)cwnd - rs->losses, 1);`
  2. **Recovery 突入時**:
     - `ca->packet_conservation = 1;`
     - 1 ラウンド計測の起点として `ca->next_rtt_delivered = tp->delivered;` をセット
     - `cwnd = tcp_packets_in_flight(tp) + max(acked, 1U);`
  3. **Recovery / Loss 脱出時**:
     - `cwnd = max(cwnd, ca->prior_cwnd);` で元のウィンドウを復元
     - `ca->packet_conservation = 0;` をクリア
  4. **状態更新と戻り値**:
     - `ca->prev_ca_state = state;` を保存します。
     - `if (ca->packet_conservation)` が真なら、`*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U));` を設定して `return true;` を返します（これにより呼び出し元の `bbr_set_cwnd()` は通常の BDP 計算をスキップして `done:` ラベルへジャンプします）。偽なら `*new_cwnd = cwnd; return false;` を返します。

</details>

---

### Q3. なぜ RTO 発生時は `bpf_bbr6_set_state()` で `TCP_CA_Loss` を記録する必要があるのでしょうか？

Fast Recovery（`TCP_CA_Recovery`）は `.cong_control` 内の `bbr_set_cwnd_to_recover_or_restore()` だけで遷移を検出できますが、RTO タイムアウト（`TCP_CA_Loss`）は `bpf_bbr6_set_state()` でも処理を行います。もし `bpf_bbr6_set_state()` が空のままだったら、RTO から回復したときにどのような不具合が起きるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: RTO タイマーが発火した瞬間には ACK が届いていないため `.cong_control` は呼ばれず、カーネルが `tp->snd_cwnd = 1` に落としたうえで `.set_state(sk, TCP_CA_Loss)` を呼び出します。その後、再送に対する ACK が届いて一気に `TCP_CA_Open` に戻ったら、`ca->prev_ca_state` は何になっているでしょうか？
- **答え**:
  - もし `.set_state` で `ca->prev_ca_state = TCP_CA_Loss` を記録していないと、RTO 発生前に `TCP_CA_Open` だった場合、再送 ACK で一気に `TCP_CA_Open` に復帰した際にも `prev_state` が `TCP_CA_Open` のままになってしまいます。
  - すると `bbr_set_cwnd_to_recover_or_restore()` の脱出条件 `prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery` が成立せず、`tp->snd_cwnd = 1` に落とされたウィンドウが `ca->prior_cwnd` に復元されなくなってしまいます。
  - そのため `bpf_bbr6_set_state()` で `if (new_state == TCP_CA_Loss)` を捕捉し、`ca->prev_ca_state = TCP_CA_Loss;`、`ca->full_bw = 0;`、`ca->round_start = 1;` を設定しておく必要があります。

</details>

---

### Q4. `bbr_update_ack_aggregation()` と `bbr_set_cwnd()` で `extra_acked` を計算・適用する際のガード条件と上限は何ですか？

セクション 2.2 の数式を `bbr_update_ack_aggregation(sk, rs)` と `bbr_set_cwnd()` に実装するとき、
1. どのような `rate_sample` のときは計算をスキップすべきですか？
2. エポックをリセットする 2 つの条件は何ですか？
3. `extra_acked` 自身と、`target_cwnd` に加算するときのクランプ（上限）はそれぞれ何に設定しますか？

<details>
<summary>ヒントと答え</summary>

- **ヒント**: セクション 2.2 のステップ 1〜5 を確認しましょう。固定小数点のスケールシフトは `BW_SCALE`（24 ビット）です。
- **答え**:
  1. **無効サンプルのガード**: `if (rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0) return;` で早期リターンします。
  2. **エポックのリセット条件**: `expected_acked = ((__u64)ca->max_bw * epoch_us) >> BW_SCALE;` を計算した後、`ca->ack_epoch_acked <= expected_acked`（集約が解消しベースライン以下になった）または `ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH`（オーバーフロー防止）のときに `ack_epoch_acked = 0`、`epoch_mstamp = tp->delivered_mstamp`、`expected_acked = 0` にリセットします。
  3. **上限クランプ**:
     - エポック内の `extra_acked = ca->ack_epoch_acked - expected_acked` は、`min(extra_acked, tp->snd_cwnd)` で現在の `snd_cwnd` 以下に制限してから `ca->extra_acked` の最大値を更新します。
     - `bbr_set_cwnd()` で `target_cwnd` に加える際は、`target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));` として 1 BDP（× gain）分までに制限します。

</details>

---

## 4. 実装ステップと解説 (`solution.bpf.c`)

[solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/solution.bpf.c) の各関数と固定小数点演算の詳細を解説します。

### 4.1 `bbr_save_cwnd()` と `bpf_bbr6_ssthresh()` / `bpf_bbr6_undo_cwnd()` / `bpf_bbr6_set_state()`

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

- `bpf_bbr6_ssthresh()` は `bbr_save_cwnd(sk)` を呼んでロス突入前の `snd_cwnd` を `ca->prior_cwnd` に退避し、`tp->snd_ssthresh` をそのまま返します。
- `bpf_bbr6_undo_cwnd()` は `ca->full_bw = 0; ca->full_bw_cnt = 0;` をリセットしたうえで `max(tp->snd_cwnd, ca->prior_cwnd)` を返します。
- `bpf_bbr6_set_state()` は `new_state == TCP_CA_Loss` のときに `ca->prev_ca_state = TCP_CA_Loss`、`ca->full_bw = 0`、`ca->round_start = 1` を設定します。

### 4.2 `bbr_set_cwnd_to_recover_or_restore()` — パケット保存則と `prior_cwnd` 復元

```c
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
```

- `TCP_CA_Recovery` に入った最初の 1 ラウンドは `ca->packet_conservation = 1` となり、`return true` によって `bbr_set_cwnd()` の通常計算をバイパスして `tcp_packets_in_flight(tp) + max(acked, 1U)` を強制します。
- 1 ラウンドが経過すると `bbr_update_bw()` 内の `!before(rs->prior_delivered, ca->next_rtt_delivered)` ブロックで `ca->packet_conservation = 0` にクリアされ、Recovery の第 2 ラウンド以降は通常の BDP ベース制御に戻ります。
- Recovery が完了して `state < TCP_CA_Recovery` に戻ると、`cwnd = max(cwnd, ca->prior_cwnd)` で退避ウィンドウが復元されます。

### 4.3 `bbr_update_ack_aggregation()` と `bbr_set_cwnd()` — ACK 集約の推定と補償

```c
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
```

- `ca->max_bw` は `pkts/us << BW_SCALE`（`BW_SCALE = 24`）の固定小数点値であるため、`epoch_us`（マイクロ秒）を掛けて `>> BW_SCALE` することで、エポック期間中に期待される ACK パケット数 `expected_acked` が整数で求まります。
- そして `bbr_set_cwnd()` では、`bbr_inflight(sk, bw, gain)` で求めた基本 `target_cwnd` に対して `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));` を加算し、ACK 集約によるパイプ枯渇を防ぎます。

---

## 5. `packetdrill` テストの解説 (`test.pkt`) と実行方法

### `test.pkt` のパケットトレースと検証内容

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/test.pkt) は、ロス発生時のパケット保存則と Recovery 脱出後の `prior_cwnd` 復元を以下の順序で検証します。

1. **初期スロースタートによるウィンドウ成長（行 17〜24）**:
   - 10 ms RTT で 3-way ハンドシェイク後、10 セグメント（`1:10001`、計 10,000 B）を送信し、10 ms 後に `ack 10001` を受信します。
   - `STARTUP` モードにより `tcpi_snd_cwnd` が初期値 `10` から `20` 以上に成長していること（`assert tcpi_snd_cwnd >= 20`）と `tcpi_ca_state == TCP_CA_Open` を確認します。
2. **パケットロスと 3 回の SACK による Fast Recovery 突入（行 26〜36）**:
   - 続けて 4 セグメント（`10001:14001`）を送信します。先頭セグメント（`10001:11001`）が失われ、後続 3 セグメントに対する SACK 重複 ACK（`<sack 11001:12001>`、`<sack 11001:13001>`、`<sack 11001:14001>`）が到着します。
   - 3 つ目の SACK でカーネルは `bpf_bbr6_ssthresh()` を呼んで `prior_cwnd`（20）を保存したあと、`TCP_CA_Recovery` に遷移して欠損セグメント `10001:11001` を高速再送（Fast Retransmit）します。
3. **パケット保存則のアサート（行 38〜43）**:
   - 3 つ目の SACK が処理された時点（高速再送の直前）では、送信済み 4 パケットのうち 1 パケットが lost、3 パケットが SACK 済みであるため `tcp_packets_in_flight(tp)` は `0` であり、この ACK で SACK された `acked` は `1` です。
   - したがってパケット保存則 `cwnd = in_flight + max(acked, 1)` により、`tcpi_snd_cwnd == 1` になっていることを検証します。
4. **Recovery 脱出と `prior_cwnd` 復元のアサート（行 45〜52）**:
   - `+0.010` 秒後に全 14,000 B を確認する累積 ACK（`ack 14001`）が到着し、`TCP_CA_Recovery` から `TCP_CA_Open` に復帰します。
   - `bbr_set_cwnd_to_recover_or_restore()` が `ca->prior_cwnd`（20）を復元するため、`tcpi_snd_cwnd >= 20` であることを検証します。

### なぜ `starter.bpf.c` は失敗し、`solution.bpf.c` はパスするのか？

- [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/starter.bpf.c) では、`bbr_set_cwnd_to_recover_or_restore()` がプレースホルダとして Reno 風のウィンドウ半減（`cwnd = max(cwnd >> 1U, 2U)`）を行うようになっており、パケット保存則も `prior_cwnd` の復元も未実装です。そのため行 42 の `assert tcpi_snd_cwnd == 1`（および Recovery 脱出後の `assert tcpi_snd_cwnd >= 20`）で失敗します。
- [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/solution.bpf.c) では、Recovery 第 1 ラウンドのパケット保存則（`cwnd == 1`）と脱出時の `prior_cwnd` 復元（`cwnd >= 20`）が正しく動作するため、すべてのアサートをパスします。

### 実行コマンド

```sh
../../run_test.sh bbr/step06-loss-ack-aggregation starter
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```
