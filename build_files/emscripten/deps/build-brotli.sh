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
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_C_FLAGS="-pthread -fexceptions" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBROTLI_DISABLE_TESTS=ON

nice -n 19 cmake --build "$build_dir" --parallel 6
cmake -E make_directory "$install_prefix/lib" "$install_prefix/include"
cmake -E copy_if_different \
  "$build_dir/libbrotlicommon-static.a" \
  "$build_dir/libbrotlidec-static.a" \
  "$build_dir/libbrotlienc-static.a" \
  "$install_prefix/lib"
cmake -E copy_directory "$source_dir/c/include/brotli" "$install_prefix/include/brotli"
