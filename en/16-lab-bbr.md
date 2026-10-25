# 16. Hands-on lab: Building BBR with BPF CC and packetdrill

This chapter collects the 6 hands-on steps under `labs/bbr/`, building BBRv1 from `.cong_control` pacing rate control through `BtlBw`/`RTprop` tracking, `STARTUP` -> `DRAIN` -> `PROBE_BW` -> `PROBE_RTT`, and loss recovery / ACK aggregation.


## Step 1: The `.cong_control` Hook and Pacing Rate Calculation from Delivery Rate

### Goal

In this step, you will build the foundation of BBR (Bottleneck Bandwidth and RTT) using BPF `struct_ops` (`struct tcp_congestion_ops`) by implementing **pacing rate control** driven directly by measured delivery rates.

- Understand the fundamental architectural difference between `.cong_avoid` (used by Reno and CUBIC) and `.cong_control` (used by BBR).
- Enable TCP internal pacing (`SK_PACING_NEEDED`) in `.init` and initialise the pacing rate from the initial `cwnd` and initial RTT.
- Compute the instantaneous delivery rate from `struct rate_sample` inside `.cong_control`, apply a fixed-point pacing gain (`BBR_HIGH_GAIN`), and update `sk->sk_pacing_rate`.

---

### Design Checkpoints (Think before coding)

Before looking at `solution.bpf.c`, think through the following four design questions.

#### Q1. Which hook should we implement?

In Reno and CUBIC, congestion control revolves around `.cong_avoid(sk, ack, acked)` and `.ssthresh(sk)`. BBR does not implement `.cong_avoid`. Which callback in `struct tcp_congestion_ops` should we use instead, and how does defining it change the behaviour of the kernel's `tcp_cong_control()` function?

<details>
<summary>Hint and Answer</summary>

- **Hook to use**: `.cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)`.
- **Kernel behaviour**: In `tcp_cong_control()`, if `icsk_ca_ops->cong_control` is non-NULL, the kernel invokes it and returns immediately. Standard kernel helpers (`tcp_cwnd_reduction()`, `tcp_cong_avoid()`, and `tcp_update_pacing_rate()`) are completely bypassed, giving `.cong_control` full authority over both `snd_cwnd` and `sk_pacing_rate`.
- Note that `.ssthresh` and `.undo_cwnd` are still mandatory callbacks required by `tcp_validate_congestion_control()` at registration time.

</details>

#### Q2. How do we enable TCP internal pacing on a socket?

Simply writing a byte-per-second value to `sk->sk_pacing_rate` has no effect unless either the `sch_fq` qdisc is attached or TCP's built-in high-resolution timer (`hrtimer`) pacing is enabled. Which field of `struct sock` must be set in `.init(sk)` to activate internal pacing?

<details>
<summary>Hint and Answer</summary>

- Set `sk->sk_pacing_status = SK_PACING_NEEDED;`.
- The BPF `tcp_congestion_ops` verifier (`bpf_tcp_ca_btf_struct_access()`) explicitly allows writing to both `sk->sk_pacing_status` and `sk->sk_pacing_rate`.

</details>

#### Q3. How do we convert `struct rate_sample` into a byte-per-second delivery rate?

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

#### Q4. What edge cases and invalid inputs must we guard against?

Not every ACK produces a valid rate sample, and `.init(sk)` may run when `tp->srtt_us` is still zero. What checks are needed to prevent bogus updates or division by zero?

<details>
<summary>Hint and Answer</summary>

- In `.cong_control`, check `if (rs->delivered < 0 || rs->interval_us <= 0) return;`. This filters out invalid rate samples and guarantees `rs->interval_us` is strictly positive before dividing.
- In `.init`, `tp->srtt_us` stores the smoothed RTT scaled by 8 (`srtt_us >> 3` in microseconds). If `tp->srtt_us == 0`, fall back to a 1 ms nominal RTT (`USEC_PER_MSEC = 1000` us), and guard with `if (!rtt_us) rtt_us = 1;` before dividing.

</details>

---

### Step-by-step Walkthrough

#### 1. Fixed-Point Constants

Because floating-point instructions are rejected by the BPF verifier, BBR represents gains as fixed-point fractions scaled by a power of two:

- `BBR_SCALE = 10` (`BBR_UNIT = 1 << 10 = 1024`) represents a `1.0x` gain.
- `BBR_HIGH_GAIN = 2885` represents the STARTUP gain of $2 / \ln(2) \approx 2.885\text{x}$ ($2885 / 1024 \approx 2.817\text{x}$), which doubles the sending rate every RTT.

#### 2. `bpf_bbr1_init()` — Initialising Internal Pacing and Initial Rate

When the connection is established, `bpf_bbr1_init()` sets an initial pacing rate so the socket paces traffic before the first data ACK arrives:

- `tp->srtt_us` is measured from the 3-way handshake SYN-ACK exchange and shifted right by 3 (`tp->srtt_us >> 3`) to obtain microseconds. If no RTT sample exists yet, `USEC_PER_MSEC` (1 ms) is used.
- The initial rate is calculated as `(tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC / rtt_us) * BBR_HIGH_GAIN >> BBR_SCALE` and written to `sk->sk_pacing_rate`.
- Writing `sk->sk_pacing_status = SK_PACING_NEEDED` instructs the TCP stack to pace outgoing packets.

#### 3. `bpf_bbr1_cong_control()` — Updating Pacing Rate from `struct rate_sample`

On every ACK, the kernel's `tcp_rate_gen()` populates `struct rate_sample *rs` and passes it to `bpf_bbr1_cong_control()`:

1. Invalid samples (`rs->delivered < 0 || rs->interval_us <= 0`) are ignored.
2. The instantaneous delivery rate in bytes/sec is computed via `div64_u64((__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC, (__u64)rs->interval_us)`.
3. The rate is scaled by `BBR_HIGH_GAIN` (`(rate * BBR_HIGH_GAIN) >> BBR_SCALE`) and written to `sk->sk_pacing_rate`.

#### 4. `bpf_bbr1_ssthresh()` and `bpf_bbr1_undo_cwnd()`

`tcp_validate_congestion_control()` requires every congestion control algorithm to provide `.ssthresh` and `.undo_cwnd`. In this first step, they simply return the current `tp->snd_ssthresh` and `tp->snd_cwnd`.

---

### Understanding `test.pkt`

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

### How to run

```sh
## 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step01-pacing-rate starter
## 2. Implement the TODO in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step01-pacing-rate solution
```


### `labs/bbr/step01-pacing-rate/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1 (Starter): .cong_control hook and Pacing Rate calculation
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BBR_SCALE	10
#define BBR_UNIT	(1 << BBR_SCALE)	/* 1024 */
#define BBR_HIGH_GAIN	2885			/* ~2.885x (2/ln(2)) STARTUP gain */

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 rate;

	if (!rtt_us)
		rtt_us = 1;

	/* Initialize pacing rate: high_gain * init_cwnd * mss / initial_rtt */
	rate = (__u64)tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, rtt_us);
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);

	(void)tp;
	/*
	 * TODO:
	 * 1. Check if the rate sample is valid:
	 *    if (rs->delivered < 0 || rs->interval_us <= 0) return;
	 * 2. Calculate instantaneous delivery rate in bytes/sec:
	 *    rate = (u64)rs->delivered * tp->mss_cache * USEC_PER_SEC / rs->interval_us
	 *    (Hint: use div64_u64(rate, (__u64)rs->interval_us))
	 * 3. Multiply by BBR_HIGH_GAIN and shift right by BBR_SCALE:
	 *    rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;
	 * 4. Write the result to sk->sk_pacing_rate.
	 */
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr1 = {
	.name		= "bpf_bbr1",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr1_init,
	.cong_control	= (void *)bpf_bbr1_cong_control,
	.ssthresh	= (void *)bpf_bbr1_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr1_undo_cwnd,
};
```


### `labs/bbr/step01-pacing-rate/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 1: Verify .cong_control hook and pacing rate calculation from rate_sample
0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr1`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial RTT = 100 ms (so initial pacing rate is ~281 KB/s)
// MSS = 1012 -> payload MSS = 1000 bytes (with 12-byte TCP timestamps)
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.10 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 200 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Verify initial cwnd=10 and initial pacing rate (~281,738 B/s from 100 ms SYN RTT)
+0.00 %{
assert tcpi_snd_cwnd == 10, f"expected cwnd 10, got {tcpi_snd_cwnd}"
assert 200000 < tcpi_pacing_rate < 400000, f"expected initial pacing_rate ~281KB/s, got {tcpi_pacing_rate}"
}%

// Send 10 segments (10,000 bytes) and ACK after 10 ms (+0.010):
// Delivery rate = 10 * 1000 B / 0.010 s = 1,000,000 B/s
// Pacing rate = 1,000,000 * 2885 / 1024 = ~2,817,382 B/s
+0.00 write(4, ..., 10000) = 10000
+0.00 > P. 1:10001(10000) ack 1 <nop,nop,TS val 200 ecr 200>
+0.01 < . 1:1(0) ack 10001 win 257 <nop,nop,TS val 210 ecr 200>

+0.00 %{
assert tcpi_pacing_rate > 500000, f"expected pacing_rate > 500000, got {tcpi_pacing_rate}"
assert 2000000 < tcpi_pacing_rate < 3500000, f"expected pacing_rate ~2.82MB/s, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step01-pacing-rate/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: .cong_control hook and Pacing Rate calculation from Delivery Rate
 *
 * Unlike loss-based congestion control (Reno/CUBIC) which uses .cong_avoid,
 * BBR uses .cong_control(sk, ack, flag, rs) to receive a struct rate_sample
 * on every ACK and directly drive sk->sk_pacing_rate.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BBR_SCALE	10
#define BBR_UNIT	(1 << BBR_SCALE)	/* 1024 */
#define BBR_HIGH_GAIN	2885			/* ~2.885x (2/ln(2)) STARTUP gain */

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 rate;

	if (!rtt_us)
		rtt_us = 1;

	/* Initialize pacing rate: high_gain * init_cwnd * mss / initial_rtt */
	rate = (__u64)tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, rtt_us);
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u64 rate;

	/* Ignore invalid delivery rate samples */
	if (rs->delivered < 0 || rs->interval_us <= 0)
		return;

	/* Instantaneous delivery rate (bytes/sec):
	 *   bw = delivered * mss * USEC_PER_SEC / interval_us
	 */
	rate = (__u64)rs->delivered * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, (__u64)rs->interval_us);

	/* Apply STARTUP pacing gain (~2.885x) */
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr1 = {
	.name		= "bpf_bbr1",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr1_init,
	.cong_control	= (void *)bpf_bbr1_cong_control,
	.ssthresh	= (void *)bpf_bbr1_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr1_undo_cwnd,
};
```

</details>

---

## Step 2: BDP Calculation from BtlBw (Max Bandwidth Filter) and RTprop (Min RTT Filter)

### Goal

In this step, we build upon Step 1's pacing rate control to implement the two core parameters of BBR's network path model — **BtlBw (Bottleneck Bandwidth via a max-filter)** and **RTprop (Round-Trip Propagation Time via a min-filter)** — and compute the **BDP (Bandwidth-Delay Product)** to drive both `snd_cwnd` and `sk_pacing_rate`.

- Store per-socket BBR state (`min_rtt_us`, `max_bw`, `round_cnt`, `next_rtt_delivered`) inside `inet_csk_ca(sk)` (`struct bpf_bbr2_ca`).
- Understand the physical reason why RTT is tracked with a **minimum filter** whilst delivery rate (bandwidth) is tracked with a **maximum filter**.
- Prevent application-limited (`rs->is_app_limited`) rate samples from artificially dragging down `max_bw`.
- Implement `bbr_bdp()` using fixed-point `BW_UNIT` (`1 << 24`) arithmetic and set `tp->snd_cwnd = max(bdp + 4U, 4U)`.

---

### Design Checkpoints (Think before coding)

Before looking at the solution code, work through these four questions on your own.

#### Q1. Why do we take the *minimum* of RTT samples, but the *maximum* of delivery rate samples?

What goes wrong if we overwrite our RTT and bandwidth estimates on every single ACK sample (as in `starter.bpf.c`)? From basic queueing theory, why does estimating physical **round-trip propagation delay (RTprop)** require a min-filter, whereas estimating **bottleneck bandwidth (BtlBw)** requires a max-filter?

<details>
<summary>Hint & Answer</summary>

- **RTprop (minimum RTT filter)**: Every measured RTT is the sum of the two-way physical propagation delay (`RTprop`) plus any queueing delay along the path (`RTT = RTprop + queueing_delay`). Because queueing delay is always non-negative (`>= 0`), the true unloaded propagation delay corresponds to the **minimum** RTT observed.
- **BtlBw (maximum bandwidth filter)**: The delivery rate (`rs->delivered / rs->interval_us`) can never exceed the physical bottleneck capacity of the link, but it can easily fall below the bottleneck rate whenever the sender transmits less than a full pipe or shares the link momentarily. Thus, the **maximum** observed delivery rate estimates the bottleneck bandwidth (`max_bw`).

