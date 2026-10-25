# Step 3: Idle Epoch Shift (`CA_EVENT_TX_START`)

## 1. Goal

By Step 2, our CUBIC implementation has the full cubic curve, Fast Convergence, and the Reno-friendly region in Congestion Avoidance. However, one critical real-world pitfall remains: **when an application pauses sending (goes idle) and later resumes, `snd_cwnd` can surge in a single RTT and trigger a severe packet burst**.

In this step, you will learn from first principles why wall-clock-driven CUBIC is vulnerable to idle periods, and implement the `.cwnd_event_tx_start` hook (`CA_EVENT_TX_START` event) in `struct tcp_congestion_ops` to shift the epoch start timestamp `ca->epoch_start` forward by the idle duration whenever transmission resumes from zero packets in flight (`in_flight == 0`).

- For the full track overview and step list, see the [CUBIC Track README](../README.md).

---

## 2. How the Algorithm Works (Read This First)

### 2.1 Why Idle Periods Cause `snd_cwnd` Spikes in CUBIC

Classical TCP Reno (AIMD) advances `snd_cwnd` purely by counting arriving ACKs. When an application stops writing data and the connection goes idle (zero unacknowledged packets in the network, `in_flight == 0`), no ACKs arrive and Reno's congestion control clock naturally pauses.

CUBIC, by contrast, computes its target window `bic_target` ($$W_{\text{cubic}}(t)$$) in Congestion Avoidance as a cubic function of the **wall-clock time $$t$$ elapsed since the current epoch began**:

$$W_{\text{cubic}}(t) = C (t - K)^3 + W_{\max}, \quad t = t_{\text{now}} - t_{\\text{epoch}} + d_{\min}$$

What happens if a flow in Congestion Avoidance sits idle for 2 seconds and then resumes sending?

