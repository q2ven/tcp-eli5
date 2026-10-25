// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 3: STARTUP to DRAIN and PROBE_BW Transition (Starter Skeleton)
 *
 * TODO 1: Implement bbr_check_full_bw_reached() to detect 3 consecutive
 *         non-app-limited rounds without >= 25% bandwidth growth.
 * TODO 2: Implement bbr_check_drain() to transition from BBR_STARTUP to
 *         BBR_DRAIN, and from BBR_DRAIN to BBR_PROBE_BW once in-flight <= BDP.
 * TODO 3: Implement bbr_update_gains() to set pacing_gain and cwnd_gain
 *         for BBR_STARTUP, BBR_DRAIN, and BBR_PROBE_BW.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1U << BW_SCALE)
#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)
#define BBR_CWND_MIN_TARGET	4U
#define BBR_PACING_MARGIN_PCT	1U

enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* steady-state: pace around estimated bw */
};

static const __u32 bbr_high_gain      = BBR_UNIT * 2885 / 1000 + 1; /* 739 (~2.89x) */
static const __u32 bbr_drain_gain     = BBR_UNIT * 1000 / 2885;     /* 88 (~0.35x) */
static const __u32 bbr_cwnd_gain      = BBR_UNIT * 2;               /* 512 (2.0x) */
static const __u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;           /* 320 (1.25x) */
static const __u32 bbr_full_bw_cnt    = 3U;

struct bpf_bbr3_ca {
	__u32 min_rtt_us;
	__u32 max_bw;
	__u32 round_cnt;
	__u32 next_rtt_delivered;
	__u32 full_bw;
	__u32 full_bw_cnt;
	__u32 mode;
	__u32 pacing_gain;
	__u32 cwnd_gain;
	__u8  round_start;
	__u8  full_bw_reached;
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
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (sk->sk_max_pacing_rate && rate > sk->sk_max_pacing_rate)
		rate = sk->sk_max_pacing_rate;
	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
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
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
}

static __always_inline __u32 bbr_bdp(struct sock *sk, __u32 bw, int gain)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (!ca->min_rtt_us || !bw)
		return 10U;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->round_cnt++;
		ca->round_start = 1;
	}

	bw = (__u32)div64_u64((__u64)rs->delivered * BW_UNIT, (__u64)rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw) {
		if (bw > ca->max_bw)
			ca->max_bw = bw;
	}
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	/*
	 * TODO 1: Detect when STARTUP has filled the pipe.
	 * - Return early if ca->full_bw_reached, !ca->round_start, or rs->is_app_limited.
	 * - Compute bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE.
	 * - If ca->max_bw >= bw_thresh:
	 *     update ca->full_bw = ca->max_bw, reset ca->full_bw_cnt = 0, and return.
	 * - Otherwise increment ca->full_bw_cnt, and when it reaches bbr_full_bw_cnt (3),
	 *     set ca->full_bw_reached = 1.
	 */
	(void)sk;
	(void)rs;
	(void)bbr_full_bw_thresh;
	(void)bbr_full_bw_cnt;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	/*
	 * TODO 2: Transition through BBR_DRAIN to BBR_PROBE_BW.
	 * - If ca->mode == BBR_STARTUP and ca->full_bw_reached:
	 *     set ca->mode = BBR_DRAIN and tp->snd_ssthresh = bbr_bdp(sk, ca->max_bw, BBR_UNIT).
	 * - If ca->mode == BBR_DRAIN and tcp_packets_in_flight(tp) <= bbr_bdp(sk, ca->max_bw, BBR_UNIT):
	 *     set ca->mode = BBR_PROBE_BW.
	 */
	(void)sk;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	/*
	 * TODO 3: Set ca->pacing_gain and ca->cwnd_gain based on ca->mode:
	 * - BBR_STARTUP:  pacing_gain = bbr_high_gain,  cwnd_gain = bbr_high_gain
	 * - BBR_DRAIN:    pacing_gain = bbr_drain_gain, cwnd_gain = bbr_high_gain
	 * - BBR_PROBE_BW: pacing_gain = BBR_UNIT,       cwnd_gain = bbr_cwnd_gain
	 */
	(void)bbr_drain_gain;
	(void)bbr_cwnd_gain;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked,
					 __u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		return;

	target_cwnd = bbr_bdp(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < 10U)
		cwnd = cwnd + acked;

	cwnd = max(cwnd, BBR_CWND_MIN_TARGET);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->min_rtt_us = 0;
	ca->max_bw = 0;
	ca->round_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;
	ca->round_start = 0;
	ca->full_bw_reached = 0;

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	sk->sk_pacing_status = SK_PACING_NEEDED;

	bbr_init_pacing_rate_from_rtt(sk);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr3_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us > 0 &&
	    (rs->rtt_us < (__s32)ca->min_rtt_us || ca->min_rtt_us == 0))
		ca->min_rtt_us = rs->rtt_us;

	bbr_update_bw(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr3_undo_cwnd, struct sock *sk)
{
	struct bpf_bbr3_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr3 = {
	.name		= "bpf_bbr3",
	.flags		= TCP_CONG_NON_RESTRICTED,
	.init		= (void *)bpf_bbr3_init,
	.cong_control	= (void *)bpf_bbr3_cong_control,
	.ssthresh	= (void *)bpf_bbr3_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr3_undo_cwnd,
};
