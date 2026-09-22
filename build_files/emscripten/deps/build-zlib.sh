#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <source-dir> <build-dir> <install-prefix>" >&2
  exit 64
fi

source_dir=$1
build_dir=$2
install_prefix=$3
available_kb=$(df -Pk / | awk 'NR == 2 {print $4}')

if (( available_kb < 25 * 1024 * 1024 )); then
  echo "refusing build: less than 25 GiB available on /" >&2
  exit 75
fi

cmake -E make_directory "$build_dir"
cd "$build_dir"
CHOST=wasm32-unknown-emscripten CFLAGS="-pthread -fexceptions -O3 -DNDEBUG" emconfigure "$source_dir/configure" \
  --static \
  --prefix="$install_prefix"
nice -n 19 emmake make -j6 libz.a
nice -n 19 emmake make install
