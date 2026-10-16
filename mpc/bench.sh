#!/usr/bin/env bash
# CPU check on the Force itself, with the community toolkit's bench (its docs/BENCH.md has the PASS/WARN/FAIL rules):
#   mpc/bench.sh <force-ip> [bench args, e.g. -p -j]
# Cross-builds the bench (no Docker), copies it and the plugin to /tmp on the device over SSH, runs it pinned to core 1
# at normal priority (MPC's real-time audio threads always win, so MPC can keep running) and deletes both afterwards.
# Nothing is installed. Numbers from a PC or from qemu say nothing about the Force; this is the one that counts.
set -euo pipefail
cd "$(dirname "$0")/.."
IP="${1:?usage: mpc/bench.sh <force-ip> [bench args]}"
shift
MPC_VST="${MPC_VST:-$(pwd)/../mpc-vst-plugins}"
[ -f "$MPC_VST/tools/bench.c" ] || { echo "error: no mpc-vst-plugins checkout at $MPC_VST (see mpc/build.sh)" >&2; exit 1; }
make -s force
arm-linux-gnueabihf-gcc -O2 -Wall -o build/armhf/vstbench "$MPC_VST/tools/bench.c" -ldl -lm
scp -q build/armhf/vstbench build/armhf/sk_filterbank.so "root@$IP:/tmp/"
ssh "root@$IP" "taskset 2 /tmp/vstbench /tmp/sk_filterbank.so $*; r=\$?; rm -f /tmp/vstbench /tmp/sk_filterbank.so; exit \$r"
