# Step 1: Minimal BPF Congestion Control Skeleton + Slow Start & Reno AI

## Goal

In this step, you will implement a Linux kernel `struct tcp_congestion_ops` in BPF using `struct_ops`, building **TCP Reno (Slow Start and Additive Increase)** from scratch.

Rather than delegating to kernel `extern __ksym` helpers such as `tcp_slow_start()` or `tcp_cong_avoid_ai()`, you will write every line in pure BPF so you can see and verify how `tp->snd_cwnd`, `tp->snd_ssthresh`, and `tp->snd_cwnd_cnt` evolve on every incoming ACK.

---

## Design Checkpoints (Think before coding)

Before opening `starter.bpf.c` or `solution.bpf.c`, pause and work through these four design questions on your own.

### Q1. State Design: Does Reno need a custom private state struct (`icsk_ca_priv`)?

Algorithms like CUBIC and BBR store per-socket state in `inet_csk_ca(sk)`. Are the fields already present in `struct tcp_sock` sufficient to implement Reno Slow Start and Additive Increase (`+1 cwnd` per RTT)?

<details>
<summary>Hint & Answer</summary>

- **Hint**: Check `struct tcp_sock` for the congestion window, slow start threshold, and a counter that accumulates sub-window ACK credits.
- **Answer**: No custom `icsk_ca_priv` struct is needed. `struct tcp_sock` already provides everything Reno requires:
  - `tp->snd_cwnd`: current congestion window (in segments)
  - `tp->snd_ssthresh`: slow start threshold where Slow Start transitions into Congestion Avoidance
  - `tp->snd_cwnd_cnt`: linear counter accumulating ACKed segments during Congestion Avoidance
  - `tp->prior_cwnd`: saved `snd_cwnd` before loss recovery (used when undoing a spurious cwnd reduction)

</details>

### Q2. Hook Selection & Guard Condition: Should `snd_cwnd` grow when the application is send-limited?

The `.cong_avoid(sk, ack, acked)` hook runs on every ACK that acknowledges new data. What goes wrong if we grow `tp->snd_cwnd` unconditionally — for example, when `snd_cwnd = 10` but the application only sends 1 segment per RTT?

<details>
<summary>Hint & Answer</summary>

- **Hint**: If `snd_cwnd` grows without the sender ever filling the pipe to test network capacity, a sudden application burst can inject an arbitrarily large flight of packets into an untested path (see RFC 7661 Cwnd Validation).
- **Answer**: You must only grow `snd_cwnd` when transmission is genuinely limited by `snd_cwnd`. At the very start of `.cong_avoid()`, check `if (!tcp_is_cwnd_limited(sk)) return;` so that application-limited flights do not inflate `snd_cwnd`.

</details>

### Q3. Boundary Condition: What happens when a stretch ACK crosses `snd_ssthresh` during Slow Start?

Suppose `tp->snd_cwnd = 20` and `tp->snd_ssthresh = 22`. If a single stretch ACK arrives acknowledging `acked = 20` segments at once, what should the resulting `snd_cwnd` and `snd_cwnd_cnt` be?

<details>
<summary>Hint & Answer</summary>

- **Hint**: Slow Start increases `snd_cwnd` by 1 per ACKed segment up to `snd_ssthresh` (22). What should happen to the remaining ACKed segments in the same ACK?
- **Answer**:
  - First, Slow Start consumes `22 - 20 = 2` segments from `acked`, raising `tp->snd_cwnd` to `22` (`tp->snd_ssthresh`).
  - The remaining `acked = 20 - 2 = 18` segments must not be discarded; they fall through to Congestion Avoidance and are added to `tp->snd_cwnd_cnt` (`tp->snd_cwnd_cnt += 18`).
  - Conversely, if all `acked` segments are consumed in Slow Start (`!acked`), we return early before Congestion Avoidance.

</details>

### Q4. Integer-Only Additive Increase: How do we express `cwnd += acked / cwnd` without floating-point maths in BPF?

BPF programs cannot use floating-point instructions. In Congestion Avoidance, each ACKed segment should increase `snd_cwnd` by `1 / snd_cwnd` (so that one full `snd_cwnd` of ACKs increases `snd_cwnd` by `+1`). How do we write this using only integers?

<details>
<summary>Hint & Answer</summary>

- **Hint**: Keep the numerator of the fraction `tp->snd_cwnd_cnt / tp->snd_cwnd` in `tp->snd_cwnd_cnt`.
- **Answer**:
  - On each ACK in Congestion Avoidance, add `acked` to the numerator: `tp->snd_cwnd_cnt += acked`.
  - Once `tp->snd_cwnd_cnt >= tp->snd_cwnd` (a full window of segments has been acknowledged), subtract `tp->snd_cwnd` from `tp->snd_cwnd_cnt` and increment `tp->snd_cwnd` by `1` (clamped at `tp->snd_cwnd_clamp`).

