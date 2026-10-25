# Step 4: HyStart (Early Slow Start Exit via ACK Train and Delay Increase)

## Goal

Through Step 3, we have built a complete Congestion Avoidance engine featuring the CUBIC 3rd-degree polynomial curve, Fast Convergence, the Reno-friendly region, and idle epoch shifting (see the [CUBIC Track Overview](../README.md) for the full track structure).
However, our implementation still suffers from a major flaw during connection startup: **exponential growth in the initial Slow Start phase severely overshoots bottleneck router buffers**. In this step, you will implement **HyStart (Hybrid Slow Start)** inside the `.pkts_acked` hook (`bpf_cubic6_acked()` and `hystart_update()`) to detect early signs of network saturation—via **ACK Train** spacing and **Delay Increase**—and smoothly transition from Slow Start into Congestion Avoidance before any packet loss occurs.

---

## How the Algorithm Works (Read This First)

### 1. Why Does Initial Slow Start Cause Massive Loss? (The Overshoot Problem)

When a new TCP connection starts, the sender has no knowledge of the network's Bandwidth-Delay Product (BDP), so it begins in **Slow Start**.
Initially, the slow start threshold `tp->snd_ssthresh` is set to `TCP_INFINITE_SSTHRESH` (`0x7fffffff`). As long as `tp->snd_cwnd < tp->snd_ssthresh`, the kernel's `tcp_slow_start()` increases `tp->snd_cwnd` by 1 for every acknowledged packet—**doubling `snd_cwnd` every round-trip time ($$10 \to 20 \to 40 \to 80 \dots$$)**.

This creates a severe overshoot problem:

- Suppose that at the start of a round, `snd_cwnd` reaches the exact BDP of the path ($$W_{\text{BDP}}$$), completely filling the bottleneck pipe.
- Because no packet has been dropped yet, Slow Start continues uninterrupted. As those $$W_{\text{BDP}}$$ packets are acknowledged over the course of the RTT, `snd_cwnd` **doubles to $$2 \times W_{\text{BDP}}$$**.
- If the bottleneck switch or router buffer holds fewer than $$W_{\text{BDP}}$$ packets, up to $$W_{\text{BDP}}$$ excess packets overflow the buffer and are tail-dropped in a single massive burst.
- Recovering from such a large multi-packet loss often overwhelms SACK Fast Recovery and triggers a Retransmission Timeout (RTO), collapsing `snd_cwnd` to 1 and stalling the connection right after startup.

**The Core Idea of HyStart (Hybrid Slow Start)**:
Instead of blindly doubling `snd_cwnd` until packets are dropped, HyStart monitors returning ACKs on a per-round (1 RTT) basis during the initial Slow Start (`tp->snd_ssthresh == TCP_INFINITE_SSTHRESH`). As soon as it detects that the bottleneck pipe is full or that a router queue is starting to build, it sets `tp->snd_ssthresh = tp->snd_cwnd` (and marks `ca->found = 1`), exiting Slow Start into Congestion Avoidance with **zero packet loss**.

---

### 2. Defining and Tracking a "Round" (1 RTT Flight)

HyStart organises its measurements into 1-RTT rounds using TCP sequence numbers:

1. **Initialising a Round (`hystart_reset()`)**:
   At the beginning of a round, HyStart snapshots the next sequence number to be sent (`tp->snd_nxt`) into `ca->end_seq`, and saves the current microsecond timestamp (`now_us = tp->tcp_mstamp`) into both `ca->round_start` and `ca->last_ack`. It also resets the current round's minimum RTT `ca->curr_rtt` to `~0U` (`0xffffffff`) and the sample counter `ca->sample_cnt` to `0`.
2. **Detecting Round Completion**:
   When an arriving ACK advances the oldest unacknowledged sequence number `tp->snd_una` past `ca->end_seq` (`after(tp->snd_una, ca->end_seq)` is true), every packet in flight when the round began has been acknowledged. HyStart immediately calls `hystart_reset(sk)` to start the next round.
