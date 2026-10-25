# ハンズオン全体の概要と進め方 (`reno`, `cubic`, `bbr`)

Linux カーネルの BPF `struct_ops`（`struct tcp_congestion_ops`）と `packetdrill` を使って、**Reno**・**CUBIC**・**BBRv1** の 3 つの輻輳制御アルゴリズムをステップバイステップでゼロから実装・検証するハンズオン教材（`labs/` ディレクトリ）です。

事前知識がなくても読めるよう、各トラックの冒頭（[第 15 章: Reno](15-lab-reno.md)、[第 16 章: CUBIC](16-lab-cubic.md)、[第 17 章: BBR](17-lab-bbr.md)）と各ステップの最初に **「なぜそのアルゴリズムが生まれたのか」「どういう仕組み・数式で動くのか」「`struct tcp_sock` に最初から何が用意されているのか」** の解説を置き、その仕組みを理解したうえで **設計チェックポイント（VibeWise 方式の設計問答）** $\to$ **BPF 実装** $\to$ **`packetdrill` テスト** に進む構成になっています。

---

## 3 つのトラックとディレクトリ構成

```text
labs/
├── Makefile                    # 全ステップの .bpf.o ビルド & 一括テスト (make check)
├── run_test.sh                 # 単一ステップ / 全ステップの vng + bpftool + packetdrill ランナー
├── include/
│   ├── vmlinux.h               # BTF から生成したカーネル型定義
│   └── bpf_cc_helpers.h        # BPF CC 用の共通インラインヘルパー
├── reno/                       # トラック 1: すべての輻輳制御の原点 TCP Reno (全 2 ステップ)
│   ├── README.md               # Reno のアルゴリズム解説（AIMD・Slow Start・CA・tcp_sock の構造）
│   ├── step01-slow-start-ai/   # Step 1: Slow Start と Additive Increase
│   └── step02-loss-undo/       # Step 2: Loss (Multiplicative Decrease) と DSACK Undo
├── cubic/                      # トラック 2: 高帯域ネットワーク向け TCP CUBIC (全 4 ステップ)
│   ├── README.md               # なぜ Reno ではダメなのか & CUBIC の 3 次関数アルゴリズム解説
│   ├── step01-cubic-curve/     # Step 1: β=0.7 と固定小数点の立方根・W_cubic(t) 曲線
│   ├── step02-reno-friendly-fast-conv/ # Step 2: Reno-friendly 領域 (W_est) と Fast Convergence
│   ├── step03-idle-epoch-shift/# Step 3: アイドル後の Epoch シフト (CA_EVENT_TX_START)
│   └── step04-hystart/         # Step 4: HyStart (ACK Train & Delay 増加による Slow Start 脱出)
└── bbr/                        # トラック 3: 帯域と最小 RTT のモデル駆動 TCP BBRv1 (全 6 ステップ)
    ├── README.md               # なぜロスベース CC ではダメなのか & BBR の水道管モデル解説
    ├── step01-pacing-rate/     # Step 1: .cong_control フックと Delivery Rate からの Pacing Rate
    ├── step02-btlbw-rtprop/    # Step 2: BtlBw (最大帯域) と RTprop (最小 RTT) による BDP 計算
    ├── step03-startup-drain/   # Step 3: STARTUP から DRAIN への遷移（満杯パイプ検出）
    ├── step04-probe-bw/        # Step 4: PROBE_BW の 8 フェーズ Gain サイクル
    ├── step05-probe-rtt/       # Step 5: PROBE_RTT への遷移と cwnd = 4
    └── step06-loss-ack-aggregation/ # Step 6: Loss 時の cwnd 保存・復元と extra_acked 補償
```

| 章 / トラック | 対象アルゴリズム | ステップ数 | 学べること |
| :--- | :--- | :--- | :--- |
| **[15. TCP Reno](15-lab-reno.md)** | RFC 5681 Reno | 全 2 ステップ | フロー制御（`snd_wnd`）と輻輳制御（`snd_cwnd`）の違い、AIMD の公平性、Slow Start / Congestion Avoidance の整数演算（`snd_cwnd_cnt`）、`icsk_ca_priv` と DSACK Undo |
| **[16. TCP CUBIC](16-lab-cubic.md)** | RFC 9438 CUBIC | 全 4 ステップ | 高 BDP 回線での Reno の限界、3 次関数 $W_{\text{cubic}}(t)$ と固定小数点立方根 `cubic_root()`、Reno-friendly 領域（$W_{\text{est}}$）と Fast Convergence、アイドル後の Epoch シフト、HyStart（ACK Train & Delay Increase） |
| **[17. TCP BBR](17-lab-bbr.md)** | BBRv1 | 全 6 ステップ | Bufferbloat と浅いバッファでのロスベース CC の限界、BtlBw × RTprop の水道管モデル、`.cong_control` フックと `sk_pacing_rate`、STARTUP / DRAIN / PROBE_BW（8 フェーズ） / PROBE_RTT の状態機械、Loss Undo と `extra_acked` 補償 |

---

## 使い方

### 1. トラック解説を読んでからステップごとに取り組む

1. まず各トラックの章（[15. TCP Reno](15-lab-reno.md)、[16. TCP CUBIC](16-lab-cubic.md)、[17. TCP BBR](17-lab-bbr.md)）でアルゴリズムの全体像を掴みます。
2. 続いて各ステップの **「アルゴリズムと背景の解説」** を読み、**「設計チェックポイント」** の問いを自分の頭で考えてから `starter.bpf.c` を開きます。
3. 未実装の `starter` が `packetdrill` のアサートで失敗することを確認します：
   ```sh
   cd labs
   ./run_test.sh reno/step01-slow-start-ai starter
   ```
4. `starter.bpf.c` の `TODO` を埋めて再実行するか、模範解答 `solution` を走らせて PASS することを確認します：
   ```sh
   ./run_test.sh reno/step01-slow-start-ai solution
   ```

### 2. 全 12 ステップの模範解答を一括ビルド＆テストする

```sh
cd labs
make check
```
