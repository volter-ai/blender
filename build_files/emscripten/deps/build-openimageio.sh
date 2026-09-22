#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <source-dir> <build-dir> <install-prefix>" >&2
  exit 64
fi

source_dir=$1
build_dir=$2
install_prefix=$3
patch_file=$(cd "$(dirname "$0")" && pwd)/openimageio-emscripten.patch
available_kb=$(df -Pk / | awk 'NR == 2 {print $4}')

if (( available_kb < 25 * 1024 * 1024 )); then
  echo "refusing build: less than 25 GiB available on /" >&2
  exit 75
fi

if patch --dry-run --silent --forward -d "$source_dir" -p1 < "$patch_file"; then
  patch --silent --forward -d "$source_dir" -p1 < "$patch_file"
elif patch --dry-run --silent --forward -R -d "$source_dir" -p1 < "$patch_file"; then
  echo "OpenImageIO Emscripten patch already applied"
else
  echo "refusing build: OpenImageIO source does not match the pinned patch" >&2
  exit 65
fi

PKG_CONFIG_PATH= \
PKG_CONFIG_LIBDIR="$install_prefix/lib/pkgconfig:$install_prefix/share/pkgconfig" \
emcmake cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_PREFIX_PATH="$install_prefix" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_FLAGS="-pthread -O3 -DNDEBUG" \
  -DCMAKE_CXX_FLAGS="-pthread -fexceptions -O3 -DNDEBUG" \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DLINKSTATIC=ON \
  -DOpenImageIO_REQUIRED_DEPS="TIFF;OpenEXR;PNG;libjpeg-turbo;fmt;Robinmap;ZLIB;OpenColorIO" \
  -DOpenImageIO_BUILD_MISSING_DEPS= \
  -DOIIO_BUILD_TOOLS=OFF \
  -DOIIO_BUILD_TESTS=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_DOCS=OFF \
  -DINSTALL_DOCS=OFF \
  -DINSTALL_FONTS=OFF \
  -DEMBEDPLUGINS=ON \
  -DUSE_SIMD=0 \
  -DSTOP_ON_WARNING=OFF \
  -DUSE_EXTERNAL_PUGIXML=OFF \
  -DUSE_PYTHON=OFF \
  -DUSE_TBB=OFF \
  -DUSE_OPENCOLORIO=ON \
  -DUSE_FREETYPE=OFF \
  -DUSE_QT=OFF \
  -DUSE_NUKE=OFF \
  -DUSE_OPENCV=OFF \
  -DUSE_OPENVDB=OFF \
  -DUSE_FFMPEG=OFF \
  -DUSE_WEBP=OFF \
  -DUSE_OPENJPEG=OFF \
  -DUSE_LIBHEIF=OFF \
  -DUSE_LIBRAW=OFF \
  -DUSE_JXL=OFF \
  -DUSE_GIF=OFF \
  -DUSE_DCMTK=OFF \
  -DUSE_PTEX=OFF \
  -DUSE_DICOM=OFF \
  -DUSE_R3DSDK=OFF \
  -DUSE_BZIP2=OFF \
  -DOpenEXR_ROOT="$install_prefix" \
  -DImath_ROOT="$install_prefix" \
  -Dfmt_ROOT="$install_prefix" \
  -DRobinmap_ROOT="$install_prefix" \
  -DROBINMAP_INCLUDE_DIR="$install_prefix/include" \
  -DImath_DIR="$install_prefix/lib/cmake/Imath" \
  -DOpenEXR_DIR="$install_prefix/lib/cmake/OpenEXR" \
  -DOpenColorIO_DIR="$install_prefix/lib/cmake/OpenColorIO" \
  -Dyaml-cpp_DIR="$install_prefix/lib/cmake/yaml-cpp" \
  -Dexpat_DIR="$install_prefix/lib/cmake/expat-2.7.5" \
  -Dexpat_INCLUDE_DIR="$install_prefix/include" \
  -Dexpat_LIBRARY="$install_prefix/lib/libexpat.a" \
  -Dpystring_ROOT="$install_prefix" \
  -Dpystring_INCLUDE_DIR="$install_prefix/include" \
  -Dpystring_LIBRARY="$install_prefix/lib/libpystring.a" \
  -Dminizip-ng_INCLUDE_DIR="$install_prefix/include/minizip-ng/minizip" \
  -Dminizip-ng_LIBRARY="$install_prefix/lib/libminizip.a" \
  -Dlibdeflate_DIR="$install_prefix/lib/cmake/libdeflate" \
  -Dopenjph_DIR="$install_prefix/lib/cmake/openjph" \
  -Dfmt_DIR="$install_prefix/lib/cmake/fmt" \
  -DTIFF_DIR="$install_prefix/lib/cmake/tiff" \
  -DPNG_DIR="$install_prefix/lib/cmake/PNG" \
  -Dlibjpeg-turbo_DIR="$install_prefix/lib/cmake/libjpeg-turbo" \
  -Dtsl-robin-map_DIR="$install_prefix/share/cmake/tsl-robin-map" \
  -DZLIB_ROOT="$install_prefix" \
  -DZLIB_INCLUDE_DIR="$install_prefix/include" \
  -DZLIB_LIBRARY="$install_prefix/lib/libz.a" \
  -DJPEG_ROOT="$install_prefix" \
  -DJPEG_INCLUDE_DIR="$install_prefix/include" \
  -DJPEG_LIBRARY="$install_prefix/lib/libjpeg.a" \
  -DPNG_ROOT="$install_prefix" \
  -DPNG_PNG_INCLUDE_DIR="$install_prefix/include" \
  -DPNG_LIBRARY="$install_prefix/lib/libpng16.a" \
  -DTIFF_INCLUDE_DIR="$install_prefix/include" \
  -DTIFF_LIBRARY="$install_prefix/lib/libtiff.a"

nice -n 19 cmake --build "$build_dir" --parallel 6
nice -n 19 cmake --install "$build_dir"
