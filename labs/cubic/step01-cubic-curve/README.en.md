# Step 1: Fixed-Point Cubic Root and the W_cubic(t) Curve (Concave, Plateau, and Convex)

## 1. Goal

In this step, you will implement the core building blocks of Linux's default congestion control algorithm, **TCP CUBIC** ([net/ipv4/tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c)), from scratch using BPF `struct_ops` (`bpf_cubic3`): **multiplicative decrease ($\beta \approx 0.7$)**, the **fixed-point cubic root `cubic_root()`**, and the **3rd-degree polynomial window growth curve $W_{\text{cubic}}(t)$**. For the full track structure and background, see the [CUBIC Track Overview (`../README.md`)](../README.md).

---

## 2. How the Algorithm Works (Read This First)

Before looking at the design questions or C code, let's build a complete picture from first principles: **why TCP Reno struggles on high-speed paths, how CUBIC's cubic polynomial and fixed-point arithmetic work, and which fields the kernel already provides in `struct tcp_sock` and `icsk_ca_priv`**.

### 2.1 Why TCP Reno Struggles on Long Fat Networks (LFNs)

TCP Reno (AIMD) halves `snd_cwnd` ($\beta = 0.5$) whenever a packet is lost, and then recovers linearly by adding **only $+1$ segment per RTT** during Congestion Avoidance.

On paths with both high bandwidth and long round-trip delay (**Long Fat Networks, LFNs**), growing by $+1$ per RTT is far too slow:
- For example, on a **10 Gbps link with RTT = 100 ms**, the Bandwidth-Delay Product (BDP) is roughly $83,333$ segments.
- If a single packet loss halves `snd_cwnd` from $83,333$ down to $41,666$, climbing back to $83,333$ at $+1$ segment per $100\text{ ms}$ takes **41,666 RTTs $\approx$ 69 minutes**!
- Moreover, because Reno's growth rate depends directly on the RTT clock ($+1$ per RTT), flows with short RTTs grab bandwidth much faster than flows with long RTTs (**RTT unfairness**).

### 2.2 The Core Idea of CUBIC: A Real-Time Cubic Polynomial Centred at $W_{\max}$

CUBIC solves both problems with two key ideas:

1. **Gentler Multiplicative Decrease ($\beta \approx 0.7$) and Remembering $W_{\max}$**:
   When packet loss occurs, CUBIC saves the pre-loss `snd_cwnd` as **$W_{\max}$ (`last_max_cwnd`)** — the window size where the link saturated — and reduces `snd_cwnd` to roughly **$0.7\times$** (`beta = 717 / 1024 ~= 0.7002`) instead of Reno's $0.5\times$.
2. **A 3rd-Degree Polynomial $W_{\text{cubic}}(t)$ Driven by Wall-Clock Time $t$**:
   Instead of incrementing the window by a fixed amount per RTT, CUBIC defines its target window as a cubic function of the elapsed wall-clock time $t$ since the current Congestion Avoidance epoch began, placing its inflection point (where the slope flattens to zero) right at $(t = K, W = W_{\max})$:

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$$

Here $C = 0.4$ is a scaling constant, and $K$ is the time it takes for $W_{\text{cubic}}(t)$ to climb from the initial post-recovery window $\text{cwnd}_{\text{epoch}}$ (at $t = 0$) back up to $W_{\max}$. Substituting $W_{\text{cubic}}(0) = \text{cwnd}_{\text{epoch}}$ gives $K$ as a cubic root:

$$K = \sqrt[3]{\frac{W_{\max} - \text{cwnd}_{\text{epoch}}}{C}}$$

```text
  cwnd
   ^
   |                                            /  <-- (3) Convex region (t > K)
   |                                          /        Gently probes for new capacity, then accelerates
W_max (20) +.................................*.........
   |                            . '   ' .
   |                        . '          <-- (2) Plateau (t ~= K)
   |                      /                  Slope flattens to ~0 around W_max to stabilise the link
   |                    /
   |                   /  <-- (1) Concave region (t < K)
cwnd_epoch (14) +     *       Climbs rapidly at first, slowing down as it nears W_max
   |
   +------------------+----------------------+---------> Elapsed time t
                    t = 0                  t = K
                (Epoch start)        (Inflection point)
```

