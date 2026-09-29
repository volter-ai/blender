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

# Source-owned dependency change; refuse an unexpected upstream revision.
patch_file="$(dirname "$0")/opensubdiv-release-stencils.patch"
if patch -d "$source_dir" -p1 --dry-run --forward < "$patch_file" >/dev/null 2>&1; then
  patch -d "$source_dir" -p1 --forward < "$patch_file"
else
  patch -d "$source_dir" -p1 --dry-run --reverse < "$patch_file" >/dev/null
fi

emcmake cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_FLAGS="-pthread -fexceptions -Wno-unused-command-line-argument" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions -Wno-unused-command-line-argument" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DNO_LIB=OFF \
  -DNO_EXAMPLES=ON \
  -DNO_TUTORIALS=ON \
  -DNO_REGRESSION=ON \
  -DNO_PTEX=ON \
  -DNO_DOC=ON \
  -DNO_OMP=ON \
  -DNO_TBB=OFF \
  -DNO_CUDA=ON \
  -DNO_OPENCL=ON \
  -DNO_CLEW=ON \
  -DNO_OPENGL=ON \
  -DOSD_PATCH_SHADER_SOURCE_GLSL=ON \
  -DNO_METAL=ON \
  -DNO_DX=ON \
  -DNO_TESTS=ON \
  -DNO_GLTESTS=ON \
  -DNO_GLEW=ON \
  -DNO_GLFW=ON \
  -DNO_GLFW_X11=ON \
  -DTBB_DIR="${TBB_DIR:-$install_prefix/lib/cmake/TBB}"

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
