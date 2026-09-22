# Headless WebAssembly release

Read this when rebuilding or releasing the Blender engine used by vgai.
The base is Blender v5.2.0, commit
`fbe6228777e7d9afefcd61a413844e790ae75db7`. The release manifest must name the
full fork commit containing this recipe and the source changes. No third-party
Blender port checkout, patch series or prebuilt dependency is an input.

## Toolchain and source

Use Emscripten SDK 6.0.5 (`1db513782be24469589d7cb8a1f1834e9a33f271` is the emcc-reported compiler revision),
Node 24, CMake, Ninja, ccache, a native C++20 compiler, pkg-config, Python 3,
rsync and brotli. Build with six jobs at most; the recipes use `nice -n 19`.
The native compiler makes build-time generators only. Activate the SDK with
`source <emsdk>/emsdk_env.sh`; verify `emcc --version` before compiling.

Clone the release commit with `GIT_LFS_SKIP_SMUDGE=1`. This profile materializes
only `release/datafiles/startup.blend`; fetch that exact upstream LFS object
before configuring. The fonts and display LUTs used here are ordinary Git
files. Other upstream LFS paths remain pointers, matching the measured working
payload; do not run an unfiltered `git lfs pull` when reproducing this release.

```bash
git remote add blender-upstream https://projects.blender.org/blender/blender.git
git lfs fetch blender-upstream v5.2.0 --include=release/datafiles/startup.blend --exclude=
git lfs checkout release/datafiles/startup.blend
```

Set absolute paths appropriate to the machine; no path below refers to the
original builder's filesystem:

```bash
set -euo pipefail
BLENDER_SOURCE=$(pwd)
BLENDER_BUILD=/absolute/path/to/blender-wasm-build
mkdir -p "$BLENDER_BUILD"/{downloads,src,deps-build,install}
```

`deps/sources.tsv` lists every compiled dependency archive in build order,
its official URL, extracted directory and SHA-256. Download and check each
archive, then invoke the existing per-dependency recipe. Those scripts contain
the complete configure/build/install arguments and all target feature choices:

```bash
while IFS=$'\t' read -r name archive directory sha256 url; do
  [[ "$name" == \#* || -z "$name" ]] && continue
  curl -fL --retry 3 "$url" -o "$BLENDER_BUILD/downloads/$archive"
  actual=$(shasum -a 256 "$BLENDER_BUILD/downloads/$archive" | awk '{print $1}')
  [[ "$actual" == "$sha256" ]] || exit 1
  tar -xf "$BLENDER_BUILD/downloads/$archive" -C "$BLENDER_BUILD/src"
  bash "$BLENDER_SOURCE/build_files/emscripten/deps/build-$name.sh" \
    "$BLENDER_BUILD/src/$directory" "$BLENDER_BUILD/deps-build/$name" \
    "$BLENDER_BUILD/install"
done < "$BLENDER_SOURCE/build_files/emscripten/deps/sources.tsv"
bash "$BLENDER_SOURCE/build_files/emscripten/deps/stage-python-wheels.sh" \
  "$BLENDER_BUILD/downloads/wheels" "$BLENDER_BUILD/install/python/lib/python3.13"
```

OpenEXR uses the explicitly built Imath, libdeflate and OpenJPH. OpenColorIO
uses expat, yaml-cpp, pystring and minizip-ng. OpenImageIO uses those image and
color libraries, TIFF, PNG, JPEG, fmt and robin-map; its only source patch is
`deps/openimageio-emscripten.patch`, applied by its recipe. The patch uses
ordinary string assignment on Emscripten: OIIO's private libc++ layout shortcut
corrupts long metadata names on this ABI and makes PNG writes fail. Blender's
image initialization keeps OIIO on its caller and OpenEXR's worker count at zero,
matching the browser profile. Manifold and OpenSubdiv use the built oneTBB. PugiXML is not enabled or linked.

CPython 3.13.13 builds a complete native helper interpreter and a separate
static browser-target interpreter. Its upstream wasm config site and explicit
module profile are in `deps/build-python.sh` and `python/browser-module-profile.txt`.
`-DPY_CALL_TRAMPOLINE` is required for Blender's CPython slot arity casts.
The HACL SHA2 archive and system expat are part of the final link.

