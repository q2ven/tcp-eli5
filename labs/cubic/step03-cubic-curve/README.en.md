# Step 3: Fixed-Point Cubic Root and the W_cubic(t) Curve (Concave & Convex)

## Goal

In this step, you will implement the core 3rd-degree polynomial congestion window growth curve $W_{\text{cubic}}(t)$ of Linux's default congestion control algorithm, **CUBIC** ([net/ipv4/tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c)), using BPF `struct_ops` (`bpf_cubic3`).

Specifically, you will learn:

- **Multiplicative Decrease ($\beta = 0.7$)**: How CUBIC reduces `snd_cwnd` to roughly $0.7\times$ (`717 / 1024`) rather than Reno's $0.5\times$ on packet loss, while remembering the pre-loss window size as $W_{\max}$ (`last_max_cwnd`).
- **Fixed-Point Cubic Root `cubic_root()`**: How to approximate the cubic root of a 64-bit integer with ~$0.195\%$ average error in a kernel/BPF environment without floating-point instructions, combining `fls64()`, a 64-entry lookup table `v[]`, and a single Newton-Raphson iteration — plus the explicit bounds check required to satisfy the BPF verifier.
- **The Cubic Window Curve $W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$**:
  - **Concave region ($t < K$)**: Rapidly climbs towards $W_{\max}$ and flattens as it approaches the inflection point.
  - **Plateau ($t \approx K$)**: Holds the congestion window nearly constant (`ca->cnt = 100 * cwnd`) around $W_{\max}$ to stabilise the network.
  - **Convex region ($t > K$)**: Gently resumes window growth and accelerates over time to probe for new available bandwidth.

---

## Design Checkpoints (Think before coding)

Before opening [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/starter.bpf.c) or [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/solution.bpf.c), think through the following four design questions on your own.

### Q1. What per-socket state do we need in `inet_csk_ca(sk)`?

To evaluate $W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$ on incoming ACKs and compute `cnt` (the number of ACKed packets needed to increment `snd_cwnd` by 1 in `tcp_cong_avoid_ai()`), what fields belong in our private state struct? What should we cache at the start of an epoch so we do not recompute `cubic_root()` on every packet?

<details>
<summary>Hints and Answer</summary>

- We need `last_max_cwnd` to store the window size $W_{\max}$ at the last loss event, and `bic_origin_point` for the origin of the current cubic curve.
- By caching the epoch start timestamp `epoch_start` and the time-to-origin `bic_K` (computed once via `cubic_root()` when `epoch_start == 0`), subsequent ACKs only need to compute $(t - K)^3$ without repeating the cubic root.
- We also track the minimum RTT `delay_min` (added to the elapsed time $t$), the resulting increment interval `cnt`, and `last_cwnd` / `last_time` to rate-limit redundant recalculations.

</details>

### Q2. Why does CUBIC add `delay_min` (minimum RTT) to the elapsed time `t`?

`epoch_start` is initialised when the first Congestion Avoidance ACK arrives after Fast Recovery completes. If we computed $t = \text{now} - \text{epoch\_start}$ without adding `delay_min`, what would happen during the first RTT of Congestion Avoidance?

<details>
<summary>Hints and Answer</summary>

- On the very first ACK of the epoch, `now - epoch_start == 0`. Without `delay_min`, the target window would be evaluated at $t = 0$, giving $W_{\text{cubic}}(0) = \text{cwnd}$, which results in `bic_target == cwnd` and stalls window growth for the first RTT.
- By computing $t = (\text{now} - \text{epoch\_start}) + \text{delay\_min}$, CUBIC looks ahead to where the window curve will be **one RTT in the future** (`bic_target`) and paces the growth across the current RTT via `cnt = cwnd / (bic_target - cwnd)`.

</details>

### Q3. How do we evaluate both the concave ($t < K$) and convex ($t > K$) regions using only unsigned 64-bit integers (`__u64`)?

Mathematically, $(t - K)^3$ is negative when $t < K$ and positive when $t > K$. How can we compute $W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$ cleanly with unsigned integers without signed overflow issues?

<details>
<summary>Hints and Answer</summary>

- First, compute the unsigned distance from the inflection point: `offs = (t < ca->bic_K) ? (ca->bic_K - t) : (t - ca->bic_K)`.
- Next, compute the non-negative cubic offset `delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ)`.
- Finally, subtract `delta` from `ca->bic_origin_point` in the concave region (`t < ca->bic_K`) and add `delta` to `ca->bic_origin_point` in the convex region (`t >= ca->bic_K`).

