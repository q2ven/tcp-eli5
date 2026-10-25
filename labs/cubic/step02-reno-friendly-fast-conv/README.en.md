# Step 2: Reno-friendly Region ($$W_{\text{est}}$$) and Fast Convergence

## 1. Goal

In this step, you will extend the pure CUBIC curve from Step 1 ($$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$$) with two mechanisms essential for real networks: **Fast Convergence** (which helps existing CUBIC flows yield bandwidth quickly when new competing flows join) and the **Reno-friendly region ($$W_{\text{est}}$$ / `tcp_cwnd`)** (which ensures CUBIC never grows slower than standard TCP Reno on short-RTT or low-BDP paths). For the full curriculum and how this step fits into the track, see the [CUBIC Track Overview (`../README.md`)](../README.md).

---

## 2. How the Algorithm Works (Read This First)

Before looking at the `TODO` items in `starter.bpf.c`, let's build a complete picture from first principles: why pure CUBIC runs into trouble in these two scenarios, how the mathematics of Fast Convergence and the Reno-friendly region solve them, and which kernel structures and fields are already at your disposal.

### 2.1 Two Practical Limitations of the Pure CUBIC Curve

As we saw in Step 1, when packet loss occurs at window $$W$$, pure CUBIC saves $$W_{\max} = W$$ (`ca->last_max_cwnd`), reduces `cwnd` multiplicatively to $$W_0 = \beta W$$ (with $$\beta \approx 0.7$$), and then grows the window along a cubic function of elapsed time $$t$$:

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}, \quad K = \sqrt[3]{\frac{W_{\max} - W_0}{C}}$$

In the concave region ($$t < K$$), `cwnd` climbs towards $$W_{\max}$$; near $$t = K$$, it flattens into a **plateau** around $$W_{\max}$$; and in the convex region ($$t > K$$), it probes upwards for additional bandwidth.

While this curve excels on long-fat networks (high BDP), running the pure cubic equation by itself on real networks reveals two weaknesses:

- **Problem 1 (Slow convergence when new competing flows join)**: Suppose an existing CUBIC flow is running with a large $$W_{\max}$$ when a new flow joins the same bottleneck link. Each flow's fair share is now smaller than the old $$W_{\max}$$. Because the existing flow keeps aiming its concave climb and plateau at that outdated, high $$W_{\max}$$, it releases bandwidth very slowly, requiring many loss rounds before the flows converge to fairness.
- **Problem 2 (Underperforming TCP Reno on short-RTT / small-BDP paths)**: Because $$W_{\text{cubic}}(t)$$ depends on wall-clock time $$t$$ (in seconds) rather than RTT count—and has a slope near zero around the plateau ($$t \approx K$$)—on short-RTT paths (where many RTTs elapse within a fraction of a second) or when $$W_{\max}$$ is small, the cubic curve can grow more slowly than standard TCP Reno (AIMD), which adds 1 segment every RTT.

### 2.2 Algorithm 1: Fast Convergence

#### Intuition and Key Idea

How can a sender tell that a new competing flow has joined the bottleneck and reduced its available share?

The clue lies in **comparing the current `cwnd` at the moment of packet loss against `ca->last_max_cwnd` ($$W_{\max}$$) from the previous loss event**:

- **When $$\text{cwnd} \ge \text{last}_{\max,\text{cwnd}}$$**: The flow successfully recovered all the way to its previous maximum $$W_{\max}$$ and was probing beyond it in the convex region when loss occurred. The bottleneck share has not shrunk, so recording `last_max_cwnd = cwnd` is appropriate.
- **When $$\text{cwnd} < \text{last}_{\max,\text{cwnd}}$$**: The flow suffered another packet loss *before* it could even climb back to its previous $$W_{\max}$$. This indicates that the bottleneck bandwidth available to this flow has shrunk (typically because a new competing flow has joined).

