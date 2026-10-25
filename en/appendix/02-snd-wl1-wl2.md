# What Are `SND.WL1` (`snd_wl1`) and `SND.WL2` (`snd_wl2`) For?

(Related chapters: [Chapter 1: Core data structures](../01-data-structures.md), [Chapter 7: ACK processing and RTT estimation](../07-ack-processing.md))

Every incoming TCP header carries the peer's current receive window (`seg.wnd`), which the sender records in `tp->snd_wnd`.
However, packets can be **reordered** in the network. If the sender blindly overwrote `tp->snd_wnd` with every arriving packet's `seg.wnd`, a delayed older packet arriving out of order could overwrite a newer window update with a stale value.

```text
Receiver                                       Sender
--------                                       ------
(1) Sends packet P1:
    seq=500, ack=1000, win=4000 (plenty of room)
         \
          \  (P1 is delayed and overtaken in the network)
           \
(2) Sends packet P2:                           (2') P2 arrives first!
    seq=500, ack=2000, win=0 (buffer full!)    --->  Updates snd_wnd = 0
                                                     Records snd_wl1 = 500, snd_wl2 = 2000
             \
              \-------------------------------> (1') Delayed P1 arrives later!
                                                     seg.seq=500, seg.ack=1000, seg.wnd=4000
                                                     If accepted blindly, snd_wnd would roll
                                                     back to 4000 and overrun the full peer!
```

To prevent stale window updates, TCP (RFC 9293 §3.3) records **the `seq` (`snd_wl1`) and `ack` (`snd_wl2`) of the segment that last updated `snd_wnd`**, and `tcp_may_update_window()` only updates `snd_wnd` when the incoming segment is provably newer:

1. **`after(seq, snd_wl1)`**: The peer has advanced its own send sequence number (`seq > snd_wl1`).
2. **`seq == snd_wl1 && after(ack, snd_wl2)`**: The peer's `seq` is unchanged, but it is acknowledging newer data from us (`ack > snd_wl2`).
3. **`seq == snd_wl1 && ack == snd_wl2 && nwin > snd_wnd`**: Both `seq` and `ack` are identical, and the peer is advertising a larger window (a pure Window Update ACK after the peer application read data).

In the diagram above, when the delayed P1 arrives with `seq == 500` and `ack (1000) < snd_wl2 (2000)`, `tcp_may_update_window()` recognizes it as stale and ignores its `win=4000`.
