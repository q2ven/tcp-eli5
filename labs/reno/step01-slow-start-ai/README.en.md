# Step 1: Minimal BPF Congestion Control Skeleton + Slow Start & Reno AI

## 1. Goal

In this step, you will implement the Linux kernel's pluggable congestion control interface (`struct tcp_congestion_ops`) using BPF `struct_ops`, building the foundation of all TCP congestion control algorithms — **TCP Reno (Slow Start, Additive Increase, and Multiplicative Decrease)** — completely from scratch. Rather than calling existing kernel `extern __ksym` helpers such as `tcp_slow_start()` or `tcp_cong_avoid_ai()`, you will write every line in pure BPF so you can see and verify how the congestion window evolves and transitions on every incoming ACK.

- See the [Reno Track README](../README.en.md) ([Japanese](../README.md)) for the overall track structure and step list.

---

## 2. How the Algorithm Works (Read This First)

Before looking at the design checkpoints or code, let's build a complete picture of how TCP Reno works from first principles and what data structures and hooks the Linux kernel already provides.

### 2.1 Why Does TCP Need Congestion Control?

If a sender injects packets into the network without regard for available path capacity, router queues along the path overflow, packet loss cascades, and useful throughput collapses (*congestion collapse*). Yet a sender has no prior knowledge of how much spare capacity exists along an arbitrary Internet path.

To solve this, a TCP sender maintains a self-imposed limit on the amount of unacknowledged (*in-flight*) data it may have in the network at any instant: the **congestion window (`cwnd`, measured in MSS segments)**. The sender dynamically adjusts `cwnd` using returning ACKs as feedback.

### 2.2 The Two Growth Phases of TCP Reno: Slow Start and Congestion Avoidance

TCP Reno (RFC 5681) switches between two distinct window growth regimes separated by the **slow start threshold (`ssthresh`)**:

```
cwnd (segments)
  ^
  |                        /  <- Congestion Avoidance (Additive Increase)
23|....................../..     +1 cwnd per RTT (linear growth)
22|-----------------> *---       Switch growth regime at ssthresh = 22
  |                 /
20|...............*
  |              /            <- Slow Start
  |             /                +1 cwnd per ACKed segment
  |            /                 (doubles every RTT — exponential growth)
10|----------*
  +----------------------------> Time (RTTs)
   RTT 0   RTT 1   RTT 2   RTT 3
```

1. **Slow Start (when `cwnd < ssthresh`)**
   - **Intuition**: At the start of a connection, the sender has no idea how large the path's bandwidth-delay product is. Starting from a conservative initial window (10 segments by default in Linux), it probes capacity rapidly by doubling `cwnd` every round-trip time (RTT).
   - **Formula and rule**: Whenever an incoming ACK acknowledges `acked` new segments of data, the sender increases `cwnd` by that exact amount:
     $$\text{cwnd} \leftarrow \text{cwnd} + \text{acked}$$
   - When an entire flight of `cwnd` segments is sent and acknowledged over one RTT, `cwnd` grows to $\text{cwnd} + \text{cwnd} = 2 \times \text{cwnd}$ (**exponential doubling every RTT**).
   - **Clamping at the `ssthresh` boundary**: Slow Start is only permitted up to `ssthresh`. If a single ACK (such as a stretch ACK acknowledging many segments at once) would push `cwnd + acked` past `ssthresh`, Slow Start consumes only enough `acked` segments to raise `cwnd` to `ssthresh`. **Any leftover `acked` segments are not discarded — they carry directly over into Congestion Avoidance.**