Without Fast Convergence, setting `last_max_cwnd = cwnd` would place the next cubic plateau right at `cwnd`, causing the flow to hold on to bandwidth stubbornly. **Fast Convergence** resolves this: whenever loss occurs with `cwnd < last_max_cwnd`, it proactively lowers $$W_{\max}$$ further—to the **midpoint (average)** between the pre-loss window $$\text{cwnd}$$ ($$1.0 \times \text{cwnd}$$) and the post-loss window $$\beta \cdot \text{cwnd}$$:

$$W_{\max} = \text{cwnd} \times \frac{1 + \beta}{2}$$

With $$\beta \approx 0.7$$, $$\frac{1 + 0.7}{2} = 0.85$$. Lowering $$W_{\max}$$ to $$\approx 0.85 \times \text{cwnd}$$ pulls the next epoch's plateau down immediately, allowing the existing flow to stabilise at a lower window and yield bandwidth to the newcomer much faster.

```text
cwnd
  ^
20|      * (Loss #1: cwnd=20 -> last_max_cwnd = 20, ssthresh = 14)
  |     /
14|    /   * (Loss #2: cwnd=14 < last_max_cwnd=20 -> Shrinking share detected!)
  |   /   /  Without Fast Convergence: next epoch plateaus at W_max = 14
11|  /   /   With Fast Convergence:    next epoch plateaus at W_max = 14 * (1 + 0.7) / 2 = 11
  | /   /                              (plateaus earlier at W_max=11 to yield bandwidth)
 9|/   +-----> New epoch starts from ssthresh = 9
  +---------------------------------------------------------------------> Time t
```

#### Fixed-Point Formula and Concrete Numerical Example

In `starter.bpf.c` and `solution.bpf.c`, $$\beta$$ is represented in fixed-point arithmetic using `BICTCP_BETA_SCALE = 1024` and `beta = 717` ($$717 / 1024 \approx 0.7002$$). Multiplying `cwnd` by $$\frac{1 + \beta}{2}$$ in integer arithmetic therefore becomes:

$$\text{last}_{\max,\text{cwnd}} = \left\lfloor \frac{\text{cwnd} \times (\text{BETA}_{\text{SCALE}} + \text{beta})}{2 \times \text{BETA}_{\text{SCALE}}} \right\rfloor = \left\lfloor \frac{\text{cwnd} \times (1024 + 717)}{2048} \right\rfloor = \left\lfloor \frac{\text{cwnd} \times 1741}{2048} \right\rfloor$$

Note that the slow-start threshold returned by `.ssthresh` remains the standard multiplicative decrease $$\max(\lfloor \text{cwnd} \times 717 / 1024 \rfloor, 2)$$.

- **Concrete Example (from `test.pkt`)**:
  1. **Loss #1 at `cwnd = 20`** (with initial `last_max_cwnd = 0`):
     Since `20 < 0` is false, `last_max_cwnd = 20` and `ssthresh = (20 * 717) / 1024 = 14`. After recovery, `cwnd = 14`.
  2. **Loss #2 at `cwnd = 14`** (before recovering to `20`):
     Because `cwnd (14) < last_max_cwnd (20)` is true, Fast Convergence triggers:
     - `last_max_cwnd = (14 * 1741) / 2048 = 24374 / 2048 = 11` (whereas without Fast Convergence it would be `14`).
     - `ssthresh = (14 * 717) / 1024 = 9`.

### 2.3 Algorithm 2: The Reno-friendly Region ($$W_{\text{est}}$$ / `tcp_cwnd`)

#### Intuition and Key Idea

To guarantee that CUBIC never grows more slowly than TCP Reno on short-RTT or small-window paths, CUBIC maintains a parallel shadow variable $$W_{\text{est}}$$ (`ca->tcp_cwnd` in the code) that tracks **what the congestion window of an equivalent AIMD flow would be at the current point in the epoch**. On every Congestion Avoidance update, CUBIC ensures its target window is at least $$W_{\text{est}}$$:

$$W_{\text{target}} = \max\left(W_{\text{cubic}}(t),\, W_{\text{est}}\right)$$

#### Why Does $$W_{\text{est}}$$ Grow by $$+1$$ Every $$\sim 1.875$$ RTTs Instead of Every $$1$$ RTT?

