# BPF TCP Congestion Control & Qdisc Hands-on Labs (`reno`, `cubic`, `bbr`, `qdisc-fifo`, `qdisc-fq`, `qdisc-fq-codel`)

[日本語](README.md)

This hands-on curriculum walks you through implementing and testing **three TCP congestion control algorithms (`reno`, `cubic`, `bbr`)** and **three packet schedulers / Qdiscs (`qdisc-fifo`, `qdisc-fq`, `qdisc-fq-codel`)** — 6 tracks and 18 steps in total — from scratch, step by step, using Linux kernel BPF `struct_ops` (`struct tcp_congestion_ops` and `struct Qdisc_ops`) and `packetdrill`.

No prior familiarity with these algorithms is assumed. Every track (`README.en.md`) and every step begins with a first-principles explanation of **why the algorithm exists, how its state machine and formulas work, and what kernel structs and BPF kfuncs are already provided**, followed by **Socratic design checkpoints (VibeWise style)** $$\to$$ **BPF implementation** $$\to$$ **deterministic `packetdrill` verification**.

---

## The 6 Tracks (18 Steps) & Directory Layout

```text
labs/
├── Makefile                    # Builds all 18 .bpf.o objects & runs all tests (make check)
├── run_test.sh                 # Runs vng + bpftool + packetdrill for a single step or all steps
├── include/
│   ├── vmlinux.h               # Kernel BTF type definitions
│   ├── bpf_cc_helpers.h        # Common inline helpers for BPF CC programs
│   └── bpf_qdisc_helpers.h     # BPF Qdisc kfunc declarations and inline helpers
├── reno/                       # Track 1: TCP Reno — The Foundation of All Congestion Control (2 steps)
│   ├── README.en.md            # Reno algorithm guide (AIMD, Slow Start, CA, and tcp_sock layout)
│   ├── step01-slow-start-ai/   # Step 1: Slow Start and Additive Increase
│   └── step02-loss-undo/       # Step 2: Loss (Multiplicative Decrease) and DSACK Undo
├── cubic/                      # Track 2: TCP CUBIC for High-BDP Networks (4 steps)
│   ├── README.en.md            # Why Reno falls short & how CUBIC's 3rd-order polynomial works
│   ├── step01-cubic-curve/     # Step 1: beta=0.7, fixed-point cube root, and W_cubic(t) curve
│   ├── step02-reno-friendly-fast-conv/ # Step 2: Reno-friendly region (W_est) and Fast Convergence
│   ├── step03-idle-epoch-shift/# Step 3: Epoch shift after idle periods (CA_EVENT_TX_START)
│   └── step04-hystart/         # Step 4: HyStart (ACK Train & Delay increase Slow Start exit)
├── bbr/                        # Track 3: Model-Based TCP BBRv1 — BtlBw and RTprop (6 steps)
│   ├── README.en.md            # Why loss-based CC struggles & BBR's pipe model
│   ├── step01-pacing-rate/     # Step 1: .cong_control hook and Pacing Rate from Delivery Rate
│   ├── step02-btlbw-rtprop/    # Step 2: BDP calculation from BtlBw (max bw) and RTprop (min RTT)
│   ├── step03-startup-drain/   # Step 3: STARTUP to DRAIN transition (full-pipe detection)
│   ├── step04-probe-bw/        # Step 4: 8-phase PROBE_BW gain cycling
│   ├── step05-probe-rtt/       # Step 5: PROBE_RTT transition and cwnd = 4
│   └── step06-loss-ack-aggregation/ # Step 6: Loss cwnd save/restore and extra_acked compensation
├── qdisc-fifo/                 # Track 4: BPF Qdisc Basics — Packet/Byte FIFO & Priority Bands (2 steps)
│   ├── README.en.md            # Qdisc_ops architecture, kfuncs, and link-rate emulation guide
│   ├── step01-pfifo/           # Step 1: Packet-limit FIFO (pfifo) and Tail Drop
│   └── step02-bfifo-prio/      # Step 2: Byte-limit FIFO (bfifo) and 2-band Strict Priority Queue
├── qdisc-fq/                   # Track 5: Fair Queueing (FQ) and EDT Pacing (2 steps)
│   ├── README.en.md            # Head-of-Line blocking, DRR, and EDT (Earliest Departure Time) guide
│   ├── step01-flow-drr/        # Step 1: Flow hashing, new_flows sparse-flow boost, and DRR scheduling
│   └── step02-edt-pacing/      # Step 2: skb->tstamp (EDT) and bpf_qdisc_watchdog_schedule pacing
└── qdisc-fq-codel/             # Track 6: CoDel AQM and FQ-CoDel (2 steps)
    ├── README.en.md            # Bufferbloat, CoDel sojourn time & inverse-sqrt law, and FQ-CoDel guide
    ├── step01-codel-aqm/       # Step 1: CoDel sojourn-time tracking and 1/sqrt(count) head-drop
    └── step02-fq-codel/        # Step 2: Combining per-flow DRR queues with per-flow CoDel AQM
```

---

## How to Use

### 1. Read the Track Overview, Then Work Step by Step

1. Start with the track overview (`labs/reno/README.en.md`, `labs/qdisc-fifo/README.en.md`, etc.) to build a mental model of the algorithm.
2. Open the step's `README.en.md`, read the **Algorithm & Background** section, and work through the **Design Checkpoints** before opening `starter.bpf.c`.
3. Run `starter` first to confirm that the unimplemented skeleton fails the `packetdrill` assertion as expected:
   ```sh
   ./run_test.sh reno/step01-slow-start-ai starter
   ./run_test.sh qdisc-fifo/step01-pfifo starter
   ```
4. Fill in the `TODO` markers in `starter.bpf.c` and re-run, or run the reference `solution` to verify that all `packetdrill` assertions pass:
   ```sh
   ./run_test.sh reno/step01-slow-start-ai solution
   ./run_test.sh qdisc-fifo/step01-pfifo solution
   ```

### 2. Build and Verify All 18 Reference Solutions at Once

```sh
make check
```