If `ca->epoch_start` is left untouched at its pre-idle timestamp, then on the very first ACK after transmission resumes, the elapsed time $$t = \text{jiffies}_{32} - \text{ca}\to\text{epoch}_{\text{start}}$$ **jumps forward by the entire 2-second idle gap at once**. As a result, $$(t - K)^3$$ explodes, `bic_target` leaps upwards, and `snd_cwnd` surges by up to $$1.5\times$$ (`ca->cnt = 2`) in a single RTT right after idle — even though the flow did not probe the network at all during those 2 seconds! This is why [RFC 9438 Section 5.8](https://www.rfc-editor.org/rfc/rfc9438#section-5.8) specifies that the elapsed time $$t$$ MUST NOT include periods during which `cwnd` was not updated due to application-limited (idle) behaviour.

### 2.2 The Solution: Horizontal Time Shift (`+= delta`), Not Epoch Reset (`= 0`)

To exclude the idle duration $$\Delta_{\text{idle}}$$ from the elapsed time $$t$$, we shift `ca->epoch_start` forward by $$\Delta_{\text{idle}}$$ at the exact moment transmission resumes:

$$t_{\\text{epoch}} \leftarrow t_{\\text{epoch}} + \Delta_{\text{idle}}, \quad \text{where } \Delta_{\text{idle}} = t_{\text{now}} - t_{\\text{last}}$$

When the next ACK arrives, the idle gap cancels out of the elapsed time calculation:

$$t_{\text{after}} = t_{\text{now}} - (t_{\\text{epoch}} + \Delta_{\text{idle}}) = (t_{\\text{last}} + \Delta_{\text{idle}}) - t_{\\text{epoch}} - \Delta_{\text{idle}} = t_{\\text{last}} - t_{\\text{epoch}}$$

```text
  cwnd
   ^
20 |               ........---'''      ........---'''
   |            .-'                 .-'
15 |         .-'  <-- 2.0s idle -->'  (resumes at the exact same point & slope)
14 |       .'     [shift epoch_start by +2.0s]
   +-------+---------------+---------------+-------------> time (jiffies)
       epoch_start     lsndtime          now (resume)
```

Notice why we **shift `ca->epoch_start += delta` instead of resetting `ca->epoch_start = 0`**:
CUBIC's concave region ($$t < K$$) is steepest right at the start of an epoch and flattens out as `cwnd` approaches the $$W_{\max}$$ plateau. If we reset `ca->epoch_start = 0` on every idle pause, `bictcp_update()` would recompute `ca->bic_K` from the current `cwnd` on the next ACK and start a brand-new epoch from scratch. A flow that had already reached the flat plateau near $$W_{\max}$$ would repeatedly restart the steep initial climb as if it had just exited recovery. Shifting `ca->epoch_start += delta` translates the cubic curve horizontally in time, **preserving `ca->bic_K`, `ca->bic_origin_point`, and the flow's exact position on the curve** while pausing the clock during the idle gap.

### 2.3 Edge Case 1: When No Epoch Has Started Yet (`ca->epoch_start == 0`)

In CUBIC, `ca->epoch_start == 0` serves as a **sentinel value** meaning "no Congestion Avoidance epoch is currently active" (for example, while the flow is still in Slow Start, or right after packet loss before the first Congestion Avoidance ACK arrives).
If we added `delta` when `ca->epoch_start == 0`, we would corrupt the `0` sentinel into a non-zero timestamp, causing the next call to `bictcp_update()` to skip initialising `ca->bic_K` and `ca->bic_origin_point`. Therefore, we must only shift `ca->epoch_start` when **`ca->epoch_start != 0` and `delta > 0`**.

### 2.4 Edge Case 2: How `ca->epoch_start + delta` Can End Up in the Future (`> now`)

At first glance, since `delta = now - tp->lsndtime`, it might seem impossible for `ca->epoch_start + delta` to exceed `now`. However, because the two timestamps are recorded on different events, **`ca->epoch_start + delta > now` (a timestamp in the future) can genuinely happen**:

- `tp->lsndtime` is recorded when a **data packet is transmitted**.
- `ca->epoch_start` is recorded when an **ACK arrives** and `bictcp_update()` runs.

Consider the following sequence of events:

1. At time $$T_0$$, the sender transmits its last data segment before Fast Recovery finishes (`tp->lsndtime` $$= T_0$$).
2. At time $$T_1$$ ($$T_1 > T_0$$, e.g. one RTT later), a cumulative ACK arrives that completes recovery, and `bictcp_update()` runs on that ACK (or a subsequent ACK), initialising `ca->epoch_start` $$= T_1$$. Because the application has no new data to send at $$T_1$$, no packet is transmitted and `tp->lsndtime` remains $$T_0$$.
3. The connection sits idle until time $$T_2$$ (`now` $$= T_2$$), when the application writes new data.

At $$T_2$$, the idle duration since the last send is $$\Delta_{\text{idle}} = \text{now} - \text{lsndtime} = T_2 - T_0$$. Adding $$\Delta_{\text{idle}}$$ to `ca->epoch_start` gives:

$$\text{ca}\to\text{epoch}_{\text{start}} + \Delta_{\text{idle}} = T_1 + (T_2 - T_0) = T_2 + (T_1 - T_0) > T_2 \ (\text{now})$$

`ca->epoch_start` has moved into the **future** by $$T_1 - T_0$$ beyond `now` ($$T_2$$)!

If an ACK arrives in the same jiffy ($$T_2$$) while `ca->epoch_start > tcp_jiffies32`, line 174 of `bictcp_update()` suffers a catastrophic underflow:

```c
t = (__s32)(tcp_jiffies32 - ca->epoch_start) * USEC_PER_JIFFY;
```

Because `tcp_jiffies32 < ca->epoch_start`, `(__s32)(tcp_jiffies32 - ca->epoch_start)` evaluates to a **negative signed integer**. Multiplying by `USEC_PER_JIFFY` and assigning to `__u64 t` sign-extends and wraps that negative value into a massive unsigned number near $$2^{64}$$, blowing up `offs^3` and `bic_target`.

To prevent `ca->epoch_start` from ever moving into the future, we must clamp it right after adding `delta`: if `after(ca->epoch_start, now)` is true, set `ca->epoch_start = now`.

### 2.5 Concrete Numerical Example (Matching `test.pkt`)

Suppose a flow with RTT $$= 20\text{ ms}$$ ($$d_{\min} = 20{,}000\ \mu\text{s}$$) experiences loss at `cwnd = 20`, setting `last_max_cwnd` ($$W_{\max}$$) $$= 20$$ and `ssthresh` $$= 14$$ ($$K \approx 2.46\text{ s}$$):

- Upon exiting recovery into Congestion Avoidance, `cwnd` grows from $$14 \to 15$$, and after sending and ACKing one RTT ($$20\text{ ms}$$) of data, the elapsed time is $$\approx 40\text{ ms}$$ and `cwnd = 15`.
- The connection now sits **idle for $$2.0\text{ s}$$**, then sends 15 segments and receives an ACK $$20\text{ ms}$$ later:
  - **With idle epoch shift (correct)**: `ca->epoch_start` is shifted forward by $$2.0\text{ s}$$ when transmission resumes. When the ACK arrives $$20\text{ ms}$$ later, the effective elapsed time $$t$$ is only $$\approx 60\text{ ms} + d_{\min}$$. Because $$t \ll K$$, `bic_target` stays at $$15$$ and `cwnd` **remains $$15$$**.
  - **Without idle epoch shift (bug)**: `ca->epoch_start` is not shifted, so $$t$$ jumps by $$2.0\text{ s}$$ to $$\approx 2.06\text{ s}$$ on the first ACK after idle. Because $$2.06\text{ s}$$ is close to $$K = 2.46\text{ s}$$, `bic_target` leaps to $$W_{\max} = 20$$, `ca->cnt` drops to $$15 / (20 - 15) = 3$$, and `cwnd` **spikes from $$15$$ to $$20$$** in a single ACK!

### 2.6 What the Kernel and `bpf_cc_helpers.h` Provide for This Step

Before tackling the design checkpoints in Section 3, here is every kernel callback, struct field, and helper macro you will use in this step (note that the code uses the `bpf_cubic5` identifier prefix for historical reasons):

| Item | Type / Signature | Role and Key Behaviour |
| :--- | :--- | :--- |
| `.cwnd_event_tx_start` | `void (*)(struct sock *sk)` (`struct tcp_congestion_ops` member) | Dedicated callback invoked by `tcp_event_data_sent()` in the TCP transmit path (via `CA_EVENT_TX_START`) at the exact moment a data packet is transmitted while **zero packets are currently in flight (`tcp_packets_in_flight(tp) == 0`)**. |
| `tcp_sk(sk)->lsndtime` | `__u32` (`struct tcp_sock` member) | Jiffies timestamp of the last transmitted data packet. **Crucial ordering detail**: inside `tcp_event_data_sent()`, the kernel runs `if (tcp_packets_in_flight(tp) == 0) tcp_ca_event(sk, CA_EVENT_TX_START);` **immediately before** updating `tp->lsndtime = now;`. Therefore, while `.cwnd_event_tx_start()` is running, `tp->lsndtime` still holds the timestamp of the **last packet sent before the connection went idle**. |
| `tcp_jiffies32` | `((__u32)bpf_jiffies64())` | Current 32-bit kernel jiffies timestamp (`now`), in the same 32-bit jiffies units as `tp->lsndtime` and `ca->epoch_start`. |
| `after(seq2, seq1)` | `bool` (`bpf_cc_helpers.h`) | Wrap-around-safe 32-bit comparison macro (`(__s32)(seq1 - seq2) < 0`). Returns `true` when `seq2` is strictly after `seq1`. `after(ca->epoch_start, now)` tests whether `ca->epoch_start` is in the future relative to `now`. |
| `inet_csk_ca(sk)` | `struct bpf_bictcp *` (`icsk_ca_priv`) | Pointer to the per-socket private congestion control state (`icsk_ca_priv` in `struct inet_connection_sock`), which stores `ca->epoch_start` (`__u32`, `0` when no epoch is active). |

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Using what you just learned in Section 2, answer these four design questions before editing `starter.bpf.c`.

### Q1. Choosing the Hook for Resuming Transmission After Idle

Which callback in `struct tcp_congestion_ops` is invoked at the exact moment a connection transmits a new data packet after sitting idle with zero packets in flight (`in_flight == 0`)?

<details>
<summary>Hint and Answer</summary>

As listed in Section 2.6, we use the **`.cwnd_event_tx_start`** callback in `struct tcp_congestion_ops`.
In the TCP transmit path, `tcp_event_data_sent()` checks `if (tcp_packets_in_flight(tp) == 0)` and dispatches `CA_EVENT_TX_START` to `.cwnd_event_tx_start(sk)`. In `starter.bpf.c`, `bpf_cubic5_cwnd_event_tx_start()` is already wired to `.cwnd_event_tx_start`, so we implement our logic inside `bpf_cubic5_cwnd_event_tx_start()`.

</details>

### Q2. Computing the Idle Duration `delta` Without Adding New State Fields

How can we compute the idle duration `delta` (in jiffies) using an existing field of `struct tcp_sock` without adding any new timestamp variable to `struct bpf_bictcp`? Why does that field still contain the pre-idle timestamp when `.cwnd_event_tx_start` executes?

<details>
<summary>Hint and Answer</summary>

We use `tcp_sk(sk)->lsndtime`.
As explained in Section 2.6, `tcp_event_data_sent()` executes in this order:

```c
if (tcp_packets_in_flight(tp) == 0)
	tcp_ca_event(sk, CA_EVENT_TX_START);

tp->lsndtime = now;
```

Because `CA_EVENT_TX_START` fires **right before** `tp->lsndtime = now` overwrites the timestamp, `tcp_sk(sk)->lsndtime` inside `.cwnd_event_tx_start(sk)` still holds the jiffies timestamp of the last data packet sent before the connection went idle. Subtracting it from `now = tcp_jiffies32`:

```c
delta = now - tcp_sk(sk)->lsndtime;
```

gives the exact idle duration in jiffies as a signed 32-bit integer (`__s32`).

</details>

### Q3. Why We Check `if (ca->epoch_start && delta > 0)` and Shift (`+= delta`) Instead of Resetting (`= 0`)

When updating `ca->epoch_start` with `delta`: (1) why do we shift `ca->epoch_start += delta` instead of resetting `ca->epoch_start = 0`, and (2) why must we guard the update with `if (ca->epoch_start && delta > 0)`?

<details>
<summary>Hint and Answer</summary>

1. **Why we shift instead of resetting (Section 2.2)**:
   Resetting `ca->epoch_start = 0` would force `bictcp_update()` on the next ACK to recompute `ca->bic_K` and start a brand-new epoch from scratch. Because CUBIC's concave curve is steepest right at the start of an epoch and flattens near $$W_{\max}$$, resetting `epoch_start = 0` on every idle pause would cause a flow near the $$W_{\max}$$ plateau to repeatedly restart the aggressive initial climb. Shifting `ca->epoch_start += delta` translates the curve horizontally in time, preserving `ca->bic_K`, `ca->bic_origin_point`, and the flow's position on the curve.
2. **Why we check `if (ca->epoch_start && delta > 0)` (Section 2.3)**:
   `ca->epoch_start == 0` is the sentinel indicating that no Congestion Avoidance epoch is active yet (e.g. during Slow Start or right after packet loss). Adding `delta` when `ca->epoch_start == 0` would corrupt the `0` sentinel into a non-zero timestamp and skip epoch initialisation in `bictcp_update()`. Checking `delta > 0` ensures at least 1 jiffy of idle time has elapsed.

</details>

### Q4. Clamping `ca->epoch_start` So It Never Moves into the Future

Right after `ca->epoch_start += delta;`, why do we need `if (after(ca->epoch_start, now)) ca->epoch_start = now;`? What breaks in `bictcp_update()` if `ca->epoch_start` is allowed to exceed `now`?

<details>
<summary>Hint and Answer</summary>

As explained in Section 2.4, `tp->lsndtime` is recorded on **packet transmission** ($$T_0$$), whereas `ca->epoch_start` is recorded on **ACK arrival** ($$T_1 > T_0$$). If the last packet before recovery completes was sent at $$T_0$$, the epoch started on an ACK at $$T_1 > T_0$$ without sending new data, and the flow then sat idle until $$T_2$$ (`now`), then:

$$\text{ca}\to\text{epoch}_{\text{start}} + \text{delta} = T_1 + (T_2 - T_0) = T_2 + (T_1 - T_0) > \text{now}$$

placing `ca->epoch_start` in the future relative to `now`!
If an ACK arrives while `ca->epoch_start > tcp_jiffies32`, then `(__s32)(tcp_jiffies32 - ca->epoch_start)` in `bictcp_update()` evaluates to a negative number, which sign-extends and wraps to a massive unsigned value near $$2^{64}$$ when assigned to `__u64 t`, corrupting the cubic calculation. Clamping with `if (after(ca->epoch_start, now)) ca->epoch_start = now;` prevents `ca->epoch_start` from ever exceeding `now`.

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

All functions from Step 2 (`bictcp_update()`, `bpf_cubic5_cong_avoid()`, `bpf_cubic5_recalc_ssthresh()`, `bpf_cubic5_state()`, `bpf_cubic5_acked()`, and `bpf_cubic5_undo_cwnd()`) carry over unchanged. We implement `bpf_cubic5_cwnd_event_tx_start()` and wire it into `struct tcp_congestion_ops bpf_cubic5`.

### 4.1 Implementing `bpf_cubic5_cwnd_event_tx_start()`

```c
SEC("struct_ops")
void BPF_PROG(bpf_cubic5_cwnd_event_tx_start, struct sock *sk)
{
	struct bpf_bictcp *ca = inet_csk_ca(sk);
	__u32 now = tcp_jiffies32;
	__s32 delta;

	delta = now - tcp_sk(sk)->lsndtime;

	/* We were application limited (idle) for a while.
	 * Shift epoch_start to keep cwnd growth to cubic curve.
	 */
	if (ca->epoch_start && delta > 0) {
		ca->epoch_start += delta;
		if (after(ca->epoch_start, now))
			ca->epoch_start = now;
	}
}
```

1. **Computing the idle duration**:
   - `now = tcp_jiffies32` reads the lower 32 bits of `bpf_jiffies64()`.
   - `delta = now - tcp_sk(sk)->lsndtime` computes the signed 32-bit jiffies elapsed since the last data packet was transmitted before going idle.
2. **Shifting the epoch start timestamp**:
   - `if (ca->epoch_start && delta > 0)` ensures that an epoch is already active (`ca->epoch_start != 0`) and that a positive idle gap elapsed before shifting `ca->epoch_start += delta;`.
3. **Clamping to `now`**:
   - `if (after(ca->epoch_start, now)) ca->epoch_start = now;` uses `after(seq2, seq1)` from `bpf_cc_helpers.h` to guarantee `ca->epoch_start` never exceeds `now`.

### 4.2 How the Shift Feeds into `bictcp_update()`

Because `bpf_cubic5_cwnd_event_tx_start()` advances `ca->epoch_start` by the idle duration when transmission resumes, the fixed-point elapsed-time calculation in lines 174–177 of `bictcp_update()` works seamlessly on subsequent ACKs without any changes:

```c
	t = (__s32)(tcp_jiffies32 - ca->epoch_start) * USEC_PER_JIFFY;
	t += ca->delay_min;
	t <<= BICTCP_HZ;
	t /= USEC_PER_SEC;
```

- `tcp_jiffies32 - ca->epoch_start` now measures only the active (non-idle) time elapsed in the epoch, keeping `t` (in units of $$1/1024\text{ s}$$) aligned with the cubic curve prior to the idle pause.

### 4.3 Registering `.cwnd_event_tx_start` in `struct tcp_congestion_ops bpf_cubic5`

```c
SEC(".struct_ops")
struct tcp_congestion_ops bpf_cubic5 = {
	.init			= (void *)bpf_cubic5_init,
	.ssthresh		= (void *)bpf_cubic5_recalc_ssthresh,
	.cong_avoid		= (void *)bpf_cubic5_cong_avoid,
	.set_state		= (void *)bpf_cubic5_state,
	.undo_cwnd		= (void *)bpf_cubic5_undo_cwnd,
	.cwnd_event_tx_start	= (void *)bpf_cubic5_cwnd_event_tx_start,
	.pkts_acked		= (void *)bpf_cubic5_acked,
	.flags			= TCP_CONG_NON_RESTRICTED,
	.name			= "bpf_cubic5",
};
```

---

## 5. Understanding `test.pkt` and How to Run

### 5.1 Walkthrough of `test.pkt`

`test.pkt` tests the idle epoch shift across five stages on a connection with a $$20\text{ ms}$$ RTT:

1. **Connection establishment and sysctl setup (lines 8–20)**:
   - Sets `net.ipv4.tcp_congestion_control=bpf_cubic5`.
   - Disables the kernel's generic slow-start-after-idle reset (`net.ipv4.tcp_slow_start_after_idle=0`) so CUBIC's own idle handling is tested in isolation, then completes the 3-way handshake (`cwnd = 10`, `delay_min = 20 ms`).
2. **Slow Start to `cwnd = 20` (lines 22–28)**:
   - Sends 10 segments (10,000 bytes) and receives an ACK, growing `tcpi_snd_cwnd` from `10` to `20`.
3. **Packet loss and Fast Recovery into Congestion Avoidance (lines 30–46)**:
   - Sends 20 segments (20,000 bytes); loses the first segment and receives 3 SACKs to enter Fast Recovery.
   - `bpf_cubic5_recalc_ssthresh()` sets `ca->last_max_cwnd = 20`, `snd_ssthresh = (20 * 717) / 1024 = 14`, and `ca->epoch_start = 0`.
   - A cumulative ACK (`ack 30001`) completes recovery: `tcp_complete_cwr()` sets `snd_cwnd = ssthresh = 14` and transitions to `TCP_CA_Open`, and `bpf_cubic5_cong_avoid()` runs on the same ACK. `bictcp_update()` initialises `ca->epoch_start = tcp_jiffies32` and `ca->bic_K = 2524` ($$\approx 2.46\text{ s}$$), and with `bic_target = 15` (`ca->cnt = 14`), `tcpi_snd_cwnd` grows from `14` to `15`.
4. **One RTT (15 segments) in Congestion Avoidance before going idle (lines 48–54)**:
   - Sends 15 segments (15,000 bytes) and receives an ACK $$20\text{ ms}$$ later, verifying `tcpi_snd_cwnd == 15`. At this point, `in_flight` drops to `0`.
5. **2.0-second idle period followed by 15 segments and ACK (lines 56–67)**:
   - Sits idle for `+2.000` seconds with `in_flight == 0`, then writes 15 segments (`15,000` bytes), receives the ACK $$20\text{ ms}$$ later, and asserts `tcpi_snd_cwnd <= 15`.
   - **Why `starter.bpf.c` fails**: `bpf_cubic5_cwnd_event_tx_start()` is a no-op, leaving `ca->epoch_start` 2 seconds in the past. When the ACK arrives after the idle period, `t` jumps to $$\approx 2.06\text{ s}$$, `bic_target` leaps to `W_max = 20`, `ca->cnt` drops to `15 / (20 - 15) = 3`, and `tcpi_snd_cwnd` spikes from `15` to **`>= 20`** in a single ACK, failing `assert tcpi_snd_cwnd <= 15`.
   - **Why `solution.bpf.c` passes**: sending the first segment after the pause triggers `bpf_cubic5_cwnd_event_tx_start()`, which shifts `ca->epoch_start` forward by the 2-second idle delta. When the ACK arrives $$20\text{ ms}$$ later, `t` is only $$\approx 60\text{ ms}$$, `bic_target = 15 <= cwnd (15)` (`ca->cnt = 100 * 15 = 1500`), and `tcpi_snd_cwnd` stays **`15`** (`<= 15`), passing the assertion.

### 5.2 How to Run

```sh
../../run_test.sh cubic/step03-idle-epoch-shift starter
../../run_test.sh cubic/step03-idle-epoch-shift solution
```
