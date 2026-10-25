# `TCP_NOTSENT_LOWAT` (`notsent_lowat`) and `tcp_stream_memory_free()`

(Related chapter: [Chapter 3: Sending (1): tcp_sendmsg and the write queue](../03-sendmsg.md))

## Why Do We Need `TCP_NOTSENT_LOWAT` in Addition to `sk_sndbuf`?

A TCP socket's send buffer (`sk_wmem_queued`, capped by `sk_sndbuf`) holds **two distinct categories of data separated by `snd_nxt`**:

```text
|<------------------------ sk_wmem_queued (limit: sk_sndbuf) ---------------------->|
+----------------------------------------+------------------------------------------+
| (A) Sent but unacknowledged data       | (B) Unsent data never put on the wire    |
|     (In-Flight: must keep for retrans) |     yet (notsent_bytes)                  |
+----------------------------------------+------------------------------------------+
^                                        ^                                          ^
snd_una                                  snd_nxt                                    write_seq
```

- **(A) Sent but unacknowledged (`snd_nxt - snd_una`)**:
  - Data that has already been transmitted onto the wire but not yet acknowledged by the peer. The kernel must retain these `sk_buff`s in case retransmission is needed.
  - On high-BDP paths, (A) must grow to several megabytes to keep the pipe full, so Linux's send-buffer autotuning (`tcp_moderate_sndbuf()`) automatically expands `sk_sndbuf` to multiple megabytes (e.g. 4 MB).
- **(B) Unsent data (`write_seq - snd_nxt` = `notsent_bytes`)**:
  - Data that the application has passed to `sendmsg()`, waiting in the kernel queue for cwnd or pacing to allow it onto the wire for the first time.

If only `sk_sndbuf` (say, 4 MB) caps the queue and (A) is currently 500 KB, a non-blocking `epoll` loop will pour **3.5 MB of unsent data (B)** into the kernel.
Once bytes are handed to `sendmsg()`, the application can no longer cancel or reorder them. If an HTTP/2 or video server queues 3.5 MB of a low-priority image and immediately receives a high-priority API request or stream cancellation, that urgent response is stuck behind 3.5 MB of already-queued bytes (application-layer Head-of-Line blocking / bufferbloat).

Setting **`TCP_NOTSENT_LOWAT`** (default `UINT_MAX`, unlimited) to e.g. **`16 KB`** solves this:
1. **(A) In-flight data** can still use the full multi-megabyte `sk_sndbuf` (preserving 100% of high-BDP throughput), while
2. **(B) Unsent data (`write_seq - snd_nxt`) is capped at 16 KB**.
3. As packets leave on the wire and `snd_nxt` advances, the kernel wakes the application **just-in-time** to supply the highest-priority stream's next 16 KB (commit `c9bee3b7fdec`).

## Why Is an Upper Limit Called `LOWAT`, and Why Does `<< wake` Halve It?

```c
bool tcp_stream_memory_free(const struct sock *sk, int wake)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 notsent_bytes = READ_ONCE(tp->write_seq) - READ_ONCE(tp->snd_nxt);

	return (notsent_bytes << wake) < tcp_notsent_lowat(tp);
}
```

- **Why `LOWAT` (Low Watermark)**: From `poll()` / `epoll`'s perspective, it is the low watermark below which unsent bytes (B) must drop before the socket becomes writable (`EPOLLOUT`).
- **Why `<< wake` halves the threshold (Hysteresis)**:
  - **During `sendmsg()` (`wake = 0`)**: `notsent_bytes < lowat` (the application can write until unsent bytes reach e.g. 16 KB).
  - **When checking whether to emit `EPOLLOUT` (`wake = 1`)**: `(notsent_bytes << 1) < lowat`, i.e. **`notsent_bytes < lowat / 2` (do not wake the application until unsent bytes drop below half the limit, e.g. 8 KB)**.
  - Without this hysteresis, transmitting a single 1.4 KB packet from a 16 KB queue would immediately trigger `EPOLLOUT` at 14.6 KB, waking the application on every single packet just to write 1.4 KB and hit `-EAGAIN` again (commits `8ba3c9d1c6d7`, `a74f0fa082b7`).