2. **Congestion Avoidance / Reno Additive Increase (when `cwnd >= ssthresh`)**
   - **Intuition**: Once `cwnd` reaches `ssthresh` (which marks either a target threshold or the vicinity of a past packet loss), doubling the window again would quickly overflow router buffers. Instead, the sender switches to **Additive Increase (AI)**: it increases `cwnd` by just **+1 segment per full RTT** of successfully acknowledged data.
   - **Integer-only maths without floating point**: Conceptually, each ACKed segment in Congestion Avoidance increases `cwnd` by $\frac{1}{\text{cwnd}}$ (or $\text{cwnd} \leftarrow \text{cwnd} + \frac{\text{acked}}{\text{cwnd}}$). Because floating-point arithmetic (`float` / `double`) is forbidden in the Linux kernel and BPF programs, TCP represents the fractional credit $\frac{\text{cnt}}{\text{cwnd}}$ using an integer numerator counter **`snd_cwnd_cnt`**:
     1. On each ACK in Congestion Avoidance, add `acked` to the numerator: `snd_cwnd_cnt += acked`.
     2. Whenever the numerator reaches or exceeds the denominator (`snd_cwnd_cnt >= snd_cwnd`), subtract `snd_cwnd` from `snd_cwnd_cnt` (`snd_cwnd_cnt -= snd_cwnd`) and increment `snd_cwnd` by `1`.

### 2.3 Concrete Numerical Trace (`cwnd = 10`, `ssthresh = 22`)

Let's trace the exact numbers tested by `test.pkt` in this step:

| Round | State Before ACK | Incoming Event | Step-by-Step Calculation | State After ACK |
| :--- | :--- | :--- | :--- | :--- |
| **Initial** | — | 3-way handshake completes | `bpf_reno1_init()` sets `snd_cwnd_cnt = 0` and `snd_ssthresh = 22`. | `cwnd = 10`<br>`ssthresh = 22`<br>`cnt = 0` |
| **Round 1**<br>(Pure Slow Start) | `cwnd = 10`<br>`ssthresh = 22`<br>`cnt = 0` | Send 10 segments; receive ACK for `acked = 10` segments | `cwnd < ssthresh`, so run Slow Start.<br>`10 + 10 = 20 <= 22`: all 10 ACKed segments are consumed in Slow Start (`acked` becomes `0`). | `cwnd = 20`<br>`ssthresh = 22`<br>`cnt = 0` |
| **Round 2**<br>(Boundary Crossing) | `cwnd = 20`<br>`ssthresh = 22`<br>`cnt = 0` | Send 20 segments; receive ACK for `acked = 20` segments | 1. Slow Start consumes `22 - 20 = 2` segments to reach `ssthresh` (`cwnd = 22`).<br>2. The leftover `acked = 20 - 2 = 18` segments fall through to Congestion Avoidance: `cnt = 0 + 18 = 18`.<br>3. Since `cnt (18) < cwnd (22)`, `cwnd` stays `22`. | `cwnd = 22`<br>`ssthresh = 22`<br>`cnt = 18` |
| **Round 3**<br>(Reno AI) | `cwnd = 22`<br>`ssthresh = 22`<br>`cnt = 18` | Send 22 segments; receive ACK for `acked = 22` segments | `cwnd >= ssthresh`, so run Congestion Avoidance directly.<br>1. `cnt = 18 + 22 = 40`.<br>2. Since `cnt (40) >= cwnd (22)`, subtract `22` (`cnt = 18`) and increment `cwnd` from `22` to `23`. | `cwnd = 23`<br>`ssthresh = 22`<br>`cnt = 18` |

### 2.4 Cwnd Validation (Guarding Against Application-Limited Growth)

What happens if `cwnd = 10`, but the application only writes 1 segment per RTT?
Because the sender never actually puts 10 segments in flight to test whether the network path can sustain them, growing `cwnd` to 11, 12, ... on every ACK would build up an artificially huge `cwnd`. If the application later writes a large burst of data, TCP would dump an untested flood of packets into the network all at once (see RFC 7661, Congestion Window Validation).

Therefore, at the very beginning of `.cong_avoid()`, a congestion control algorithm must check whether transmission is genuinely limited by `cwnd` (i.e. the sender is actually filling the congestion window). If not, `.cong_avoid()` must return immediately without growing `cwnd`.