</details>

#### Q2. How should `max_bw` handle application-limited (`rs->is_app_limited`) samples?

Suppose a flow sends a 10-packet burst at line rate, establishing an accurate `max_bw`, and then the application pauses and writes only 2 packets over a long interval. The resulting `rate_sample` will have `rs->is_app_limited == true` and a much lower delivery rate. Under what condition should `ca->max_bw` be updated?

<details>
<summary>Hint & Answer</summary>

- When `rs->is_app_limited` is set, the sender did not keep the pipe full; the low delivery rate reflects application write behaviour rather than network capacity.
- Therefore, we gate the filter with `if (!rs->is_app_limited || bw >= ca->max_bw)` and update `ca->max_bw` when `bw > ca->max_bw`. Note that if an app-limited sample still manages to exceed our current `ca->max_bw`, it proves the bottleneck is at least that fast, so we allow it to raise `ca->max_bw`.

</details>

#### Q3. How do we count packet-timed round trips (`round_cnt`) without using wall-clock timers?

In later steps (Startup exit detection and ProbeBW gain cycling), BBR measures time in **packet-timed round trips** rather than wall-clock milliseconds. How can you use `tp->delivered` and `rs->prior_delivered` to detect that one full round trip has elapsed?

<details>
<summary>Hint & Answer</summary>

- Record the cumulative delivered packet counter at the start of a round in `ca->next_rtt_delivered = tp->delivered`.
- Each incoming ACK carries `rs->prior_delivered` — the value of `tp->delivered` at the moment the newly ACKed packet was transmitted.
- Once `!before(rs->prior_delivered, ca->next_rtt_delivered)` holds, a packet sent at or after the start of the current round has been acknowledged, meaning **one full packet-timed round trip** has completed. We increment `ca->round_cnt++` and advance `ca->next_rtt_delivered = tp->delivered`.

</details>

#### Q4. How do we compute `bdp` in packets from `max_bw` (`BW_UNIT = 1 << 24`) and `min_rtt_us`?

Because `rs->delivered / rs->interval_us` (in packets per microsecond) is almost always `< 1`, BBR scales `bw` by `BW_UNIT = 1 << 24` (`BW_SCALE = 24`). Given `bw` in `packets/usec << 24`, `ca->min_rtt_us` in microseconds, and `gain` scaled by `BBR_UNIT = 1 << 8` (`BBR_SCALE = 8`), how do we compute `bdp = ceil(bw * min_rtt_us * gain)` using integer arithmetic?

<details>
<summary>Hint & Answer</summary>

- First multiply in 64-bit integer precision: `w = (__u64)bw * ca->min_rtt_us;` (which represents `BDP * BW_UNIT`).
- Apply `gain` (`>> BBR_SCALE`) and perform ceiling division by `BW_UNIT`:
  ```c
  bdp = (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
  ```
- Rounding up (`+ BW_UNIT - 1`) prevents truncation from shrinking `snd_cwnd` slightly below the true BDP, avoiding a negative feedback loop where the flow becomes permanently cwnd-limited below capacity.

</details>

---

### Step-by-step Walkthrough

#### 1. Per-Socket State Struct `struct bpf_bbr2_ca`

Each socket provides private congestion control storage in `inet_csk(sk)->icsk_ca_priv`, accessed via `inet_csk_ca(sk)` in [bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h):

```c
struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};
```

- `min_rtt_us`: Minimum observed RTT (`RTprop`, in microseconds; `0` before the first sample).
- `max_bw`: Maximum observed delivery rate (`BtlBw`, in `packets / usec << BW_SCALE`).
- `round_cnt`: Count of packet-timed round trips elapsed.
- `next_rtt_delivered`: `tp->delivered` threshold marking the end of the current round trip.

#### 2. `bbr_bdp()` and `bbr_rate_bytes_per_sec()`

In [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c), we use the same fixed-point shifts as Linux's `tcp_bbr.c` (`BW_SCALE = 24`, `BBR_SCALE = 8`):

- `bbr_rate_bytes_per_sec()`: Converts `bw` (`pkts/us << 24`) into `bytes/sec`, applying `gain` (`>> BBR_SCALE`) and subtracting the 1% pacing margin (`BBR_PACING_MARGIN_PCT = 1U`).
- `bbr_bdp()`: Returns `10U` if `ca->min_rtt_us` or `bw` is not yet initialised (`0`), and otherwise computes `(((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT`.

#### 3. `bpf_bbr2_cong_control()` — Filtering `BtlBw` & `RTprop` and Setting `snd_cwnd`

On every ACK, `bpf_bbr2_cong_control()` executes four steps:

1. **Min-filter `min_rtt_us` (`RTprop`)**:
   - If `rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0)`, update `ca->min_rtt_us = rs->rtt_us`.
2. **Advance `round_cnt` and max-filter `max_bw` (`BtlBw`)**:
   - When `rs->delivered > 0 && rs->interval_us > 0`, check `!before(rs->prior_delivered, ca->next_rtt_delivered)` to advance `ca->round_cnt` and `ca->next_rtt_delivered = tp->delivered`.
   - Compute `bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us)`.
   - If `!rs->is_app_limited || bw >= ca->max_bw`, update `ca->max_bw = max(ca->max_bw, bw)`.
3. **Update `sk_pacing_rate`**:
   - Call `bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT)`.
4. **Set `snd_cwnd` from BDP**:
   - Compute `bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT)` and set `tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET)`.

---

### Understanding `test.pkt`

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/test.pkt) exercises both the `BtlBw` max-filter and the `RTprop` min-filter across two phases:

1. **3-way Handshake (RTT = 10 ms, payload MSS = 1000 B)**:
   - SYN and SYN-ACK are exchanged at `0.00`, and the handshake ACK arrives at `+0.01` (10 ms RTT).
2. **Phase 1: High-bandwidth burst (10 segments ACKed in 10 ms)**:
   - `write(4, ..., 10000)` sends 10 segments (`1:10001(10000)`), which are ACKed 10 ms later (`+0.01`).
   - This produces `rs->rtt_us = 10000` (10 ms), `rs->delivered = 10`, and `rs->interval_us = 10000` (10 ms).
   - `ca->min_rtt_us` becomes `10000` us, `ca->max_bw` becomes `1000 pkts/sec` (`1,000,000 B/s`), `bdp = 10` packets, `tcpi_snd_cwnd = 10 + 4 = 14`, and `tcpi_pacing_rate` is `~990,000 B/s` (with the 1% pacing margin).
3. **Phase 2: App-limited small burst with longer interval and inflated RTT (2 segments, interval 50 ms, RTT 20 ms)**:
   - After a 30 ms pause (`+0.03`), the application writes only 2,000 bytes (2 segments), and the ACK returns 20 ms later (`+0.02`).
   - This sample has `rs->rtt_us = 20000` (inflated to 20 ms), `rs->delivered = 2`, and `rs->interval_us = 50000` (40 pkts/sec).
   - In [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c), naively overwriting `min_rtt_us` and `max_bw` collapses the bandwidth estimate by 25x, causing the `tcpi_snd_cwnd == 14` and `tcpi_pacing_rate` assertions to fail.
   - In [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c), the min-filter keeps `min_rtt_us = 10000` and the max-filter keeps `max_bw` at `1000 pkts/sec`, preserving `tcpi_snd_cwnd == 14` and `tcpi_pacing_rate ~ 990 KB/s`.

---

### How to run

```sh
## 1. Verify that starter.bpf.c fails the packetdrill assertions
../../run_test.sh bbr/step02-btlbw-rtprop starter
## 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step02-btlbw-rtprop solution
```


### `labs/bbr/step02-btlbw-rtprop/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 2: Bottleneck Bandwidth (BtlBw) and Round-Trip Propagation Time (RtProp) Tracking (Starter Skeleton)
 *
 * TODO 1: Implement min-filter for RtProp (min_rtt_us) instead of blindly overwriting
 * TODO 2: Implement max-filter for BtlBw (max_bw) and respect app-limited rate samples
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE 24
#define BW_UNIT (1U << BW_SCALE)
#define BBR_SCALE 8
#define BBR_UNIT (1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET 4U
#define BBR_PACING_MARGIN_PCT 1U

struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (rate > 0)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	bbr_set_pacing_rate(sk, bw, BBR_UNIT);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u32 bw, bdp;

	/*
	 * TODO 1: Track path propagation delay (RtProp).
	 * Do not overwrite min_rtt_us with every sample. Instead, track the minimum
	 * observed RTT across non-zero RTT samples.
	 */
	if (rs->rtt_us > 0)
		ca->min_rtt_us = rs->rtt_us;

	if (rs->delivered > 0 && rs->interval_us > 0) {
		if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
			ca->round_cnt++;
			ca->next_rtt_delivered = tp->delivered;
		}

		bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);

		/*
		 * TODO 2: Track bottleneck bandwidth (BtlBw).
		 * A flow cannot accurately measure bandwidth when application-limited,
		 * because it doesn't send fast enough to saturate the pipe.
		 * Ignore app-limited samples unless they exceed the current max_bw estimate,
		 * and track the maximum bandwidth across samples.
		 */
		ca->max_bw = bw;
	}

	bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT);

	bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_ssthresh, struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr2 = {
	.name		= "bpf_bbr2",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr2_init,
	.cong_control	= (void *)bpf_bbr2_cong_control,
	.ssthresh	= (void *)bpf_bbr2_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr2_undo_cwnd,
};
```


### `labs/bbr/step02-btlbw-rtprop/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 2: Track BtlBw and RtProp with app-limited rate sample filtering

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr2`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with 10 ms RTT (0.000 SYN, 0.000 SYN-ACK, 0.010 ACK)
// MSS = 1012 with 12-byte TCP timestamps -> payload MSS = 1000 bytes
+0.00 < S 0:0(0) win 32792 <mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>
+0.00 > S. 0:0(0) ack 1 <mss 1460,sackOK,TS val 100 ecr 100,nop,wscale 7>
+0.01 < . 1:1(0) ack 1 win 257 <nop,nop,TS val 110 ecr 100>
+0.00 accept(3, ..., ...) = 4

// Phase 1: High-bandwidth burst
// write(4, ..., 10000) sends 10 segments (1:10001) at 0.010, ACKed at 0.020.
// rtt_us = 10000, delivered = 10, interval_us = 10000
// bw = 10 pkts / 10 ms = 1000 pkts/s (1,000,000 B/s)
// bdp = 10 pkts
// cwnd = bdp + 4 = 14
// pacing_rate = 99% of 1 MB/s = ~990,000 B/s
+0.00 write(4, ..., 10000) = 10000
+0.00 > P. 1:10001(10000) ack 1 <nop,nop,TS val 110 ecr 110>
+0.01 < . 1:1(0) ack 10001 win 257 <nop,nop,TS val 120 ecr 110>

+0.00 %{
assert tcpi_snd_cwnd == 14, f"expected cwnd 14, got {tcpi_snd_cwnd}"
assert 950000 < tcpi_pacing_rate < 1050000, f"expected pacing_rate ~990KB/s, got {tcpi_pacing_rate}"
}%

// Phase 2: App-limited smaller burst with longer interval and higher RTT
// Send 2 segments (2,000 bytes) at 0.050 (+0.030), ACKed at 0.070 (+0.020).
// rtt_us = 20000, interval_us = 50000, is_app_limited = true.
// BtlBw and RtProp tracking should preserve max_bw = 1000 pkts/s and min_rtt = 10000 us!
+0.03 write(4, ..., 2000) = 2000
+0.00 > P. 10001:12001(2000) ack 1 <nop,nop,TS val 150 ecr 120>
+0.02 < . 1:1(0) ack 12001 win 257 <nop,nop,TS val 170 ecr 150>

+0.00 %{
assert tcpi_snd_cwnd == 14, f"expected cwnd 14 to be preserved, got {tcpi_snd_cwnd}"
assert 950000 < tcpi_pacing_rate < 1050000, f"expected pacing_rate ~990KB/s to be preserved, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step02-btlbw-rtprop/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 2: Bottleneck Bandwidth (BtlBw) and Round-Trip Propagation Time (RtProp) Tracking
 *
 * In BBR, the network path model consists of:
 * - RtProp (min_rtt_us): the two-way propagation delay of the path without queuing.
 *   Tracked via a windowed min-filter over RTT samples.
 * - BtlBw (max_bw): the bottleneck bandwidth available to the flow.
 *   Tracked via a windowed max-filter over rate samples from ACKs.
 *
 * Important BBR invariant:
 * Application-limited (app-limited) rate samples must NEVER decrease max_bw
 * unless the sample is larger than the current max_bw estimate.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE 24
#define BW_UNIT (1U << BW_SCALE)
#define BBR_SCALE 8
#define BBR_UNIT (1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET 4U
#define BBR_PACING_MARGIN_PCT 1U

struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (rate > 0)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	bbr_set_pacing_rate(sk, bw, BBR_UNIT);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u32 bw, bdp;

	if (rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	if (rs->delivered > 0 && rs->interval_us > 0) {
		if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
			ca->round_cnt++;
			ca->next_rtt_delivered = tp->delivered;
		}

		bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);

		if (!rs->is_app_limited || bw >= ca->max_bw) {
			if (bw > ca->max_bw)
				ca->max_bw = bw;
		}
	}

	bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT);

	bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_ssthresh, struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr2 = {
	.name		= "bpf_bbr2",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr2_init,
	.cong_control	= (void *)bpf_bbr2_cong_control,
	.ssthresh	= (void *)bpf_bbr2_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr2_undo_cwnd,
};
```