A single cubic equation naturally produces three distinct phases:
- **(1) Concave region ($t < K$)**: Since $t - K < 0$, $W_{\text{cubic}}(t) = W_{\max} - C(K - t)^3$. Right after loss recovery, $|t - K|$ is large, so `cwnd` climbs aggressively back towards $W_{\max}$ and decelerates smoothly as $t \to K$.
- **(2) Plateau ($t \approx K$)**: Around the previous saturation point $W_{\max}$, $|t - K| \approx 0$ and the derivative of $(t - K)^3$ vanishes. CUBIC holds `cwnd` nearly constant, dwelling safely at full capacity without immediately causing another packet loss.
- **(3) Convex / Max-Probing region ($t > K$)**: If no loss occurs at $W_{\max}$, extra bandwidth may have become available. For $t > K$, $W_{\text{cubic}}(t) = W_{\max} + C(t - K)^3$ probes upward very slowly at first and then accelerates over time.

### 2.3 Pacing the Curve Over One RTT (`t + delay_min` and `ca->cnt`)

The kernel's generalised linear counter `bpf_cubic3_cong_avoid_ai(tp, ca->cnt, acked)` increases `snd_cwnd` by $+1$ for every `ca->cnt` acknowledged segments. How does CUBIC translate a continuous target curve $W_{\text{cubic}}(t)$ into the discrete interval `ca->cnt`?

1. **Looking one RTT ahead via $t = (\text{now} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}$**:
   `epoch_start` is initialised to `now_us` on the very first Congestion Avoidance ACK after recovery. If we evaluated $W_{\text{cubic}}(t)$ at $t = \text{now} - \text{epoch}_{\text{start}} = 0$, we would get $W_{\text{cubic}}(0) = \text{cwnd}$, leaving `bic_target == cwnd` and stalling growth for the entire first RTT!
   Instead, CUBIC adds the connection's minimum RTT (`delay_min`) to look one RTT ahead:
   $$t = (\text{now} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}$$
   Evaluating `bic_target` $= W_{\text{cubic}}(t)$ tells us **what window size we want to reach one RTT from now**.
2. **Converting `bic_target` into `ca->cnt`**:
   Over the next RTT, roughly `cwnd` segments will be ACKed. To grow the window from `cwnd` to `bic_target` over those `cwnd` ACKs (an increase of `bic_target - cwnd` segments), we must increment `snd_cwnd` by $+1$ every:
   - **When `bic_target > cwnd`**:
     $$\text{cnt} = \left\lfloor \frac{\text{cwnd}}{\text{bic}_{\text{target}} - \text{cwnd}} \right\rfloor$$
   - **When `bic_target <= cwnd` (at the $W_{\max}$ plateau)**:
     To prevent division by zero or negative values and keep the window virtually flat, CUBIC sets `ca->cnt = 100 * cwnd` (only $+1$ segment per 100 RTTs).
   - **Initial epoch with no prior loss (`last_max_cwnd == 0` and `ca->cnt > 20`)**:
     Caps `ca->cnt = 20`, and finally enforces a lower bound `ca->cnt = max(ca->cnt, 2U)`.

### 2.4 Fixed-Point Arithmetic and the Integer Cubic Root `cubic_root()`

Because neither the Linux kernel nor BPF allows floating-point instructions, CUBIC scales all fractional constants by powers of two ($1024 = 2^{10}$):

