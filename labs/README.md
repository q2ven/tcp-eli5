# BPF TCP Congestion Control Hands-on Labs (`cubic` & `bbr`)

Linux カーネルの BPF `struct_ops`（`struct tcp_congestion_ops`）と `packetdrill` を使って、**CUBIC** と **BBRv1** をステップバイステップでゼロから実装・検証するハンズオン教材です。

カーネルの再ビルドや再起動は不要です。各ステップの BPF プログラムを `clang -target bpf` でコンパイルし、`vng`（virtme-ng）内の軽量 VM で `bpftool struct_ops register` して `packetdrill` スクリプトを走らせることで、1 ステップずつ手で動かしながら学べます。

---

## ディレクトリ構成

```text
labs/
├── Makefile                    # 全ステップの .bpf.o ビルド & 一括テスト (make check)
├── run_test.sh                 # 単一ステップ / 全ステップの vng + bpftool + packetdrill ランナー
├── include/
│   ├── vmlinux.h               # BTF から生成したカーネル型定義
│   └── bpf_cc_helpers.h        # BPF CC 用の共通インラインヘルパー
├── cubic/                      # トラック A: Reno から始める BPF CUBIC (全 6 ステップ)
│   ├── step01-reno/
│   ├── step02-loss-undo/
│   ├── step03-cubic-curve/
│   ├── step04-reno-friendly-fast-conv/
│   ├── step05-idle-epoch-shift/
│   └── step06-hystart/
└── bbr/                        # トラック B: ゼロから作る BPF BBRv1 (全 6 ステップ)
    ├── step01-pacing-rate/
    ├── step02-btlbw-rtprop/
    ├── step03-startup-drain/
    ├── step04-probe-bw/
    ├── step05-probe-rtt/
    └── step06-loss-ack-aggregation/
```

各ステップのディレクトリには以下の 5 ファイルが入っています。

- `README.md` / `README.en.md`: 学習ゴール、**設計チェックポイント（VibeWise 方式の設計問答）**、コードと数式の解説、`test.pkt` の解説
- `starter.bpf.c`: 自分で手を動かすための `TODO` 穴埋めスケルトン（コンパイル・ロードは通りますが、`test.pkt` のアサートで FAIL します）
- `solution.bpf.c`: 模範解答の BPF 実装（`test.pkt` が PASS します）
- `test.pkt`: そのステップの挙動を検証する `packetdrill` スクリプト

---

## 使い方

### 1. ステップ単位で取り組む（おすすめ）

1. 各ステップの `README.md` を読み、**設計チェックポイント** の問いに自分の頭で答えてから `starter.bpf.c` を開きます。
2. まず未実装の `starter` が `packetdrill` のアサートで失敗することを確認します：
   ```sh
   ./run_test.sh cubic/step01-reno starter
   ```
3. `starter.bpf.c` の `TODO` を埋めて再実行し、PASS するかを確かめます。
4. 模範解答（`solution.bpf.c`）の動作を確認するには：
   ```sh
   ./run_test.sh cubic/step01-reno solution
   ```

### 2. 全 12 ステップの模範解答を一括ビルド＆テストする

```sh
make check
```
