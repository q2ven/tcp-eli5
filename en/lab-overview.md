# Lab Overview and Setup (`reno`, `cubic`, `bbr`)

This hands-on curriculum (`labs/` directory) walks you through implementing and testing three TCP congestion control algorithms — **Reno**, **CUBIC**, and **BBRv1** — from scratch, step by step, using Linux kernel BPF `struct_ops` (`struct tcp_congestion_ops`) and `packetdrill`.

No prior familiarity with Reno, CUBIC, or BBR is assumed. Every track ([Chapter 15: Reno](15-lab-reno.md), [Chapter 16: CUBIC](16-lab-cubic.md), [Chapter 17: BBR](17-lab-bbr.md)) and every step begins with a first-principles explanation of **why the algorithm exists, how its state machine and formulas work, and what fields `struct tcp_sock` already provides**, followed by **Socratic design checkpoints (VibeWise style)** $\to$ **BPF implementation** $\to$ **deterministic `packetdrill` verification**.

---

## The 3 Tracks & Directory Layout

```text
labs/
├── Makefile                    # Builds all .bpf.o objects & runs all tests (make check)
├── run_test.sh                 # Runs vng + bpftool + packetdrill for a single step or all steps
├── include/
│   ├── vmlinux.h               # Kernel BTF type definitions
│   └── bpf_cc_helpers.h        # Common inline helpers for BPF CC programs
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
└── bbr/                        # Track 3: Model-Based TCP BBRv1 — BtlBw and RTprop (6 steps)
    ├── README.en.md            # Why loss-based CC struggles & BBR's pipe model
    ├── step01-pacing-rate/     # Step 1: .cong_control hook and Pacing Rate from Delivery Rate
    ├── step02-btlbw-rtprop/    # Step 2: BDP calculation from BtlBw (max bw) and RTprop (min RTT)
    ├── step03-startup-drain/   # Step 3: STARTUP to DRAIN transition (full-pipe detection)
    ├── step04-probe-bw/        # Step 4: 8-phase PROBE_BW gain cycling
    ├── step05-probe-rtt/       # Step 5: PROBE_RTT transition and cwnd = 4
    └── step06-loss-ack-aggregation/ # Step 6: Loss cwnd save/restore and extra_acked compensation
```

| Chapter / Track | Algorithm | Steps | What You Will Learn |
| :--- | :--- | :--- | :--- |
| **[15. TCP Reno](15-lab-reno.md)** | RFC 5681 Reno | 2 steps | Flow control (`snd_wnd`) vs congestion control (`snd_cwnd`), why AIMD converges to fairness, Slow Start / Congestion Avoidance integer arithmetic (`snd_cwnd_cnt`), `icsk_ca_priv`, and DSACK Undo |
| **[16. TCP CUBIC](16-lab-cubic.md)** | RFC 9438 CUBIC | 4 steps | Why Reno underutilizes high-BDP pipes, the 3rd-order polynomial $W_{\text{cubic}}(t)$ and fixed-point cube root `cubic_root()`, the Reno-friendly region ($W_{\text{est}}$) and Fast Convergence, post-idle Epoch shifting, and HyStart (ACK Train & Delay Increase) |
| **[17. TCP BBR](17-lab-bbr.md)** | BBRv1 | 6 steps | Bufferbloat and shallow-buffer collapse under loss-based CC, the BtlBw × RTprop pipe model, `.cong_control` hook and `sk_pacing_rate`, the STARTUP / DRAIN / PROBE_BW (8-phase) / PROBE_RTT state machine, Loss Undo, and `extra_acked` aggregation compensation |

---

## How to Use

### 1. Read the Track Overview, Then Work Step by Step

1. Start with the track overview ([15. TCP Reno](15-lab-reno.md), [16. TCP CUBIC](16-lab-cubic.md), or [17. TCP BBR](17-lab-bbr.md)) to build a mental model of the algorithm.
2. Read each step's **Algorithm & Background** section and work through the **Design Checkpoints** before opening `starter.bpf.c`.
3. Run `starter` first to confirm that the unimplemented skeleton fails the `packetdrill` assertion as expected:
   ```sh
   cd labs
   ./run_test.sh reno/step01-slow-start-ai starter
   ```
4. Fill in the `TODO` markers in `starter.bpf.c` and re-run, or run the reference `solution` to verify that all `packetdrill` assertions pass:
   ```sh
   ./run_test.sh reno/step01-slow-start-ai solution
   ```

### 2. Build and Verify All 12 Reference Solutions at Once

```sh
cd labs
make check
```
