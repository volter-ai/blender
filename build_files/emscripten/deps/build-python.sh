#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <Python-3.13.13-source> <build-root> <install-prefix>" >&2
  exit 64
fi

source_dir=$1
build_root=$2
install_prefix=$3
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
emscripten_dir=$(cd "$script_dir/.." && pwd)
native_build="$build_root/native"
native_prefix="$install_prefix/host-python"
wasm_build="$build_root/browser"
stage="$build_root/stage"
build_triple=$(/usr/bin/cc -dumpmachine 2>/dev/null || true)
if [[ -z "$build_triple" ]]; then
  build_triple=$("$source_dir/config.guess")
fi

available_kb=$(df -Pk / | awk 'NR == 2 {print $4}')
if (( available_kb < 25 * 1024 * 1024 )); then
  echo "refusing build: less than 25 GiB available on /" >&2
  exit 75
fi

mkdir -p "$native_build" "$wasm_build"
if [[ ! -f "$native_build/Makefile" ]]; then
  (cd "$native_build" && "$source_dir/configure" \
    --prefix="$native_prefix" --disable-shared --without-ensurepip)
fi
# Cross compilation runs several generated-code helpers through the native
# interpreter.  Build its complete standard profile rather than only the
# interpreter target, which can leave those helper modules unavailable.
nice -n 19 make -C "$native_build" -j6
nice -n 19 make -C "$native_build" install
build_python_candidates=("$native_prefix/bin/python3.13" "$native_prefix/bin/python3.13.exe")
build_python=""
for candidate in "${build_python_candidates[@]}"; do
  if [[ -x "$candidate" ]]; then
    if [[ -n "$build_python" ]]; then
      echo "more than one installed native build Python found" >&2
      exit 65
    fi
    build_python=$candidate
  fi
done
if [[ -z "$build_python" ]]; then
  echo "installed native build Python was not produced under $native_prefix/bin" >&2
  exit 65
fi
"$build_python" -c 'import sys; assert sys.version_info[:3] == (3, 13, 13)'

if [[ ! -f "$wasm_build/Makefile" ]]; then
  (
    cd "$wasm_build"
    CONFIG_SITE="$source_dir/Tools/wasm/config.site-wasm32-emscripten" \
    CFLAGS="-fexceptions -matomics -mbulk-memory -DPY_CALL_TRAMPOLINE" \
    CPPFLAGS="-matomics -mbulk-memory -DPY_CALL_TRAMPOLINE -I$install_prefix/include" \
    LDFLAGS="-fexceptions -L$install_prefix/lib" \
    LIBEXPAT_CFLAGS="-I$install_prefix/include" \
    LIBEXPAT_LDFLAGS="-L$install_prefix/lib -lexpat" \
      emconfigure "$source_dir/configure" -C \
        --host=wasm32-unknown-emscripten \
        --build="$build_triple" \
        --with-emscripten-target=browser \
        --with-build-python="$build_python" \
        --disable-shared --disable-ipv6 --with-system-expat \
        py_cv_module__sqlite3=n/a py_cv_module__bz2=n/a \
        py_cv_module__decimal=n/a py_cv_module__lzma=n/a
  )
fi

make_value()
{
  local key=$1
  awk -F= -v key="$key" '$1 == key {sub(/^[[:space:]]+/, "", $2); print $2; exit}' \
    "$wasm_build/Makefile"
}

for required_setting in \
  'CONFIGURE_CFLAGS=-fexceptions -matomics -mbulk-memory -DPY_CALL_TRAMPOLINE' \
  'MODULE_PYEXPAT_STATE=yes' \
  'MODULE__ELEMENTTREE_STATE=yes' \
  'MODULE__MULTIPROCESSING_STATE=n/a'; do
  setting_name=${required_setting%%=*}
  setting_value=${required_setting#*=}
  if [[ "$(make_value "$setting_name")" != "$setting_value" ]]; then
    echo "browser Python configure profile mismatch: $required_setting" >&2
    echo "use a fresh build root rather than reusing this cache" >&2
    exit 65
  fi
done
while IFS= read -r expected_state; do
  [[ -z "$expected_state" || "$expected_state" == \#* ]] && continue
  state_name=${expected_state%%=*}
  state_value=${expected_state#*=}
  if [[ "$(make_value "$state_name")" != "$state_value" ]]; then
    echo "browser Python module profile mismatch: $expected_state" >&2
    exit 65
  fi
done <"$emscripten_dir/python/browser-module-profile.txt"
nice -n 19 emmake make -C "$wasm_build" -j6

stage_python="$stage/python"
mkdir -p "$stage_python/include/python3.13" \
  "$stage_python/lib/python3.13"
cp "$wasm_build/libpython3.13.a" "$stage_python/lib/libpython3.13.a"
cp "$wasm_build/Modules/_hacl/libHacl_Hash_SHA2.a" \
  "$stage_python/lib/libHacl_Hash_SHA2.a"
cp -R "$source_dir/Include/." "$stage_python/include/python3.13/"
cp "$wasm_build/pyconfig.h" "$stage_python/include/python3.13/pyconfig.h"
cp -R "$source_dir/Lib/." "$stage_python/lib/python3.13/"
cp "$emscripten_dir/python/_multiprocessing.py" \
  "$stage_python/lib/python3.13/_multiprocessing.py"
mkdir -p "$stage_python/lib/python3.13/site-packages"
cp "$emscripten_dir/python/js.py" \
  "$stage_python/lib/python3.13/site-packages/js.py"
mkdir -p "$stage_python/lib/python3.13/site-packages/pyodide"
cp "$emscripten_dir/python/pyodide/"*.py \
  "$stage_python/lib/python3.13/site-packages/pyodide/"

sysconfig_file=$(find "$wasm_build/build" -type f -name '_sysconfigdata__emscripten_*.py' -print -quit)
if [[ -z "$sysconfig_file" ]]; then
  echo "target Emscripten sysconfig metadata was not generated" >&2
  exit 65
fi
cp "$sysconfig_file" "$stage_python/lib/python3.13/"

if [[ ! -d "$stage_python/lib/python3.13/test" ]]; then
  echo "expected CPython Lib/test before staging prune" >&2
  exit 65
fi
rm -rf "$stage_python/lib/python3.13/test"
find "$stage_python" -type d -name __pycache__ -prune -exec rm -rf {} +
find "$stage_python" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete

mkdir -p "$install_prefix/python"
rsync -a --delete "$stage_python/" "$install_prefix/python/"

llvm_nm=${EMSDK:?source emsdk_env.sh first}/upstream/bin/llvm-nm
nm_output="$build_root/libpython.symbols"
"$llvm_nm" "$install_prefix/python/lib/libpython3.13.a" >"$nm_output"
grep '_PyEM_TrampolineCall_Reflection' "$nm_output" >/dev/null
grep '__em_js___PyEM_TrampolineCall_JavaScript' "$nm_output" >/dev/null
"$llvm_nm" "$install_prefix/python/lib/libHacl_Hash_SHA2.a" \
  >"$build_root/libhacl.symbols"
grep 'python_hashlib_Hacl_Hash_SHA2' "$build_root/libhacl.symbols" >/dev/null
