# Introduction

This book walks through the Linux kernel TCP stack step by step,
following the code of **upstream net-next** at commit
[`279d5c8afcd3`](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/commit/?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e).

## Who is this for?

Readers who are at home on the "slow path" (socket lookup, bind,
netns, ...) but have not yet read the data-transfer fast path and
congestion control in depth.  The goal is to read, review and change
this code with the same eyes as the TCP maintainers.

## How each chapter is organised

1. **The ELI5 version** – build intuition in a world of post and parcels
2. **What the RFCs say** – from an overview of the relevant RFCs down
   to the rules and formulas the code implements
3. **The big picture** – a diagram
4. **Walking through the code** – the key functions, line by line
5. **RFC vs Linux**
6. **Design history** – the upstream commits that shaped the code
7. **Performance notes** – cachelines, locking, lockless readers,
   fixed-point arithmetic
8. **Observing it** – `nstat`, `ss -ti`, tracepoints, packetdrill
9. **IPv4 vs IPv6**
10. **Check your understanding**
11. **Summary**

## Reading order

Chapters 1-2 are a warm-up.  Chapters 3-10 cover the data-transfer
fast path (send -> receive -> ACK -> loss recovery -> timers ->
windows and memory), chapters 11-14 cover congestion control, and
chapters 15-17 cover packet scheduling (Qdisc: FIFO / MQ, FQ / EDT
pacing, and CoDel / FQ-CoDel / RED / NetEm).  Chapters 18-23 are
hands-on labs where you implement and verify Reno, CUBIC, BBR, FIFO,
FQ, and FQ-CoDel from scratch using BPF `struct_ops` and
`packetdrill`.  The fast-path chapters are best read in order; the
congestion control and Qdisc chapters can be read in any order once
you have read chapters 7-8.

## About the source code

Every code excerpt is quoted verbatim from the commit above, and all
links point to the same commit on git.kernel.org.  To read along
locally:

```sh
git worktree add --detach ../tcp-eli5-src 279d5c8afcd38bd841832a39bfe1aa4f8518d98e
```
