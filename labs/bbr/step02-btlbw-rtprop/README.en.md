# Step 2: BDP Calculation from BtlBw (Max Bandwidth Filter) and RTprop (Min RTT Filter)

## 1. Goal

In Step 1, we updated `sk_pacing_rate` on every ACK by directly overwriting our bandwidth estimate with the latest delivery rate sample (`rs->delivered / rs->interval_us`). In a real network, however, a single ACK sample is noisy: router queues temporarily inflate RTT, and application pauses cause the measured delivery rate to drop far below the physical link capacity. In this step, we store per-socket state in `icsk_ca_priv` to track the two core parameters of BBR's network path model — **RTprop (Round-Trip Propagation Time via a minimum RTT filter)** and **BtlBw (Bottleneck Bandwidth via an application-limited-aware maximum bandwidth filter)** — and use their product, the **BDP (Bandwidth-Delay Product)**, to control both `snd_cwnd` and `sk_pacing_rate` (see [../README.md](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/README.md) for the BBR track overview).

---

## 2. How the Algorithm Works (Read This First)

Before looking at the code or design questions, let us build the model from first principles: why naive per-ACK overwriting fails, how BBR extracts physical path properties from noisy ACK samples, and which kernel structures and fields are already provided for us.

### Why Overwriting on Every ACK Fails

Each time an ACK arrives, the TCP sender observes an instantaneous **RTT sample (`rs->rtt_us`)** and an instantaneous **delivery rate sample (`rs->delivered / rs->interval_us`)**. Blindly overwriting our path model with every new sample breaks down for two physical reasons:

1. **Queueing Delay Inflates RTT**:
   Every measured round-trip time $\text{RTT}_t$ is the sum of the two-way physical propagation delay of the path ($\text{RTprop}$, determined by speed-of-light propagation in fibre and router serialisation delays) plus any queueing delay inside bottleneck buffers ($\text{QueueDelay}_t \ge 0$):
   $$\text{RTT}_t = \text{RTprop} + \text{QueueDelay}_t$$
   Because queueing delay can never be negative ($\text{QueueDelay}_t \ge 0$), $\text{RTT}_t$ equals $\text{RTprop}$ only when the bottleneck queue is empty. Whenever packets queue up behind a burst or competing traffic, $\text{RTT}_t$ rises above $\text{RTprop}$. Therefore, to estimate the true unloaded propagation delay (`min_rtt_us`), we must apply a **minimum filter (min-filter)** across valid RTT samples (`rs->rtt_us > 0`).

2. **Under-Utilised Pipes Understate Bandwidth**:
   Conversely, the delivery rate measured over an ACK interval ($\text{bw}_t = \text{delivered} / \Delta t$) can never exceed the physical capacity of the bottleneck link ($\text{BtlBw}$), because packets cannot arrive at the receiver faster than the bottleneck link transmits them ($\text{bw}_t \le \text{BtlBw}$). However, whenever the sender transmits less data than needed to keep the bottleneck link full, the measured delivery rate falls below $\text{BtlBw}$. Therefore, to estimate the bottleneck link capacity (`max_bw`), we must apply a **maximum filter (max-filter)** across delivery rate samples.

### The Application-Limited (`rs->is_app_limited`) Invariant

A crucial edge case arises when the application itself does not supply enough data to keep the network pipe full — for instance, between HTTP requests, during interactive SSH sessions, or after a brief pause between writes. This condition is called being **application-limited (app-limited)**.

Suppose a flow on a $1000\text{ pkts/s}$ link sends a 10-packet burst at line rate (measuring $\text{max\_bw} = 1000\text{ pkts/s}$), and then the application pauses for $30\text{ ms}$ before writing just 2 packets. The delivery interval stretches to $\Delta t = 50\text{ ms}$, yielding an apparent delivery rate of $2 / 50\text{ ms} = 40\text{ pkts/s}$. The network did not become $25\times$ slower; **the application simply had nothing to send**.

The Linux kernel's rate estimator (`tcp_rate.c`) automatically detects whenever a rate sample was collected while the sender was not filling the pipe and sets `rs->is_app_limited = true`. BBR enforces a strict invariant on app-limited samples:

