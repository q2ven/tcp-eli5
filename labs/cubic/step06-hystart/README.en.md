# Step 6: HyStart (Early Slow Start Exit via ACK Train and Delay Increase)

## Goal

Through Step 5, we have built a complete Congestion Avoidance engine featuring the cubic curve, Fast Convergence, Reno-friendly region, and idle epoch shifting.
However, up to this point, CUBIC still suffers from a critical drawback: **it severely overshoots network buffers during the initial Slow Start**.

Traditional Slow Start (as in Reno and standard TCP) doubles `snd_cwnd` every round-trip time (exponential growth). This exponential increase does not cease when the bandwidth-delay product (BDP) is reached; rather, **it accelerates until the bottleneck buffer overflows and packets are dropped**.
Especially on high-BDP paths (high bandwidth and long delay) or switches with shallow buffers, the number of in-flight packets at the end of Slow Start can reach up to twice the BDP. When the buffer suddenly overflows, massive bursts of packets are dropped simultaneously, forcing the connection into prolonged recovery or retransmission timeouts (RTO).

**HyStart (Hybrid Slow Start)** is an algorithm designed to detect early indicators of network buffer saturation **prior** to packet loss, smoothly transitioning the connection from Slow Start into Congestion Avoidance.
In this step, we implement HyStart's two primary detectors—**ACK Train detection** and **Delay Increase detection**—in BPF via the `struct tcp_congestion_ops` `.pkts_acked` hook.

---

## Design Checkpoints (Think before coding)

Before opening `starter.bpf.c`, think through the following four questions.

### Q1. Why must initial Slow Start not be allowed to continue until packet loss? (The Slow Start Overshoot Problem)

Slow Start doubles the congestion window every RTT. Why is the classic approach—continuing until loss occurs and then halving (or multiplying by 0.7) into Congestion Avoidance—problematic?

<details>
<summary>Hint & Answer</summary>

During Slow Start, the sender transmits two new packets for every ACK received.
If $W$ denotes the congestion window at the exact moment the bottleneck pipe (BDP) becomes full, the sender will attempt to transmit $2W$ packets across the subsequent RTT.
If the bottleneck router buffer holds less than $W$ packets, nearly $W$ excess packets are tail-dropped in a sudden burst.

Such a massive burst loss cannot typically be repaired by SACK Fast Recovery alone; it frequently causes retransmission timeouts (RTO), dropping `cwnd` all the way down to 1. Consequently, the connection experiences substantial packet stall and severely degraded throughput right after startup.
HyStart avoids this catastrophe by setting `ssthresh = cwnd` as soon as queueing delay begins to build up, **exiting Slow Start into Congestion Avoidance with zero packet loss**.

</details>

### Q2. Why combine two independent detection mechanisms (ACK Train and Delay Increase)?

HyStart incorporates both ACK Train detection and Delay Increase detection. Why is neither detector sufficient on its own?

<details>
<summary>Hint & Answer</summary>

The two detectors complement each other's inherent limitations:

- **ACK Train detection**: When bottleneck capacity saturates, packets are forwarded spaced by the bottleneck transmission delay. Consequently, returning ACKs arrive closely spaced at the bottleneck service rate—forming a dense "train". If this tightly spaced ACK train spans at least $\text{delay\_min} / 2$ from the start of the round, in-flight data has filled the BDP. However, on lower-bandwidth paths or networks with ACK coalescing/filtering (e.g. LRO/GRO, ACK suppression), ACK spacing can become distorted.
- **Delay Increase detection**: As packets queue up in the bottleneck buffer, round-trip times lengthen. When the minimum RTT observed during a round exceeds the connection's baseline `delay_min` by a calibrated threshold, queueing delay is reliably identified. However, wireless links (Wi-Fi, cellular) often experience transient transmission jitter and scheduling delays that could trigger false positives.

By monitoring both the throughput signal (ACK train spacing) and the latency signal (RTT inflation), HyStart achieves robust and accurate detection across heterogeneous network environments.

</details>

### Q3. Why must the round reset check (`after(tp->snd_una, ca->end_seq)`) be placed before checking `cwnd < 16`?

In `hystart_update()`, testing for round completion via `after(tp->snd_una, ca->end_seq)` precedes the `tp->snd_cwnd < HYSTART_LOW_WINDOW` (16) check. What bug arose in the Linux kernel when these checks were inverted?

<details>
<summary>Hint & Answer</summary>

This addresses a subtle kernel bug fixed in commit `25c1a9ca53db`.
If the `cwnd < 16` check were evaluated before the round reset, `hystart_update()` would return immediately throughout the initial phase of the connection (where `cwnd` typically starts at 10 and remains below 16). Consequently, `end_seq` would never be updated to demarcate rounds.

