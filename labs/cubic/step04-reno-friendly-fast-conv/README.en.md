# Step 4: Reno-friendly Region ($W_{\text{est}}$) and Fast Convergence

## Goal

The pure CUBIC curve implemented in Step 3 ($W_{\text{cubic}}(t) = C(t - K)^3 + W_{\max}$) has two practical shortcomings on real networks:

- **Problem 1 (Slow convergence when new flows join)**: When an existing CUBIC flow with a large $W_{\max}$ is joined by a new competing flow, the fair bandwidth share shrinks. Because the cubic curve plateaus around the old $W_{\max}$, the existing flow is slow to release bandwidth.
- **Problem 2 (Underperforming TCP Reno on short-RTT / low-BDP paths)**: Because the cubic function grows as a function of wall-clock time $t$, on short-RTT paths or at small window sizes, its growth rate can be slower than standard TCP Reno (AIMD), which adds 1 MSS per RTT.

In this step, you will implement **Fast Convergence** and the **Reno-friendly region ($W_{\text{est}}$ / `tcp_cwnd`)** to complete `bpf_cubic4`.

---

## Design Checkpoints (Think before coding)

Before opening `starter.bpf.c` or `solution.bpf.c`, think through the following four questions on your own.

### Q1. In which hook and under what condition can the sender detect that a new competing flow has likely joined?

When packet loss occurs, if you compare the current `cwnd` against the previous maximum window `ca->last_max_cwnd` ($W_{\max}$), what does it tell you about the bottleneck share?

<details>
<summary>Hints and Answer</summary>

- **Hook**: `.ssthresh` (`bpf_cubic4_ssthresh()`), which is invoked when packet loss is detected.
- **Condition**: If another loss happens before `cwnd` could even reach the previous `ca->last_max_cwnd` — i.e. **`cwnd < ca->last_max_cwnd`** — it indicates that the available bandwidth share has shrunk (e.g. a new flow joined the bottleneck).
- **Fast Convergence action**: Instead of setting `ca->last_max_cwnd = cwnd`, we intentionally lower `ca->last_max_cwnd` further so that the cubic plateau in the next epoch sits lower, releasing bandwidth more quickly.

</details>

### Q2. To what value should Fast Convergence lower `last_max_cwnd`?

Let the window at the moment of loss be $W$ and the multiplicative decrease factor be $\beta \approx 0.7$ (`beta = 717`, `BICTCP_BETA_SCALE = 1024`). How do we set $W_{\max}$ to the midpoint between the pre-loss window $W$ and the post-loss window $\beta W$ using integer arithmetic?

<details>
<summary>Hints and Answer</summary>

- The midpoint between $W$ ($= 1.0 W$) and $\beta W$ is:
  $$W_{\max} = W \times \frac{1 + \beta}{2} \approx 0.85 W$$
- Using fixed-point integer arithmetic with `BICTCP_BETA_SCALE = 1024` and `beta = 717`:
  ```c
  if (cwnd < ca->last_max_cwnd)
          ca->last_max_cwnd = (cwnd * (BICTCP_BETA_SCALE + beta))
                              / (2 * BICTCP_BETA_SCALE);
  else
          ca->last_max_cwnd = cwnd;
  ```
- For example, if the first loss occurs at `cwnd = 20` (`last_max_cwnd = 20`, `ssthresh = 14`) and a second loss occurs at `cwnd = 14` before reaching 20 again, `14 < 20` triggers Fast Convergence and sets `last_max_cwnd = (14 * 1741) / 2048 = 11`.

</details>

### Q3. Why does CUBIC's emulated Reno window (`ca->tcp_cwnd`) grow by +1 every ~1.875 RTTs instead of +1 every 1 RTT?

Standard TCP Reno reduces `cwnd` by $0.5\times$ on loss and increases it by $+1$ per RTT. CUBIC, however, only reduces `cwnd` by $\beta \approx 0.7\times$ on loss. What would happen to average throughput fairness if CUBIC started at $0.7 W_{\max}$ after loss and still grew by $+1$ every RTT?

<details>
<summary>Hints and Answer</summary>

- Starting from $0.7 W_{\max}$ and adding $+1$ per RTT ($\alpha = 1$) would keep CUBIC's window consistently above TCP Reno's ($\beta_{\text{reno}} = 0.5, \alpha_{\text{reno}} = 1$), making it overly aggressive against Reno flows.
- For an AIMD($\alpha, \beta$) flow to achieve the same average window as TCP Reno, its additive increase slope $\alpha$ must satisfy:
  $$\alpha = \frac{3(1 - \beta)}{1 + \beta}$$
- Substituting $\beta = 717 / 1024 \approx 0.7$ gives $\alpha \approx 0.529$ segments per RTT, so the **number of RTTs needed to increase `tcp_cwnd` by 1** is:
  $$\frac{1}{\alpha} = \frac{1 + \beta}{3(1 - \beta)} \approx 1.889\text{ RTTs}$$
- The kernel precomputes $8 / \alpha$ as `beta_scale`:
  ```c
  beta_scale = 8 * (BICTCP_BETA_SCALE + beta) / 3 / (BICTCP_BETA_SCALE - beta); /* = 15 */
  ```
  and increments `ca->tcp_cwnd` by 1 every `delta = (cwnd * beta_scale) >> 3` ACKed segments ($\approx 1.875 \times \text{cwnd}$ ACKs).

</details>

### Q4. What happens if you copy the `while (ca->ack_cnt > delta)` loop from kernel `tcp_cubic.c` directly into BPF?

In the kernel's `tcp_cubic.c`, `ca->tcp_cwnd` is updated using `while (ca->ack_cnt > delta) { ca->ack_cnt -= delta; ca->tcp_cwnd++; }`. How should we express this in BPF?

