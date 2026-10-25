# BBRv1 Step 6: Saving/Restoring cwnd Across Loss & ACK Aggregation (`extra_acked`) Compensation

## 1. Goal

In this final step of the BBRv1 track, we complete our BPF BBRv1 implementation by adding **loss recovery handling (Packet Conservation and `prior_cwnd` preservation/restoration)** and **ACK aggregation compensation (`extra_acked`)** for Wi-Fi, cellular, and LRO/GRO paths. Unlike loss-based congestion control algorithms such as Reno or CUBIC, BBR does not permanently halve `snd_cwnd` upon packet loss; instead, it briefly applies packet conservation during the first round of recovery, restores its pre-loss window upon exiting recovery, and augments `target_cwnd` to absorb bursty ACK aggregation.

- For the full track overview and list of steps, see [../README.md](../README.md).

---

## 2. How the Algorithm Works (Read This First)

Before looking at the code or design questions, let us build a complete mental model of how BBRv1 handles packet loss and ACK aggregation from first principles, along with the exact structs and callbacks provided by the Linux kernel.

### 2.1 Why BBR Does Not Halve `cwnd` on Loss, and How It Responds Instead

Traditional loss-based algorithms (Reno, CUBIC) treat every packet loss as a direct symptom of bottleneck buffer overflow and cut `snd_cwnd` by 30%–50%. In modern networks—especially high-speed links with shallow switch buffers or wireless links—isolated packet drops frequently occur due to transient microbursts or random media errors even when the average sending rate matches the link capacity.

BBR explicitly models the physical pipe using **bottleneck bandwidth (`max_bw` = BtlBw)** and **propagation delay (`min_rtt_us` = RTprop)**. Because a single lost packet does not shrink the physical link bandwidth or lengthen the fibre, BBR does not reduce `max_bw` or its pacing rate on loss. Instead, BBR handles loss in **three stages**:

1. **Preserving the Pre-Loss Window (`bbr_save_cwnd()`)**:
   Right before the kernel enters Fast Recovery (`TCP_CA_Recovery`) or RTO Loss recovery (`TCP_CA_Loss`)—and also right before entering `BBR_PROBE_RTT`—BBR saves the healthy congestion window into `ca->prior_cwnd`.
   - If the connection is *not* currently in recovery/loss (`ca->prev_ca_state < TCP_CA_Recovery`) and *not* in `BBR_PROBE_RTT` (`ca->mode != BBR_PROBE_RTT`), `tp->snd_cwnd` represents the normal operating window, so we assign `ca->prior_cwnd = tp->snd_cwnd`.
   - Otherwise (for example, if loss occurs while `snd_cwnd` is already clamped to `4` in `BBR_PROBE_RTT`, or if multiple loss events overlap), overwriting `ca->prior_cwnd` with `tp->snd_cwnd` would destroy the saved pre-loss window. In that case, we preserve the maximum: $$\text{prior}_{\text{cwnd}} = \max(\text{prior}_{\text{cwnd}}, \text{snd}_{\text{cwnd}})$$.
2. **Packet Conservation During the First Round of Recovery**:
   When an ACK reports newly lost packets (`rs->losses > 0`), BBR immediately subtracts those lost packets from `cwnd` (with a floor of `1`):
   $$\text{cwnd} = \max(\text{cwnd} - \text{rs->losses}, 1)$$
   Furthermore, on the exact ACK that transitions the socket into `TCP_CA_Recovery` (`state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery`), BBR enables **Van Jacobson's Packet Conservation** (`ca->packet_conservation = 1`) for **one packet-timed round trip (1 RTT)**. Packet conservation dictates that a sender may only inject a new packet into the network when an old packet is confirmed to have left the network:
   $$\text{cwnd} = \text{inflight}(\text{tp}) + \max(\text{acked}, 1)$$
   To time this single round trip, BBR sets `ca->next_rtt_delivered = tp->delivered` upon entering `TCP_CA_Recovery`. Once `bbr_update_bw()` observes `rs->prior_delivered >= ca->next_rtt_delivered` (`!before(rs->prior_delivered, ca->next_rtt_delivered)`), one full round trip has completed and `ca->packet_conservation` is cleared back to `0`.