| Constant / Symbol | Value / Formula | Mathematical Meaning |
| :--- | :--- | :--- |
| `BICTCP_BETA_SCALE` | `1024` | Denominator for $\beta$ ($2^{10}$) |
| `beta` | `717` | Multiplicative decrease factor $\beta = 717 / 1024 \approx 0.7002$ |
| `BICTCP_HZ` | `10` | Time scale for $t$ and $K$: $1\text{ second} = 2^{10} = 1024\text{ units}$ (~$0.976\text{ ms}$ per unit) |
| `bic_scale` | `41` | Base scale factor (`cube_rtt_scale = bic_scale * 10 = 410`) |
| `cube_rtt_scale` | `410` | $C \times 1024 = 0.4 \times 1024 \approx 410$ (so $C = 410 / 2^{10}$) |
| `cube_factor` | `(1ULL << 40) / 410` ($= 2,681,735,677$) | Pre-scaled multiplier for $K$ in `BICTCP_HZ` units: $\frac{(2^{10})^3}{C} = \frac{2^{30}}{410 / 2^{10}} = \frac{2^{40}}{410}$ |

With these fixed-point constants, every formula reduces to integer shifts, multiplications, and divisions:
- **Time to origin `bic_K` (in `BICTCP_HZ` units)**:
  $$K_{\text{hz}} = \sqrt[3]{\text{cube}_{\text{factor}} \times (W_{\max} - \text{cwnd})}$$
- **Elapsed time `t` (microseconds $\to$ `BICTCP_HZ` units)**:
  $$\text{t} = \frac{((\text{now}_{\text{us}} - \text{epoch}_{\text{start}}) + \text{delay}_{\min}) \ll 10}{1,000,000}$$
- **Cubic distance from origin `delta` $= C |t - K|^3$**:
  To work purely with unsigned 64-bit integers (`__u64`), we first take the absolute distance $\text{offs} = |t - \text{bic}_{K}|$, and then shift right by $10 + 3 \times 10 = 40$ bits to undo the scaling of $C$ ($2^{10}$) and $\text{offs}^3$ ($2^{30}$):
  $$\text{delta} = (\text{cube}_{\text{rtt,scale}} \times \text{offs}^3) \gg (10 + 3 \times \text{BICTCP}_{\text{HZ}})$$
  In the concave region (`t < ca->bic_K`), `bic_target = ca->bic_origin_point - delta`; in the convex region (`t >= ca->bic_K`), `bic_target = ca->bic_origin_point + delta`.

#### How `cubic_root(__u64 a)` Computes $\lfloor \sqrt[3]{a} \rfloor$ in Integer Arithmetic

To compute the cubic root of a 64-bit integer $a$ without floating-point operations, CUBIC uses a three-step algorithm from [tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c):
1. **Divide the bit-width by 3 (exponent extraction)**:
   `fls64(a)` returns the 1-based position of the most significant set bit ($1 \dots 64$). Computing `b = ((fls64(a) * 84) >> 8) - 1` divides that bit position by 3 (since $84 / 256 \approx 1/3$), giving the number of 3-bit octets to shift. (For small inputs $a < 64$, it returns `((__u32)v[(__u32)a] + 35) >> 6` directly.)
2. **64-entry lookup table `v[]` for the initial estimate**:
   `shift = a >> (b * 3)` extracts the top 6 bits of $a$ into `[0, 63]`. Indexing the precomputed table `v[shift]` and scaling back by `<< b` gives a close initial estimate `x = (((__u32)v[shift] + 10) << b) >> 6`.
   (*Important BPF Verifier rule*: Even though `shift` is mathematically always in `[0, 63]`, the BPF verifier cannot prove the upper bound of a variable shift `a >> (b * 3)`. We must place an explicit `if (shift >= 64) return 0;` guard right before `v[shift]` so the verifier can prove the array access is in bounds.)
3. **One Newton-Raphson iteration**:
   Solving $f(x) = x^3 - a = 0$ via one step of $x_1 = \frac{1}{3}\left(2x_0 + \frac{a}{x_0^2}\right)$ refines the estimate to ~$0.195\%$ average error, using `x * (x - 1)` for the denominator and $341 / 1024 \approx 1/3$:
   ```c
   x = (2 * x + (__u32)div64_u64(a, (__u64)x * (__u64)(x - 1)));
   x = ((x * 341) >> 10);
   ```

