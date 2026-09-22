/* SPDX-FileCopyrightText: 2026 blender-wasm-headless spike
 * SPDX-FileCopyrightText: 2011-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The long-lived headless session door.
 *
 * `blender -b --factory-startup` normally ends its life inside `WM_exit()` as soon
 * as the argument list is exhausted (source/creator/creator.cc, the `G.background`
 * branch). This file keeps that process ALIVE instead: `main()` stashes the
 * initialised `bContext` here and returns, the Emscripten runtime stays up
 * (`-sEXIT_RUNTIME=0`), and JS then calls `blender_web_exec()` as many times as it
 * likes against that one warm session — which is exactly the shape an MCP
 * `execute_blender_code` call needs.
 *
 * Capture: the executed string runs under a Python-level stdout/stderr
 * redirection, so everything a script prints (and any traceback) comes back to the
 * caller. C-level output from Blender's own printf paths is NOT captured here; it
 * still goes to Module.print.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "BLI_utildefines.h"

#include "BPY_extern_run.hh"

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#else
#  define EMSCRIPTEN_KEEPALIVE
#endif

namespace blender {
struct bContext;
}
using blender::bContext;

static bContext *g_bw_session_C = nullptr;
static std::string g_bw_last_output;
static int g_bw_last_status = 0; /* 0 = ok, 1 = python raised, 2 = no session. */

/* Paths inside the module's own filesystem; fixed literals, so no quoting of user
 * code is ever required. */
#define BW_CODE_PATH "/tmp/_bw_code.py"
#define BW_OUT_PATH "/tmp/_bw_out.txt"
#define BW_STATUS_PATH "/tmp/_bw_status.txt"

/* The wrapper is a constant: it reads the user's code from disk, runs it in
 * __main__'s namespace (so state persists across calls, like a REPL), and writes
 * the captured stream + status back. */
static const char *BW_WRAPPER =
    "import sys, io, traceback\n"
    "_bw_main = sys.modules['__main__'].__dict__\n"
    "_bw_buf = io.StringIO()\n"
    "_bw_so, _bw_se = sys.stdout, sys.stderr\n"
    "sys.stdout = sys.stderr = _bw_buf\n"
    "_bw_status = 0\n"
    "try:\n"
    "    with open('" BW_CODE_PATH "', 'r') as _f:\n"
    "        _bw_src = _f.read()\n"
    "    exec(compile(_bw_src, '<blender_web_exec>', 'exec'), _bw_main)\n"
    "except BaseException:\n"
    "    _bw_status = 1\n"
    "    traceback.print_exc(file=_bw_buf)\n"
    "finally:\n"
    "    sys.stdout, sys.stderr = _bw_so, _bw_se\n"
    "with open('" BW_OUT_PATH "', 'w') as _f:\n"
    "    _f.write(_bw_buf.getvalue())\n"
    "with open('" BW_STATUS_PATH "', 'w') as _f:\n"
    "    _f.write(str(_bw_status))\n";

static std::string bw_read_file(const char *path)
{
  std::string out;
  FILE *fp = fopen(path, "rb");
  if (fp == nullptr) {
    return out;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    out.append(buf, n);
  }
  fclose(fp);
  return out;
}

/* Called from creator.cc's background branch, in place of WM_exit(). */
void blender_web_session_begin(bContext *C)
{
  g_bw_session_C = C;
}

bool blender_web_session_hold_requested()
{
  const char *v = getenv("BW_SESSION");
  return (v != nullptr && v[0] != '\0' && v[0] != '0');
}

extern "C" {

/* The door. Returns captured stdout+stderr of `code`; the error flag is
 * blender_web_last_status() (0 ok, 1 python raised, 2 no live session). */
EMSCRIPTEN_KEEPALIVE const char *blender_web_exec(const char *code)
{
  if (g_bw_session_C == nullptr) {
    g_bw_last_status = 2;
    g_bw_last_output = "blender_web_exec: no live session (main() has not held yet)\n";
    return g_bw_last_output.c_str();
  }

  FILE *fp = fopen(BW_CODE_PATH, "wb");
  if (fp == nullptr) {
    g_bw_last_status = 2;
    g_bw_last_output = "blender_web_exec: cannot write " BW_CODE_PATH "\n";
    return g_bw_last_output.c_str();
  }
  const size_t len = strlen(code);
  if (len) {
    fwrite(code, 1, len, fp);
  }
  fclose(fp);

  remove(BW_OUT_PATH);
  remove(BW_STATUS_PATH);

  const bool ok = blender::BPY_run_string_exec(g_bw_session_C, nullptr, BW_WRAPPER);

  g_bw_last_output = bw_read_file(BW_OUT_PATH);
  const std::string status = bw_read_file(BW_STATUS_PATH);
  g_bw_last_status = (!ok || status.empty()) ? 1 : (status[0] == '0' ? 0 : 1);
  return g_bw_last_output.c_str();
}

EMSCRIPTEN_KEEPALIVE int blender_web_last_status()
{
  return g_bw_last_status;
}

/* True once main() has handed the session over and the door is usable. */
EMSCRIPTEN_KEEPALIVE int blender_web_session_alive()
{
  return g_bw_session_C != nullptr ? 1 : 0;
}

} /* extern "C" */
