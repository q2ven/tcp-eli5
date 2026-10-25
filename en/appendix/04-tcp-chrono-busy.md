# What Do `enum tcp_chrono` and `TCP_CHRONO_BUSY` Measure?

(Related chapter: [Chapter 3: Sending (1): tcp_sendmsg and the write queue](../03-sendmsg.md))

When a TCP connection achieves lower throughput than expected in production, there are four possible culprits:
1. The application itself is idle (not writing data).
2. The network path is limiting transmission via `snd_cwnd` or pacing.
3. The peer's receive window (`snd_wnd` / `rwnd`) is throttling the sender.
4. The local send buffer (`SO_SNDBUF` / `sk_sndbuf`) is full, blocking `sendmsg()` even though `cwnd` and `rwnd` have room.

`tcp_chrono_start()` and `tcp_chrono_stop()` (`enum tcp_chrono`) act as a microsecond stopwatch attributing active send time across these states:

```text
Priority order (higher-numbered state overrides lower-numbered state):
  TCP_CHRONO_UNSPEC (0)        : Idle (both write queue and rtx queue are empty)
    ↓
  TCP_CHRONO_BUSY (1)          : At least 1 byte is queued or in flight (total active send time)
    ↓
  TCP_CHRONO_RWND_LIMITED (2)  : Throttled by peer receive window (snd_wnd)
    ↓
  TCP_CHRONO_SNDBUF_LIMITED (3): Throttled because local send buffer (sk_sndbuf) is full
```

- The moment `tcp_sendmsg_locked()` queues the very first `sk_buff` onto a completely empty socket (`tcp_skb_entail()`), it calls `tcp_chrono_start(sk, TCP_CHRONO_BUSY)` to start the active-transmission stopwatch.
- Once all queued and in-flight data is acknowledged and the queues become empty again, `tcp_chrono_stop(sk, TCP_CHRONO_BUSY)` stops the clock.
- The accumulated durations in `tp->chrono_stat[]` are exposed via `ss -ti` (`busy:120ms rwnd_limited:30ms(25.0%) sndbuf_limited:60ms(50.0%)`) and `struct tcp_info` (`tcpi_busy_time`, `tcpi_rwnd_limited`, `tcpi_sndbuf_limited`).