### 2.5 Concrete Numerical Walkthrough (Matching `test.pkt`)

Let's trace the exact numbers on a connection with RTT = $100\text{ ms}$ (`delay_min = 100,000` usec) that experiences packet loss at `cwnd = 20`:

1. **On packet loss (`bpf_cubic3_ssthresh()`)**:
   - Saves $W_{\max}$ as `last_max_cwnd = 20` and resets `epoch_start = 0`.
   - Returns the new `ssthresh` (and post-recovery `cwnd`): $\lfloor 20 \times 717 / 1024 \rfloor = 14$.
2. **At epoch start (Flight 1 right after recovery, $\text{cwnd} = 14$)**:
   - `bic_origin_point = 20`
   - `bic_K = cubic_root(2,681,735,677 * (20 - 14)) = cubic_root(16,090,414,062) = 2523` ($2523 / 1024 \approx 2.46\text{ s}$)
3. **Evolution of `bic_target` and `ca->cnt` across all five flights**:

| Flight | Elapsed $\text{now} - \text{epoch}_{\text{start}}$ | Lookahead $t$ (`BICTCP_HZ`) | $\text{offs} = \|t - 2523\|$ | $\text{delta} = C \cdot \text{offs}^3$ | Region & `bic_target` | `ca->cnt` | `cwnd` after 1 RTT |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Flight 1** | $0.0\text{ s}$ | $102$ ($0.1\text{ s}$) | $2421$ | $5$ | Concave: $20 - 5 = 15$ | $14 / (15 - 14) = 14$ | $14 \to 15$ |
| **Flight 2** | $0.6\text{ s}$ | $716$ ($0.7\text{ s}$) | $1807$ | $2$ | Concave: $20 - 2 = 18$ | $15 / (18 - 15) = 5$ | $15 \to 18$ ($+3$ jump) |
| **Flight 3** | $1.8\text{ s}$ | $1945$ ($1.9\text{ s}$) | $578$ | $0$ | Origin reached: $20 - 0 = 20$ | $18 / (20 - 18) = 9$ | $18 \to 20$ ($W_{\max}$) |
| **Flight 4** | $2.1\text{ s}$ | $2252$ ($2.2\text{ s}$) | $271$ | $0$ | Plateau: $20 \le \text{cwnd}$ | $100 \times 20 = 2000$ | $20 \to 20$ (flat plateau) |
| **Flight 5** | $4.0\text{ s}$ | $4198$ ($4.1\text{ s}$) | $1675$ | $1$ | Convex: $20 + 1 = 21$ | $20 / (21 - 20) = 20$ | $20 \to 21$ (probing) |

### 2.6 What the Kernel Provides vs. Our Private Struct (`icsk_ca_priv`)

Here is the complete list of fields provided by the kernel and the per-socket CUBIC state we store in `icsk_ca_priv`:

1. **`struct tcp_sock *tp = tcp_sk(sk)` (Kernel-maintained TCP state)**:
   - `tp->snd_cwnd`: Current congestion window (in segments).
   - `tp->snd_ssthresh`: Slow start threshold (`tcp_in_slow_start(tp)` checks `tp->snd_cwnd < tp->snd_ssthresh`).
   - `tp->snd_cwnd_cnt`: Linear ACK counter used by `bpf_cubic3_cong_avoid_ai()` to accumulate `acked` segments until `tp->snd_cwnd_cnt >= w`.
   - `tp->snd_cwnd_clamp`: Upper bound on `snd_cwnd`.
   - `tp->prior_cwnd`: Saved `snd_cwnd` prior to loss recovery (used by `.undo_cwnd`).
   - `tp->tcp_mstamp`: Monotonic per-socket timestamp in microseconds, read via `bictcp_clock_us(sk)`.
