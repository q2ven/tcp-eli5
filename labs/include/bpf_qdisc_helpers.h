/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BPF_QDISC_HELPERS_H
#define __BPF_QDISC_HELPERS_H

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef clamp
#define clamp(val, lo, hi) min(max(val, lo), hi)
#endif

#define NET_XMIT_SUCCESS	0x00
#define NET_XMIT_DROP		0x01
#define NET_XMIT_CN		0x02

#define TC_PRIO_CONTROL		7
#define TC_PRIO_MAX		15

#define NSEC_PER_USEC		1000ULL
#define NSEC_PER_MSEC		1000000ULL
#define NSEC_PER_SEC		1000000000ULL

#ifndef __kptr
#define __kptr __attribute__((btf_type_tag("kptr")))
#endif

#ifndef __contains
#define __contains(name, node) __attribute__((btf_decl_tag("contains:" #name ":" #node)))
#endif

#ifndef private
#define private(name) SEC(".data." #name) __hidden __attribute__((aligned(8)))
#endif

#ifndef offsetof
#define offsetof(type, member) ((unsigned long)&((type *)0)->member)
#endif

#ifndef container_of
#define container_of(ptr, type, member)				\
	({							\
		void *__mptr = (void *)(ptr);			\
		((type *)(__mptr - offsetof(type, member)));	\
	})
#endif

struct bpf_sk_buff_ptr;

static __always_inline struct qdisc_skb_cb *qdisc_skb_cb(const struct sk_buff *skb)
{
	return (struct qdisc_skb_cb *)skb->cb;
}

static __always_inline unsigned int qdisc_pkt_len(const struct sk_buff *skb)
{
	return qdisc_skb_cb(skb)->pkt_len;
}

/* Qdisc kfuncs */
extern void bpf_qdisc_skb_drop(struct sk_buff *skb,
			       struct bpf_sk_buff_ptr *to_free_list) __weak __ksym;
extern void bpf_qdisc_bstats_update(struct Qdisc *sch,
				    const struct sk_buff *skb) __weak __ksym;
extern void bpf_qdisc_watchdog_schedule(struct Qdisc *sch, u64 expire,
					u64 delta_ns) __weak __ksym;
extern u32 bpf_skb_get_hash(struct sk_buff *skb) __weak __ksym;
extern void bpf_kfree_skb(struct sk_buff *skb) __weak __ksym;

/* Graph data structure (kptr / bpf_list / bpf_rbtree) kfuncs */
extern void *bpf_obj_new_impl(u64 local_type_id__k, void *meta__ign) __weak __ksym;
extern void bpf_obj_drop_impl(void *p__alloc, void *meta__ign) __weak __ksym;
extern void *bpf_refcount_acquire_impl(void *p__refcounted_kptr,
				       void *meta__ign) __weak __ksym;
extern int bpf_list_push_front_impl(struct bpf_list_head *head,
				    struct bpf_list_node *node,
				    void *meta__ign, u64 off) __weak __ksym;
extern int bpf_list_push_back_impl(struct bpf_list_head *head,
				   struct bpf_list_node *node,
				   void *meta__ign, u64 off) __weak __ksym;
extern struct bpf_list_node *bpf_list_pop_front(struct bpf_list_head *head) __weak __ksym;
extern struct bpf_list_node *bpf_list_pop_front_impl(struct bpf_list_head *head) __weak __ksym;
extern int bpf_rbtree_add_impl(struct bpf_rb_root *root, struct bpf_rb_node *node,
			       bool (*less)(struct bpf_rb_node *, const struct bpf_rb_node *),
			       void *meta__ign, u64 off) __weak __ksym;
extern struct bpf_rb_node *bpf_rbtree_remove(struct bpf_rb_root *root,
					     struct bpf_rb_node *node) __weak __ksym;
extern struct bpf_rb_node *bpf_rbtree_first(struct bpf_rb_root *root) __weak __ksym;

#define bpf_obj_new(type) ((type *)bpf_obj_new_impl(bpf_core_type_id_local(type), NULL))
#define bpf_obj_drop(kptr) bpf_obj_drop_impl(kptr, NULL)
#define bpf_refcount_acquire(kptr) bpf_refcount_acquire_impl(kptr, NULL)
#define bpf_list_push_front(head, node) bpf_list_push_front_impl(head, node, NULL, 0)
#define bpf_list_push_back(head, node) bpf_list_push_back_impl(head, node, NULL, 0)
#define bpf_list_pop_front(head) bpf_list_pop_front(head)
#define bpf_rbtree_add(head, node, less) bpf_rbtree_add_impl(head, node, (void *)less, NULL, 0)

struct bpf_iter_num;
extern int bpf_iter_num_new(struct bpf_iter_num *it, int start, int end) __weak __ksym;
extern int *bpf_iter_num_next(struct bpf_iter_num *it) __weak __ksym;
extern void bpf_iter_num_destroy(struct bpf_iter_num *it) __weak __ksym;

#ifndef bpf_for
#define bpf_for(i, start, end) for (								\
	struct bpf_iter_num ___it __attribute__((aligned(8),					\
						 cleanup(bpf_iter_num_destroy))),		\
			    *___p __attribute__((unused)) = (					\
				bpf_iter_num_new(&___it, (start), (end)),			\
				(void)bpf_iter_num_destroy, (void *)0);				\
	({											\
		int *___t = bpf_iter_num_next(&___it);						\
		(___t && ((i) = *___t, (i) >= (start) && (i) < (end)));				\
	});											\
)
#endif /* bpf_for */

#endif /* __BPF_QDISC_HELPERS_H */
