# `write_seq` や `copied_seq` はなぜ `0` から始まらないのか？

（関連章: [第 1 章: 主要データ構造](../01-data-structures.md)）

TCP のシーケンス番号は、`0` バイト目から始まるオフセットではなく、3 ウェイハンドシェイク（SYN / SYN-ACK）のときに暗号学的なハッシュ（SipHash）でランダムに選ばれた **ISN（Initial Sequence Number：初期シーケンス番号）** から始まります。

## 送信側の境界（`snd_una` / `snd_nxt` / `write_seq`）

```text
送信ストリーム (バイト列のシーケンス番号空間)
  ... | 送信済み・ACK 済み | 送信済み・未 ACK (In-Flight) | 未送信 (sendmsg 済み) |
                           ^                              ^                       ^
                        snd_una                        snd_nxt                write_seq
```

- **`snd_una`**: まだ相手から ACK されていない最古のバイトのシーケンス番号です。その 1 つ手前（`snd_una - 1`）までは相手に届いたことが確認できています。
- **`snd_nxt`**: 次にワイヤへ初めて送り出すバイトのシーケンス番号です。`snd_nxt - snd_una` が「いまワイヤ上（または再送キュー）にある送信済み・未 ACK バイト数」になります。
- **`write_seq`**: アプリケーションが `write()` / `sendmsg()` でカーネルの送信キューに書き込んだ直後の「次の空き位置」です。`write_seq - snd_nxt` が「まだ 1 度もワイヤに出ていない未送信バイト数（`notsent_bytes`）」になります。

## 受信側の境界（`copied_seq` / `rcv_nxt`）

```text
受信ストリーム (バイト列のシーケンス番号空間)
  ... | アプリが recvmsg 済み | 受信済み・アプリ未読 (Receive Queue) | まだ届いていない |
                              ^                                      ^
                          copied_seq                              rcv_nxt
```

- **`copied_seq`**: アプリがまだ `read()` / `recvmsg()` で読み出していない先頭バイトのシーケンス番号です（その 1 つ手前 `copied_seq - 1` までがユーザ空間へコピー済み）。
- **`rcv_nxt`**: カーネルが「次に相手から届くのを待っている」バイトのシーケンス番号です（その 1 つ手前 `rcv_nxt - 1` までが欠損なくカーネルの受信キューに届いています）。
- したがって **`rcv_nxt - copied_seq`** が「カーネルには届いているが、アプリがまだ読んでいない未読バイト数（`ioctl(SIOCINQ)` の値）」になります。