2. **`const struct ack_sample *sample` (Passed to `.pkts_acked`)**:
   - `sample->rtt_us`: Latest RTT measurement in microseconds (or `< 0` if unavailable).
3. **`inet_csk_ca(sk)` / `icsk_ca_priv` (Our private `struct bpf_cubic3_ca`)**:
   - `struct inet_connection_sock` reserves an inline byte array `icsk_ca_priv` for congestion control modules, accessed via `inet_csk_ca(sk)`.
   - [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c) already defines `struct bpf_cubic3_ca` with:
     - `cnt`: Number of ACKed segments required to increment `snd_cwnd` by 1.
     - `last_max_cwnd`: $W_{\max}$ saved at the most recent packet loss.
     - `last_cwnd`, `last_time`: Cache used to skip redundant `bictcp_update()` calls within $1/32\text{ s}$ when `cwnd` has not changed.
     - `bic_origin_point`: Origin window of the cubic curve in the current epoch ($W_{\max}$).
     - `bic_K`: Time from `epoch_start` to reach `bic_origin_point` (in `BICTCP_HZ` units).
     - `delay_min`: Minimum RTT observed in `.pkts_acked` (in microseconds).
     - `epoch_start`: Timestamp when the current Congestion Avoidance epoch started (in microseconds; `0` means no active epoch).

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Using what you just learned in Section 2, think through these four design questions before filling in the `TODO`s in [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c).

### Q1. Why do we compute `bic_K` only once when `ca->epoch_start == 0` instead of on every ACK?

`bictcp_update()` runs on every ACK during Congestion Avoidance, yet `ca->bic_K = cubic_root(...)` is only executed inside `if (ca->epoch_start == 0)`. Why doesn't `bic_K` need to be recomputed as `cwnd` grows during the epoch, and when does `ca->epoch_start` get reset to `0`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Look at the formula $K = \sqrt[3]{(W_{\max} - \text{cwnd}_{\text{epoch}}) / C}$ in Section 2.2. Do any of its inputs change while the socket stays in the same Congestion Avoidance epoch?
- **Answer**:
  - $K$ is the fixed time distance from the start of the epoch ($t = 0$, where the window was $\text{cwnd}_{\text{epoch}}$) to the inflection point $W_{\max}$. Because the cubic curve itself stays fixed throughout the epoch, computing `ca->bic_K` and `ca->bic_origin_point` once when `ca->epoch_start == 0` lets every subsequent ACK evaluate $W_{\text{cubic}}(t)$ with simple subtraction and cubing, avoiding repeated `cubic_root()` calls.
  - `ca->epoch_start` is reset to `0` on packet loss in `bpf_cubic3_ssthresh()` (and on RTO timeout in `bpf_cubic3_set_state()` via `bictcp_reset(ca)`). If `ca->last_max_cwnd <= cwnd` at the start of an epoch, we are already at or above the historical maximum, so we set `ca->bic_K = 0` and `ca->bic_origin_point = cwnd` to start immediately in the convex region.

</details>

### Q2. Why must we add `ca->delay_min` when computing `t` in `bictcp_update()`?

In `bictcp_update()`, elapsed time is computed as `(now_us - ca->epoch_start) + ca->delay_min`. What would happen on Flight 1 (`now_us == ca->epoch_start`) if we omitted `+ ca->delay_min`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Check Section 2.3 and the Flight 1 row in Section 2.5's table: what is $W_{\text{cubic}}(0)$?
- **Answer**:
  - On the first ACK of a new epoch, `ca->epoch_start` is set to `now_us`, so `now_us - ca->epoch_start == 0`. Without `ca->delay_min`, $t = 0$ and $W_{\text{cubic}}(0) = \text{cwnd}_{\text{epoch}} = \text{cwnd}$.
  - That would make `bic_target == cwnd`, sending execution into the plateau branch (`ca->cnt = 100 * cwnd`) and freezing window growth for the first RTT.
  - Adding `ca->delay_min` evaluates the curve one RTT into the future, giving the target window `bic_target` at the end of the current RTT so `ca->cnt = cwnd / (bic_target - cwnd)` paces growth properly across the incoming flight of ACKs.