</details>

### Q4. Why does the BPF verifier reject `v[shift]` without an explicit bounds check?

In `cubic_root(__u64 a)`, `b = fls64(a); b = ((b * 84) >> 8) - 1; shift = a >> (b * 3);` extracts the top 6 bits of `a` to index into the 64-byte lookup table `v[shift]`. Mathematically `shift` is always in `[0, 63]`, yet the BPF verifier rejects a direct `v[shift]` lookup unless we add a check. Why, and how do we fix it?

<details>
<summary>Hints and Answer</summary>

- The BPF verifier tracks value ranges instruction by instruction, and cannot algebraically prove across `fls64(a)` and the variable shift `a >> (b * 3)` that `shift` is strictly less than 64. It therefore flags `v[shift]` as a potential out-of-bounds read on the 64-byte `.rodata` array.
- Adding `if (shift >= 64) return 0;` right before indexing `v[shift]` bounds the register range to `[0, 63]` for the verifier.

</details>

---

## Step-by-step Walkthrough

### 1. Fixed-Point Constants and `BICTCP_HZ`

CUBIC defines its target window curve as ($C = 0.4, \beta = 0.7$):

$$W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}, \quad K = \sqrt[3]{\frac{W_{\max} - \text{cwnd}}{C}}$$

In [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/solution.bpf.c), we use the exact fixed-point scaling constants from [tcp_cubic.c](file:///usr/local/google/home/kuniyu/kernel/tcp-eli5-src/net/ipv4/tcp_cubic.c):

- `BICTCP_BETA_SCALE = 1024`, `beta = 717`: $\beta = 717 / 1024 \approx 0.7002$.
- `BICTCP_HZ = 10`: Time is measured in units of $2^{-10}\text{ s}$ ($1024\text{ units} = 1\text{ second}$).
- `bic_scale = 41`, `cube_rtt_scale = bic_scale * 10 = 410`: $C = 410 / 1024 \approx 0.4004$.
- `cube_factor = (1ULL << (10 + 3 * BICTCP_HZ)) / (bic_scale * 10)`:
  To express $K$ in `BICTCP_HZ` ($2^{10}$) units, the radicand is scaled by $(2^{10})^3 = 2^{30}$ and divided by $C = 410 / 2^{10}$, yielding $2^{40} / 410 = 2,681,735,677$.

### 2. `fls64()` and `cubic_root()`

1. **Locate the most significant bit**: `fls64(a)` finds the 1-based MSB position ($1 \dots 64$), and `b = ((b * 84) >> 8) - 1` computes $\lfloor (\text{fls64}(a) - 1) / 3 \rfloor$ (since $84 / 256 \approx 1/3$).
2. **Lookup table initial estimate**: `shift = a >> (b * 3)` normalises `a` into `[0, 63]`. After the verifier guard `if (shift >= 64) return 0;`, `x = (((v[shift] + 10) << b) >> 6)` produces an initial approximation of $\sqrt[3]{a}$.
3. **One Newton-Raphson iteration**:
   Applying $x_{k+1} = \frac{1}{3}\left(2x_k + \frac{a}{x_k^2}\right)$ via `x = (2 * x + div64_u64(a, x * (x - 1)))` and `x = (x * 341) >> 10` ($341 / 1024 \approx 1/3$) refines the cubic root to ~0.195% average error.

### 3. `bpf_cubic3_ssthresh()` — Multiplicative Decrease ($\beta \approx 0.7$)

When packet loss is detected, `.ssthresh` (`bpf_cubic3_ssthresh()`) is invoked:

- Resets `ca->epoch_start = 0` so the next Congestion Avoidance ACK starts a fresh CUBIC epoch.
- Saves `ca->last_max_cwnd = tp->snd_cwnd` ($W_{\max}$; Fast Convergence will be added in Step 4).
- Returns `max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U)`. For `cwnd = 20`, this yields `20 * 717 / 1024 = 14` (compared to Reno's `10`).

### 4. `bictcp_update()` — Concave, Plateau, and Convex Regions

On each Congestion Avoidance ACK, `bictcp_update()` updates `ca->cnt`:

1. **Epoch initialisation (`ca->epoch_start == 0`)**:
   - Records `ca->epoch_start = now_us`.
   - If `ca->last_max_cwnd <= cwnd`, sets `ca->bic_K = 0` and `ca->bic_origin_point = cwnd`.
   - Otherwise, computes `ca->bic_K = cubic_root(cube_factor * (ca->last_max_cwnd - cwnd))` and sets `ca->bic_origin_point = ca->last_max_cwnd`.
2. **Evaluating $t$ and $\Delta = C |t - K|^3$**:
   - Converts `(now_us - ca->epoch_start) + ca->delay_min` from microseconds to `BICTCP_HZ` units via `t = (t << BICTCP_HZ) / USEC_PER_SEC`.
   - Computes `offs = |t - ca->bic_K|` and `delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ)`.
   - Sets `bic_target = ca->bic_origin_point - delta` in the **concave** region (`t < ca->bic_K`) and `bic_target = ca->bic_origin_point + delta` in the **convex** region (`t >= ca->bic_K`).
3. **Converting `bic_target` into `ca->cnt`**:
   - If `bic_target > cwnd`, sets `ca->cnt = cwnd / (bic_target - cwnd)`.
   - Otherwise (at the $W_{\max}$ plateau), sets `ca->cnt = 100 * cwnd` for a near-zero slope.
   - Clamps `ca->cnt = max(ca->cnt, 2U)` and passes it to `bpf_cubic3_cong_avoid_ai()`.

---

## Understanding `test.pkt`

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/test.pkt) drives a connection with RTT = 100ms (`delay_min = 100,000` usec) through all phases of the CUBIC curve:

1. **Slow Start (`cwnd: 10 -> 20`)**:
   Sends 10 segments (10,000 bytes). The ACK grows `tcpi_snd_cwnd` from 10 to 20 and records `delay_min = 100ms` in `bpf_cubic3_acked()`.
2. **Packet Loss at `cwnd = 20` and $\beta = 0.7$ Check**:
   Sends 4 segments where the first segment is lost. Three SACKs trigger Fast Recovery and invoke `bpf_cubic3_ssthresh()`, asserting `tcpi_snd_ssthresh == 14` (`20 * 717 / 1024 = 14`, where [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/cubic/step03-cubic-curve/starter.bpf.c) fails with Reno's `10`).
3. **Flight 1 (Epoch start, Concave region, $t_{\text{elapsed}} = 0.0\text{ s}$)**:
   After recovery completes at `cwnd = 14`, Flight 1 initialises the epoch with `bic_K = cubic_root(cube_factor * 6) = 2523` (~$2.46\text{ s}$), `t = 102`, `delta = 5`, `bic_target = 15`, and `ca->cnt = 14`. ACKing 14 packets advances `tcpi_snd_cwnd` to `15`.
4. **Flight 2 (Steep Concave climb, $t_{\text{elapsed}} = 0.6\text{ s}$)**:
   At $t_{\text{elapsed}} = 0.6\text{ s}$, `delta = 2` and `bic_target = 18`, giving `ca->cnt = 15 / (18 - 15) = 5`. ACKing 15 packets increases `cwnd` by `15 / 5 = +3` in a single RTT to `tcpi_snd_cwnd == 18`.
5. **Flight 3 (Reaching the origin $W_{\max} = 20$, $t_{\text{elapsed}} = 1.8\text{ s}$)**:
   Approaching $K \approx 2.46\text{ s}$, `delta = 0` and `bic_target = 20`, giving `ca->cnt = 18 / (20 - 18) = 9`. ACKing 18 packets brings `tcpi_snd_cwnd` to `20` ($W_{\max}$).
6. **Flight 4 (Plateau at $W_{\max} = 20$ near $t \approx K$, $t_{\text{elapsed}} = 2.1\text{ s}$)**:
   With `cwnd == 20` and `bic_target == 20`, `bic_target <= cwnd` sets `ca->cnt = 100 * cwnd = 2000`. Even after 20 segments are ACKed, `tcpi_snd_cwnd` remains flat at `20`.
7. **Flight 5 (Convex bandwidth probing, $t_{\text{elapsed}} = 4.0\text{ s} > K$)**:
   Past $K$, `t > bic_K` and `delta = 1` push `bic_target = 20 + 1 = 21 > cwnd`. ACKing 20 segments grows `tcpi_snd_cwnd` past `20` into the convex probing region.

---

## How to run

```sh
# 1. Verify that starter.bpf.c fails on the packetdrill assertion
../../run_test.sh cubic/step03-cubic-curve starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh cubic/step03-cubic-curve solution
```