- **Non-app-limited samples (`!rs->is_app_limited`)**: The sender was actively testing the pipe, so the sample is fed into the max-filter (`if (bw > ca->max_bw) ca->max_bw = bw`).
- **App-limited samples (`rs->is_app_limited != 0`)**: Ignored by default so they cannot drag down or corrupt our bandwidth history. **However, if an app-limited sample still manages to match or exceed our current `ca->max_bw` (`bw >= ca->max_bw`), BBR accepts it!** Achieving a higher delivery rate even without filling the pipe is undeniable proof that the bottleneck is *at least* that fast.

In C, this rule is expressed as:

```c
if (!rs->is_app_limited || bw >= ca->max_bw) {
	if (bw > ca->max_bw)
		ca->max_bw = bw;
}
```

```
  Delivery Rate (bw)                          RTT (rtt_us)
      ^                                           ^
1000  |   [Phase 1: 10 pkts / 10 ms]        20 ms |                 [Phase 2: inflated RTT]
pkts/s|   *========================> max_bw       |                 * (ignored by min-filter)
      |   |                                 10 ms |   [Phase 1]     |
  40  |   |             [Phase 2: App-Limited]    |   *=============+==========> min_rtt_us
pkts/s|   |             * (ignored, max_bw kept!) |   |
      +---+-------------+----------> Time         +---+-------------+----------> Time
```

### Fixed-Point BDP Calculation and Target `snd_cwnd`

Once we know both $\text{BtlBw}$ (`ca->max_bw`) and $\text{RTprop}$ (`ca->min_rtt_us`), Kleinrock's optimal operating point states that the exact volume of in-flight data required to achieve 100% bottleneck utilisation with zero standing queue is the **Bandwidth-Delay Product (BDP)**:

$$\text{BDP (packets)} = \text{BtlBw (packets / }\mu\text{s)} \times \text{RTprop (}\mu\text{s)}$$

Because floating-point arithmetic is not permitted in the kernel or BPF programs, BBR uses the same two fixed-point scaling constants as Linux's `tcp_bbr.c`:

- **`BW_SCALE = 24` (`BW_UNIT = 1U << 24 = 16,777,216`)**:
  Because `rs->delivered / rs->interval_us` in packets per microsecond is almost always a fraction less than `1`, BBR scales the numerator by `BW_UNIT` before dividing:
  $$\text{bw} = \left\lfloor \frac{\text{rs->delivered} \times \text{BW\_UNIT}}{\text{rs->interval\_us}} \right\rfloor$$
- **`BBR_SCALE = 8` (`BBR_UNIT = 1U << 8 = 256`)**:
  Fixed-point scale for pacing and cwnd gains. A unity gain ($1.0\times$) is `BBR_UNIT` (`256`).

Given `bw` (in $\text{packets}/\mu\text{s} \times 2^{24}$), `ca->min_rtt_us` (in $\mu\text{s}$), and `gain` (scaled by $2^8$), `bbr_bdp()` computes the BDP in packets as follows:

1. Multiply `bw` and `ca->min_rtt_us` in 64-bit integer precision: `w = (__u64)bw * ca->min_rtt_us;` (which represents $\text{BDP} \times \text{BW\_UNIT}$).
2. Multiply by `gain`, shift right by `BBR_SCALE` (`8` bits), and perform **ceiling division** by `BW_UNIT`:
   ```c
   return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
   ```
   Why **ceiling division** (`+ BW_UNIT - 1`) rather than integer truncation? If truncation rounded `bdp` down by even 1 packet below the true BDP (e.g., $9.8 \to 9$), `snd_cwnd` would cap in-flight data below the bottleneck capacity, preventing the sender from ever filling the pipe or measuring the true bandwidth — a permanent negative feedback loop.
3. If either `ca->min_rtt_us` or `bw` is not yet initialised (`0`), `bbr_bdp()` returns a safe default initial window of `10U` packets.
4. Finally, BBR adds a 4-packet margin (`bdp + 4U`, floored at `BBR_CWND_MIN_TARGET = 4U`) to `tp->snd_cwnd` to absorb receiver delayed ACKs and TSO/GSO burst quantisation:
   $$\text{snd\_cwnd} = \max(\text{bdp} + 4, 4)$$

