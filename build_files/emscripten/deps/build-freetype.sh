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
  -DCMAKE_PREFIX_PATH="$install_prefix" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_C_FLAGS="-pthread -O3 -DNDEBUG" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions -O3 -DNDEBUG" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DZLIB_LIBRARY="$install_prefix/lib/libz.a" \
  -DZLIB_INCLUDE_DIR="$install_prefix/include" \
  -DBROTLIDEC_INCLUDE_DIRS="$install_prefix/include" \
  -DBROTLIDEC_LIBRARIES="$install_prefix/lib/libbrotlidec-static.a;$install_prefix/lib/libbrotlicommon-static.a" \
  -DFT_DISABLE_BZIP2=ON \
  -DFT_DISABLE_HARFBUZZ=ON \
  -DFT_DISABLE_PNG=ON \
  -DFT_REQUIRE_BROTLI=ON \
  -DFT_REQUIRE_ZLIB=ON

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
