# Step 1: .cong_control フックと Delivery Rate からの Pacing Rate 計算

## 学習ゴール

このステップでは、BPF `struct_ops`（`struct tcp_congestion_ops`）を用いて BBR（Bottleneck Bandwidth and RTT）の基礎となる **Pacing Rate（送信レート）制御** を実装します。

- Reno や CUBIC が用いる `.cong_avoid` フックと、BBR が用いる `.cong_control` フックの決定的な違いを理解します。
- `.init` フックでソケットの内部ペーシング（`SK_PACING_NEEDED`）を有効化し、初期 `cwnd` と初期 RTT から初期ペーシングレートを設定します。
- `.cong_control` フックに渡される `struct rate_sample` から瞬間配送レート（Delivery Rate）を計算し、固定小数点のゲイン（`BBR_HIGH_GAIN`）を掛けて `sk->sk_pacing_rate` を更新します。

---

## 設計チェックポイント（VibeWise 方式 — コードを見る前に考えよう）

いきなりコードを書く前に、以下の 4 つの問いを自分の頭で考えてみましょう。

### Q1. どのフックを使うべきでしょうか？

Reno や CUBIC では `.cong_avoid(sk, ack, acked)` と `.ssthresh(sk)` を中心に実装しました。しかし BBR では `.cong_avoid` を使いません。`struct tcp_congestion_ops` のどのコールバックを使うべきでしょうか？また、そのコールバックを定義するとカーネルの `tcp_cong_control()` の挙動はどう変わるでしょうか？

<details>
<summary>ヒントと答え</summary>

- **使うべきフック**: `.cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)` です。
- **カーネルの挙動の変化**: `tcp_cong_control()` は `icsk_ca_ops->cong_control` が定義されている場合、それを呼び出して即座に `return` します。つまり、カーネル標準の `tcp_cwnd_reduction()`・`tcp_cong_avoid()`・`tcp_update_pacing_rate()` がすべてバイパスされ、輻輳ウィンドウ（`snd_cwnd`）とペーシングレート（`sk_pacing_rate`）の制御が完全に `.cong_control` に委ねられます。
- なお、`tcp_validate_congestion_control()` の登録チェックを通すため、`.ssthresh` と `.undo_cwnd` も必ず定義する必要があります。

</details>

### Q2. TCP の内部ペーシングを有効化するには何を書き換えますか？

`sk->sk_pacing_rate` にバイト/秒のレートを書き込むだけでは、FQ（Fair Queueing）qdisc を使っていない環境ではパケットがペーシングされません。TCP スタック自身の高分解能タイマー（hrtimer）による内部ペーシングを有効化するには、`.init(sk)` で `struct sock` のどのフィールドに何を設定する必要があるでしょうか？

<details>
<summary>ヒントと答え</summary>

- `sk->sk_pacing_status = SK_PACING_NEEDED;` を設定します。
- BPF の `tcp_congestion_ops` ベリファイア（`bpf_tcp_ca_btf_struct_access()`）は、`struct sock` の `sk_pacing_status` と `sk_pacing_rate` への直接書き込みを許可しています。

</details>

### Q3. `struct rate_sample` からどのようにバイト/秒の配送レートを計算しますか？

`.cong_control` の第 4 引数 `const struct rate_sample *rs` には、直近の観測区間における配送パケット数 `rs->delivered` と経過時間（マイクロ秒）`rs->interval_us` が格納されています。MSS（1 セグメントあたりのペイロードバイト数 `tp->mss_cache`）を使って、配送レート（bytes/sec）を求める式を整数演算で組み立ててみましょう。

<details>
<summary>ヒントと答え</summary>

- 1 秒は `USEC_PER_SEC`（`1,000,000` マイクロ秒）なので、瞬間配送レート（bytes/sec）は次の式で求まります。
  ```c
  rate = (__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC;
  rate = div64_u64(rate, (__u64)rs->interval_us);
  ```
- 先に分子を 64 ビット整数（`__u64`）で掛け算してから `rs->interval_us` で割ることで、整数除算による桁落ち（truncation）を防ぎます。
- STARTUP フェーズのペーシングゲイン（`2 / ln(2) ~= 2.885`）を固定小数点（`BBR_UNIT = 1024`, `BBR_HIGH_GAIN = 2885`）で掛けるには、`(rate * BBR_HIGH_GAIN) >> BBR_SCALE` を計算します。

</details>

### Q4. どのようなエッジケース（無効なサンプルやゼロ除算）に注意すべきでしょうか？

すべての ACK で有効な `rate_sample` が得られるとは限りません。また、`.init(sk)` が呼ばれた時点でまだ RTT サンプル（`tp->srtt_us`）が存在しない場合もあります。どの値をチェックしてガードを入れるべきでしょうか？

<details>
<summary>ヒントと答え</summary>

