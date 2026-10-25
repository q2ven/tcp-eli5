# BBRv1 Step 5: Transition to PROBE_RTT & Clamping to Minimum Pipe Size (cwnd = 4)

## 1. Goal

In this step, you will implement BBRv1's fourth state, `BBR_PROBE_RTT`, in BPF to periodically drain standing queues at the bottleneck and refresh stale minimum RTT estimates (`min_rtt_us`). Specifically, you will implement the minimum-RTT filter window expiry check (`BBR_MIN_RTT_WIN_US`), save the pre-probe congestion window into `ca->prior_cwnd`, clamp `snd_cwnd` down to the minimum pipe size (`BBR_MIN_PIPE_CWND = 4`), and hold `BBR_PROBE_RTT` until inflight has remained at or below 4 packets for both at least 200 ms and one full packet-timed round trip before restoring the window and mode (see [../README.md](../README.md) for the full track overview).

---

## 2. How the Algorithm Works (Read This First)

### Why `BBR_PROBE_BW` Alone Loses Track of the True Propagation Delay (RTprop)

BBR estimates a path's bandwidth-delay product ($\text{BDP} = \text{BtlBw} \times \text{RTprop}$) by tracking two independent quantities: the windowed maximum delivery rate `max_bw` ($\approx \text{BtlBw}$) and the windowed minimum round-trip time `min_rtt_us` ($\approx \text{RTprop}$). However, the physical two-way propagation delay ($\text{RTprop}$) can only be observed when the bottleneck queue is completely empty (queueing delay $= 0$).

In steady-state `BBR_PROBE_BW`, a flow cycles through 8 pacing gain phases (`[1.25, 0.75, 1, 1, 1, 1, 1, 1]`). While phase 1 (`0.75`) drains the excess queue (`0.25 * BDP`) that the flow itself created during phase 0 (`1.25`), the remaining 6 phases keep inflight at $1.0 \times \text{BDP}$. It never intentionally drops inflight below $1.0 \times \text{BDP}$. This leads to two fundamental issues:

1. **Standing Queues from Competing Flows**:
   If another flow joins while a queue is already present at the bottleneck buffer, each flow maintaining $1.0 \times \text{BDP}$ prevents the bottleneck queue from ever draining all the way to zero packets. Every RTT sample measured by every flow equals `true RTprop + standing queue delay`, leaving `min_rtt_us` permanently inflated.
2. **Route Changes Increasing True RTprop**:
   If physical path routing changes to a longer path (for instance, true `RTprop` increases from `10 ms` to `15 ms`), tracking `min_rtt_us` as an all-time minimum over the lifetime of the connection would keep the obsolete `10 ms` value forever.

### Core Idea and Mathematical Model of `BBR_PROBE_RTT`

To solve both problems, BBR attaches a **filter expiration window `BBR_MIN_RTT_WIN_US`** to `min_rtt_us` (10 seconds in Linux kernel `net/ipv4/tcp_bbr.c`, shortened to **2 seconds** in this lab so `packetdrill` tests complete in ~2.5 seconds).

- **Refreshing `min_rtt_us` Within the Window (Strict Inequality `<`)**:
  Whenever an ACK produces a valid RTT sample `rs->rtt_us >= 0` that is **strictly smaller** than the current estimate (`(__u32)rs->rtt_us < ca->min_rtt_us`), BBR updates `ca->min_rtt_us = rs->rtt_us` and records the current timestamp `ca->min_rtt_stamp = tp->tcp_mstamp`. Using strict `<` rather than `<=` is essential: if identical RTT samples repeated continuously (e.g. under a constant standing queue or coarse timestamp granularity), `<=` would keep refreshing `ca->min_rtt_stamp` and the window would never expire.
- **Detecting Filter Expiry (`filter_expired`)**:
  If more than `BBR_MIN_RTT_WIN_US` microseconds have elapsed since `ca->min_rtt_stamp`, the filter expires:
  $$\text{filter\_expired} \iff \text{tp->tcp\_mstamp} > \text{ca->min\_rtt\_stamp} + \text{BBR\_MIN\_RTT\_WIN\_US}$$
  When `filter_expired` is true, as long as the ACK was not artificially delayed by the receiver's delayed-ACK timer (`!rs->is_ack_delayed`), BBR overwrites `ca->min_rtt_us = rs->rtt_us` and `ca->min_rtt_stamp = tp->tcp_mstamp` (even if `rs->rtt_us >= ca->min_rtt_us`) and transitions into `BBR_PROBE_RTT`.