### 2.5 Multiplicative Decrease on Loss and Undoing Spurious Reductions

When packet loss is detected — or when a loss detection later turns out to be a false alarm — TCP Reno applies two rules:

- **Multiplicative Decrease (`.ssthresh()`)**: Packet loss signals that the current `cwnd` exceeded path capacity. Reno cuts `ssthresh` to half of the current `cwnd` ($\lfloor \text{cwnd} / 2 \rfloor$, written as `snd_cwnd >> 1U`), floored at a minimum of `2U` segments as mandated by RFC 5681 so transmission never stalls:
  $$\text{ssthresh} = \max(\lfloor \text{cwnd} / 2 \rfloor, 2)$$
- **Undoing Spurious Window Reductions (`.undo_cwnd()`)**: If the kernel reduces `cwnd` on loss recovery entry, but subsequently discovers via DSACK or timestamps that no packet was actually lost (e.g. packets were merely reordered), it calls `.undo_cwnd()` to restore `snd_cwnd` to the larger of the current `snd_cwnd` and the pre-loss `prior_cwnd`: $\max(\text{snd\_cwnd}, \text{prior\_cwnd})$.

### 2.6 What the Kernel Already Provides (Structs, Fields, and Helpers)

To design a BPF congestion control algorithm, you need to know which fields already live on the kernel's socket structures and which helpers are available.

#### (A) Core `struct tcp_congestion_ops` Hooks Implemented in This Step

| Hook | Signature | When It Is Called & What It Does |
| :--- | :--- | :--- |
| `.init` | `void (*init)(struct sock *sk)` | Called when a connection is established (or switched to this CC via `setsockopt`). Initialises per-socket CC state. |
| `.ssthresh` | `__u32 (*ssthresh)(struct sock *sk)` | Called upon entering loss recovery to compute and return the new `snd_ssthresh`. |
| `.cong_avoid` | `void (*cong_avoid)(struct sock *sk, __u32 ack, __u32 acked)` | Called on every incoming ACK that acknowledges new data. `ack` is the ACK sequence number; `acked` is the number of newly acknowledged segments. |
| `.undo_cwnd` | `__u32 (*undo_cwnd)(struct sock *sk)` | Called when loss recovery is detected to be spurious; returns the restored `snd_cwnd` value. |

#### (B) Standard Fields Already Built into `struct tcp_sock` (`struct tcp_sock *tp = tcp_sk(sk)`)

Every Linux TCP socket (`struct tcp_sock`) already has **all of Reno's state variables built right into the core socket struct**:

- `tp->snd_cwnd` (`__u32`): Current congestion window (in segments).
- `tp->snd_ssthresh` (`__u32`): Current slow start threshold (in segments).
- `tp->snd_cwnd_cnt` (`__u32`): Linear Congestion Avoidance counter that accumulates ACKed segments (the numerator of $\frac{\text{cnt}}{\text{cwnd}}$).
- `tp->snd_cwnd_clamp` (`__u32`): Maximum allowed value for `tp->snd_cwnd` (always clamp increases using `min(new_cwnd, tp->snd_cwnd_clamp)`).
- `tp->prior_cwnd` (`__u32`): Value of `snd_cwnd` automatically saved by the kernel right before entering loss recovery.

#### (C) What Is the Per-Socket Private Area `icsk_ca_priv`?

- `struct inet_connection_sock` (the parent struct of `tcp_sock`) reserves a 104-byte scratch buffer (`icsk_ca_priv`), accessed via `inet_csk_ca(sk)`.
- Complex congestion control algorithms such as CUBIC (which tracks cubic epoch start timestamps, `W_max`, and `bic_K`) or BBR (which tracks windowed max-bandwidth filters and state machines) define a custom C struct inside `icsk_ca_priv` because `struct tcp_sock` does not have dedicated fields for those algorithm-specific variables.
- By contrast, **every variable required by TCP Reno (`snd_cwnd`, `snd_ssthresh`, `snd_cwnd_cnt`, and `prior_cwnd`) already exists directly on `struct tcp_sock`**.

