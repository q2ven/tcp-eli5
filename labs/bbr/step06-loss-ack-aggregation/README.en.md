# Step 6: Saving/Restoring cwnd Across Loss & ACK Aggregation (extra_acked) Compensation

In this step, we implement BBRv1's response to packet loss (packet conservation and `prior_cwnd` restoration) as well as the adaptation mechanism for ACK aggregation commonly observed on Wi-Fi and LRO/GRO networks.

---

## Goal

- **Understanding BBR's loss response vs Reno/CUBIC**: Learn why BBR does not permanently halve `cwnd` or reduce its pacing rate upon packet loss.
- **Packet conservation in the first round of Recovery**: Master the "one packet sent per ACK received" principle to safely drain the pipeline immediately following loss detection.
- **Restoring `prior_cwnd` upon exiting Recovery**: Implement saving and restoring `prior_cwnd` so that BBR recovers its pre-loss window upon returning to `TCP_CA_Open`.
- **ACK aggregation estimation (`extra_acked`)**: Understand how stretch ACKs and aggregation bursts can starve a strict BDP window, and how BBR measures and compensates with `extra_acked`.

---

## Design Checkpoints (Think before coding)

### Q1. Why does BBR avoid permanently halving its `cwnd` or pacing rate upon packet loss, unlike Reno or CUBIC?
- Hint: How does BBR's explicit physical model (`BtlBw` and `RTprop`) treat packet loss?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Traditional loss-based algorithms like Reno and CUBIC treat packet loss as unambiguous evidence of buffer overflow and congestion, cutting the congestion window by 30% to 50%. However, in modern networks (particularly shallow-buffered switches or wireless links), non-congestive random packet drops and transient queue spikes are frequent.
- BBR explicitly models the physical capacity of the bottleneck (`BtlBw` and `RTprop`). An isolated loss event does not diminish the physical bandwidth or increase the propagation delay of the link. Therefore, BBR does not reduce its estimated bandwidth (`max_bw`) or pacing rate based on loss; it merely subtracts the lost packets from `cwnd` temporarily without penalising subsequent throughput.
</details>

---

### Q2. Why is packet conservation necessary during the first round of Fast Recovery, and how does `prior_cwnd` restoration work?
- Hint: What happens to in-flight packets immediately after loss, and what would happen if `cwnd` remained deflated after recovery finishes?

<details>
<summary>Hint and Answer</summary>

- **Answer**: When loss is first detected, missing packets exist in the network pipe, and uncoordinated retransmissions or bursts could provoke cascading loss. BBR therefore enforces strict packet conservation during the first round trip of Recovery by constraining `cwnd = tcp_packets_in_flight(tp) + max(acked, 1)`, ensuring the sender injects at most one new packet into the network for every acknowledged packet that leaves.
- Once Fast Recovery terminates and the connection returns to `TCP_CA_Open`, BBR restores the window to `cwnd = max(cwnd, ca->prior_cwnd)`, which was saved prior to entering Recovery. This allows BBR to resume its full BDP target window without an unnecessary additive-increase recovery period.
</details>

---

### Q3. Why does the `.set_state` callback handle `TCP_CA_Loss` (RTO) but leave `TCP_CA_Recovery` to `.cong_control`?
- Hint: How do Fast Recovery (duplicate ACKs) and RTO timeouts differ in terms of pipeline state and ACK delivery?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Fast Recovery is triggered by incoming duplicate ACKs, meaning that acknowledgements are still actively arriving and the transmission pipeline is functioning. Therefore, state transitions and packet conservation can be cleanly evaluated inside the per-ACK `.cong_control` hook via the `rate_sample`.
- In contrast, an RTO timeout (`TCP_CA_Loss`) indicates that the pipe has stalled completely and no ACKs are arriving. In that scenario, `.cong_control` is not invoked. Instead, the timer path in the kernel invokes `.set_state` directly. When `new_state == TCP_CA_Loss`, BBR resets `ca->full_bw = 0` and sets `ca->round_start = 1` to safely re-initialise the model.
</details>

---