3. **Minimum Window Requirement (`HYSTART_LOW_WINDOW = 16`) and Ordering**:
   When `snd_cwnd` is very small (`tp->snd_cwnd < 16`), there are too few packets in flight to form a reliable ACK train or RTT distribution, so HyStart skips its detectors.
   **Crucially, the round boundary check (`if (after(tp->snd_una, ca->end_seq)) hystart_reset(sk);`) MUST be executed BEFORE checking `if (tp->snd_cwnd < HYSTART_LOW_WINDOW) return;`.**
   If `hystart_update()` returned early on `snd_cwnd < 16` before advancing the round, `ca->end_seq` and `ca->round_start` would remain frozen at their initial connection-startup values while `snd_cwnd` grew from 10 to 16. The first active round at `snd_cwnd >= 16` would then use a stale `round_start` from several RTTs ago, causing false detections (a classic Linux kernel bug fixed in commit `25c1a9ca53db`).

---

### 3. HyStart's Two Complementary Detectors

HyStart runs two independent detectors in each round once `tp->snd_cwnd >= 16`. If **either** detector triggers, HyStart sets `ca->found = 1` and `tp->snd_ssthresh = tp->snd_cwnd`.

```text
  [Sender]                   [Bottleneck Router]                [Receiver]
  Round starts (round_start)
  Sends burst of packets ───► Fills pipe & starts queuing ─────► Data arrives
                                                                      │
  ◄────────────────────────── Closely spaced ACK Train returns ───────┘
  |◄── ACK delta <= 2ms ──►|
  |◄────── Train duration >= delay_min / 2  ==> [Detector 1 Fires!]
  |
  |  Min RTT over >= 8 samples (curr_rtt) >= delay_min + thresh ==> [Detector 2 Fires!]
```

#### Detector 1: ACK Train Length (`HYSTART_ACK_TRAIN`)

- **Intuition**:
  During Slow Start, packets sent in a round traverse the bottleneck link back-to-back at the bottleneck bandwidth rate. Their corresponding ACKs return to the sender closely spaced (within `HYSTART_ACK_DELTA_US = 2000` $$\mu\text{s}$$ = $$2\text{ ms}$$ of one another), forming a dense "ACK train".
  The total duration of this continuous ACK train from `ca->round_start` reflects how long it took the bottleneck link to serialise the entire flight of packets. Because the minimum round-trip propagation time `ca->delay_min` comprises the forward data path plus the reverse ACK path (roughly $$\text{delay}_{\min} / 2$$ in each direction), **an unbroken ACK train lasting at least $$\text{delay}_{\min} / 2$$ indicates that the flight of packets has filled the forward bottleneck pipe**.
- **Exact Rule and Formula**:
  1. Check whether the time elapsed since the previous ACK in the train (`ca->last_ack`) is at most `HYSTART_ACK_DELTA_US` ($$2000\ \mu\text{s}$$):
     ```c
     if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US)
     ```
  2. If so, extend the train by updating `ca->last_ack = now_us` (if an ACK arrives more than $$2\text{ ms}$$ after `ca->last_ack`, `ca->last_ack` is not updated, so `now_us - ca->last_ack` grows even larger on subsequent ACKs and the train detector cannot fire for the rest of that round).
  3. Check whether the train duration since `ca->round_start` has reached at least half of `ca->delay_min` (`ca->delay_min >> 1`):
     ```c
     if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
         ca->found = 1;
         tp->snd_ssthresh = tp->snd_cwnd;
     }
     ```

#### Detector 2: Delay Increase (`HYSTART_DELAY`)

- **Intuition**:
  On links where ACK coalescing (GRO/LRO) or ACK thinning disrupts ACK spacing, the ACK train detector may not fire. However, as soon as in-flight packets exceed the BDP and begin queueing in the bottleneck buffer, **the measured RTT increases above the baseline minimum RTT (`ca->delay_min`)**.
  To avoid false exits caused by a single delayed ACK (e.g., OS scheduling jitter or NIC interrupt moderation), HyStart collects at least `HYSTART_MIN_SAMPLES` (`8`) RTT samples within the current round and tracks their **minimum** (`ca->curr_rtt`). Only if even the smallest of the first 8+ RTT samples in the round exceeds `ca->delay_min` by a safety threshold does HyStart declare that a persistent queue has formed.
