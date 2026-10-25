# Table of contents

- [Introduction](README.md)

## Getting ready

- [1. Core data structures](01-data-structures.md)
- [2. A connection's life on fast-forward](02-connection-lifecycle.md)

## The data-transfer fast path

- [3. Sending (1): tcp_sendmsg and the write queue](03-sendmsg.md)
- [4. Sending (2): from tcp_write_xmit to the wire](04-write-xmit.md)
- [5. Receiving (1): tcp_v4_rcv and header prediction](05-rcv-established.md)
- [6. Receiving (2): queueing data and tcp_recvmsg](06-data-queue-recvmsg.md)
- [7. ACK processing and RTT estimation](07-ack-processing.md)
- [8. Loss detection and recovery](08-loss-recovery.md)
- [9. Timers](09-timers.md)
- [10. Windows and memory management](10-window-memory.md)

## Congestion control

- [11. The congestion control framework and Reno](11-cc-framework-reno.md)
- [12. CUBIC](12-cubic.md)
- [13. BBR](13-bbr.md)
- [14. ECN and DCTCP](14-ecn-dctcp.md)

## Packet scheduling (Qdisc)

- [15. Packet Scheduling (Qdisc) Fundamentals, FIFO, and MQ](15-qdisc-framework-fifo.md)
- [16. Fair Queueing (sch_fq) and EDT (Earliest Departure Time) Pacing](16-qdisc-fq-edt.md)
- [17. AQM (CoDel / FQ-CoDel / RED) and Network Emulation (NetEm)](17-qdisc-aqm-codel-red-netem.md)

## BPF CC & Qdisc hands-on labs

- [Lab overview and setup](lab-overview.md)
- [18. Hands-on (1): Building TCP Reno with BPF CC and packetdrill](18-lab-reno.md)
- [19. Hands-on (2): Building TCP CUBIC with BPF CC and packetdrill](19-lab-cubic.md)
- [20. Hands-on (3): Building TCP BBR with BPF CC and packetdrill](20-lab-bbr.md)
- [21. Hands-on (4): Building FIFO and PRIO with BPF Qdisc](21-lab-qdisc-fifo.md)
- [22. Hands-on (5): Building FQ and EDT Pacing with BPF Qdisc](22-lab-qdisc-fq.md)
- [23. Hands-on (6): Building CoDel and FQ-CoDel with BPF Qdisc](23-lab-qdisc-fq-codel.md)

## Appendix

- [Deep-Dive Notes & Q&A](appendix-notes.md)