</details>

### Q3. How do we evaluate both the concave ($t < K$) and convex ($t \ge K$) regions using only unsigned integers (`__u64`)?

In $W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$, the term $(t - K)^3$ is negative when $t < K$ and positive when $t > K$. How should we structure the C code with unsigned variables `t`, `offs`, and `delta` to avoid unsigned underflow?

<details>
<summary>Hint and Answer</summary>

- **Hint**: As shown in Section 2.4, separate the magnitude $|t - K|$ from the sign of $(t - K)$.
- **Answer**:
  1. Compute the non-negative distance from the inflection point:
     ```c
     if (t < ca->bic_K)
         offs = ca->bic_K - t;
     else
         offs = t - ca->bic_K;
     ```
  2. Compute the non-negative cubic offset `delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ);`.
  3. Subtract `delta` from `ca->bic_origin_point` in the concave region (`t < ca->bic_K`) and add `delta` to `ca->bic_origin_point` in the convex region (`t >= ca->bic_K`).

</details>

### Q4. Why does the BPF verifier require `if (shift >= 64) return 0;` before `v[shift]` in `cubic_root()`?

In `cubic_root(__u64 a)`, `b = fls64(a); b = ((b * 84) >> 8) - 1; shift = (a >> (b * 3));` normalises `a` so that `shift` is mathematically in `[0, 63]`. Why does loading the BPF program fail unless we add an explicit bounds check before reading `v[shift]`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Recall the BPF verifier note in Section 2.4 regarding variable bit shifts and static array lookups.
- **Answer**:
  - The BPF verifier tracks scalar register ranges instruction by instruction, but cannot algebraically prove across `fls64(a)` and the variable shift `a >> (b * 3)` that the resulting `shift` is strictly less than 64.
  - Without an explicit check, the verifier rejects `v[shift]` as a potential out-of-bounds read on the 64-byte `.rodata` array `v[]`.
  - Adding `if (shift >= 64) return 0;` immediately before `v[shift]` restricts the register range to `[0, 63]` on the fallthrough path, satisfying the verifier.

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

Let's walk through each function and fixed-point calculation in [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/solution.bpf.c).

### 4.1 `cubic_root(__u64 a)` — Table Lookup and Newton-Raphson Iteration (TODO 1)

```c
static __always_inline __u32 cubic_root(__u64 a)
{
	__u32 x, b, shift;

	if (a < 64)
		return ((__u32)v[(__u32)a] + 35) >> 6;

	b = fls64(a);
	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	/* Bounds check required by the BPF verifier for v[shift] */
	if (shift >= 64)
		return 0;

	x = ((__u32)(((__u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration:
	 *   x_{k+1} = (2 * x_k + a / x_k^2) / 3
	 * Note: 341 / 1024 ~= 1 / 3.
	 */
	x = (2 * x + (__u32)div64_u64(a, (__u64)x * (__u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}
```

- **Fast path for $a < 64$**: Directly indexes `v[a]` with rounding (`+ 35`) and shifts right by 6.
- **Exponent `b` and top 6 bits `shift`**: Uses `fls64(a)` and `b = ((b * 84) >> 8) - 1` to find the 3-bit shift count, then normalises `a` via `shift = a >> (b * 3)`.
- **Verifier bounds check and initial guess**: Guards `if (shift >= 64) return 0;`, then computes `x = (((v[shift] + 10) << b) >> 6)`.
- **One Newton-Raphson step**: Refines `x` using `div64_u64(a, (__u64)x * (__u64)(x - 1))` and multiplies by `341 >> 10` ($\approx 1/3$).

### 4.2 `bictcp_update()` — Evaluating $W_{\text{cubic}}(t)$ and `ca->cnt` (TODO 2)