</details>

---

## Step 3: Transitioning from STARTUP to DRAIN (Full Pipe Detection)

### Goal

In this step, you will implement BBR's state transitions from **STARTUP** (exponential bandwidth search) to **DRAIN** (draining the queue created during STARTUP) and into steady-state **PROBE_BW** (cruising at the estimated bottleneck bandwidth).

- Understand BBR's three core modes (`BBR_STARTUP`, `BBR_DRAIN`, and `BBR_PROBE_BW`) and the `pacing_gain` / `cwnd_gain` used in each mode.
- Track packet-timed round trips using `tp->delivered` and `ca->next_rtt_delivered`.
- Implement `bbr_check_full_bw_reached()` to detect when the bottleneck pipe is full after 3 consecutive non-application-limited rounds without at least 25% (`1.25x`) bandwidth growth.
- Implement `bbr_check_drain()` to transition from `BBR_STARTUP` to `BBR_DRAIN` once the pipe is full, and from `BBR_DRAIN` to `BBR_PROBE_BW` once packets in flight (`tcp_packets_in_flight()`) drop to or below `1.0x BDP`.

---

### Design Checkpoints (Think before coding)

Before inspecting `solution.bpf.c`, think through the following four design questions.

#### Q1. How can BBR tell that the pipe is full without waiting for packet loss?

Loss-based congestion control algorithms such as Reno and CUBIC double `cwnd` in slow start until packets are dropped (or until HyStart detects an RTT increase). Because BBR does not use packet loss as a congestion signal, how should it decide when to exit `BBR_STARTUP` (where gain is $2 / \ln(2) \approx 2.89\text{x}$)?

<details>
<summary>Hint and Answer</summary>

- **Watch for a plateau in estimated bottleneck bandwidth (`BtlBw`)**.
- While the bottleneck link is not yet saturated, doubling the sending rate each round causes the measured delivery rate (`ca->max_bw`) to grow rapidly each round.
- Once the sending rate reaches the bottleneck capacity, sending faster only builds a queue at the bottleneck router (increasing RTT), whilst the **delivery rate (`ca->max_bw`) stops growing**.
- BBR concludes that the pipe is full when `ca->max_bw` fails to grow by at least **25% (`bbr_full_bw_thresh = 5/4 = 1.25x`)** over its baseline `ca->full_bw` for **3 consecutive non-app-limited rounds (`bbr_full_bw_cnt = 3`)**.

</details>

#### Q2. How do we count round trips using packet delivery counts instead of wall-clock timers?

To check whether bandwidth has plateaued for 3 round trips, we need a clean definition of a round trip that does not depend on noisy timers. How can we use `tp->delivered` and `rs->prior_delivered` to detect when one packet-timed round has completed?

<details>
<summary>Hint and Answer</summary>

- Record the value of `tp->delivered` at the start of a round in `ca->next_rtt_delivered`.
- Whenever a packet is transmitted, the kernel's `tcp_rate_skb_sent()` snapshots the current `tp->delivered` into the `skb` control block. When that packet is later acknowledged, `tcp_rate_skb_delivered()` copies the snapshot into `rs->prior_delivered`.
- Therefore, when `!before(rs->prior_delivered, ca->next_rtt_delivered)` holds (or `tp->delivered >= ca->next_rtt_delivered` when one ACK covers the round), a packet sent at or after the start of the current round has been acknowledged — meaning one full round trip has elapsed.
- We then advance the round boundary by setting `ca->next_rtt_delivered = tp->delivered;` and `ca->round_start = 1`.

</details>

#### Q3. Why does BBR enter `BBR_DRAIN` before `BBR_PROBE_BW` instead of switching directly?

By the time `BBR_STARTUP` detects 3 rounds without 25% bandwidth growth, how much excess queue has accumulated at the bottleneck, and how should `pacing_gain` and `cwnd_gain` be chosen to drain it?

<details>
<summary>Hint and Answer</summary>

- Because `BBR_STARTUP` sends with `bbr_high_gain` (`~2.89x`) and waits 3 rounds to confirm the bandwidth plateau, in-flight data reaches roughly `2.89x BDP`, leaving an excess queue of about `1.89x BDP` at the bottleneck.
- Switching straight to `1.0x BtlBw` pacing in `BBR_PROBE_BW` would never drain that standing queue.
- Instead, BBR enters `BBR_DRAIN` with `pacing_gain = bbr_drain_gain = BBR_UNIT * 1000 / 2885` (`88 / 256`, `~0.35x`, the reciprocal of `bbr_high_gain`) to pace slower than the bottleneck rate and drain the queue, whilst keeping `cwnd_gain = bbr_high_gain` so pacing — not an abrupt `cwnd` cut — smoothly drains the pipe.
- As soon as `tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)` (`1.0x BDP`), the queue is drained and BBR transitions to `BBR_PROBE_BW` (`pacing_gain = 1.0x`, `cwnd_gain = 2.0x`).

</details>

#### Q4. Why does `bbr_set_pacing_rate()` check `ca->full_bw_reached` before lowering `sk->sk_pacing_rate`?

In `bbr_set_pacing_rate()`, the socket pacing rate is updated only when `if (ca->full_bw_reached || rate > sk->sk_pacing_rate)` is true. Why must BBR refuse to lower `sk->sk_pacing_rate` before `full_bw_reached` is set, yet allow lowering it afterwards?

<details>
<summary>Hint and Answer</summary>

- Early in `BBR_STARTUP`, a flow may see artificially low rate samples due to small initial windows or brief application limits. Lowering `sk->sk_pacing_rate` before filling the pipe (`!ca->full_bw_reached`) would stall STARTUP's exponential ramp-up.
- Once the pipe has been filled (`ca->full_bw_reached == 1`), BBR must lower its pacing rate from `2.89x BtlBw` down to `0.35x BtlBw` in `BBR_DRAIN` and `1.0x BtlBw` in `BBR_PROBE_BW`.

</details>

---

### Step-by-step Walkthrough

#### 1. Modes and Fixed-Point Constants

With `BBR_SCALE = 8` (`BBR_UNIT = 256` representing `1.0x`), the gains and full-pipe thresholds are defined as:

- `bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1 = 739` (`~2.89x`): `pacing_gain` and `cwnd_gain` in `BBR_STARTUP`.
- `bbr_drain_gain = BBR_UNIT * 1000 / 2885 = 88` (`~0.35x`): `pacing_gain` in `BBR_DRAIN`.
- `bbr_cwnd_gain = BBR_UNIT * 2 = 512` (`2.0x`): `cwnd_gain` in `BBR_PROBE_BW`.
- `bbr_full_bw_thresh = BBR_UNIT * 5 / 4 = 320` (`1.25x`): required bandwidth growth factor (+25%) per round to remain in `BBR_STARTUP`.
- `bbr_full_bw_cnt = 3`: number of consecutive non-app-limited rounds without 25% growth that triggers full-pipe detection.

#### 2. `bbr_check_full_bw_reached()` — Full Pipe Detection

Evaluated at the end of each packet-timed round (`ca->round_start == 1`) when the rate sample is not application-limited (`!rs->is_app_limited`):

1. Compute the 1.25x growth threshold: `bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE`.
2. If `ca->max_bw >= bw_thresh`, bandwidth grew by at least 25%: record the new baseline `ca->full_bw = ca->max_bw` and reset `ca->full_bw_cnt = 0`.
3. Otherwise, increment `++ca->full_bw_cnt`; when it reaches `bbr_full_bw_cnt` (`3`), set `ca->full_bw_reached = 1`.

#### 3. `bbr_check_drain()` — Transitioning to `BBR_DRAIN` and `BBR_PROBE_BW`

1. When `ca->mode == BBR_STARTUP && ca->full_bw_reached`, transition to `ca->mode = BBR_DRAIN` and record `1.0x BDP` in `tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT)` for observability.
2. Using a separate `if` statement (so it can fall through on the same ACK if in-flight packets are already at or below BDP), check `ca->mode == BBR_DRAIN && tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)`. When satisfied, transition to `ca->mode = BBR_PROBE_BW`.

#### 4. `bbr_update_gains()` and `bbr_set_cwnd()`

- `bbr_update_gains()` updates `ca->pacing_gain` and `ca->cwnd_gain` according to `ca->mode`.
- `bbr_set_cwnd()` computes `target_cwnd = bbr_bdp(sk, bw, gain)`. Once `ca->full_bw_reached` is set, `cwnd = min(cwnd + acked, target_cwnd)` clamps `snd_cwnd` down to `target_cwnd` (`2.0x BDP` in `BBR_PROBE_BW`).

---

### Understanding `test.pkt`

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step03-startup-drain/test.pkt) drives the connection across a constant bottleneck rate (1 segment / 10 ms = `100 KB/s`) for 4 rounds and verifies the transition out of `BBR_STARTUP`:

1. **Handshake and 5,000-byte Bulk Write**:
   - The 3-way handshake establishes a connection with `MSS = 1000` bytes and a receive window of `win 1000` (1 segment).
   - A single `write(4, ..., 5000)` queues 5 segments at once so the write queue never empties across rounds, ensuring `rs->is_app_limited == 0` after the initial warmup round.
2. **Round 1 (`1001:2001`)**:
   - `ca->max_bw` (`~100 KB/s`) is stored in `ca->full_bw` and `ca->full_bw_cnt` is `0`.
   - The flow is in `BBR_STARTUP` (`pacing_gain = 739/256 ~= 2.89x`), so `tcpi_pacing_rate` is `~286 KB/s` (`250000 < tcpi_pacing_rate < 320000`) and `tcpi_snd_ssthresh` remains `0x7fffffff`.
3. **Rounds 2 and 3 (`2001:3001`, `3001:4001`)**:
   - Because the bottleneck rate stays constant at `100 KB/s` (0% growth `< 25%`), `ca->full_bw_cnt` increments to `1` and `2`. The flow stays in `BBR_STARTUP` at `~286 KB/s`.
4. **Round 4 (`4001:5001`)**:
   - After the 3rd plateau round without 25% bandwidth growth, `ca->full_bw_cnt` reaches `3` and sets `ca->full_bw_reached = 1`.
   - `bbr_check_drain()` transitions `BBR_STARTUP` -> `BBR_DRAIN` (`tp->snd_ssthresh = bbr_bdp(...)`) and, since `in_flight == 0 <= BDP`, immediately into `BBR_PROBE_BW` (`pacing_gain = 1.0x`, `cwnd_gain = 2.0x`).
   - The script asserts that `tcpi_pacing_rate` drops to `~99 KB/s` (`90000 < tcpi_pacing_rate < 110000`), `1 <= tcpi_snd_ssthresh <= 4`, and `tcpi_snd_cwnd == 4` (clamped to `bbr_cwnd_min_target = 4`).

---

### How to run

```sh
## 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step03-startup-drain starter
## 2. Implement the TODO in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step03-startup-drain solution
```


### `labs/bbr/step03-startup-drain/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 3: STARTUP to DRAIN and PROBE_BW Transition (Starter Skeleton)
 *
 * TODO 1: Implement bbr_check_full_bw_reached() to detect 3 consecutive
 *         non-app-limited rounds without >= 25% bandwidth growth.
 * TODO 2: Implement bbr_check_drain() to transition from BBR_STARTUP to
 *         BBR_DRAIN, and from BBR_DRAIN to BBR_PROBE_BW once in-flight <= BDP.
 * TODO 3: Implement bbr_update_gains() to set pacing_gain and cwnd_gain
 *         for BBR_STARTUP, BBR_DRAIN, and BBR_PROBE_BW.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1U << BW_SCALE)
#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET	4U
#define BBR_PACING_MARGIN_PCT	1U

enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* steady-state: pace around estimated bw */
};

static const __u32 bbr_high_gain      = BBR_UNIT * 2885 / 1000 + 1; /* 739 (~2.89x) */
static const __u32 bbr_drain_gain     = BBR_UNIT * 1000 / 2885;     /* 88 (~0.35x) */
static const __u32 bbr_cwnd_gain      = BBR_UNIT * 2;               /* 512 (2.0x) */
static const __u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;           /* 320 (1.25x) */
static const __u32 bbr_full_bw_cnt    = 3U;

