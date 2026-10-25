# Step 1: The `.cong_control` Hook and Pacing Rate Calculation from Delivery Rate

## Goal

In this step, you will build the foundation of BBR (Bottleneck Bandwidth and RTT) using BPF `struct_ops` (`struct tcp_congestion_ops`) by implementing **pacing rate control** driven directly by measured delivery rates.

- Understand the fundamental architectural difference between `.cong_avoid` (used by Reno and CUBIC) and `.cong_control` (used by BBR).
- Enable TCP internal pacing (`SK_PACING_NEEDED`) in `.init` and initialise the pacing rate from the initial `cwnd` and initial RTT.
- Compute the instantaneous delivery rate from `struct rate_sample` inside `.cong_control`, apply a fixed-point pacing gain (`BBR_HIGH_GAIN`), and update `sk->sk_pacing_rate`.

---

## Design Checkpoints (Think before coding)

Before looking at `solution.bpf.c`, think through the following four design questions.

### Q1. Which hook should we implement?

In Reno and CUBIC, congestion control revolves around `.cong_avoid(sk, ack, acked)` and `.ssthresh(sk)`. BBR does not implement `.cong_avoid`. Which callback in `struct tcp_congestion_ops` should we use instead, and how does defining it change the behaviour of the kernel's `tcp_cong_control()` function?

<details>
<summary>Hint and Answer</summary>

- **Hook to use**: `.cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)`.
- **Kernel behaviour**: In `tcp_cong_control()`, if `icsk_ca_ops->cong_control` is non-NULL, the kernel invokes it and returns immediately. Standard kernel helpers (`tcp_cwnd_reduction()`, `tcp_cong_avoid()`, and `tcp_update_pacing_rate()`) are completely bypassed, giving `.cong_control` full authority over both `snd_cwnd` and `sk_pacing_rate`.
- Note that `.ssthresh` and `.undo_cwnd` are still mandatory callbacks required by `tcp_validate_congestion_control()` at registration time.

</details>

### Q2. How do we enable TCP internal pacing on a socket?

Simply writing a byte-per-second value to `sk->sk_pacing_rate` has no effect unless either the `sch_fq` qdisc is attached or TCP's built-in high-resolution timer (`hrtimer`) pacing is enabled. Which field of `struct sock` must be set in `.init(sk)` to activate internal pacing?

<details>
<summary>Hint and Answer</summary>

- Set `sk->sk_pacing_status = SK_PACING_NEEDED;`.
- The BPF `tcp_congestion_ops` verifier (`bpf_tcp_ca_btf_struct_access()`) explicitly allows writing to both `sk->sk_pacing_status` and `sk->sk_pacing_rate`.

</details>

### Q3. How do we convert `struct rate_sample` into a byte-per-second delivery rate?

The fourth parameter of `.cong_control`, `const struct rate_sample *rs`, provides `rs->delivered` (packets delivered over the sampling interval) and `rs->interval_us` (duration of the interval in microseconds). Given the segment payload size `tp->mss_cache`, how should we write the integer formula for the delivery rate in bytes/sec?

<details>
<summary>Hint and Answer</summary>

- Because there are `USEC_PER_SEC` (`1,000,000`) microseconds in a second, the instantaneous delivery rate in bytes/sec is:
  ```c
  rate = (__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC;
  rate = div64_u64(rate, (__u64)rs->interval_us);
  ```
- Multiplying the numerator in 64-bit (`__u64`) before dividing by `rs->interval_us` avoids integer truncation.
- To apply the STARTUP pacing gain ($2 / \ln(2) \approx 2.885$) in fixed-point arithmetic (`BBR_UNIT = 1024`, `BBR_HIGH_GAIN = 2885`), compute `(rate * BBR_HIGH_GAIN) >> BBR_SCALE`.

</details>

### Q4. What edge cases and invalid inputs must we guard against?

Not every ACK produces a valid rate sample, and `.init(sk)` may run when `tp->srtt_us` is still zero. What checks are needed to prevent bogus updates or division by zero?

<details>
<summary>Hint and Answer</summary>

