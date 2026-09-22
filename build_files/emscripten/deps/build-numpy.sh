#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <numpy-2.3.4-source> <build-root> <install-prefix>" >&2
  exit 64
fi

source_dir=$1
build_root=$2
install_prefix=$3
native_python="$install_prefix/host-python/bin/python3.13"
target_python="$install_prefix/python"
build_dir="$build_root/build"
tool_venv="$build_root/buildvenv"
cross_dir="$build_root/crossenv"

available_kb=$(df -Pk / | awk 'NR == 2 {print $4}')
if (( available_kb < 25 * 1024 * 1024 )); then
  echo "refusing build: less than 25 GiB available on /" >&2
  exit 75
fi
if [[ ! -x "$native_python" ]]; then
  echo "run build-python.sh first; native Python helper is missing" >&2
  exit 65
fi

mkdir -p "$build_root" "$cross_dir"
if [[ ! -x "$tool_venv/bin/python" ]]; then
  "$native_python" -m venv "$tool_venv"
fi
"$tool_venv/bin/python" -m pip install \
  --disable-pip-version-check 'Cython==3.3.0'
# Meson discovers the Cython compiler as a build-machine program before it
# consults target compiler metadata.  Make the pinned tool visible through
# normal program discovery as well as recording it in the cross file below.
export PATH="$tool_venv/bin:$PATH"

# NumPy 2.3.4 intentionally ships its CPU-dispatch `features` module in a
# vendored Meson fork and selects it in pyproject.toml.  Stock Meson 1.8.3 has
# the same version number but lacks that module, so follow NumPy's upstream
# build entry point instead of substituting the stock package.
meson=("$tool_venv/bin/python" "$source_dir/vendored-meson/meson/meson.py")
if [[ "$("${meson[@]}" --version)" != 1.8.3 ]]; then
  echo "NumPy's vendored Meson is not the required 1.8.3" >&2
  exit 65
fi

sysconfig_file=$(find "$target_python/lib/python3.13" -maxdepth 1 -type f \
  -name '_sysconfigdata__emscripten_*.py' -print -quit)
if [[ -z "$sysconfig_file" ]]; then
  echo "target CPython sysconfig metadata is missing" >&2
  exit 65
fi
sysconfig_name=$(basename "$sysconfig_file" .py)
cp "$sysconfig_file" "$cross_dir/"

cat >"$cross_dir/sitecustomize.py" <<EOF
import os
import sys
import sysconfig

_target = '$target_python'
_include = _target + '/include/python3.13'
_lib = _target + '/lib'
_vars = sysconfig.get_config_vars()
_vars.update({
    'prefix': _target,
    'base': _target,
    'installed_base': _target,
    'platbase': _target,
    'installed_platbase': _target,
    'exec_prefix': _target,
    'INCLUDEPY': _include,
    'CONFINCLUDEPY': _include,
    'LIBDIR': _lib,
})
sys.prefix = sys.base_prefix = _target
sys.exec_prefix = sys.base_exec_prefix = _target
os.environ['_PYTHON_HOST_PLATFORM'] = 'wasm32-emscripten'
EOF

cat >"$cross_dir/cross-python.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export _PYTHON_SYSCONFIGDATA_NAME='$sysconfig_name'
export PYTHONPATH='$cross_dir'
exec '$native_python' "\$@"
EOF
chmod +x "$cross_dir/cross-python.sh"

"$cross_dir/cross-python.sh" - <<'PY'
import sys
import sysconfig

paths = sysconfig.get_paths()
assert sys.prefix.endswith('/python'), sys.prefix
assert paths['include'] == sys.prefix + '/include/python3.13', paths
assert paths['platinclude'] == paths['include'], paths
assert sysconfig.get_config_var('EXT_SUFFIX') == '.cpython-313-wasm32-emscripten.so'
assert sysconfig.get_config_var('SOABI') == 'cpython-313-wasm32-emscripten'
assert sysconfig.get_config_var('SIZEOF_VOID_P') == 4
assert 'emcc' in sysconfig.get_config_var('CC')
assert 'emcc' in sysconfig.get_config_var('LDSHARED')
assert sysconfig.get_platform() == 'wasm32-emscripten'
PY

cat >"$cross_dir/wasm32-emscripten.ini" <<EOF
[binaries]
c = '${EMSDK:?source emsdk_env.sh first}/upstream/emscripten/emcc'
cpp = '$EMSDK/upstream/emscripten/em++'
ar = '$EMSDK/upstream/emscripten/emar'
strip = '$EMSDK/upstream/emscripten/emstrip'
python = '$cross_dir/cross-python.sh'
cython = '$tool_venv/bin/cython'
pkg-config = 'false'

[properties]
skip_sanity_check = true
needs_exe_wrapper = true
# Emscripten 6.0.5 represents the 16-byte long double as little-endian
# IEEE-128.  Seed NumPy's supported cross property so Meson does not attempt
# to execute its target-format probe on the build host.
longdouble_format = 'IEEE_QUAD_LE'

