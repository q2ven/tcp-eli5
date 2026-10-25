// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 5: Transition to PROBE_RTT & BBR_MIN_PIPE_CWND (cwnd = 4)
 *
 * Periodically drains the bottleneck queue when min_rtt_us has not been
 * refreshed within BBR_MIN_RTT_WIN_US.
 *
 * Note: Linux kernel tcp_bbr.c uses bbr_min_rtt_win_sec = 10 seconds.
 * In this lab we set BBR_MIN_RTT_WIN_US = 2 * USEC_PER_SEC (2 seconds)
 * so packetdrill tests complete in ~2.5 seconds instead of waiting 10s.
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE		24
#define BW_UNIT			(1ULL << BW_SCALE)

#define BBR_SCALE		8
#define BBR_UNIT		(1U << BBR_SCALE)

#define BBR_HIGH_GAIN		(BBR_UNIT * 2885 / 1000 + 1)
#define BBR_DRAIN_GAIN		(BBR_UNIT * 1000 / 2885)
#define BBR_CWND_GAIN		(BBR_UNIT * 2)

#define BBR_FULL_BW_THRESH	(BBR_UNIT * 5 / 4)
#define BBR_FULL_BW_CNT		3U

#define BBR_MIN_PIPE_CWND	4U
#define TCP_INIT_CWND		10U
#define CYCLE_LEN		8U

/*
 * Kernel tcp_bbr.c uses 10 seconds (bbr_min_rtt_win_sec = 10).
 * We use 2 seconds in this lab so packetdrill runs quickly.
 */
#define BBR_MIN_RTT_WIN_US	(2ULL * USEC_PER_SEC)
#define BBR_PROBE_RTT_MODE_US	(200ULL * USEC_PER_MSEC)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

static const __u32 bbr_pacing_gain[CYCLE_LEN] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
	BBR_UNIT, BBR_UNIT, BBR_UNIT,
};

