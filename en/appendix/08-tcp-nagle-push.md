# The 3 Bits of `tp->nonagle` and the Role of `TCP_NAGLE_PUSH`

**Related sections**: [Chapter 3: `tcp_skb_entail()`](../03-sendmsg.md), [Chapter 4: `tcp_nagle_test()` / `tcp_nagle_check()`](../04-write-xmit.md)

## The 3 Bit Flags in `tp->nonagle`

`tp->nonagle` in `struct tcp_sock` combines three bit flags ([include/net/tcp.h#L261-L263](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/net/tcp.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n261)):

```c
/* Flags in tp->nonagle */
#define TCP_NAGLE_OFF		1	/* Nagle's algo is disabled */
#define TCP_NAGLE_CORK		2	/* Socket is corked	    */
#define TCP_NAGLE_PUSH		4	/* Cork is overridden for already queued data */
```

- **`TCP_NAGLE_OFF` (1)**: Set by `setsockopt(TCP_NODELAY, 1)`. Disables Nagle's algorithm so sub-MSS segments are sent immediately without waiting for ACKs.
- **`TCP_NAGLE_CORK` (2)**: Set by `setsockopt(TCP_CORK, 1)`. Plugs a "cork" into the socket, holding back any partial (< 1 MSS) segment.
- **Basic precedence rule**: **`TCP_CORK` is stronger than `TCP_NODELAY`** (`tcp_nagle_check()` in [net/ipv4/tcp_output.c#L2234](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2234)). When both `TCP_NAGLE_CORK` and `TCP_NAGLE_OFF` are set, normal writes are still held back by the cork.

## Why Does `TCP_NAGLE_PUSH` (4) Exist?

In Linux TCP, **even when a socket is corked (`TCP_CORK`), calling `setsockopt(TCP_NODELAY, 1)` performs an explicit one-time flush of whatever data is *already* sitting in the write queue at that moment, overriding `TCP_CORK`** ([net/ipv4/tcp.c#L3666-L3680](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n3666)):

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

When `TCP_NAGLE_PUSH` is set, `tcp_nagle_test()` ([net/ipv4/tcp_output.c#L2375](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n2375)) immediately returns `true` (allow send) **before** checking `TCP_NAGLE_CORK` or Nagle's rule.

- **Why store `TCP_NAGLE_PUSH` in `tp->nonagle` instead of passing a local variable?**
  - Because at the moment `setsockopt(TCP_NODELAY, 1)` calls `tcp_push_pending_frames(sk)`, `cwnd` or `rwnd` might be full, preventing immediate transmission.
  - Recording `TCP_NAGLE_PUSH` in `tp->nonagle` ensures that when an ACK arrives a few milliseconds later and opens the window, the already-queued segment is still pushed out.

## Why Must `tcp_skb_entail()` Clear `TCP_NAGLE_PUSH` on New Data? (Commit `89ebd197eb2c`)

`TCP_NAGLE_PUSH` is strictly a one-shot instruction for **data that was already in the queue when `setsockopt()` was called**—it must **not** apply to new data written afterwards.

Notice in `__tcp_sock_set_nodelay()` above that when an application calls `setsockopt(TCP_NODELAY, 0)` (`on == false`), the kernel clears `TCP_NAGLE_OFF` (`tp->nonagle &= ~TCP_NAGLE_OFF`) **but does NOT clear `TCP_NAGLE_PUSH`**.

Consider what happens if an application runs the following sequence starting with an empty send queue:

1. Call `setsockopt(TCP_NODELAY, 1)` -> `tp->nonagle` becomes `TCP_NAGLE_OFF | TCP_NAGLE_PUSH`.
2. Call `setsockopt(TCP_NODELAY, 0)` (re-enabling Nagle) and/or `setsockopt(TCP_CORK, 1)` -> `TCP_NAGLE_OFF` is cleared, **but `TCP_NAGLE_PUSH` remains set in `tp->nonagle`**.
3. Call `write()` with a small payload.

Prior to commit `89ebd197eb2c`, `tcp_skb_entail()` had:

```c
/* Buggy code prior to 89ebd197eb2c */
if (!sk->sk_send_head)
	sk->sk_send_head = skb;
else if (tp->nonagle & TCP_NAGLE_PUSH)
	tp->nonagle &= ~TCP_NAGLE_PUSH;
```

Because of `else if`, adding the first `sk_buff` to an empty queue (`!sk->sk_send_head`) took the `if` branch and **failed to clear `TCP_NAGLE_PUSH`**, leaving `TCP_NAGLE_PUSH` stuck on and bypassing both Nagle and `TCP_CORK`! Commit `89ebd197eb2c` removed the `else` so `tcp_skb_entail()` unconditionally clears `TCP_NAGLE_PUSH` whenever any new `sk_buff` is queued.
