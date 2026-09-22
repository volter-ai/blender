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
  -DCMAKE_FIND_ROOT_PATH="$install_prefix" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_FLAGS="-pthread -O2" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions -O2" \
  -DBUILD_SHARED_LIBS=OFF \
  -DMANIFOLD_JSBIND=OFF \
  -DMANIFOLD_CBIND=OFF \
  -DMANIFOLD_PYBIND=OFF \
  -DMANIFOLD_PAR=ON \
  -DMANIFOLD_CROSS_SECTION=OFF \
  -DMANIFOLD_EXPORT=OFF \
  -DMANIFOLD_DEBUG=OFF \
  -DMANIFOLD_TEST=OFF \
  -DMANIFOLD_DOWNLOADS=OFF \
  -DTRACY_ENABLE=OFF \
  -DTBB_ROOT="$install_prefix" \
  -DTBB_DIR="$install_prefix/lib/cmake/TBB"

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
