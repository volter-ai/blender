// Hand back the packed `.data` payload once WasmFS has copied it into the heap.
//
// MEASURED 2026-09-18. `wasmFSPreloadedFiles` holds one `{pathName, fileData,
// mode}` record per preloaded file, and every `fileData` is a SUBARRAY of the
// single ~55 MB ArrayBuffer the `.data` package was read into. WasmFS copies
// all of it into linear memory during its own init
// (`__wasmfs_copy_preloaded_file_data`) and never reads the array again -- but
// emscripten never clears it, so the whole packed payload stays resident for
// the life of the module as a second copy of bytes already in the wasm heap.
// In Node, `process.memoryUsage().arrayBuffers` sat at 55 MB from `factory
// resolved` through every later call, and two forced GCs with `Module.preRun`
// emptied did not move it: the subarrays are what hold it.
//
// `Module.preRun` goes too, in the same call: it holds the file-packager's
// `runWithFS`, whose closure owns the ArrayBuffer itself. Dropping only one of
// the two frees nothing.
//
// THE CALLER is the worker, immediately after the session's ready line
// (`packages/blender/browser/blender-engine.mts`). By then WasmFS has long
// since flushed -- `wasmFSPreloadingFlushed` is set the first time
// `__wasmfs_get_num_preloaded_files` is called, during wasmfs init -- so
// anything staged later takes `FS_createDataFile`'s post-flush branch
// (`FS_create` + `FS_writeFile`) and never consults this array.
Module['releasePreloadedFileData'] = () => {
  let bytes = 0;
  for (const preloaded of wasmFSPreloadedFiles) bytes += preloaded.fileData.length;
  const files = wasmFSPreloadedFiles.length;
  wasmFSPreloadedFiles.length = 0;
  wasmFSPreloadedDirs.length = 0;
  if (Array.isArray(Module['preRun'])) Module['preRun'].length = 0;
  return { files, bytes };
};