- **Exact Rule and Formula**:
  1. Update the current round's minimum RTT `ca->curr_rtt` with the latest ACK's RTT `delay` ($$\mu\text{s}$$):
     ```c
     if (ca->curr_rtt > delay)
         ca->curr_rtt = delay;
     ```
  2. Increment the round sample counter `ca->sample_cnt` up to `HYSTART_MIN_SAMPLES` (`8`):
     ```c
     if (ca->sample_cnt < HYSTART_MIN_SAMPLES)
         ca->sample_cnt++;
     ```
  3. Once `ca->sample_cnt >= HYSTART_MIN_SAMPLES` (at least 8 samples collected in this round), compare `ca->curr_rtt` against `ca->delay_min` plus a clamped threshold $$\Delta_{\text{thresh}}$$:
     $$\Delta_{\text{thresh}} = \text{clamp}\left(\frac{\text{delay}_{\min}}{8},\ 4000\ \mu\text{s},\ 16000\ \mu\text{s}\right)$$
     ```c
     if (ca->curr_rtt >= ca->delay_min + clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)) {
         ca->found = 1;
         tp->snd_ssthresh = tp->snd_cwnd;
     }
     ```
  - **Why clamp `delay_min >> 3` ($$12.5\%$$) between $$4\text{ ms}$$ and $$16\text{ ms}$$?**:
    - By default, HyStart tolerates a queueing delay of $$1/8$$ ($$12.5\%$$) of the baseline RTT (`ca->delay_min >> 3`).
    - **Lower bound `HYSTART_DELAY_MIN = 4000U` ($$4\text{ ms}$$)**: On low-latency data-centre or LAN paths where $$\text{delay}_{\min} \le 1\text{ ms}$$, $$\text{delay}_{\min} / 8$$ is only $$125\ \mu\text{s}$$—smaller than normal TSO/GRO batching jitter. Enforcing a $$4\text{ ms}$$ floor prevents premature Slow Start exits.
    - **Upper bound `HYSTART_DELAY_MAX = 16000U` ($$16\text{ ms}$$)**: On long-delay paths (e.g., transcontinental or satellite links with $$\text{delay}_{\min} > 200\text{ ms}$$), waiting for $$\text{delay}_{\min} / 8 > 25\text{ ms}$$ of queue buildup would cause excessive bufferbloat, so the threshold is capped at $$16\text{ ms}$$.

---

### 4. Concrete Numerical Example (Delay Increase Detection)

Let us trace a connection with a baseline minimum RTT of $$20\text{ ms}$$ (`delay_min = 20000` $$\mu\text{s}$$):

- **Computing the delay threshold**:
  `delay_min >> 3` is $$20000 / 8 = 2500\ \mu\text{s}$$ ($$2.5\text{ ms}$$). Clamping `2500` to `[4000U, 16000U]` selects the lower bound **`4000` $$\mu\text{s}$$ ($$4\text{ ms}$$)**.
  Therefore, Delay Increase detection will fire if `curr_rtt >= 20000 + 4000 = 24000` $$\mu\text{s}$$ (**$$24\text{ ms}$$**) after 8 samples in a round.
- **Round 1 (`snd_cwnd: 10 -> 20`)**:
  The sender transmits its initial window of 10 segments. After $$20\text{ ms}$$, a cumulative ACK arrives: `ca->delay_min = 20000` is recorded, `snd_cwnd` doubles from `10` to `20` ($$\ge 16$$), and the sender transmits the next 20 segments.
- **Round 2 (`snd_cwnd >= 20`, queue builds up and RTT rises to $$35\text{ ms}$$)**:
  - **1st ACK arrives** (`delay = 35000` $$\mu\text{s}$$): `after(tp->snd_una, ca->end_seq)` is true, so `hystart_reset()` starts Round 2. `ca->curr_rtt` becomes `35000` and `ca->sample_cnt` becomes `1`.
  - **2nd through 7th ACKs arrive** (`delay = 35000` $$\mu\text{s}$$): `ca->sample_cnt` increments to `7`. Because fewer than 8 samples have been collected, `snd_ssthresh` remains `0x7fffffff` (while `snd_cwnd` grows by $$+1$$ per ACK).
  - **8th sample ACK arrives** (`delay = 35000` $$\mu\text{s}$$): `ca->sample_cnt` reaches `8` (`HYSTART_MIN_SAMPLES`). Because `ca->curr_rtt` ($$35000\ \mu\text{s}$$) $$\ge$$ `24000` $$\mu\text{s}$$, **HyStart immediately sets `ca->found = 1` and `tp->snd_ssthresh = tp->snd_cwnd`, exiting Slow Start with zero packet loss!**

---

### 5. Kernel Structs, Fields, and Helpers Provided for This Step

Before answering the design questions in Section 3, review the exact kernel fields and helper macros already available in your BPF environment:

