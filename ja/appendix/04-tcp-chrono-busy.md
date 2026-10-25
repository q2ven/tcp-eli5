# `enum tcp_chrono` と `TCP_CHRONO_BUSY` は何を測っているのか？

（関連章: [第 3 章: 送信 (1): tcp_sendmsg と送信キュー](../03-sendmsg.md)）

本番環境で「TCP のスループットが出ない」とき、原因は大きく 4 つあります。
1. アプリがそもそもデータを書いていない（アイドル状態）
2. ネットワークの輻輳ウィンドウ（`snd_cwnd`）やペーシングで制限されている
3. 受信側のウィンドウ（`snd_wnd` / `rwnd`）が小さくて止められている
4. 送信側のソケットバッファ（`SO_SNDBUF` / `sk_sndbuf`）が小さすぎて、`cwnd` には余裕があるのに `sendmsg()` が詰まっている

この「どれがボトルネックだったのか」をマイクロ秒単位のストップウォッチで記録するのが **`tcp_chrono_start()` / `tcp_chrono_stop()`**（`enum tcp_chrono`）です。

```text
状態優先度 (番号が大きい方が優先してカウントされる):
  TCP_CHRONO_UNSPEC (0)        : アイドル (送信キューも RTX キューも空)
    ↓
  TCP_CHRONO_BUSY (1)          : 送信したいデータが 1 バイト以上ある状態 (全体稼働時間)
    ↓
  TCP_CHRONO_RWND_LIMITED (2)  : 受信側のウィンドウ (snd_wnd) が原因で送れない状態
    ↓
  TCP_CHRONO_SNDBUF_LIMITED (3): 送信バッファ (sk_sndbuf) が満杯でそれ以上積めない状態
```

- 送信キューと再送キューが完全に空だった状態から、`tcp_sendmsg_locked()` が最初の 1 パケットをキューに繋いだ瞬間（`tcp_skb_entail()`）、カーネルは `tcp_chrono_start(sk, TCP_CHRONO_BUSY)` を呼んで「送信ビジー時間」のストップウォッチを開始します。
- その後、すべてのデータが ACK されて送信キューも再送キューも空になると、`tcp_chrono_stop(sk, TCP_CHRONO_BUSY)` でストップウォッチが止まります。
- 計測された累積時間は `tp->chrono_stat[]` に加算され、`ss -ti` の出力（`busy:120ms rwnd_limited:30ms(25.0%) sndbuf_limited:60ms(50.0%)`）や `struct tcp_info`（`tcpi_busy_time`, `tcpi_rwnd_limited`, `tcpi_sndbuf_limited`）からいつでも確認できます。
