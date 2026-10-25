// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1: Minimal BPF Congestion Control Skeleton + Slow Start & Reno AI
 *
 * Implements TCP Reno from scratch in pure BPF (without calling kernel
 * extern __ksym helpers) so every line of Slow Start and Additive Increase
 * is visible and testable with packetdrill.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

SEC("struct_ops")
void BPF_PROG(bpf_reno1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->snd_cwnd_cnt = 0;
	/* Set initial ssthresh to 22 segments so we can observe:
	 * 1) Pure Slow Start (cwnd: 10 -> 20)
	 * 2) Slow Start -> Congestion Avoidance boundary (cwnd: 20 -> 22,
	 *    with 18 leftover ACKed segments carried into snd_cwnd_cnt)
	 * 3) Reno Additive Increase (cwnd: 22 -> 23)
	 */
	tp->snd_ssthresh = 22;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_ssthresh, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}

SEC("struct_ops")
void BPF_PROG(bpf_reno1_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	/* 1. Slow Start: exponential growth (+1 cwnd per ACKed segment)
	 *    clamped at tp->snd_ssthresh. Any leftover acked segments
	 *    fall through to Congestion Avoidance.
	 */
	if (tcp_in_slow_start(tp)) {
		__u32 cwnd = min(tp->snd_cwnd + acked, tp->snd_ssthresh);

		acked -= cwnd - tp->snd_cwnd;
		tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
		if (!acked)
			return;
	}

	/* 2. Congestion Avoidance (Reno Additive Increase):
	 *    accumulate ACKed segments in tp->snd_cwnd_cnt and increase
	 *    tp->snd_cwnd by 1 once a full cwnd worth of ACKs arrives.
	 */
	tp->snd_cwnd_cnt += acked;
	if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
		tp->snd_cwnd_cnt -= tp->snd_cwnd;
		tp->snd_cwnd = min(tp->snd_cwnd + 1, tp->snd_cwnd_clamp);
	}
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_reno1_undo_cwnd, struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->prior_cwnd);
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_reno1 = {
	.init		= (void *)bpf_reno1_init,
	.ssthresh	= (void *)bpf_reno1_ssthresh,
	.cong_avoid	= (void *)bpf_reno1_cong_avoid,
	.undo_cwnd	= (void *)bpf_reno1_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_reno1",
};
