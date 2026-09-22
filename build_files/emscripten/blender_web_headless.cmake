# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT EMSCRIPTEN)
  return()
endif()

function(blender_web_payload_exclusions out_var python_home scripts)
  set(_paths
    "${python_home}/ensurepip"
    "${python_home}/idlelib"
    "${python_home}/tkinter"
    "${python_home}/turtledemo"
    "${python_home}/turtle.py"
    "${python_home}/pydoc_data"
    "${python_home}/venv"
    "${scripts}/addons_core/ui_translate")
  set(_result "")
  foreach(_path IN LISTS _paths)
    if(NOT EXISTS "${_path}")
      message(FATAL_ERROR "wasm payload exclusion missing: ${_path}")
    endif()
    string(APPEND _result " --exclude-file ${_path}")
  endforeach()
  set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(blender_web_add_editor source_target variant)
  if(NOT TARGET ${source_target})
    message(FATAL_ERROR "wasm editor source target missing: ${source_target}")
  endif()
  if(variant STREQUAL "release")
    set(_target blender_editor)
    set(_output_dir "${CMAKE_BINARY_DIR}/build-editor/bin")
  elseif(variant STREQUAL "diagnostic")
    set(_target blender_editor_diag)
    set(_output_dir "${CMAKE_BINARY_DIR}/build-editor-diag/bin")
  else()
    message(FATAL_ERROR "unknown wasm editor variant: ${variant}")
  endif()

  get_target_property(_sources ${source_target} SOURCES)
  get_target_property(_source_dir ${source_target} SOURCE_DIR)
  set(_absolute_sources "")
  foreach(_source IN LISTS _sources)
    if(IS_ABSOLUTE "${_source}")
      list(APPEND _absolute_sources "${_source}")
    else()
      list(APPEND _absolute_sources "${_source_dir}/${_source}")
    endif()
  endforeach()
  add_executable(${_target} ${_absolute_sources})
  add_dependencies(${_target} makesdna makesrna)

  foreach(_property LINK_LIBRARIES COMPILE_FEATURES)
    get_target_property(_value ${source_target} ${_property})
    if(_value)
      set_target_properties(${_target} PROPERTIES ${_property} "${_value}")
    endif()
  endforeach()
  foreach(_property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS)
    set(_merged "")
    get_target_property(_target_value ${source_target} ${_property})
    if(_target_value)
      list(APPEND _merged ${_target_value})
    endif()
    get_directory_property(_directory_value DIRECTORY "${_source_dir}" ${_property})
    if(_directory_value)
      list(APPEND _merged ${_directory_value})
    endif()
    if(_merged)
      list(REMOVE_DUPLICATES _merged)
      set_target_properties(${_target} PROPERTIES ${_property} "${_merged}")
    endif()
  endforeach()

  set(_python_home "${BLENDER_WASM_DEPS_ROOT}/python/lib/python3.13")
  set(_scripts "${CMAKE_SOURCE_DIR}/scripts")
  set(_datafiles "${CMAKE_SOURCE_DIR}/release/datafiles")
  set(_cycles_addon "${CMAKE_SOURCE_DIR}/intern/cycles/blender/addon")
  foreach(_path
      "${_python_home}"
      "${_scripts}"
      "${_datafiles}/colormanagement"
      "${_datafiles}/fonts/Inter.woff2"
      "${_datafiles}/fonts/DejaVuSansMono.woff2"
      "${_cycles_addon}")
    if(NOT EXISTS "${_path}")
      message(FATAL_ERROR "wasm editor preload input missing: ${_path}")
    endif()
  endforeach()
  blender_web_payload_exclusions(_exclusions "${_python_home}" "${_scripts}")

  set(_flags
    "-pthread -fexceptions -sMALLOC=dlmalloc -sWASM_BIGINT -sALLOW_MEMORY_GROWTH \
-sINITIAL_MEMORY=536870912 -sMAXIMUM_MEMORY=4294967296 \
-sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 -sSTACK_SIZE=8388608 \
-sPTHREAD_POOL_SIZE=12 -sWASMFS -sFORCE_FILESYSTEM=1 \
-sENVIRONMENT=web,worker,node \
-sMODULARIZE=1 -sEXPORT_NAME=createBlenderModule \
-sEXPORTED_RUNTIME_METHODS=ENV,FS,callMain,HEAPU8 \
-sEXPORTED_FUNCTIONS=_main,_blender_web_export_frame,_blender_web_export_buffer,_blender_web_export_buffer_size,_blender_web_export_session_reset,_malloc,_free \
-sINCOMING_MODULE_JS_API=ENVIRONMENT,arguments,canvas,dynamicLibraries,elementPointerLock,instantiateWasm,locateFile,mainScriptUrlOrBlob,monitorRunDependencies,noExitRuntime,noInitialRun,onAbort,onExit,onRuntimeInitialized,postRun,preInit,preRun,print,printErr,setStatus,statusMessage,stderr,stdin,stdout,thisProgram,wasm,websocket")
  if(variant STREQUAL "release")
    string(APPEND _flags " -O2 -g0 -sASSERTIONS=0")
  else()
    string(APPEND _flags " -O2 --profiling-funcs -sASSERTIONS=1 -sSTACK_OVERFLOW_CHECK=2")
  endif()
  string(APPEND _flags
    " --post-js ${CMAKE_SOURCE_DIR}/build_files/emscripten/release-preloaded-file-data.js"
    " --preload-file ${_python_home}@/bw/python/lib/python3.13"
    " --preload-file ${_scripts}@/bw/scripts"
    " --preload-file ${_cycles_addon}@/bw/scripts/addons_core/cycles"
    " --preload-file ${_datafiles}/colormanagement@/bw/datafiles/colormanagement"
    " --preload-file ${_datafiles}/fonts/Inter.woff2@/bw/datafiles/fonts/Inter.woff2"
    " --preload-file ${_datafiles}/fonts/DejaVuSansMono.woff2@/bw/datafiles/fonts/DejaVuSansMono.woff2"
    "${_exclusions}")

  set_target_properties(${_target} PROPERTIES
    LINK_FLAGS "${_flags}"
    OUTPUT_NAME blender_browser
    RUNTIME_OUTPUT_DIRECTORY "${_output_dir}")
  message(STATUS "wasm editor target ${_target} -> ${_output_dir}/blender_browser.*")
endfunction()

cmake_language(DEFER CALL blender_web_add_editor blender release)
cmake_language(DEFER CALL blender_web_add_editor blender diagnostic)
