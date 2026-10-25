# Step 5: Idle Epoch Shift (`CA_EVENT_TX_START`)

## Goal

By Step 4, our CUBIC implementation has the full cubic curve, Fast Convergence, and the Reno-friendly region in Congestion Avoidance. However, one critical real-world pitfall remains that can trigger severe packet bursts after an application pauses sending.

Reno advances `snd_cwnd` purely by counting arriving ACKs, so when an application goes idle and no ACKs arrive, Reno's clock naturally pauses. CUBIC, by contrast, computes `bic_target` as a cubic function of the **wall-clock time elapsed since the start of the current epoch**, `t = tcp_jiffies32 - ca->epoch_start`.

What happens if a flow in Congestion Avoidance sits idle for 2–5 seconds and then resumes sending?
If `ca->epoch_start` is left untouched at its old timestamp, then on the very first ACK after the idle gap, the elapsed time `t` jumps ahead by several seconds at once. `(t - K)^3` explodes, `bic_target` shoots up, and `snd_cwnd` surges by up to 1.5x (`ca->cnt = 2`) in a single RTT right after idle! This is why [RFC 9438 §5.8](https://www.rfc-editor.org/rfc/rfc9438#section-5.8) states that the elapsed time $t$ MUST NOT include periods during which `cwnd` was not updated due to application-limited behaviour.

In this step, you will implement the `.cwnd_event_tx_start` hook (`CA_EVENT_TX_START` event) in `struct tcp_congestion_ops` to shift `ca->epoch_start` forward by the idle duration whenever transmission resumes from `in_flight == 0`.

---

## Design Checkpoints (VibeWise style — Think before coding)

Before opening `starter.bpf.c`, think through these four design questions on your own.

### Q1. Which hook notifies the congestion control module when transmission resumes from idle?

How does the TCP core notify a congestion control algorithm at the exact moment a new data packet is transmitted when there are currently zero packets in flight (`tcp_packets_in_flight(tp) == 0`)? Which member of `struct tcp_congestion_ops` should we implement?

<details>
<summary>Hint and Answer</summary>

In the transmit path, `tcp_event_data_sent()` ([net/ipv4/tcp_output.c#L179](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/net/ipv4/tcp_output.c?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n179)) checks `if (tcp_packets_in_flight(tp) == 0)` and fires `tcp_ca_event(sk, CA_EVENT_TX_START)`.

In `include/net/tcp.h`, `tcp_ca_event()` ([include/net/tcp.h#L1491](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/include/net/tcp.h?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n1491)) dispatches `CA_EVENT_TX_START` to the dedicated `.cwnd_event_tx_start(sk)` callback (split out from the generic `.cwnd_event(sk, ev)` in commit `d1e59a469737` to optimise indirect branch prediction). Therefore, we wire our handler to `.cwnd_event_tx_start` in `struct tcp_congestion_ops`.

</details>

### Q2. Which `struct tcp_sock` field tells us how long the connection was idle?

Without adding any new timestamp field to `struct bpf_bictcp`, we can compute the idle duration `delta` (in jiffies) using a field that `struct tcp_sock` already maintains for the last data transmission time. Which field is it, and why does it still hold the pre-idle timestamp when `.cwnd_event_tx_start` runs?

<details>
<summary>Hint and Answer</summary>

We use `tcp_sk(sk)->lsndtime`. Inside `tcp_event_data_sent()`, the sequence of operations is:

```c
if (tcp_packets_in_flight(tp) == 0)
	tcp_ca_event(sk, CA_EVENT_TX_START);

tp->lsndtime = now;
```

Because `CA_EVENT_TX_START` fires **immediately before** `tp->lsndtime = now` updates the timestamp, `tp->lsndtime` inside `.cwnd_event_tx_start(sk)` still holds the jiffies timestamp of the last packet transmitted before the connection went idle.
Thus, `__s32 delta = (__s32)(now - tcp_sk(sk)->lsndtime);` gives the exact idle duration in jiffies.

</details>

### Q3. Why shift `ca->epoch_start += delta` instead of resetting `ca->epoch_start = 0`?

If our only goal were preventing `t` from exploding after idle, we could simply reset `ca->epoch_start = 0` so the next ACK starts a fresh epoch. Why do we shift `ca->epoch_start += delta` instead of resetting it to `0`?

<details>
<summary>Hint and Answer</summary>

If we reset `ca->epoch_start = 0`, the first ACK after the idle period would force `bictcp_update()` to recompute `ca->bic_K` and start a brand-new epoch from scratch.
Because CUBIC's concave region is steepest right at the beginning of an epoch and flattens out as `cwnd` approaches `W_max`, resetting `epoch_start = 0` on every brief idle pause would cause a flow near the `W_max` plateau to repeatedly restart the aggressive initial climb as if it had just exited recovery.
Shifting `ca->epoch_start += delta` translates the cubic curve horizontally in time, **preserving the shape of the curve and the flow's current position on it** while excluding only the idle gap (commit `30927520dbae`).

</details>

### Q4. How can `ca->epoch_start + delta` end up in the future (`> now`), and what breaks if we don't clamp it?

Right after `ca->epoch_start += delta`, we must clamp `if (after(ca->epoch_start, now)) ca->epoch_start = now;`. Given that `delta = now - tp->lsndtime`, how could `ca->epoch_start + delta` possibly exceed `now`? And what happens in `bictcp_update()` if `ca->epoch_start` is in the future?

<details>
<summary>Hint and Answer</summary>

`tp->lsndtime` is recorded on **packet transmission**, whereas `ca->epoch_start` is recorded on **ACK arrival** (`bictcp_update()`).
Consider the following event sequence:

1. At time `T0`, the sender transmits its last data segment before recovery finishes (`tp->lsndtime = T0`).
2. At time `T1` (`T1 > T0`), an ACK arrives that completes recovery, or the first Congestion Avoidance ACK arrives and initialises `ca->epoch_start = T1` (no new data is sent at `T1`, so `tp->lsndtime` remains `T0`).
3. The connection sits idle until time `T2`, when the application writes new data.

At `T2`, `delta = T2 - T0`, so `ca->epoch_start + delta = T1 + (T2 - T0) = T2 + (T1 - T0) > T2 (now)`, placing `ca->epoch_start` in the future by `T1 - T0`!
If an ACK arrives in the same jiffy (`T2`) while `ca->epoch_start > tcp_jiffies32`, then `(__s32)(tcp_jiffies32 - ca->epoch_start)` in `bictcp_update()` evaluates to a **negative number**. When assigned to `__u64 t`, sign extension turns it into a huge value near $2^{64}$, corrupting `offs^3` and `bic_target` (commit `c2e7204d180f`). Clamping with `if (after(ca->epoch_start, now)) ca->epoch_start = now;` prevents `epoch_start` from ever moving into the future.

</details>

---

## Step-by-step Walkthrough (`solution.bpf.c`)

All functions from Step 4 (`bictcp_update()`, `bpf_cubic5_cong_avoid()`, `bpf_cubic5_recalc_ssthresh()`, `bpf_cubic5_state()`, `bpf_cubic5_acked()`, and `bpf_cubic5_undo_cwnd()`) carry over unchanged. We add `bpf_cubic5_cwnd_event_tx_start()` and register it in `struct tcp_congestion_ops bpf_cubic5`.

### 1. `bpf_cubic5_cwnd_event_tx_start()`

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

- `now = tcp_jiffies32`: reads the lower 32 bits of `bpf_jiffies64()`.
- `delta = now - tcp_sk(sk)->lsndtime`: computes the signed 32-bit jiffies elapsed since the last packet was sent.
- `if (ca->epoch_start && delta > 0)`:
  - Checking `ca->epoch_start != 0` ensures we do nothing during Slow Start or immediately after loss before an epoch has been initialised (adding `delta` when `epoch_start == 0` would corrupt the `0` sentinel).
  - When `delta > 0`, `ca->epoch_start += delta` shifts the epoch start forward by the idle duration.
- `if (after(ca->epoch_start, now)) ca->epoch_start = now;`: uses `after(seq2, seq1)` from `bpf_cc_helpers.h` to clamp `ca->epoch_start` so it never exceeds `now`.

### 2. Registering in `struct tcp_congestion_ops bpf_cubic5`

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

## Understanding `test.pkt`

`test.pkt` verifies the idle epoch shift across five stages (with a 20 ms RTT):

1. **Connection establishment & sysctls**:
   - Sets `net.ipv4.tcp_congestion_control=bpf_cubic5` and `net.ipv4.tcp_slow_start_after_idle=0`, then completes the 3-way handshake (`cwnd = 10`, `delay_min = 20 ms`).
2. **Slow Start to `cwnd = 20`**:
   - Sends 10 segments (10,000 bytes) and receives an ACK, growing `tcpi_snd_cwnd` from `10` to `20`.
3. **Packet loss and Fast Recovery into Congestion Avoidance**:
   - Sends 20 segments; loses the first segment and receives 3 SACKs to enter Fast Recovery.
   - `bpf_cubic5_recalc_ssthresh()` records `last_max_cwnd = 20`, sets `snd_ssthresh = (20 * 717) / 1024 = 14`, and resets `epoch_start = 0`.
   - A cumulative ACK (`ack 30001`) completes recovery: `tcp_complete_cwr()` sets `snd_cwnd = ssthresh = 14` and transitions to `TCP_CA_Open`, and `bpf_cubic5_cong_avoid()` runs on the same ACK (`acked = 17`). `bictcp_update()` initialises `ca->epoch_start = tcp_jiffies32` and `ca->bic_K = 2524` (~2.46 seconds), and with `bic_target = 15` (`ca->cnt = 14`), `tcpi_snd_cwnd` grows from `14` to `15`.
4. **One RTT (15 segments) in Congestion Avoidance before going idle**:
   - Sends 15 segments (15,000 bytes) and ACKs them, confirming `tcpi_snd_cwnd == 15`.
5. **2.0-second idle period followed by 15 segments and ACK**:
   - Waits idle for `+2.000` seconds with `in_flight == 0`, then writes 15 segments (`15,000` bytes).
   - **With `solution.bpf.c`**: transmitting the first segment triggers `bpf_cubic5_cwnd_event_tx_start()`, which shifts `ca->epoch_start` forward by the 2-second idle delta. When the ACK arrives 20 ms later, `t` is only ~60 ms, so `bic_target = 15 <= cwnd (15)`, `ca->cnt = 100 * 15 = 1500` (and Reno-friendly `ca->tcp_cwnd == 15`). `tcpi_snd_cwnd` remains **`15`** (`<= 15`), passing the assertion.
   - **With `starter.bpf.c`**: `ca->epoch_start` is not shifted, so `t` jumps by 2 seconds to ~2.06 seconds. `bic_target` leaps to `W_max = 20`, `ca->cnt` drops to `15 / (20 - 15) = 3`, and `tcpi_snd_cwnd` spikes from `15` to **`>= 20`** in a single ACK, failing `assert tcpi_snd_cwnd <= 15`.

---

## How to run

```sh
# 1. Verify that starter.bpf.c fails the packetdrill assertion
../../run_test.sh cubic/step05-idle-epoch-shift starter

# 2. Implement the TODO in starter.bpf.c and re-run (or run the model solution)
../../run_test.sh cubic/step05-idle-epoch-shift solution
```
