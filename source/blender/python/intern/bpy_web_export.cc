/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * THE EXPORT DOOR: Blender's own memory read out as typed columns, for a
 * three.js presenter to draw.
 *
 * One call answers with a small JSON FRAME (`blender_web_export_frame`) and a
 * side ARENA of bytes; every large column lives in the arena and the frame
 * names it as `{offset, length, dtype, count, stride}`. Nothing large is ever
 * spelled in the JSON, because the JSON is parsed and the arena is not: in the
 * browser the JS reads the arena straight off the wasm heap
 * (`blender_web_export_buffer` / `_buffer_size`), and natively the same bytes
 * are written to the path `options_json.buffer_path` names.
 *
 * REVISIONS ARE THE DEPSGRAPH'S OWN UPDATE RECORD, never a hash. After each
 * evaluation the door walks `DEG_iterator_ids_begin` with `only_updated`,
 * which is precisely what Python's `depsgraph.updates` exposes, and bumps a
 * per-session revision for every ID that appears. A caller that already holds
 * a revision says so in `options_json.known` -- keyed `mesh:<geometry key>` and
 * `image:<image name>`, because one flat map shared by two namespaces answers
 * "unchanged" to a picture that happens to share a mesh's name -- and that
 * datablock comes back as `{revision, unchanged: true}` with no columns (a
 * picture the caller holds is simply left out; the presenter's image schema has
 * no unchanged form and the material's texture reference carries the revision).
 *
 * Geometry is keyed separately from the mesh datablock, because the geometry
 * this door ships is EVALUATED: a deformed or modified object's geometry moves
 * with the object while the mesh datablock never changes. A geometry key's
 * revision bumps on ID_RECALC_GEOMETRY of either the object or its data --
 * never on ID_RECALC_TRANSFORM, which is why moving an object re-ships the
 * object row and no columns at all.
 *
 * THE MATERIAL REDUCTION IS THE DOOR'S OWN: walk from the material output to
 * the surface -- past a Mix Shader a Light Path splits, to a Principled BSDF,
 * an Emission or a Transparent -- and reduce each carried input to a CONSTANT
 * or a SINGLE texture, because that is what a three.js standard material IS. A
 * graph this cannot reduce is a WARNING naming the node (carried in the
 * frame's `warnings`), and the material falls back to the constant -- never an
 * exception, because this call also carries the geometry.
 *
 * WHAT THE FRAME IS, is the presenter's own schema
 * (`packages/mesh/contributions/blender-runtime-view.ts`), which is strict in
 * both directions: an unrecognized key is rejected and a missing column is
 * rejected. A mismatch is fixed HERE, never by loosening that schema. What
 * this door does NOT describe is the scene's WORLD and its CAMERAS: the
 * session reduces a sky/gradient world graph to the presenter's world
 * expression, and frames a photograph through a camera, and both stay there.
 */

#include <Python.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BLI_listbase.h"
#include "BLI_color.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "DNA_ID.h"
#include "DNA_camera_types.h"
#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_world_types.h"

#include "BKE_attribute.hh"
#include "BKE_callbacks.hh"

#include "RNA_types.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "IMB_imbuf_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

namespace blender::web_export {

/* -------------------------------------------------------------------- */
/** \name The arena
 * \{ */

/** Every column the frame names lives here; the frame holds only offsets. */
static std::vector<uint8_t> g_arena;
/** The answer, owned by the door and valid until the next call. */
static std::string g_frame;

struct ColumnRef {
  size_t offset = 0;
  size_t length = 0;
  const char *dtype = "f32";
  size_t count = 0;
  int stride = 1;
  bool present = false;
};

static ColumnRef arena_write(const void *data, size_t bytes, const char *dtype, size_t count, int stride)
{
  while (g_arena.size() % 8 != 0) {
    g_arena.push_back(0);
  }
  ColumnRef ref;
  ref.offset = g_arena.size();
  ref.length = bytes;
  ref.dtype = dtype;
  ref.count = count;
  ref.stride = stride;
  ref.present = true;
  /* A zero-length column is still a column: an edgeless mesh ships an empty
   * `edge` array, not a missing one, because the frame schema is strict. */
  if (bytes != 0) {
    g_arena.insert(g_arena.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
  }
  return ref;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Minimal JSON writing and reading
 *
 * Floats print with `%.9g`, which round-trips a binary32 EXACTLY -- the proof
 * compares these numbers bit for bit against `bpy`'s own, so a shorter format
 * would be a measurement error dressed as a mismatch.
 * \{ */

static void json_escape(std::string &out, const char *text)
{
  out.push_back('"');
  for (const char *c = text; *c; c++) {
    const unsigned char ch = static_cast<unsigned char>(*c);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buf[8];
          SNPRINTF(buf, "\\u%04x", ch);
          out += buf;
        }
        else {
          out.push_back(char(ch));
        }
    }
  }
  out.push_back('"');
}

static void json_float(std::string &out, const float value)
{
  if (!std::isfinite(value)) {
    out += "0";
    return;
  }
  char buf[32];
  SNPRINTF(buf, "%.9g", double(value));
  out += buf;
}

static void json_int(std::string &out, const long long value)
{
  char buf[32];
  SNPRINTF(buf, "%lld", value);
  out += buf;
}

static void json_column(std::string &out, const char *name, const ColumnRef &ref, bool &first)
{
  if (!ref.present) {
    return;
  }
  if (!first) {
    out += ",";
  }
  first = false;
  json_escape(out, name);
  out += ":{\"offset\":";
  json_int(out, (long long)ref.offset);
  out += ",\"length\":";
  json_int(out, (long long)ref.length);
  out += ",\"dtype\":";
  json_escape(out, ref.dtype);
  out += ",\"count\":";
  json_int(out, (long long)ref.count);
  out += ",\"stride\":";
  json_int(out, ref.stride);
  out += "}";
}

/**
 * The options this door reads, scanned out of `options_json` WITHOUT a JSON
 * parser: the only shapes it accepts are `"buffer_path": "<path>"`,
 * `"evaluate": true|false` and `"known": {"<key>": <int>, ...}`, and a scanner
 * for exactly those is smaller than a parser and has no dependency of its own.
 * An unrecognized key is ignored; a malformed one yields no options at all,
 * which is the same as asking for a full frame.
 */
struct Options {
  std::string buffer_path;
  /** The caller's session name, echoed as the frame's `session`. */
  std::string session;
  bool evaluate = true;
  /** Revisions the CALLER already holds, keyed by mesh/image name. The key is
   *  `known`; see `parse_options`, which names any other key in the frame's
   *  warnings rather than ignoring it. */
  std::unordered_map<std::string, long long> known;
  std::vector<std::string> unknown_keys;
};

static const char *skip_space(const char *c)
{
  while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') {
    c++;
  }
  return c;
}

static const char *read_string(const char *c, std::string &out)
{
  if (*c != '"') {
    return nullptr;
  }
  c++;
  out.clear();
  while (*c && *c != '"') {
    if (*c == '\\' && c[1]) {
      c++;
      switch (*c) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        default:
          out.push_back(*c);
      }
    }
    else {
      out.push_back(*c);
    }
    c++;
  }
  return *c == '"' ? c + 1 : nullptr;
}

/** Past one JSON value, whatever shape it is -- so an unrecognized key's nested
 *  contents are not scanned as though they were more top-level keys. Without
 *  this, `{"held": {"Cube": 1}}` reported BOTH `held` and `Cube` as unknown
 *  options, and the second name is an invention: there is no `Cube` option. */
static const char *skip_value(const char *c)
{
  c = skip_space(c);
  if (*c == '"') {
    std::string ignored;
    const char *end = read_string(c, ignored);
    return end ? end : c + 1;
  }
  if (*c == '{' || *c == '[') {
    const char open = *c;
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (*c) {
      if (*c == '"') {
        std::string ignored;
        const char *end = read_string(c, ignored);
        c = end ? end : c + 1;
        continue;
      }
      if (*c == open) {
        depth++;
      }
      else if (*c == close) {
        depth--;
        if (depth == 0) {
          return c + 1;
        }
      }
      c++;
    }
    return c;
  }
  while (*c && *c != ',' && *c != '}' && *c != ']') {
    c++;
  }
  return c;
}

static Options parse_options(const char *options_json)
{
  Options options;
  if (options_json == nullptr) {
    return options;
  }
  const char *c = options_json;
  while (*c) {
    if (*c != '"') {
      c++;
      continue;
    }
    std::string key;
    const char *after = read_string(c, key);
    if (after == nullptr) {
      break;
    }
    const char *value = skip_space(after);
    if (*value != ':') {
      c = after;
      continue;
    }
    value = skip_space(value + 1);
    if (key == "session" && *value == '"') {
      const char *end = read_string(value, options.session);
      c = end ? end : value + 1;
      continue;
    }
    if (key == "buffer_path" && *value == '"') {
      const char *end = read_string(value, options.buffer_path);
      c = end ? end : value + 1;
      continue;
    }
    if (key == "evaluate") {
      options.evaluate = (strncmp(value, "true", 4) == 0);
      c = value;
      continue;
    }
    if (key == "known" && *value == '{') {
      const char *entry = value + 1;
      while (*entry && *entry != '}') {
        entry = skip_space(entry);
        if (*entry != '"') {
          entry++;
          continue;
        }
        std::string name;
        const char *end = read_string(entry, name);
        if (end == nullptr) {
          break;
        }
        end = skip_space(end);
        if (*end != ':') {
          entry = end;
          continue;
        }
        end = skip_space(end + 1);
        char *number_end = nullptr;
        const long long revision = strtoll(end, &number_end, 10);
        if (number_end == end) {
          entry = end;
          continue;
        }
        options.known[name] = revision;
        entry = number_end;
      }
      c = entry;
      continue;
    }
    /* AN UNRECOGNIZED OPTION IS NAMED, NEVER IGNORED. A caller that spells the
     * held-revision map anything but `known` would otherwise be answered with
     * a full frame and no hint why -- silently paying for every column on
     * every call. Measured: a review probe passed `held` and saw exactly that. */
    options.unknown_keys.push_back(key);
    c = skip_value(value);
  }
  return options;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The session: revisions, and nothing else
 * \{ */

struct Session {
  /** Revision per datablock, bumped from the depsgraph's update record. */
  std::unordered_map<const ID *, long long> id_revision;
  /** Revision per GEOMETRY key -- an evaluated mesh, not a mesh datablock. */
  std::unordered_map<std::string, long long> geometry_revision;
  /** THE UPDATE RECORD SINCE THE LAST FRAME, from every evaluation whoever ran
   *  it -- see `web_export_update_post`. Original ID -> OR of `recalc` flags. */
  std::unordered_map<const ID *, uint32_t> pending;
  long long frame_revision = 0;
  std::string id;
};

static Session g_session;
/** Warnings already named once, so a refusal is reported per cause and not
 *  per frame. Cleared with the session. */
static std::unordered_set<std::string> g_warned;

/* -------------------------------------------------------------------- */
/** \name The update record, captured at EVERY evaluation
 *
 * THE DOOR CANNOT BE THE ONLY ONE WHO EVALUATES. `BKE_scene_graph_update_tagged`
 * -- which is what `bpy.context.view_layer.update()`,
 * `evaluated_depsgraph_get()` and most operators run -- ends with
 * `DEG_ids_clear_recalc`, so any evaluation the door did not perform itself
 * CONSUMES the record and leaves nothing behind. Measured: a script doing
 * `cube.location.x = 3; view_layer.update()` before the call made the door
 * report `updated: []`, and the same for a mesh edit, whose revision then never
 * moved -- a presenter keyed on that revision keeps the stale geometry forever.
 *
 * So the door LISTENS instead of asking. `BKE_CB_EVT_DEPSGRAPH_UPDATE_POST` is
 * the hook `bpy.app.handlers.depsgraph_update_post` is built on; it fires
 * inside `scene_graph_update_tagged` with the record still intact, BEFORE the
 * clear. Every evaluation accumulates into `Session::pending`, and
 * `export_frame` merges that with its own evaluation's record.
 * \{ */

static void accumulate_updates(Depsgraph *depsgraph)
{
  if (depsgraph == nullptr) {
    return;
  }
  DEGIDIterData data = {};
  data.graph = depsgraph;
  data.only_updated = true;
  BLI_Iterator iter = {};
  iter.valid = true;
  DEG_iterator_ids_begin(&iter, &data);
  while (iter.valid) {
    ID *id_eval = static_cast<ID *>(iter.current);
    if (id_eval != nullptr) {
      g_session.pending[DEG_get_original_id(id_eval)] |= uint32_t(id_eval->recalc);
      if (GS(id_eval->name) == ID_OB) {
        const Object *object = reinterpret_cast<const Object *>(id_eval);
        if (object->data != nullptr) {
          const ID *data_id = static_cast<const ID *>(object->data);
          if (data_id->recalc & ID_RECALC_ALL) {
            /* Blender's own rule for "this object's geometry changed"
             * (`rna_DepsgraphUpdate_is_updated_geometry_get`): any recalc on the
             * object's DATA counts. That is what a deformed or modified object
             * needs -- its evaluated mesh moves while the datablock does not. */
            g_session.pending[DEG_get_original_id(&object->id)] |= uint32_t(ID_RECALC_GEOMETRY);
            g_session.pending[DEG_get_original_id(data_id)] |= uint32_t(data_id->recalc);
          }
        }
      }
    }
    DEG_iterator_ids_next(&iter);
  }
  DEG_iterator_ids_end(&iter);
}

/** ONE ACCUMULATOR, TWO EVENTS.
 *
 * `scene.frame_set()` runs `BKE_scene_graph_update_for_newframe`, which fires
 * FRAME_CHANGE_POST and NOT DEPSGRAPH_UPDATE_POST -- measured in the tab on a
 * shape-keyed cube animated 1 to 10: update-post logged nothing while
 * frame-change-post carried a depsgraph naming the deformed mesh. Listening on
 * one event leaves every animated deformation permanently stale on the page.
 * Both events carry `(id, depsgraph)` in the same two pointers.
 *
 * A RENDER'S OWN EVALUATION IS NOT AN AUTHORING CHANGE. `bpy.ops.render.render`
 * builds a render depsgraph and evaluates every object into it, and this fires
 * for that too -- measured on the courtyard (287 objects): the present after
 * each photograph shipped 227 of 229 meshes as changed though nothing was
 * edited. The scene the presenter holds is the VIEWPORT's. */
static void web_export_update_post(Main * /*bmain*/,
                                   PointerRNA **pointers,
                                   int pointers_num,
                                   void * /*arg*/)
{
  if (pointers_num < 2 || pointers[1] == nullptr) {
    return;
  }
  Depsgraph *depsgraph = static_cast<Depsgraph *>(pointers[1]->data);
  if (depsgraph == nullptr || DEG_get_mode(depsgraph) != DAG_EVAL_VIEWPORT) {
    return;
  }
  accumulate_updates(depsgraph);
}

/** Defined with the image change record below; a file load drops every reader. */
static void forget_image_watchers();

static void web_export_load_post(Main * /*bmain*/,
                                 PointerRNA ** /*pointers*/,
                                 int /*pointers_num*/,
                                 void * /*arg*/)
{
  /* A NEW MAIN IS A NEW SESSION. Every revision this door holds describes
   * datablocks that no longer exist -- ID pointers into a freed Main, and names
   * that now mean something else -- so a file read (including
   * `read_factory_settings`, which reaches this same `LOAD_POST`) starts the
   * numbering over rather than handing a presenter a revision that matches by
   * accident. The FRAME counter here is per-`Main` for exactly that reason; the
   * frame number the presenter reads is the SESSION's and is stamped there. */
  forget_image_watchers();
  g_session = Session();
  g_warned.clear();
}

static bCallbackFuncStore g_update_post_store = {};
static bCallbackFuncStore g_frame_change_post_store = {};
static bCallbackFuncStore g_load_post_store = {};
static bool g_callbacks_registered = false;

/** Registered ONCE per process: `BKE_callback_global_finalize` runs only at
 *  exit, so these survive every file read and must never be added twice. */
static void ensure_callbacks()
{
  if (g_callbacks_registered) {
    return;
  }
  g_callbacks_registered = true;
  g_update_post_store.func = web_export_update_post;
  g_update_post_store.alloc = false;
  BKE_callback_add(&g_update_post_store, BKE_CB_EVT_DEPSGRAPH_UPDATE_POST);
  g_frame_change_post_store.func = web_export_update_post;
  g_frame_change_post_store.alloc = false;
  BKE_callback_add(&g_frame_change_post_store, BKE_CB_EVT_FRAME_CHANGE_POST);
  g_load_post_store.func = web_export_load_post;
  g_load_post_store.alloc = false;
  BKE_callback_add(&g_load_post_store, BKE_CB_EVT_LOAD_POST);
}

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name The standard-material reduction
 *
 * The reduction is the door's own; see this file's header.
 * \{ */

static std::vector<std::string> *g_warnings = nullptr;

static void unreached(const std::string &what)
{
  if (g_warned.count(what)) {
    return;
  }
  g_warned.insert(what);
  if (g_warnings) {
    g_warnings->push_back(what);
  }
}

static bool socket_available(const bNodeSocket *socket)
{
  return socket && (socket->flag & SOCK_UNAVAIL) == 0;
}

static const bNodeSocket *find_input(const bNode *node, const char *name)
{
  for (const bNodeSocket &socket : node->inputs) {
    if (socket_available(&socket) && STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
}

static const bNodeLink *incoming(const bNodeTree *tree, const bNode *node, const char *name)
{
  const bNodeSocket *socket = find_input(node, name);
  if (socket == nullptr) {
    return nullptr;
  }
  for (const bNodeLink &link : tree->links) {
    if (link.tosock == socket && link.tonode == node) {
      return &link;
    }
  }
  return nullptr;
}

static float socket_float(const bNodeSocket *socket, const float fallback)
{
  if (socket == nullptr || socket->default_value == nullptr) {
    return fallback;
  }
  if (socket->type == SOCK_FLOAT) {
    return static_cast<const bNodeSocketValueFloat *>(socket->default_value)->value;
  }
  if (socket->type == SOCK_RGBA) {
    return static_cast<const bNodeSocketValueRGBA *>(socket->default_value)->value[0];
  }
  return fallback;
}

static bool socket_rgba(const bNodeSocket *socket, float r_color[4])
{
  if (socket == nullptr || socket->default_value == nullptr || socket->type != SOCK_RGBA) {
    return false;
  }
  const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(socket->default_value);
  for (int i = 0; i < 4; i++) {
    r_color[i] = value->value[i];
  }
  return true;
}

/** A linear Map Range on the way to Principled Roughness, BAKED.
 *
 * three.js samples one roughness map and multiplies it by a constant; it has
 * no node in between. Blender's Map Range is therefore resolved here, into a
 * DERIVED greyscale picture the presenter treats as an ordinary image with its
 * own name -- the same reduction the session's Python exporter made, moved to
 * the door that already holds the pixels. */
struct RoughnessRemap {
  bool present = false;
  float from_min = 0.0f;
  float from_max = 1.0f;
  float to_min = 0.0f;
  float to_max = 1.0f;
  bool clamp = false;
};

struct StandardTexture {
  bool present = false;
  std::string image_name;
  std::string uv;
  const char *extension = "REPEAT";
  bool has_tint = false;
  float tint[3] = {1.0f, 1.0f, 1.0f};
  long long image_revision = 0;
  Image *image = nullptr;
};

struct StandardMaterial {
  std::string name;
  float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float roughness = 0.5f;
  float metallic = 0.0f;
  float transmission = 0.0f;
  /* Principled's own default in 5.2, which is what an unlinked socket reads. */
  float ior = 1.45f;
  float coat = 0.0f;
  float coat_roughness = 0.03f;
  float coat_ior = 1.5f;
  float coat_tint[4] = {1, 1, 1, 1};
  float sheen = 0.0f;
  float sheen_roughness = 0.5f;
  float sheen_tint[4] = {1, 1, 1, 1};
  float anisotropy = 0.0f;
  float anisotropy_rotation = 0.0f;
  float specular_level = 0.5f;
  float specular_tint[4] = {1, 1, 1, 1};
  float film_thickness = 0.0f;
  float film_ior = 1.33f;
  StandardTexture texture;
  StandardTexture normal_texture;
  float normal_strength = 1.0f;
  bool normal_object_space = false;
  bool normal_directx = false;
  /* A Roughness image, straight or through a baked linear Map Range; when it
   * is present `roughness` is the multiplier and the reduction sends 1. */
  StandardTexture roughness_texture;
  RoughnessRemap roughness_remap;
  bool has_emission = false;
  float emission_color[3] = {0.0f, 0.0f, 0.0f};
  float emission_strength = 0.0f;
};

/** A MULTIPLY mix as `(image node, constant colour)`, or null. */
static const bNode *multiply_operands(const Material *material,
                                      const bNodeTree *tree,
                                      const bNode *node,
                                      float r_tint[3])
{
  int blend = -1;
  if (STREQ(node->idname, "ShaderNodeMixRGB")) {
    blend = node->custom1;
  }
  else if (node->storage != nullptr) {
    blend = static_cast<const NodeShaderMix *>(node->storage)->blend_type;
  }
  if (blend != MA_RAMP_MULT) {
    unreached(std::string(material->id.name + 2) +
              ": Base Color mixes with a blend type that is not MULTIPLY; a standard material "
              "multiplies its map by its colour, so only MULTIPLY reduces to one");
    return nullptr;
  }
  for (const char *factor_name : {"Factor", "Fac"}) {
    const bNodeSocket *factor = find_input(node, factor_name);
    if (factor == nullptr) {
      continue;
    }
    if (incoming(tree, node, factor_name) != nullptr) {
      unreached(std::string(material->id.name + 2) + ": Base Color mix has a linked factor");
      return nullptr;
    }
    const float value = socket_float(factor, 1.0f);
    if (fabsf(value - 1.0f) > 1e-6f) {
      unreached(std::string(material->id.name + 2) +
                ": Base Color mix factor is not 1; only 1 is a plain multiply");
      return nullptr;
    }
    break;
  }
  const bNode *linked_source = nullptr;
  const bNodeSocket *constant_socket = nullptr;
  int linked_count = 0, constant_count = 0, operand_count = 0;
  for (const char *operand : {"A", "B", "Color1", "Color2"}) {
    const bNodeSocket *socket = find_input(node, operand);
    if (socket == nullptr || socket->type != SOCK_RGBA) {
      continue;
    }
    operand_count++;
    const bNodeLink *link = incoming(tree, node, operand);
    if (link != nullptr) {
      linked_count++;
      linked_source = link->fromnode;
    }
    else {
      constant_count++;
      constant_socket = socket;
    }
  }
  if (linked_count != 1 || constant_count != 1) {
    unreached(std::string(material->id.name + 2) +
              ": Base Color mix needs exactly one linked and one constant colour; it has " +
              std::to_string(linked_count) + " linked of " + std::to_string(operand_count));
    return nullptr;
  }
  float constant[4];
  if (socket_rgba(constant_socket, constant)) {
    for (int i = 0; i < 3; i++) {
      r_tint[i] = constant[i];
    }
  }
  return linked_source;
}

/** The UV map an Image Texture reads, by following its Vector input. */
static std::string texture_uv_map(const bNodeTree *tree, const bNode *node)
{
  const bNodeLink *link = incoming(tree, node, "Vector");
  if (link == nullptr || link->fromnode == nullptr) {
    return "";
  }
  if (!STREQ(link->fromnode->idname, "ShaderNodeUVMap") || link->fromnode->storage == nullptr) {
    return "";
  }
  return std::string(static_cast<const NodeShaderUVMap *>(link->fromnode->storage)->uv_map);
}

static void reduce_texture(const Material *material,
                           const bNodeTree *tree,
                           const bNode *node,
                           const char *socket_label,
                           StandardTexture &out)
{
  float tint[3] = {1.0f, 1.0f, 1.0f};
  bool has_tint = false;
  if (STREQ(node->idname, "ShaderNodeMixRGB") || STREQ(node->idname, "ShaderNodeMix")) {
    const bNode *source = multiply_operands(material, tree, node, tint);
    if (source == nullptr) {
      return;
    }
    has_tint = true;
    node = source;
  }
  if (!STREQ(node->idname, "ShaderNodeTexImage")) {
    unreached(std::string(material->id.name + 2) + ": " + socket_label + " is linked to " +
              node->idname +
              "; a standard material holds one Image Texture, optionally multiplied by a "
              "constant colour, and the constant is what is drawn");
    return;
  }
  if (node->id == nullptr) {
    unreached(std::string(material->id.name + 2) + ": the " + socket_label +
              " Image Texture has no image");
    return;
  }
  int extension = SHD_IMAGE_EXTENSION_REPEAT;
  if (node->storage != nullptr) {
    extension = static_cast<const NodeTexImage *>(node->storage)->extension;
  }
  out.present = true;
  out.image = reinterpret_cast<Image *>(node->id);
  out.image_name = std::string(node->id->name + 2);
  out.uv = texture_uv_map(tree, node);
  out.extension = extension == SHD_IMAGE_EXTENSION_EXTEND  ? "EXTEND" :
                  extension == SHD_IMAGE_EXTENSION_MIRROR ? "MIRROR" :
                  extension == SHD_IMAGE_EXTENSION_CLIP   ? "CLIP" :
                                                            "REPEAT";
  out.has_tint = has_tint;
  for (int i = 0; i < 3; i++) {
    out.tint[i] = tint[i];
  }
}

/** Blender's Mix Shader takes the FIRST shader at Fac 0 and the SECOND at
 *  Fac 1 (`node_shader_mix_shader.cc`); `socket_index` is that 1-based input. */
static const bNode *mix_shader_input(const bNodeTree *tree,
                                     const bNode *node,
                                     const int socket_index)
{
  int index = 0;
  for (const bNodeSocket &socket : node->inputs) {
    if (!socket_available(&socket)) {
      continue;
    }
    if (index == socket_index) {
      for (const bNodeLink &link : tree->links) {
        if (link.tosock == &socket && link.tonode == node) {
          return link.fromnode;
        }
      }
      return nullptr;
    }
    index++;
  }
  return nullptr;
}

/** The FACTOR socket of a Mix Shader, and whatever drives it. */
static const bNodeSocket *mix_shader_factor(const bNode *node)
{
  for (const bNodeSocket &socket : node->inputs) {
    if (socket_available(&socket)) {
      return &socket;
    }
  }
  return nullptr;
}

/** A Roughness input reduced to an image, straight or through a LINEAR Map
 *  Range this door bakes. Anything else is a warning naming the node, and the
 *  constant roughness is what is drawn. */
static void reduce_roughness(const Material *material,
                             const bNodeTree *tree,
                             const bNode *node,
                             StandardMaterial &out)
{
  if (STREQ(node->idname, "ShaderNodeMapRange")) {
    const NodeMapRange *storage = static_cast<const NodeMapRange *>(node->storage);
    if (storage == nullptr || storage->data_type != CD_PROP_FLOAT ||
        storage->interpolation_type != NODE_MAP_RANGE_LINEAR)
    {
      unreached(std::string(material->id.name + 2) +
                ": Roughness Map Range is not a linear float remap; the constant roughness is "
                "what is drawn");
      return;
    }
    const char *bounds[4] = {"From Min", "From Max", "To Min", "To Max"};
    float values[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; i++) {
      if (incoming(tree, node, bounds[i]) != nullptr) {
        unreached(std::string(material->id.name + 2) +
                  ": Roughness Map Range has a linked bound; the constant roughness is what is "
                  "drawn");
        return;
      }
      values[i] = socket_float(find_input(node, bounds[i]), values[i]);
    }
    const bNodeLink *value_link = incoming(tree, node, "Value");
    if (value_link == nullptr || value_link->fromnode == nullptr) {
      unreached(std::string(material->id.name + 2) +
                ": Roughness Map Range Value is empty; the constant roughness is what is drawn");
      return;
    }
    out.roughness_remap.present = true;
    out.roughness_remap.from_min = values[0];
    out.roughness_remap.from_max = values[1];
    out.roughness_remap.to_min = values[2];
    out.roughness_remap.to_max = values[3];
    out.roughness_remap.clamp = (storage->clamp != 0);
    node = value_link->fromnode;
  }
  reduce_texture(material, tree, node, "Roughness", out.roughness_texture);
  if (!out.roughness_texture.present) {
    out.roughness_remap.present = false;
    return;
  }
  if (out.roughness_remap.present) {
    /* A DERIVED picture, named by the remap that made it: the presenter holds
     * it as an ordinary image and the source keeps its own identity, so one
     * texture remapped two ways is two pictures and never one overwritten. */
    char suffix[128];
    SNPRINTF(suffix,
             "@roughness(%g,%g,%g,%g%s)",
             double(out.roughness_remap.from_min),
             double(out.roughness_remap.from_max),
             double(out.roughness_remap.to_min),
             double(out.roughness_remap.to_max),
             out.roughness_remap.clamp ? ",clamp" : "");
    out.roughness_texture.image_name += suffix;
  }
  /* three multiplies the map by `roughness`; the reduction sends the map. */
  out.roughness = 1.0f;
}

static StandardMaterial reduce_material(const Material *material)
{
  StandardMaterial out;
  out.name = std::string(material->id.name + 2);
  const bNodeTree *tree = material->nodetree;
  if (tree == nullptr) {
    /* NO TREE: the material IS its viewport values. */
    out.color[0] = material->r;
    out.color[1] = material->g;
    out.color[2] = material->b;
    out.color[3] = material->a;
    out.roughness = material->roughness;
    out.metallic = material->metallic;
    return out;
  }
  const bNode *output = nullptr;
  int output_count = 0;
  for (const bNode &node : tree->nodes) {
    if (!STREQ(node.idname, "ShaderNodeOutputMaterial")) {
      continue;
    }
    output_count++;
    if (output == nullptr || (node.flag & NODE_DO_OUTPUT) != 0) {
      if (output == nullptr || (output->flag & NODE_DO_OUTPUT) == 0) {
        output = &node;
      }
    }
  }
  if (output == nullptr) {
    unreached(out.name + " has no material output; the object colour is what is drawn");
    return out;
  }
  if (output_count > 1) {
    unreached(out.name + ": the shader tree has " + std::to_string(output_count) +
              " material outputs; the active one is drawn");
  }
  const bNodeLink *surface_link = incoming(tree, output, "Surface");
  if (surface_link == nullptr || surface_link->fromnode == nullptr) {
    unreached(out.name + ": nothing is linked to Surface; the object colour is what is drawn");
    return out;
  }
  const bNode *surface = surface_link->fromnode;

  /* A MIX SHADER SPLIT BY A LIGHT PATH is two surfaces and the camera sees one
   * of them: `Is Shadow Ray` is 0 on a camera ray (the first shader),
   * `Is Camera Ray` is 1 (the second). Any other factor mixes two BSDFs per
   * pixel, which a standard material cannot draw, and is refused by name. */
  while (STREQ(surface->idname, "ShaderNodeMixShader")) {
    const bNodeSocket *factor = mix_shader_factor(surface);
    const bNodeLink *factor_link = nullptr;
    for (const bNodeLink &link : tree->links) {
      if (link.tosock == factor && link.tonode == surface) {
        factor_link = &link;
        break;
      }
    }
    int side = 0;
    if (factor_link == nullptr) {
      const float value = socket_float(factor, 0.5f);
      side = value == 0.0f ? 1 : (value == 1.0f ? 2 : 0);
      if (side == 0) {
        unreached(out.name + ": Mix Shader is mixed by a constant " + std::to_string(value) +
                  "; the object colour is what is drawn");
        return out;
      }
    }
    else if (factor_link->fromnode != nullptr &&
             STREQ(factor_link->fromnode->idname, "ShaderNodeLightPath") &&
             factor_link->fromsock != nullptr)
    {
      if (STREQ(factor_link->fromsock->name, "Is Shadow Ray")) {
        side = 1;
      }
      else if (STREQ(factor_link->fromsock->name, "Is Camera Ray")) {
        side = 2;
      }
    }
    if (side == 0) {
      unreached(out.name + ": Mix Shader is mixed by " +
                (factor_link->fromnode ? factor_link->fromnode->idname : "nothing") +
                "; only a Light Path ray query separates the camera's surface from the others");
      return out;
    }
    const bNode *chosen = mix_shader_input(tree, surface, side);
    if (chosen == nullptr) {
      unreached(out.name + ": Mix Shader input " + std::to_string(side) + " is empty");
      return out;
    }
    surface = chosen;
  }

  if (STREQ(surface->idname, "ShaderNodeBsdfTransparent")) {
    float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    socket_rgba(find_input(surface, "Color"), colour);
    for (int i = 0; i < 3; i++) {
      out.color[i] = colour[i];
    }
    out.color[3] = 0.0f;
    return out;
  }

  if (STREQ(surface->idname, "ShaderNodeEmission")) {
    /* AN EMISSION SURFACE IS A LIGHT WITH NO SHADING: black under lights, its
     * own colour times strength on top -- three's `emissive`. */
    if (incoming(tree, surface, "Color") != nullptr) {
      unreached(out.name + ": Emission Color is linked; a standard material holds a constant "
                           "there, and the object colour is what is drawn");
      return out;
    }
    if (incoming(tree, surface, "Strength") != nullptr) {
      unreached(out.name + ": Emission Strength is linked; a standard material holds a constant "
                           "there, and the object colour is what is drawn");
      return out;
    }
    float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    socket_rgba(find_input(surface, "Color"), colour);
    out.has_emission = true;
    for (int i = 0; i < 3; i++) {
      out.emission_color[i] = colour[i];
      out.color[i] = 0.0f;
    }
    out.color[3] = 1.0f;
    out.emission_strength = socket_float(find_input(surface, "Strength"), 1.0f);
    return out;
  }

  const bNode *bsdf = surface;
  if (!STREQ(bsdf->idname, "ShaderNodeBsdfPrincipled")) {
    unreached(out.name + ": the surface is " + bsdf->idname +
              "; the viewport draws a Principled BSDF, and this material is drawn with its "
              "object colour instead");
    return out;
  }
  float base[4];
  if (socket_rgba(find_input(bsdf, "Base Color"), base)) {
    for (int i = 0; i < 3; i++) {
      out.color[i] = base[i];
    }
  }
  /* Principled opacity is a separate socket; Base Color's fourth component does
   * not control surface coverage in Blender. */
  out.color[3] = socket_float(find_input(bsdf, "Alpha"), 1.0f);
  if (incoming(tree, bsdf, "Alpha") != nullptr) {
    unreached(out.name +
              ": Principled Alpha is linked; a standard material holds a constant there, and "
              "the constant is what is drawn");
  }
  struct {
    const char *socket;
    float *target;
  } const carried[] = {{"Metallic", &out.metallic},
                       {"Transmission Weight", &out.transmission},
                       {"IOR", &out.ior},
                       {"Coat Weight", &out.coat},
                       {"Coat Roughness", &out.coat_roughness},
                       {"Coat IOR", &out.coat_ior},
                       {"Sheen Weight", &out.sheen},
                       {"Sheen Roughness", &out.sheen_roughness},
                       {"Anisotropic", &out.anisotropy},
                       {"Anisotropic Rotation", &out.anisotropy_rotation},
                       {"Specular IOR Level", &out.specular_level},
                       {"Thin Film Thickness", &out.film_thickness},
                       {"Thin Film IOR", &out.film_ior}};
  for (const auto &entry : carried) {
    const bNodeSocket *socket = find_input(bsdf, entry.socket);
    if (socket != nullptr) {
      *entry.target = socket_float(socket, *entry.target);
    }
    if (incoming(tree, bsdf, entry.socket) != nullptr) {
      unreached(out.name + ": Principled " + entry.socket +
                " is linked; a standard material holds a constant there, and the constant is "
                "what is drawn");
    }
  }
  for (const auto &entry : {std::pair<const char *, float *>{"Coat Tint", out.coat_tint},
                           {"Sheen Tint", out.sheen_tint},
                           {"Specular Tint", out.specular_tint}})
  {
    socket_rgba(find_input(bsdf, entry.first), entry.second);
    if (incoming(tree, bsdf, entry.first) != nullptr) {
      unreached(out.name + ": Principled " + entry.first + " is linked; only its constant is drawn");
    }
  }
  /* ROUGHNESS: a constant, or a picture (optionally through a baked linear
   * Map Range). */
  const bNodeLink *roughness_link = incoming(tree, bsdf, "Roughness");
  if (roughness_link == nullptr || roughness_link->fromnode == nullptr) {
    out.roughness = socket_float(find_input(bsdf, "Roughness"), out.roughness);
  }
  else {
    reduce_roughness(material, tree, roughness_link->fromnode, out);
  }
  const bNodeLink *normal_link = incoming(tree, bsdf, "Normal");
  if (normal_link != nullptr && normal_link->fromnode != nullptr) {
    const bNode *normal = normal_link->fromnode;
    const NodeShaderNormalMap *settings = STREQ(normal->idname, "ShaderNodeNormalMap") ?
        static_cast<const NodeShaderNormalMap *>(normal->storage) : nullptr;
    if (settings != nullptr && (settings->space == SHD_SPACE_TANGENT || settings->space == SHD_SPACE_OBJECT)) {
      const bNodeLink *image = incoming(tree, normal, "Color");
      if (image != nullptr && image->fromnode != nullptr) {
        reduce_texture(material, tree, image->fromnode, "Normal Map Color", out.normal_texture);
      }
      out.normal_strength = socket_float(find_input(normal, "Strength"), 1.0f);
      out.normal_object_space = settings->space == SHD_SPACE_OBJECT;
      out.normal_directx = settings->convention == SHD_NORMAL_MAP_CONVENTION_DIRECTX;
      if (incoming(tree, normal, "Strength") != nullptr) {
        unreached(out.name + ": Normal Map Strength is linked; only its constant is drawn");
      }
    }
    else {
      unreached(out.name + ": Normal requires a tangent/object-space Normal Map; received " + normal->idname);
    }
  }
  /* PRINCIPLED EMISSION: a constant colour and strength become `emissive`; a
   * strength of 0 (the default) is nothing to carry. */
  if (incoming(tree, bsdf, "Emission Strength") != nullptr ||
      incoming(tree, bsdf, "Emission Color") != nullptr)
  {
    unreached(out.name + ": Principled Emission is linked; a standard material holds constants "
                         "there, and no emission is drawn");
  }
  else {
    const float strength = socket_float(find_input(bsdf, "Emission Strength"), 0.0f);
    if (strength > 0.0f) {
      float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      socket_rgba(find_input(bsdf, "Emission Color"), colour);
      out.has_emission = true;
      out.emission_strength = strength;
      for (int i = 0; i < 3; i++) {
        out.emission_color[i] = colour[i];
      }
    }
  }
  /* A LINKED BASE COLOUR IS NOT A DEFAULT VALUE: reading the constant past a
   * link is how an authored texture vanishes. */
  const bNodeLink *base_link = incoming(tree, bsdf, "Base Color");
  if (base_link != nullptr && base_link->fromnode != nullptr) {
    reduce_texture(material, tree, base_link->fromnode, "Base Color", out.texture);
  }
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name A picture's own change record
 *
 * A `pixels` write with NO `update_tag()` leaves NOTHING in the depsgraph:
 * `rna_Image_pixels_set` marks a full PARTIAL-UPDATE CHANGESET on the image and
 * tags no ID. A door that reads only the depsgraph therefore never re-ships a
 * repainted texture -- the gap this reads Blender's own changeset to close.
 * One reader per image, kept for the session; `collect_changes` advances the
 * reader even when it answers FullUpdateNeeded, so "anything but
 * NoChangesDetected is a change" settles after one frame instead of re-shipping
 * every picture forever.
 * \{ */

static std::unordered_map<const Image *, PartialUpdateUser *> g_image_watchers;

static void forget_image_watchers()
{
  for (const std::pair<const Image *const, PartialUpdateUser *> &entry : g_image_watchers) {
    if (entry.second != nullptr) {
      BKE_image_partial_update_free(entry.second);
    }
  }
  g_image_watchers.clear();
}

static bool image_pixels_changed(Image *image)
{
  namespace partial = bke::image::partial_update;
  PartialUpdateUser *&user = g_image_watchers[image];
  if (user == nullptr) {
    user = BKE_image_partial_update_create(image);
  }
  const partial::ePartialUpdateCollectResult result =
      partial::BKE_image_partial_update_collect_changes(image, user);
  if (result == partial::ePartialUpdateCollectResult::PartialChangesDetected) {
    partial::PartialUpdateRegion region;
    /* WHERE it changed is not this door's business -- the presenter takes a
     * whole picture -- but draining the list is what leaves the reader clean. */
    while (partial::BKE_image_partial_update_get_next_change(user, &region) ==
           partial::ePartialUpdateIterResult::ChangeAvailable)
    {
    }
    return true;
  }
  return result == partial::ePartialUpdateCollectResult::FullUpdateNeeded;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The frame
 * \{ */

static const char *object_type_name(const short type)
{
  switch (type) {
    case OB_MESH:
      return "MESH";
    case OB_CURVES_LEGACY:
      return "CURVE";
    case OB_SURF:
      return "SURFACE";
    case OB_FONT:
      return "FONT";
    case OB_MBALL:
      return "META";
    case OB_ARMATURE:
      return "ARMATURE";
    case OB_LATTICE:
      return "LATTICE";
    case OB_VOLUME:
      return "VOLUME";
    case OB_CAMERA:
      return "CAMERA";
    case OB_LAMP:
      return "LIGHT";
    case OB_SPEAKER:
      return "SPEAKER";
    case OB_GREASE_PENCIL:
      return "GREASEPENCIL";
    case OB_POINTCLOUD:
      return "POINTCLOUD";
    case OB_LIGHTPROBE:
      return "LIGHT_PROBE";
    default:
      return "EMPTY";
  }
}

/** The active object's mode, as `bpy.types.Object.mode` spells it.
 *
 * Read from the ORIGINAL object: the evaluated copy is the depsgraph's, and the
 * mode is the session's own state. A frame that always said OBJECT is how the
 * presenter never learns the session entered Edit Mode. */
static const char *object_mode_name(const Object *object)
{
  if (object == nullptr) {
    return "OBJECT";
  }
  const Object *original = reinterpret_cast<const Object *>(
      DEG_get_original_id(const_cast<ID *>(&object->id)));
  const int mode = original != nullptr ? original->mode : object->mode;
  switch (mode) {
    case OB_MODE_EDIT:
      return "EDIT";
    case OB_MODE_SCULPT:
      return "SCULPT";
    case OB_MODE_VERTEX_PAINT:
      return "VERTEX_PAINT";
    case OB_MODE_WEIGHT_PAINT:
      return "WEIGHT_PAINT";
    case OB_MODE_TEXTURE_PAINT:
      return "TEXTURE_PAINT";
    case OB_MODE_PARTICLE_EDIT:
      return "PARTICLE_EDIT";
    case OB_MODE_POSE:
      return "POSE";
    default:
      return "OBJECT";
  }
}

/** `nullptr` for a light three.js has no lamp for; the caller warns by name. */
static const char *light_type_name(const short type)
{
  switch (type) {
    case LA_SUN:
      return "SUN";
    case LA_SPOT:
      return "SPOT";
    case LA_AREA:
      return "AREA";
    case LA_LOCAL:
      return "POINT";
    default:
      return nullptr;
  }
}

struct Resolved {
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
  Depsgraph *depsgraph = nullptr;
};

/** The session's scene, view layer and PERSISTENT depsgraph.
 *
 * Persistent is the whole point: the revision map is only meaningful against a
 * graph that survives between calls, because `id->recalc` is what one
 * evaluation left behind for the next reader. `BKE_scene_ensure_depsgraph`
 * hands back the scene's own, which is the one `bpy.context` also answers
 * with. */
static Resolved resolve()
{
  Resolved out;
  out.bmain = G_MAIN;
  if (out.bmain == nullptr) {
    return out;
  }
  wmWindowManager *wm = static_cast<wmWindowManager *>(out.bmain->wm.first);
  if (wm != nullptr) {
    wmWindow *window = static_cast<wmWindow *>(wm->windows.first);
    if (window != nullptr && window->scene != nullptr) {
      out.scene = window->scene;
    }
  }
  if (out.scene == nullptr) {
    out.scene = static_cast<Scene *>(out.bmain->scenes.first);
  }
  if (out.scene == nullptr) {
    return out;
  }
  out.view_layer = static_cast<ViewLayer *>(out.scene->view_layers.first);
  if (out.view_layer == nullptr) {
    return out;
  }
  out.depsgraph = BKE_scene_ensure_depsgraph(out.bmain, out.scene, out.view_layer);
  return out;
}

/** The presenter's name for one attribute type.
 *
 * `nullptr` is a type `MeshColumns` has no column shape for; the layer is
 * WARNED by name and left out, never shipped as something else.
 */
static const char *attribute_type_name(const bke::AttrType type)
{
  switch (type) {
    case bke::AttrType::Float:
      return "FLOAT";
    case bke::AttrType::Int32:
      return "INT";
    case bke::AttrType::Int8:
      return "INT8";
    case bke::AttrType::Bool:
      return "BOOLEAN";
    case bke::AttrType::Float2:
      return "FLOAT2";
    case bke::AttrType::Float3:
      return "FLOAT_VECTOR";
    case bke::AttrType::ColorFloat:
      return "FLOAT_COLOR";
    case bke::AttrType::ColorByte:
      return "BYTE_COLOR";
    case bke::AttrType::Quaternion:
      return "QUATERNION";
    case bke::AttrType::Float4x4:
      return "FLOAT4X4";
    case bke::AttrType::Float4:
      return "FLOAT4";
    case bke::AttrType::Int32_2D:
      return "INT32_2D";
    case bke::AttrType::Int16_2D:
      return "INT16_2D";
    default:
      return nullptr;
  }
}

static const char *attribute_domain_name(const bke::AttrDomain domain)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      return "POINT";
    case bke::AttrDomain::Edge:
      return "EDGE";
    case bke::AttrDomain::Face:
      return "FACE";
    case bke::AttrDomain::Corner:
      return "CORNER";
    default:
      return nullptr;
  }
}

/** One attribute layer's values, materialized contiguously into the arena. */
template<typename T>
static ColumnRef attribute_column(const bke::AttributeAccessor &attributes,
                                  const StringRef name,
                                  const bke::AttrDomain domain,
                                  const char *dtype,
                                  const int stride,
                                  const int count)
{
  const bke::AttributeReader<T> reader = attributes.lookup<T>(name, domain);
  if (!reader) {
    return ColumnRef();
  }
  const VArraySpan<T> values = *reader;
  return arena_write(values.data(), values.size() * sizeof(T), dtype, size_t(count), stride);
}

/** A boolean layer as the presenter's one-byte flag column; an absent layer is
 *  the RNA default (false for every flag `MeshColumns` carries), so the column
 *  is always the mesh's full length and never missing. */
static ColumnRef flag_column(const bke::AttributeAccessor &attributes,
                             const StringRef name,
                             const bke::AttrDomain domain,
                             const int count,
                             const bool invert)
{
  const VArray<bool> values = *attributes.lookup_or_default<bool>(name, domain, false);
  std::vector<uint8_t> bytes(size_t(count), uint8_t(0));
  for (int i = 0; i < count; i++) {
    const bool value = values[i];
    bytes[size_t(i)] = uint8_t((invert ? !value : value) ? 1 : 0);
  }
  return arena_write(bytes.data(), bytes.size(), "u8", size_t(count), 1);
}

/** The mesh columns, written into the arena and named in the frame.
 *
 * The column NAMES, their dtypes and the `counts` block are `MeshColumns`
 * (`packages/mesh/src/mesh-store.ts`) as the presenter's frame schema spells
 * it, because `drawArraysFromColumns` is what draws them. Every one of the
 * sixteen is always present: an absent Blender layer is its RNA default, not a
 * missing column, and the schema is strict in both directions.
 *
 * NO NORMAL COLUMN. The presenter derives corner normals itself from `smooth`,
 * `edgeSharp` and the `custom_normal` layer (which arrives through the generic
 * attributes below), so a normal column here would be bytes nothing reads.
 */
static void write_mesh(std::string &out, const Mesh &mesh, const long long revision)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  const Span<int> face_offsets = mesh.face_offsets();
  const Span<int2> edges = mesh.edges();
  const int nv = int(positions.size());
  const int nf = int(face_offsets.is_empty() ? 0 : face_offsets.size() - 1);
  const int nc = int(corner_verts.size());
  const int ne = int(edges.size());

  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArray<int> material_index = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);

  std::vector<float> co(size_t(nv) * 3);
  for (int i = 0; i < nv; i++) {
    co[size_t(i) * 3 + 0] = positions[i].x;
    co[size_t(i) * 3 + 1] = positions[i].y;
    co[size_t(i) * 3 + 2] = positions[i].z;
  }
  std::vector<uint32_t> corner(size_t(nc), 0u);
  std::vector<int32_t> corner_edge(size_t(nc), 0);
  for (int i = 0; i < nc; i++) {
    corner[size_t(i)] = uint32_t(corner_verts[i]);
    corner_edge[size_t(i)] = corner_edges.is_empty() ? -1 : corner_edges[i];
  }
  std::vector<uint32_t> face_start(size_t(nf) + 1);
  for (int i = 0; i <= nf; i++) {
    face_start[size_t(i)] = uint32_t(face_offsets.is_empty() ? 0 : face_offsets[i]);
  }
  std::vector<uint32_t> edge(size_t(ne) * 2);
  for (int i = 0; i < ne; i++) {
    edge[size_t(i) * 2 + 0] = uint32_t(edges[i][0]);
    edge[size_t(i) * 2 + 1] = uint32_t(edges[i][1]);
  }
  std::vector<uint32_t> material(size_t(nf), 0u);
  for (int i = 0; i < nf; i++) {
    const int index = material_index[i];
    material[size_t(i)] = uint32_t(index < 0 ? 0 : index);
  }
  /* Edge crease has been an ordinary attribute layer since 4.0; absent is zero. */
  std::vector<float> edge_crease(size_t(ne), 0.0f);
  {
    const VArray<float> values = *attributes.lookup_or_default<float>(
        "crease_edge", bke::AttrDomain::Edge, 0.0f);
    for (int i = 0; i < ne; i++) {
      edge_crease[size_t(i)] = values[i];
    }
  }

  const ColumnRef co_ref = arena_write(co.data(), co.size() * 4, "f32", size_t(nv), 3);
  /* Evaluated normals are Blender's, including smooth fans, sharp edges and
   * custom normals. UV splits in the presenter must not recompute them. */
  const Span<float3> corner_normals = mesh.corner_normals();
  const ColumnRef corner_normal_ref = arena_write(
      corner_normals.data(), corner_normals.size() * sizeof(float3), "f32", size_t(nc), 3);
  const ColumnRef vert_select_ref = flag_column(
      attributes, ".select_vert", bke::AttrDomain::Point, nv, false);
  const ColumnRef vert_hide_ref = flag_column(
      attributes, ".hide_vert", bke::AttrDomain::Point, nv, false);
  const ColumnRef edge_ref = arena_write(edge.data(), edge.size() * 4, "u32", size_t(ne), 2);
  const ColumnRef edge_sharp_ref = flag_column(
      attributes, "sharp_edge", bke::AttrDomain::Edge, ne, false);
  const ColumnRef edge_seam_ref = flag_column(
      attributes, "uv_seam", bke::AttrDomain::Edge, ne, false);
  const ColumnRef edge_select_ref = flag_column(
      attributes, ".select_edge", bke::AttrDomain::Edge, ne, false);
  const ColumnRef edge_hide_ref = flag_column(
      attributes, ".hide_edge", bke::AttrDomain::Edge, ne, false);
  const ColumnRef edge_crease_ref = arena_write(
      edge_crease.data(), edge_crease.size() * 4, "f32", size_t(ne), 1);
  const ColumnRef face_start_ref = arena_write(
      face_start.data(), face_start.size() * 4, "u32", size_t(nf) + 1, 1);
  const ColumnRef material_ref = arena_write(
      material.data(), material.size() * 4, "u32", size_t(nf), 1);
  /* `smooth` is the INVERSE of Blender's `sharp_face`, the way
   * `MeshPolygon.use_smooth` reads it. */
  const ColumnRef smooth_ref = flag_column(
      attributes, "sharp_face", bke::AttrDomain::Face, nf, true);
  const ColumnRef face_select_ref = flag_column(
      attributes, ".select_poly", bke::AttrDomain::Face, nf, false);
  const ColumnRef face_hide_ref = flag_column(
      attributes, ".hide_poly", bke::AttrDomain::Face, nf, false);
  const ColumnRef corner_ref = arena_write(corner.data(), corner.size() * 4, "u32", size_t(nc), 1);
  const ColumnRef corner_edge_ref = arena_write(
      corner_edge.data(), corner_edge.size() * 4, "i32", size_t(nc), 1);

  /* EVERY attribute layer, as the generic columns `MeshColumns.attributes`
   * holds: the UV maps `drawArraysFromColumns` picks from by name, the
   * `custom_normal` layer it shades with, colour layers, and whatever else the
   * mesh carries. A layer whose type or domain has no column shape is a
   * warning naming it, never a silent drop. */
  std::string attributes_json;
  bool first_attribute = true;
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    const char *domain_name = attribute_domain_name(iter.domain);
    const char *type_name = attribute_type_name(iter.data_type);
    const std::string layer(iter.name.c_str());
    if (domain_name == nullptr) {
      unreached("mesh attribute " + layer + " is on domain " +
                std::to_string(int(iter.domain)) + ", which a mesh column has no length for");
      return;
    }
    if (type_name == nullptr) {
      unreached("mesh attribute " + layer + " has type " + std::to_string(int(iter.data_type)) +
                ", which the presenter's attribute columns do not hold");
      return;
    }
    const int count = iter.domain == bke::AttrDomain::Point ? nv :
                      iter.domain == bke::AttrDomain::Edge  ? ne :
                      iter.domain == bke::AttrDomain::Face  ? nf :
                                                              nc;
    ColumnRef ref;
    switch (iter.data_type) {
      case bke::AttrType::Float:
        ref = attribute_column<float>(attributes, iter.name, iter.domain, "f32", 1, count);
        break;
      case bke::AttrType::Int32:
        ref = attribute_column<int>(attributes, iter.name, iter.domain, "i32", 1, count);
        break;
      case bke::AttrType::Int8:
        ref = attribute_column<int8_t>(attributes, iter.name, iter.domain, "i8", 1, count);
        break;
      case bke::AttrType::Bool:
        ref = flag_column(attributes, iter.name, iter.domain, count, false);
        break;
      case bke::AttrType::Float2:
        ref = attribute_column<float2>(attributes, iter.name, iter.domain, "f32", 2, count);
        break;
      case bke::AttrType::Float3:
        ref = attribute_column<float3>(attributes, iter.name, iter.domain, "f32", 3, count);
        break;
      case bke::AttrType::ColorFloat:
        ref = attribute_column<ColorGeometry4f>(attributes, iter.name, iter.domain, "f32", 4, count);
        break;
      case bke::AttrType::ColorByte:
        ref = attribute_column<ColorGeometry4b>(attributes, iter.name, iter.domain, "u8", 4, count);
        break;
      case bke::AttrType::Quaternion:
        ref = attribute_column<math::Quaternion>(attributes, iter.name, iter.domain, "f32", 4, count);
        break;
      case bke::AttrType::Float4x4:
        ref = attribute_column<float4x4>(attributes, iter.name, iter.domain, "f32", 16, count);
        break;
      case bke::AttrType::Float4:
        ref = attribute_column<float4>(attributes, iter.name, iter.domain, "f32", 4, count);
        break;
      case bke::AttrType::Int32_2D:
        ref = attribute_column<int2>(attributes, iter.name, iter.domain, "i32", 2, count);
        break;
      case bke::AttrType::Int16_2D:
        ref = attribute_column<short2>(attributes, iter.name, iter.domain, "i16", 2, count);
        break;
      default:
        break;
    }
    if (!ref.present) {
      unreached("mesh attribute " + layer + " (" + type_name + ") could not be read");
      return;
    }
    if (!first_attribute) {
      attributes_json += ",";
    }
    first_attribute = false;
    attributes_json += "{\"name\":";
    json_escape(attributes_json, layer.c_str());
    attributes_json += ",\"domain\":";
    json_escape(attributes_json, domain_name);
    attributes_json += ",\"type\":";
    json_escape(attributes_json, type_name);
    attributes_json += ",\"data\":";
    bool only = true;
    std::string entry;
    json_column(entry, "x", ref, only);
    /* `json_column` writes `"x":{...}`; only the object is wanted here. */
    attributes_json += entry.substr(entry.find(':') + 1);
    attributes_json += "}";
  });

  const StringRefNull active_uv = mesh.active_uv_map_name();
  const StringRefNull render_uv = mesh.default_uv_map_name();

  out += "{\"revision\":";
  json_int(out, revision);
  out += ",\"counts\":{\"verts\":";
  json_int(out, nv);
  out += ",\"edges\":";
  json_int(out, ne);
  out += ",\"faces\":";
  json_int(out, nf);
  out += ",\"corners\":";
  json_int(out, nc);
  out += "},\"activeUv\":";
  if (active_uv.is_empty()) {
    out += "null";
  }
  else {
    json_escape(out, active_uv.c_str());
  }
  out += ",\"renderUv\":";
  if (render_uv.is_empty()) {
    out += "null";
  }
  else {
    json_escape(out, render_uv.c_str());
  }
  out += ",\"columns\":{";
  bool first = true;
  json_column(out, "co", co_ref, first);
  json_column(out, "cornerNormal", corner_normal_ref, first);
  json_column(out, "faceStart", face_start_ref, first);
  json_column(out, "corner", corner_ref, first);
  json_column(out, "cornerEdge", corner_edge_ref, first);
  json_column(out, "edge", edge_ref, first);
  json_column(out, "edgeSharp", edge_sharp_ref, first);
  json_column(out, "edgeSeam", edge_seam_ref, first);
  json_column(out, "edgeCrease", edge_crease_ref, first);
  json_column(out, "material", material_ref, first);
  json_column(out, "smooth", smooth_ref, first);
  json_column(out, "vertSelect", vert_select_ref, first);
  json_column(out, "vertHide", vert_hide_ref, first);
  json_column(out, "edgeSelect", edge_select_ref, first);
  json_column(out, "edgeHide", edge_hide_ref, first);
  json_column(out, "faceSelect", face_select_ref, first);
  json_column(out, "faceHide", face_hide_ref, first);
  out += "},\"attributes\":[" + attributes_json + "]}";
}

/** Rec.709 luma: what a colour image means as a single roughness value. */
static const float g_luma[3] = {0.2126f, 0.7152f, 0.0722f};

static uint8_t quantize(const float value)
{
  const float scaled = std::isfinite(value) ? value * 255.0f + 0.5f : 0.0f;
  return uint8_t(scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled));
}

/** One picture as raw RGBA in BLENDER'S OWN ROW ORDER, v=0 first: the page
 *  builds a `DataTexture` straight from these bytes and GL's bottom-left
 *  origin puts v=0 where Blender puts it, so nothing flips.
 *
 *  False means the image has no readable pixels; the caller leaves it out of
 *  the frame rather than shipping a zero-sized one, because the presenter's
 *  raster schema has no such thing. */
static bool write_image(std::string &out,
                        Image *image,
                        const long long revision,
                        const RoughnessRemap &remap)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf == nullptr || ibuf->x <= 0 || ibuf->y <= 0) {
    BKE_image_release_ibuf(image, ibuf, lock);
    unreached(std::string("image ") + (image->id.name + 2) + " has no readable pixels");
    return false;
  }
  const int width = ibuf->x;
  const int height = ibuf->y;
  const size_t texels = size_t(width) * size_t(height);
  std::vector<uint8_t> rgba(texels * 4, uint8_t(0));
  if (ibuf->byte_buffer.data != nullptr) {
    memcpy(rgba.data(), ibuf->byte_buffer.data, texels * 4);
  }
  else if (ibuf->float_buffer.data != nullptr) {
    const float *source = ibuf->float_buffer.data;
    const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
    for (size_t i = 0; i < texels; i++) {
      for (int c = 0; c < 4; c++) {
        const float value = c < channels ? source[i * size_t(channels) + size_t(c)] :
                                           (c == 3 ? 1.0f : 0.0f);
        rgba[i * 4 + size_t(c)] = quantize(value);
      }
    }
  }
  else {
    BKE_image_release_ibuf(image, ibuf, lock);
    unreached(std::string("image ") + (image->id.name + 2) + " has no readable pixels");
    return false;
  }
  BKE_image_release_ibuf(image, ibuf, lock);

  /* Blender's own colour space for the picture: an sRGB image is linearised by
   * the presenter's sampler, a Non-Color one is data. A BAKED roughness map is
   * always data -- it is a number per texel, not a colour. */
  const char *colorspace = remap.present ? "data" :
                           STREQ(image->colorspace_settings.name, "sRGB") ? "sRGB" :
                                                                            "data";
  if (remap.present) {
    const float from_span = (remap.from_max - remap.from_min) == 0.0f ?
                                1.0f :
                                (remap.from_max - remap.from_min);
    const float low = remap.to_min < remap.to_max ? remap.to_min : remap.to_max;
    const float high = remap.to_min < remap.to_max ? remap.to_max : remap.to_min;
    for (size_t i = 0; i < texels; i++) {
      const float value = (float(rgba[i * 4 + 0]) / 255.0f) * g_luma[0] +
                          (float(rgba[i * 4 + 1]) / 255.0f) * g_luma[1] +
                          (float(rgba[i * 4 + 2]) / 255.0f) * g_luma[2];
      float mapped = remap.to_min + (value - remap.from_min) / from_span *
                                        (remap.to_max - remap.to_min);
      if (remap.clamp) {
        mapped = mapped < low ? low : (mapped > high ? high : mapped);
      }
      const uint8_t grey = quantize(mapped);
      rgba[i * 4 + 0] = grey;
      rgba[i * 4 + 1] = grey;
      rgba[i * 4 + 2] = grey;
      rgba[i * 4 + 3] = 255;
    }
  }

  const ColumnRef rgba_ref = arena_write(rgba.data(), rgba.size(), "u8", texels, 4);
  out += "{\"revision\":";
  json_int(out, revision);
  out += ",\"width\":";
  json_int(out, width);
  out += ",\"height\":";
  json_int(out, height);
  out += ",\"colorspace\":";
  json_escape(out, colorspace);
  out += ",\"rgba\":";
  bool first = true;
  std::string column;
  json_column(column, "x", rgba_ref, first);
  /* `json_column` writes `"x":{...}`; only the object is wanted here. */
  out += column.substr(column.find(':') + 1);
  out += "}";
  return true;
}

static void write_matrix(std::string &out, const float4x4 &matrix)
{
  /* ROW major, the way `bpy`'s `matrix_world` reads: `m[row][col]`, while
   * Blender's own float4x4 stores columns. */
  out += "[";
  for (int row = 0; row < 4; row++) {
    if (row) {
      out += ",";
    }
    out += "[";
    for (int col = 0; col < 4; col++) {
      if (col) {
        out += ",";
      }
      json_float(out, matrix[col][row]);
    }
    out += "]";
  }
  out += "]";
}

static std::string export_frame(const char *options_json)
{
  const Options options = parse_options(options_json);
  g_arena.clear();
  std::vector<std::string> warnings;
  g_warnings = &warnings;
  ensure_callbacks();
  for (const std::string &key : options.unknown_keys) {
    unreached("options: unrecognized key '" + key +
              "'; the held-revision map is 'known', and an unrecognized key is "
              "ignored, so this call paid for every column it already had");
  }

  const Resolved resolved = resolve();
  if (resolved.depsgraph == nullptr) {
    g_warnings = nullptr;
    return "{\"error\":\"no scene\"}";
  }
  /* EVALUATE WITHOUT CLEARING, THEN CLEAR AFTER READING -- the render engine's
   * own pattern, and the only one that can see the update record at all.
   * `BKE_scene_graph_update_tagged` ends with `DEG_ids_clear_recalc`, so a door
   * calling it reads an EMPTY record every time and concludes that nothing ever
   * changes: measured here, a moved object and an edited mesh both reported
   * `updated: []`. `scene.cc` names this case itself -- "can be skipped for
   * example renderers that will read these and clear the flags later" -- so the
   * door does what `RE_engine` does: the same evaluation, minus the clear. */
  if (options.evaluate) {
    BKE_main_view_layers_synced_ensure(resolved.bmain);
    DEG_graph_relations_update(resolved.depsgraph);
    DEG_evaluate_on_refresh(resolved.depsgraph, DEG_EVALUATE_SYNC_WRITEBACK_YES);
  }

  /* OUR OWN evaluation's record, added to everything the callback already
   * accumulated from evaluations this door did not run. `DEG_evaluate_on_refresh`
   * does not fire `DEPSGRAPH_UPDATE_POST` itself, so this call is what covers
   * the door's own work; the callback covers everybody else's. */
  accumulate_updates(resolved.depsgraph);

  std::unordered_set<const ID *> dirty;
  std::unordered_set<const ID *> geometry_dirty;
  for (const std::pair<const ID *const, uint32_t> &entry : g_session.pending) {
    dirty.insert(entry.first);
    if (entry.second & uint32_t(ID_RECALC_GEOMETRY)) {
      geometry_dirty.insert(entry.first);
    }
  }
  g_session.pending.clear();
  for (const ID *id : dirty) {
    g_session.id_revision[id] += 1;
  }
  /* The record has been read; the next evaluation starts from a clean slate. */
  DEG_ids_clear_recalc(resolved.depsgraph, false);

  g_session.frame_revision += 1;
  if (!options.session.empty()) {
    g_session.id = options.session;
  }
  if (g_session.id.empty()) {
    g_session.id = "native";
  }

  Scene *scene_eval = DEG_get_evaluated_scene(resolved.depsgraph);
  ViewLayer *view_layer_eval = DEG_get_evaluated_view_layer(resolved.depsgraph);
  BKE_view_layer_synced_ensure(*resolved.bmain, scene_eval, view_layer_eval);

  std::string objects_json;
  std::string meshes_json;
  std::string lights_json;
  std::unordered_map<std::string, StandardMaterial> materials;
  std::unordered_map<std::string, std::pair<Image *, RoughnessRemap>> images;
  std::unordered_set<std::string> seen_meshes;
  std::vector<std::string> mesh_order;
  std::unordered_set<std::string> seen_lights;
  bool first_object = true, first_mesh = true, first_light = true;

  for (Base &base_ref : *BKE_view_layer_object_bases_get(view_layer_eval)) {
    Base *base = &base_ref;
    Object *object = base->object;
    if (object == nullptr) {
      continue;
    }
    const ID *original = DEG_get_original_id(&object->id);
    const std::string name(object->id.name + 2);

    /* The GEOMETRY KEY: the mesh datablock's name when the evaluated geometry
     * IS that datablock, and the object's own name when evaluation produced
     * something else (a modifier, a deform). Two objects sharing one unmodified
     * mesh therefore share one set of columns, and a deformed one gets its
     * own. */
    std::string geometry_key;
    Mesh *mesh_eval = BKE_object_get_evaluated_mesh(object);
    if (mesh_eval != nullptr) {
      const ID *mesh_original = DEG_get_original_id(&mesh_eval->id);
      const bool is_datablock = (object->data != nullptr) &&
                                (mesh_original == DEG_get_original_id(
                                                      static_cast<ID *>(object->data)));
      geometry_key = is_datablock ? std::string(mesh_original->name + 2) : name;
      const bool moved = geometry_dirty.count(original) ||
                         geometry_dirty.count(mesh_original) ||
                         (object->data != nullptr &&
                          geometry_dirty.count(DEG_get_original_id(static_cast<ID *>(object->data))));
      long long &revision = g_session.geometry_revision[geometry_key];
      if (revision == 0 || moved) {
        revision += 1;
      }
      if (!seen_meshes.count(geometry_key)) {
        seen_meshes.insert(geometry_key);
        mesh_order.push_back(geometry_key);
        if (!first_mesh) {
          meshes_json += ",";
        }
        first_mesh = false;
        json_escape(meshes_json, geometry_key.c_str());
        meshes_json += ":";
        const auto known = options.known.find("mesh:" + geometry_key);
        if (known != options.known.end() && known->second == revision) {
          meshes_json += "{\"revision\":";
          json_int(meshes_json, revision);
          meshes_json += ",\"unchanged\":true}";
        }
        else {
          write_mesh(meshes_json, *mesh_eval, revision);
        }
      }
    }

    std::string light_key;
    if (object->type == OB_LAMP && object->data != nullptr) {
      const Light *light = reinterpret_cast<const Light *>(object->data);
      const std::string light_name(light->id.name + 2);
      const char *kind = light_type_name(light->type);
      if (kind == nullptr) {
        unreached("viewport light type " + std::to_string(int(light->type)) +
                  " (" + light_name + ") has no equivalent here; it is not drawn");
      }
      else {
        light_key = light_name;
      }
      if (kind != nullptr && seen_lights.insert(light_name).second) {
        if (!first_light) {
          lights_json += ",";
        }
        first_light = false;
        json_escape(lights_json, light_name.c_str());
        lights_json += ":{\"type\":";
        json_escape(lights_json, kind);
        lights_json += ",\"color\":[";
        json_float(lights_json, light->r);
        lights_json += ",";
        json_float(lights_json, light->g);
        lights_json += ",";
        json_float(lights_json, light->b);
        /* `Light.exposure` is stops on top of Power, the way `energy` reads
         * through RNA; the presenter takes one number. */
        lights_json += "],\"energy\":";
        json_float(lights_json, light->energy * powf(2.0f, light->exposure));
        lights_json += ",\"use_shadow\":";
        lights_json += (light->mode & LA_SHADOW) ? "true" : "false";
        /* EACH SHAPE FIELD ONLY WHERE BLENDER HAS ONE: a sun has no radius and
         * a point light no area size, and the presenter reads what is there. */
        if (light->type == LA_LOCAL || light->type == LA_SPOT) {
          lights_json += ",\"radius\":";
          json_float(lights_json, light->radius);
        }
        if (light->type == LA_SPOT) {
          lights_json += ",\"spot_size\":";
          json_float(lights_json, light->spotsize);
          lights_json += ",\"spot_blend\":";
          json_float(lights_json, light->spotblend);
        }
        if (light->type == LA_AREA) {
          lights_json += ",\"shape\":";
          json_escape(lights_json,
                      light->area_shape == LA_AREA_RECT    ? "RECTANGLE" :
                      light->area_shape == LA_AREA_DISK    ? "DISK" :
                      light->area_shape == LA_AREA_ELLIPSE ? "ELLIPSE" :
                                                             "SQUARE");
          lights_json += ",\"size\":";
          json_float(lights_json, light->area_size);
          lights_json += ",\"size_y\":";
          json_float(lights_json, light->area_sizey);
        }
        lights_json += "}";
      }
    }

    if (!first_object) {
      objects_json += ",";
    }
    first_object = false;
    objects_json += "{\"id\":";
    json_escape(objects_json, name.c_str());
    objects_json += ",\"name\":";
    json_escape(objects_json, name.c_str());
    objects_json += ",\"type\":";
    json_escape(objects_json, object_type_name(object->type));
    objects_json += ",\"mesh\":";
    if (geometry_key.empty()) {
      objects_json += "null";
    }
    else {
      json_escape(objects_json, geometry_key.c_str());
    }
    /* NAMED ONLY WHEN IT IS IN THE FRAME: a light whose type was refused above
     * is not in `lights`, and an object pointing at a light the presenter does
     * not have is a lookup that silently finds nothing. */
    objects_json += ",\"light\":";
    if (light_key.empty()) {
      objects_json += "null";
    }
    else {
      json_escape(objects_json, light_key.c_str());
    }
    objects_json += ",\"materials\":[";
    for (int slot = 0; slot < object->totcol; slot++) {
      if (slot) {
        objects_json += ",";
      }
      Material *material = BKE_object_material_get_eval(object, short(slot + 1));
      if (material == nullptr) {
        objects_json += "null";
        continue;
      }
      const Material *material_original = reinterpret_cast<const Material *>(
          DEG_get_original_id(&material->id));
      const std::string material_name(material_original->id.name + 2);
      json_escape(objects_json, material_name.c_str());
      if (!materials.count(material_name)) {
        materials[material_name] = reduce_material(material_original);
      }
    }
    objects_json += "],\"matrix\":";
    write_matrix(objects_json, object->object_to_world());
    objects_json += ",\"visible\":";
    objects_json += (base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) ? "true" : "false";
    objects_json += ",\"render_visible\":";
    objects_json += (base->flag & BASE_ENABLED_RENDER) ? "true" : "false";
    objects_json += ",\"selected\":";
    objects_json += (base->flag & BASE_SELECTED) ? "true" : "false";
    objects_json += ",\"parent\":";
    if (object->parent != nullptr) {
      json_escape(objects_json, object->parent->id.name + 2);
    }
    else {
      objects_json += "null";
    }
    objects_json += "}";
  }

  /* Materials, and the images their Base Colour names. */
  std::string materials_json;
  /* One changeset read per image per frame, however many slots name it. */
  std::unordered_set<const Image *> repainted;
  bool first_material = true;
  for (auto &entry : materials) {
    StandardMaterial &material = entry.second;
    /* Every picture a material names, wanted at the revision its SOURCE
     * datablock is at -- a derived (remapped) roughness map is the source
     * image seen through a formula, so it moves exactly when the source does. */
    struct {
      StandardTexture *texture;
      const RoughnessRemap *remap;
    } const wanted[] = {{&material.texture, nullptr},
                        {&material.normal_texture, nullptr},
                        {&material.roughness_texture, &material.roughness_remap}};
    for (const auto &entry : wanted) {
      StandardTexture &texture = *entry.texture;
      if (!texture.present || texture.image == nullptr) {
        continue;
      }
      images[texture.image_name] = {texture.image,
                                    entry.remap ? *entry.remap : RoughnessRemap()};
      if (repainted.insert(texture.image).second && image_pixels_changed(texture.image)) {
        g_session.id_revision[&texture.image->id] += 1;
      }
      texture.image_revision = g_session.id_revision[&texture.image->id];
      if (texture.image_revision == 0) {
        texture.image_revision = 1;
        g_session.id_revision[&texture.image->id] = 1;
      }
    }
    if (!first_material) {
      materials_json += ",";
    }
    first_material = false;
    json_escape(materials_json, material.name.c_str());
    materials_json += ":{\"name\":";
    json_escape(materials_json, material.name.c_str());
    materials_json += ",\"color\":[";
    for (int i = 0; i < 4; i++) {
      if (i) {
        materials_json += ",";
      }
      json_float(materials_json, material.color[i]);
    }
    materials_json += "],\"roughness\":";
    json_float(materials_json, material.roughness);
    materials_json += ",\"metallic\":";
    json_float(materials_json, material.metallic);
    materials_json += ",\"transmission\":";
    json_float(materials_json, material.transmission);
    materials_json += ",\"ior\":";
    json_float(materials_json, material.ior);
    materials_json += ",\"physical\":{";
    bool first_physical = true;
    for (const auto &value : {std::pair<const char *, float>{"coat", material.coat},
                             {"coat_roughness", material.coat_roughness},
                             {"coat_ior", material.coat_ior},
                             {"sheen", material.sheen},
                             {"sheen_roughness", material.sheen_roughness},
                             {"anisotropy", material.anisotropy},
                             {"anisotropy_rotation", material.anisotropy_rotation},
                             {"specular_level", material.specular_level},
                             {"film_thickness", material.film_thickness},
                             {"film_ior", material.film_ior}})
    {
      if (!first_physical) materials_json += ",";
      first_physical = false;
      json_escape(materials_json, value.first);
      materials_json += ":";
      json_float(materials_json, value.second);
    }
    for (const auto &value : {std::pair<const char *, const float *>{"coat_tint", material.coat_tint},
                             {"sheen_tint", material.sheen_tint},
                             {"specular_tint", material.specular_tint}})
    {
      materials_json += ",";
      json_escape(materials_json, value.first);
      materials_json += ":[";
      for (int i = 0; i < 3; i++) {
        if (i) materials_json += ",";
        json_float(materials_json, value.second[i]);
      }
      materials_json += "]";
    }
    materials_json += "}";
    for (const auto &named : {std::pair<const char *, const StandardTexture *>{
                                  "normal_texture", &material.normal_texture},
                              std::pair<const char *, const StandardTexture *>{
                                  "texture", &material.texture},
                              std::pair<const char *, const StandardTexture *>{
                                  "roughness_texture", &material.roughness_texture}})
    {
      const StandardTexture &texture = *named.second;
      if (!texture.present) {
        continue;
      }
      materials_json += ",";
      json_escape(materials_json, named.first);
      materials_json += ":{\"image\":{\"name\":";
      json_escape(materials_json, texture.image_name.c_str());
      materials_json += ",\"revision\":";
      json_int(materials_json, texture.image_revision);
      materials_json += "},\"extension\":";
      json_escape(materials_json, texture.extension);
      materials_json += ",\"uv\":";
      json_escape(materials_json, texture.uv.c_str());
      if (texture.has_tint) {
        materials_json += ",\"tint\":[";
        for (int i = 0; i < 3; i++) {
          if (i) {
            materials_json += ",";
          }
          json_float(materials_json, texture.tint[i]);
        }
        materials_json += "]";
      }
      materials_json += "}";
    }
    materials_json += ",\"normal_strength\":";
    json_float(materials_json, material.normal_strength);
    materials_json += ",\"normal_space\":";
    json_escape(materials_json, material.normal_object_space ? "OBJECT" : "TANGENT");
    materials_json += ",\"normal_directx\":";
    materials_json += material.normal_directx ? "true" : "false";
    if (material.has_emission) {
      materials_json += ",\"emission\":{\"color\":[";
      for (int i = 0; i < 3; i++) {
        if (i) {
          materials_json += ",";
        }
        json_float(materials_json, material.emission_color[i]);
      }
      materials_json += "],\"strength\":";
      json_float(materials_json, material.emission_strength);
      materials_json += "}";
    }
    materials_json += "}";
  }

  std::string images_json;
  bool first_image = true;
  for (auto &entry : images) {
    const long long revision = g_session.id_revision[&entry.second.first->id];
    const auto known = options.known.find("image:" + entry.first);
    if (known != options.known.end() && known->second == revision) {
      /* The presenter holds these bytes at this revision; a picture it already
       * has is one the frame leaves out. There is no "unchanged image" form --
       * the material's texture reference carries the revision that names it. */
      continue;
    }
    std::string described;
    if (!write_image(described, entry.second.first, revision, entry.second.second)) {
      continue;
    }
    if (!first_image) {
      images_json += ",";
    }
    first_image = false;
    json_escape(images_json, entry.first.c_str());
    images_json += ":" + described;
  }

  std::sort(mesh_order.begin(), mesh_order.end());
  std::string updated_json;
  for (size_t i = 0; i < mesh_order.size(); i++) {
    if (i) {
      updated_json += ",";
    }
    json_escape(updated_json, mesh_order[i].c_str());
  }

  Object *active = BKE_view_layer_active_object_get(view_layer_eval);

  std::string out;
  out.reserve(objects_json.size() + meshes_json.size() + materials_json.size() + 1024);
  out += "{\"session\":";
  json_escape(out, g_session.id.c_str());
  out += ",\"revision\":";
  json_int(out, g_session.frame_revision);
  out += ",\"frame\":";
  json_int(out, scene_eval->r.cfra);
  out += ",\"mode\":";
  json_escape(out, object_mode_name(active));
  out += ",\"active\":";
  if (active != nullptr) {
    json_escape(out, active->id.name + 2);
  }
  else {
    out += "null";
  }
  /* NO ARENA DESCRIPTOR IN THE FRAME. The arena's base and size are C exports
   * (`blender_web_export_buffer` / `_buffer_size`) and natively the path is the
   * caller's own string, so a `buffer` key here would be a fact the reader
   * already has -- and the presenter's frame schema is strict. */
  out += ",\"updated\":[" + updated_json + "],\"objects\":[" + objects_json + "],\"meshes\":{" +
         meshes_json + "},\"materials\":{" + materials_json + "},\"images\":{" + images_json +
         "},\"lights\":{" + lights_json + "},\"warnings\":[";
  for (size_t i = 0; i < warnings.size(); i++) {
    if (i) {
      out += ",";
    }
    json_escape(out, warnings[i].c_str());
  }
  out += "]}";

  if (!options.buffer_path.empty()) {
    FILE *file = fopen(options.buffer_path.c_str(), "wb");
    if (file != nullptr) {
      if (!g_arena.empty()) {
        fwrite(g_arena.data(), 1, g_arena.size(), file);
      }
      fclose(file);
    }
  }
  g_warnings = nullptr;
  return out;
}

/** \} */

}  // namespace blender::web_export

/* -------------------------------------------------------------------- */
/** \name The C door
 * \{ */

extern "C" const char *blender_web_export_frame(const char *options_json);
extern "C" const uint8_t *blender_web_export_buffer();
extern "C" size_t blender_web_export_buffer_size();
extern "C" void blender_web_export_session_reset();

const char *blender_web_export_frame(const char *options_json)
{
  blender::web_export::g_frame = blender::web_export::export_frame(options_json);
  return blender::web_export::g_frame.c_str();
}

const uint8_t *blender_web_export_buffer()
{
  return blender::web_export::g_arena.data();
}

size_t blender_web_export_buffer_size()
{
  return blender::web_export::g_arena.size();
}

void blender_web_export_session_reset()
{
  /* The callbacks stay registered: they are process-global and adding them a
   * second time would record every update twice. */
  blender::web_export::g_session = blender::web_export::Session();
  blender::web_export::g_warned.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The Python binding
 *
 * `export_frame` is what the session calls; `buffer` is the native proof's way
 * to read the arena (in the browser the JS side reads it off the heap through
 * the C exports instead).
 * \{ */

static PyObject *py_export_frame(PyObject * /*self*/, PyObject *args)
{
  const char *options_json = nullptr;
  if (!PyArg_ParseTuple(args, "|z:export_frame", &options_json)) {
    return nullptr;
  }
  const char *frame = blender_web_export_frame(options_json);
  return PyUnicode_FromString(frame);
}

static PyObject *py_buffer(PyObject * /*self*/, PyObject * /*args*/)
{
  return PyBytes_FromStringAndSize(
      reinterpret_cast<const char *>(blender_web_export_buffer()),
      Py_ssize_t(blender_web_export_buffer_size()));
}

static PyObject *py_buffer_size(PyObject * /*self*/, PyObject * /*args*/)
{
  return PyLong_FromSize_t(blender_web_export_buffer_size());
}

static PyObject *py_session_reset(PyObject * /*self*/, PyObject * /*args*/)
{
  blender_web_export_session_reset();
  Py_RETURN_NONE;
}

static PyMethodDef web_methods[] = {
    {"export_frame", py_export_frame, METH_VARARGS, "The export door: one frame as JSON."},
    {"buffer", py_buffer, METH_NOARGS, "The side arena the last frame's columns live in."},
    {"buffer_size", py_buffer_size, METH_NOARGS,
     "How many bytes the last frame's columns came to -- the accounting number, "
     "without copying them."},
    {"session_reset", py_session_reset, METH_NOARGS, "Forget every revision."},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef web_module = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "_blender_web",
    /*m_doc*/ "Blender's scene, read out for a web presenter.",
    /*m_size*/ -1,
    /*m_methods*/ web_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

/* Declared inside `namespace blender` by `bpy_interface.cc`'s module table,
 * so the definition has to live there too -- a global-scope twin links as a
 * different symbol and the executable fails at the very last step. */
namespace blender {

PyObject *BPyInit_blender_web();
PyObject *BPyInit_blender_web()
{
  /* Armed at IMPORT, not at first frame: an update that happens before the
   * first `export_frame` still has to be recorded. */
  web_export::ensure_callbacks();
  return PyModule_Create(&web_module);
}

}  // namespace blender

/** \} */