3. **Restoring `prior_cwnd` Upon Exiting Recovery/Loss**:
   As soon as all lost segments are recovered and the TCP state transitions from `>= TCP_CA_Recovery` back to `< TCP_CA_Recovery` (such as `TCP_CA_Open`), or when a loss event is declared spurious via `.undo_cwnd`, BBR immediately restores its saved congestion window:
   $$\text{cwnd} = \max(\text{cwnd}, \text{ca}\to\text{prior}_{\text{cwnd}})$$

#### Concrete Numerical Example

- Suppose a connection in `TCP_CA_Open` has `snd_cwnd = 20`. It sends 4 segments (`10001:14001`), the first segment (`10001:11001`) is dropped, and the peer sends 3 SACK duplicate ACKs for the remaining 3 segments.
- On the 3rd duplicate ACK, the kernel invokes `.ssthresh(sk)`, where `bbr_save_cwnd()` saves `ca->prior_cwnd = 20` (and leaves `snd_ssthresh` untouched), then transitions to `TCP_CA_Recovery` and fast-retransmits `10001:11001`.
- During `bbr_set_cwnd_to_recover_or_restore()` on that 3rd SACK (just before the retransmit is accounted as in-flight), 1 packet was marked lost and 3 packets were SACKed, so `tcp_packets_in_flight(tp) == 0` and `acked == 1`. Packet conservation sets `snd_cwnd = 0 + max(1, 1) = 1`.
- When the cumulative ACK (`ack 14001`) arrives 10 ms later, the socket exits `TCP_CA_Recovery` back to `TCP_CA_Open`. BBR restores `snd_cwnd = max(cwnd, prior_cwnd) = 20` instead of halving it to `10`.

---

### 2.2 ACK Aggregation and `extra_acked` Compensation

#### Why Does This Problem Happen? (Intuition)

On Wi-Fi links (which batch frames at the MAC layer), cellular networks, cable modems, or receivers using GRO/LRO (Generic/Large Receive Offload), returning TCP ACKs are frequently coalesced. Instead of receiving a smooth stream of ACKs paced at the bottleneck rate, the sender experiences **a silent pause with zero ACKs followed by a sudden burst of ACKs acknowledging many packets at once**.

If `target_cwnd` is sized strictly to $$\text{BDP} \times \text{cwnd}_{\text{gain}}$$, the sender's in-flight packets will hit `snd_cwnd` during the silent ACK pause. Even though the pacing timer is ready to send and the bottleneck link is not congested, the sender becomes **cwnd-limited and stalls**, under-utilising the link until the delayed ACK burst arrives.

#### Idea and Mathematical Formula

To prevent ACK silences from stalling transmission, BBR measures **how many excess packets were acknowledged in a burst compared to the number of ACKs expected at the steady delivery rate `max_bw`** over a sampling epoch, and adds that excess (`extra_acked`) to `target_cwnd`.

```text
Cumulative ACKs (packets)
  ^
  |                     +--- Actual cumulative ACKs (ack_epoch_acked)
  |                     |    ↑
  |                     |    | extra_acked = ack_epoch_acked - expected_acked
  |              +------+    ↓
  |              |     /
  |       +------+    /  <-- Expected steady ACK line (expected_acked = max_bw * epoch_us)
  |      /           /
  +-----+-----------+---------> Time (delivered_mstamp)
  epoch_mstamp
```

On every valid ACK (`rs->acked_sacked > 0 && rs->delivered > 0 && rs->interval_us > 0`), `bbr_update_ack_aggregation()` performs the following steps:

1. **Compute Epoch Duration and Expected ACKs**:
   Using the microsecond delivery timestamp `tp->delivered_mstamp` and the epoch start timestamp `ca->epoch_mstamp`, compute the elapsed time `epoch_us` and multiply by `ca->max_bw` (which is scaled by `BW_SCALE = 24`, in units of `pkts/us << 24`):
   $$\text{epoch}_{\text{us}} = \text{tp}\to\text{delivered}_{\text{mstamp}} - \text{ca}\to\text{epoch}_{\text{mstamp}}$$
   $$\text{expected}_{\text{acked}} = (\text{ca}\to\text{max}_{\text{bw}} \times \text{epoch}_{\text{us}}) \gg 24$$