1. **`struct tcp_sock *tp = tcp_sk(sk)` (TCP socket state)**:
   - `tp->snd_cwnd` (`__u32`): Current congestion window in packets.
   - `tp->snd_ssthresh` (`__u32`): Slow start threshold, initialised to `TCP_INFINITE_SSTHRESH` (`0x7fffffff`). Setting `tp->snd_ssthresh = tp->snd_cwnd` causes `tcp_in_slow_start(tp)` (`tp->snd_cwnd < tp->snd_ssthresh`) to become false, transitioning the socket into Congestion Avoidance.
   - `tp->snd_una` (`__u32`): Oldest unacknowledged sequence number.
   - `tp->snd_nxt` (`__u32`): Next sequence number to be sent (saved into `ca->end_seq` when a round starts).
   - `tp->tcp_mstamp` (`__u64`, cast to `__u32 now_us`): Current microsecond timestamp maintained by the TCP stack.
2. **`const struct ack_sample *sample` (passed to `.pkts_acked`)**:
   - `sample->rtt_us` (`__s32`): RTT measured from this ACK in microseconds (`< 0` if no valid RTT measurement was produced by this ACK).
3. **`icsk_ca_priv` and `struct bictcp *ca = inet_csk_ca(sk)`**:
   - Every `struct inet_connection_sock` includes a per-socket private storage area `icsk_ca_priv` for congestion control state, accessed via `inet_csk_ca(sk)`.
   - In this lab, we use the kernel's `struct bictcp` definition from `vmlinux.h`, which provides:
     - `ca->delay_min` (`__u32`): Minimum RTT observed over the lifetime of the connection ($$\mu\text{s}$$).
     - `ca->round_start` (`__u32`): Timestamp (`tp->tcp_mstamp` in $$\mu\text{s}$$) when the current round started.
     - `ca->last_ack` (`__u32`): Timestamp ($$\mu\text{s}$$) of the most recent ACK in the current ACK train.
     - `ca->end_seq` (`__u32`): Sequence number (`tp->snd_nxt`) marking the end of the current round.
     - `ca->curr_rtt` (`__u32`): Minimum RTT ($$\mu\text{s}$$) observed among samples in the current round (initialised to `~0U` in `hystart_reset()`).
     - `ca->sample_cnt` (`__u32`): Number of RTT samples collected in the current round.
     - `ca->found` (`__u8`): Flag set to `1` once either HyStart detector triggers.
4. **Inline helpers in `bpf_cc_helpers.h`**:
   - `after(seq2, seq1)`: Wraparound-safe 32-bit sequence number comparison (`(__s32)(seq1 - seq2) < 0`).
   - `clamp(val, lo, hi)`: Clamps `val` to the inclusive range `[lo, hi]`.
   - `tcp_in_slow_start(tp)`: Returns `tp->snd_cwnd < tp->snd_ssthresh`.

---

## Design Checkpoints (Translate the Algorithm into Code)

Using the concepts and kernel fields introduced in Section 2, think through how to implement the `TODO` in `hystart_update()` inside `starter.bpf.c`.

### Q1. Under what exact conditions does `bpf_cubic6_acked()` invoke `hystart_update()`?

In `bpf_cubic6_acked()` (already provided in `starter.bpf.c`), after updating `ca->delay_min`, three conditions must hold before calling `hystart_update(sk, delay)`. What are those three conditions, and why is each one needed?

<details>
<summary>Hint and Answer</summary>

`bpf_cubic6_acked()` guards the call with:

```c
if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH &&
    tcp_in_slow_start(tp) && !ca->found)
	hystart_update(sk, delay);
```

- `tp->snd_ssthresh == TCP_INFINITE_SSTHRESH`: Ensures HyStart only runs during the **initial Slow Start** of a connection (later Slow Starts after a timeout already have a finite `snd_ssthresh` derived from prior congestion).
- `tcp_in_slow_start(tp)`: Verifies that `tp->snd_cwnd < tp->snd_ssthresh`.
- `!ca->found`: Ensures HyStart stops running once either detector has already triggered.

</details>

### Q2. In what order must `hystart_update()` check the round boundary (`ca->end_seq`) and `HYSTART_LOW_WINDOW` (`16`)?

At the start of `hystart_update(struct sock *sk, __u32 delay)`, we need to check whether `tp->snd_una` has passed `ca->end_seq` (calling `hystart_reset(sk)` if so) and whether `tp->snd_cwnd < HYSTART_LOW_WINDOW` (returning early if so). Which `if` statement must come first, and why?

<details>
<summary>Hint and Answer</summary>

The round boundary check **must come first**, before the `HYSTART_LOW_WINDOW` check:

