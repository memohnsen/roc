#!/usr/bin/env bash
set -e
export PATH=$HOME/.local/zig-x86_64-linux-0.17.0:$PATH
W=~/dev/roc-llvm-work
cd $W/harness
L=$W/llvm22/lib
libs=$(ls $L/libLLVM*.a | tr '\n' ' ')
nice zig c++ -target x86_64-linux-musl -O2 -std=c++17 -fno-exceptions -fno-rtti -DNDEBUG -I$W/llvm22/include pcg.cpp \
  -Wl,--start-group $libs $L/libz.a $L/libzstd.a -Wl,--end-group -o pcg 2>&1 | grep -v "warning" | head -40
ls -la pcg
