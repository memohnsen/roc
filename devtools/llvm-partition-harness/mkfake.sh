#!/usr/bin/env bash
# mkfake.sh <variant> <objdir-or-archive>: a fake roc that hands out a prepared archive.
set -e
W=~/dev/roc-llvm-work
v=$1; src=$2
d=$W/fake/$v; rm -rf $d; mkdir -p $d
if [ -d "$src" ]; then
  (cd $d && ar x $W/runs/exp1-dump.a roc_static_data_x64glibc.o)
  ar rcs $d/archive.a $src/obj.*.o $d/roc_static_data_x64glibc.o
  rm $d/roc_static_data_x64glibc.o
else
  cp "$src" $d/archive.a
fi
cat > $d/roc <<'EOS'
#!/usr/bin/env bash
if [ "$1" = version ]; then echo "Roc compiler version fake"; exit 0; fi
for a in "$@"; do case $a in --output=*) out=${a#--output=};; esac; done
cp "$(dirname "$0")/archive.a" "$out"
EOS
chmod +x $d/roc
ar t $d/archive.a | tr '\n' ' '; echo
