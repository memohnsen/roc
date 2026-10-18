#!/usr/bin/env bash
# hrun.sh <label> <mode> <threads> [env...]: harness run under the heavy lock.
set -u
label=$1; mode=$2; threads=$3; shift 3
W=~/dev/roc-llvm-work
mkdir -p $W/h/$label
env "$@" TMPDIR=$HOME/.cache/atlas-tmp flock ~/.cache/atlas-heavy-build.lock \
  systemd-run --user --scope --quiet -p MemoryMax=20G -p MemorySwapMax=0 -p CPUWeight=30 -- \
  python3 -I $W/timeit.py $W/harness/pcg $W/preopt.bc $W/h/$label/obj $mode $threads > $W/h/$label.log 2>&1
size $W/h/$label/obj.*.o | awk 'NR>1{t+=$1;d+=$2;b+=$3} END{print "text="t" data="d" bss="b}' >> $W/h/$label.log
echo "$label: $(grep -o 'TIMEIT.*' $W/h/$label.log) $(tail -1 $W/h/$label.log)"