### Counting Packet-Timed Round Trips (`round_cnt`)

In subsequent steps (Startup exit detection in Step 3 and ProbeBW gain cycling in Step 4), BBR measures time in **packet-timed round trips** rather than wall-clock timers. We lay that foundation here:

1. At the start of a round trip, BBR records the cumulative delivered packet counter: `ca->next_rtt_delivered = tp->delivered`.
2. Whenever the kernel transmits a packet, it records the current value of `tp->delivered` in the packet's metadata. When that packet is later acknowledged, the kernel copies that snapshot into `rs->prior_delivered`.
3. Therefore, as soon as an ACK arrives with `rs->prior_delivered >= ca->next_rtt_delivered` (checked safely against 32-bit unsigned wrap-around using `!before(rs->prior_delivered, ca->next_rtt_delivered)`), a packet sent at or after the beginning of the current round has completed a full round trip.
4. BBR then increments `ca->round_cnt++` and advances the threshold for the next round to `ca->next_rtt_delivered = tp->delivered`.

### Concrete Numerical Example (Matching `test.pkt`)

Let us trace the exact numbers from `test.pkt` on a path with payload $\text{MSS} = 1000\text{ Bytes}$:

- **Phase 1 (Full-speed 10-packet burst)**:
  - The sender writes 10 packets ($10,000\text{ Bytes}$) at $t = 10\text{ ms}$ and receives an ACK for all 10 packets $10\text{ ms}$ ($10,000\ \mu\text{s}$) later.
  - `rs->rtt_us = 10000`, `rs->delivered = 10`, `rs->interval_us = 10000`, `rs->is_app_limited = false`.
  - Because `ca->min_rtt_us` was `0`, the min-filter initialises `ca->min_rtt_us = 10000`.
  - $\text{bw} = \lfloor 10 \times 16,777,216 / 10000 \rfloor = 16,777$ ($\approx 0.001\text{ pkts/}\mu\text{s} = 1000\text{ pkts/s}$).
  - Since `!rs->is_app_limited` and `bw > ca->max_bw`, the max-filter sets `ca->max_bw = 16777`.
  - In `bbr_bdp()`, $w = 16,777 \times 10000 = 167,770,000$. With unity gain (`gain = BBR_UNIT`), ceiling division gives $\lceil 167,770,000 / 16,777,216 \rceil = 10\text{ packets}$.
  - Thus `tp->snd_cwnd = max(10 + 4, 4) = 14`, and `sk_pacing_rate` is set to $\approx 990,000\text{ Bytes/sec}$ ($1\text{ MB/s}$ minus the 1% pacing margin).
- **Phase 2 (App-limited 2-packet write after a $30\text{ ms}$ pause, with inflated RTT)**:
  - The application pauses for $30\text{ ms}$, then writes only 2 packets ($2000\text{ Bytes}$), which are ACKed $20\text{ ms}$ later due to transient queueing.
  - `rs->rtt_us = 20000`, `rs->delivered = 2`, `rs->interval_us = 50000`, `rs->is_app_limited = true`.
  - **RTprop filter**: `rs->rtt_us` (`20000`) is greater than `ca->min_rtt_us` (`10000`), so the min-filter ignores it and keeps `ca->min_rtt_us = 10000`.
  - **BtlBw filter**: The sample rate is $\text{bw} = \lfloor 2 \times 16,777,216 / 50000 \rfloor = 671$ ($40\text{ pkts/s}$). Because `rs->is_app_limited` is true and `bw < ca->max_bw`, it is rejected and `ca->max_bw = 16777` is preserved.
  - Consequently, both `tp->snd_cwnd == 14` and `sk_pacing_rate` ($\approx 990\text{ KB/s}$) remain intact after Phase 2.

### What the Kernel Already Provides

Before answering the design questions in Section 3, review the exact fields and helpers provided by the kernel and [bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h):