At first glance, one might think: "Standard TCP Reno grows by $$+1$$ segment per RTT, so shouldn't `ca->tcp_cwnd` also grow by $$+1$$ every `cwnd` ACKs (1 RTT)?"

The answer is **No**, because of how much CUBIC backs off on loss! Standard TCP Reno cuts its window in half on loss ($$\beta_{\text{reno}} = 0.5$$) and grows by $$\alpha_{\text{reno}} = 1$$ segment per RTT. CUBIC, by contrast, only cuts its window to $$\beta \approx 0.7 \times W_{\max}$$. If CUBIC started each epoch at $$0.7 W_{\max}$$ and still increased $$W_{\text{est}}$$ by $$+1$$ per RTT, its AIMD sawtooth would sit strictly above TCP Reno's sawtooth and steal bandwidth from coexisting Reno flows.

From steady-state AIMD throughput analysis, an $$\text{AIMD}(\alpha, \beta)$$ flow achieves the **exact same average congestion window** as $$\text{TCP Reno}(1, 0.5)$$ when its additive increase rate $$\alpha$$ (segments per RTT) satisfies:

$$\alpha = \frac{3(1 - \beta)}{1 + \beta}$$

Substituting CUBIC's $$\beta = 717 / 1024 \approx 0.7$$:

$$\alpha = \frac{3 \times (1024 - 717)}{1024 + 717} = \frac{3 \times 307}{1741} = \frac{921}{1741} \approx 0.529 \text{ segments per RTT}$$

Taking the reciprocal gives the **number of RTTs required to increment $$W_{\text{est}}$$ (`ca->tcp_cwnd`) by $$1$$ segment**:

$$\frac{1}{\alpha} = \frac{1 + \beta}{3(1 - \beta)} = \frac{1741}{921} \approx 1.890 \text{ RTTs}$$

#### Fixed-Point Constant `beta_scale` and ACK-Counting Rules

Because roughly `cwnd` segments are acknowledged per RTT, advancing `ca->tcp_cwnd` by 1 requires $$\Delta = \text{cwnd} \times \frac{1}{\alpha}$$ ACKed segments.

To compute this in integer arithmetic, the kernel (and `starter.bpf.c`) precomputes `beta_scale` as $$\frac{8}{\alpha}$$ (scaled by $$2^3 = 8$$):

```c
static const __u32 beta_scale = 8 * (BICTCP_BETA_SCALE + beta) / 3
				/ (BICTCP_BETA_SCALE - beta);
```

With `BICTCP_BETA_SCALE = 1024` and `beta = 717`, integer division yields `beta_scale = 4642 / 307 = 15` (representing $$15 / 8 = 1.875$$ RTTs). The number of ACKed segments `delta` needed to increment `ca->tcp_cwnd` by 1 is therefore obtained by shifting right by 3 (`>> 3`, i.e. dividing by 8):

$$\text{delta} = \left\lfloor \frac{\text{cwnd} \times \text{beta}_{\text{scale}}}{8} \right\rfloor = ((\text{cwnd} \times \text{beta}_{\text{scale}}) \gg 3)$$

In `bictcp_update(sk, cwnd, acked)`, the Reno-friendly region operates in four steps:

1. **Accumulate ACKed segments at the very top of `bictcp_update()`**:
   Every call adds the newly acknowledged segments via `ca->ack_cnt += acked;`.
   *(Note: When a new epoch starts with `ca->epoch_start == 0`, the existing initialisation block resets `ca->ack_cnt = acked;` and `ca->tcp_cwnd = cwnd;`. Furthermore, if multiple ACKs arrive within the same timestamp `now_us == ca->last_time`, `bictcp_update()` skips recomputing the cubic curve via `goto tcp_friendliness;` so the newly accumulated `ca->ack_cnt` is still processed.)*
