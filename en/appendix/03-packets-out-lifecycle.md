# Do `packets_out`, `sacked_out`, `lost_out`, and `retrans_out` Grow Forever?

(Related chapters: [Chapter 1: Core data structures](../01-data-structures.md), [Chapter 7: ACK processing and RTT estimation](../07-ack-processing.md), [Chapter 8: Loss detection and recovery](../08-loss-recovery.md))

**No — none of these four fields are monotonic cumulative counters.**
All four are **instantaneous gauges** describing the current breakdown of unacknowledged packets sitting in the retransmit queue between `snd_una` and `snd_nxt`. As cumulative ACKs advance `snd_una` and free `sk_buff`s from the retransmit queue, **all four return to `0`**.

```text
|<------------------------ packets_out (e.g. 10 packets) ----------------------->|
+------------------+------------------+------------------+-----------------------+
|   lost_out (2)   |  sacked_out (3)  |       ...        |                  ...  |
|  "Marked lost"   | "Arrived (SACK)" | (Still flying: 5)|                       |
+------------------+------------------+------------------+-----------------------+
         |
         +---> Retransmitted & currently in flight: retrans_out (e.g. 1)
```

$$\text{Packets In Flight} = \text{packets}_{\text{out}} - (\text{sacked}_{\text{out}} + \text{lost}_{\text{out}}) + \text{retrans}_{\text{out}}$$

| Field | Meaning | Incremented (`+`) when | Decremented (`-`) when |
| :--- | :--- | :--- | :--- |
| **`packets_out`** | Total original data packets between `snd_una` and `snd_nxt` | New data is transmitted (`tcp_event_new_data_sent()`) | Cumulative ACK advances `snd_una` and frees `sk_buff`s from the rtx queue (`tcp_clean_rtx_queue()`) |
| **`sacked_out`** | Subset of `packets_out` selectively acknowledged via SACK blocks (or estimated via Reno dupacks) | New SACK blocks are tagged on the rtx queue (`tcp_sacktag_one()`) | **Cumulative ACK (`snd_una`) advances past the SACKed `sk_buff` and frees it** (or RTO clears SACK state) |
| **`lost_out`** | Subset of `packets_out` deemed lost by RACK or SACK reordering threshold | Packets are marked lost (`tcp_mark_skb_lost()`, or all un-SACKed packets on RTO) | **Cumulative ACK covers and frees the lost `sk_buff`** (or loss recovery is undone) |
| **`retrans_out`** | Subset of `lost_out` that has been retransmitted and is currently in flight | A lost `sk_buff` is retransmitted (`__tcp_retransmit_skb()`) | **Cumulative ACK frees the retransmitted `sk_buff`**, or the retransmission itself is deemed lost again |

- **Which fields DO grow monotonically over the life of a connection?**
  - **`tp->delivered`** (total packets delivered to the peer, used by BBR's delivery-rate estimator), **`tp->delivered_ce`** (total ECN CE-marked packets delivered), and **`tp->total_retrans`** (lifetime cumulative retransmissions).