struct bbr_ca {
	__u64 min_rtt_stamp;		/* tcp_mstamp when min_rtt_us was updated */
	__u64 probe_rtt_done_stamp;	/* end timestamp (usec) for BBR_PROBE_RTT */
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT in BBR_MIN_RTT_WIN_US window */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 prior_cwnd;		/* cwnd saved before PROBE_RTT */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  probe_rtt_round_done;	/* completed 1 round in BBR_PROBE_RTT? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode != BBR_PROBE_RTT)
		ca->prior_cwnd = tp->snd_cwnd;
	else
		ca->prior_cwnd = max(ca->prior_cwnd, tp->snd_cwnd);
}

static __always_inline void bbr_reset_mode(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (!ca->full_bw_reached) {
		ca->mode = BBR_STARTUP;
	} else {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) >> BW_SCALE;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	__u32 inflight = bbr_bdp(sk, bw, gain);

	return max(inflight, BBR_MIN_PIPE_CWND);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * 99;
	return rate >> BW_SCALE;
}

static __always_inline void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 rtt_us;
	__u64 bw;

	if (tp->srtt_us) {
		rtt_us = max(tp->srtt_us >> 3, 1U);
		ca->has_seen_rtt = 1;
	} else {
		rtt_us = USEC_PER_MSEC;
	}

	bw = div64_u64((__u64)tp->snd_cwnd * BW_UNIT, rtt_us);
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, BBR_HIGH_GAIN);
}

static __always_inline void bbr_set_pacing_rate(struct sock *sk, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 rate = bbr_rate_bytes_per_sec(sk, bw, gain);

	if (!ca->has_seen_rtt && tp->srtt_us)
		bbr_init_pacing_rate_from_rtt(sk);

	if (ca->full_bw_reached || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static __always_inline void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 bw;

	ca->round_start = 0;
	if (rs->delivered <= 0 || rs->interval_us <= 0)
		return;

	if (!before(rs->prior_delivered, ca->next_rtt_delivered)) {
		ca->next_rtt_delivered = tp->delivered;
		ca->rtt_cnt++;
		ca->round_start = 1;
	}

	bw = div64_u64((__u64)rs->delivered * BW_UNIT, rs->interval_us);
	if (!rs->is_app_limited || bw >= ca->max_bw)
		ca->max_bw = bw;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	bool is_full_length;
	__u32 inflight;

	if (ca->mode != BBR_PROBE_BW)
		return;

	is_full_length = (tp->delivered_mstamp - ca->cycle_mstamp) > ca->min_rtt_us;
	inflight = rs->prior_in_flight;

	if (ca->pacing_gain == BBR_UNIT) {
		if (!is_full_length)
			return;
	} else if (ca->pacing_gain > BBR_UNIT) {
		if (!is_full_length ||
		    (!rs->losses && inflight < bbr_inflight(sk, ca->max_bw, ca->pacing_gain)))
			return;
	} else {
		if (!is_full_length &&
		    inflight > bbr_inflight(sk, ca->max_bw, BBR_UNIT))
			return;
	}

	ca->cycle_idx = (ca->cycle_idx + 1) & (CYCLE_LEN - 1);
	ca->cycle_mstamp = tp->delivered_mstamp;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * BBR_FULL_BW_THRESH) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= BBR_FULL_BW_CNT)
		ca->full_bw_reached = 1;
}

static __always_inline void bbr_check_drain(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (ca->mode == BBR_STARTUP && ca->full_bw_reached) {
		ca->mode = BBR_DRAIN;
		tp->snd_ssthresh = bbr_inflight(sk, ca->max_bw, BBR_UNIT);
	}

	if (ca->mode == BBR_DRAIN &&
	    tcp_packets_in_flight(tp) <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)) {
		ca->mode = BBR_PROBE_BW;
		ca->cycle_idx = 0;
		ca->cycle_mstamp = tp->delivered_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us) {
		ca->min_rtt_us = rs->rtt_us;
		ca->min_rtt_stamp = tp->tcp_mstamp;
	}

	/*
	 * TODO: Implement BBR_PROBE_RTT logic and min_rtt window expiration:
	 * 1. Compute filter_expired = tp->tcp_mstamp > ca->min_rtt_stamp + BBR_MIN_RTT_WIN_US.
	 * 2. Update ca->min_rtt_us and ca->min_rtt_stamp when rs->rtt_us >= 0 and
	 *    either ((__u32)rs->rtt_us < ca->min_rtt_us) or (filter_expired && !rs->is_ack_delayed).
	 * 3. When filter_expired && ca->mode != BBR_PROBE_RTT, save cwnd (bbr_save_cwnd(sk)),
	 *    set ca->mode = BBR_PROBE_RTT, and reset ca->probe_rtt_done_stamp = 0.
	 * 4. While ca->mode == BBR_PROBE_RTT, maintain the probe phase:
	 *    - Set tp->app_limited = (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
	 *    - If !ca->probe_rtt_done_stamp && tcp_packets_in_flight(tp) <= BBR_MIN_PIPE_CWND:
	 *        ca->probe_rtt_done_stamp = tp->tcp_mstamp + BBR_PROBE_RTT_MODE_US;
	 *        ca->probe_rtt_round_done = 0;
	 *        ca->next_rtt_delivered = tp->delivered;
	 *    - Else if ca->probe_rtt_done_stamp:
	 *        if ca->round_start: ca->probe_rtt_round_done = 1;
	 *        if ca->probe_rtt_round_done && tp->tcp_mstamp > ca->probe_rtt_done_stamp:
	 *            ca->min_rtt_stamp = tp->tcp_mstamp;
	 *            tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	 *            bbr_reset_mode(sk);
	 */
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = BBR_HIGH_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = BBR_DRAIN_GAIN;
		ca->cwnd_gain = BBR_HIGH_GAIN;
		break;
	case BBR_PROBE_BW:
		ca->pacing_gain = bbr_pacing_gain[ca->cycle_idx & (CYCLE_LEN - 1)];
		ca->cwnd_gain = BBR_CWND_GAIN;
		break;
	case BBR_PROBE_RTT:
	default:
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = BBR_UNIT;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, __u32 acked, __u32 bw, __u32 gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd, target_cwnd;

	if (!acked)
		goto done;

	target_cwnd = bbr_inflight(sk, bw, gain);
	if (ca->full_bw_reached)
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, BBR_MIN_PIPE_CWND);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
	/* TODO: If ca->mode == BBR_PROBE_RTT, clamp tp->snd_cwnd to BBR_MIN_PIPE_CWND (4). */
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->probe_rtt_done_stamp = 0;
	ca->probe_rtt_round_done = 0;
	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;
	ca->min_rtt_stamp = tp->tcp_mstamp;

	ca->max_bw = 0;
	ca->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	ca->round_start = 0;
	ca->full_bw_reached = 0;
	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	ca->cycle_mstamp = 0;
	ca->cycle_idx = 0;
	ca->mode = BBR_STARTUP;
	ca->pacing_gain = BBR_HIGH_GAIN;
	ca->cwnd_gain = BBR_HIGH_GAIN;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr5_cong_control, struct sock *sk, __u32 ack, int flag,
	      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	bbr_update_bw(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);

	bbr_set_pacing_rate(sk, ca->max_bw, ca->pacing_gain);
	bbr_set_cwnd(sk, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_ssthresh, struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr5_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr5 = {
	.init		= (void *)bpf_bbr5_init,
	.cong_control	= (void *)bpf_bbr5_cong_control,
	.ssthresh	= (void *)bpf_bbr5_ssthresh,
	.undo_cwnd	= (void *)bpf_bbr5_undo_cwnd,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr5",
};