2. **Reset the Epoch When Returning to Baseline (or on Overflow)**:
   If the cumulative packets acknowledged in the current epoch so far is at or below the steady-state expectation (`ca->ack_epoch_acked <= expected_acked`), or if adding `rs->acked_sacked` would reach the overflow threshold `BBR_ACK_EPOCH_ACKED_RESET_THRESH` ($$2^{20}$$ packets), start a new epoch at the current ACK:
   - `ca->ack_epoch_acked = 0`
   - `ca->epoch_mstamp = tp->delivered_mstamp`
   - `expected_acked = 0`
3. **Accumulate ACKs and Track Peak `extra_acked`**:
   Add the newly acknowledged packets `rs->acked_sacked`, compute the excess over `expected_acked`, clamp it to `tp->snd_cwnd`, and update the running maximum `ca->extra_acked`:
   $$\text{epoch}_{\text{acked}} \mathrel{+}= \text{rs}\to\text{acked}_{\text{sacked}}$$
   $$\text{extra}_{\text{acked}} = \min(\text{epoch}_{\text{acked}} - \text{expected}_{\text{acked}}, \text{tp}\to\text{snd}_{\text{cwnd}})$$
   $$\text{ca}\to\text{extra}_{\text{acked}} = \max(\text{ca}\to\text{extra}_{\text{acked}}, \text{extra}_{\text{acked}})$$
4. **Windowed Decay Over 5 Round Trips (`BBR_EXTRA_ACKED_WIN_RTTS = 5`)**:
   So that a single old burst does not inflate `cwnd` indefinitely, whenever `ca->round_start` is true, BBR increments `ca->extra_acked_win_rtts = min(0x1FU, ca->extra_acked_win_rtts + 1U)`. When `ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS` (`5`), both `ca->extra_acked_win_rtts` and `ca->extra_acked` are reset to `0`.
5. **Augment `target_cwnd` in `bbr_set_cwnd()`**:
   When computing `target_cwnd`, BBR adds `ca->extra_acked` capped at the current BDP ($$\text{bdp}(\text{sk}, \text{bw}, \text{gain})$$):
   $$\text{target}_{\text{cwnd}} = \text{inflight}(\text{sk}, \text{bw}, \text{gain}) + \min(\text{ca}\to\text{extra}_{\text{acked}}, \text{bdp}(\text{sk}, \text{bw}, \text{gain}))$$

---

### 2.3 What the Kernel Already Provides

To translate these rules into BPF C code, you have access to the following kernel structures, fields, and callbacks:

- **`struct inet_connection_sock` (via `inet_csk(sk)`)**:
  - `icsk_ca_state` (`__u8`): The kernel's TCP congestion state machine. Its numeric ordering is important:
    - `TCP_CA_Open (0)`, `TCP_CA_Disorder (1)`, `TCP_CA_CWR (2)`: Normal / non-recovery states (`< TCP_CA_Recovery`)
    - `TCP_CA_Recovery (3)`: Fast Recovery triggered by SACK / duplicate ACKs
    - `TCP_CA_Loss (4)`: RTO timeout loss recovery
    Thus, `state >= TCP_CA_Recovery` means the connection is currently in Fast Recovery or RTO Loss, and `state < TCP_CA_Recovery` means it is outside recovery/loss.
  - `icsk_ca_priv` (via `inet_csk_ca(sk)`): The per-socket private memory block holding our custom `struct bbr_ca` (which stores `prior_cwnd`, `prev_ca_state`, `packet_conservation`, `epoch_mstamp`, `ack_epoch_acked`, `extra_acked`, and `extra_acked_win_rtts`).
