#!/usr/bin/env bash
set -euo pipefail

source_dir="${1:?usage: build-host-tools.sh SOURCE_DIR OUTPUT_DIR}"
output_dir="${2:?usage: build-host-tools.sh SOURCE_DIR OUTPUT_DIR}"
cxx="${CXX:-c++}"

mkdir -p "${output_dir}/objects"

"${cxx}" -std=c++20 -O2 -DNDEBUG \
  "${source_dir}/source/blender/datatoc/datatoc.cc" \
  -o "${output_dir}/datatoc"

shader_dir="${source_dir}/source/blender/gpu/shader_tool"
shader_sources=(
  attribute.cc enum.cc flow_control.cc function.cc grammar.cc intermediate.cc metadata.cc
  namespace.cc processor.cc resource_table.cc shader_tool.cc string.cc struct.cc template.cc union.cc
  lexit/lexit.cc
)
shader_objects=()
for shader_source in "${shader_sources[@]}"; do
  object_name="${shader_source//\//_}.o"
  "${cxx}" -std=c++20 -O2 -DNDEBUG -c \
    "${shader_dir}/${shader_source}" -o "${output_dir}/objects/${object_name}"
  shader_objects+=("${output_dir}/objects/${object_name}")
done
"${cxx}" "${shader_objects[@]}" -o "${output_dir}/shader_tool"