- **`inet_csk(sk)->icsk_ca_priv` and `inet_csk_ca(sk)`**:
  - Every TCP socket (`struct inet_connection_sock`) embeds a dedicated byte array `icsk_ca_priv` where a congestion control module can store per-socket state.
  - Calling `inet_csk_ca(sk)` returns a `void *` pointer to `icsk_ca_priv`, which you cast to your custom state struct: `struct bpf_bbr2_ca *ca = inet_csk_ca(sk);`.
- **`struct rate_sample *rs` (4th parameter of `cong_control`)**:
  - `rs->rtt_us` (`__s32`): RTT measured by this ACK in microseconds (`<= 0` if no RTT sample was available on this ACK).
  - `rs->delivered` (`__s32`): Number of packets delivered to the receiver over this measurement interval.
  - `rs->interval_us` (`__s32`): Duration of the delivery measurement interval in microseconds.
  - `rs->prior_delivered` (`__u32`): Snapshot of `tp->delivered` at the time the newly ACKed packet was transmitted.
  - `rs->is_app_limited` (`bool`): Non-zero (`true`) if the sender was application-limited during this measurement interval.
- **`struct tcp_sock *tp` (via `tcp_sk(sk)`)**:
  - `tp->delivered` (`__u32`): Cumulative count of packets delivered across the lifetime of the connection.
  - `tp->snd_cwnd` (`__u32`): Sending congestion window in packets.
  - `tp->snd_ssthresh` (`__u32`): Slow-start threshold (set to `TCP_INFINITE_SSTHRESH` in BBR).
  - `tp->srtt_us` (`__u32`): Smoothed RTT scaled by $8\times$ (`tp->srtt_us >> 3` is microseconds).
  - `tp->mss_cache` (`__u32`): Current Maximum Segment Size in bytes.
- **`struct sock *sk`**:
  - `sk->sk_pacing_status`: Set to `SK_PACING_NEEDED` to enable TCP pacing.
  - `sk->sk_pacing_rate` (`unsigned long`): Pacing rate in bytes per second.
  - `sk->sk_max_pacing_rate` (`unsigned long`): Optional upper bound set via `SO_MAX_PACING_RATE`.
- **Helper functions ([bpf_cc_helpers.h](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/include/bpf_cc_helpers.h))**:
  - `before(seq1, seq2)`: Wrap-around-safe 32-bit comparison returning `true` when `seq1 < seq2`. Thus `!before(a, b)` means `a >= b`.
  - `div64_u64(dividend, divisor)`: Safe 64-bit unsigned integer division (returns `0` if `divisor == 0`).

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Use what you learned in Section 2 to work through these four design questions for [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c). Every question can be answered directly from Section 2.

### Q1. How should we write the `RTprop` min-filter condition so that it also handles the very first RTT sample (`ca->min_rtt_us == 0`)?

In `bpf_bbr2_init()`, `ca->min_rtt_us` is initialised to `0`. What would go wrong if we wrote `if (rs->rtt_us > 0 && rs->rtt_us < ca->min_rtt_us)` without checking for `0`? Taking into account the signed/unsigned types (`rs->rtt_us` is `__s32` while `ca->min_rtt_us` is `__u32`), what is the exact condition for TODO 1?

<details>
<summary>Hint and Answer</summary>

- Because `ca->min_rtt_us` starts at `0`, a positive `rs->rtt_us` would never satisfy `rs->rtt_us < 0`, leaving `ca->min_rtt_us` stuck at `0` forever.
- We must update `ca->min_rtt_us` whenever `rs->rtt_us > 0` and *either* `ca->min_rtt_us == 0` (uninitialised) *or* `rs->rtt_us < (__s32)ca->min_rtt_us`:
  ```c
  if (rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
  	ca->min_rtt_us = rs->rtt_us;
  ```

</details>

### Q2. How should we write the `BtlBw` max-filter and app-limited check for TODO 2?

In [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c), line 120 unconditionally overwrites `ca->max_bw = bw;`. How should you replace that line to implement the app-limited invariant and maximum filter explained in Section 2?

<details>
<summary>Hint and Answer</summary>