```c
static __always_inline void bictcp_update(struct sock *sk, __u32 cwnd, __u32 acked)
{
	struct bpf_cubic3_ca *ca = inet_csk_ca(sk);
	__u32 now_us = bictcp_clock_us(sk);
	__u32 delta, bic_target;
	__u64 offs, t;

	if (ca->last_cwnd == cwnd &&
	    (__s32)(now_us - ca->last_time) <= (USEC_PER_SEC / 32))
		return;

	if (ca->epoch_start && now_us == ca->last_time)
		goto out;

	ca->last_cwnd = cwnd;
	ca->last_time = now_us;

	if (ca->epoch_start == 0) {
		ca->epoch_start = now_us;

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			ca->bic_K = cubic_root(cube_factor *
					       (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	t = (__u64)((__s32)(now_us - ca->epoch_start) + ca->delay_min);
	t <<= BICTCP_HZ;
	t /= USEC_PER_SEC;

	if (t < ca->bic_K)
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* delta = C * |t - K|^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ);
	if (t < ca->bic_K)	/* Concave region: below origin W_max */
		bic_target = ca->bic_origin_point - delta;
	else			/* Convex region: above origin W_max */
		bic_target = ca->bic_origin_point + delta;

	/* Convert target window into ACK count interval ca->cnt */
	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = 100 * cwnd;	/* Very small increment at plateau */

	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;

out:
	ca->cnt = max(ca->cnt, 2U);
}
```

- **Rate-limiting guard**: Skips recalculation if `cwnd` is unchanged and less than $1/32\text{ s}$ ($31.25\text{ ms}$) has elapsed since `last_time`.
- **Epoch initialisation**: When `ca->epoch_start == 0`, records `now_us` and computes `ca->bic_K = cubic_root(cube_factor * (ca->last_max_cwnd - cwnd))` if `ca->last_max_cwnd > cwnd`.
- **Evaluating the cubic curve**: Converts `(now_us - ca->epoch_start) + ca->delay_min` to `BICTCP_HZ` units, computes `offs = |t - ca->bic_K|` and `delta`, and sets `bic_target` in the concave (`t < ca->bic_K`) or convex (`t >= ca->bic_K`) region.
- **Computing `ca->cnt`**: Divides `cwnd / (bic_target - cwnd)` when `bic_target > cwnd`, or sets `100 * cwnd` at the plateau, clamped to at least `2U`.