#### (D) Helper Functions Provided in `bpf_cc_helpers.h`

- `tcp_sk(sk)`: Casts `struct sock *sk` to `struct tcp_sock *tp`.
- `tcp_is_cwnd_limited(sk)`: Returns `true` if transmission is currently limited by `snd_cwnd` (the pipe is full), or `false` if the socket is application-limited.
- `tcp_in_slow_start(tp)`: Returns `true` when `tp->snd_cwnd < tp->snd_ssthresh`.
- `min(a, b)` / `max(a, b)`: Returns the minimum or maximum of two values.

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Using what you just learned in Section 2, work through these four design questions to plan your implementation of the TODOs in `starter.bpf.c`. Every question can be answered directly from Section 2.

### Q1. State Design: Does TCP Reno need a custom private struct in `icsk_ca_priv`?

Review the standard `struct tcp_sock` fields in Section 2.6 (B) and the role of `icsk_ca_priv` in Section 2.6 (C). Do we need to define a custom struct for `inet_csk_ca(sk)` to implement Reno's Slow Start, Additive Increase, Multiplicative Decrease, and Undo?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Check whether every variable appearing in the formulas in Sections 2.2–2.5 already exists on `struct tcp_sock`.
- **Answer**: **No custom `icsk_ca_priv` struct is needed.**
  `struct tcp_sock` already provides every field TCP Reno requires:
  - `tp->snd_cwnd`: current congestion window
  - `tp->snd_ssthresh`: threshold separating Slow Start and Congestion Avoidance
  - `tp->snd_cwnd_cnt`: integer numerator counter accumulating `acked` segments during Additive Increase
  - `tp->snd_cwnd_clamp`: upper bound used to clamp `tp->snd_cwnd`
  - `tp->prior_cwnd`: saved `snd_cwnd` from before loss recovery, used by `.undo_cwnd()`

</details>

### Q2. Guard Condition: What must `bpf_reno1_cong_avoid()` check first before growing `snd_cwnd`?

Based on Section 2.4 (Cwnd Validation) and the helpers in Section 2.6 (D), what check should appear at the very top of `bpf_reno1_cong_avoid(sk, ack, acked)` before any Slow Start or Additive Increase logic runs?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Which helper tells us whether the sender is actually filling `snd_cwnd` rather than being application-limited?
- **Answer**: Check **`if (!tcp_is_cwnd_limited(sk)) return;`** at the start of `bpf_reno1_cong_avoid()` (**TODO 2** in `starter.bpf.c`). This ensures `snd_cwnd` only grows when the connection is genuinely cwnd-limited.

</details>

### Q3. Boundary Condition: How should the C code handle an ACK that crosses `tp->snd_ssthresh` during Slow Start?

Translate Round 2 from Section 2.3 (`tp->snd_cwnd = 20`, `tp->snd_ssthresh = 22`, and an incoming ACK with `acked = 20`) into C code inside `if (tcp_in_slow_start(tp))`:
1. How do you compute the new Slow Start window `cwnd` without exceeding `tp->snd_ssthresh`?
2. How do you subtract the segments consumed by Slow Start from `acked`?
3. When should the function return early, and when should it fall through to Congestion Avoidance?

<details>
<summary>Hint and Answer</summary>

