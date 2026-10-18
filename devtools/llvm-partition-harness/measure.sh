#!/usr/bin/env bash
# measure.sh <roc-binary> <label> [extra env assignments...]
# One --opt=speed build of the stock UI, alone under the heavy-build lock.
set -u
roc=$1; label=$2; shift 2
W=~/dev/roc-llvm-work
mkdir -p $W/runs ~/.cache/atlas-tmp
log=$W/runs/$label.log
rm -rf $W/cache-$label
start=$(date +%s)
env "$@" ROC_CACHE_DIR=$W/cache-$label TMPDIR=$HOME/.cache/atlas-tmp \
  flock ~/.cache/atlas-heavy-build.lock \
  systemd-run --user --scope --quiet -p MemoryMax=20G -p MemorySwapMax=0 -p CPUWeight=30 -- \
  python3 -I $W/timeit.py $roc build --target=x64glibc --opt=speed --no-cache --jobs=${JOBS:-2} --timings \
    --output=$W/runs/$label.a $W/ui-main/desktop/ui/app/main.roc > $log 2>&1
echo "$label: $(grep -o 'TIMEIT.*' $log) total_incl_lock=$(( $(date +%s) - start ))s"
rm -rf $W/cache-$label
