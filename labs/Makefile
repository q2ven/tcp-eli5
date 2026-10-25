CLANG ?= /usr/local/google/home/kuniyu/kernel/tools/llvm-project/build/bin/clang
ifeq ($(wildcard $(CLANG)),)
CLANG := clang
endif

BPF_CFLAGS := -g -O2 -target bpf -Wno-missing-declarations -Iinclude

SRCS := $(wildcard 01-reno/step*/*.bpf.c 02-cubic/step*/*.bpf.c 03-bbr/step*/*.bpf.c 04-qdisc-fifo/step*/*.bpf.c 05-qdisc-fq/step*/*.bpf.c 06-qdisc-fq-codel/step*/*.bpf.c)
OBJS := $(SRCS:.bpf.c=.bpf.o)

.PHONY: all check clean vmlinux

all: include/vmlinux.h $(OBJS)

include/vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

%.bpf.o: %.bpf.c include/vmlinux.h include/bpf_cc_helpers.h include/bpf_qdisc_helpers.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

check: all
	./run_test.sh all solution

clean:
	rm -f $(OBJS)