2. **Advance `ca->tcp_cwnd` in $$O(1)$$ at the `tcp_friendliness:` label**:
   Compute `delta = (cwnd * beta_scale) >> 3;`. For every `delta` segments accumulated in `ca->ack_cnt`, increment `ca->tcp_cwnd` and keep the remainder in `ca->ack_cnt`.
   - **Important BPF Verifier Constraint**: In the Linux kernel's `net/ipv4/tcp_cubic.c`, this update is written as `while (ca->ack_cnt > delta) { ca->ack_cnt -= delta; ca->tcp_cwnd++; }`. However, in BPF, an unbounded `while` loop whose iteration count depends on runtime variables (`ca->ack_cnt` and `delta`) cannot be proven to terminate by the BPF verifier and will be rejected at load time! Instead, after guarding against division by zero with `if (delta)`, we express the exact same update in $$O(1)$$ using integer division (`/`) and modulo (`%`):
     ```c
     delta = (cwnd * beta_scale) >> 3;
     if (delta) {
             ca->tcp_cwnd += ca->ack_cnt / delta;
             ca->ack_cnt %= delta;
     }
     ```
3. **Clamp `ca->cnt` when `ca->tcp_cwnd > cwnd`**:
   Recall how `bpf_cubic4_cong_avoid_ai(tp, ca->cnt, acked)` works: it increments `tp->snd_cwnd` by 1 every `ca->cnt` ACKed segments. That means a **smaller** `ca->cnt` produces **faster** window growth.
   When `ca->tcp_cwnd > cwnd` (the Reno-equivalent window is ahead of `cwnd`), reaching `ca->tcp_cwnd` over one RTT (`cwnd` ACKs) requires adding 1 segment every `max_cnt` ACKs:
   $$\text{max}_{\text{cnt}} = \left\lfloor \frac{\text{cwnd}}{\text{ca}\to\text{tcp}_{\text{cwnd}} - \text{cwnd}} \right\rfloor$$
   If the cubic curve's `ca->cnt` is larger (slower) than `max_cnt`, we clamp `ca->cnt` downwards:
   ```c
   if (ca->tcp_cwnd > cwnd) {
           max_cnt = cwnd / (ca->tcp_cwnd - cwnd);
           if (ca->cnt > max_cnt)
                   ca->cnt = max_cnt;
   }
   ```
4. **Enforce a minimum `ca->cnt` of `2U`**:
   The final line `ca->cnt = max(ca->cnt, 2U);` (already present in `starter.bpf.c`) ensures Congestion Avoidance increases `cwnd` by at most 1 segment per 2 ACKs.

- **Concrete Numerical Example (Breaking Out of the Plateau in `test.pkt`)**:
  - After Loss #2 (`cwnd = 14`), Fast Convergence sets `last_max_cwnd = 11` and `ssthresh = 9`.
  - Congestion Avoidance starts at `cwnd = 9` (`tcp_cwnd = 9`), and across Flight 1 (9 ACKs) and Flight 2 (10 ACKs), `cwnd` grows to `11` (`== last_max_cwnd`), while `tcp_cwnd` reaches `10` with remainder `ack_cnt = 1`.
  - At `cwnd = 11`, while $$t < K \approx 1.71\text{ s}$$, pure CUBIC sits on its plateau (`bic_target == 11 == cwnd`) and sets `ca->cnt = 100 * 11 = 1100` (virtually freezing `cwnd` growth).
  - Meanwhile, for the Reno-friendly region at `cwnd = 11`, `delta = (11 * 15) >> 3 = 165 >> 3 = 20` ACKs. Across four 11-packet flights (44 ACKs total), `ack_cnt` crosses `20` twice, advancing `ca->tcp_cwnd` from `10 -> 11 -> 12`.
  - As soon as `ca->tcp_cwnd (12) > cwnd (11)`, `max_cnt = 11 / (12 - 11) = 11` clamps `ca->cnt` from `1100` down to `11`, allowing `cwnd` to grow past the plateau to `12`!

### 2.4 What the Kernel and `starter.bpf.c` Already Provide

All data structures, fields, constants, and helper functions required for this step are already provided in `vmlinux.h`, `bpf_cc_helpers.h`, and `starter.bpf.c`:

- **`icsk_ca_priv` and `struct bpf_cubic4_ca` (accessed via `inet_csk_ca(sk)`)**:
  Every kernel TCP socket (`struct inet_connection_sock`) embeds `icsk_ca_priv`, an inline per-socket byte array reserved for the congestion control algorithm's private state. The helper `inet_csk_ca(sk)` casts `inet_csk(sk)->icsk_ca_priv` to `struct bpf_cubic4_ca *`, which defines:
  - `ca->cnt` (`__u32`): Number of ACKed segments required to increase `snd_cwnd` by 1 in `bpf_cubic4_cong_avoid_ai()` (smaller means faster growth).
  - `ca->last_max_cwnd` (`__u32`): Maximum window $$W_{\max}$$ recorded at the previous loss event (adjusted by Fast Convergence).
  - `ca->last_cwnd`, `ca->last_time` (`__u32`): Window and timestamp ($$\mu\text{s}$$) of the previous `bictcp_update()` execution, used to rate-limit cubic curve recalculations to at most once every $$1/32\text{ s}$$ when `cwnd` has not changed.
  - `ca->bic_origin_point`, `ca->bic_K` (`__u32`): Origin window ($$W_{\max}$$) and time offset $$K$$ (in units of $$1 / 1024\text{ s}$$) of the current cubic curve.
  - `ca->delay_min` (`__u32`): Minimum observed RTT in microseconds.
  - `ca->epoch_start` (`__u32`): Timestamp ($$\mu\text{s}$$) when the current Congestion Avoidance epoch started (`0` indicates that the next Congestion Avoidance ACK should initialise a new epoch).
  - `ca->ack_cnt` (`__u32`): Cumulative count of ACKed segments for the Reno-friendly calculation.
  - `ca->tcp_cwnd` (`__u32`): Estimated Reno-equivalent window $$W_{\text{est}}$$.
- **`struct tcp_sock` fields (accessed via `tcp_sk(sk)`)**:
  - `tp->snd_cwnd` (`__u32`): Current congestion window in segments.
  - `tp->snd_ssthresh` (`__u32`): Slow-start threshold in segments.
  - `tp->snd_cwnd_cnt` (`__u32`): Linear ACK counter used by `bpf_cubic4_cong_avoid_ai()` to increment `tp->snd_cwnd` every `ca->cnt` ACKs.
  - `tp->snd_cwnd_clamp` (`__u32`): Upper bound on `snd_cwnd`.
  - `tp->tcp_mstamp` (`__u64` / `__u32`): Packet timestamp in microseconds (returned by `bictcp_clock_us(sk)`).
- **Pre-defined Constants in `starter.bpf.c`**:
  - `BICTCP_BETA_SCALE` (`1024`)
  - `beta` (`717` $$\approx 0.7 \times 1024$$)
  - `beta_scale` (`8 * (BICTCP_BETA_SCALE + beta) / 3 / (BICTCP_BETA_SCALE - beta)` $$= 15$$)

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Use the concepts from Section 2 to answer the following four design questions before editing `starter.bpf.c`. Every question can be answered directly from what you just read.

### Q1. In `bpf_cubic4_ssthresh()`, which condition triggers Fast Convergence, and how do we express $$W_{\max} = \text{cwnd} \times \frac{1 + \beta}{2}$$ in fixed-point integer arithmetic?

In `bpf_cubic4_ssthresh()`, `cwnd = tp->snd_cwnd` and `ca = inet_csk_ca(sk)` are already fetched. How should we replace the unconditional `ca->last_max_cwnd = cwnd;` in `starter.bpf.c` using `BICTCP_BETA_SCALE` and `beta`?

<details>
<summary>Hint and Answer</summary>

- **Condition**: Check whether loss occurred before recovering to the previous maximum window: `if (cwnd < ca->last_max_cwnd)`.
- **Formula**:
  - When `cwnd < ca->last_max_cwnd`, set `ca->last_max_cwnd` to the midpoint `(cwnd * (BICTCP_BETA_SCALE + beta)) / (2 * BICTCP_BETA_SCALE)`.
  - Otherwise (`cwnd >= ca->last_max_cwnd`), record `ca->last_max_cwnd = cwnd`.
  ```c
  if (cwnd < ca->last_max_cwnd)
          ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
                              / (2 * BICTCP_BETA_SCALE);
  else
          ca->last_max_cwnd = cwnd;
  ```

