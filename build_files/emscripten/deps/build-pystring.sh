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

cmake -E make_directory "$build_dir" "$install_prefix/lib" "$install_prefix/include"
nice -n 19 em++ -pthread -fexceptions -O3 -DNDEBUG -fPIC \
  -c "$source_dir/pystring.cpp" -o "$build_dir/pystring.o"
emar rc "$build_dir/libpystring.a" "$build_dir/pystring.o"
cmake -E copy_if_different "$build_dir/libpystring.a" "$install_prefix/lib/libpystring.a"
cmake -E copy_if_different "$source_dir/pystring.h" "$install_prefix/include/pystring.h"
