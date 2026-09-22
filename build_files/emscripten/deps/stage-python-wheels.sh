#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <download-cache> <python-lib-dir>" >&2
  exit 64
fi

download_cache=$1
python_lib=$2
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
manifest="$script_dir/../python/pure-wheels.tsv"
site_packages="$python_lib/site-packages"

mkdir -p "$download_cache" "$site_packages"
while IFS=$'\t' read -r name version filename url sha256; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  wheel="$download_cache/$filename"
  if [[ ! -f "$wheel" ]]; then
    curl -fL --retry 3 -o "$wheel" "$url"
  fi
  actual=$(shasum -a 256 "$wheel" | awk '{print $1}')
  if [[ "$actual" != "$sha256" ]]; then
    echo "$filename: sha256 mismatch ($actual)" >&2
    exit 65
  fi
  python3 - "$wheel" "$site_packages" <<'PY'
import pathlib
import sys
import zipfile

wheel = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2]).resolve()
with zipfile.ZipFile(wheel) as archive:
    for info in archive.infolist():
        relative = pathlib.PurePosixPath(info.filename)
        if relative.is_absolute() or ".." in relative.parts:
            raise SystemExit(f"unsafe wheel member: {info.filename}")
        if relative.suffix in {".so", ".a"}:
            raise SystemExit(f"native artifact in pure wheel: {info.filename}")
    archive.extractall(destination)
PY
  echo "staged $name $version ($sha256)"
done <"$manifest"

find "$site_packages" -type d -name __pycache__ -prune -exec rm -rf {} +
find "$site_packages" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete
