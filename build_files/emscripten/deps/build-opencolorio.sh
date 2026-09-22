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
  -DCMAKE_PREFIX_PATH="$install_prefix" \
  -DCMAKE_FIND_ROOT_PATH="$install_prefix" \
  -DCMAKE_C_FLAGS="-pthread -fexceptions" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DOCIO_BUILD_APPS=OFF \
  -DOCIO_BUILD_PYTHON=OFF \
  -DOCIO_BUILD_NUKE=OFF \
  -DOCIO_BUILD_JAVA=OFF \
  -DOCIO_BUILD_DOCS=OFF \
  -DOCIO_BUILD_FROZEN_DOCS=OFF \
  -DOCIO_BUILD_TESTS=OFF \
  -DOCIO_BUILD_GPU_TESTS=OFF \
  -DOCIO_USE_SIMD=OFF \
  -DOCIO_USE_SSE=OFF \
  -DOCIO_INSTALL_EXT_PACKAGES=NONE \
  -DImath_DIR="$install_prefix/lib/cmake/Imath" \
  -Dexpat_DIR="$install_prefix/lib/cmake/expat-2.7.5" \
  -Dexpat_ROOT="$install_prefix" \
  -Dyaml-cpp_DIR="$install_prefix/lib/cmake/yaml-cpp" \
  -Dyaml-cpp_VERSION=0.8.0 \
  -Dpystring_ROOT="$install_prefix" \
  -Dpystring_INCLUDE_DIR="$install_prefix/include" \
  -Dpystring_LIBRARY="$install_prefix/lib/libpystring.a" \
  -Dminizip-ng_ROOT="$install_prefix" \
  -Dminizip-ng_INCLUDE_DIR="$install_prefix/include/minizip-ng/minizip" \
  -Dminizip-ng_LIBRARY="$install_prefix/lib/libminizip.a" \
  -Dminizip_LIBRARY="$install_prefix/lib/libminizip.a" \
  -DZLIB_ROOT="$install_prefix" \
  -DZLIB_INCLUDE_DIR="$install_prefix/include" \
  -DZLIB_LIBRARY="$install_prefix/lib/libz.a"

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