When `cwnd` finally reached 16, `end_seq` would still hold the sequence number from connection startup. The first active round would then span multiple round trips, retaining stale timestamps and invalid RTT samples, leading to premature and spurious detections.
Evaluating the round boundary first ensures that round demarcations advance properly on every ACK, so that detection begins from a clean, properly initialised round state as soon as `cwnd >= 16`.

</details>

### Q4. In Delay Increase detection, why collect at least 8 samples and use a threshold clamped to `[4ms, 16ms]`?

Delay Increase detection does not trigger on a single elevated RTT sample. Instead, it tracks the minimum RTT (`curr_rtt`) across at least 8 samples (`HYSTART_MIN_SAMPLES = 8`) and compares it against `delay_min + clamp(delay_min >> 3, 4000U, 16000U)`. What is the rationale for this design?

<details>
<summary>Hint & Answer</summary>

- **Minimum of at least 8 samples (`curr_rtt`)**: An individual ACK may experience transient operating system scheduling delays or NIC interrupt moderation jitter. Taking the minimum over 8 or more samples filters out transient spikes and accurately measures persistent buffer queueing.
- **`delay_min >> 3` (12.5% proportional threshold)**: On long-distance paths (e.g. 100 ms RTT), 12.5% corresponds to 12.5 ms. Scaling the threshold with baseline latency allows reasonable buffer utilisation without triggering premature slow start exits.
- **`clamp(..., 4000U, 16000U)` (4 ms to 16 ms boundaries)**:
  - **Lower bound (4 ms)**: In data-centre or LAN environments with sub-millisecond RTTs, $1\text{ ms} \gg 3 = 125\ \mu\text{s}$. TSO/GRO packet aggregation delay alone could easily exceed $125\ \mu\text{s}$ and trigger false exits. Enforcing a 4 ms floor eliminates spurious detections caused by local aggregation.
  - **Upper bound (16 ms)**: On very high-latency paths such as geostationary satellite links (e.g. 600 ms RTT), $600\text{ ms} \gg 3 = 75\text{ ms}$. Waiting for 75 ms of queue backlog before reacting would introduce severe bufferbloat and risk packet drops. A 16 ms ceiling prevents excessive buffer accumulation.
  (These constants—4 ms, 16 ms, and 8 samples—were subsequently standardised in RFC 9406 HyStart++).

</details>

---

## Step-by-step Walkthrough (`solution.bpf.c`)

### 1. Constants and Data Structures

```c
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	4000U	/* 4 ms */
#define HYSTART_DELAY_MAX	16000U	/* 16 ms */
#define HYSTART_LOW_WINDOW	16
#define HYSTART_ACK_DELTA_US	2000U	/* 2 ms: maximum ACK spacing for train */
```

HyStart fields in `struct bictcp` (defined in `vmlinux.h`):
- `round_start`: Timestamp when the current round commenced ($\mu\text{s}$).
- `end_seq`: Sequence number terminating the current round (`tp->snd_nxt`).
- `last_ack`: Timestamp of the most recent closely spaced ACK ($\mu\text{s}$).
- `curr_rtt`: Minimum RTT observed in the current round ($\mu\text{s}$, initialised to `~0U`).
- `sample_cnt`: Number of valid RTT samples collected in the current round.
- `found`: Flag indicating that HyStart has detected the Slow Start exit point (1 = detected).

### 2. `hystart_reset()`: Round Initialisation

```c
static __always_inline void hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 now_us = tp->tcp_mstamp;

	ca->round_start = ca->last_ack = now_us;
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0U;
	ca->sample_cnt = 0;
}
```

- `now_us`: Microsecond timestamp from `tp->tcp_mstamp`.
- `end_seq = tp->snd_nxt`: When all data transmitted up to this sequence number has been acknowledged, one complete RTT of data has traversed the network.
- `curr_rtt = ~0U`: Initialising to `0xFFFFFFFF` allows unconditional `if (ca->curr_rtt > delay)` assignment on the first sample without an extra branch.

### 3. `hystart_update()`: The Dual Detectors

```c
static __always_inline void hystart_update(struct sock *sk, __u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 now_us = tp->tcp_mstamp;

	if (after(tp->snd_una, ca->end_seq))
		hystart_reset(sk);

	if (tp->snd_cwnd < HYSTART_LOW_WINDOW)
		return;

	/* Detector 1: HYSTART_ACK_TRAIN */
	if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US) {
		ca->last_ack = now_us;
		if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
			ca->found = 1;
			tp->snd_ssthresh = tp->snd_cwnd;
		}
	}

	/* Detector 2: HYSTART_DELAY */
	if (ca->curr_rtt > delay)
		ca->curr_rtt = delay;

	if (ca->sample_cnt < HYSTART_MIN_SAMPLES)
		ca->sample_cnt++;

	if (ca->sample_cnt >= HYSTART_MIN_SAMPLES) {
		if (ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)) {
			ca->found = 1;
			tp->snd_ssthresh = tp->snd_cwnd;
		}
	}
}
```