</details>

---

## Step-by-step Walkthrough

Let's walk through each function in [`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno/solution.bpf.c).

### 1. `bpf_reno1_init()` — Initialising the Socket

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->snd_cwnd_cnt = 0;
	tp->snd_ssthresh = 22;
}
```

- Called when the TCP connection is established (or when `setsockopt(TCP_CONGESTION)` switches the socket to `bpf_reno1`).
- Resets the linear congestion avoidance counter `tp->snd_cwnd_cnt` to `0`.
- Sets `tp->snd_ssthresh = 22` so that `test.pkt` can exercise pure Slow Start (`10 -> 20`), the exact `snd_ssthresh` boundary transition with leftover `acked` credits (`20 -> 22`), and Reno Additive Increase (`22 -> 23`) in three clean rounds.

### 2. `bpf_reno1_ssthresh()` — Multiplicative Decrease Threshold

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}
```

- Called when entering loss recovery to compute the new slow start threshold.
- Halves `tp->snd_cwnd` (`tp->snd_cwnd >> 1U`), floored at `2U` as required by RFC 5681.

### 3. `bpf_reno1_cong_avoid()` — Slow Start and Reno Additive Increase

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		__u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

		acked -= cwnd - tp->snd_cwnd;
		tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
		if (!acked)
			return;
	}

	tp->snd_cwnd_cnt += acked;
	if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
		tp->snd_cwnd_cnt -= tp->snd_cwnd;
		tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
	}
}
```

- **Cwnd-limited check**: Returns immediately if `!tcp_is_cwnd_limited(sk)`.
- **Slow Start phase**: While `tcp_in_slow_start(tp)` (`tp->snd_cwnd < tp->snd_ssthresh`), computes `cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh)`, subtracts the consumed growth `cwnd - tp->snd_cwnd` from `acked`, updates `tp->snd_cwnd`, and returns early if `acked == 0`.
- **Congestion Avoidance (Reno AI) phase**: Adds any remaining `acked` segments to `tp->snd_cwnd_cnt`. When `tp->snd_cwnd_cnt >= tp->snd_cwnd`, subtracts `tp->snd_cwnd` and increments `tp->snd_cwnd` by `1` (clamped at `tp->snd_cwnd_clamp`).
- **BPF Verifier note**: Because this implementation uses bounded arithmetic without loops, the BPF verifier proves termination and safety trivially.

### 4. `bpf_reno1_undo_cwnd()` — Undoing a Spurious Window Reduction

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_undo_cwnd, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}
```

- Restores `snd_cwnd` to `max(tp->snd_cwnd, tp->prior_cwnd)` when the kernel detects that a loss recovery episode was spurious (explored in detail in Step 2).

---

## Understanding `test.pkt`

[`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno/test.pkt) drives `bpf_reno1` through three deterministic rounds:

1. **Handshake with 1000-byte Effective MSS**:
   - The incoming SYN advertises `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>`.
   - With 12-byte TCP Timestamps (`nop,nop,TS`), `tp->mss_cache` becomes `1012 - 12 = 1000` bytes, making segment arithmetic exact (`10,000` bytes = `10` segments).
   - Immediately after `accept()`, `test.pkt` asserts `tcpi_snd_cwnd == 10` and `tcpi_snd_ssthresh == 22`.
2. **Round 1 — Pure Slow Start (`cwnd: 10 -> 20`)**:
   - `write(4, ..., 10000)` sends 10 segments (`1:10001`), filling `cwnd = 10` so `tp->is_cwnd_limited` becomes `true`.
   - Receiving `ack 10001` (`acked = 10`) doubles `tcpi_snd_cwnd` from `10` to `20`.
3. **Round 2 — Clamping at `snd_ssthresh` & Carrying Leftover Credits (`cwnd: 20 -> 22`)**:
   - `write(4, ..., 20000)` sends 20 segments (`10001:30001`), filling `cwnd = 20`.
   - Receiving `ack 30001` (`acked = 20`) grows `cwnd` by `2` in Slow Start to reach `snd_ssthresh = 22`, and carries the remaining `18` ACKed segments into `tp->snd_cwnd_cnt = 18`.
4. **Round 3 — Reno Additive Increase (`cwnd: 22 -> 23`)**:
   - `write(4, ..., 22000)` sends 22 segments (`30001:52001`), filling `cwnd = 22`.
   - Receiving `ack 52001` (`acked = 22`) raises `tp->snd_cwnd_cnt` to `18 + 22 = 40 >= 22`, incrementing `tcpi_snd_cwnd` from `22` to `23`.

---

## How to run

Run the following commands from `/usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-reno` (or from `labs/`):

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh cubic/step01-reno starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh cubic/step01-reno solution
```
