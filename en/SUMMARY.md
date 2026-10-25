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

## BPF CC hands-on labs

- [15. Hands-on lab: Building CUBIC with BPF CC and packetdrill](15-lab-cubic.md)
- [16. Hands-on lab: Building BBR with BPF CC and packetdrill](16-lab-bbr.md)
