# Step 2: BDP Calculation from BtlBw (Max Bandwidth Filter) and RTprop (Min RTT Filter)

## Goal

In this step, we build upon Step 1's pacing rate control to implement the two core parameters of BBR's network path model — **BtlBw (Bottleneck Bandwidth via a max-filter)** and **RTprop (Round-Trip Propagation Time via a min-filter)** — and compute the **BDP (Bandwidth-Delay Product)** to drive both `snd_cwnd` and `sk_pacing_rate`.

- Store per-socket BBR state (`min_rtt_us`, `max_bw`, `round_cnt`, `next_rtt_delivered`) inside `inet_csk_ca(sk)` (`struct bpf_bbr2_ca`).
- Understand the physical reason why RTT is tracked with a **minimum filter** whilst delivery rate (bandwidth) is tracked with a **maximum filter**.
- Prevent application-limited (`rs->is_app_limited`) rate samples from artificially dragging down `max_bw`.
- Implement `bbr_bdp()` using fixed-point `BW_UNIT` (`1 << 24`) arithmetic and set `tp->snd_cwnd = max(bdp + 4U, 4U)`.

---

## Design Checkpoints (Think before coding)

Before looking at the solution code, work through these four questions on your own.

### Q1. Why do we take the *minimum* of RTT samples, but the *maximum* of delivery rate samples?

What goes wrong if we overwrite our RTT and bandwidth estimates on every single ACK sample (as in `starter.bpf.c`)? From basic queueing theory, why does estimating physical **round-trip propagation delay (RTprop)** require a min-filter, whereas estimating **bottleneck bandwidth (BtlBw)** requires a max-filter?

<details>
<summary>Hint & Answer</summary>

- **RTprop (minimum RTT filter)**: Every measured RTT is the sum of the two-way physical propagation delay (`RTprop`) plus any queueing delay along the path (`RTT = RTprop + queueing_delay`). Because queueing delay is always non-negative (`>= 0`), the true unloaded propagation delay corresponds to the **minimum** RTT observed.
- **BtlBw (maximum bandwidth filter)**: The delivery rate (`rs->delivered / rs->interval_us`) can never exceed the physical bottleneck capacity of the link, but it can easily fall below the bottleneck rate whenever the sender transmits less than a full pipe or shares the link momentarily. Thus, the **maximum** observed delivery rate estimates the bottleneck bandwidth (`max_bw`).

</details>

### Q2. How should `max_bw` handle application-limited (`rs->is_app_limited`) samples?

Suppose a flow sends a 10-packet burst at line rate, establishing an accurate `max_bw`, and then the application pauses and writes only 2 packets over a long interval. The resulting `rate_sample` will have `rs->is_app_limited == true` and a much lower delivery rate. Under what condition should `ca->max_bw` be updated?

<details>
<summary>Hint & Answer</summary>

- When `rs->is_app_limited` is set, the sender did not keep the pipe full; the low delivery rate reflects application write behaviour rather than network capacity.
- Therefore, we gate the filter with `if (!rs->is_app_limited || bw >= ca->max_bw)` and update `ca->max_bw` when `bw > ca->max_bw`. Note that if an app-limited sample still manages to exceed our current `ca->max_bw`, it proves the bottleneck is at least that fast, so we allow it to raise `ca->max_bw`.

</details>

### Q3. How do we count packet-timed round trips (`round_cnt`) without using wall-clock timers?

In later steps (Startup exit detection and ProbeBW gain cycling), BBR measures time in **packet-timed round trips** rather than wall-clock milliseconds. How can you use `tp->delivered` and `rs->prior_delivered` to detect that one full round trip has elapsed?

<details>
<summary>Hint & Answer</summary>

- Record the cumulative delivered packet counter at the start of a round in `ca->next_rtt_delivered = tp->delivered`.
- Each incoming ACK carries `rs->prior_delivered` — the value of `tp->delivered` at the moment the newly ACKed packet was transmitted.
- Once `!before(rs->prior_delivered, ca->next_rtt_delivered)` holds, a packet sent at or after the start of the current round has been acknowledged, meaning **one full packet-timed round trip** has completed. We increment `ca->round_cnt++` and advance `ca->next_rtt_delivered = tp->delivered`.

</details>

### Q4. How do we compute `bdp` in packets from `max_bw` (`BW_UNIT = 1 << 24`) and `min_rtt_us`?

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

## Step-by-step Walkthrough

### 1. Per-Socket State Struct `struct bpf_bbr2_ca`

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

### 2. `bbr_bdp()` and `bbr_rate_bytes_per_sec()`

In [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c), we use the same fixed-point shifts as Linux's `tcp_bbr.c` (`BW_SCALE = 24`, `BBR_SCALE = 8`):

- `bbr_rate_bytes_per_sec()`: Converts `bw` (`pkts/us << 24`) into `bytes/sec`, applying `gain` (`>> BBR_SCALE`) and subtracting the 1% pacing margin (`BBR_PACING_MARGIN_PCT = 1U`).
- `bbr_bdp()`: Returns `10U` if `ca->min_rtt_us` or `bw` is not yet initialised (`0`), and otherwise computes `(((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT`.

### 3. `bpf_bbr2_cong_control()` — Filtering `BtlBw` & `RTprop` and Setting `snd_cwnd`

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

## Understanding `test.pkt`

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

## How to run

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertions
../../run_test.sh bbr/step02-btlbw-rtprop starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step02-btlbw-rtprop solution
```
