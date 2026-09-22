#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <source-dir> <build-dir> <install-prefix>" >&2
  exit 64
fi

source_dir=$(cd "$1" && pwd)
build_dir=$2
install_prefix=$3
available_kb=$(df -Pk / | awk 'NR == 2 {print $4}')

if (( available_kb < 25 * 1024 * 1024 )); then
  echo "refusing build: less than 25 GiB available on /" >&2
  exit 75
fi

mkdir -p "$build_dir"
cd "$build_dir"

nice -n 19 emconfigure "$source_dir/configure" \
  --prefix="$install_prefix" \
  --host=wasm32-unknown-emscripten \
  --enable-static \
  --disable-shared \
  --enable-cxx \
  --disable-assembly \
  CFLAGS="-pthread -O2" \
  CXXFLAGS="-pthread -fexceptions -O2" \
  CC_FOR_BUILD=/usr/bin/cc \
  HOST_CC=/usr/bin/cc

nice -n 19 emmake make -j6
nice -n 19 emmake make install
