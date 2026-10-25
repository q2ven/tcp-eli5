# BBR Step 5: Transition to PROBE_RTT & Queue Draining (cwnd = 4)

## Learning Goals

- Understand the necessity and control flow of why BBR periodically transitions to `BBR_PROBE_RTT` to clamp inflight down to the minimum pipe size (`BBR_MIN_PIPE_CWND = 4`).
- Learn how BBR resolves the standing queue problem in bottleneck buffers and avoids stale two-way propagation delay estimates (`min_rtt_us` / RTprop).
- Implement the filter window expiration logic governed by `BBR_MIN_RTT_WIN_US` (standard 10 seconds in the kernel, shortened to 2 seconds in this lab).
- Implement the two completion criteria in BPF for `BBR_PROBE_RTT` (spending at least 200 ms and completing 1 packet-timed round trip with inflight <= 4).
- Understand how `ca->prior_cwnd` saves the pre-probe window, restores it upon exit, and how `bbr_set_cwnd()` safely enforces the window clamping.

---

## Design Checkpoints (Think Before Looking at the Code)

### Q1. Why can a flow in `BBR_PROBE_BW` get stuck with an inflated `min_rtt_us`?
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

### Q2. What state fields are required to track the `min_rtt` window and the `BBR_PROBE_RTT` hold timer?
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

### Q3. Why does `BBR_PROBE_RTT` require BOTH 200 ms AND at least 1 packet-timed round with `inflight <= 4`?
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

### Q4. Why must `bbr_save_cwnd()` preserve `prior_cwnd` and why does `bbr_set_cwnd()` apply the cap at the end?
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

## Step-by-step Walkthrough

### 1. Mathematical Design and Constants

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

### 2. `bbr_update_min_rtt()`: Min RTT Tracking & PROBE_RTT Control

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

### 3. `bbr_check_probe_rtt_done()` and Mode Restoration

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

### 4. Enforcing Clamping in `bbr_set_cwnd()`

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

## Understanding `test.pkt`

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

## How to run

```sh
# 1. Verify that starter.bpf.c fails on the assertion
../../run_test.sh bbr/step05-probe-rtt starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step05-probe-rtt solution
```

- In `starter.bpf.c`, `filter_expired`, `BBR_PROBE_RTT` transition, and `snd_cwnd` clamping are omitted, causing the assertion at 2.1s (`assert tcpi_snd_cwnd == 4`) to fail.
- In `solution.bpf.c`, all `packetdrill` assertions pass and `=== [PASS] bbr/step05-probe-rtt (solution) ===` is printed.
