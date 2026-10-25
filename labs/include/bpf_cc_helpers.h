/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BPF_CC_HELPERS_H
#define __BPF_CC_HELPERS_H

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define clamp(val, lo, hi) min(max(val, lo), hi)

#define TCP_INFINITE_SSTHRESH	0x7fffffff
#define TCP_CONG_NON_RESTRICTED	0x1
#define USEC_PER_MSEC		1000ULL
#define USEC_PER_SEC		1000000ULL
#define NSEC_PER_USEC		1000ULL

static __always_inline bool before(__u32 seq1, __u32 seq2)
{
	return (__s32)(seq1 - seq2) < 0;
}
#define after(seq2, seq1) before(seq1, seq2)

static __always_inline struct inet_connection_sock *inet_csk(const struct sock *sk)
{
	return (struct inet_connection_sock *)sk;
}

static __always_inline void *inet_csk_ca(const struct sock *sk)
{
	return (void *)inet_csk(sk)->icsk_ca_priv;
}

static __always_inline struct tcp_sock *tcp_sk(const struct sock *sk)
{
	return (struct tcp_sock *)sk;
}

static __always_inline bool tcp_in_slow_start(const struct tcp_sock *tp)
{
	return tp->snd_cwnd < tp->snd_ssthresh;
}

static __always_inline bool tcp_is_cwnd_limited(const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return !!tp->is_cwnd_limited;
}

static __always_inline __u32 tcp_left_out(const struct tcp_sock *tp)
{
	return tp->sacked_out + tp->lost_out;
}

static __always_inline __u32 tcp_packets_in_flight(const struct tcp_sock *tp)
{
	return tp->packets_out - tcp_left_out(tp) + tp->retrans_out;
}

static __always_inline __u64 div64_u64(__u64 dividend, __u64 divisor)
{
	return divisor ? dividend / divisor : 0;
}

#endif /* __BPF_CC_HELPERS_H */