NumPy 2.3.4 uses its own vendored Meson 1.8.3 and Cython 3.3.0. The recipe
harvests exactly thirteen production extension link closures into one archive
using LLVM ar's MRI interface. `bpy_numpy_modules.cc` registers their genuine
initializers before Python starts. Runtime Python files come from the same
sdist plus its generated `__config__.py`; extension test binaries are not
shipped. The pure wheels have their own URL/hash list in `python/pure-wheels.tsv`.
The small `js`, `pyodide` and `_multiprocessing` compatibility modules are
source-owned here; they do not provide subprocesses or a JavaScript bridge.

## Blender build

```bash
nice -n 19 bash "$BLENDER_SOURCE/build_files/emscripten/build-host-tools.sh" \
  "$BLENDER_SOURCE" "$BLENDER_BUILD/host-tools"
emcmake cmake -S "$BLENDER_SOURCE" -B "$BLENDER_BUILD/browser" -G Ninja \
  -C "$BLENDER_SOURCE/build_files/emscripten/headless_config.cmake" \
  -DCMAKE_PROJECT_INCLUDE="$BLENDER_SOURCE/build_files/emscripten/blender_web_headless.cmake" \
  -DBLENDER_WASM_DEPS_ROOT="$BLENDER_BUILD/install" \
  -DBLENDER_WASM_HOST_TOOLS_DIR="$BLENDER_BUILD/host-tools" \
  -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG' \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG' \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
/usr/bin/time -l nice -n 19 cmake --build "$BLENDER_BUILD/browser" \
  --target blender_editor --parallel 6
```

`datatoc` and `shader_tool` are native executables built from this exact source.
`makesdna` and `makesrna` are target executables run through Node with NODERAWFS,
so generated layouts match wasm32. All DNA assertions stay enabled.

The browser link is in `blender_web_headless.cmake`: JS exceptions, WasmFS,
pthreads with a twelve-worker pool, 512 MiB initial and 4 GiB maximum memory,
and the existing export/session doors. `blender_editor_diag` links the same
objects with assertions, stack checks and function names for diagnosis. Do not
build both final links concurrently. The release output is
`$BLENDER_BUILD/browser/build-editor/bin/blender_browser.{js,wasm,data}`.
The preload list and exclusions are in the same CMake file. The browser
consumer calls `releasePreloadedFileData` after session ready, not merely after
the module factory resolves. Leave `BW_SESSION` unset in the product.

## Verification and packaging

Run runtime/editor verification inside a World containing only the vendors
actually used. A local engine probe needs no vendor substitution. Use the
existing vgai model editor and battery, with `VGAI_BLENDER_WASM_DIR` set on the
editor server to this build's output directory. Preserve its worker, session,
observer, input catalog and recorded oracles. The musl catalog is
`packages/blender-engine/bench/battery/scenes/catalog-musl.json`; its five
models contain 95, 63, 41, 70 and 93 calls (362 total). Record actual results,
source stamps, timings, memory, named refusals and artifact comparisons.
A successful compile is not a passed battery.

Check Blender 5.2.0, Python 3.13.13, NumPy 2.3.4, the factory scene and
BLENDER_EEVEE, all baseline build options and builtins, driver math outcomes,
XML, hashes, and mathutils slot dispatch. Inspect the actual link command for
foreign or host-native library inputs. Keep raw and compressed SHA-256 values
and byte sizes with the release evidence.

After verification, compress each wasm/data file at brotli quality 11 into a
temporary file, decompress and compare it to its raw input, then rename it.
Copy the JS, both compressed files and BUNDLE.json together from a complete
staging directory. BUNDLE.json names the full fork commit and actual digests.

Blender and this engine are conveyed under GPL-3.0-or-later; retain Blender's
COPYING and all dependency license notices from their source archives. Publish
the corresponding source commit, this recipe, source/hash manifests and all
patches before conveying the binary outside Volter AI. A private source fork
does not satisfy that outward release gate. Do not remove package publication
restrictions until source availability and the replacement's battery evidence
are established.