- **Round boundary check**: If `after(tp->snd_una, ca->end_seq)` is true, an ACK beyond the previous round's boundary has arrived. `hystart_reset(sk)` begins a fresh round.
- **Window threshold**: Returns immediately if `tp->snd_cwnd < HYSTART_LOW_WINDOW` (16).
- **Detector 1 (ACK Train)**:
  - If the spacing between successive ACKs is within 2 ms (`now_us - ca->last_ack <= 2000`), the train continues and `last_ack` is updated. A gap exceeding 2 ms breaks the train for the remainder of the round.
  - If train duration (`now_us - ca->round_start`) reaches half of `ca->delay_min`, in-flight data has filled the bottleneck pipe: `ca->found = 1; tp->snd_ssthresh = tp->snd_cwnd;`.
- **Detector 2 (Delay Increase)**:
  - Tracks the minimum RTT sample in `ca->curr_rtt`.
  - Once `sample_cnt` reaches 8, it evaluates `ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, 4000U, 16000U)`.
  - If queueing delay exceeds the threshold, `ca->found = 1; tp->snd_ssthresh = tp->snd_cwnd;` terminates Slow Start.

### 4. `bpf_cubic6_acked()`: Invocation Hook

```c
SEC("struct_ops")
void BPF_PROG(bpf_cubic6_acked, struct sock *sk, const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	__u32 delay;

	if (sample->rtt_us < 0)
		return;

	delay = sample->rtt_us ? (__u32)sample->rtt_us : 1U;

	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH &&
	    tcp_in_slow_start(tp) && !ca->found)
		hystart_update(sk, delay);
}
```

- `hystart_update()` is invoked only during initial Slow Start (`tp->snd_ssthresh == TCP_INFINITE_SSTHRESH`, `tcp_in_slow_start(tp)`, and `!ca->found`).
- Once HyStart fires and clamps `snd_ssthresh = snd_cwnd`, `tcp_in_slow_start(tp)` evaluates to false, ensuring `hystart_update()` is never called again for the lifetime of that connection phase.

---

## Understanding `test.pkt`

`test.pkt` tests the exact behaviour of HyStart Delay Increase detection:

```text
0.00 `sysctl -q net.ipv4.tcp_congestion_control=bpf_cubic6`
```

1. **Three-way Handshake**:
   - Establishes connection with MSS 1012 and receive window 65535.
2. **Round 1 (Baseline delay_min and Window Growth)**:
   - Application issues `write()` for 30 segments (30360 bytes).
   - Initial window `cwnd = 10` causes the first 10 segments (1:10121) to be transmitted immediately.
   - At 20 ms (`+.020`), an ACK for 10121 arrives, establishing `delay_min = 20000` (20 ms).
   - Slow Start doubles `cwnd` from 10 to 20 (`>= 16`), triggering the immediate transmission of the remaining 20 segments (10121:30361).
   - Assertions confirm `tcpi_snd_cwnd == 20` and `tcpi_snd_ssthresh == 0x7fffffff`.
3. **Round 2 (HyStart Delay Trigger)**:
   - After 35 ms (`+.035`, representing network queue inflation), the first Round 2 ACK (ack 11133) arrives.
   - `after(11133, 10121)` starts Round 2, resetting `sample_cnt = 0` and updating `round_start`.
   - The subsequent 8 ACKs (ack 12145 through 19229) arrive in a rapid burst (`+0.000`).
   - Upon the 9th ACK (ack 19229), `sample_cnt` has reached 8, and the round's minimum RTT (35 ms) exceeds `delay_min + 4ms` (20 ms + 4 ms = 24 ms).
   - Delay Increase detection triggers! `tp->snd_ssthresh = tp->snd_cwnd` (28) executes.
   - Assertions verify `tcpi_snd_ssthresh == tcpi_snd_cwnd` and `tcpi_snd_ssthresh < 0x7fffffff`.
   - Slow Start has safely terminated without a single dropped packet!

---

## How to Run

### 1. Verify that starter fails on assertion

```sh
../../run_test.sh cubic/step06-hystart starter
```

Because `starter.bpf.c` leaves `hystart_update()` as an empty stub, `ssthresh` remains at `0x7fffffff`. The packetdrill assertion fails.

### 2. Verify that solution passes

```sh
../../run_test.sh cubic/step06-hystart solution
```

HyStart activates as intended, displaying `=== [PASS] cubic/step06-hystart (solution) ===`.