- **`struct tcp_sock` (via `tcp_sk(sk)`)**:
  - `tp->snd_cwnd` (`__u32`): Current congestion window in packets.
  - `tp->snd_cwnd_clamp` (`__u32`): Maximum allowed `snd_cwnd`.
  - `tp->snd_ssthresh` (`__u32`): Slow-start threshold.
  - `tp->delivered` (`__u32`): Total cumulative packets delivered so far.
  - `tp->delivered_mstamp` (`__u64`): Microsecond timestamp of the most recent delivery update (used for ACK aggregation epochs).
  - `tp->tcp_mstamp` (`__u64`): Current socket timestamp in microseconds.
  - `tcp_packets_in_flight(tp)` (helper in `bpf_cc_helpers.h`): Returns the number of packets currently in flight in the pipe (`packets_out - (sacked_out + lost_out) + retrans_out`).
- **`struct rate_sample` (`const struct rate_sample *rs` passed to `.cong_control`)**:
  - `rs->acked_sacked` (`__u32`): Number of packets newly ACKed or SACKed by this ACK.
  - `rs->losses` (`__s32`): Number of packets newly marked lost on this ACK.
  - `rs->delivered` (`__s32`), `rs->interval_us` (`long`): Delivered packet count and sample duration in microseconds (`<= 0` indicates an invalid sample).
  - `rs->prior_delivered` (`__u32`): `tp->delivered` snapshot when the newly ACKed packet was sent (used to detect round-trip boundaries).
- **`struct tcp_congestion_ops` Callbacks**:
  - `.ssthresh(sk)`: Invoked by the kernel immediately before entering `TCP_CA_Recovery` or `TCP_CA_Loss`.
  - `.set_state(sk, new_state)`: Invoked whenever `icsk_ca_state` changes to `new_state` (crucial for catching RTO `TCP_CA_Loss` transitions when no ACK arrives).
  - `.undo_cwnd(sk)`: Invoked when the kernel detects a spurious loss recovery and undoes the window reduction.

---

## 3. Design Checkpoints (Translate the Algorithm into Code)

Use the concepts from Section 2 to work through the design of the `TODO` items in [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/starter.bpf.c).

### Q1. How should `bbr_save_cwnd()` and `bpf_bbr6_ssthresh()` preserve `ca->prior_cwnd`?

The kernel calls `bpf_bbr6_ssthresh()` right before entering Fast Recovery, and `bbr_update_min_rtt()` also calls `bbr_save_cwnd()` right before entering `BBR_PROBE_RTT`. What would go wrong if a packet loss occurred *while* the connection was already in `BBR_PROBE_RTT` (where `tp->snd_cwnd` is clamped to `4`) and `bbr_save_cwnd()` unconditionally wrote `ca->prior_cwnd = tp->snd_cwnd`? How should we guard `bbr_save_cwnd()`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Recall Stage 1 of Section 2.1 and the `icsk_ca_state` ordering in Section 2.3. Under which combinations of `ca->prev_ca_state` and `ca->mode` does `tp->snd_cwnd` reflect the normal un-clamped congestion window?
- **Answer**:
  - If `bbr_save_cwnd()` blindly overwrote `ca->prior_cwnd = tp->snd_cwnd` during `BBR_PROBE_RTT` (`snd_cwnd == 4`) or during an ongoing `TCP_CA_Recovery` (`snd_cwnd` reduced by packet conservation), the previously saved `prior_cwnd` (e.g. `20`) would be overwritten with `4` or `1` and permanently lost.
  - Therefore, `bbr_save_cwnd()` should only assign `ca->prior_cwnd = tp->snd_cwnd;` when `ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT`. Otherwise, it must keep `ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);`.
  - In `bpf_bbr6_ssthresh(sk)`, we simply call `bbr_save_cwnd(sk)` and return `tcp_sk(sk)->snd_ssthresh` without halving `snd_cwnd`.

</details>

---

### Q2. How should `bbr_set_cwnd_to_recover_or_restore()` handle entering, maintaining, and exiting Recovery?