- `.cong_control` では、`rs->delivered < 0 || rs->interval_us <= 0` のサンプルは無効な観測値であるため、何もせず `return` します（これにより `rs->interval_us == 0` によるゼロ除算も防げます）。
- `.init` では、`tp->srtt_us`（1/8 マイクロ秒単位の平滑化 RTT）が `0` の場合、デフォルトとして 1 ミリ秒（`USEC_PER_MSEC = 1000` us）を使います。さらに `tp->srtt_us >> 3` が `0` になる極端なケースにも備えて `if (!rtt_us) rtt_us = 1;` でゼロ除算を防止します。

</details>

---

## 実装ステップと解説

### 1. 固定小数点定数の定義

BPF プログラム内では浮動小数点演算が使えないため、BBR のゲイン計算には 2 のべき乗を基準とした固定小数点演算を用います。

- `BBR_SCALE = 10`（`BBR_UNIT = 1 << 10 = 1024`）を `1.0x` の基準とします。
- STARTUP フェーズのゲイン $2 / \ln(2) \approx 2.885$ は、`BBR_HIGH_GAIN = 2885`（$2885 / 1024 \approx 2.817$）として表現します。

### 2. `bpf_bbr1_init()` — ペーシングの有効化と初期レート計算

コネクション確立時に呼ばれる `bpf_bbr1_init()` では、最初の ACK から `rate_sample` が届くまでの間にバースト送信が起きないよう、初期 `snd_cwnd` と初期 RTT から初期ペーシングレートを設定します。

- `tp->srtt_us` は 3-way ハンドシェイクの SYN-ACK RTT から計測されており、実際のマイクロ秒値を得るには 3 ビット右シフト（`tp->srtt_us >> 3`）します。未計測（`0`）の場合は 1 ms（`USEC_PER_MSEC`）を仮定します。
- 初期帯域幅 `tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC / rtt_us` に `BBR_HIGH_GAIN` を掛けて `sk->sk_pacing_rate` に書き込み、`sk->sk_pacing_status = SK_PACING_NEEDED` で内部ペーシングを有効化します。

### 3. `bpf_bbr1_cong_control()` — Delivery Rate からの Pacing Rate 更新

ACK ごとに呼ばれる `bpf_bbr1_cong_control()` では、カーネルの `tcp_rate_gen()` が計測した `struct rate_sample *rs` を読み取ります。

1. `rs->delivered < 0 || rs->interval_us <= 0` の場合は無効サンプルとして無視します。
2. 配送バイト数 `(__u64)rs->delivered * tp->mss_cache` に `USEC_PER_SEC` を掛け、`div64_u64()` で `rs->interval_us` で割ることで、瞬間配送レート（bytes/sec）を求めます。
3. `(rate * BBR_HIGH_GAIN) >> BBR_SCALE` により STARTUP ゲインを適用し、`sk->sk_pacing_rate` を更新します。

### 4. `bpf_bbr1_ssthresh()` と `bpf_bbr1_undo_cwnd()`

カーネルの `tcp_validate_congestion_control()` は、`.ssthresh` と `.undo_cwnd` が未定義の `tcp_congestion_ops` を `-EINVAL` で拒否します。このステップではまだロス回復時の `cwnd` 保存を扱わないため、それぞれ現在の `tp->snd_ssthresh` と `tp->snd_cwnd` をそのまま返します。

---

## packetdrill テストの解説

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step01-pacing-rate/test.pkt) は以下のシナリオで `bpf_bbr1` の動作を検証します。

1. **3-way ハンドシェイク（RTT = 100 ms, MSS = 1000 B）**:
   - クライアントが `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>` で SYN を送り、12 バイトの TCP タイムスタンプオプションを差し引いたペイロード MSS（`tp->mss_cache`）は `1000` バイトになります。
   - SYN-ACK に対する ACK を `+0.10`（100 ms 後）に返すことで、`tp->srtt_us >> 3` は `100,000` us（100 ms）になります。
   - `bpf_bbr1_init()` により、初期ペーシングレートは `10 * 1000 B / 0.1 s * (2885 / 1024) = 281,738 B/s` に設定されます（`200000 < tcpi_pacing_rate < 400000` をアサート）。
2. **10 セグメント（10,000 バイト）の送信と 10 ms 後の ACK**:
   - サーバーが `write(4, ..., 10000)` を実行して 10 セグメントを送信し、`+0.01`（10 ms 後）にクライアントが `ack 10001` を返します。
   - この ACK により `rs->delivered = 10`、`rs->interval_us = 10000`（10 ms）の `rate_sample` が生成され、配送レートは `10,000 B / 0.01 s = 1,000,000 B/s`（1 MB/s）となります。
   - `bpf_bbr1_cong_control()` がゲイン `2885 / 1024` を適用し、`tcpi_pacing_rate` が約 `2,817,382 B/s`（`> 500000` かつ `2000000 < tcpi_pacing_rate < 3500000`）に更新されたことをアサートします。

---

## 実行方法

```sh
# 1. starter.bpf.c がアサートで失敗することを確認
../../run_test.sh bbr/step01-pacing-rate starter
# 2. starter.bpf.c の TODO を実装して再実行（または模範解答 solution を実行）
../../run_test.sh bbr/step01-pacing-rate solution
```
