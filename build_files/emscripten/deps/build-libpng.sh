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

emcmake cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_PREFIX_PATH="$install_prefix" \
  -DCMAKE_C_FLAGS="-pthread -fexceptions" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DZLIB_ROOT="$install_prefix" \
  -DZLIB_LIBRARY="$install_prefix/lib/libz.a" \
  -DZLIB_INCLUDE_DIR="$install_prefix/include" \
  -DPNG_SHARED=OFF \
  -DPNG_STATIC=ON \
  -DPNG_TESTS=OFF \
  -DPNG_TOOLS=OFF \
  -DPNG_EXECUTABLES=OFF \
  -DPNG_HARDWARE_OPTIMIZATIONS=OFF

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