- **Hint**: If `cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh)`, then the number of `acked` segments consumed by Slow Start is `cwnd - tp->snd_cwnd`.
- **Answer**: You can express the entire Slow Start phase and boundary handoff in four lines (**TODO 3** in `starter.bpf.c`):
  ```c
  if (tcp_in_slow_start(tp)) {
      __u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

      acked -= cwnd - tp->snd_cwnd;
      tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
      if (!acked)
          return;
  }
  ```
  - In Round 1 (`cwnd = 10, ssthresh = 22, acked = 10`), `cwnd = min(20, 22) = 20`, `acked -= 20 - 10` leaves `acked == 0`, and `if (!acked) return;` exits early.
  - In Round 2 (`cwnd = 20, ssthresh = 22, acked = 20`), `cwnd = min(40, 22) = 22`, `acked -= 22 - 20` leaves `acked == 18`, and execution falls straight through to Congestion Avoidance with `acked = 18`.

</details>

### Q4. Integer-Only Additive Increase: How do we update `tp->snd_cwnd_cnt` and `tp->snd_cwnd` with the remaining `acked` segments?

Translate the integer Congestion Avoidance rule from Section 2.2 into C code (**TODO 4** in `starter.bpf.c`).

<details>
<summary>Hint and Answer</summary>

- **Hint**: Add `acked` to the numerator `tp->snd_cwnd_cnt`. If `tp->snd_cwnd_cnt >= tp->snd_cwnd`, subtract `tp->snd_cwnd` and increment `tp->snd_cwnd` by `1` (clamped at `tp->snd_cwnd_clamp`).
- **Answer**:
  ```c
  tp->snd_cwnd_cnt += acked;
  if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
      tp->snd_cwnd_cnt -= tp->snd_cwnd;
      tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
  }
  ```
  Because this uses a single `if` statement rather than a loop, the BPF verifier can verify termination and safety immediately.

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

Let's walk through each function in [`solution.bpf.c`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/reno/step01-slow-start-ai/solution.bpf.c).

### 4.1 `bpf_reno1_init()` — Initialising the Socket

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->snd_cwnd_cnt = 0;
	tp->snd_ssthresh = 22;
}
```

- Called when the TCP connection is established (or switched to `bpf_reno1` via `setsockopt(TCP_CONGESTION)`), resetting the Additive Increase counter `tp->snd_cwnd_cnt` to `0`.
- Although Linux normally starts with an infinite `snd_ssthresh` (`TCP_INFINITE_SSTHRESH`), we initialise `tp->snd_ssthresh = 22` here so that `test.pkt` can deterministically test pure Slow Start (`10 -> 20`), the exact `snd_ssthresh` boundary transition with leftover `acked` credits (`20 -> 22`), and Reno Additive Increase (`22 -> 23`) in three consecutive RTTs without needing to induce packet loss first.

### 4.2 `bpf_reno1_ssthresh()` — Multiplicative Decrease Threshold (TODO 1)

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}
```

- Called upon entering loss recovery to calculate the new `snd_ssthresh`.
- Halves `tp->snd_cwnd` via a right bit-shift (`tp->snd_cwnd >> 1U`) and enforces the RFC 5681 minimum of `2U` segments using `max(..., 2U)`.

### 4.3 `bpf_reno1_cong_avoid()` — Slow Start and Reno Additive Increase (TODOs 2–4)

```c
SEC("struct_ops")
void BPF_PROG(bpf_reno1_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		__u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

		acked -= cwnd - tp->snd_cwnd;
		tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
		if (!acked)
			return;
	}

	tp->snd_cwnd_cnt += acked;
	if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
		tp->snd_cwnd_cnt -= tp->snd_cwnd;
		tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
	}
}
```

- **TODO 2 (Cwnd validation)**: Returns early if `!tcp_is_cwnd_limited(sk)`.
- **TODO 3 (Slow Start and boundary handoff)**: While `tcp_in_slow_start(tp)` is true, grows `tp->snd_cwnd` up to `tp->snd_ssthresh`, subtracts the consumed growth (`cwnd - tp->snd_cwnd`) from `acked`, and returns early if `acked` reaches `0`. Any leftover `acked` segments fall through to Congestion Avoidance.
- **TODO 4 (Reno Additive Increase)**: Accumulates `acked` in `tp->snd_cwnd_cnt`. Once `tp->snd_cwnd_cnt >= tp->snd_cwnd`, subtracts `tp->snd_cwnd` and increments `tp->snd_cwnd` by `1` (clamped at `tp->snd_cwnd_clamp`).