- In `.cong_control`, check `if (rs->delivered < 0 || rs->interval_us <= 0) return;`. This filters out invalid rate samples and guarantees `rs->interval_us` is strictly positive before dividing.
- In `.init`, `tp->srtt_us` stores the smoothed RTT scaled by 8 (`srtt_us >> 3` in microseconds). If `tp->srtt_us == 0`, fall back to a 1 ms nominal RTT (`USEC_PER_MSEC = 1000` us), and guard with `if (!rtt_us) rtt_us = 1;` before dividing.

</details>

---

## Step-by-step Walkthrough

### 1. Fixed-Point Constants

Because floating-point instructions are rejected by the BPF verifier, BBR represents gains as fixed-point fractions scaled by a power of two:

- `BBR_SCALE = 10` (`BBR_UNIT = 1 << 10 = 1024`) represents a `1.0x` gain.
- `BBR_HIGH_GAIN = 2885` represents the STARTUP gain of $2 / \ln(2) \approx 2.885\text{x}$ ($2885 / 1024 \approx 2.817\text{x}$), which doubles the sending rate every RTT.

### 2. `bpf_bbr1_init()` — Initialising Internal Pacing and Initial Rate

When the connection is established, `bpf_bbr1_init()` sets an initial pacing rate so the socket paces traffic before the first data ACK arrives:

- `tp->srtt_us` is measured from the 3-way handshake SYN-ACK exchange and shifted right by 3 (`tp->srtt_us >> 3`) to obtain microseconds. If no RTT sample exists yet, `USEC_PER_MSEC` (1 ms) is used.
- The initial rate is calculated as `(tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC / rtt_us) * BBR_HIGH_GAIN >> BBR_SCALE` and written to `sk->sk_pacing_rate`.
- Writing `sk->sk_pacing_status = SK_PACING_NEEDED` instructs the TCP stack to pace outgoing packets.

### 3. `bpf_bbr1_cong_control()` — Updating Pacing Rate from `struct rate_sample`

On every ACK, the kernel's `tcp_rate_gen()` populates `struct rate_sample *rs` and passes it to `bpf_bbr1_cong_control()`:

1. Invalid samples (`rs->delivered < 0 || rs->interval_us <= 0`) are ignored.
2. The instantaneous delivery rate in bytes/sec is computed via `div64_u64((__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC, (__u64)rs->interval_us)`.
3. The rate is scaled by `BBR_HIGH_GAIN` (`(rate * BBR_HIGH_GAIN) >> BBR_SCALE`) and written to `sk->sk_pacing_rate`.

### 4. `bpf_bbr1_ssthresh()` and `bpf_bbr1_undo_cwnd()`

`tcp_validate_congestion_control()` requires every congestion control algorithm to provide `.ssthresh` and `.undo_cwnd`. In this first step, they simply return the current `tp->snd_ssthresh` and `tp->snd_cwnd`.

---

## Understanding `test.pkt`

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step01-pacing-rate/test.pkt) validates `bpf_bbr1` in two stages:

1. **3-way Handshake (100 ms RTT, 1000-byte payload MSS)**:
   - The client sends a SYN with `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>`. Subtracting the 12-byte TCP timestamp option leaves `tp->mss_cache = 1000` bytes.
   - The handshake ACK arrives after `+0.10` (100 ms), setting `tp->srtt_us >> 3 = 100,000` us.
   - `bpf_bbr1_init()` sets the initial pacing rate to `10 * 1000 B / 0.1 s * (2885 / 1024) = 281,738 B/s`, verified by `assert 200000 < tcpi_pacing_rate < 400000`.
2. **Data Transmission (10 segments / 10,000 bytes ACKed after 10 ms)**:
   - The server writes `10,000` bytes (10 segments), and the client ACKs all `10,000` bytes after `+0.01` (10 ms).
   - This produces a `rate_sample` with `rs->delivered = 10` and `rs->interval_us = 10,000` us, corresponding to a delivery rate of `10,000 B / 0.01 s = 1,000,000 B/s` (1 MB/s).
   - `bpf_bbr1_cong_control()` multiplies this rate by `2885 / 1024`, updating `tcpi_pacing_rate` to `~2,817,382 B/s` (verified by `assert tcpi_pacing_rate > 500000` and `assert 2000000 < tcpi_pacing_rate < 3500000`).

---

## How to run

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step01-pacing-rate starter
# 2. Implement the TODO in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step01-pacing-rate solution
```