struct bpf_bbr3_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
	__u32 full_bw;
	__u32 full_bw_cnt;
	__u32 mode;
	__u32 pacing_gain;
	__u32 cwnd_gain;
	__u8  round_start;
	__u8  full_bw_reached;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->round_cnt++;
		ca->round_start = 1;
	}

	bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw) {
		if (bw > ca->max_bw)
			ca->max_bw = bw;
	}
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	/*
	 * TODO 1: Detect when STARTUP has filled the pipe.
	 * - Return early if ca->full_bw_reached, !ca->round_start, or rs->is_app_limited.
	 * - Compute bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE.
	 * - If ca->max_bw >= bw_thresh:
	 *     update ca->full_bw = ca->max_bw, reset ca->full_bw_cnt = 0, and return.
	 * - Otherwise increment ca->full_bw_cnt, and when it reaches bbr_full_bw_cnt (3),
	 *     set ca->full_bw_reached = 1.
	 */
	(void)sk;
	(void)rs;
	(void)bbr_full_bw_thresh;
	(void)bbr_full_bw_cnt;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	/*
	 * TODO 2: Transition through BBR_DRAIN to BBR_PROBE_BW.
	 * - If ca->mode == BBR_STARTUP and ca->full_bw_reached:
	 *     set ca->mode = BBR_DRAIN and tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT).
	 * - If ca->mode == BBR_DRAIN and tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT):
	 *     set ca->mode = BBR_PROBE_BW.
	 */
	(void)sk;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	/*
	 * TODO 3: Set ca->pacing_gain and ca->cwnd_gain based on ca->mode:
	 * - BBR_STARTUP:  pacing_gain = bbr_high_gain,  cwnd_gain = bbr_high_gain
	 * - BBR_DRAIN:    pacing_gain = bbr_drain_gain, cwnd_gain = bbr_high_gain
	 * - BBR_PROBE_BW: pacing_gain = BBR_UNIT,       cwnd_gain = bbr_cwnd_gain
	 */
	(void)bbr_drain_gain;
	(void)bbr_cwnd_gain;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked,
					 __u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		return;

	target_cwnd = bbr_bdp(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < 10U)
		cwnd = cwnd + acked;

	cwnd = max(cwnd, BBR_CWND_MIN_TARGET);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
	ca->round_start = 0;
	ca->full_bw_reached = 0;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us > 0 &&
	    (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	bbr_update_bw(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_undo_cwnd, struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr3 = {
	.name		= "bpf_bbr3",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr3_init,
	.cong_control	= (void *)bpf_bbr3_cong_control,
	.ssthresh	= (void *)bpf_bbr3_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr3_undo_cwnd,
};
```


### `labs/bbr/step03-startup-drain/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 3: BBR_STARTUP to BBR_DRAIN / BBR_PROBE_BW transition after 3 plateau rounds

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr3`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial 100 ms RTT so initial pacing rate is ~141 KB/s
// MSS = 1000 bytes, receive window = 1,000 bytes (1 segment per RTT -> BtlBw = 100 KB/s at 10 ms RTT)
+0.00 < S 0:0(0) win 1000 <mss 1000,sackOK,nop,nop>
+0.00 > S. 0:0(0) ack 1 <...>
+0.10 < . 1:1(0) ack 1 win 1000
+0.00 accept(3, ..., ...) = 4

// Queue 5,000 bytes (5 segments) in one write() so the write queue stays
// non-empty across rounds (avoiding app-limited rate samples after warmup).
+0.00 write(4, ..., 5000) = 5000

// Warmup round (1:1001): clears initial app_limited state upon ACK at +0.01
+0.00 > P. 1:1001(1000) ack 1
+0.01 < . 1:1(0) ack 1001 win 1000

// Round 1 (1001:2001): non-app-limited, sets full_bw = BtlBw (~100 KB/s), full_bw_cnt = 0
// In BBR_STARTUP, pacing_gain = bbr_high_gain (~2.89x) -> tcpi_pacing_rate ~= 286 KB/s
+0.00 > P. 1001:2001(1000) ack 1
+0.01 < . 1:1(0) ack 2001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 1 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
assert tcpi_snd_ssthresh == 0x7fffffff, f"Round 1 STARTUP: expected infinite ssthresh, got {tcpi_snd_ssthresh}"
}%

// Round 2 (2001:3001): 0% bw growth (< 25%) -> full_bw_cnt = 1 (still in BBR_STARTUP)
+0.00 > P. 2001:3001(1000) ack 1
+0.01 < . 1:1(0) ack 3001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 2 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 3 (3001:4001): 0% bw growth (< 25%) -> full_bw_cnt = 2 (still in BBR_STARTUP)
+0.00 > P. 3001:4001(1000) ack 1
+0.01 < . 1:1(0) ack 4001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 3 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 4 (4001:5001): 0% bw growth (< 25%) -> full_bw_cnt = 3 -> full_bw_reached = 1!
// Transitions BBR_STARTUP -> BBR_DRAIN -> BBR_PROBE_BW (since in_flight == 0 <= BDP).
// Pacing rate drops from ~2.89x BtlBw (~286 KB/s) to ~1.0x BtlBw (~99 KB/s)!
+0.00 > P. 4001:5001(1000) ack 1
+0.01 < . 1:1(0) ack 5001 win 1000
+0.00 %{
assert 90000 < tcpi_pacing_rate < 110000, f"Round 4 exited STARTUP: expected ~99KB/s, got {tcpi_pacing_rate}"
assert 1 <= tcpi_snd_ssthresh <= 4, f"Round 4 exited STARTUP: expected ssthresh in [1, 4], got {tcpi_snd_ssthresh}"
assert tcpi_snd_cwnd == 4, f"Round 4 PROBE_BW: expected cwnd == 4, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step03-startup-drain/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 3: STARTUP to DRAIN and PROBE_BW Transition (Full Pipe Detection)
 *
 * BBR starts in BBR_STARTUP with a high pacing/cwnd gain (2/ln(2) ~= 2.89x) to
 * double its sending rate each round and quickly discover bottleneck bandwidth.
 *
 * When estimated bottleneck bandwidth (max_bw) stops growing by at least 25%
 * (bbr_full_bw_thresh = 1.25x) for 3 consecutive non-app-limited rounds
 * (bbr_full_bw_cnt = 3), BBR estimates that the pipe is full:
 * 1. Transition from BBR_STARTUP to BBR_DRAIN (pacing_gain = 1/high_gain ~= 0.35x,
 *    cwnd_gain = high_gain ~= 2.89x) to drain the queue created during STARTUP.
 * 2. Once packets in flight fall to <= 1.0x BDP, transition from BBR_DRAIN to
 *    BBR_PROBE_BW (pacing_gain = 1.0x, cwnd_gain = 2.0x).
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1U << BW_SCALE)
#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET	4U
#define BBR_PACING_MARGIN_PCT	1U

enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* steady-state: pace around estimated bw */
};

static const __u32 bbr_high_gain      = BBR_UNIT * 2885 / 1000 + 1; /* 739 (~2.89x) */
static const __u32 bbr_drain_gain     = BBR_UNIT * 1000 / 2885;     /* 88 (~0.35x) */
static const __u32 bbr_cwnd_gain      = BBR_UNIT * 2;               /* 512 (2.0x) */
static const __u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;           /* 320 (1.25x) */
static const __u32 bbr_full_bw_cnt    = 3U;

struct bpf_bbr3_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
	__u32 full_bw;
	__u32 full_bw_cnt;
	__u32 mode;
	__u32 pacing_gain;
	__u32 cwnd_gain;
	__u8  round_start;
	__u8  full_bw_reached;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->round_cnt++;
		ca->round_start = 1;
	}

	bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw) {
		if (bw > ca->max_bw)
			ca->max_bw = bw;
	}
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT))
		ca->mode = BBR_PROBE_BW;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked,
					 __u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		return;

	target_cwnd = bbr_bdp(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < 10U)
		cwnd = cwnd + acked;

	cwnd = max(cwnd, BBR_CWND_MIN_TARGET);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
	ca->round_start = 0;
	ca->full_bw_reached = 0;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us > 0 &&
	    (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	bbr_update_bw(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_undo_cwnd, struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr3 = {
	.name		= "bpf_bbr3",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr3_init,
	.cong_control	= (void *)bpf_bbr3_cong_control,
	.ssthresh	= (void *)bpf_bbr3_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr3_undo_cwnd,
};
```

</details>

---

## Step 4: PROBE_BW 8-Phase Gain Cycle (Bandwidth Probing and Queue Draining)

### Goal

In this step, you will implement the steady-state **PROBE_BW** mode's 8-phase pacing gain cycle (`bbr_pacing_gain[]`) and the phase transition logic.

- Understand the structure and purpose of the 8-phase gain cycle (`1.25x` → `0.75x` → `1.0x` × 6) used to probe for newly available bandwidth while preventing persistent bufferbloat.
- Implement `bbr_is_next_cycle_phase()` and understand how transition conditions differ between cruise phases, probe phases, and drain phases (minimum RTT elapsed, target in-flight reached, packet loss, and queue drainage).
- Implement `bbr_advance_cycle_phase()` and `bbr_update_cycle_phase()` to increment the cycle index (`ca->cycle_idx`) and timestamp each phase boundary (`ca->cycle_mstamp`).
- Update `bbr_update_gains()` to look up `bbr_pacing_gain[]` using `ca->cycle_idx`.

---

### Design Checkpoints (Think before coding)

Before inspecting `solution.bpf.c`, think through the following four design questions.

#### Q1. Why does BBR cycle through an 8-phase gain pattern (1.25x → 0.75x → 1.0x × 6) instead of pacing at a constant 1.0x?

Once `BBR_STARTUP` and `BBR_DRAIN` have established the estimated bottleneck bandwidth (`ca->max_bw`) and minimum RTT (`ca->min_rtt_us`), why shouldn't BBR simply cruise at `1.0x` indefinitely?

<details>
<summary>Hint and Answer</summary>

- **Available network bandwidth changes dynamically over time**.
- If competing flows terminate or network routing shifts, spare bandwidth becomes available. A sender pacing strictly at its current `1.0x` estimate would never discover that extra bandwidth.
- BBR therefore periodically (roughly every 8 RTTs) increases its pacing rate to `1.25x` (+25%) during **Phase 0 (Probe)** to probe for higher capacity.
- If there is no extra bandwidth, the 25% surplus traffic creates a queue of about `0.25x BDP` at the bottleneck router. To prevent this queue from persisting and causing bufferbloat, BBR drops its pacing rate to `0.75x` (-25%) in **Phase 1 (Drain)** to quickly drain the newly created queue.
- It then cruises at `1.0x` for 6 rounds (**Phases 2..7 (Cruise)**) to maintain high throughput and low queuing delay.

</details>

#### Q2. What conditions are required to exit Phase 0 (Probe, 1.25x) and enter Phase 1 (Drain)?

When deciding whether Phase 0 (`ca->pacing_gain > BBR_UNIT`) is complete, why must BBR check in-flight packet volume (`rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)`) or packet loss (`rs->losses`) in addition to verifying that 1 RTT (`min_rtt_us`) has elapsed?

<details>
<summary>Hint and Answer</summary>

- If the sender was application-limited (scarcity of data in the socket write buffer) or receive-window-limited, it might not have actually pushed `1.25x BDP` of packets into the network despite 1 RTT having passed.
- Exiting Phase 0 prematurely would abort the probe before testing whether the network can absorb the extra bandwidth.
- Thus, Phase 0 continues until at least one minimum RTT has elapsed (`is_full_length`) AND either the pipe has been filled to `1.25x BDP` (`rs->prior_in_flight >= bbr_inflight(1.25x)`) or packet loss occurs (`rs->losses`), signalling buffer overflow.

</details>

#### Q3. Why is Phase 1 (Drain, 0.75x) allowed to exit before a full RTT has elapsed (OR condition)?

Unlike Phase 0, Phase 1 (`ca->pacing_gain < BBR_UNIT`) exits when `is_full_length || rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)`. Why is an early exit permitted here?

<details>
<summary>Hint and Answer</summary>

- The sole purpose of Phase 1 is to drain the excess queue created during Phase 0.
- When an ACK arrives indicating that packets in flight have fallen to or below `1.0x BDP` (`bbr_inflight(sk, ca->max_bw, BBR_UNIT)`), the bottleneck queue has been completely emptied.
- Continuing to send at `0.75x` after the queue is drained would unnecessarily underutilise the link. BBR therefore transitions immediately to Phase 2 (`1.0x` cruise) as soon as in-flight reaches BDP.

</details>

#### Q4. Why does full BBR choose a random initial `cycle_idx`, whereas this lab starts at 0? What is the purpose of randomisation?

When transitioning from `BBR_DRAIN` to `BBR_PROBE_BW`, the upstream Linux BBR implementation selects a random starting index among the cruise phases (phases 2 to 7). Why is this random offset necessary?

<details>
<summary>Hint and Answer</summary>

- **To prevent synchronisation among competing BBR flows**.
- If multiple BBR connections sharing a bottleneck were to enter Phase 0 (`1.25x`) at the exact same moment, all flows would simultaneously inject 25% extra traffic, inducing large queue spikes and heavy loss.
- By randomising each flow's starting phase offset, probe phases are desynchronised. Flows take turns probing the bottleneck at different times, yielding steady aggregate link utilisation and predictable delay.
- In this lab, to ensure deterministic, reproducible packetdrill assertions (verifying Phase 0 → Phase 1 → Phase 2 transitions), we deliberately initialise `ca->cycle_idx = 0`.

</details>

---

### Step-by-step Walkthrough

#### 1. The `bbr_pacing_gain[]` Array

In steady state (`BBR_PROBE_BW`), BBR cycles through an 8-entry pacing gain table:

```c
##define CYCLE_LEN 8

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4, /* Phase 0: 1.25x (Probe) */
	BBR_UNIT * 3 / 4, /* Phase 1: 0.75x (Drain) */
	BBR_UNIT,         /* Phase 2: 1.0x (Cruise) */
	BBR_UNIT,         /* Phase 3: 1.0x (Cruise) */
	BBR_UNIT,         /* Phase 4: 1.0x (Cruise) */
	BBR_UNIT,         /* Phase 5: 1.0x (Cruise) */
	BBR_UNIT,         /* Phase 6: 1.0x (Cruise) */
	BBR_UNIT,         /* Phase 7: 1.0x (Cruise) */
};
```

- **Phase 0 (1.25x)**: Increases the sending rate by 25% to probe for additional available bandwidth.
- **Phase 1 (0.75x)**: Decreases the sending rate by 25% to drain any bottleneck queue created during Phase 0.
- **Phases 2..7 (1.0x)**: Cruises at the estimated bottleneck rate for 6 rounds without building queues.

#### 2. `bbr_is_next_cycle_phase()` — Evaluating Phase Completion

Determines whether the current phase has completed:

```c
static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length = (tp->tcp_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;

	if (ca->pacing_gain == BBR_UNIT)
		return is_full_length;

	if (ca->pacing_gain > BBR_UNIT)
		return is_full_length &&
			(rs->losses ||
			 rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain));

	return is_full_length ||
		rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT);
}
```

1. **Duration check (`is_full_length`)**: Evaluates whether elapsed time since `ca->cycle_mstamp` exceeds `ca->min_rtt_us`.
2. **Cruise phases (`ca->pacing_gain == BBR_UNIT`)**: Returns true as soon as 1 RTT has elapsed (`is_full_length`).
3. **Probe phase (`ca->pacing_gain > BBR_UNIT`)**: Returns true only when 1 RTT has elapsed AND either packet losses occurred (`rs->losses`) or in-flight data reached `1.25x BDP`.
4. **Drain phase (`ca->pacing_gain < BBR_UNIT`)**: Returns true if 1 RTT has elapsed OR in-flight data has dropped to or below `1.0x BDP` (queue drained).

#### 3. `bbr_advance_cycle_phase()` and `bbr_update_cycle_phase()` — Advancing Phases

```c
static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->tcp_mstamp;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
		bbr_advance_cycle_phase(sk);
}
```

- `bbr_advance_cycle_phase()` increments `ca->cycle_idx` modulo 8 (`& (CYCLE_LEN - 1)`) and records `tp->tcp_mstamp` in `ca->cycle_mstamp`.
- `bbr_update_cycle_phase()` triggers the transition whenever `ca->mode == BBR_PROBE_BW` and `bbr_is_next_cycle_phase()` evaluates to true.

#### 4. Updating Gains in `bbr_update_gains()`

```c
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
```

In `BBR_PROBE_BW`, `ca->pacing_gain` is looked up from `bbr_pacing_gain[]` using `ca->cycle_idx & 7`. `ca->cwnd_gain` remains fixed at `bbr_cwnd_gain` (`2.0x`).

---

### Understanding `test.pkt`

[test.pkt](test.pkt) verifies the transitions from Phase 0 (Probe: `1.25x`) → Phase 1 (Drain: `0.75x`) → Phase 2 (Cruise: `1.0x`):

1. **Exiting STARTUP and Entering PROBE_BW Phase 0 (Round 4)**:
   - After 4 plateau rounds, `full_bw_reached` triggers `BBR_STARTUP` → `BBR_DRAIN` → `BBR_PROBE_BW` (`ca->cycle_idx = 0`).
   - Phase 0 gain is `5/4` (`1.25x`). With a 100 KB/s bottleneck and 1% pacing margin, `tcpi_pacing_rate` is `~123 KB/s` (`115000 < tcpi_pacing_rate < 135000`).
   - The receive window re-opens to 2,000 bytes (2 segments) to allow probing.
2. **Transition from Phase 0 to Phase 1**:
   - Two segments (`6001:7001` and `7001:8001`) are sent 10 ms apart, reaching an in-flight count of 2 (`prior_in_flight >= bbr_inflight(5/4) == 2`).
   - An ACK arriving after 20 ms (`> min_rtt_us = 10 ms`) satisfies both `is_full_length` and the in-flight condition, advancing to Phase 1 (Drain).
   - Phase 1 gain is `3/4` (`0.75x`), and `tcpi_pacing_rate` drops to `~74 KB/s` (`65000 < tcpi_pacing_rate < 85000`).
3. **Transition from Phase 1 to Phase 2**:
   - One segment (`8001:9001`) is transmitted, and ACKed after 15 ms (`> min_rtt_us = 10 ms` and `prior_in_flight == 1 <= BDP`).
   - With the queue drained, BBR advances to Phase 2 (Cruise, `cycle_idx = 2`).
   - Phase 2 gain is `1.0x`, and `tcpi_pacing_rate` returns to `~99 KB/s` (`90000 < tcpi_pacing_rate < 110000`).

---

### How to run

```sh
## 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step04-probe-bw starter
## 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step04-probe-bw solution
```


### `labs/bbr/step04-probe-bw/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 4: PROBE_BW phase cycling (8-phase gain cycle)
 *
 * Steady-state bandwidth probing using an 8-phase gain cycle:
 *   Phase 0: 1.25x (probe for more available bandwidth)
 *   Phase 1: 0.75x (drain excess queue created in phase 0)
 *   Phases 2..7: 1.0x (cruise at estimated bandwidth)
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE			24
#define BW_UNIT				(1ULL << BW_SCALE)

#define BBR_SCALE			8
#define BBR_UNIT			(1 << BBR_SCALE)

#define CYCLE_LEN			8
#define TCP_INIT_CWND			10

static const int bbr_pacing_margin_percent = 1;
static const int bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain = BBR_UNIT * 2;
static const int bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const int bbr_full_bw_cnt = 3;
static const int bbr_cwnd_min_target = 4;

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
};

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
};