[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[built-in options]
c_args = ['-fexceptions', '-matomics', '-mbulk-memory', '-O3', '-DNDEBUG']
cpp_args = ['-fexceptions', '-matomics', '-mbulk-memory', '-O3', '-DNDEBUG']
c_link_args = ['-fexceptions', '-matomics', '-mbulk-memory']
cpp_link_args = ['-fexceptions', '-matomics', '-mbulk-memory']
EOF

if [[ ! -f "$build_dir/build.ninja" ]]; then
  PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR= \
  "${meson[@]}" setup "$build_dir" "$source_dir" \
    --cross-file "$cross_dir/wasm32-emscripten.ini" \
    --buildtype release --prefix=/ \
    -Dallow-noblas=true \
    -Db_ndebug=true \
    -Ddisable-highway=true \
    -Ddisable-intel-sort=true \
    -Ddisable-optimization=true \
    -Ddisable-svml=true \
    -Ddisable-threading=true \
    -Dwerror=false \
    -Dcpu-baseline=none \
    -Dcpu-dispatch=none
fi
nice -n 19 "${meson[@]}" compile -C "$build_dir" -j 6

# The working payload carries only upstream's Python files and the generated
# config. Do not install intermediate shared modules: they become builtins below.
"$native_python" - "$source_dir" "$build_dir" "$target_python" <<'PY_STAGE'
from pathlib import Path
import shutil
import sys
source, build, target = map(Path, sys.argv[1:])
package = target / 'lib/python3.13/site-packages/numpy'
if package.exists():
    shutil.rmtree(package)
for path in (source / 'numpy').rglob('*.py'):
    dest = package / path.relative_to(source / 'numpy')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)
shutil.copy2(build / 'numpy/__config__.py', package / '__config__.py')
# Headers are build inputs, not files in the browser's Python payload.
headers = target / 'include/python3.13/numpy'
shutil.copytree(source / 'numpy/_core/include/numpy', headers, dirs_exist_ok=True)
for name in ['_numpyconfig.h', '__multiarray_api.h', '__ufunc_api.h']:
    shutil.copy2(build / 'numpy/_core' / name, headers / name)
PY_STAGE

"$native_python" - "$build_dir" "$target_python/lib/libnumpy.a" \
  "$EMSDK/upstream/emscripten/emar" <<'PY'
import hashlib
import pathlib
import shlex
import shutil
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
emar = sys.argv[3]
modules = [
    "numpy/_core/_multiarray_umath",
    "numpy/fft/_pocketfft_umath",
    "numpy/linalg/_umath_linalg",
    "numpy/linalg/lapack_lite",
    "numpy/random/_bounded_integers",
    "numpy/random/_common",
    "numpy/random/_generator",
    "numpy/random/_mt19937",
    "numpy/random/_pcg64",
    "numpy/random/_philox",
    "numpy/random/_sfc64",
    "numpy/random/bit_generator",
    "numpy/random/mtrand",
]

closure = set()
for module in modules:
    matches = list(build.glob(module + ".cpython-313-wasm32-emscripten.so"))
    if len(matches) != 1:
        raise SystemExit(f"expected one built extension for {module}, found {matches}")
    target = matches[0].relative_to(build).as_posix()
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True
    ).splitlines()
    link_commands = []
    for line in commands:
        command_tokens = shlex.split(line)
        outputs = []
        for index, token in enumerate(command_tokens):
            if token == "-o" and index + 1 < len(command_tokens):
                outputs.append(command_tokens[index + 1])
            elif token.startswith("-o") and len(token) > 2:
                outputs.append(token[2:])
        if target in outputs:
            link_commands.append(line)
    if len(link_commands) != 1:
        raise SystemExit(f"expected one link command for {target}")
    tokens = shlex.split(link_commands[0])
    expanded = []
    for token in tokens:
        if token.startswith("@"):
            expanded.extend(shlex.split((build / token[1:]).read_text()))
        else:
            expanded.append(token)
    for token in expanded:
        if not token.endswith((".o", ".a")):
            continue
        path = pathlib.Path(token)
        if not path.is_absolute():
            path = build / path
        path = path.resolve()
        if not path.is_file():
            raise SystemExit(f"missing NumPy link input: {path}")
        closure.add(path)

# LLVM's archive tool already knows how to merge ordinary and thin archives.
# MRI ADDLIB carries their actual object members into one ordinary archive.
if output.exists():
    output.unlink()
output.parent.mkdir(parents=True, exist_ok=True)
commands = [f'CREATE "{output}"']
# MRI prepends each input. Add the internal libraries last, as in the working
# build, so their shared distribution routines precede extension-local copies.
for path in sorted(closure, key=lambda path: (path.suffix == '.a', str(path))):
    commands.append(f'{"ADDLIB" if path.suffix == ".a" else "ADDMOD"} "{path}"')
commands.extend(['SAVE', 'END'])
subprocess.run([emar, '-M'], input='\n'.join(commands) + '\n', text=True, check=True)
print(f"libnumpy.a: {len(closure)} unique link inputs")
PY

"$EMSDK/upstream/bin/llvm-nm" --defined-only "$target_python/lib/libnumpy.a" \
  >"$build_root/libnumpy.symbols"
for symbol in \
  PyInit__multiarray_umath PyInit__pocketfft_umath PyInit__umath_linalg \
  PyInit_lapack_lite PyInit__bounded_integers PyInit__common PyInit__generator \
  PyInit__mt19937 PyInit__pcg64 PyInit__philox PyInit__sfc64 \
  PyInit_bit_generator PyInit_mtrand; do
  grep " $symbol$" "$build_root/libnumpy.symbols" >/dev/null
done
