# Step 4: PROBE_BW 8-Phase Gain Cycle (Bandwidth Probing and Queue Draining)

## Goal

In this step, you will implement the steady-state **PROBE_BW** mode's 8-phase pacing gain cycle (`bbr_pacing_gain[]`) and the phase transition logic.

- Understand the structure and purpose of the 8-phase gain cycle (`1.25x` → `0.75x` → `1.0x` × 6) used to probe for newly available bandwidth while preventing persistent bufferbloat.
- Implement `bbr_is_next_cycle_phase()` and understand how transition conditions differ between cruise phases, probe phases, and drain phases (minimum RTT elapsed, target in-flight reached, packet loss, and queue drainage).
- Implement `bbr_advance_cycle_phase()` and `bbr_update_cycle_phase()` to increment the cycle index (`ca->cycle_idx`) and timestamp each phase boundary (`ca->cycle_mstamp`).
- Update `bbr_update_gains()` to look up `bbr_pacing_gain[]` using `ca->cycle_idx`.

---

## Design Checkpoints (Think before coding)

Before inspecting `solution.bpf.c`, think through the following four design questions.

### Q1. Why does BBR cycle through an 8-phase gain pattern (1.25x → 0.75x → 1.0x × 6) instead of pacing at a constant 1.0x?

Once `BBR_STARTUP` and `BBR_DRAIN` have established the estimated bottleneck bandwidth (`ca->max_bw`) and minimum RTT (`ca->min_rtt_us`), why shouldn't BBR simply cruise at `1.0x` indefinitely?

<details>
<summary>Hint and Answer</summary>

- **Available network bandwidth changes dynamically over time**.
- If competing flows terminate or network routing shifts, spare bandwidth becomes available. A sender pacing strictly at its current `1.0x` estimate would never discover that extra bandwidth.
- BBR therefore periodically (roughly every 8 RTTs) increases its pacing rate to `1.25x` (+25%) during **Phase 0 (Probe)** to probe for higher capacity.
- If there is no extra bandwidth, the 25% surplus traffic creates a queue of about `0.25x BDP` at the bottleneck router. To prevent this queue from persisting and causing bufferbloat, BBR drops its pacing rate to `0.75x` (-25%) in **Phase 1 (Drain)** to quickly drain the newly created queue.
- It then cruises at `1.0x` for 6 rounds (**Phases 2..7 (Cruise)**) to maintain high throughput and low queuing delay.

</details>

### Q2. What conditions are required to exit Phase 0 (Probe, 1.25x) and enter Phase 1 (Drain)?

When deciding whether Phase 0 (`ca->pacing_gain > BBR_UNIT`) is complete, why must BBR check in-flight packet volume (`rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)`) or packet loss (`rs->losses`) in addition to verifying that 1 RTT (`min_rtt_us`) has elapsed?

<details>
<summary>Hint and Answer</summary>

- If the sender was application-limited (scarcity of data in the socket write buffer) or receive-window-limited, it might not have actually pushed `1.25x BDP` of packets into the network despite 1 RTT having passed.
- Exiting Phase 0 prematurely would abort the probe before testing whether the network can absorb the extra bandwidth.
- Thus, Phase 0 continues until at least one minimum RTT has elapsed (`is_full_length`) AND either the pipe has been filled to `1.25x BDP` (`rs->prior_in_flight >= bbr_inflight(1.25x)`) or packet loss occurs (`rs->losses`), signalling buffer overflow.

</details>

### Q3. Why is Phase 1 (Drain, 0.75x) allowed to exit before a full RTT has elapsed (OR condition)?

Unlike Phase 0, Phase 1 (`ca->pacing_gain < BBR_UNIT`) exits when `is_full_length || rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)`. Why is an early exit permitted here?

<details>
<summary>Hint and Answer</summary>

- The sole purpose of Phase 1 is to drain the excess queue created during Phase 0.
- When an ACK arrives indicating that packets in flight have fallen to or below `1.0x BDP` (`bbr_inflight(sk, ca->max_bw, BBR_UNIT)`), the bottleneck queue has been completely emptied.
- Continuing to send at `0.75x` after the queue is drained would unnecessarily underutilise the link. BBR therefore transitions immediately to Phase 2 (`1.0x` cruise) as soon as in-flight reaches BDP.

</details>

### Q4. Why does full BBR choose a random initial `cycle_idx`, whereas this lab starts at 0? What is the purpose of randomisation?

When transitioning from `BBR_DRAIN` to `BBR_PROBE_BW`, the upstream Linux BBR implementation selects a random starting index among the cruise phases (phases 2 to 7). Why is this random offset necessary?

<details>
<summary>Hint and Answer</summary>

- **To prevent synchronisation among competing BBR flows**.
- If multiple BBR connections sharing a bottleneck were to enter Phase 0 (`1.25x`) at the exact same moment, all flows would simultaneously inject 25% extra traffic, inducing large queue spikes and heavy loss.
- By randomising each flow's starting phase offset, probe phases are desynchronised. Flows take turns probing the bottleneck at different times, yielding steady aggregate link utilisation and predictable delay.
- In this lab, to ensure deterministic, reproducible packetdrill assertions (verifying Phase 0 → Phase 1 → Phase 2 transitions), we deliberately initialise `ca->cycle_idx = 0`.

</details>

---

## Step-by-step Walkthrough

### 1. The `bbr_pacing_gain[]` Array

In steady state (`BBR_PROBE_BW`), BBR cycles through an 8-entry pacing gain table:

```c
#define CYCLE_LEN 8

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

### 2. `bbr_is_next_cycle_phase()` — Evaluating Phase Completion

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

### 3. `bbr_advance_cycle_phase()` and `bbr_update_cycle_phase()` — Advancing Phases

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

### 4. Updating Gains in `bbr_update_gains()`

```c
	case BBR_PROBE_BW:
	default:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & 7];
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
```

In `BBR_PROBE_BW`, `ca->pacing_gain` is looked up from `bbr_pacing_gain[]` using `ca->cycle_idx & 7`. `ca->cwnd_gain` remains fixed at `bbr_cwnd_gain` (`2.0x`).

---

## Understanding `test.pkt`

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

## How to run

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh bbr/step04-probe-bw starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh bbr/step04-probe-bw solution
```