### Why Clamp `cwnd = 4` and Wait for Both 200 ms and 1 Packet-Timed Round?

Upon entering `BBR_PROBE_RTT`, BBR saves the prior working congestion window into `ca->prior_cwnd`, sets `pacing_gain = 1.0` (`BBR_UNIT`) and `cwnd_gain = 1.0` (`BBR_UNIT`), and clamps `tp->snd_cwnd` down to the minimum pipe size **`BBR_MIN_PIPE_CWND = 4` packets**.

- **Why 4 packets instead of 1 or 2?**:
  Standard TCP receivers implement delayed ACKs, sending one ACK for every two data segments received (or waiting 40–200 ms for a delayed-ACK timeout). If `snd_cwnd` were reduced to 1 or 2 packets, the pipe would stall waiting for delayed-ACK timeouts. Keeping `BBR_MIN_PIPE_CWND = 4` guarantees at least two immediate ACK triggers per round trip while draining almost the entire bottleneck queue.
- **Why mark the socket `tp->app_limited`?**:
  While `snd_cwnd` is clamped to 4 packets, the flow is intentionally window-limited and delivers data far below the bottleneck capacity. To prevent these artificially low delivery rates from corrupting `ca->max_bw`, BBR marks the socket application-limited on every ACK in `BBR_PROBE_RTT`:
  `tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;`
  (since `0` means "not app-limited" in the kernel, `: 1` ensures a non-zero value).
- **When the Hold Timer Starts and the Two Exit Criteria**:
  Clamping `tp->snd_cwnd = 4` does not make packets already in the network vanish instantaneously; dozens or hundreds of packets may still be in flight (`tcp_packets_in_flight(tp) > 4`). BBR therefore waits until **in-flight packets actually drop to 4 or fewer (`tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`)** before starting the hold timer and packet-timed round counter:
  - **Criterion A (Time Duration: 200 ms)**: Once `tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`, BBR sets `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` (`200 ms`). Holding for 200 ms ensures that competing flows with different RTTs overlap their drain phases. With a 200 ms probe every 10 seconds in production Linux BBR, the theoretical throughput cost is bounded to $\frac{200\text{ ms}}{10\text{ s}} = 2\%$.
  - **Criterion B (Packet-Timed Round: 1 full round trip)**: At the same moment `probe_rtt_done_stamp` is armed, BBR resets `ca->probe_rtt_round_done = 0` and sets `ca->next_rtt_delivered = tp->delivered`. When a full packet-timed round finishes (`ca->round_start == 1`), BBR sets `ca->probe_rtt_round_done = 1`. This guarantees that on long-RTT paths (e.g. a 300 ms satellite link where 200 ms is less than one RTT), BBR waits until at least one packet sent while the queue was drained has completed a full round trip and delivered its RTT sample.
- **Exiting `BBR_PROBE_RTT` and Restoring `prior_cwnd`**:
  Once **both** `ca->probe_rtt_round_done` and `tp->tcp_mstamp > ca->probe_rtt_done_stamp` hold (`bbr_check_probe_rtt_done()`), BBR refreshes `ca->min_rtt_stamp = tp->tcp_mstamp` (so it does not immediately re-enter `BBR_PROBE_RTT`), restores `tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)` to resume full-speed transmission without re-running slow start, and calls `bbr_reset_mode()` to return to `BBR_PROBE_BW` (if `full_bw_reached`) or `BBR_STARTUP` (if not).

```
[BBR_STARTUP / BBR_PROBE_BW] (cwnd = 20, min_rtt_us = 10 ms)
       │
       │  2.0 s elapsed (tp->tcp_mstamp > min_rtt_stamp + BBR_MIN_RTT_WIN_US)
       ▼
  filter_expired == true
  - Save prior_cwnd = 20 via bbr_save_cwnd()
  - Set ca->mode = BBR_PROBE_RTT, ca->probe_rtt_done_stamp = 0
  - Clamp tp->snd_cwnd = min(cwnd, 4) at the end of bbr_set_cwnd()
       │
       ▼
[BBR_PROBE_RTT: Waiting for Pipe to Drain]
  - Set tp->app_limited to protect max_bw
  - As soon as tcp_packets_in_flight(tp) <= 4, arm timer and round tracker:
      probe_rtt_done_stamp = tp->tcp_mstamp + 200 ms
      probe_rtt_round_done = 0
      next_rtt_delivered   = tp->delivered
       │
       │  (1) round_start == 1 sets probe_rtt_round_done = 1 (1 round completed)
       │  (2) tp->tcp_mstamp > probe_rtt_done_stamp (200 ms elapsed)
       ▼
[bbr_check_probe_rtt_done()]
  - Refresh ca->min_rtt_stamp = tp->tcp_mstamp
  - Restore tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)  (cwnd = 20 restored)
  - Call bbr_reset_mode() to return to BBR_STARTUP or BBR_PROBE_BW
```