In `bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd)`, we compare the current state `state = inet_csk(sk)->icsk_ca_state` against `prev_state = ca->prev_ca_state`. How should we update `cwnd` and the fields of `struct bbr_ca` across:
1. `rs->losses > 0`
2. Entering `TCP_CA_Recovery` (`state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery`)
3. Exiting Recovery/Loss (`prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery`)
4. While `ca->packet_conservation` is active?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Follow Stages 2 and 3 of Section 2.1. Remember to cast `cwnd` to `__s32` when subtracting `rs->losses` so that if `rs->losses >= cwnd`, the subtraction does not wrap around as an unsigned integer before taking `max(..., 1)`.
- **Answer**:
  1. **Subtract newly detected losses**: `if (rs->losses > 0) cwnd = max((__s32)cwnd - rs->losses, 1);`
  2. **On entry to `TCP_CA_Recovery`**:
     - Set `ca->packet_conservation = 1;`
     - Record the 1-RTT boundary `ca->next_rtt_delivered = tp->delivered;`
     - Set `cwnd = tcp_packets_in_flight(tp) + max(acked, 1U);`
  3. **On exit from Recovery/Loss (`prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery`)**:
     - Restore `cwnd = max(cwnd, ca->prior_cwnd);`
     - Clear `ca->packet_conservation = 0;`
  4. **Update `prev_ca_state` and return**:
     - Save `ca->prev_ca_state = state;`
     - If `ca->packet_conservation` is non-zero, set `*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U));` and `return true;` (telling `bbr_set_cwnd()` to skip normal BDP target calculation and jump straight to `done:`). Otherwise set `*new_cwnd = cwnd;` and `return false;`.

</details>

---

### Q3. Why must `bpf_bbr6_set_state()` explicitly record `TCP_CA_Loss` on an RTO timeout?

Fast Recovery (`TCP_CA_Recovery`) transitions are detected on incoming ACKs inside `.cong_control`. Why do we also need `bpf_bbr6_set_state()` to check `if (new_state == TCP_CA_Loss)` and set `ca->prev_ca_state = TCP_CA_Loss`? What bug would occur after an RTO timeout if `bpf_bbr6_set_state()` were left empty?

<details>
<summary>Hint and Answer</summary>

- **Hint**: When an RTO timer expires, no ACK is arriving, so `.cong_control` is not called. The kernel sets `tp->snd_cwnd = 1` and calls `.set_state(sk, TCP_CA_Loss)`. What happens later when a cumulative ACK arrives for the retransmitted packet and transitions the state directly back to `TCP_CA_Open`?
- **Answer**:
  - If `bpf_bbr6_set_state()` did not set `ca->prev_ca_state = TCP_CA_Loss`, and the socket was in `TCP_CA_Open` prior to the RTO timeout, then when the recovery ACK arrives and returns the socket to `TCP_CA_Open`, `bbr_set_cwnd_to_recover_or_restore()` would see `prev_state == TCP_CA_Open` and `state == TCP_CA_Open`.
  - Consequently, the exit condition `prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery` would evaluate to false, and `tp->snd_cwnd` (which the RTO timer reduced to `1`) would never be restored to `ca->prior_cwnd`!
  - Setting `ca->prev_ca_state = TCP_CA_Loss;`, `ca->full_bw = 0;`, and `ca->round_start = 1;` inside `bpf_bbr6_set_state()` ensures that exiting `TCP_CA_Loss` properly restores `prior_cwnd` and resets full-bandwidth detection.

</details>

---

### Q4. How do we guard, reset, and clamp the `extra_acked` calculation in `bbr_update_ack_aggregation()` and `bbr_set_cwnd()`?

When translating Section 2.2 into `bbr_update_ack_aggregation(sk, rs)` and `bbr_set_cwnd()`:
1. Which `rate_sample` values must be ignored at the start of `bbr_update_ack_aggregation()`?
2. Under what two conditions do we reset the current ACK aggregation epoch (`ack_epoch_acked = 0`, `epoch_mstamp = tp->delivered_mstamp`, `expected_acked = 0`)?
3. How do we clamp `extra_acked` inside `bbr_update_ack_aggregation()`, and how do we cap the amount added to `target_cwnd` in `bbr_set_cwnd()`?

<details>
<summary>Hint and Answer</summary>