```c
if (after(tp->snd_una, ca->end_seq))
	hystart_reset(sk);

if (tp->snd_cwnd < HYSTART_LOW_WINDOW)
	return;
```

Advancing the round on every ACK—even while `snd_cwnd < 16`—ensures that `ca->end_seq` and `ca->round_start` are always up to date when `snd_cwnd` reaches `16`.

</details>

### Q3. How do we write Detector 1 (`HYSTART_ACK_TRAIN`) in C?

Using `__u32 now_us = tp->tcp_mstamp;`, write the C statements for Detector 1: check if the gap since `ca->last_ack` is at most `HYSTART_ACK_DELTA_US` (`2000U`), update `ca->last_ack`, and if the elapsed time since `ca->round_start` is at least half of `ca->delay_min`, exit Slow Start.

<details>
<summary>Hint and Answer</summary>

Cast timestamp differences to `(__s32)` for wraparound safety, and use `ca->delay_min >> 1` for half of the minimum RTT:

```c
/* Detector 1: HYSTART_ACK_TRAIN */
if ((__s32)(now_us - ca->last_ack) <= HYSTART_ACK_DELTA_US) {
	ca->last_ack = now_us;
	if ((__s32)(now_us - ca->round_start) >= (ca->delay_min >> 1)) {
		ca->found = 1;
		tp->snd_ssthresh = tp->snd_cwnd;
	}
}
```

</details>

### Q4. How do we write Detector 2 (`HYSTART_DELAY`) in C?

Using the `delay` parameter ($$\mu\text{s}$$), write the C statements for Detector 2: update the round minimum `ca->curr_rtt`, increment `ca->sample_cnt` up to `HYSTART_MIN_SAMPLES` (`8`), and once `ca->sample_cnt >= HYSTART_MIN_SAMPLES`, check whether `ca->curr_rtt` exceeds `ca->delay_min` by at least `clamp(ca->delay_min >> 3, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)`.

<details>
<summary>Hint and Answer</summary>

Because `ca->curr_rtt` is initialised to `~0U` in `hystart_reset()`, a simple `if (ca->curr_rtt > delay)` tracks the minimum RTT in the round:

```c
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
```

</details>

---

## Step-by-step Code Walkthrough (`solution.bpf.c`)

### 1. HyStart Constants

```c
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	4000U
#define HYSTART_DELAY_MAX	16000U
#define HYSTART_LOW_WINDOW	16
#define HYSTART_ACK_DELTA_US	2000U
```

- `HYSTART_LOW_WINDOW` (`16`): Minimum `snd_cwnd` required before running the HyStart detectors.
- `HYSTART_ACK_DELTA_US` (`2000U` = $$2\text{ ms}$$): Maximum inter-ACK spacing for consecutive ACKs to belong to the same train.
- `HYSTART_MIN_SAMPLES` (`8`): Minimum number of RTT samples required in a round before evaluating Delay Increase detection.
- `HYSTART_DELAY_MIN` (`4000U` = $$4\text{ ms}$$) and `HYSTART_DELAY_MAX` (`16000U` = $$16\text{ ms}$$): Bounds for clamping the dynamic delay threshold `ca->delay_min >> 3`.

### 2. `hystart_reset()` — Initialising a New Round

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

- Called from `bpf_cubic6_init()` at connection initialisation, from `bpf_cubic6_state()` on `TCP_CA_Loss`, and from `hystart_update()` whenever `tp->snd_una` advances past `ca->end_seq`.
- Sets `ca->round_start` and `ca->last_ack` to `tp->tcp_mstamp`, records `ca->end_seq = tp->snd_nxt`, and resets `ca->curr_rtt = ~0U` and `ca->sample_cnt = 0`.

### 3. `hystart_update()` — Round Management and Dual Detectors

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

- Advances the round via `hystart_reset(sk)` when `after(tp->snd_una, ca->end_seq)` is true.
- Returns early if `tp->snd_cwnd < HYSTART_LOW_WINDOW` (`16`).
- Evaluates Detector 1 (`HYSTART_ACK_TRAIN`) and Detector 2 (`HYSTART_DELAY`). When either detector triggers, sets `ca->found = 1` and `tp->snd_ssthresh = tp->snd_cwnd`.

### 4. `bpf_cubic6_acked()` — `.pkts_acked` Callback

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

