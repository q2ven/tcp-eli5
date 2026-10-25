# Step 6: Loss 時の cwnd 保存・復元と ACK 集約 (extra_acked) 補償

このステップでは、BBRv1 のパケットロス時の挙動（Packet Conservation と `prior_cwnd` の復元）および、Wi-Fi や LRO/GRO 環境で生じる ACK 集約（ACK Aggregation）への適応アルゴリズムを実装します。

---

## 学習ゴール

- **BBR と Reno/CUBIC のロス応答の違いの理解**: なぜ BBR はパケットロスが発生しても `cwnd` を半減させず、送信レート（ペーシングレート）を変更しないのかを理解します。
- **Recovery 第 1 ラウンドのパケット保存則（Packet Conservation）**: ロス検出直後に一時的に「1 個 ACK されたら 1 個送る」保存則（Packet Conservation）を適用し、安全にパイプをドレインする方法を学びます。
- **Fast Recovery 終了後の `prior_cwnd` 復元**: ロス復旧が完了した際、事前退避した `prior_cwnd` へ安全にウィンドウを復元する仕組みを習得します。
- **ACK 集約（`extra_acked`）の推定と補正**: バースト ACK やストレッチ ACK によって生じる一時的な cwnd 制約によるスループット低下を防ぐため、超過 ACK 数を計測して `target_cwnd` に上乗せする機構を理解します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

### Q1. なぜ BBR は Reno や CUBIC のようにロス検出で `cwnd` やペーシングレートを永続的に半減させないのでしょうか？
- ヒント: BBR のネットワークモデル（BtlBw と RTprop）において、パケットロスはどのように扱われているでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: Reno や CUBIC はパケットロスを「バッファ溢れ＝輻輳」の直接的な証拠とみなしてウィンドウを 50%〜30% 削減します。しかし現代のネットワーク（特に浅いバッファのスイッチや無線リンク）では、輻輳とは無関係なランダムロスや一時的なバッファ競合が日常的に発生します。
- BBR はボトルネック容量を物理量（`BtlBw` と `RTprop`）として直接計測・モデル化しています。パケットロスが発生したからといって回線の帯域（`BtlBw`）や伝搬遅延（`RTprop`）自体が縮むわけではありません。そのため、BBR はパケットロスを理由に永続的に帯域推定（`max_bw`）やペーシングレートを引き下げることはせず、ロスしたパケット数を一時的に差し引くにとどめます。
</details>

---

### Q2. Fast Recovery 突入時の第 1 ラウンドで「パケット保存則（Packet Conservation）」が必要な理由と、終了後に `prior_cwnd` を復元する仕組みはどうなっていますか？
- ヒント: パケットロスが起きた瞬間、ネットワーク内の実フライト数はどうなっていますか？ また、復旧完了後に `cwnd` が切り詰められたままだと何が起こるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: ロスが検知された瞬間、パイプ内には実際に欠損したパケットが存在しており、過剰な再送やバースト送信を行うとさらなるロスを招きます。そこで BBR は Recovery 突入直後の 1 ラウンドトリップ（RTT）の間、`cwnd = tcp_packets_in_flight(tp) + max(acked, 1)` に設定し、「ACK が届いてネットワークからパケットが抜けた分だけ新しく送信を許可する」パケット保存則（Packet Conservation）を適用します。
- そして Recovery から通常の `TCP_CA_Open` に復帰した際は、Recovery 前に退避していた `ca->prior_cwnd` を使って `cwnd = max(cwnd, ca->prior_cwnd)` と復元します。これにより、不要なスループット低下を防ぎ、即座に本来の BDP 目標ウィンドウへ戻ることができます。
</details>

---

### Q3. なぜ `.set_state` フックでは `TCP_CA_Recovery` ではなく `TCP_CA_Loss`（RTO 発生時）のみを拾って状態を更新するのでしょうか？
- ヒント: Fast Recovery（SACK/dupACK）と RTO（タイムアウト）の重大度の違いは何でしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: 3 重重複 ACK などによる Fast Recovery（`TCP_CA_Recovery`）は、ACK が継続的に返ってきており、パイプが完全に止まっているわけではありません。そのため、通常の `.cong_control` フック内の `rate_sample` から状態遷移とパケット保存則をシームレスに処理できます。
- 一方、RTO タイムアウト（`TCP_CA_Loss`）はネットワークが完全に停止したかパイプが完全に空になったことを意味します。このときは ACK が届かないため `.cong_control` は呼ばれず、カーネルのタイマーパスから `.set_state` が直接呼び出されます。BBR は `.set_state` で `new_state == TCP_CA_Loss` を検知したときのみ、`ca->full_bw = 0` や `ca->round_start = 1` をリセットしてモデルを安全に再初期化します。
</details>

---

### Q4. Wi-Fi や GRO（Generic Receive Offload）における ACK 集約（ACK Aggregation）とは何であり、BBR はどのように `extra_acked` を計算して補正しているでしょうか？
- ヒント: 本来 1 パケットごとに分散して届くはずの ACK が、受信側や中継機器でまとめられて一度にドッと届くと、送信側のレートやウィンドウ計算にどのような影響が出るでしょうか？

<details>
<summary>ヒントと答え</summary>

- **答え**: 無線 LAN のフレームアグリゲーションや NIC の LRO/GRO により、複数の TCP ACK が 1 つにまとめられてバースト的に受信される現象を ACK 集約と呼びます。ACK が集約されると、ACK が来ない間は送信が一時的に止まり、集約 ACK 到着時に大量のデータが一気に ACK されます。
- `cwnd` が純粋な BDP（`BtlBw * RTprop`）ぴったりに設定されていると、集約によって ACK が間欠的になった際に送信側が cwnd 制限にかかり、パイプが枯渇してスループットが低下します。
- そこで BBR はサンプリングエポック内の経過時間 `epoch_us` から期待される ACK 数 `expected_acked = max_bw * epoch_us` を計算し、実際に届いた ACK 数との差分 `extra_acked = ack_epoch_acked - expected_acked` を計測します。この `extra_acked`（直近 5 RTT の最大値、上限は BDP）を `target_cwnd` に上乗せすることで、ACK 集約環境でも途切れなくペーシング送信を継続できます。
</details>

---

## 実装ステップと解説

### 1. `struct bbr_ca` の拡張
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

### 2. `bbr_save_cwnd()` による事前 cwnd 退避
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

### 3. `bbr_set_cwnd_to_recover_or_restore()` によるロス制御
- パケットロス発生時は、ロス分のみを `cwnd` から減算します（下限 1）。
- `TCP_CA_Recovery` 突入時は `packet_conservation = 1` とし、`cwnd = tcp_packets_in_flight(tp) + max(acked, 1)` を強制します。
- `TCP_CA_Recovery` から抜けた際は、`ca->prior_cwnd` まで `cwnd` を復元します。

### 4. `bbr_update_ack_aggregation()` による集約推定
- 1 ラウンドごとに `extra_acked_win_rtts` をインクリメントし、5 RTT が経過したら `extra_acked` をリセットして古い推定値を減衰させます。
- エポック内の実際の ACK 数が期待 ACK 数を超えた場合、その差分を `extra_acked` の最大値として記録します。
- `bbr_set_cwnd()` 内で `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));` を加算します。

---

## packetdrill テストの解説 (`test.pkt`)

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

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step06-loss-ack-aggregation starter

# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```
