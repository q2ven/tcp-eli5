// SPDX-License-Identifier: GPL-2.0
/*
 * BBRv1 Step 4: PROBE_BW phase cycling (8-phase gain cycle)
 *
 * Steady-state bandwidth probing using an 8-phase gain cycle:
 *   Phase 0: 1.25x (probe for more available bandwidth)
 *   Phase 1: 0.75x (drain excess queue created in phase 0)
 *   Phases 2..7: 1.0x (cruise at estimated bandwidth)
 */
#include "bpf_cc_helpers.h"

char _license[] SEC("license") = "GPL";

#define BW_SCALE			24
#define BW_UNIT				(1ULL << BW_SCALE)

#define BBR_SCALE			8
#define BBR_UNIT			(1 << BBR_SCALE)

#define CYCLE_LEN			8
#define TCP_INIT_CWND			10

static const int bbr_pacing_margin_percent = 1;
static const int bbr_high_gain = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain = BBR_UNIT * 2;
static const int bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const int bbr_full_bw_cnt = 3;
static const int bbr_cwnd_min_target = 4;

static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,
	BBR_UNIT * 3 / 4,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
	BBR_UNIT,
};

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
};

struct bbr_ca {
	__u64 cycle_mstamp;		/* start timestamp of current PROBE_BW phase */
	__u32 min_rtt_us;		/* min RTT seen */
	__u32 max_bw;			/* estimated bottleneck bw (pkts/us << 24) */
	__u32 rtt_cnt;			/* packet-timed rounds elapsed */
	__u32 next_rtt_delivered;	/* tp->delivered at end of current round */
	__u32 full_bw;			/* baseline bw for STARTUP full-pipe check */
	__u32 full_bw_cnt;		/* rounds without 25% bw growth */
	__u32 mode;			/* enum bbr_mode */
	__u32 pacing_gain;		/* current pacing gain */
	__u32 cwnd_gain;		/* current cwnd gain */
	__u32 cycle_idx;		/* index in bbr_pacing_gain[] (0..7) */
	__u8  round_start;		/* start of a new packet-timed round? */
	__u8  full_bw_reached;		/* reached full pipe bandwidth in STARTUP? */
	__u8  has_seen_rtt;		/* seen at least one RTT sample? */
};

static __always_inline __u32 bbr_bdp(const struct sock *sk, __u32 bw, __u32 gain)
{
	const struct bbr_ca *ca = inet_csk_ca(sk);
	__u64 w;

	if (ca->min_rtt_us == ~0U || !bw)
		return TCP_INIT_CWND;

	w = (__u64)bw * ca->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static __always_inline __u32 bbr_inflight(const struct sock *sk, __u32 bw, __u32 gain)
{
	return bbr_bdp(sk, bw, gain);
}

static __always_inline __u64 bbr_rate_bytes_per_sec(struct sock *sk, __u64 rate, __u32 gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache ? : 1460;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
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
	sk->sk_pacing_rate = bbr_rate_bytes_per_sec(sk, bw, bbr_high_gain);
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

static __always_inline bool bbr_is_next_cycle_phase(struct sock *sk,
						    const struct rate_sample *rs)
{
	/*
	 * TODO 1: PROBE_BW のフェーズ終了条件を判定する
	 *
	 * 1. フェーズ開始からの経過時間 (tp->tcp_mstamp - ca->cycle_mstamp) が
	 *    ca->min_rtt_us を超えているか (is_full_length) を判定する。
	 * 2. ca->pacing_gain == BBR_UNIT (巡航フェーズ 2..7) の場合:
	 *    is_full_length を返す (経過時間のみで判定)。
	 * 3. ca->pacing_gain > BBR_UNIT (探索フェーズ 0, 1.25x) の場合:
	 *    is_full_length かつ、ロス発生 (rs->losses) または
	 *    rs->prior_in_flight >= bbr_inflight(sk, ca->max_bw, ca->pacing_gain)
	 *    に達していれば true を返す。
	 * 4. ca->pacing_gain < BBR_UNIT (排出フェーズ 1, 0.75x) の場合:
	 *    is_full_length または、キューが抜けて
	 *    rs->prior_in_flight <= bbr_inflight(sk, ca->max_bw, BBR_UNIT)
	 *    に下がっていれば true を返す。
	 */
	(void)sk;
	(void)rs;
	return false;
}

static __always_inline void bbr_advance_cycle_phase(struct sock *sk)
{
	/*
	 * TODO 2: 次のフェーズへ進める
	 *
	 * 1. ca->cycle_idx を 1 つ進め、CYCLE_LEN (8) でラップさせる (& (CYCLE_LEN - 1))。
	 * 2. ca->cycle_mstamp に現在のタイムスタンプ tp->tcp_mstamp を記録する。
	 */
	(void)sk;
}

static __always_inline void bbr_update_cycle_phase(struct sock *sk,
						   const struct rate_sample *rs)
{
	/*
	 * TODO 3: PROBE_BW モードにおけるフェーズ更新
	 *
	 * ca->mode == BBR_PROBE_BW かつ bbr_is_next_cycle_phase(sk, rs) が真なら
	 * bbr_advance_cycle_phase(sk) を呼び出す。
	 */
	(void)sk;
	(void)rs;
}

static __always_inline void bbr_check_full_bw_reached(struct sock *sk,
						      const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);
	__u32 bw_thresh;

	if (ca->full_bw_reached || !ca->round_start || rs->is_app_limited)
		return;

	bw_thresh = ((__u64)ca->full_bw * bbr_full_bw_thresh) >> BBR_SCALE;
	if (ca->max_bw >= bw_thresh) {
		ca->full_bw = ca->max_bw;
		ca->full_bw_cnt = 0;
		return;
	}

	if (++ca->full_bw_cnt >= bbr_full_bw_cnt)
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
		ca->cycle_mstamp = tp->tcp_mstamp;
	}
}