</details>

### Q2. Why must `ca->ack_cnt += acked;` be placed at the very top of `bictcp_update()` (before the rate-limiting `if` statements), and how does that interact with `ca->epoch_start == 0`?

At the beginning of `bictcp_update()`, there is a rate-limiting return and a same-timestamp jump:

```c
	/* Position of TODO 1 */

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto tcp_friendliness;
```

Why must `ca->ack_cnt += acked;` come before both `if` statements?

<details>
<summary>Hint and Answer</summary>

- **Reason 1 (Counting ACKs during the $$1/32\text{ s}$$ rate-limit window)**: While the time-based cubic curve $$W_{\text{cubic}}(t)$$ only needs to be recomputed at most once per $$1/32\text{ s}$$ when `cwnd` is unchanged, the Reno-friendly window `ca->tcp_cwnd` advances based on **every ACKed segment**. Adding `ca->ack_cnt += acked;` before the early `return;` ensures that ACKs arriving during rate-limited intervals are not lost and are applied the next time execution reaches `tcp_friendliness:`.
- **Reason 2 (Handling `goto tcp_friendliness;`)**: When multiple ACKs arrive with the same timestamp (`now_us == ca->last_time`), jumping to `tcp_friendliness:` requires `ca->ack_cnt` to already include the current `acked` count.
- **Interaction with `ca->epoch_start == 0`**: On the very first call of a new epoch (`ca->epoch_start == 0`), the initialisation block immediately overwrites `ca->ack_cnt = acked;` and `ca->tcp_cwnd = cwnd;`, so incrementing at the top does not double-count the first ACK.

</details>

### Q3. At the `tcp_friendliness:` label, how should we update `ca->tcp_cwnd` and `ca->ack_cnt` so the BPF verifier accepts the program and division by zero is impossible?

From Section 2.3, the number of ACKed segments required per $$+1$$ increment of `ca->tcp_cwnd` is `delta = (cwnd * beta_scale) >> 3;`. How do we replace the kernel's `while (ca->ack_cnt > delta)` loop in BPF?

<details>
<summary>Hint and Answer</summary>

- **Use integer division and modulo with a non-zero guard**: Because the BPF verifier rejects unbounded `while` loops whose trip counts depend on runtime variables, we guard against `delta == 0` with `if (delta)` and update `ca->tcp_cwnd` and `ca->ack_cnt` in $$O(1)$$ using `/` and `%`:
  ```c
  delta = (cwnd * beta_scale) >> 3;
  if (delta) {
          ca->tcp_cwnd += ca->ack_cnt / delta;
          ca->ack_cnt %= delta;
  }
  ```

</details>

### Q4. When `ca->tcp_cwnd > cwnd`, why do we clamp `ca->cnt` *downwards* (`if (ca->cnt > max_cnt) ca->cnt = max_cnt`) to make the window grow faster?

Write the clamping logic for `ca->cnt` when `ca->tcp_cwnd > cwnd` and explain why a smaller `ca->cnt` corresponds to faster growth.

<details>
<summary>Hint and Answer</summary>

- **Why smaller `ca->cnt` means faster growth**: `ca->cnt` is the number of ACKed segments required before `bpf_cubic4_cong_avoid_ai()` increments `tp->snd_cwnd` by 1. At the cubic plateau, pure CUBIC sets `ca->cnt = 100 * cwnd` (very slow growth). To grow at least as fast as the Reno estimate `ca->tcp_cwnd`, we compute the Reno ACK interval `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` and cap `ca->cnt` so it never exceeds `max_cnt`:
  ```c
  if (ca->tcp_cwnd > cwnd) {
          max_cnt = cwnd / (ca->tcp_cwnd - cwnd);
          if (ca->cnt > max_cnt)
                  ca->cnt = max_cnt;
  }
  ```

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

`solution.bpf.c` adds Fast Convergence to `bpf_cubic4_ssthresh()` and the Reno-friendly region to `bictcp_update()`. Let's walk through both functions in detail.

