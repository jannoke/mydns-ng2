#!/bin/bash
# Usage: build.sh [asan] [extra configure args...]
# Copies the read-only /src tree to /work, bootstraps, configures and builds it.
set -e
MODE=${1:-plain}; shift || true
rsync -a --delete --exclude=.git "${SRC:-/src}/" /work/
cd /work
./bootstrap.sh >/work/bootstrap.log 2>&1
if [ "$MODE" = asan ]; then
  export CFLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fsanitize-recover=address"
  export LDFLAGS="-fsanitize=address,undefined"
fi
./configure "$@" >/work/configure.log 2>&1 || { tail -30 /work/configure.log; exit 1; }
make -j"$(nproc)" >/work/make.log 2>&1 || { grep -E 'error|Error' /work/make.log | head -40; exit 1; }
grep -c 'warning:' /work/make.log | sed 's/^/warnings: /'
src/mydns/mydns --version 2>/dev/null | head -1
