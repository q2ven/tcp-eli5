# はじめに

この本は、Linux カーネルの TCP スタックを **upstream net-next**
（コミット [`279d5c8afcd3`](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/commit/?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e)）
のコードに沿って、一歩ずつたどる解説書です。

## 誰のための本か

ソケットの lookup や bind、netns といった「slow path」側には慣れているけれど、
データ転送の fast path と輻輳制御はまだ深く読んだことがない、という人を想定しています。
ゴールは、TCP メンテナーと同じ目線でこれらのコードを読み、レビューし、変更できるようになることです。

## 各章の構成

1. **たとえ話で言うと** – 郵便と宅配の世界で直感をつかむ
2. **RFC では** – 関連 RFC の概要から入り、コードが実装している規則や式の詳細へ
3. **全体の流れ** – 図で俯瞰
4. **コードを追う** – 主要関数を 1 行ずつ
5. **RFC と実装の違い**
6. **設計の背景と歴史** – その形を決めた upstream コミット
7. **性能の勘所** – キャッシュライン、ロック、lockless な読み出し、固定小数点
8. **観測とデバッグ** – `nstat`、`ss -ti`、tracepoint、packetdrill
9. **IPv4 と IPv6**
10. **理解度チェック**
11. **まとめ**

## 読む順番

第 1〜2 章は準備運動です。第 3〜10 章がデータ転送の fast path（送信 → 受信 →
ACK → ロス回復 → タイマー → ウィンドウとメモリ）、第 11〜14 章が輻輳制御、
第 15〜17 章がパケットスケジューリング（Qdisc: FIFO / MQ、FQ / EDT Pacing、
CoDel / FQ-CoDel / RED / NetEm）です。さらに第 18〜23 章では、BPF `struct_ops`
と `packetdrill` を使って Reno・CUBIC・BBR および FIFO・FQ・FQ-CoDel Qdisc を
自分の手で一から実装・検証するハンズオンを用意しています。
fast path の章は順番に読むのがおすすめです。輻輳制御や Qdisc の章は、第 7〜8 章を読んだ後なら
どこからでも読めます。

## ソースコードについて

コード片はすべて上記コミットからそのまま引用しています。リンクは git.kernel.org の
同じコミットを指しています。手元で読むなら次のようにします。

```sh
git worktree add --detach ../tcp-eli5-src 279d5c8afcd38bd841832a39bfe1aa4f8518d98e
```
