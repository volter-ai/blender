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

PKG_CONFIG_PATH= \
PKG_CONFIG_LIBDIR="$install_prefix/lib/pkgconfig:$install_prefix/share/pkgconfig" \
emcmake cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_C_FLAGS="-pthread -O3 -DNDEBUG" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions -O3 -DNDEBUG" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DOJPH_DISABLE_SIMD=ON \
  -DOJPH_BUILD_EXECUTABLES=OFF \
  -DOJPH_BUILD_TESTS=OFF \
  -DOJPH_ENABLE_TIFF_SUPPORT=OFF

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
