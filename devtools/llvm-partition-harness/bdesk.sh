#!/usr/bin/env bash
# bdesk.sh <variant>: build the perf-harness desktop binary against fake/<variant>.
set -u
W=~/dev/roc-llvm-work
v=$1
cd $W/atlas
start=$(date +%s)
ATLAS_ROC=$W/fake/$v/roc CARGO_BUILD_JOBS=4 TMPDIR=$HOME/.cache/atlas-tmp \
  flock ~/.cache/atlas-heavy-build.lock \
  systemd-run --user --scope --quiet -p MemoryMax=20G -p MemorySwapMax=0 -p CPUWeight=30 -- \
  mise exec -- cargo build --profile desktop-dev -p atlas-desktop --features perf-harness > $W/fake/$v/cargo.log 2>&1
rc=$?
cp target/desktop-dev/atlas-desktop $W/fake/$v/atlas-desktop 2>/dev/null
echo "bdesk $v rc=$rc $(( $(date +%s) - start ))s"
tail -3 $W/fake/$v/cargo.log
