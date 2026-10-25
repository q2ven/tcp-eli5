# Step 3: Transitioning from STARTUP to DRAIN (Full Pipe Detection)

## Goal

In this step, you will implement BBR's state transitions from **STARTUP** (exponential bandwidth search) to **DRAIN** (draining the queue created during STARTUP) and into steady-state **PROBE_BW** (cruising at the estimated bottleneck bandwidth).

- Understand BBR's three core modes (`BBR_STARTUP`, `BBR_DRAIN`, and `BBR_PROBE_BW`) and the `pacing_gain` / `cwnd_gain` used in each mode.
- Track packet-timed round trips using `tp->delivered` and `ca->next_rtt_delivered`.
- Implement `bbr_check_full_bw_reached()` to detect when the bottleneck pipe is full after 3 consecutive non-application-limited rounds without at least 25% (`1.25x`) bandwidth growth.
- Implement `bbr_check_drain()` to transition from `BBR_STARTUP` to `BBR_DRAIN` once the pipe is full, and from `BBR_DRAIN` to `BBR_PROBE_BW` once packets in flight (`tcp_packets_in_flight()`) drop to or below `1.0x BDP`.

---

## Design Checkpoints (Think before coding)

Before inspecting `solution.bpf.c`, think through the following four design questions.

### Q1. How can BBR tell that the pipe is full without waiting for packet loss?

Loss-based congestion control algorithms such as Reno and CUBIC double `cwnd` in slow start until packets are dropped (or until HyStart detects an RTT increase). Because BBR does not use packet loss as a congestion signal, how should it decide when to exit `BBR_STARTUP` (where gain is $2 / \ln(2) \approx 2.89\text{x}$)?

<details>
<summary>Hint and Answer</summary>

- **Watch for a plateau in estimated bottleneck bandwidth (`BtlBw`)**.
- While the bottleneck link is not yet saturated, doubling the sending rate each round causes the measured delivery rate (`ca->max_bw`) to grow rapidly each round.
- Once the sending rate reaches the bottleneck capacity, sending faster only builds a queue at the bottleneck router (increasing RTT), whilst the **delivery rate (`ca->max_bw`) stops growing**.
- BBR concludes that the pipe is full when `ca->max_bw` fails to grow by at least **25% (`bbr_full_bw_thresh = 5/4 = 1.25x`)** over its baseline `ca->full_bw` for **3 consecutive non-app-limited rounds (`bbr_full_bw_cnt = 3`)**.

</details>

### Q2. How do we count round trips using packet delivery counts instead of wall-clock timers?

To check whether bandwidth has plateaued for 3 round trips, we need a clean definition of a round trip that does not depend on noisy timers. How can we use `tp->delivered` and `rs->prior_delivered` to detect when one packet-timed round has completed?

<details>
<summary>Hint and Answer</summary>

- Record the value of `tp->delivered` at the start of a round in `ca->next_rtt_delivered`.
- Whenever a packet is transmitted, the kernel's `tcp_rate_skb_sent()` snapshots the current `tp->delivered` into the `skb` control block. When that packet is later acknowledged, `tcp_rate_skb_delivered()` copies the snapshot into `rs->prior_delivered`.
- Therefore, when `!before(rs->prior_delivered, ca->next_rtt_delivered)` holds (or `tp->delivered >= ca->next_rtt_delivered` when one ACK covers the round), a packet sent at or after the start of the current round has been acknowledged — meaning one full round trip has elapsed.
- We then advance the round boundary by setting `ca->next_rtt_delivered = tp->delivered;` and `ca->round_start = 1`.

</details>

### Q3. Why does BBR enter `BBR_DRAIN` before `BBR_PROBE_BW` instead of switching directly?

By the time `BBR_STARTUP` detects 3 rounds without 25% bandwidth growth, how much excess queue has accumulated at the bottleneck, and how should `pacing_gain` and `cwnd_gain` be chosen to drain it?

<details>
<summary>Hint and Answer</summary>

