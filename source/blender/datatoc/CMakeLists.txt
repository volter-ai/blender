# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


# -----------------------------------------------------------------------------
# Build `datatoc` executable.
set(SRC
  datatoc.cc
)

# `SRC_DNA_INC` is defined in the parent directory.
if(EMSCRIPTEN AND BLENDER_WASM_HOST_TOOLS_DIR)
  add_executable(datatoc IMPORTED GLOBAL)
  set_target_properties(datatoc PROPERTIES
    IMPORTED_LOCATION "${BLENDER_WASM_HOST_TOOLS_DIR}/datatoc")
else()
  add_executable(datatoc ${SRC})
  optimize_debug_target(datatoc)
endif()