### 4.3 `bpf_cubic3_ssthresh()` — Multiplicative Decrease with $\beta \approx 0.7$ (TODO 3)

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_cubic3_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_cubic3_ca *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;
	ca->last_max_cwnd = tp->snd_cwnd;

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}
```

- Resets `ca->epoch_start = 0` so the next Congestion Avoidance ACK starts a fresh epoch, and saves `ca->last_max_cwnd = tp->snd_cwnd` ($W_{\max}$; Fast Convergence and Reno-friendly mode will be added in Step 2).
- Returns `max((tp->snd_cwnd * 717) / 1024, 2U)`.

### 4.4 Other Callbacks Provided in the Skeleton

- `bpf_cubic3_init()`: Zeroes out `struct bpf_cubic3_ca` via `bictcp_reset(ca)` when the socket is initialised.
- `bpf_cubic3_cong_avoid()`: Checks `tcp_is_cwnd_limited(sk)`, grows `snd_cwnd` up to `snd_ssthresh` via `bpf_cubic3_slow_start()` if in Slow Start, and then calls `bictcp_update()` followed by `bpf_cubic3_cong_avoid_ai(tp, ca->cnt, acked)`.
- `bpf_cubic3_set_state()`: Resets CUBIC state via `bictcp_reset(ca)` when entering `TCP_CA_Loss` (RTO).
- `bpf_cubic3_acked()`: Tracks the minimum RTT `ca->delay_min` from `sample->rtt_us` (ignoring samples during the first 1 second after Fast Recovery).
- `bpf_cubic3_undo_cwnd()`: Restores `max(tp->snd_cwnd, tp->prior_cwnd)` on spurious loss recovery.

---

## 5. Understanding `test.pkt` and How to Run

### 5.1 Packet Trace Walkthrough (`test.pkt`)

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/test.pkt) establishes a connection with a 1000-byte effective MSS and RTT = 100 ms (`delay_min = 100,000` usec) and verifies the exact numbers from Section 2.5 across seven stages:

1. **Handshake and Slow Start (`cwnd: 10 -> 20`)**:
   Sends 10 segments (`10000` bytes). The ACK at $+0.100\text{ s}$ grows `tcpi_snd_cwnd` from 10 to 20 and records `delay_min = 100 ms` in `bpf_cubic3_acked()`.
2. **Packet Loss at `cwnd = 20` and $\beta = 0.7$ Assertion (`ssthresh == 14`)**:
   Sends 4 segments (`10001:14001`) where the first segment (`10001:11001`) is lost. Three SACKs trigger Fast Recovery (`TCP_CA_Recovery`) and invoke `bpf_cubic3_ssthresh()`.
   - **Why `starter.bpf.c` fails**: `bpf_cubic3_ssthresh()` in [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve/starter.bpf.c) returns Reno's $0.5\times$ reduction (`10`), immediately failing line 42 (`assert tcpi_snd_ssthresh == 14`). Furthermore, its `bictcp_update()` leaves `ca->cnt = cwnd`, which would fail the subsequent concave and plateau assertions.
   - **Why `solution.bpf.c` passes**: `20 * 717 / 1024 = 14` satisfies `tcpi_snd_ssthresh == 14`, and the recovery ACK at line 46 exits Fast Recovery into `TCP_CA_Open` with `tcpi_snd_cwnd == 14` and $W_{\max} = 20$.
3. **Flight 1 (Epoch start, Concave region, $t_{\text{elapsed}} = 0.0\text{ s}$)**:
   Sending 14 segments and receiving the ACK initialises `bic_K = 2523` (~$2.46\text{ s}$), `bic_target = 15`, and `ca->cnt = 14`, advancing `tcpi_snd_cwnd` from `14` to `15`.
4. **Flight 2 (Steep Concave climb, $t_{\text{elapsed}} = 0.6\text{ s}$)**:
   At $t_{\text{elapsed}} = 0.6\text{ s}$, `bic_target = 18` and `ca->cnt = 15 / (18 - 15) = 5`. ACKing 15 segments jumps `tcpi_snd_cwnd` by $+3$ in a single RTT to `18`.
5. **Flight 3 (Reaching the origin $W_{\max} = 20$, $t_{\text{elapsed}} = 1.8\text{ s}$)**:
   Approaching $K \approx 2.46\text{ s}$, `delta = 0`, `bic_target = 20`, and `ca->cnt = 18 / (20 - 18) = 9`. ACKing 18 segments brings `tcpi_snd_cwnd` to `20` ($W_{\max}$).
6. **Flight 4 (Plateau at $W_{\max} = 20$ near $t \approx K$, $t_{\text{elapsed}} = 2.1\text{ s}$)**:
   With `cwnd == 20` and `bic_target == 20`, `bic_target <= cwnd` sets `ca->cnt = 100 * 20 = 2000`. Even after 20 segments are ACKed, `tcpi_snd_cwnd` stays flat at `20`.
7. **Flight 5 (Convex bandwidth probing, $t_{\text{elapsed}} = 4.0\text{ s} > K$)**:
   Past $K \approx 2.46\text{ s}$, `t > bic_K` and `delta = 1` push `bic_target = 20 + 1 = 21 > cwnd` (`ca->cnt = 20`), growing `tcpi_snd_cwnd` beyond `20`.

### 5.2 How to Run

Run the following commands from `/usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step01-cubic-curve` (or any step directory):

```sh
../../run_test.sh cubic/step01-cubic-curve starter
../../run_test.sh cubic/step01-cubic-curve solution
```