- As explained in Section 2, we only feed a sample into the max-filter if the sample was *not* app-limited (`!rs->is_app_limited`) or if its rate still managed to match or exceed our current estimate (`bw >= ca->max_bw`). Within that guard, we update `ca->max_bw` when `bw > ca->max_bw`:
  ```c
  if (!rs->is_app_limited || bw >= ca->max_bw) {
  	if (bw > ca->max_bw)
  		ca->max_bw = bw;
  }
  ```
- Note that for a simple scalar maximum, `if (bw > ca->max_bw)` alone would already reject smaller samples; writing the outer `if (!rs->is_app_limited || bw >= ca->max_bw)` mirrors the structure of Linux's `bbr_update_bw()`, where non-app-limited samples also advance the windowed max-filter state in later steps.

</details>

### Q3. How do we detect the completion of a packet-timed round trip (`ca->round_cnt`)?

After `bpf_bbr2_init()` sets `ca->next_rtt_delivered = tp->delivered;`, how does `bpf_bbr2_cong_control()` use `rs->prior_delivered` and `tp->delivered` to advance `ca->round_cnt` when a valid rate sample arrives?

<details>
<summary>Hint and Answer</summary>

- We check whether `rs->prior_delivered` has reached or passed `ca->next_rtt_delivered` using `!before(rs->prior_delivered, ca->next_rtt_delivered)`.
- When true, one full packet-timed round trip has completed, so we increment `ca->round_cnt++` and set the next round's boundary to `ca->next_rtt_delivered = tp->delivered`:
  ```c
  if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
  	ca->round_cnt++;
  	ca->next_rtt_delivered = tp->delivered;
  }
  ```

</details>

### Q4. Why does `bbr_bdp()` check `if (!ca->min_rtt_us || !bw) return 10U;` and use ceiling division (`+ BW_UNIT - 1`)?

Look at `bbr_bdp()` in [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c). What two failure modes are prevented by the initial zero check and the `+ BW_UNIT - 1` term in `(((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT`?

<details>
<summary>Hint and Answer</summary>

- **Uninitialised guard (`!ca->min_rtt_us || !bw`)**: Before the first valid delivery sample is processed, `min_rtt_us` or `max_bw` is `0`. Without this guard, `w` would be `0` and `snd_cwnd` would clamp down to `4` packets (`BBR_CWND_MIN_TARGET`) instead of allowing a standard 10-packet initial window.
- **Ceiling division (`+ BW_UNIT - 1`)**: Rounding up prevents integer truncation from setting `snd_cwnd` slightly below the true BDP, which would otherwise starve the pipe by 1 packet and prevent the flow from ever probing full bottleneck capacity.

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

Let us walk through each part of [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c).

### 1. Scaling Constants and Per-Socket State `struct bpf_bbr2_ca`

```c
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
```

- `BW_UNIT` (`1 << 24`) scales delivery rates in packets per microsecond; `BBR_UNIT` (`1 << 8`) scales gains.
- `struct bpf_bbr2_ca` is stored in each socket's `icsk_ca_priv` area and accessed via `inet_csk_ca(sk)`.

### 2. Pacing Rate Conversion (`bbr_rate_bytes_per_sec()` and `bbr_set_pacing_rate()`)

```c
static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}
```

- Converts `rate` (in `packets/usec << BW_SCALE`) into bytes per second by multiplying by `mss`, applying `gain` (`>> BBR_SCALE`), multiplying by `USEC_PER_SEC` with a 1% pacing margin (`BBR_PACING_MARGIN_PCT = 1U`, i.e. $990,000$), and shifting right by `BW_SCALE`.
- `bbr_set_pacing_rate()` clamps the result against `sk->sk_max_pacing_rate` (if set) and writes it to `sk->sk_pacing_rate`.

### 3. BDP Helper `bbr_bdp()`

```c
static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}
```

- Returns `10U` until both `ca->min_rtt_us` and `bw` are non-zero, and otherwise computes $\lceil (\text{bw} \times \text{min\_rtt\_us} \times \text{gain} \gg 8) / 2^{24} \rceil$.

### 4. Initialisation (`bpf_bbr2_init()`) and Per-ACK Control (`bpf_bbr2_cong_control()`)

```c
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
```