### Concrete Numerical Example (Following `test.pkt`)

1. **`t = 0.010 s` (Handshake completes)**: The initial RTT sample is `10 ms` (`10,000 us`), setting `ca->min_rtt_us = 10000` and `ca->min_rtt_stamp = t_0`. Initial `snd_cwnd` is `10`.
2. **`t = 0.020 s` (10 segments sent and ACKed)**: During `BBR_STARTUP`, 10 segments are ACKed at `RTT = 10 ms`, growing `snd_cwnd` from `10` to `20`. Because `10 ms` is not strictly less than `min_rtt_us` (`10 ms`), `min_rtt_stamp` remains `t_0`.
3. **`t = 2.135 s` (`+2.1 s` later, 1 segment ACKed at `RTT = 15 ms`)**: `tp->tcp_mstamp - ca->min_rtt_stamp` is `2.125 s > 2.0 s` (`BBR_MIN_RTT_WIN_US`), so `filter_expired = true`.
   - Because `!rs->is_ack_delayed`, `ca->min_rtt_us` is updated to `15000` (`15 ms`) and `ca->min_rtt_stamp = tp->tcp_mstamp`.
   - `bbr_save_cwnd()` saves `ca->prior_cwnd = 20`, and `ca->mode` becomes `BBR_PROBE_RTT`.
   - Because `tcp_packets_in_flight(tp) == 0 <= 4`, BBR immediately sets `ca->probe_rtt_done_stamp = tp->tcp_mstamp + 200 ms` and `ca->next_rtt_delivered = tp->delivered`.
   - At the end of `bbr_set_cwnd()`, `tp->snd_cwnd` is clamped to `4` (`BBR_MIN_PIPE_CWND`).
4. **`t = 2.395 s` (`+0.25 s` later, 1 segment sent and ACKed at `RTT = 10 ms`)**:
   - Because `rs->rtt_us = 10000 < ca->min_rtt_us (15000)`, `ca->min_rtt_us` is updated back to the true `10000` (`10 ms`).
   - Delivering the new packet triggers `ca->round_start = 1`, setting `ca->probe_rtt_round_done = 1`.
   - Because `tp->tcp_mstamp > ca->probe_rtt_done_stamp` (`250 ms > 200 ms`) also holds, `bbr_check_probe_rtt_done()` restores `tp->snd_cwnd = max(4, 20) = 20` and exits `BBR_PROBE_RTT`.

### What the Kernel Already Provides

To translate this algorithm into BPF C code, the kernel (`struct tcp_sock`, `struct rate_sample`, `struct inet_connection_sock`) and `bpf_cc_helpers.h` already provide the following fields and helpers:

- **`icsk_ca_priv` and `inet_csk_ca(sk)`**:
  `icsk_ca_priv` inside `struct inet_connection_sock` is per-socket private storage dedicated to the congestion control module. We access it via `struct bbr_ca *ca = inet_csk_ca(sk)` to store BBR state (`min_rtt_us`, `min_rtt_stamp`, `probe_rtt_done_stamp`, `prior_cwnd`, `probe_rtt_round_done`, `next_rtt_delivered`, `round_start`, `mode`, etc.).
- **`struct tcp_sock` (accessed via `tp = tcp_sk(sk)`)**:
  - `tp->tcp_mstamp`: Current microsecond timestamp refreshed on packet/ACK processing, used to compare against `ca->min_rtt_stamp` and `ca->probe_rtt_done_stamp`.
  - `tp->delivered`: Cumulative count of data packets delivered to the receiver, used for packet-timed round tracking (`ca->next_rtt_delivered`) and `tp->app_limited`.
  - `tp->delivered_mstamp`: Timestamp of the latest delivery (`cycle_mstamp` reset when returning to `BBR_PROBE_BW`).
  - `tp->snd_cwnd` and `tp->snd_cwnd_clamp`: Current congestion window (in packets) and its upper clamp.
  - `tp->app_limited`: Non-zero delivery index marking the flow as application-limited (or intentionally window-limited during `BBR_PROBE_RTT`).
