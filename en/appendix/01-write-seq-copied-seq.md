# Why Don't `write_seq` and `copied_seq` Start at `0`?

(Related chapter: [Chapter 1: Core data structures](../01-data-structures.md))

TCP sequence numbers do not start at byte offset `0`; they start at a cryptographically chosen random **ISN (Initial Sequence Number)** negotiated via SipHash during the 3-way handshake (SYN / SYN-ACK).

## Sender Sequence Boundaries (`snd_una` / `snd_nxt` / `write_seq`)

```text
Send stream (byte sequence number space)
  ... | Sent & ACKed | Sent & UnACKed (In-Flight) | Unsent (queued by sendmsg) |
                     ^                            ^                            ^
                  snd_una                      snd_nxt                     write_seq
```

- **`snd_una`**: Sequence number of the oldest byte that has not yet been acknowledged by the peer. Everything up through `snd_una - 1` has been confirmed delivered.
- **`snd_nxt`**: Sequence number of the next byte to be transmitted onto the wire for the first time. `snd_nxt - snd_una` is the number of transmitted, unacknowledged bytes currently in flight (or in the retransmit queue).
- **`write_seq`**: Sequence number immediately following the last byte appended to the socket write queue by `write()` / `sendmsg()`. `write_seq - snd_nxt` is the number of unsent bytes (`notsent_bytes`) that have never been put on the wire yet.

## Receiver Sequence Boundaries (`copied_seq` / `rcv_nxt`)

```text
Receive stream (byte sequence number space)
  ... | Copied to user via recvmsg | Received in kernel, unread | Not yet received |
                                   ^                            ^
                               copied_seq                    rcv_nxt
```

- **`copied_seq`**: Sequence number of the first byte in the receive queue that the application has not yet read via `read()` / `recvmsg()` (everything up through `copied_seq - 1` has already been copied to user space).
- **`rcv_nxt`**: Sequence number of the next contiguous byte the kernel expects from the peer (everything up through `rcv_nxt - 1` has arrived in order).
- Therefore, **`rcv_nxt - copied_seq`** is the number of unread bytes waiting in the kernel's receive queue (`ioctl(SIOCINQ)`).