- **Hint**: Check Steps 1–5 in Section 2.2. Remember that `ca->max_bw` uses fixed-point scale `BW_SCALE` (`24`).
- **Answer**:
  1. **Guard against invalid samples**: Return early if `rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0`.
  2. **Epoch reset conditions**: After computing `epoch_us = tp->delivered_mstamp - ca->epoch_mstamp` and `expected_acked = ((__u64)ca->max_bw * epoch_us) >> BW_SCALE`, reset the epoch if `ca->ack_epoch_acked <= expected_acked` (delivery caught up with or fell below the steady expectation) or `ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH` (to prevent counter overflow).
  3. **Clamping**:
     - Inside `bbr_update_ack_aggregation()`, clamp `extra_acked = min(ca->ack_epoch_acked - expected_acked, tp->snd_cwnd)` before updating `ca->extra_acked`.
     - Inside `bbr_set_cwnd()`, add at most one gained BDP: `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));`.

</details>

---

## 4. Step-by-step Code Walkthrough (`solution.bpf.c`)

Let us walk through each function implemented in [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/solution.bpf.c).

### 4.1 `bbr_save_cwnd()` and `.ssthresh` / `.undo_cwnd` / `.set_state`

```c
static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->prev_ca_state < TCP_CA_Recovery && ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}
```

- `bpf_bbr6_ssthresh()` calls `bbr_save_cwnd(sk)` so that the pre-loss window is saved into `ca->prior_cwnd`, and returns `tp->snd_ssthresh` without cutting the window.
- `bpf_bbr6_undo_cwnd()` resets `ca->full_bw = 0` and `ca->full_bw_cnt = 0`, and returns `max(tp->snd_cwnd, ca->prior_cwnd)`.
- `bpf_bbr6_set_state()` checks for `new_state == TCP_CA_Loss` and sets `ca->prev_ca_state = TCP_CA_Loss`, `ca->full_bw = 0`, and `ca->round_start = 1`.

### 4.2 `bbr_set_cwnd_to_recover_or_restore()` — Packet Conservation and Window Restoration

```c
static __always_inline bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, __u32 acked, __u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u8 prev_state = ca->prev_ca_state;
	__u8 state = inet_csk(sk)->icsk_ca_state;
	__u32 cwnd = tp->snd_cwnd;

	if (rs->losses > 0)
		cwnd = max((__s32)cwnd - rs->losses, 1);

	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		ca->packet_conservation = 1;
		ca->next_rtt_delivered = tp->delivered;
		cwnd = tcp_packets_in_flight(tp) + max(acked, 1U);
	} else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
		cwnd = max(cwnd, ca->prior_cwnd);
		ca->packet_conservation = 0;
	}
	ca->prev_ca_state = state;

	if (ca->packet_conservation) {
		*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U));
		return true;
	}
	*new_cwnd = cwnd;
	return false;
}
```

- During the first packet-timed round trip of `TCP_CA_Recovery`, `ca->packet_conservation` is `1`, and the function sets `*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + max(acked, 1U))` and returns `true`, causing `bbr_set_cwnd()` to jump directly to `done:`.
- Once `bbr_update_bw()` detects the start of the next round trip (`!before(rs->prior_delivered, ca->next_rtt_delivered)`), it clears `ca->packet_conservation = 0`.
- When the connection exits `TCP_CA_Recovery` or `TCP_CA_Loss` (`prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery`), `cwnd` is restored to `max(cwnd, ca->prior_cwnd)`.

### 4.3 `bbr_update_ack_aggregation()` and `bbr_set_cwnd()` — Measuring and Adding `extra_acked`

```c
static __always_inline void bbr_update_ack_aggregation(struct sock *sk,
						       const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 epoch_us, expected_acked, extra_acked;

	if (rs->acked_sacked <= 0 || rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (ca->round_start) {
		ca->extra_acked_win_rtts = min(0x1FU, ca->extra_acked_win_rtts + 1U);
		if (ca->extra_acked_win_rtts >= BBR_EXTRA_ACKED_WIN_RTTS) {
			ca->extra_acked_win_rtts = 0;
			ca->extra_acked = 0;
		}
	}

	epoch_us = tp->delivered_mstamp - ca->epoch_mstamp;
	expected_acked = ((__u64)ca->max_bw * epoch_us) >> BW_SCALE;

	if (ca->ack_epoch_acked <= expected_acked ||
	    ca->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH) {
		ca->ack_epoch_acked = 0;
		ca->epoch_mstamp = tp->delivered_mstamp;
		expected_acked = 0;
	}

	ca->ack_epoch_acked += rs->acked_sacked;
	extra_acked = ca->ack_epoch_acked - expected_acked;
	extra_acked = min(extra_acked, tp->snd_cwnd);
	if (extra_acked > ca->extra_acked)
		ca->extra_acked = extra_acked;
}
```