- **Helper `tcp_packets_in_flight(tp)`**:
  Computes `tp->packets_out - tp->left_out + tp->retrans_out`, returning the exact number of packets currently in flight in the pipe.
- **`struct rate_sample` (passed as `rs` to `.cong_control`)**:
  - `rs->rtt_us`: Latest RTT sample in microseconds (`s32`; negative `< 0` if unavailable).
  - `rs->is_ack_delayed`: Boolean flag set by the kernel when the ACK was delayed by the receiver's delayed-ACK timer.
  - `rs->is_app_limited`: Boolean flag set when the rate sample was collected while `tp->app_limited` was active.
  - `rs->acked_sacked`: Number of packets newly ACKed or SACKed on this ACK.

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Use the concepts from Section 2 to work through these four design questions before filling in the TODOs in `starter.bpf.c`.

### Q1. What are the two conditions for updating `ca->min_rtt_us` in `bbr_update_min_rtt()`?
Given a valid RTT sample (`rs->rtt_us >= 0`), how should we write the condition to update `ca->min_rtt_us` and `ca->min_rtt_stamp`? Why must we use strict `<` instead of `<=`, and why do we check `!rs->is_ack_delayed` when `filter_expired` is true?

<details>
<summary>Hint and Answer</summary>

**Hint**:
There are two cases: either a new sample arrives within the window that beats the current minimum, or the window has expired (`filter_expired`) and we must replace the stale minimum with the current RTT sample.

**Answer**:
We write the check as:
```c
filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US;
if (rs->rtt_us >= 0 &&
    ((__u32)rs->rtt_us < ca->min_rtt_us ||
     (filter_expired && !rs->is_ack_delayed))) {
	ca->min_rtt_us = rs->rtt_us;
	ca->min_rtt_stamp = tp->tcp_mstamp;
}
```
- Strict inequality `(__u32)rs->rtt_us < ca->min_rtt_us` prevents repeated identical RTT samples from endlessly refreshing `ca->min_rtt_stamp` and starving `filter_expired`.
- Checking `filter_expired && !rs->is_ack_delayed` ensures that when the window expires, we do not replace `min_rtt_us` with an RTT sample inflated by the receiver's delayed-ACK timer.
</details>

---

### Q2. When transitioning into `BBR_PROBE_RTT` upon `filter_expired`, why do we set `probe_rtt_done_stamp = 0` instead of immediately starting the 200 ms timer?
When `filter_expired && ca->mode != BBR_PROBE_RTT` triggers, why shouldn't we immediately set `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US`?

<details>
<summary>Hint and Answer</summary>

**Hint**:
Consider how many packets are still in flight in the network at the exact instant `ca->mode` switches to `BBR_PROBE_RTT`.

**Answer**:
When entering `BBR_PROBE_RTT`, we save the prior window (`bbr_save_cwnd(sk)`), set `ca->mode = BBR_PROBE_RTT`, and initialise `ca->probe_rtt_done_stamp = 0` (timer disarmed).
At the moment of transition, dozens or hundreds of previously transmitted packets may still be in flight (`tcp_packets_in_flight(tp) > BBR_MIN_PIPE_CWND`). If the 200 ms timer started immediately before those packets drained from the pipe, the timer could expire before the bottleneck queue actually emptied. Therefore, we only arm `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US` once `tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND` (4 packets).
</details>

---

### Q3. How do we coordinate `tp->app_limited`, `probe_rtt_done_stamp`, and `probe_rtt_round_done` while in `BBR_PROBE_RTT`?
Inside the `if (ca->mode == BBR_PROBE_RTT)` block, what actions must run on each ACK when `ca->probe_rtt_done_stamp` is still `0` versus when it is already non-zero?

<details>
<summary>Hint and Answer</summary>

**Hint**:
1. Always mark `tp->app_limited` non-zero first so `ca->max_bw` is protected.
2. If `!ca->probe_rtt_done_stamp`, check whether `tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND` to arm the timer and start a new packet-timed round.
3. Otherwise (`else if (ca->probe_rtt_done_stamp)`), update `ca->probe_rtt_round_done` when `ca->round_start` is true, and check the exit condition when `ca->probe_rtt_round_done` is true.