<details>
<summary>Hints and Answer</summary>

- **BPF verifier constraint**: An unbounded `while` loop whose trip count depends on runtime values `ca->ack_cnt` and `delta` cannot be statically bounded by the BPF verifier and will be rejected.
- **Replace with division and modulo**: After checking `if (delta)`, compute the update in $O(1)$ using integer division and modulo:
  ```c
  delta = (cwnd * beta_scale) >> 3;
  if (delta) {
          ca->tcp_cwnd += ca->ack_cnt / delta;
          ca->ack_cnt %= delta;
  }
  ```
- Then, if `ca->tcp_cwnd > cwnd` (meaning the emulated Reno window is ahead of `cwnd`), compute `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)` and clamp `ca->cnt` (`if (ca->cnt > max_cnt) ca->cnt = max_cnt`).

</details>

---

## Step-by-step Walkthrough

`solution.bpf.c` adds two pieces of logic on top of Step 3:

### 1. Fast Convergence in `bpf_cubic4_ssthresh()`

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

- When loss occurs, `ca->epoch_start` is reset to `0` to end the current CUBIC epoch.
- If `cwnd < ca->last_max_cwnd`, loss occurred before the flow could recover to its previous maximum window $W_{\max}$. Fast Convergence lowers `ca->last_max_cwnd` to `cwnd * (1024 + 717) / 2048` ($\approx 0.85 \times \text{cwnd}$).
- In the next epoch, both `ca->bic_origin_point` ($= W_{\max}$) and `ca->bic_K` become smaller, causing the flow to plateau at a lower window and yield bandwidth to competing flows.

### 2. Reno-friendly Region (`tcp_friendliness`) in `bictcp_update()`

```c
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
```

- At the top of `bictcp_update()`, `ca->ack_cnt += acked` accumulates the number of ACKed segments (initialised to `ca->ack_cnt = acked` and `ca->tcp_cwnd = cwnd` at the start of each epoch).
- If multiple ACKs arrive within the same jiffy, the cubic curve calculation is skipped via `goto tcp_friendliness;`, updating only `ca->tcp_cwnd`.
- Every `delta = (cwnd * beta_scale) >> 3` ACKed segments, `ca->tcp_cwnd` advances by 1.
- Whenever `ca->tcp_cwnd > cwnd`, `ca->cnt` (which can be as large as `100 * cwnd` near the cubic plateau) is clamped to `max_cnt = cwnd / (ca->tcp_cwnd - cwnd)`, ensuring CUBIC grows at least as fast as TCP Reno.

---

## Understanding `test.pkt`

`test.pkt` verifies both **Fast Convergence** and the **Reno-friendly region** step by step:

1. **Slow Start (`cwnd: 10 -> 20`)**:
   - Sends the initial 10-segment flight (10,000 bytes) and receives an ACK, growing `cwnd` to `20`.
2. **Loss Episode #1 at `cwnd = 20`**:
   - Sends 20 segments and receives 3 SACKs to enter Recovery.
   - Because `ca->last_max_cwnd` was `0`, `ca->last_max_cwnd` becomes `20` and `ssthresh` becomes `(20 * 717) / 1024 = 14`. After Recovery completes, `cwnd == 14`.
3. **Loss Episode #2 at `cwnd = 14 < last_max_cwnd (20)` — Fast Convergence triggers**:
   - Before `cwnd` can grow back to 20, another loss occurs at `cwnd = 14`.
   - `ssthresh` becomes `(14 * 717) / 1024 = 9`.
   - **With Fast Convergence (`solution`)**: Because `14 < 20`, `ca->last_max_cwnd` is lowered to `(14 * 1741) / 2048 = 11`.
   - **Without Fast Convergence (`starter`)**: `ca->last_max_cwnd` is set to `14`.
4. **Congestion Avoidance after 2nd loss (`cwnd = 9`) — Verifying Fast Convergence**:
   - **Flight 1 (epoch start, `t_elapsed = 0.0s`)**: Sends 9 segments at `cwnd = 9`; with `bic_target = 10` (`cnt = 9`), 9 ACKed segments grow `cwnd` from `9` to `10`.
   - **Flight 2 (`t_elapsed = 0.8s`)**: Sends 10 segments at `cwnd = 10`.
     - In `solution` (`last_max_cwnd = 11`), `bic_target` has already reached the plateau at `11`, giving `cnt = 10 / (11 - 10) = 10` so 10 ACKed segments grow `cwnd` to `11`.
     - In `starter` (`last_max_cwnd = 14`), `bic_target` at 0.8 seconds is `13`, giving `cnt = 10 / (13 - 10) = 3`, which causes `cwnd` to jump to `13` (`!= 11`) and fail the assertion.
5. **Reno-friendly Region at the Plateau (`cwnd = 11 == last_max_cwnd`)**:
   - Once `cwnd` reaches `11`, for `t < bic_K` (~1.71 s) the cubic target is flat (`bic_target == 11 == cwnd`), so pure CUBIC sets `ca->cnt = 100 * 11 = 1100`.
   - With the Reno-friendly region enabled, `ca->tcp_cwnd` advances by 1 every `delta = (11 * 15) >> 3 = 20` ACKed segments (`9 -> 10 -> 11 -> 12`). As soon as `ca->tcp_cwnd == 12 > cwnd (11)`, `max_cnt = 11 / (12 - 11) = 11` clamps `ca->cnt` from `1100` down to `11`, growing `cwnd > 11`.

---

## How to run

```sh
# 1. Verify that starter.bpf.c fails on the packetdrill assertion
../../run_test.sh cubic/step04-reno-friendly-fast-conv starter
# 2. Implement the TODOs in starter.bpf.c and re-run (or run the solution)
../../run_test.sh cubic/step04-reno-friendly-fast-conv solution
```