### Q4. What is ACK aggregation in wireless and GRO environments, and how does BBR calculate and compensate for it with `extra_acked`?
- Hint: What occurs on the sender side when ACKs that were supposed to arrive smoothly are batched together and arrive in sporadic bursts?

<details>
<summary>Hint and Answer</summary>

- **Answer**: Due to Wi-Fi MAC frame aggregation or NIC receive offloads (LRO/GRO), multiple returning TCP ACKs can be coalesced into a single bursty acknowledgement. Between bursts, the sender receives no ACKs, meaning that if `cwnd` were strictly capped at 1× or 2× BDP, the sender would become cwnd-limited before the next batch of ACKs arrives, causing the transmission pipe to drain and degrading throughput.
- To compensate, BBR tracks the elapsed time `epoch_us` in a sampling epoch and calculates the expected number of acknowledged packets: `expected_acked = max_bw * epoch_us`. Any excess packets received above this expected rate (`extra_acked = ack_epoch_acked - expected_acked`) represent aggregation volume. BBR tracks the peak `extra_acked` across a 5-RTT window and adds `min(ca->extra_acked, bdp)` to `target_cwnd`.
</details>

---

## Step-by-step Walkthrough

### 1. Extending `struct bbr_ca`
We add fields to track the ACK aggregation epoch and recovery state:
```c
struct bbr_ca {
	...
	__u64 epoch_mstamp;		/* start of ACK aggregation sampling epoch */
	__u32 ack_epoch_acked;		/* packets (S)ACKed in sampling epoch */
	__u32 extra_acked;		/* max excess packets ACKed in epoch */
	__u8  extra_acked_win_rtts;	/* age of extra_acked in round trips */
	__u8  prev_ca_state;		/* CA state on previous ACK */
	__u8  packet_conservation;	/* packet conservation in 1st round of Recovery */
};
```

### 2. Preserving Pre-Recovery Window with `bbr_save_cwnd()`
We save `prior_cwnd` whenever the connection is in a healthy state before Recovery or PROBE_RTT:
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

### 3. Loss Control in `bbr_set_cwnd_to_recover_or_restore()`
- Subtract losses directly from `cwnd` (`cwnd = max(cwnd - rs->losses, 1)`).
- On transition into `TCP_CA_Recovery`, activate packet conservation and cap `cwnd = tcp_packets_in_flight(tp) + max(acked, 1)`.
- Upon transitioning back out of Recovery, restore `cwnd = max(cwnd, ca->prior_cwnd)`.

### 4. ACK Aggregation Tracking with `bbr_update_ack_aggregation()`
- On each packet-timed round, increment `extra_acked_win_rtts`. After 5 RTTs, decay `extra_acked` by resetting it to 0.
- Compare accumulated ACKs against expected bandwidth delivery (`max_bw * epoch_us`).
- In `bbr_set_cwnd()`, augment the target window: `target_cwnd += min(ca->extra_acked, bbr_bdp(sk, bw, gain))`.

---

## Understanding `test.pkt`

`test.pkt` automates validation of the loss recovery workflow:

1. **Slow Start Initialisation**:
   - Sends 10 segments (10,000 bytes) and verifies that `tcpi_snd_cwnd` doubles from 10 to at least 20 upon ACK arrival.
2. **Fast Recovery Trigger**:
   - Transmits 4 segments (`10001:14001`) with the first segment dropped, prompting 3 selective ACKs (SACKs) from the peer.
   - The kernel enters `TCP_CA_Recovery` and emits a fast retransmit for `10001:11001`.
3. **Packet Conservation Check**:
   - Asserts that `tcpi_ca_state == TCP_CA_Recovery` and `tcpi_snd_cwnd == 1` under packet conservation.
4. **Window Restoration Check**:
   - Acknowledges all 14,000 bytes, returning the state machine to `TCP_CA_Open`.
   - Asserts that `tcpi_snd_cwnd >= 20` is restored rather than halved.

---

## How to run

```sh
# 1. Verify that starter.bpf.c fails packetdrill assertions
../../run_test.sh bbr/step06-loss-ack-aggregation starter

# 2. Implement the TODOs in starter.bpf.c and re-run (or run the reference solution)
../../run_test.sh bbr/step06-loss-ack-aggregation solution
```