**Answer**:
1. Set `tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;` on every ACK in `BBR_PROBE_RTT`.
2. When `!ca->probe_rtt_done_stamp && tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND`:
   - `ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;`
   - `ca->probe_rtt_round_done = 0;`
   - `ca->next_rtt_delivered = tp->delivered;`
3. When `else if (ca->probe_rtt_done_stamp)`:
   - `if (ca->round_start) ca->probe_rtt_round_done = 1;`
   - `if (ca->probe_rtt_round_done) bbr_check_probe_rtt_done(sk);` (which verifies `tp->tcp_mstamp > ca->probe_rtt_done_stamp`, refreshes `ca->min_rtt_stamp = tp->tcp_mstamp`, restores `tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd)`, and calls `bbr_reset_mode(sk)`).
</details>

---

### Q4. Where in `bbr_set_cwnd()` should `tp->snd_cwnd` be clamped to `BBR_MIN_PIPE_CWND`?
In `bbr_set_cwnd()`, there is an early `if (!acked) goto done;` branch as well as standard BDP target window calculations. Where should the `BBR_PROBE_RTT` clamp be placed?

<details>
<summary>Hint and Answer</summary>

**Hint**:
Even if `acked == 0` jumps directly to `done:`, `tp->snd_cwnd` must still be capped to 4 packets whenever `ca->mode == BBR_PROBE_RTT`.

**Answer**:
Place the clamp at the very end of `bbr_set_cwnd()`, after the `done:` label and `tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);`:
```c
done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	if (ca->mode == BBR_PROBE_RTT)
		tp->snd_cwnd = min(tp->snd_cwnd, BBR_MIN_PIPE_CWND);
```
This guarantees that regardless of how `cwnd` was computed (or whether `!acked` took the early jump), `tp->snd_cwnd` never exceeds `BBR_MIN_PIPE_CWND` (4) while in `BBR_PROBE_RTT`.
</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

Let's walk through the functions and fixed-point calculations in `solution.bpf.c`.

### 1. Constants and Fixed-Point Scales

```c
#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_MIN_PIPE_CWND	4U
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)
```

- Bandwidth `max_bw` is stored in packets per microsecond shifted left by `BW_SCALE = 24` (`BW_UNIT = 1ULL << 24`), and gains are stored in fixed-point units of `BBR_SCALE = 8` (`BBR_UNIT = 256` represents `1.0`).
- In `bbr_bdp()`, `w = (__u64)bw * ca->min_rtt_us` is multiplied by `gain`, shifted right by `BBR_SCALE`, rounded up with `+ BW_UNIT - 1`, and shifted right by `BW_SCALE` to compute the target BDP in packets.
- `BBR_MIN_RTT_WIN_US` is `2ULL * USEC_PER_SEC` (2,000,000 us = 2 s) and `BBR_PROBE_RTT_MODE_US` is `200ULL * USEC_PER_MSEC` (200,000 us = 200 ms).

### 2. `bbr_save_cwnd()` and `bbr_reset_mode()`

```c
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
```

- `bbr_save_cwnd()` records `tp->snd_cwnd` into `ca->prior_cwnd` when outside `BBR_PROBE_RTT` (or preserves the larger value if called while already in `BBR_PROBE_RTT`, such as via `bpf_bbr5_ssthresh()`).
- `bbr_reset_mode()` returns the flow to `BBR_STARTUP` if `ca->full_bw_reached` is still 0, or to `BBR_PROBE_BW` (at `cycle_idx = 0`) if the pipe has already been filled.

