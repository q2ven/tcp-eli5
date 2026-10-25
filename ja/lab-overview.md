# ハンズオン全体の概要と進め方 (`reno`, `cubic`, `bbr`, `qdisc-fifo`, `qdisc-fq`, `qdisc-fq-codel`)

Linux カーネルの BPF `struct_ops`（`struct tcp_congestion_ops` および `struct Qdisc_ops`）と `packetdrill` を使って、**TCP 輻輳制御（Reno・CUBIC・BBRv1）** と **パケットスケジューラ / Qdisc（FIFO・FQ + EDT Pacing・CoDel / FQ-CoDel）** の計 6 トラック・18 ステップをステップバイステップでゼロから実装・検証するハンズオン教材（`labs/` ディレクトリ）です。

事前知識がなくても読めるよう、各トラックの冒頭（[第 18 章: Reno](18-lab-reno.md)〜[第 23 章: FQ-CoDel](23-lab-qdisc-fq-codel.md)）と各ステップの最初に **「なぜそのアルゴリズムが生まれたのか」「どういう仕組み・数式で動くのか」「カーネルが最初から提供している構造体・kfunc は何か」** の解説を置き、その仕組みを理解したうえで **設計チェックポイント（VibeWise 方式の設計問答）** $$\to$$ **BPF 実装** $$\to$$ **`packetdrill` テスト** に進む構成になっています。

---

## 6 つのトラック（全 18 ステップ）とディレクトリ構成

```text
labs/
├── Makefile                    # 全 18 ステップの .bpf.o ビルド & 一括テスト (make check)
├── run_test.sh                 # 単一ステップ / 全ステップの vng + bpftool + packetdrill ランナー
├── include/
│   ├── vmlinux.h               # BTF から生成したカーネル型定義
│   ├── bpf_cc_helpers.h        # BPF CC 用の共通インラインヘルパー
│   └── bpf_qdisc_helpers.h     # BPF Qdisc 用の kfunc 宣言と共通インラインヘルパー
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
├── bbr/                        # トラック 3: 帯域と最小 RTT のモデル駆動 TCP BBRv1 (全 6 ステップ)
│   ├── README.md               # なぜロスベース CC ではダメなのか & BBR の水道管モデル解説
│   ├── step01-pacing-rate/     # Step 1: .cong_control フックと Delivery Rate からの Pacing Rate
│   ├── step02-btlbw-rtprop/    # Step 2: BtlBw (最大帯域) と RTprop (最小 RTT) による BDP 計算
│   ├── step03-startup-drain/   # Step 3: STARTUP から DRAIN への遷移（満杯パイプ検出）
│   ├── step04-probe-bw/        # Step 4: PROBE_BW の 8 フェーズ Gain サイクル
│   ├── step05-probe-rtt/       # Step 5: PROBE_RTT への遷移と cwnd = 4
│   └── step06-loss-ack-aggregation/ # Step 6: Loss 時の cwnd 保存・復元と extra_acked 補償
├── qdisc-fifo/                 # トラック 4: BPF Qdisc 入門 — パケット数 FIFO とバイト数 FIFO + 優先度バンド (全 2 ステップ)
│   ├── README.md               # Qdisc_ops の基本構造・kfunc・リンクレートエミュレーション解説
│   ├── step01-pfifo/           # Step 1: パケット数上限 FIFO (pfifo) と Tail Drop
│   └── step02-bfifo-prio/      # Step 2: バイト数上限 FIFO (bfifo) と 2 バンド厳密優先度キュー
├── qdisc-fq/                   # トラック 5: Fair Queueing (FQ) と EDT Pacing (全 2 ステップ)
│   ├── README.md               # Head-of-Line ブロッキング・DRR・EDT (Earliest Departure Time) 解説
│   ├── step01-flow-drr/        # Step 1: フロー分類・new_flows 優先 (Sparse Flow)・DRR スケジューリング
│   └── step02-edt-pacing/      # Step 2: skb->tstamp (EDT) と bpf_qdisc_watchdog_schedule による Pacing
└── qdisc-fq-codel/             # トラック 6: CoDel AQM と FQ-CoDel (全 2 ステップ)
    ├── README.md               # Bufferbloat・CoDel の滞在時間 (sojourn time) と逆平方根則・FQ-CoDel 解説
    ├── step01-codel-aqm/       # Step 1: CoDel 滞在時間計測と 1/sqrt(count) 間隔の Head Drop
    └── step02-fq-codel/        # Step 2: フロー別 DRR キュー × フロー別 CoDel AQM の統合 (FQ-CoDel)
```