### 4.4 `bpf_reno1_undo_cwnd()` — Undoing a Spurious Window Reduction (TODO 5)

```c
SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_undo_cwnd, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}
```

- Restores `snd_cwnd` to `max(tp->snd_cwnd, tp->prior_cwnd)` when the kernel detects that a loss recovery episode was spurious (we will test this path with packet losses in Step 2).

---

## 5. Understanding `test.pkt` and How to Run

### 5.1 Walkthrough of [`test.pkt`](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/reno/step01-slow-start-ai/test.pkt)

`test.pkt` drives `bpf_reno1` through the exact three-round sequence traced in Section 2.3 and verifies `tcpi_snd_cwnd` after each ACK using Python assertions:

1. **Connection Establishment with a 1000-byte Effective MSS (lines 8–22)**:
   - The client SYN advertises `<mss 1012,sackOK,TS val 100 ecr 0,nop,wscale 7>`.
   - Because 12-byte TCP Timestamps (`nop,nop,TS`) are enabled, the effective payload size per segment (`tp->mss_cache`) is `1012 - 12 = 1000` bytes (`10,000` bytes = `10` segments).
   - Right after `accept()`, `test.pkt` asserts `tcpi_snd_cwnd == 10` and `tcpi_snd_ssthresh == 22`.
2. **Round 1 — Pure Slow Start (lines 24–31, `cwnd: 10 -> 20`)**:
   - `write(4, ..., 10000)` sends 10 segments (`1:10001`), filling `cwnd = 10` so `tp->is_cwnd_limited` becomes `true`.
   - Receiving `ack 10001` (`acked = 10`) doubles `tcpi_snd_cwnd` from `10` to `20`.
3. **Round 2 — Clamping at `snd_ssthresh` & Carrying Leftover Credits (lines 33–43, `cwnd: 20 -> 22`)**:
   - `write(4, ..., 20000)` sends 20 segments (`10001:30001`), filling `cwnd = 20`.
   - Receiving `ack 30001` (`acked = 20`) uses `2` segments in Slow Start to reach `tcpi_snd_cwnd == 22` and carries the remaining `18` ACKed segments into `tp->snd_cwnd_cnt = 18`.
4. **Round 3 — Reno Additive Increase (lines 45–53, `cwnd: 22 -> 23`)**:
   - `write(4, ..., 22000)` sends 22 segments (`30001:52001`), filling `cwnd = 22`.
   - Receiving `ack 52001` (`acked = 22`) raises `tp->snd_cwnd_cnt` to `18 + 22 = 40 >= 22`, incrementing `tcpi_snd_cwnd` from `22` to `23`.

### 5.2 Why `starter.bpf.c` Fails and `solution.bpf.c` Passes

- **Why `starter.bpf.c` fails**: In `starter.bpf.c`, `bpf_reno1_cong_avoid()` is a no-op stub. When `ack 10001` arrives in Round 1, `tp->snd_cwnd` remains stuck at its initial value of `10`, triggering the assertion failure on line 30 (`expected cwnd 20 after Slow Start, got 10`).
- **Why `solution.bpf.c` passes**: With Slow Start, `snd_ssthresh` boundary credit handoff, and Reno Additive Increase implemented, `tcpi_snd_cwnd` reaches `20` in Round 1, `22` in Round 2, and `23` in Round 3, satisfying all assertions.

### 5.3 How to Run the Test

Run the following commands to see `starter` fail and `solution` (or your completed `starter`) pass:

```sh
../../run_test.sh reno/step01-slow-start-ai starter
../../run_test.sh reno/step01-slow-start-ai solution
```