### 3. `bbr_check_probe_rtt_done()` and `bbr_update_min_rtt()`

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
```

- In `bpf_bbr5_cong_control()`, `bbr_update_bw()` runs first and updates `ca->round_start`, followed by `bbr_update_min_rtt()`.
- On the ACK where `ca->probe_rtt_done_stamp` is first armed (`!ca->probe_rtt_done_stamp`), the `if` branch runs and starts tracking the round from `ca->next_rtt_delivered = tp->delivered`. On subsequent ACKs, the `else if (ca->probe_rtt_done_stamp)` branch checks `ca->round_start` and calls `bbr_check_probe_rtt_done()`.
- When `bbr_check_probe_rtt_done()` exits `BBR_PROBE_RTT`, it restores `tp->snd_cwnd` and resets `ca->mode` before `bbr_update_gains()` and `bbr_set_cwnd()` execute, so the restored window is immediately used on that same ACK.

### 4. `bbr_update_gains()` and `bbr_set_cwnd()`

```c
static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	// ...
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
```

- During `BBR_PROBE_RTT`, both `pacing_gain` and `cwnd_gain` are set to `BBR_UNIT` (`1.0`).
- At the end of `bbr_set_cwnd()`, `if (ca->mode == BBR_PROBE_RTT)` clamps `tp->snd_cwnd` down to `BBR_MIN_PIPE_CWND` (4).

---

## 5. Understanding `test.pkt` and How to Run

### Packet Trace and Assertions in `test.pkt`

`test.pkt` validates the `BBR_PROBE_RTT` transition, window clamping, and restoration across three stages:

1. **Connection Establishment and Initial Window Growth (lines 12–23)**:
   ```
   +0.000 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7>
   +0.000 > S. 0:0(0) ack 1 <mss 1460,nop,nop,sackOK,nop,wscale 7>
   +0.010 < . 1:1(0) ack 1 win 512
   ...
   +0.000 write(4, ..., 10000) = 10000
   +0.000 > P. 1:10001(10000) ack 1
   +0.010 < . 1:1(0) ack 10001 win 512
   +0.000 %{
   assert tcpi_snd_cwnd == 20, f"expected cwnd == 20, got {tcpi_snd_cwnd}"
   }%
   ```
   - The 3-way handshake establishes an initial `RTT = 10 ms` (`min_rtt_us = 10000`).
   - Sending 10 segments (10,000 bytes with `mss = 1000`) and receiving an ACK 10 ms later in `BBR_STARTUP` grows `snd_cwnd` from `10` to `20`.

2. **Filter Expiry After `+2.100 s` and Clamping to `cwnd = 4` (lines 27–34)**:
   ```
   +2.100 write(4, ..., 1000) = 1000
   +0.000 > P. 10001:11001(1000) ack 1
   +0.015 < . 1:1(0) ack 11001 win 512

   +0.000 %{
   assert tcpi_snd_cwnd == 4, f"expected cwnd == 4 in PROBE_RTT, got {tcpi_snd_cwnd}"
   }%
   ```
   - After advancing `+2.100` seconds (`> BBR_MIN_RTT_WIN_US = 2 s`), 1 segment is sent and ACKed with `RTT = 15 ms` (`> 10 ms`), so `min_rtt_us` is not refreshed early.
   - `filter_expired` fires, saving `prior_cwnd = 20`, entering `BBR_PROBE_RTT`, and clamping `tcpi_snd_cwnd` to `4`.

3. **Exiting `BBR_PROBE_RTT` After `+0.250 s` and 1 Round Trip (lines 38–45)**:
   ```
   +0.250 write(4, ..., 1000) = 1000
   +0.000 > P. 11001:12001(1000) ack 1
   +0.010 < . 1:1(0) ack 12001 win 512

   +0.000 %{
   assert tcpi_snd_cwnd >= 20, f"expected restored cwnd >= 20, got {tcpi_snd_cwnd}"
   }%
   ```
   - After waiting `+0.250` seconds (`> BBR_PROBE_RTT_MODE_US = 200 ms`) with inflight $\le 4$, 1 segment is sent and ACKed at `RTT = 10 ms`, completing 1 packet-timed round trip (`probe_rtt_round_done = 1`).
   - With both the 200 ms timer and 1 round trip satisfied, BBR exits `BBR_PROBE_RTT`, restores `prior_cwnd` (20), and adds the newly ACKed segment so `tcpi_snd_cwnd >= 20` passes.

### Why `starter.bpf.c` Fails and `solution.bpf.c` Passes

- In `starter.bpf.c`, the `filter_expired` check, the transition to `BBR_PROBE_RTT`, and the `BBR_MIN_PIPE_CWND` clamp in `bbr_set_cwnd()` are left as TODOs. Consequently, after `+2.100 s`, the flow stays in `BBR_STARTUP` and grows `snd_cwnd` to `21`, failing the assertion `assert tcpi_snd_cwnd == 4` on line 33.
- In `solution.bpf.c`, the filter expires after 2.1 seconds, clamps `snd_cwnd` to `4`, and restores `snd_cwnd >= 20` after the 0.25-second hold and round trip complete, passing all assertions.

### How to Run

Run the following commands to verify `starter` (fails as expected before adding your code) and `solution` (passes):

```sh
../../run_test.sh bbr/step05-probe-rtt starter
../../run_test.sh bbr/step05-probe-rtt solution
```
