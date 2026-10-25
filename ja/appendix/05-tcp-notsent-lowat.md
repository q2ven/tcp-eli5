# `TCP_NOTSENT_LOWAT` (`notsent_lowat`) と `tcp_stream_memory_free()`

（関連章: [第 3 章: 送信 (1): tcp_sendmsg と送信キュー](../03-sendmsg.md)）

## なぜ `sk_sndbuf` とは別に `TCP_NOTSENT_LOWAT` が必要なのか？

TCP の送信バッファ（`sk_wmem_queued`、上限は `sk_sndbuf`）には、**`snd_nxt` を境に 2 種類のデータ** が入っています。

```text
|<------------------------ sk_wmem_queued (上限: sk_sndbuf) ----------------------->|
+----------------------------------------+------------------------------------------+
| (A) 送信済み・未 ACK のデータ          | (B) まだ 1 度もワイヤに出ていない        |
|     (In-Flight: 再送のために保持が必須) |     未送信データ (notsent_bytes)         |
+----------------------------------------+------------------------------------------+
^                                        ^                                          ^
snd_una                                  snd_nxt                                    write_seq
```

- **(A) 送信済み・未 ACK（`snd_nxt - snd_una`）**:
  - すでにワイヤへ送り出したが、相手からまだ ACK が返っていないデータです。万一途中でロスしたら再送しなければならないため、ACK が返るまでカーネルはメモリを解放できません。
  - 高帯域・長距離（高 BDP）の回線で速度を出すには (A) を数 MB 飛ばす必要があるため、Linux の送信バッファ自動調整（`tcp_moderate_sndbuf()`）は `sk_sndbuf` を **数 MB（たとえば 4 MB）** まで広げます。
- **(B) 未送信データ（`write_seq - snd_nxt` = `notsent_bytes`）**:
  - アプリが `sendmsg()` で書き込んだけれど、cwnd やペーシングの制限で **まだ 1 度もワイヤに出ていない順番待ちのデータ** です。

もし `sk_sndbuf`（例: 4 MB）だけで制限していると、そのうち (A) が 500 KB しかなくても、非ブロッキングなアプリ（`epoll` で書けるだけ書くループ）は残りの **3.5 MB 分も未送信データ (B) をカーネルに先回りして詰め込めてしまいます**。
一度 `sendmsg()` でカーネルに渡したデータは後からキャンセルも順番入れ替えもできないため、HTTP/2 や動画配信サーバが「直後に来た高優先度のレスポンスを先に送りたい」「画面スクロールで不要になった画像の送信をやめたい」と思っても、すでに積んだ 3.5 MB の後ろに並ばされてしまいます（アプリケーション層の Bufferbloat / Head-of-Line Blocking）。

そこで **`TCP_NOTSENT_LOWAT`**（既定値は `UINT_MAX` で無制限）をたとえば **`16 KB`** に設定すると：
1. **(A) の送信済み・未 ACK データ** には `sk_sndbuf` の数 MB をフルに使わせたまま（＝高 BDP 回線でのスループットを一切落とさず）、
2. **(B) の未送信データ（`write_seq - snd_nxt`）だけを 16 KB 以下に抑える** ことができます。
3. パケットがワイヤへ出て `snd_nxt` が進むたびにアプリが起こされるため、**「本当にワイヤに出る直前（Just-In-Time）」** にいま最優先のデータを `sendmsg()` できるようになります（コミット `c9bee3b7fdec`）。

## なぜ上限なのに名前が `LOWAT` で、`<< wake` で半分にするのか？

```c
bool tcp_stream_memory_free(const struct sock *sk, int wake)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 notsent_bytes = READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_nxt);

	return (notsent_bytes << wake) < tcp_notsent_lowat(tp);
}
```

- **名前の由来**: `poll()` / `epoll` の視点から見て、「未送信データ (B) が減っていって、この水位を **下回ったら（Low Watermark を切ったら）** 書き込み可能（`EPOLLOUT`）を通知する」という意味で `LOWAT` と名付けられています。
- **なぜ `<< wake` で閾値を変えるのか（ヒステリシス）**:
  - **`sendmsg()` で書き込むとき（`wake = 0`）**: `notsent_bytes < lowat`（例: 16 KB 未満なら書ける）。
  - **送信が進んで `EPOLLOUT` でアプリを起こすか判定するとき（`wake = 1`）**: `(notsent_bytes << 1) < lowat`、つまり **`notsent_bytes < lowat / 2`（半分＝8 KB 未満に減るまで `EPOLLOUT` を出さない）**。
  - もし 16 KB 溜まっている状態から 1 パケット（1.4 KB）送信されて 14.6 KB になった瞬間にすぐ `EPOLLOUT` を出してしまうと、アプリが起きて 1.4 KB だけ書いてまた `-EAGAIN` で寝る……という細かい wakeup が毎パケット発生してしまいます。半分（8 KB）空くまで待ってから起こすことで、1 回の起床につき 8 KB まとめて書けるようチャタリングを防いでいます（コミット `8ba3c9d1c6d7`、`a74f0fa082b7`）。