struct bbr_ca {
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT seen */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	return bbr_bdp(sk, bw, gain);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	/*
	 * TODO 1: PROBE_BW のフェーズ終了条件を判定する
	 *
	 * 1. フェーズ開始からの経過時間 (tp->tcp_mstamp - ca->cycle_mstamp) が
	 *    ca->min_rtt_us を超えているか (is_full_length) を判定する。
	 * 2. ca->pacing_gain == BBR_UNIT (巡航フェーズ 2..7) の場合:
	 *    is_full_length を返す (経過時間のみで判定)。
	 * 3. ca->pacing_gain > BBR_UNIT (探索フェーズ 0, 1.25x) の場合:
	 *    is_full_length かつ、ロス発生 (rs->losses) または
	 *    rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)
	 *    に達していれば true を返す。
	 * 4. ca->pacing_gain < BBR_UNIT (排出フェーズ 1, 0.75x) の場合:
	 *    is_full_length または、キューが抜けて
	 *    rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)
	 *    に下がっていれば true を返す。
	 */
	(void)sk;
	(void)rs;
	return false;
}

static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	/*
	 * TODO 2: 次のフェーズへ進める
	 *
	 * 1. ca->cycle_idx を 1 つ進め、CYCLE_LEN (8) でラップさせる (& (CYCLE_LEN - 1))。
	 * 2. ca->cycle_mstamp に現在のタイムスタンプ tp->tcp_mstamp を記録する。
	 */
	(void)sk;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	/*
	 * TODO 3: PROBE_BW モードにおけるフェーズ更新
	 *
	 * ca->mode == BBR_PROBE_BW かつ bbr_is_next_cycle_phase(sk, rs) が真なら
	 * bbr_advance_cycle_phase(sk) を呼び出す。
	 */
	(void)sk;
	(void)rs;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->tcp_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rs->rtt_us;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		/*
		 * TODO 4: PROBE_BW での pacing_gain を bbr_pacing_gain[] から設定する
		 *
		 * ca->cycle_idx (0..7) に応じて bbr_pacing_gain[ca->cycle_idx & 7] を設定する。
		 * starter では暫定的にフラットな BBR_UNIT (1.0x) を設定している。
		 */
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, (__u32)bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_main, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_sndbuf_expand, struct sock *sk)
{
	return 3;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr4 = {
	.init		= (void *)bpf_bbr4_init,
	.cong_control	= (void *)bpf_bbr4_main,
	.sndbuf_expand	= (void *)bpf_bbr4_sndbuf_expand,
	.undo_cwnd	= (void *)bpf_bbr4_undo_cwnd,
	.ssthresh	= (void *)bpf_bbr4_ssthresh,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr4",
};
```


### `labs/bbr/step04-probe-bw/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 4: PROBE_BW 8-phase gain cycle (5/4 -> 3/4 -> 1.0)

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr4`

// Server socket setup
+0.00 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.00 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.00 bind(3, ..., ...) = 0
+0.00 listen(3, 1) = 0

// 3-way handshake with initial 100 ms RTT and win 2000 on the ACK so
// tp->max_window = 2000 and tp->mss_cache = 1000 from the start.
+0.00 < S 0:0(0) win 2000 <mss 1000,sackOK,nop,nop>
+0.00 > S. 0:0(0) ack 1 <...>
+0.10 < . 1:1(0) ack 1 win 2000
+0.00 accept(3, ..., ...) = 4

// Send 1 segment (1:1001) and ACK with win 1000 to set snd_wnd = 1000
// (1 segment per 10 ms RTT -> BtlBw = 100 KB/s) while keeping max_window = 2000.
+0.00 write(4, ..., 1000) = 1000
+0.00 > P. 1:1001(1000) ack 1
+0.01 < . 1:1(0) ack 1001 win 1000

// Queue 5,000 bytes (5 segments) in one write() so the write queue stays
// non-empty across STARTUP rounds (avoiding app-limited rate samples after warmup).
+0.01 write(4, ..., 5000) = 5000

// Warmup round (1001:2001): clears app_limited state
+0.00 > . 1001:2001(1000) ack 1
+0.01 < . 1:1(0) ack 2001 win 1000

// Round 1 (2001:3001): non-app-limited, sets full_bw = BtlBw (~100 KB/s), full_bw_cnt = 0
+0.00 > . 2001:3001(1000) ack 1
+0.01 < . 1:1(0) ack 3001 win 1000
+0.00 %{
assert 250000 < tcpi_pacing_rate < 320000, f"Round 1 STARTUP: expected ~286KB/s, got {tcpi_pacing_rate}"
}%

// Round 2 (3001:4001): 0% bw growth (< 25%) -> full_bw_cnt = 1
+0.00 > . 3001:4001(1000) ack 1
+0.01 < . 1:1(0) ack 4001 win 1000

// Round 3 (4001:5001): 0% bw growth (< 25%) -> full_bw_cnt = 2
+0.00 > . 4001:5001(1000) ack 1
+0.01 < . 1:1(0) ack 5001 win 1000

// Round 4 (5001:6001): 0% bw growth (< 25%) -> full_bw_cnt = 3 -> full_bw_reached = 1!
// Transitions BBR_STARTUP -> BBR_DRAIN -> BBR_PROBE_BW (cycle_idx = 0, pacing_gain = 5/4).
// Pacing rate is 1.25x BtlBw (* 0.99 margin) ~= 123 KB/s!
// Open receive window back to 2000 bytes (2 segments) for Phase 0 probing.
+0.00 > P. 5001:6001(1000) ack 1
+0.01 < . 1:1(0) ack 6001 win 2000
+0.00 %{
assert 115000 < tcpi_pacing_rate < 135000, f"PROBE_BW Phase 0 (5/4 gain): expected ~123KB/s, got {tcpi_pacing_rate}"
assert tcpi_snd_cwnd == 4, f"PROBE_BW Phase 0: expected cwnd == 4, got {tcpi_snd_cwnd}"
}%

// Phase 0 -> Phase 1 transition:
// Send 2 segments spaced by 10 ms so prior_in_flight == 2 >= bbr_inflight(5/4) == 2,
// and ACK after 10 ms (total 2 segments over 20 ms = 100 KB/s, elapsed > min_rtt_us).
// Transitions to Phase 1 (cycle_idx = 1, pacing_gain = 3/4 -> ~74 KB/s)!
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 6001:7001(1000) ack 1
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 7001:8001(1000) ack 1
+0.01 < . 1:1(0) ack 8001 win 2000
+0.00 %{
assert 65000 < tcpi_pacing_rate < 85000, f"PROBE_BW Phase 1 (3/4 gain): expected ~74KB/s, got {tcpi_pacing_rate}"
}%

// Phase 1 -> Phase 2 transition:
// Send 1 segment and ACK after 15 ms (> min_rtt_us = 10 ms, prior_in_flight == 1 <= BDP).
// Transitions to Phase 2 (cycle_idx = 2, pacing_gain = 1.0 -> ~99 KB/s)!
+0.01 write(4, ..., 1000) = 1000
+0.00 > P. 8001:9001(1000) ack 1
+0.015 < . 1:1(0) ack 9001 win 2000
+0.00 %{
assert 90000 < tcpi_pacing_rate < 110000, f"PROBE_BW Phase 2 (1.0 gain): expected ~99KB/s, got {tcpi_pacing_rate}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step04-probe-bw/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 4: PROBE_BW phase cycling (8-phase gain cycle)
 *
 * Steady-state bandwidth probing using an 8-phase gain cycle:
 *   Phase 0: 1.25x (probe for more available bandwidth)
 *   Phase 1: 0.75x (drain excess queue created in phase 0)
 *   Phases 2..7: 1.0x (cruise at estimated bandwidth)
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE			24
#define BW_UNIT				(1ULL << BW_SCALE)

#define BBR_SCALE			8
#define BBR_UNIT			(1 << BBR_SCALE)

#define CYCLE_LEN			8
#define TCP_INIT_CWND			10

static const int bbr_pacing_margin_percent = 1;
static const int bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain = BBR_UNIT * 2;
static const int bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const int bbr_full_bw_cnt = 3;
static const int bbr_cwnd_min_target = 4;

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
};

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
};

struct bbr_ca {
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT seen */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	return bbr_bdp(sk, bw, gain);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length = (tp->tcp_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;

	if (ca->pacing_gain == BBR_UNIT)
		return is_full_length;

	if (ca->pacing_gain > BBR_UNIT)
		return is_full_length &&
			(rs->losses ||
			 rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain));

	return is_full_length ||
		rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT);
}

static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->tcp_mstamp;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
		bbr_advance_cycle_phase(sk);
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->tcp_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rs->rtt_us;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, (__u32)bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_main, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);


}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_sndbuf_expand, struct sock *sk)
{
	return 3;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr4 = {
	.init		= (void *)bpf_bbr4_init,
	.cong_control	= (void *)bpf_bbr4_main,
	.sndbuf_expand	= (void *)bpf_bbr4_sndbuf_expand,
	.undo_cwnd	= (void *)bpf_bbr4_undo_cwnd,
	.ssthresh	= (void *)bpf_bbr4_ssthresh,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr4",
};
```

</details>

---

## BBR Step 5: Transition to PROBE_RTT & Queue Draining (cwnd = 4)

### Learning Goals

- Understand the necessity and control flow of why BBR periodically transitions to `BBR_PROBE_RTT` to clamp inflight down to the minimum pipe size (`BBR_MIN_PIPE_CWND = 4`).
- Learn how BBR resolves the standing queue problem in bottleneck buffers and avoids stale two-way propagation delay estimates (`min_rtt_us` / RTprop).
- Implement the filter window expiration logic governed by `BBR_MIN_RTT_WIN_US` (standard 10 seconds in the kernel, shortened to 2 seconds in this lab).
- Implement the two completion criteria in BPF for `BBR_PROBE_RTT` (spending at least 200 ms and completing 1 packet-timed round trip with inflight <= 4).
- Understand how `ca->prior_cwnd` saves the pre-probe window, restores it upon exit, and how `bbr_set_cwnd()` safely enforces the window clamping.

---

### Design Checkpoints (Think Before Looking at the Code)

#### Q1. Why can a flow in `BBR_PROBE_BW` get stuck with an inflated `min_rtt_us`?
In `BBR_PROBE_BW`, the pacing gain cycle (1.25 to probe bandwidth, 0.75 to drain excess queue, and 1.0 for the remaining 6 phases) attempts to clear transient queues created during probing. However, if other flows join or path routing changes, why can the steady-state `BBR_PROBE_BW` mode fail to re-measure the true propagation delay (RTprop)?

<details>
<summary>Hint & Answer</summary>

**Hint**:
Phase 1 of `BBR_PROBE_BW` (pacing gain 0.75) only drains the extra queue that the flow itself created in phase 0 (0.25 × BDP). If competing flows create queues at the bottleneck, dropping inflight only down to 1.0 × BDP will never drain the bottleneck queue completely to zero packets.

**Answer**:
Because `BBR_PROBE_BW` maintains inflight at or above 1.0 × BDP at all times, any standing queue persisting at the bottleneck will cause every packet to suffer queueing delay. Every RTT sample collected will reflect `true RTprop + queue delay`.
When multiple flows share the bottleneck, no single flow ever observes an empty queue during normal operation. Therefore, a dedicated mode (`BBR_PROBE_RTT`) is essential: all flows must periodically and autonomously cut their inflight to the absolute minimum, allowing the bottleneck queue to drain completely so the true physical RTprop can be measured.
</details>

---

#### Q2. What state fields are required to track the `min_rtt` window and the `BBR_PROBE_RTT` hold timer?
What variables in `struct bbr_ca` are needed to track the min RTT timestamp and manage the lifecycle of `BBR_PROBE_RTT`?

<details>
<summary>Hint & Answer</summary>

**Hint**:
- The timestamp when the minimum RTT was last updated
- The deadline timestamp indicating when the flow is allowed to exit `BBR_PROBE_RTT`
- A flag recording whether at least one round trip has completed with inflight <= 4
- A storage variable to remember the operating cwnd before entering `BBR_PROBE_RTT`

**Answer**:
The following state variables are used:
- `ca->min_rtt_stamp`: The timestamp (`tp->tcp_mstamp`) when `min_rtt_us` was last updated. When `tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US`, the filter window expires.
- `ca->probe_rtt_done_stamp`: The timestamp (usec) when `BBR_PROBE_RTT` mode may finish. It is initialised to `0` upon entry, and armed to `tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` once inflight falls to `BBR_MIN_PIPE_CWND` or below.
- `ca->probe_rtt_round_done`: A boolean flag tracking whether 1 packet-timed round trip has completed while maintaining inflight <= 4.
- `ca->prior_cwnd`: The saved `tp->snd_cwnd` prior to entering `BBR_PROBE_RTT`, used to restore the transmission scale upon exit.
</details>

---

#### Q3. Why does `BBR_PROBE_RTT` require BOTH 200 ms AND at least 1 packet-timed round with `inflight <= 4`?
Why is a simple 200 ms wall-clock timer insufficient? And why must the timer start only after inflight drops to 4 packets or fewer?

<details>
<summary>Hint & Answer</summary>

**Hint**:
When `snd_cwnd` is suddenly reduced to 4, packets already in flight (tens or hundreds of packets) do not instantly vanish. Furthermore, on high-latency paths (e.g. satellite links with RTT = 500 ms), 200 ms is shorter than a single round trip.

**Answer**:
1. **Draining existing packets in flight**: Immediately after setting cwnd to 4, packets already in flight are still traversing the network and traversing the bottleneck queue. The queue only becomes truly drained after inflight actually drops to `BBR_MIN_PIPE_CWND` (4). Hence, the 200 ms hold timer must only start once `tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`.
2. **Handling high-latency paths**: On paths where the RTT exceeds 200 ms, a pure 200 ms timer would expire before a single packet sent at the 4-packet limit could complete a round trip and return an ACK. Without completing a round trip at the minimum inflight level, the flow would never measure an RTT sample corresponding to the drained queue state.
Therefore, BBR requires `max(200 ms, 1 round)`, meaning both conditions (at least 200 ms elapsed and `probe_rtt_round_done == 1`) must be satisfied before exiting `BBR_PROBE_RTT`.
</details>

---

#### Q4. Why must `bbr_save_cwnd()` preserve `prior_cwnd` and why does `bbr_set_cwnd()` apply the cap at the end?
Consider how `snd_cwnd` is manipulated during `BBR_PROBE_RTT` and how it is restored upon exit. Why is `BBR_MIN_PIPE_CWND` applied at the very end of `bbr_set_cwnd()` rather than replacing normal calculations?

<details>
<summary>Hint & Answer</summary>

**Hint**:
The regular cwnd calculation logic (BDP calculation, slow-start additions, socket window clamp) still executes upon ACK reception, but during `BBR_PROBE_RTT` the output must be capped. When exiting, the flow needs to resume transmission immediately at its full capacity without restarting slow start.

**Answer**:
- **`bbr_save_cwnd()`**: Saves the operating `tp->snd_cwnd` into `ca->prior_cwnd` right before entering `BBR_PROBE_RTT`. When exiting after 200 ms, `tp->snd_cwnd` is immediately restored to `max(tp->snd_cwnd, ca->prior_cwnd)`, avoiding any throughput penalty from a slow-start ramp-up.
- **Clamping at the end**: In `bbr_set_cwnd()`, normal ACK processing computes the target cwnd based on estimated BDP and state. At the very end of the function (after the `done:` label), if `ca->mode == BBR_PROBE_RTT`, the code enforces `tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND)`. This ensures that regular cwnd bookkeeping remains intact whilst strictly clamping the transmission window to 4 packets.
</details>

---

### Step-by-step Walkthrough

#### 1. Mathematical Design and Constants

In Linux kernel `net/ipv4/tcp_bbr.c`, the relevant parameters are configured as:

- `bbr_min_rtt_win_sec = 10` (10 seconds)
- `bbr_probe_rtt_mode_ms = 200` (200 milliseconds)
- `bbr_cwnd_min_target = 4` (4 packets)

**Bounded 2% Throughput Penalty**:
In steady state, a BBR flow enters `BBR_PROBE_RTT` for 200 ms every 10 seconds:
$$
\frac{200\text{ ms}}{10\text{ s}} = \frac{0.2\text{ s}}{10\text{ s}} = 2\%
$$
This bounds the theoretical throughput penalty to roughly 2% of total transfer time, while ensuring periodic complete drainage of bottleneck queues to re-estimate true physical RTprop.

> [!NOTE]
> In this lab, to allow `packetdrill` tests to execute in ~2.5 seconds instead of waiting 10 seconds, `BBR_MIN_RTT_WIN_US` is set to `2 * USEC_PER_SEC` (2 seconds). The state transitions, timers, and invariants are identical to the kernel implementation.

Why is `BBR_MIN_PIPE_CWND` set to **4 packets**?
TCP receivers frequently use delayed ACKs, sending one ACK for every two packets received. If cwnd were reduced to 1 or 2 packets, the receiver would wait for its delayed ACK timer (up to 40–200 ms) before responding, causing the pipe to stall. With 4 packets in flight, at least two ACK triggers are generated, ensuring continuous ACK clocking without triggering delayed ACK timeouts.

---

#### 2. `bbr_update_min_rtt()`: Min RTT Tracking & PROBE_RTT Control

```c
static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	// 1. Check whether 2 seconds (10s in kernel) have elapsed since last min_rtt update
	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;

	// 2. Update min_rtt if a lower sample arrived or if the window has expired
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	// 3. Enter BBR_PROBE_RTT if the window expired and not already in this mode
	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	// 4. Manage the PROBE_RTT hold duration and exit criteria
	if (ca->mode == BBR_PROBE_RTT) {
		// Mark app_limited so low throughput during cwnd=4 does not corrupt max_bw
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;

		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			// Arm the 200 ms timer only after inflight drops to 4 packets or fewer
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}
```

- **Strict inequality `<`**: Using strict inequality `(__u32)rs->rtt_us < ca->min_rtt_us` is critical. If `<=` were used, repeated samples rounded to the same value (e.g. from millisecond TCP timestamps during loss recovery) would continuously refresh `min_rtt_stamp`, preventing the filter window from ever expiring.
- **Ignoring delayed ACKs**: When the filter expires, `!rs->is_ack_delayed` prevents adopting an artificially inflated RTT sample caused by receiver delayed ACK timers.
- **`tp->app_limited` marking**: While inflight is clamped to 4, rate samples will be very small. Marking the socket app-limited ensures that these artificially small samples do not degrade `ca->max_bw`.

---

#### 3. `bbr_check_probe_rtt_done()` and Mode Restoration

```c
static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}
```

- `tp->tcp_mstamp > ca->probe_rtt_done_stamp`: Confirms that the 200 ms minimum hold duration has elapsed.
- `ca->min_rtt_stamp = tp->tcp_mstamp`: Resets the filter timestamp so that even if no new minimum RTT was discovered (for instance if competing traffic filled the buffer), the flow will not immediately re-enter `BBR_PROBE_RTT`.
- `tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)`: Restores `snd_cwnd` to the saved pre-probe value.
- `bbr_reset_mode()`: Resets the state back to `BBR_PROBE_BW` (cycle index 0) if the pipe was already filled.

---

#### 4. Enforcing Clamping in `bbr_set_cwnd()`

```c
static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	// ... Standard cwnd calculation ...
done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}
```

At the end of cwnd calculation, if `ca->mode == BBR_PROBE_RTT`, `tp->snd_cwnd` is strictly clamped to `BBR_MIN_PIPE_CWND` (4).

---

### Understanding `test.pkt`

`test.pkt` verifies the entire lifecycle in milliseconds:

1. **Connection setup and window growth**:
   - Establishes a connection with base RTT = 10 ms.
   - Sends 10 packets (10,000 bytes) and receives an ACK, growing `snd_cwnd` from 10 to 20.
   - Verified via `assert tcpi_snd_cwnd == 20`.

2. **Advancing 2.1 seconds & filter expiration**:
   - Advances time by `+2.100` seconds (> `BBR_MIN_RTT_WIN_US` = 2 s).
   - Transmits 1 packet and receives an ACK with RTT = 15 ms (> 10 ms).
   - Because `min_rtt` is not refreshed and the filter window has expired, BBR transitions to `BBR_PROBE_RTT`.
   - Verified via `assert tcpi_snd_cwnd == 4`.

3. **Advancing 0.25 seconds & exiting PROBE_RTT**:
   - Advances time by `+0.250` seconds (> `BBR_PROBE_RTT_MODE_US` = 200 ms).
   - Transmits 1 packet and receives an ACK at RTT = 10 ms, completing 1 round trip (`probe_rtt_round_done = 1`).
   - With both the 200 ms duration and 1 round trip satisfied, BBR exits `BBR_PROBE_RTT`.
   - Verified via `assert tcpi_snd_cwnd >= 20`, confirming restoration of `prior_cwnd`.

---

### How to run

```sh
## 1. Verify that starter.bpf.c fails on the assertion
../../run_test.sh bbr/step05-probe-rtt starter
## 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step05-probe-rtt solution
```

- In `starter.bpf.c`, `filter_expired`, `BBR_PROBE_RTT` transition, and `snd_cwnd` clamping are omitted, causing the assertion at 2.1s (`assert tcpi_snd_cwnd == 4`) to fail.
- In `solution.bpf.c`, all `packetdrill` assertions pass and `=== [PASS] bbr/step05-probe-rtt (solution) ===` is printed.


### `labs/bbr/step05-probe-rtt/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 5: Transition to PROBE_RTT & BBR_MIN_PIPE_CWND (cwnd = 4)
 *
 * Periodically drains the bottleneck queue when min_rtt_us has not been
 * refreshed within BBR_MIN_RTT_WIN_US.
 *
 * Note: Linux kernel tcp_bbr.c uses bbr_min_rtt_win_sec = 10 seconds.
 * In this lab we set BBR_MIN_RTT_WIN_US = 2 * USEC_PER_SEC (2 seconds)
 * so packetdrill tests complete in ~2.5 seconds instead of waiting 10s.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

/*
 * Kernel tcp_bbr.c uses 10 seconds (bbr_min_rtt_win_sec = 10).
 * We use 2 seconds in this lab so packetdrill runs quickly.
 */
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	/*
	 * TODO: Implement BBR_PROBE_RTT logic and min_rtt window expiration:
	 * 1. Compute filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US.
	 * 2. Update ca->min_rtt_us and ca->min_rtt_stamp when rs->rtt_us >= 0 and
	 *    either ((__u32)rs->rtt_us < ca->min_rtt_us) or (filter_expired && !rs->is_ack_delayed).
	 * 3. When filter_expired && ca->mode != BBR_PROBE_RTT, save cwnd (bbr_save_cwnd(sk)),
	 *    set ca->mode = BBR_PROBE_RTT, and reset ca->probe_rtt_done_stamp = 0.
	 * 4. While ca->mode == BBR_PROBE_RTT, maintain the probe phase:
	 *    - Set tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
	 *    - If !ca->probe_rtt_done_stamp && tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND:
	 *        ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
	 *        ca->probe_rtt_round_done = 0;
	 *        ca->next_rtt_delivered = tp->delivered;
	 *    - Else if ca->probe_rtt_done_stamp:
	 *        if ca->round_start: ca->probe_rtt_round_done = 1;
	 *        if ca->probe_rtt_round_done && tp->tcp_mstamp > ca->probe_rtt_done_stamp:
	 *            ca->min_rtt_stamp = tp->tcp_mstamp;
	 *            tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	 *            bbr_reset_mode(sk);
	 */
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	/* TODO: If ca->mode == BBR_PROBE_RTT, clamp tp->snd_cwnd to BBR_MIN_PIPE_CWND (4). */
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr5 = {
	.init		= (void *)bpf_bbr5_init,
	.cong_control	= (void *)bpf_bbr5_cong_control,
	.ssthresh	= (void *)bpf_bbr5_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr5_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr5",
};
```


### `labs/bbr/step05-probe-rtt/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 5: BBR_PROBE_RTT transition, clamping cwnd to 4, and restoring prior_cwnd

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr5`

// Establish a connection with base RTT = 10 ms
+0.000 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.000 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.000 bind(3, ..., ...) = 0
+0.000 listen(3, 1) = 0

+0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
+0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
+0.010 < . 1:1(0) ack 1 win 512
+0.000 accept(3, ..., ...) = 4

// Send 10 segments at RTT = 10 ms to grow cwnd from 10 to 20
+0.000 write(4, ..., 10000) = 10000
+0.000 > P. 1:10001(10000) ack 1
+0.010 < . 1:1(0) ack 10001 win 512
+0.000 %{
assert tcpi_snd_cwnd == 20, f"expected cwnd == 20, got {tcpi_snd_cwnd}"
}%

// Advance +2.1s (> BBR_MIN_RTT_WIN_US = 2s) and ACK with RTT = 15 ms (> 10 ms)
// so min_rtt_us is not refreshed early and the filter window expires.
+2.100 write(4, ..., 1000) = 1000
+0.000 > P. 10001:11001(1000) ack 1
+0.015 < . 1:1(0) ack 11001 win 512

// BBR must enter BBR_PROBE_RTT and clamp cwnd to BBR_MIN_PIPE_CWND (4)!
+0.000 %{
assert tcpi_snd_cwnd == 4, f"expected cwnd == 4 in PROBE_RTT, got {tcpi_snd_cwnd}"
}%

// Wait +0.25s (> BBR_PROBE_RTT_MODE_US = 200 ms) with in_flight <= 4
// and complete 1 packet-timed round trip at RTT = 10 ms.
+0.250 write(4, ..., 1000) = 1000
+0.000 > P. 11001:12001(1000) ack 1
+0.010 < . 1:1(0) ack 12001 win 512

// BBR exits BBR_PROBE_RTT and restores cwnd >= prior_cwnd (20)!
+0.000 %{
assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step05-probe-rtt/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 5: Transition to PROBE_RTT & BBR_MIN_PIPE_CWND (cwnd = 4)
 *
 * Periodically drains the bottleneck queue when min_rtt_us has not been
 * refreshed within BBR_MIN_RTT_WIN_US.
 *
 * Note: Linux kernel tcp_bbr.c uses bbr_min_rtt_win_sec = 10 seconds.
 * In this lab we set BBR_MIN_RTT_WIN_US = 2 * USEC_PER_SEC (2 seconds)
 * so packetdrill tests complete in ~2.5 seconds instead of waiting 10s.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

/*
 * Kernel tcp_bbr.c uses 10 seconds (bbr_min_rtt_win_sec = 10).
 * We use 2 seconds in this lab so packetdrill runs quickly.
 */
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr5 = {
	.init		= (void *)bpf_bbr5_init,
	.cong_control	= (void *)bpf_bbr5_cong_control,
	.ssthresh	= (void *)bpf_bbr5_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr5_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr5",
};
```

</details>

---

## Step 6: Saving/Restoring cwnd Across Loss & ACK Aggregation (extra_acked) Compensation

In this step, we implement BBRv1's response to packet loss (packet conservation and `prior_cwnd` restoration) as well as the adaptation mechanism for ACK aggregation commonly observed on Wi-Fi and LRO/GRO networks.

---

### Goal

- **Understanding BBR's loss response vs Reno/CUBIC**: Learn why BBR does not permanently halve `cwnd` or reduce its pacing rate upon packet loss.
- **Packet conservation in the first round of Recovery**: Master the "one packet sent per ACK received" principle to safely drain the pipeline immediately following loss detection.
- **Restoring `prior_cwnd` upon exiting Recovery**: Implement saving and restoring `prior_cwnd` so that BBR recovers its pre-loss window upon returning to `TCP_CA_Open`.
- **ACK aggregation estimation (`extra_acked`)**: Understand how stretch ACKs and aggregation bursts can starve a strict BDP window, and how BBR measures and compensates with `extra_acked`.

---

### Design Checkpoints (Think before coding)

#### Q1. Why does BBR avoid permanently halving its `cwnd` or pacing rate upon packet loss, unlike Reno or CUBIC?
- Hint: How does BBR's explicit physical model (`BtlBw` and `RTprop`) treat packet loss?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Traditional loss-based algorithms like Reno and CUBIC treat packet loss as unambiguous evidence of buffer overflow and congestion, cutting the congestion window by 30% to 50%. However, in modern networks (particularly shallow-buffered switches or wireless links), non-congestive random packet drops and transient queue spikes are frequent.
- BBR explicitly models the physical capacity of the bottleneck (`BtlBw` and `RTprop`). An isolated loss event does not diminish the physical bandwidth or increase the propagation delay of the link. Therefore, BBR does not reduce its estimated bandwidth (`max_bw`) or pacing rate based on loss; it merely subtracts the lost packets from `cwnd` temporarily without penalising subsequent throughput.
</details>

---

#### Q2. Why is packet conservation necessary during the first round of Fast Recovery, and how does `prior_cwnd` restoration work?
- Hint: What happens to in-flight packets immediately after loss, and what would happen if `cwnd` remained deflated after recovery finishes?

<details>
<summary>Hint and Answer</summary>

- **Answer**: When loss is first detected, missing packets exist in the network pipe, and uncoordinated retransmissions or bursts could provoke cascading loss. BBR therefore enforces strict packet conservation during the first round trip of Recovery by constraining `cwnd = tcp_packets_in_flight(tp) + max(acked, 1)`, ensuring the sender injects at most one new packet into the network for every acknowledged packet that leaves.
- Once Fast Recovery terminates and the connection returns to `TCP_CA_Open`, BBR restores the window to `cwnd = max(cwnd, ca->prior_cwnd)`, which was saved prior to entering Recovery. This allows BBR to resume its full BDP target window without an unnecessary additive-increase recovery period.
</details>

---

#### Q3. Why does the `.set_state` callback handle `TCP_CA_Loss` (RTO) but leave `TCP_CA_Recovery` to `.cong_control`?
- Hint: How do Fast Recovery (duplicate ACKs) and RTO timeouts differ in terms of pipeline state and ACK delivery?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Fast Recovery is triggered by incoming duplicate ACKs, meaning that acknowledgements are still actively arriving and the transmission pipeline is functioning. Therefore, state transitions and packet conservation can be cleanly evaluated inside the per-ACK `.cong_control` hook via the `rate_sample`.
- In contrast, an RTO timeout (`TCP_CA_Loss`) indicates that the pipe has stalled completely and no ACKs are arriving. In that scenario, `.cong_control` is not invoked. Instead, the timer path in the kernel invokes `.set_state` directly. When `new_state == TCP_CA_Loss`, BBR resets `ca->full_bw = 0` and sets `ca->round_start = 1` to safely re-initialise the model.
</details>

---

#### Q4. What is ACK aggregation in wireless and GRO environments, and how does BBR calculate and compensate for it with `extra_acked`?
- Hint: What occurs on the sender side when ACKs that were supposed to arrive smoothly are batched together and arrive in sporadic bursts?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Due to Wi-Fi MAC frame aggregation or NIC receive offloads (LRO/GRO), multiple returning TCP ACKs can be coalesced into a single bursty acknowledgement. Between bursts, the sender receives no ACKs, meaning that if `cwnd` were strictly capped at 1× or 2× BDP, the sender would become cwnd-limited before the next batch of ACKs arrives, causing the transmission pipe to drain and degrading throughput.
- To compensate, BBR tracks the elapsed time `epoch_us` in a sampling epoch and calculates the expected number of acknowledged packets: `expected_acked = max_bw * epoch_us`. Any excess packets received above this expected rate (`extra_acked = ack_epoch_acked - expected_acked`) represent aggregation volume. BBR tracks the peak `extra_acked` across a 5-RTT window and adds `min(ca->extra_acked, bdp)` to `target_cwnd`.
</details>

---

### Step-by-step Walkthrough

#### 1. Extending `struct bbr_ca`
We add fields to track the ACK aggregation epoch and recovery state:
```c
struct bbr_ca {
	...
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* packet conservation in 1st round of Recovery */
};
```

#### 2. Preserving Pre-Recovery Window with `bbr_save_cwnd()`
We save `prior_cwnd` whenever the connection is in a healthy state before Recovery or PROBE_RTT:
```c
static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}
```

#### 3. Loss Control in `bbr_set_cwnd_to_recover_or_restore()`
- Subtract losses directly from `cwnd` (`cwnd = max(cwnd - rs->losses, 1)`).
- On transition into `TCP_CA_Recovery`, activate packet conservation and cap `cwnd = tcp_packets_in_flight(tp) + max(acked, 1)`.
- Upon transitioning back out of Recovery, restore `cwnd = max(cwnd, ca->prior_cwnd)`.

#### 4. ACK Aggregation Tracking with `bbr_update_ack_aggregation()`
- On each packet-timed round, increment `extra_acked_win_rtts`. After 5 RTTs, decay `extra_acked` by resetting it to 0.
- Compare accumulated ACKs against expected bandwidth delivery (`max_bw * epoch_us`).
- In `bbr_set_cwnd()`, augment the target window: `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain))`.

---

### Understanding `test.pkt`

`test.pkt` automates validation of the loss recovery workflow:

1. **Slow Start Initialisation**:
   - Sends 10 segments (10,000 bytes) and verifies that `tcpi_snd_cwnd` doubles from 10 to at least 20 upon ACK arrival.
2. **Fast Recovery Trigger**:
   - Transmits 4 segments (`10001:14001`) with the first segment dropped, prompting 3 selective ACKs (SACKs) from the peer.
   - The kernel enters `TCP_CA_Recovery` and emits a fast retransmit for `10001:11001`.
3. **Packet Conservation Check**:
   - Asserts that `tcpi_ca_state == TCP_CA_Recovery` and `tcpi_snd_cwnd == 1` under packet conservation.
4. **Window Restoration Check**:
   - Acknowledges all 14,000 bytes, returning the state machine to `TCP_CA_Open`.
   - Asserts that `tcpi_snd_cwnd >= 20` is restored rather than halved.

---

### How to run

```sh
## 1. Verify that starter.bpf.c fails packetdrill assertions
../../run_test.sh bbr/step06-loss-ack-aggregation starter

## 2. Implement the TODOs in starter.bpf.c and re-run (or run the reference solution)
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```


### `labs/bbr/step06-loss-ack-aggregation/starter.bpf.c` (Starter skeleton)

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 6 (Starter): Loss Recovery, Packet Conservation, & ACK Aggregation
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

#define BBR_EXTRA_ACKED_WIN_RTTS	5U
#define BBR_ACK_EPOCH_ACKED_RESET_THRESH (1U << 20)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT or Recovery */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* use packet conservation in 1st round of Recovery? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	/*
	 * TODO: Save cwnd before Recovery and PROBE_RTT:
	 * If ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT,
	 * save ca->prior_cwnd = tp->snd_cwnd.
	 * Otherwise keep ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd).
	 */
	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
		ca->packet_conservation = 0;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_ack_aggregation(struct sock *sk,
						       const struct rate_sample *rs)
{
	(void)sk;
	(void)rs;
	/*
	 * TODO: ACK aggregation estimation:
	 * 1. If rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0, return.
	 * 2. On round_start: increment ca->extra_acked_win_rtts.
	 *    If ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS, reset both
	 *    extra_acked_win_rtts = 0 and extra_acked = 0.
	 * 3. Compute epoch_us = tp->delivered_mstamp - ca->epoch_mstamp.
	 *    expected_acked = ((u64)ca->max_bw * epoch_us) >> BW_SCALE.
	 * 4. If ca->ack_epoch_acked <= expected_acked or
	 *    ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH:
	 *    Reset ca->ack_epoch_acked = 0, ca->epoch_mstamp = tp->delivered_mstamp, expected_acked = 0.
	 * 5. ca->ack_epoch_acked += rs->acked_sacked.
	 *    extra_acked = min(ca->ack_epoch_acked - expected_acked, tp->snd_cwnd).
	 *    If extra_acked > ca->extra_acked: ca->extra_acked = extra_acked.
	 */
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, __u32 acked, __u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd;

	(void)ca;
	(void)acked;
	/*
	 * TODO: Implement BBR Loss Recovery and cwnd restoration:
	 * 1. Subtract rs->losses from cwnd (floor 1).
	 * 2. On entry to TCP_CA_Recovery (state == Recovery && prev_state != Recovery):
	 *    enable packet_conservation = 1, set next_rtt_delivered = tp->delivered,
	 *    and cwnd = packets_in_flight + max(acked, 1).
	 * 3. On exit from Recovery (prev_state >= Recovery && state < Recovery):
	 *    restore cwnd = max(cwnd, ca->prior_cwnd), clear packet_conservation = 0.
	 * 4. Update ca->prev_ca_state = state.
	 * 5. If ca->packet_conservation is active, set *new_cwnd = max(cwnd, packets_in_flight + max(acked, 1))
	 *    and return true (packet conservation mode).
	 *
	 * Placeholder before TODO is completed: halve cwnd on loss like Reno/CUBIC.
	 */
	if (rs->losses > 0)
		cwnd = max(cwnd >> 1U, 2U);

	*new_cwnd = cwnd;
	return false;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	/*
	 * TODO: Add extra_acked compensation for ACK aggregation:
	 * target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));
	 */

	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->prev_ca_state = TCP_CA_Open;
	ca->packet_conservation = 0;
	ca->epoch_mstamp = tp->tcp_mstamp;
	ca->ack_epoch_acked = 0;
	ca->extra_acked = 0;
	ca->extra_acked_win_rtts = 0;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_ack_aggregation(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_ssthresh, struct sock *sk)
{
	/*
	 * TODO: Save cwnd via bbr_save_cwnd(sk) before entering loss recovery,
	 * and return tcp_sk(sk)->snd_ssthresh without halving cwnd.
	 */
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_undo_cwnd, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	/*
	 * TODO: Restore snd_cwnd to max(tp->snd_cwnd, ca->prior_cwnd).
	 */
	return tp->snd_cwnd;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_set_state, struct sock *sk, __u8 new_state)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	(void)ca;
	(void)new_state;
	/*
	 * TODO: On new_state == TCP_CA_Loss:
	 *   ca->prev_ca_state = TCP_CA_Loss;
	 *   ca->full_bw = 0;
	 *   ca->round_start = 1;
	 */
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr6 = {
	.init		= (void *)bpf_bbr6_init,
	.cong_control	= (void *)bpf_bbr6_cong_control,
	.ssthresh	= (void *)bpf_bbr6_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr6_undo_cwnd,
	.set_state	= (void *)bpf_bbr6_set_state,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr6",
};
```


### `labs/bbr/step06-loss-ack-aggregation/test.pkt` (packetdrill test)

```python
// SPDX-License-Identifier: GPL-2.0
// Step 6: Loss Recovery, Packet Conservation (cwnd = 1), & Prior cwnd Restoration

0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_bbr6`

// Establish a connection with base RTT = 10 ms
+0.000 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0.000 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
+0.000 bind(3, ..., ...) = 0
+0.000 listen(3, 1) = 0

+0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
+0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
+0.010 < . 1:1(0) ack 1 win 512
+0.000 accept(3, ..., ...) = 4

// Send 10 segments at RTT = 10 ms to grow cwnd from 10 to 20
+0.000 write(4, ..., 10000) = 10000
+0.000 > P. 1:10001(10000) ack 1
+0.010 < . 1:1(0) ack 10001 win 512
+0.000 %{
assert tcpi_snd_cwnd >= 20, f"expected cwnd >= 20, got {tcpi_snd_cwnd}"
assert tcpi_ca_state == TCP_CA_Open, f"expected CA_Open, got {tcpi_ca_state}"
}%

// Send 4 segments (10001:14001)
+0.000 write(4, ..., 4000) = 4000
+0.000 > P. 10001:14001(4000) ack 1

// Receive 3 SACK duplicate ACKs to trigger Fast Recovery
+0.010 < . 1:1(0) ack 10001 win 512 <sack 11001:12001,nop,nop>
+0.000 < . 1:1(0) ack 10001 win 512 <sack 11001:13001,nop,nop>
+0.000 < . 1:1(0) ack 10001 win 512 <sack 11001:14001,nop,nop>

// Fast retransmit of the lost segment (10001:11001)
+0.000 > . 10001:11001(1000) ack 1

// Verify packet conservation in the 1st round of Recovery:
// in_flight was 0, max(acked, 1) = 1, so cwnd = 1!
+0.000 %{
assert tcpi_ca_state == TCP_CA_Recovery, f"expected CA_Recovery, got {tcpi_ca_state}"
assert tcpi_snd_cwnd == 1, f"expected cwnd == 1 under packet conservation, got {tcpi_snd_cwnd}"
}%

// Cumulative ACK acknowledging all 14,000 bytes exits Recovery
+0.010 < . 1:1(0) ack 14001 win 512

// BBR exits Recovery back to TCP_CA_Open and restores cwnd to pre-recovery prior_cwnd (>= 20)
+0.000 %{
assert tcpi_ca_state == TCP_CA_Open, f"expected CA_Open, got {tcpi_ca_state}"
assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
}%
```


<details>
<summary>Model answer: <code>labs/bbr/step06-loss-ack-aggregation/solution.bpf.c</code></summary>

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 6: Loss Recovery, Packet Conservation, & ACK Aggregation
 *
 * Implements:
 * 1. Packet conservation during the 1st round trip of TCP_CA_Recovery.
 * 2. Saving and restoring prior_cwnd across Loss Recovery without halving cwnd.
 * 3. ACK aggregation estimation (extra_acked) with windowed decay over
 *    BBR_EXTRA_ACKED_WIN_RTTS round trips.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

#define BBR_EXTRA_ACKED_WIN_RTTS	5U
#define BBR_ACK_EPOCH_ACKED_RESET_THRESH (1U << 20)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT or Recovery */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* use packet conservation in 1st round of Recovery? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
		ca->packet_conservation = 0;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_ack_aggregation(struct sock *sk,
						       const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 epoch_us, expected_acked, extra_acked;

	if (rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (ca->round_start) {
		ca->extra_acked_win_rtts = min(0x1FU, ca->extra_acked_win_rtts + 1U);
		if (ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS) {
			ca->extra_acked_win_rtts = 0;
			ca->extra_acked = 0;
		}
	}

	epoch_us = tp->delivered_mstamp - ca->epoch_mstamp;
	expected_acked = ((__u64)ca->max_bw * epoch_us) >> BW_SCALE;

	if (ca->ack_epoch_acked <= expected_acked ||
	    ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH) {
		ca->ack_epoch_acked = 0;
		ca->epoch_mstamp = tp->delivered_mstamp;
		expected_acked = 0;
	}

	ca->ack_epoch_acked += rs->acked_sacked;
	extra_acked = ca->ack_epoch_acked - expected_acked;
	extra_acked = min(extra_acked, tp->snd_cwnd);
	if (extra_acked > ca->extra_acked)
		ca->extra_acked = extra_acked;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!(ca->probe_rtt_done_stamp &&
	      tp->tcp_mstamp > ca->probe_rtt_done_stamp))
		return;

	ca->min_rtt_stamp = tp->tcp_mstamp;
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	bbr_reset_mode(sk);
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool filter_expired;

	filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
	if (rs->rtt_us >= 0 &&
	    ((__u32)rs->rtt_us < ca->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	if (filter_expired && ca->mode != BBR_PROBE_RTT) {
		bbr_save_cwnd(sk);
		ca->mode = BBR_PROBE_RTT;
		ca->probe_rtt_done_stamp = 0;
	}

	if (ca->mode == BBR_PROBE_RTT) {
		tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		if (!ca->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND) {
			ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
			ca->probe_rtt_round_done = 0;
			ca->next_rtt_delivered = tp->delivered;
		} else if (ca->probe_rtt_done_stamp) {
			if (ca->round_start)
				ca->probe_rtt_round_done = 1;
			if (ca->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, __u32 acked, __u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u8 prev_state = ca->prev_ca_state;
	__u8 state = inet_csk(sk)->icsk_ca_state;
	__u32 cwnd = tp->snd_cwnd;

	if (rs->losses > 0)
		cwnd = max((__s32)cwnd - rs->losses, 1);

	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		ca->packet_conservation = 1;
		ca->next_rtt_delivered = tp->delivered;
		cwnd = tcp_packets_in_flight(tp) + max(acked, 1U);
	} else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
		cwnd = max(cwnd, ca->prior_cwnd);
		ca->packet_conservation = 0;
	}
	ca->prev_ca_state = state;

	if (ca->packet_conservation) {
		*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U));
		return true;
	}
	*new_cwnd = cwnd;
	return false;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));

	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->prev_ca_state = TCP_CA_Open;
	ca->packet_conservation = 0;
	ca->epoch_mstamp = tp->tcp_mstamp;
	ca->ack_epoch_acked = 0;
	ca->extra_acked = 0;
	ca->extra_acked_win_rtts = 0;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_ack_aggregation(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr6_undo_cwnd, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return max(tp->snd_cwnd, ca->prior_cwnd);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr6_set_state, struct sock *sk, __u8 new_state)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (new_state == TCP_CA_Loss) {
		ca->prev_ca_state = TCP_CA_Loss;
		ca->full_bw = 0;
		ca->round_start = 1;
	}
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr6 = {
	.init		= (void *)bpf_bbr6_init,
	.cong_control	= (void *)bpf_bbr6_cong_control,
	.ssthresh	= (void *)bpf_bbr6_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr6_undo_cwnd,
	.set_state	= (void *)bpf_bbr6_set_state,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr6",
};
```

</details>

---
