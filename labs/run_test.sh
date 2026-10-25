#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Usage:
#   ./run_test.sh                          # Run all solution.bpf.o tests in one vng VM
#   ./run_test.sh cubic/step01-reno        # Run solution.bpf.o for one step
#   ./run_test.sh cubic/step01-reno starter # Run starter.bpf.o for one step

set -euo pipefail

LAB_DIR="$(cd "$(dirname "$0")" && pwd)"
CLANG="${CLANG:-/usr/local/google/home/kuniyu/kernel/tools/llvm-project/build/bin/clang}"
PACKETDRILL="${PACKETDRILL:-/usr/local/google/home/kuniyu/kernel/tools/packetdrill/gtests/net/packetdrill/packetdrill}"
KERNEL_REL="${KERNEL_REL:-$(uname -r)}"

if [[ ! -x "$CLANG" ]]; then
	CLANG="$(command -v clang)"
fi
if [[ ! -x "$PACKETDRILL" ]]; then
	PACKETDRILL="$(command -v packetdrill)"
fi

if [[ ! -f "$LAB_DIR/include/vmlinux.h" ]]; then
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$LAB_DIR/include/vmlinux.h"
fi

STEP_FILTER="${1:-all}"
VARIANT="${2:-solution}"

if [[ "$STEP_FILTER" == "all" ]]; then
	mapfile -t STEPS < <(find "$LAB_DIR/reno" "$LAB_DIR/cubic" "$LAB_DIR/bbr" -mindepth 1 -maxdepth 1 -type d | sort)
else
	STEPS=("$LAB_DIR/${STEP_FILTER#labs/}")
fi

for step in "${STEPS[@]}"; do
	src="$step/${VARIANT}.bpf.c"
	obj="$step/${VARIANT}.bpf.o"
	if [[ -f "$src" ]]; then
		"$CLANG" -g -O2 -target bpf -Wno-missing-declarations \
			-I"$LAB_DIR/include" -c "$src" -o "$obj"
	fi
done

flock /tmp/bpf_cc_lab_vng.lock vng -r "$KERNEL_REL" -- bash -s "$LAB_DIR" "$PACKETDRILL" "$VARIANT" "${STEPS[@]}" <<'VNG_EOF'
set -euo pipefail
LAB_DIR="$1"
PACKETDRILL="$2"
VARIANT="$3"
shift 3

modprobe tun

for step in "$@"; do
	rel="${step#$LAB_DIR/}"
	obj="$step/${VARIANT}.bpf.o"
	pkt="$step/test.pkt"
	if [[ ! -f "$obj" || ! -f "$pkt" ]]; then
		echo "[SKIP] $rel ($VARIANT)"
		continue
	fi
	echo "=== [RUN] $rel ($VARIANT) ==="
	bpftool struct_ops register "$obj"
	if unshare -n bash -c "
		sysctl -q net.ipv4.tcp_timestamps=1
		sysctl -q net.ipv4.tcp_sack=1
		sysctl -q net.ipv4.tcp_dsack=1
		sysctl -q net.ipv4.tcp_slow_start_after_idle=0
		sysctl -q net.ipv4.tcp_Wait=0 2>/dev/null || true
		\"$PACKETDRILL\" --ip_version=ipv4 --tolerance_usecs=100000 \"$pkt\"
	"; then
		echo "=== [PASS] $rel ($VARIANT) ==="
	else
		bpftool struct_ops unregister "$obj" >/dev/null 2>&1 || true
		exit 1
	fi
	bpftool struct_ops unregister "$obj" >/dev/null 2>&1 || true
done
VNG_EOF