- Because `ca->max_bw` is stored in fixed-point units of `pkts/us << BW_SCALE` (`BW_SCALE = 24`), multiplying `(__u64)ca->max_bw * epoch_us` and shifting right by `BW_SCALE` yields the integer number of packets expected to be acknowledged over `epoch_us` microseconds.
- In `bbr_set_cwnd()`, `target_cwnd = bbr_inflight(sk, bw, gain)` is augmented by `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain));`.

---

## 5. Understanding `test.pkt` and How to Run

### Packet Trace and Assertions in `test.pkt`

[test.pkt](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/test.pkt) validates BBRv1's loss recovery, first-round packet conservation, and `prior_cwnd` restoration step by step:

1. **Initial Startup Window Growth (lines 17–24)**:
   - After establishing a connection with a 10 ms base RTT, the sender transmits 10 segments (`1:10001`, 10,000 bytes) and receives `ack 10001` 10 ms later.
   - In `BBR_STARTUP`, `tcpi_snd_cwnd` grows from `10` to `20`, verified by `assert tcpi_snd_cwnd >= 20` and `assert tcpi_ca_state == TCP_CA_Open`.
2. **Triggering Fast Recovery via 3 SACKs (lines 26–36)**:
   - The sender writes 4 segments (`10001:14001`). The first segment (`10001:11001`) is lost, and 3 SACK duplicate ACKs arrive for `11001:12001`, `11001:13001`, and `11001:14001`.
   - Upon receiving the 3rd SACK, the kernel calls `bpf_bbr6_ssthresh()` (saving `ca->prior_cwnd = 20`), transitions to `TCP_CA_Recovery`, and fast-retransmits `10001:11001`.
3. **Verifying Packet Conservation (`cwnd == 1`, lines 38–43)**:
   - When `.cong_control` runs on the 3rd SACK (just before the fast retransmit is counted in flight), 1 packet is marked lost and 3 are SACKed, so `tcp_packets_in_flight(tp)` is `0` and `acked` is `1`.
   - Under packet conservation (`cwnd = in_flight + max(acked, 1)`), the script asserts `tcpi_ca_state == TCP_CA_Recovery` and `tcpi_snd_cwnd == 1`.
4. **Verifying `prior_cwnd` Restoration Upon Exiting Recovery (lines 45–52)**:
   - At `+0.010` s, a cumulative ACK (`ack 14001`) acknowledges all 14,000 bytes, transitioning the socket from `TCP_CA_Recovery` back to `TCP_CA_Open`.
   - The script asserts `tcpi_ca_state == TCP_CA_Open` and `tcpi_snd_cwnd >= 20`, confirming that BBR restored `ca->prior_cwnd` instead of permanently halving the window.

### Why `starter.bpf.c` Fails and `solution.bpf.c` Passes

- In [starter.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/starter.bpf.c), `bbr_set_cwnd_to_recover_or_restore()` contains a placeholder that halves `cwnd` (`cwnd = max(cwnd >> 1U, 2U)`) like Reno/CUBIC and neither enables packet conservation nor restores `prior_cwnd`. Consequently, `starter.bpf.c` fails at line 42 (`assert tcpi_snd_cwnd == 1`).
- In [solution.bpf.c](file:///usr/local/google/home/kuniyu/tcp-eli5/labs/bbr/step06-loss-ack-aggregation/solution.bpf.c), packet conservation sets `tcpi_snd_cwnd == 1` on entry to `TCP_CA_Recovery` and restores `tcpi_snd_cwnd >= 20` when `ack 14001` returns the connection to `TCP_CA_Open`, passing all assertions.

### How to Run

```sh
../../run_test.sh bbr/step06-loss-ack-aggregation starter
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```