static __always_inline void bbr_update_min_rtt(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	if (rs->rtt_us >= 0 && (__u32)rs->rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rs->rtt_us;
}

static __always_inline void bbr_update_gains(struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	switch (ca->mode) {
	case BBR_STARTUP:
		ca->pacing_gain = bbr_high_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_DRAIN:
		ca->pacing_gain = bbr_drain_gain;
		ca->cwnd_gain = bbr_high_gain;
		break;
	case BBR_PROBE_BW:
	default:
		/*
		 * TODO 4: PROBE_BW での pacing_gain を bbr_pacing_gain[] から設定する
		 *
		 * ca->cycle_idx (0..7) に応じて bbr_pacing_gain[ca->cycle_idx & 7] を設定する。
		 * starter では暫定的にフラットな BBR_UNIT (1.0x) を設定している。
		 */
		ca->pacing_gain = BBR_UNIT;
		ca->cwnd_gain = bbr_cwnd_gain;
		break;
	}
}

static __always_inline void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
					 __u32 acked, __u32 bw, __u32 gain)
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
	cwnd = max(cwnd, (__u32)bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr_ca *ca = inet_csk_ca(sk);

	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	ca->rtt_cnt = 0;
	ca->next_rtt_delivered = tp->delivered;

	ca->min_rtt_us = tp->rtt_min.s[0].v ? : ~0U;

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
	ca->pacing_gain = bbr_high_gain;
	ca->cwnd_gain = bbr_high_gain;

	sk->sk_pacing_status = SK_PACING_NEEDED;
}

SEC("struct_ops")
void BPF_PROG(bpf_bbr4_main, struct sock *sk, __u32 ack, int flag,
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
	bbr_set_cwnd(sk, rs, rs->acked_sacked, ca->max_bw, ca->cwnd_gain);
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_sndbuf_expand, struct sock *sk)
{
	return 3;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_undo_cwnd, struct sock *sk)
{
	struct bbr_ca *ca = inet_csk_ca(sk);

	ca->full_bw = 0;
	ca->full_bw_cnt = 0;
	return tcp_sk(sk)->snd_cwnd;
}

SEC("struct_ops")
__u32 BPF_PROG(bpf_bbr4_ssthresh, struct sock *sk)
{
	return tcp_sk(sk)->snd_ssthresh;
}

SEC(".struct_ops")
struct tcp_congestion_ops bpf_bbr4 = {
	.init		= (void *)bpf_bbr4_init,
	.cong_control	= (void *)bpf_bbr4_main,
	.sndbuf_expand	= (void *)bpf_bbr4_sndbuf_expand,
	.undo_cwnd	= (void *)bpf_bbr4_undo_cwnd,
	.ssthresh	= (void *)bpf_bbr4_ssthresh,
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bpf_bbr4",
};