| 章 / トラック | 対象アルゴリズム | ステップ数 | 学べること |
| :--- | :--- | :--- | :--- |
| **[18. TCP Reno](18-lab-reno.md)** | RFC 5681 Reno | 全 2 ステップ | フロー制御（`snd_wnd`）と輻輳制御（`snd_cwnd`）の違い、AIMD の公平性、Slow Start / Congestion Avoidance の整数演算（`snd_cwnd_cnt`）、`icsk_ca_priv` と DSACK Undo |
| **[19. TCP CUBIC](19-lab-cubic.md)** | RFC 9438 CUBIC | 全 4 ステップ | 高 BDP 回線での Reno の限界、3 次関数 $$W_{\text{cubic}}(t)$$ と固定小数点立方根 `cubic_root()`、Reno-friendly 領域（$$W_{\text{est}}$$）と Fast Convergence、アイドル後の Epoch シフト、HyStart（ACK Train & Delay Increase） |
| **[20. TCP BBR](20-lab-bbr.md)** | BBRv1 | 全 6 ステップ | Bufferbloat と浅いバッファでのロスベース CC の限界、BtlBw × RTprop の水道管モデル、`.cong_control` フックと `sk_pacing_rate`、STARTUP / DRAIN / PROBE_BW（8 フェーズ） / PROBE_RTT の状態機械、Loss Undo と `extra_acked` 補償 |
| **[21. BPF Qdisc: FIFO / 優先度](21-lab-qdisc-fifo.md)** | `pfifo` / `bfifo` / Priority | 全 2 ステップ | `struct Qdisc_ops`（`.enqueue` / `.dequeue` / `.init` / `.reset` / `.destroy`）、BPF Arena リンクリスト `bpf_sk_buff_ptr`、`bpf_qdisc_skb_drop()` / `bpf_qdisc_bstats_update()`、`sch->qstats.backlog` と優先度バンド |
| **[22. BPF Qdisc: FQ と EDT Pacing](22-lab-qdisc-fq.md)** | `sch_fq` (DRR + EDT) | 全 2 ステップ | `bpf_skb_get_hash()` によるフロー分類、`new_flows` / `old_flows` の DRR（Deficit Round Robin）と Sparse Flow 優先、`skb->tstamp`（EDT）と `bpf_qdisc_watchdog_schedule()` による 타이マー駆動 Pacing |
| **[23. BPF Qdisc: CoDel と FQ-CoDel](23-lab-qdisc-fq-codel.md)** | RFC 8289 CoDel / RFC 8290 FQ-CoDel | 全 2 ステップ | Bufferbloat と CoDel のキュー滞在時間（sojourn time）計測、`interval / sqrt(count)` の逆平方根ドロップスケジュールと Head Drop、フロー別 DRR × フロー別 CoDel の統合 |

---

## 使い方

### 1. トラック解説を読んでからステップごとに取り組む

1. まず各トラックの章（[18. TCP Reno](18-lab-reno.md)〜[23. BPF Qdisc: CoDel と FQ-CoDel](23-lab-qdisc-fq-codel.md)）でアルゴリズムの全体像を掴みます。
2. 続いて各ステップの **「アルゴリズムと背景の解説」** を読み、**「設計チェックポイント」** の問いを自分の頭で考えてから `starter.bpf.c` を開きます。
3. 未実装の `starter` が `packetdrill` のアサートで失敗することを確認します：
   ```sh
   cd labs
   ./run_test.sh reno/step01-slow-start-ai starter
   ./run_test.sh qdisc-fifo/step01-pfifo starter
   ```
4. `starter.bpf.c` の `TODO` を埋めて再実行するか、模範解答 `solution` を走らせて PASS することを確認します：
   ```sh
   ./run_test.sh reno/step01-slow-start-ai solution
   ./run_test.sh qdisc-fifo/step01-pfifo solution
   ```

### 2. 全 18 ステップの模範解答を一括ビルド＆テストする

```sh
cd labs
make check
```
