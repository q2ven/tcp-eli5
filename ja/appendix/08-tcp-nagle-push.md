# `tp->nonagle` の 3 つのビットと `TCP_NAGLE_PUSH` の役割

**関連箇所**: [第 3 章: `tcp_skb_entail()`](../03-sendmsg.md)、[第 4 章: `tcp_nagle_test()` / `tcp_nagle_check()`](../04-write-xmit.md)

## `tp->nonagle` に入っている 3 つのビットフラグ

`struct tcp_sock` の `tp->nonagle` には、次の 3 つのビットフラグが定義されています（[include/net/tcp.h#L261-L263](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/net/tcp.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n261)）。

```c
/* Flags in tp->nonagle */
#define TCP_NAGLE_OFF		1	/* Nagle's algo is disabled */
#define TCP_NAGLE_CORK		2	/* Socket is corked	    */
#define TCP_NAGLE_PUSH		4	/* Cork is overridden for already queued data */
```

- **`TCP_NAGLE_OFF` (1)**: `setsockopt(TCP_NODELAY, 1)` で立ちます。Nagle アルゴリズムを無効化し、1 MSS 未満の小さなデータでも ACK を待たずにすぐ送ります。
- **`TCP_NAGLE_CORK` (2)**: `setsockopt(TCP_CORK, 1)` で立ちます。ソケットに「コルク栓」をして、1 MSS に満たない端数セグメントの送信を止めます。
- **優先順位の基本ルール**: **`TCP_CORK` は `TCP_NODELAY` より強い**（[net/ipv4/tcp_output.c#L2234](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2234) の `tcp_nagle_check()`）。つまり両方が立っているとき、通常の書き込みデータはコルク栓（`TCP_NAGLE_CORK`）が優先されて送信が止められます。

## では `TCP_NAGLE_PUSH` (4) は何のためにあるのか？

Linux の仕様では、**「`TCP_CORK` で栓をしている最中でも、アプリが明示的に `setsockopt(TCP_NODELAY, 1)` を呼んだら、その時点で『すでに送信キューに溜まっているデータ』だけはコルク栓を無視して今すぐ強制フラッシュ（push）する」** というルールがあります（[net/ipv4/tcp.c#L3666-L3680](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n3666)）。

```c
/* TCP_NODELAY is weaker than TCP_CORK, so that this option on corked socket is
 * remembered, but it is not activated until cork is cleared.
 *
 * However, when TCP_NODELAY is set we make an explicit push, which overrides
 * even TCP_CORK for currently queued segments.
 */
void __tcp_sock_set_nodelay(struct sock *sk, bool on)
{
	if (on) {
		tcp_sk(sk)->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
		tcp_push_pending_frames(sk);
	} else {
		tcp_sk(sk)->nonagle &= ~TCP_NAGLE_OFF;
	}
}
```

ここで `TCP_NAGLE_PUSH` が立てられると、送信判定の `tcp_nagle_test()`（[net/ipv4/tcp_output.c#L2375](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2375)）は、`TCP_NAGLE_CORK` や Nagle のチェックを行う前に **無条件で `return true`（送信許可）** を返します。

- **なぜ関数のローカル引数ではなく `tp->nonagle` にビットとして保存するのか？**
  - `setsockopt(TCP_NODELAY, 1)` を呼んだ瞬間（`tcp_push_pending_frames(sk)`）に、ちょうど `cwnd`（輻輳ウィンドウ）や `rwnd`（受信ウィンドウ）がいっぱいで、すぐにはワイヤに送り出せないことがあるからです。
  - `tp->nonagle` に `TCP_NAGLE_PUSH` を記憶しておけば、数ミリ秒後に ACK が届いてウィンドウが開いたときにも、「これはあのとき強制 push を指示された既存データだ」と分かって送り出せます。

## なぜ `tcp_skb_entail()` で新しいデータが来たら消すのか？（コミット `89ebd197eb2c` のバグ）

`TCP_NAGLE_PUSH` はあくまで **「`setsockopt()` を呼んだ時点で『すでにキューにあったデータ』だけを押し出す」** ためのフラグであり、**その後に新しく `write()` / `sendmsg()` されたデータには効かせてはいけません**。

特に上の `__tcp_sock_set_nodelay()` の `else` 節を見ると分かる通り、アプリが `setsockopt(TCP_NODELAY, 0)` で `TCP_NODELAY` をオフに戻したとき、カーネルは `TCP_NAGLE_OFF` しか消さず、**`TCP_NAGLE_PUSH` は消していません**。

そのため、たとえば送信キューが空の状態でアプリが次の操作をしたとします。

1. `setsockopt(TCP_NODELAY, 1)` を呼ぶ → `tp->nonagle` は `TCP_NAGLE_OFF | TCP_NAGLE_PUSH` になる。
2. `setsockopt(TCP_NODELAY, 0)` を呼んで Nagle を有効に戻す（あるいはさらに `setsockopt(TCP_CORK, 1)` でコルク栓をする） → `TCP_NAGLE_OFF` は消えるが、**`TCP_NAGLE_PUSH` が残ったまま** になる。
3. その後、小さな `write()` を呼ぶ。

コミット `89ebd197eb2c` より前の古いカーネルでは、`tcp_skb_entail()` のコードが次のようになっていました。

```c
/* 89ebd197eb2c より前のバグがあったコード */
if (!sk->sk_send_head)
	sk->sk_send_head = skb;
else if (tp->nonagle & TCP_NAGLE_PUSH)
	tp->nonagle &= ~TCP_NAGLE_PUSH;
```

`else if` になっていたせいで、**送信キューが空（`!sk->sk_send_head`）のときに新しい `sk_buff` を追加すると、`if` 側に入ってしまい `TCP_NAGLE_PUSH` が消えない** というバグがありました。その結果、手順 1→2→3 を踏むと `TCP_NAGLE_PUSH` が永久に残り続け、Nagle も `TCP_CORK` もまったく効かなくなってしまっていました。

そこでコミット `89ebd197eb2c` で `else` が外され、**「送信キューに新しい `sk_buff` をつなぐ（`tcp_skb_entail()`）ときは、必ず `TCP_NAGLE_PUSH` をクリアする」** ようになりました。
