# Step 2: Earliest Departure Time (EDT) Pacing and Horizon Drop

[Japanese](README.md)

## 1. Learning Goals

In this step, you will use red-black trees (`bpf_rb_root`) to implement the kernel's modern pacing mechanism—**Earliest Departure Time (EDT) pacing** and **Horizon Drop**—as a BPF `struct_ops` (`struct Qdisc_ops`) Qdisc.

- For an overview of the track, see the [FQ Track README](../README.en.md).

---

## 2. Algorithm and Concepts

### 2.1 Why Pacing? (Mitigating TCP Bursts)

TCP emits packets in response to incoming ACKs. When a receiver aggregates multiple packets in a single ACK (Delayed ACK or LRO/GRO), the sender dumps 10 to 30 packets into the network **back-to-back at wire speed**.

```text
Conventional Burst Transmission:
||||||||||||                ||||||||||||                ||||||||||||
[-- 10 back-to-back pkts --] [-- 10 back-to-back pkts --] [-- 10 back-to-back pkts --]
(Overflows shallow switch buffers, causing packet drops!)

Paced Transmission (EDT):
|   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |
(Smooth stream distributed across the RTT, keeping buffers empty and latency low!)
```

Dumping bursts into intermediate switches overflows shallow buffers and causes packet drops. Pacing smooths the stream into a continuous trickle.

### 2.2 Earliest Departure Time (EDT) Model

In the **EDT model** (Linux 4.20+), each packet is stamped with its scheduled departure time (**`skb->tstamp`**) based on the socket's configured pacing rate (`sk->sk_max_pacing_rate`).

The packet's scheduled departure time `time_to_send` is calculated as follows:

1. **Explicit `skb->tstamp` present**: Use the timestamp already computed by TCP / BBR.
2. **Socket pacing rate (`sk_max_pacing_rate`) active**:
   Anchor against the later of the flow's scheduled next packet time or current time:
   $$\text{time to send} = \max(\text{flow} \to \text{next packet time}, \text{now})$$
   Advance the flow's next eligible transmission timestamp by the transmission duration $$\Delta t$$:
   $$\text{flow} \to \text{next packet time} = \text{time to send} + \frac{\text{packet len} \times 10^9}{\text{rate}}$$
3. **Otherwise**: Immediately transmittable (`time_to_send = now`).

### 2.3 Throttled Flow Management (`fq_delayed`) and Watchdog Timer

During dequeue, when inspecting the head packet of a flow:
If `now < time_to_send` (the packet is scheduled for the future):

1. **Throttle the flow**: Set `flow->age = ~0ULL` and insert the flow into the red-black tree **`fq_delayed`** ordered by `flow->time_next_packet`.
2. **Arm the Watchdog Timer**: Schedule the kernel timer for the earliest throttled flow timestamp (`time_next_delayed_flow`) via `bpf_qdisc_watchdog_schedule()`.
3. **Wakeup and Restoration**: When the timer fires, dequeue calls `fq_check_throttled(now)` to pop all unthrottled flows from `fq_delayed` and move them to `fq_old_flows`.

### 2.4 Horizon Check

What if an application sets a pathological pacing rate via `setsockopt(..., SO_MAX_PACING_RATE)` (e.g. 100 bytes/sec)?
A single 1000-byte packet would delay transmission by over 10 seconds, causing packets to pile up in memory for minutes.

To prevent head-of-line blocking and memory exhaustion, FQ enforces a **horizon (`HORIZON_NS = 2` seconds)**. Any packet scheduled more than 2 seconds into the future is immediately dropped (`NET_XMIT_DROP`).

---

## 3. Concrete Trace (`test.pkt` Scenario)

Parameters: `SO_MAX_PACING_RATE = 200,000` bytes/sec (200 KB/s), packet length $$\approx 1066$$ bytes.
Pacing delay per packet:
$$\Delta t = \frac{1066 \times 10^9}{200,000} = 5,330,000 \text{ ns} \approx 5.33 \text{ ms}$$

| Event | Departure Time | State & Timer Action |
| :--- | :--- | :--- |
| `write(4, ..., 3000)` (3 segments) | **+0.000** | Pkt 1 (1:1001) is due `now`: **transmitted immediately**!<br>`time_next_packet` advances to `now + 5.33 ms`. |
| Inspection of Pkt 2 | — | Pkt 2 is due at `+5.33 ms` (future).<br>Flow is throttled into `fq_delayed`, timer armed. |
| Timer fires (+5.33 ms) | **~ +0.005** | Pkt 2 (1001:2001) **transmitted via Watchdog**!<br>`time_next_packet` advances to `now + 10.66 ms`. |
| Timer fires (+10.66 ms) | **~ +0.010** | Pkt 3 (2001:3001) **transmitted via Watchdog**! |
| Set `SO_MAX_PACING_RATE = 100` B/s | — | For 1000B packets, $$\Delta t = \frac{1066 \times 10^9}{100} > 10$$ seconds! |
| `write(4, ..., 2000)` (2 segments) | **+0.000** | Pkt 1 (3001:4001) is transmitted immediately;<br>`time_next_packet` advances to `now + 10.66s`. |
| Enqueue of Pkt 2 | — | Pkt 2 is scheduled at `now + 10.66s`, which **exceeds the 2s horizon**!<br>**Dropped immediately** via `bpf_qdisc_skb_drop()`! |

---

## 4. Design Checkpoints

### Q1. What is the precedence order when computing `time_to_send`?

<details>
<summary>Hint & Answer</summary>

- **Hint**: Consider cases where `skb->tstamp` is already set, where the socket has a pacing rate, and default fallbacks.
- **Answer**:
  1. If `skb->tstamp != 0`, respect `skb->tstamp`.
  2. Else if `rate > 0 && rate != ~0ULL`, use `max(flow->time_next_packet, now)`.
  3. Otherwise, use `now`.

</details>

### Q2. How is the horizon drop implemented?

<details>
<summary>Hint & Answer</summary>

- **Hint**: Compare using signed 64-bit integers and remember to release the stashed flow pointer back into the map value.
- **Answer**:
  ```c
  if ((s64)time_to_send > (s64)(now + HORIZON_NS)) {
      bpf_kptr_xchg_back(&sflow->flow, flow);
      goto drop;
  }
  ```
  At `drop:`, increment `sch->qstats.drops++`, call `bpf_qdisc_skb_drop(skb, to_free)`, and return `NET_XMIT_DROP`.

</details>

---

## 5. Verification Commands

```sh
cd /usr/local/google/home/kuniyu/tcp-eli5/labs

# 1. Verify starter fails
./run_test.sh qdisc-fq/step02-edt-pacing starter

# 2. Verify solution passes
./run_test.sh qdisc-fq/step02-edt-pacing solution
```
