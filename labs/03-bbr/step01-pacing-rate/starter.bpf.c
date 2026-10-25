// SPDX-License-Identifier: GPL-2.0
/*
 * Step 1 (Starter): .cong_control hook and Pacing Rate calculation
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BBR_SCALE	10
#define BBR_UNIT	(1 << BBR_SCALE)	/* 1024 */
#define BBR_HIGH_GAIN	2885			/* ~2.885x (2/ln(2)) STARTUP gain */

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 rate;

	if (!rtt_us)
		rtt_us = 1;

	/* Initialize pacing rate: high_gain * init_cwnd * mss / initial_rtt */
	rate = (__u64)tp->snd_cwnd * tp->mss_cache * USEC_PER_SEC;
	rate = div64_u64(rate, rtt_us);
	rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;

	sk->sk_pacing_rate = rate;
	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr1_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);

	(void)tp;
	/*
	 * TODO:
	 * 1. Check if the rate sample is valid:
	 *    if (rs->delivered < 0 || rs->interval_us <= 0) return;
	 * 2. Calculate instantaneous delivery rate in bytes/sec:
	 *    rate = (u64)rs->delivered * tp->mss_cache * USEC_PER_SEC / rs->interval_us
	 *    (Hint: use div64_u64(rate, (__u64)rs->interval_us))
	 * 3. Multiply by BBR_HIGH_GAIN and shift right by BBR_SCALE:
	 *    rate = (rate * BBR_HIGH_GAIN) >> BBR_SCALE;
	 * 4. Write the result to sk->sk_pacing_rate.
	 */
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr1_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr1 = {
	.name		= "bpf_bbr1",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr1_init,
	.cong_control	= (void *)bpf_bbr1_cong_control,
	.ssthresh	= (void *)bpf_bbr1_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr1_undo_cwnd,
};