- Because `BBR_STARTUP` sends with `bbr_high_gain` (`~2.89x`) and waits 3 rounds to confirm the bandwidth plateau, in-flight data reaches roughly `2.89x BDP`, leaving an excess queue of about `1.89x BDP` at the bottleneck.
- Switching straight to `1.0x BtlBw` pacing in `BBR_PROBE_BW` would never drain that standing queue.
- Instead, BBR enters `BBR_DRAIN` with `pacing_gain = bbr_drain_gain = BBR_UNIT * 1000 / 2885` (`88 / 256`, `~0.35x`, the reciprocal of `bbr_high_gain`) to pace slower than the bottleneck rate and drain the queue, whilst keeping `cwnd_gain = bbr_high_gain` so pacing — not an abrupt `cwnd` cut — smoothly drains the pipe.
- As soon as `tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)` (`1.0x BDP`), the queue is drained and BBR transitions to `BBR_PROBE_BW` (`pacing_gain = 1.0x`, `cwnd_gain = 2.0x`).

</details>

### Q4. Why does `bbr_set_pacing_rate()` check `ca->full_bw_reached` before lowering `sk->sk_pacing_rate`?

In `bbr_set_pacing_rate()`, the socket pacing rate is updated only when `if (ca->full_bw_reached || rate > sk->sk_pacing_rate)` is true. Why must BBR refuse to lower `sk->sk_pacing_rate` before `full_bw_reached` is set, yet allow lowering it afterwards?

<details>
<summary>Hint and Answer</summary>

- Early in `BBR_STARTUP`, a flow may see artificially low rate samples due to small initial windows or brief application limits. Lowering `sk->sk_pacing_rate` before filling the pipe (`!ca->full_bw_reached`) would stall STARTUP's exponential ramp-up.
- Once the pipe has been filled (`ca->full_bw_reached == 1`), BBR must lower its pacing rate from `2.89x BtlBw` down to `0.35x BtlBw` in `BBR_DRAIN` and `1.0x BtlBw` in `BBR_PROBE_BW`.

</details>

---

## Step-by-step Walkthrough

### 1. Modes and Fixed-Point Constants

With `BBR_SCALE = 8` (`BBR_UNIT = 256` representing `1.0x`), the gains and full-pipe thresholds are defined as:

- `bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1 = 739` (`~2.89x`): `pacing_gain` and `cwnd_gain` in `BBR_STARTUP`.
- `bbr_drain_gain = BBR_UNIT * 1000 / 2885 = 88` (`~0.35x`): `pacing_gain` in `BBR_DRAIN`.
- `bbr_cwnd_gain = BBR_UNIT * 2 = 512` (`2.0x`): `cwnd_gain` in `BBR_PROBE_BW`.
- `bbr_full_bw_thresh = BBR_UNIT * 5 / 4 = 320` (`1.25x`): required bandwidth growth factor (+25%) per round to remain in `BBR_STARTUP`.
- `bbr_full_bw_cnt = 3`: number of consecutive non-app-limited rounds without 25% growth that triggers full-pipe detection.

### 2. `bbr_check_full_bw_reached()` — Full Pipe Detection

Evaluated at the end of each packet-timed round (`ca->round_start == 1`) when the rate sample is not application-limited (`!rs->is_app_limited`):

1. Compute the 1.25x growth threshold: `bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE`.
2. If `ca->max_bw >= bw_thresh`, bandwidth grew by at least 25%: record the new baseline `ca->full_bw = ca->max_bw` and reset `ca->full_bw_cnt = 0`.
3. Otherwise, increment `++ca->full_bw_cnt`; when it reaches `bbr_full_bw_cnt` (`3`), set `ca->full_bw_reached = 1`.

### 3. `bbr_check_drain()` — Transitioning to `BBR_DRAIN` and `BBR_PROBE_BW`

1. When `ca->mode == BBR_STARTUP && ca->full_bw_reached`, transition to `ca->mode = BBR_DRAIN` and record `1.0x BDP` in `tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT)` for observability.
2. Using a separate `if` statement (so it can fall through on the same ACK if in-flight packets are already at or below BDP), check `ca->mode == BBR_DRAIN && tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT)`. When satisfied, transition to `ca->mode = BBR_PROBE_BW`.

### 4. `bbr_update_gains()` and `bbr_set_cwnd()`

- `bbr_update_gains()` updates `ca->pacing_gain` and `ca->cwnd_gain` according to `ca->mode`.
- `bbr_set_cwnd()` computes `target_cwnd = bbr_bdp(sk, bw, gain)`. Once `ca->full_bw_reached` is set, `cwnd = min(cwnd + acked, target_cwnd)` clamps `snd_cwnd` down to `target_cwnd` (`2.0x BDP` in `BBR_PROBE_BW`).

---

## Understanding `test.pkt`

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

## How to run

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step03-startup-drain starter
# 2. Implement the TODO in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step03-startup-drain solution
```