- Zeroes the filter state, anchors the first packet-timed round at `tp->delivered`, enables pacing via `SK_PACING_NEEDED`, and seeds an initial pacing rate from `snd_cwnd` and `srtt_us` via `bbr_init_pacing_rate_from_rtt()`.

Next, `bpf_bbr2_cong_control()` updates the path model and applies the controls on every ACK:

```c
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
```

- **TODO 1**: Updates `ca->min_rtt_us` only when `rs->rtt_us > 0` and either `ca->min_rtt_us == 0` or `rs->rtt_us < (__s32)ca->min_rtt_us`.
- **TODO 2**: Advances `ca->round_cnt` when `rs->prior_delivered` reaches `ca->next_rtt_delivered`, computes fixed-point `bw`, and updates `ca->max_bw` only when `!rs->is_app_limited || bw >= ca->max_bw`.
- Updates `sk_pacing_rate` and sets `tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET)`.

---

## 5. Understanding `test.pkt` and How to Run

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/test.pkt) tests both the `BtlBw` max-filter and the `RTprop` min-filter across two phases.

### Packet Trace Walkthrough

1. **3-way Handshake (lines 14–17, RTT = $10\text{ ms}$, payload MSS = $1000\text{ Bytes}$)**:
   - SYN and SYN-ACK are exchanged at `0.00`, and the handshake ACK arrives at `+0.01` ($10\text{ ms}$ RTT). Because `mss 1012` includes 12 bytes of TCP timestamps, the TCP payload MSS is exactly $1000\text{ Bytes}$.
2. **Phase 1: High-bandwidth burst and first assertion (lines 26–33)**:
   - `write(4, ..., 10000)` transmits 10 segments (`1:10001(10000)`) at `0.010`, and the cumulative ACK (`ack 10001`) arrives $10\text{ ms}$ later (`+0.01`).
   - This produces `rs->rtt_us = 10000` ($10\text{ ms}$), `rs->delivered = 10`, and `rs->interval_us = 10000` ($10\text{ ms}$).
   - `ca->min_rtt_us` becomes `10000`, `ca->max_bw` becomes $1000\text{ pkts/s}$ ($1,000,000\text{ Bytes/s}$), `bdp = 10` packets, `tcpi_snd_cwnd == 14` ($10 + 4$), and `tcpi_pacing_rate` is $\approx 990\text{ KB/s}$ (`950000 < tcpi_pacing_rate < 1050000`).
3. **Phase 2: App-limited small burst with inflated RTT and second assertion (lines 39–46)**:
   - After a $30\text{ ms}$ pause (`+0.03`), the application writes only $2000\text{ Bytes}$ (2 segments), and the ACK (`ack 12001`) returns $20\text{ ms}$ later (`+0.02`).
   - This sample has `rs->rtt_us = 20000` (inflated to $20\text{ ms}$), `rs->delivered = 2`, `rs->interval_us = 50000` ($40\text{ pkts/s}$), and `rs->is_app_limited = true`.
   - **Why `starter.bpf.c` fails**: [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/starter.bpf.c) blindly overwrites `ca->min_rtt_us = rs->rtt_us` and `ca->max_bw = bw`. On the Phase 2 ACK, `max_bw` collapses by $25\times$ to $40\text{ pkts/s}$, shrinking `tcpi_snd_cwnd` to `5` ($\text{BDP } 1 + 4$) and dropping `tcpi_pacing_rate` to $\approx 39.6\text{ KB/s}$, which fails the assertions on lines 44–45.
   - **Why `solution.bpf.c` passes**: [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step02-btlbw-rtprop/solution.bpf.c) rejects the inflated RTT via the min-filter (`min_rtt_us` stays `10000`) and rejects the low app-limited bandwidth sample via the max-filter (`max_bw` stays $1000\text{ pkts/s}$), preserving `tcpi_snd_cwnd == 14` and `tcpi_pacing_rate` ($\approx 990\text{ KB/s}$).

### How to Run

Run the following commands to see `starter` fail the Phase 2 assertion and `solution` pass:

```sh
../../run_test.sh bbr/step02-btlbw-rtprop starter
../../run_test.sh bbr/step02-btlbw-rtprop solution
```