- Because the Linux TCP stack invokes `.pkts_acked` **before** `.cong_avoid` on each incoming ACK, setting `tp->snd_ssthresh = tp->snd_cwnd` inside `hystart_update()` causes `tcp_in_slow_start(tp)` in `bpf_cubic6_cong_avoid()` to immediately evaluate to false on that very same ACK, smoothly handing control over to `bictcp_update()` and `tcp_cong_avoid_ai()`.

---

## Understanding `test.pkt` and How to Run

### Packet Trace Walkthrough (`test.pkt`)

`test.pkt` verifies that HyStart Delay Increase detection exits Slow Start early when RTT inflates from a $$20\text{ ms}$$ baseline to $$35\text{ ms}$$:

1. **Three-way Handshake (`0.000s` – `0.020s`)**:
   - Sets `net.ipv4.tcp_congestion_control=bpf_cubic6` and establishes a connection with MSS 1012 bytes.
   - `bpf_cubic6_init()` calls `hystart_reset()`, setting `end_seq = 1`.
2. **Round 1: Establishing `delay_min = 20 ms` and Doubling `cwnd: 10 -> 20` (`0.020s` – `0.040s`)**:
   - The application writes 30360 bytes (30 segments of 1012 bytes). With initial `snd_cwnd = 10`, the kernel sends the first 10 segments (`1:10121`).
   - At `+.020` ($$20\text{ ms}$$ later), `ack 10121` arrives:
     - `bpf_cubic6_acked()` records `ca->delay_min = 20000` ($$20\text{ ms}$$).
     - Inside `hystart_update()`, `after(10121, 1)` is true, so `hystart_reset()` advances `ca->end_seq = 10121` (since `snd_cwnd == 10 < 16`, the detectors do not run yet).
     - `bpf_cubic6_cong_avoid()` doubles `snd_cwnd` from `10` to `20`, and the kernel transmits the remaining 20 segments (`10121:30361`).
     - Assertions verify `tcpi_snd_cwnd == 20` and `tcpi_snd_ssthresh == 0x7fffffff`.
3. **Round 2: Queue Inflation ($$35\text{ ms}$$) and HyStart Trigger on the 8th Sample (`0.075s`)**:
   - At `+.035` ($$35\text{ ms}$$ RTT), the first Round 2 ACK (`ack 11133`) arrives:
     - `after(11133, 10121)` is true, so `hystart_reset()` starts Round 2 with `end_seq = 30361`, `sample_cnt = 0`, and `curr_rtt = ~0U`.
     - Because `snd_cwnd == 20 >= 16`, the detectors run: `curr_rtt` is updated to `35000` and `sample_cnt` becomes `1`. `tcpi_snd_ssthresh` is still `0x7fffffff`.
   - Next, 7 individual 1-MSS ACKs (`ack 12145` through `ack 18217`) arrive in a burst (`+0.000`), incrementing `sample_cnt` from `2` to `8` (note that because `round_start` was reset on `ack 11133` at `+.035` and subsequent ACKs arrive at `+0.000`, `now_us - round_start == 0 < 10000`, so the ACK Train detector does not fire).
   - Once the 8th sample (`ack 18217`) and 9th ACK (`ack 19229`) arrive, `sample_cnt >= 8` and `curr_rtt` ($$35\text{ ms}$$) $$\ge$$ `delay_min + 4 ms` ($$20\text{ ms} + 4\text{ ms} = 24\text{ ms}$$), so **Detector 2 (`HYSTART_DELAY`) triggers** and sets `tp->snd_ssthresh = tp->snd_cwnd`.
   - Assertions confirm `tcpi_snd_ssthresh == tcpi_snd_cwnd` and `tcpi_snd_ssthresh < 0x7fffffff` without a single packet drop.

### Why `starter.bpf.c` Fails and `solution.bpf.c` Passes

- **`starter.bpf.c`**: Because `hystart_update()` is left as an empty stub, `tp->snd_ssthresh` never changes from `0x7fffffff`, failing the assertion `assert tcpi_snd_ssthresh == tcpi_snd_cwnd` on line 49 of `test.pkt`.
- **`solution.bpf.c`**: On the 8th RTT sample in Round 2, `hystart_update()` detects that `curr_rtt` ($$35\text{ ms}$$) $$\ge 24\text{ ms}$$ and sets `tp->snd_ssthresh = tp->snd_cwnd`, passing all assertions.

### Commands to Run

```sh
../../run_test.sh cubic/step04-hystart starter
../../run_test.sh cubic/step04-hystart solution
```
