// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 2: Bottleneck Bandwidth (BtlBw) and Round-Trip Propagation Time (RtProp) Tracking
 *
 * In BBR, the network path model consists of:
 * - RtProp (min_rtt_us): the two-way propagation delay of the path without queuing.
 *   Tracked via a windowed min-filter over RTT samples.
 * - BtlBw (max_bw): the bottleneck bandwidth available to the flow.
 *   Tracked via a windowed max-filter over rate samples from ACKs.
 *
 * Important BBR invariant:
 * Application-limited (app-limited) rate samples must NEVER decrease max_bw
 * unless the sample is larger than the current max_bw estimate.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE 24
#define BW_UNIT (1U << BW_SCALE)
#define BBR_SCALE 8
#define BBR_UNIT (1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET 4U
#define BBR_PACING_MARGIN_PCT 1U

struct bpf_bbr2_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
};

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - BBR_PACING_MARGIN_PCT);
	return rate >> BW_SCALE;
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, int gain)
{
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (rate > 0)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	__u32 rtt_us = tp->srtt_us ? (tp->srtt_us >> 3) : USEC_PER_MSEC;
	__u64 bw;

	if (!rtt_us)
		rtt_us = 1;

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	bbr_set_pacing_rate(sk, bw, BBR_UNIT);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr2_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr2_ca *ca = inet_csk_ca(sk);
	__u32 bw, bdp;

	if (rs->rtt_us > 0 && (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	if (rs->delivered > 0 && rs->interval_us > 0) {
		if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
			ca->round_cnt++;
			ca->next_rtt_delivered = tp->delivered;
		}

		bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);

		if (!rs->is_app_limited || bw >= ca->max_bw) {
			if (bw > ca->max_bw)
				ca->max_bw = bw;
		}
	}

	bbr_set_pacing_rate(sk, ca->max_bw, BBR_UNIT);

	bdp = bbr_bdp(sk, ca->max_bw, BBR_UNIT);
	tp->snd_cwnd = max(bdp + 4U, BBR_CWND_MIN_TARGET);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_ssthresh, struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr2_undo_cwnd, struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr2 = {
	.name		= "bpf_bbr2",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr2_init,
	.cong_control	= (void *)bpf_bbr2_cong_control,
	.ssthresh	= (void *)bpf_bbr2_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr2_undo_cwnd,
};