### 4.1 Fast Convergence in `bpf_cubic4_ssthresh()`

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_cubic4_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_cubic4_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd;

	ca->epoch_start = 0;

	/* W_max and Fast Convergence */
	if (cwnd < ca->last_max_cwnd)
		ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
				    / (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = cwnd;

	return max((cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}
```

- `ca->epoch_start = 0;`: Ends the current CUBIC epoch on loss so that the next Congestion Avoidance call to `bictcp_update()` re-initialises `ca->bic_K`, `ca->bic_origin_point`, `ca->ack_cnt`, and `ca->tcp_cwnd`.
- `if (cwnd < ca->last_max_cwnd)`: Detects that loss occurred before recovering to the previous $$W_{\max}$$ and lowers `ca->last_max_cwnd` to `(cwnd * 1741) / 2048` ($$\approx 0.85 \times \text{cwnd}$$).
- `return max((cwnd * beta) / BICTCP_BETA_SCALE, 2U);`: Returns the new `snd_ssthresh` as $$\max(\lfloor 0.7 \times \text{cwnd} \rfloor, 2)$$.

### 4.2 Reno-friendly Region (`tcp_friendliness`) in `bictcp_update()`

```c
static __always_inline void bictcp_update(struct sock *sk, __u32 cwnd, __u32 acked)
{
	struct bpf_cubic4_ca *ca = inet_csk_ca(sk);
	__u32 now_us = bictcp_clock_us(sk);
	__u32 delta, bic_target, max_cnt;
	__u64 offs, t;

	ca->ack_cnt += acked;	/* Count the number of ACKed segments */

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = now_us;

	if (ca->epoch_start == 0) {
		ca->epoch_start = now_us;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K = cubic_root(cube_factor *
					       (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}
	/* ... (cubic target bic_target and initial ca->cnt calculation) ... */

tcp_friendliness:
	delta = (cwnd * beta_scale) >> 3;
	if (delta) {
		ca->tcp_cwnd += ca->ack_cnt / delta;
		ca->ack_cnt %= delta;
	}

	if (ca->tcp_cwnd > cwnd) {
		max_cnt = cwnd / (ca->tcp_cwnd - cwnd);
		if (ca->cnt > max_cnt)
			ca->cnt = max_cnt;
	}

	ca->cnt = max(ca->cnt, 2U);
}
```

- **Top-of-function `ca->ack_cnt += acked;`**: Accumulates every newly acknowledged segment, even when the cubic curve evaluation is rate-limited or skipped via `goto tcp_friendliness;`.
- **Epoch initialisation (`ca->epoch_start == 0`)**: Sets `ca->ack_cnt = acked` and `ca->tcp_cwnd = cwnd`, anchoring the Reno-equivalent window $$W_{\text{est}}$$ at the post-recovery `cwnd`, and computes `ca->bic_K` and `ca->bic_origin_point` from the (potentially Fast-Convergence-reduced) `ca->last_max_cwnd`.
- **`tcp_friendliness:` block**:
  - Computes `delta = (cwnd * beta_scale) >> 3` ($$\approx 1.875 \times \text{cwnd}$$ ACKs).
  - Advances `ca->tcp_cwnd` by `ca->ack_cnt / delta` and retains `ca->ack_cnt %= delta` without loops.
  - When `ca->tcp_cwnd > cwnd`, clamps `ca->cnt` to `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` so CUBIC grows at least as fast as the emulated Reno window.

---

## 5. Understanding `test.pkt` and How to Run

`test.pkt` exercises both **Fast Convergence** and the **Reno-friendly region** over a simulated connection with RTT = 100 ms and MSS = 1000 bytes (1012 bytes with TCP timestamps).

### Packet Trace Walkthrough and Why `starter` Fails vs `solution` Passes

1. **Slow Start (`0.000s - 0.200s`, `cwnd: 10 -> 20`)**:
   - After the 3-way handshake, sending 10 segments (10,000 bytes) and receiving an ACK 100 ms later grows `cwnd` from `10` to `20` and records `ca->delay_min = 100,000 us` (100 ms).
2. **Loss Episode #1 at `cwnd = 20` (`0.200s - 0.400s`)**:
   - Sending 4 segments with the first segment dropped triggers 3 SACKs and enters Recovery.
   - Because `ca->last_max_cwnd` was `0`, `20 < 0` is false: `ca->last_max_cwnd` becomes `20` and `ssthresh` becomes `(20 * 717) / 1024 = 14`. Recovery finishes with `cwnd == 14`.
3. **Loss Episode #2 at `cwnd = 14 < last_max_cwnd (20)` (`0.400s - 0.600s`) — Fast Convergence Triggers**:
   - Before `cwnd` can return to `20`, another loss occurs at `cwnd = 14`.
   - `ssthresh` becomes `(14 * 717) / 1024 = 9`, and Recovery finishes with `cwnd == 9`.
   - **In `solution.bpf.c` (with Fast Convergence)**: Because `14 < 20`, `ca->last_max_cwnd` is lowered to `(14 * 1741) / 2048 = 11`.
   - **In `starter.bpf.c` (without Fast Convergence)**: `ca->last_max_cwnd` is set to `14`.
4. **Verifying Fast Convergence in Congestion Avoidance (Flight 1 & Flight 2)**:
   - **Flight 1 (`t_elapsed = 0.0s`)**: Sends 9 segments at `cwnd = 9`. With `bic_target = 10` (`ca->cnt = 9`), 9 ACKed segments grow `cwnd` from `9` to `10`.
   - **Flight 2 (`t_elapsed = 0.8s`, line 99 assertion `assert tcpi_snd_cwnd == 11`)**: After waiting 0.7 s, sends 10 segments and receives the ACK 0.1 s later (0.8 s into the epoch).
     - **Why `solution.bpf.c` passes**: With Fast Convergence (`last_max_cwnd = 11`, `bic_K = 1750` $$\approx 1.71\text{ s}$$), at $$t = 0.8\text{ s}$$ integer truncation gives `delta = 0`, so `bic_target` has already reached the plateau at `11`. Thus `ca->cnt = 10 / (11 - 10) = 10`, and 10 ACKed segments grow `cwnd` from `10` to `11`.
     - **Why `starter.bpf.c` fails**: Without Fast Convergence, `last_max_cwnd` is `14`, so at $$t = 0.8\text{ s}$$ `bic_target` is `13`. That produces `ca->cnt = 10 / (13 - 10) = 3`, causing 10 ACKed segments to jump `cwnd` from `10` to `13` and fail the assertion (`expected cwnd 11 (Fast Convergence W_max=11), got 13`).
5. **Verifying the Reno-friendly Region at the Plateau (Flights 3-6, `cwnd = 11 == last_max_cwnd`)**:
   - Once `cwnd` reaches `11`, four consecutive 11-segment flights are sent (`t_elapsed = 0.9s, 1.0s, 1.1s, 1.2s`).
   - Throughout this interval, $$t < K \approx 1.71\text{ s}$$, so pure CUBIC stays flat on its plateau (`bic_target == 11 == cwnd`, `ca->cnt = 100 * 11 = 1100`). If you implement Fast Convergence alone without the Reno-friendly region, `cwnd` stays stuck at `11` and fails the final assertion on line 125 (`assert tcpi_snd_cwnd > 11`).
   - **Why `solution.bpf.c` passes**: In the Reno-friendly region, `delta = (11 * 15) >> 3 = 20` ACKs advances `ca->tcp_cwnd` from `10 -> 11 -> 12` across the four flights. On Flight 6, `ca->tcp_cwnd` reaches `12` (`> cwnd = 11`), clamping `ca->cnt` from `1100` down to `max_cnt = 11 / (12 - 11) = 11` and growing `cwnd` to `12` (`> 11`).

### How to Run

Run the following commands to see `starter` fail and `solution` pass:

```sh
../../run_test.sh cubic/step02-reno-friendly-fast-conv starter
../../run_test.sh cubic/step02-reno-friendly-fast-conv solution
```
