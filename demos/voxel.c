/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "demos/voxel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"
#include "demos/gl.h"

/* World shader: pre-lit face colors blended into sky fog over [14..28] units. */
static const char *WORLD_VERT_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec3 u_cam_pos;\n"
    "out vec3 v_color;\n"
    "out float v_dist;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "  v_color = a_color;\n"
    "  v_dist = length(a_pos - u_cam_pos);\n"
    "}\n";

static const char *WORLD_FRAG_SRC =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "in float v_dist;\n"
    "uniform vec3 u_sky;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  float fog = clamp((v_dist - 14.0) / 14.0, 0.0, 1.0);\n"
    "  frag_color = vec4(mix(v_color, u_sky, fog), 1.0);\n"
    "}\n";

/* Minimal 2D NDC shader for the center-screen crosshair overlay. */
static const char *HUD_VERT_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *HUD_FRAG_SRC =
    "#version 330 core\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  frag_color = vec4(1.0, 1.0, 1.0, 0.9);\n"
    "}\n";

/**
 * Object handling functions.
 */

static VoxelWorld* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (VoxelWorld*)update_self_stack(&top, s, VOXEL_WORLD);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static VoxelWorld* bind_voxel(GenericEl *w) {
  return self(w);
}

static void del(void *obj) {
  VoxelWorld *w = (VoxelWorld*)obj;
  if (w != NULL && self((GenericEl*)w) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      /* Release chunk meshes before `world_shader` so shared refs unwind first. */
      release(slf->chunks);
      release(slf->hud_gl);
      release(slf->world_shader);
      release(slf->edits);
      release(slf->mesh_verts);
      VoxelWorld *to_free = slf;
      self((GenericEl*)-1);
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * Terrain generation, voxel lookup, and chunk-local edit persistence.
 */

/*
 * Euclidean floor division (`floor(a / n)` for `n > 0`) so negative world
 * coordinates step cleanly into chunk indices `-1, -2, ...` instead of
 * mirroring around zero.
 */
static int floor_div(int a, int n) {
  int q = a / n;
  int r = a % n;
  if (r < 0) {
    q--;
  }
  return q;
}

/* Y-major flat index within a 16x16x16 chunk's `TypedCharArray`. */
static int local_idx(int lx, int ly, int lz) {
  return (ly * CH_VOXEL_CHUNK_Z + lz) * CH_VOXEL_CHUNK_X + lx;
}

/* Deterministic integer hash for reproducible procedural tree placement. */
static unsigned int hash_col(int wx, int wz) {
  unsigned int x = (unsigned int)wx * 73856093u;
  unsigned int z = (unsigned int)wz * 19349663u;
  unsigned int h = x ^ z;
  h ^= h >> 13;
  h *= 1274126177u;
  return h ^ (h >> 16);
}

/* Smooth procedural surface height clamped within `[2, CHUNK_Y - 7]` for trees. */
static int compute_terrain_height(int wx, int wz) {
  double h = 4.5 +
             2.2 * sin(wx * 0.16) * cos(wz * 0.14) +
             1.4 * sin((wx + wz) * 0.11);
  int top = (int)floor(h + 0.5);
  if (top < 2) {
    top = 2;
  } else if (top >= CH_VOXEL_CHUNK_Y - 6) {
    top = CH_VOXEL_CHUNK_Y - 6;
  }
  return top;
}

/* Places sparse trees strictly on dry land above water level (`y = 3`). */
static int has_tree_at(int wx, int wz) {
  if (compute_terrain_height(wx, wz) <= 3) {
    return 0;
  }
  return (hash_col(wx, wz) % 89u) == 0u;
}

/*
 * Looks up a chunk slot in the 3x3 sliding window (`rx, rz` in `[0..2]`).
 * Slot `(1, 1)` always holds `(w->center_cx, w->center_cz)`.
 */
static Hash* slot_at(VoxelWorld *w, int rx, int rz) {
  if (rx < 0 || rx >= CH_VOXEL_GRID_SIZE || rz < 0 || rz >= CH_VOXEL_GRID_SIZE) {
    return NULL;
  }
  return as(Hash, $(w->chunks)->get(rz * CH_VOXEL_GRID_SIZE + rx));
}

/* Returns the block ID at world `(wx, wy, wz)`, or `AIR` outside loaded 3x3. */
static int read_world_block(VoxelWorld *w, int wx, int wy, int wz) {
  if (wy < 0 || wy >= CH_VOXEL_CHUNK_Y) {
    return CH_BLOCK_AIR;
  }
  int cx = floor_div(wx, CH_VOXEL_CHUNK_X);
  int cz = floor_div(wz, CH_VOXEL_CHUNK_Z);
  Hash *slot = slot_at(w, cx - w->center_cx + 1, cz - w->center_cz + 1);
  if (slot == NULL) {
    return CH_BLOCK_AIR;
  }
  TypedCharArray *blocks = as(TypedCharArray, $(slot)->get("blocks"));
  int lx = wx - cx * CH_VOXEL_CHUNK_X;
  int lz = wz - cz * CH_VOXEL_CHUNK_Z;
  return (int)(unsigned char)blocks->data[local_idx(lx, wy, lz)];
}

static int is_solid_at(VoxelWorld *w, int wx, int wy, int wz) {
  int b = read_world_block(w, wx, wy, wz);
  return b != CH_BLOCK_AIR && b != CH_BLOCK_WATER;
}

typedef struct {
  Hash *chunk_edits;
  TypedCharArray *blocks;
} EditApplyCtx;

static void apply_chunk_edit_cb(GenericEl *key_el, void *ctx_ptr) {
  EditApplyCtx *ctx = (EditApplyCtx*)ctx_ptr;
  char *k = as(String, key_el)->chars;
  int lx, ly, lz;
  if (sscanf(k, "%d,%d,%d", &lx, &ly, &lz) == 3 &&
      lx >= 0 && lx < CH_VOXEL_CHUNK_X &&
      ly >= 0 && ly < CH_VOXEL_CHUNK_Y &&
      lz >= 0 && lz < CH_VOXEL_CHUNK_Z) {
    int val = (int)as(Number, $(ctx->chunk_edits)->get(k))->value();
    ctx->blocks->data[local_idx(lx, ly, lz)] = (char)(unsigned char)val;
  }
}

static void generate_chunk_blocks(VoxelWorld *w, Hash *slot, int cx, int cz) {
  TypedCharArray *blocks = as(TypedCharArray, $(slot)->get("blocks"));
  blocks->length = CH_VOXEL_CHUNK_VOLUME;
  $(slot)->put("cx", create(Number, (double)cx))
         ->put("cz", create(Number, (double)cz));

  /* Pass 1: base terrain strata (bedrock/stone, dirt, grass/sand, and water). */
  int lx, ly, lz;
  for (lz = 0; lz < CH_VOXEL_CHUNK_Z; lz++) {
    int wz = cz * CH_VOXEL_CHUNK_Z + lz;
    for (lx = 0; lx < CH_VOXEL_CHUNK_X; lx++) {
      int wx = cx * CH_VOXEL_CHUNK_X + lx;
      int top = compute_terrain_height(wx, wz);

      for (ly = 0; ly < CH_VOXEL_CHUNK_Y; ly++) {
        int b = CH_BLOCK_AIR;
        if (ly == 0 || ly < top - 1) {
          b = CH_BLOCK_STONE;
        } else if (ly < top) {
          b = CH_BLOCK_DIRT;
        } else if (ly == top) {
          b = (top <= 3) ? CH_BLOCK_SAND : CH_BLOCK_GRASS;
        } else if (ly <= 3) {
          b = CH_BLOCK_WATER;
        }
        blocks->data[local_idx(lx, ly, lz)] = (char)(unsigned char)b;
      }
    }
  }

  /*
   * Pass 2: overlay trees. Checking neighboring `(tx, tz)` in `[-1..1]` lets
   * leaf canopies spill seamlessly across chunk boundaries without needing
   * neighbor chunk data during generation.
   */
  for (lz = 0; lz < CH_VOXEL_CHUNK_Z; lz++) {
    int wz = cz * CH_VOXEL_CHUNK_Z + lz;
    for (lx = 0; lx < CH_VOXEL_CHUNK_X; lx++) {
      int wx = cx * CH_VOXEL_CHUNK_X + lx;
      int dz, dx;
      for (dz = -1; dz <= 1; dz++) {
        for (dx = -1; dx <= 1; dx++) {
          int tx = wx + dx;
          int tz = wz + dz;
          if (!has_tree_at(tx, tz)) {
            continue;
          }
          int ty = compute_terrain_height(tx, tz);
          if (dx == 0 && dz == 0) {
            int k;
            for (k = 1; k <= 3; k++) {
              if (ty + k < CH_VOXEL_CHUNK_Y) {
                blocks->data[local_idx(lx, ty + k, lz)] = CH_BLOCK_WOOD;
              }
            }
            if (ty + 5 < CH_VOXEL_CHUNK_Y) {
              blocks->data[local_idx(lx, ty + 5, lz)] = CH_BLOCK_LEAVES;
            }
          }
          int ky;
          for (ky = ty + 3; ky <= ty + 4; ky++) {
            if (ky < CH_VOXEL_CHUNK_Y &&
                blocks->data[local_idx(lx, ky, lz)] == CH_BLOCK_AIR) {
              blocks->data[local_idx(lx, ky, lz)] = CH_BLOCK_LEAVES;
            }
          }
        }
      }
    }
  }

  /* Pass 3: replay any persistent player edits bucketed under `(cx, cz)`. */
  char chunk_key[32];
  snprintf(chunk_key, sizeof(chunk_key), "%d,%d", cx, cz);
  GenericEl *bucket = $(w->edits)->get(chunk_key);
  if (bucket != NULL) {
    EditApplyCtx ctx;
    ctx.chunk_edits = as(Hash, bucket);
    ctx.blocks = blocks;
    $(ctx.chunk_edits->key_slice)->foreach_ctx(apply_chunk_edit_cb, &ctx);
  }
}

/**
 * Meshing using `TypedF32Array` and shared OpenGL shader program.
 */

static void block_rgb(int type, int face, float out[3]) {
  static const float base[8][3] = {
      {0.00f, 0.00f, 0.00f}, /* Air */
      {0.31f, 0.72f, 0.25f}, /* Grass */
      {0.54f, 0.37f, 0.23f}, /* Dirt */
      {0.52f, 0.54f, 0.57f}, /* Stone */
      {0.87f, 0.80f, 0.56f}, /* Sand */
      {0.46f, 0.30f, 0.16f}, /* Wood */
      {0.18f, 0.54f, 0.20f}, /* Leaves */
      {0.18f, 0.45f, 0.85f}, /* Water */
  };
  /* Directional face brightness factors (+Y top, -Y bottom, +/-X, +/-Z). */
  static const float shade[6] = {1.00f, 0.55f, 0.84f, 0.78f, 0.72f, 0.68f};

  int idx = (type >= 1 && type <= 7) ? type : 3;
  float r = base[idx][0];
  float g = base[idx][1];
  float b = base[idx][2];
  /* Grass blocks use grass green only on the top (+Y) face and dirt elsewhere. */
  if (type == CH_BLOCK_GRASS && face != 0) {
    r = base[CH_BLOCK_DIRT][0];
    g = base[CH_BLOCK_DIRT][1];
    b = base[CH_BLOCK_DIRT][2];
  }
  float k = shade[face];
  out[0] = r * k;
  out[1] = g * k;
  out[2] = b * k;
}

/* Emits one quad (two CCW triangles, 6 vertices x 6 floats) into `verts`. */
static void append_face(TypedF32Array *verts, int wx, int wy, int wz,
                        int face, int block_type) {
  static const float fv[6][6][3] = {
      {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {0, 1, 0}, {1, 1, 1}, {1, 1, 0}}, /* +Y */
      {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 0}, {1, 0, 1}, {0, 0, 1}}, /* -Y */
      {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 0}, {1, 1, 1}, {1, 0, 1}}, /* +X */
      {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 0}}, /* -X */
      {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 0, 1}, {1, 1, 1}, {0, 1, 1}}, /* +Z */
      {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, /* -Z */
  };

  float rgb[3];
  GLfloat face_verts[36];
  int i;
  block_rgb(block_type, face, rgb);
  for (i = 0; i < 6; i++) {
    face_verts[i * 6 + 0] = (GLfloat)wx + fv[face][i][0];
    face_verts[i * 6 + 1] = (GLfloat)wy + fv[face][i][1];
    face_verts[i * 6 + 2] = (GLfloat)wz + fv[face][i][2];
    face_verts[i * 6 + 3] = rgb[0];
    face_verts[i * 6 + 4] = rgb[1];
    face_verts[i * 6 + 5] = rgb[2];
  }
  $(verts)->append_buf(face_verts, 36);
}

static void rebuild_chunk_slot_mesh(VoxelWorld *w, int rx, int rz) {
  static const int dx[6] = {0, 0, 1, -1, 0, 0};
  static const int dy[6] = {1, -1, 0, 0, 0, 0};
  static const int dz[6] = {0, 0, 0, 0, 1, -1};

  Hash *slot = slot_at(w, rx, rz);
  if (slot == NULL) {
    return;
  }
  int cx = (int)as(Number, $(slot)->get("cx"))->value();
  int cz = (int)as(Number, $(slot)->get("cz"))->value();
  TypedCharArray *blocks = as(TypedCharArray, $(slot)->get("blocks"));
  Gl *chunk_gl = as(Gl, $(slot)->get("gl"));

  /* Reset length to 0 while keeping `w->mesh_verts->capacity` allocated. */
  $(w->mesh_verts)->from_buf(NULL, 0);

  /* Emit faces only where a block touches air (or solid touches water). */
  int ly, lz, lx, f;
  for (ly = 0; ly < CH_VOXEL_CHUNK_Y; ly++) {
    for (lz = 0; lz < CH_VOXEL_CHUNK_Z; lz++) {
      int wz = cz * CH_VOXEL_CHUNK_Z + lz;
      for (lx = 0; lx < CH_VOXEL_CHUNK_X; lx++) {
        int b = (int)(unsigned char)blocks->data[local_idx(lx, ly, lz)];
        if (b == CH_BLOCK_AIR) {
          continue;
        }
        int wx = cx * CH_VOXEL_CHUNK_X + lx;
        for (f = 0; f < 6; f++) {
          int nb = read_world_block(w, wx + dx[f], ly + dy[f], wz + dz[f]);
          if (nb == CH_BLOCK_AIR ||
              (nb == CH_BLOCK_WATER && b != CH_BLOCK_WATER)) {
            append_face(w->mesh_verts, wx, ly, wz, f, b);
          }
        }
      }
    }
  }

  $(chunk_gl)->mesh_buf(w->mesh_verts, 6)
             ->attr(0, 3, 0)
             ->attr(1, 3, 3);
}

/*
 * Slides the 3x3 chunk grid so `(new_cx, new_cz)` moves to center slot `[1][1]`.
 * Overlapping chunks keep their existing `Hash` slot and GPU VBO/VAO; slots
 * scrolled off-grid are recycled for newly exposed border chunks.
 */
static void shift_chunks(VoxelWorld *w, int new_cx, int new_cz) {
  int dcx = new_cx - w->center_cx;
  int dcz = new_cz - w->center_cz;
  if (dcx == 0 && dcz == 0) {
    return;
  }

  Hash *next_grid[CH_VOXEL_GRID_SIZE][CH_VOXEL_GRID_SIZE];
  Hash *recycled[CH_VOXEL_GRID_SLOTS];
  int is_new[CH_VOXEL_GRID_SIZE][CH_VOXEL_GRID_SIZE];
  int recycled_count = 0;
  int rz, rx;

  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      next_grid[rz][rx] = NULL;
      is_new[rz][rx] = 0;
    }
  }

  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      Hash *slot = slot_at(w, rx, rz);
      int nrx = rx - dcx;
      int nrz = rz - dcz;
      if (nrx >= 0 && nrx < CH_VOXEL_GRID_SIZE &&
          nrz >= 0 && nrz < CH_VOXEL_GRID_SIZE) {
        next_grid[nrz][nrx] = slot;
      } else {
        recycled[recycled_count++] = slot;
      }
    }
  }

  w->center_cx = new_cx;
  w->center_cz = new_cz;

  Slice *new_chunks = create(Slice, CH_VOXEL_GRID_SLOTS);
  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      Hash *slot = next_grid[rz][rx];
      if (slot == NULL) {
        slot = recycled[--recycled_count];
        generate_chunk_blocks(w, slot, new_cx + (rx - 1), new_cz + (rz - 1));
        is_new[rz][rx] = 1;
      }
      $(new_chunks)->add(slot);
    }
  }

  release(w->chunks);
  w->chunks = new_chunks->$$;
  w->chunks->bind((GenericEl*)-1);

  /*
   * Re-mesh newly generated border chunks plus adjacent surviving chunks whose
   * border face visibility changed when the outer world neighbor appeared.
   */
  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      int touches_new = is_new[rz][rx] ||
                        (rx > 0 && is_new[rz][rx - 1]) ||
                        (rx + 1 < CH_VOXEL_GRID_SIZE && is_new[rz][rx + 1]) ||
                        (rz > 0 && is_new[rz - 1][rx]) ||
                        (rz + 1 < CH_VOXEL_GRID_SIZE && is_new[rz + 1][rx]);
      if (touches_new) {
        rebuild_chunk_slot_mesh(w, rx, rz);
      }
    }
  }

  w->chunk_shifts++;
}

static void maybe_shift_player_chunk(VoxelWorld *w) {
  int cur_cx = floor_div((int)floorf(w->x), CH_VOXEL_CHUNK_X);
  int cur_cz = floor_div((int)floorf(w->z), CH_VOXEL_CHUNK_Z);
  if (cur_cx != w->center_cx || cur_cz != w->center_cz) {
    shift_chunks(w, cur_cx, cur_cz);
  }
}

static void write_world_block(VoxelWorld *w, int wx, int wy, int wz, int type) {
  if (wy <= 0 || wy >= CH_VOXEL_CHUNK_Y) {
    return;
  }
  int cx = floor_div(wx, CH_VOXEL_CHUNK_X);
  int cz = floor_div(wz, CH_VOXEL_CHUNK_Z);
  int rx = cx - w->center_cx + 1;
  int rz = cz - w->center_cz + 1;
  Hash *slot = slot_at(w, rx, rz);
  if (slot == NULL) {
    return;
  }

  int lx = wx - cx * CH_VOXEL_CHUNK_X;
  int lz = wz - cz * CH_VOXEL_CHUNK_Z;

  /*
   * Persist edits in a two-level `Hash` (`"cx,cz"` -> `"lx,ly,lz"` -> block ID)
   * so chunk regeneration only replays edits belonging to that chunk.
   */
  char chunk_key[32];
  char block_key[32];
  snprintf(chunk_key, sizeof(chunk_key), "%d,%d", cx, cz);
  snprintf(block_key, sizeof(block_key), "%d,%d,%d", lx, wy, lz);

  GenericEl *existing = $(w->edits)->get(chunk_key);
  Hash *chunk_edits;
  if (existing == NULL) {
    chunk_edits = create(Hash, 8);
    $(w->edits)->put(chunk_key, chunk_edits);
    chunk_edits = as(Hash, $(w->edits)->get(chunk_key));
  } else {
    chunk_edits = as(Hash, existing);
  }
  $(chunk_edits)->put(block_key, create(Number, (double)type));

  TypedCharArray *blocks = as(TypedCharArray, $(slot)->get("blocks"));
  blocks->data[local_idx(lx, wy, lz)] = (char)(unsigned char)type;

  /* Re-mesh this chunk and any neighbor whose shared border face is affected. */
  rebuild_chunk_slot_mesh(w, rx, rz);
  if (lx == 0) {
    rebuild_chunk_slot_mesh(w, rx - 1, rz);
  } else if (lx == CH_VOXEL_CHUNK_X - 1) {
    rebuild_chunk_slot_mesh(w, rx + 1, rz);
  }
  if (lz == 0) {
    rebuild_chunk_slot_mesh(w, rx, rz - 1);
  } else if (lz == CH_VOXEL_CHUNK_Z - 1) {
    rebuild_chunk_slot_mesh(w, rx, rz + 1);
  }
}

/**
 * 3D math, collision, raycasting, and Slice callbacks.
 */

static void mat4_mul(const GLfloat a[16], const GLfloat b[16], GLfloat out[16]) {
  int col, row, k;
  for (col = 0; col < 4; col++) {
    for (row = 0; row < 4; row++) {
      GLfloat sum = 0.0f;
      for (k = 0; k < 4; k++) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = sum;
    }
  }
}

/* Builds column-major `proj * view` MVP matrix from Euler `yaw` & `pitch`. */
static void build_mvp(float eye_x, float eye_y, float eye_z,
                      float yaw_deg, float pitch_deg, float aspect,
                      GLfloat mvp_out[16]) {
  float yr = yaw_deg * 0.0174532925f;
  float pr = pitch_deg * 0.0174532925f;
  /* Forward vector `f`, horizontal right vector `s = normalize(f x up)`, and `u`. */
  float fx = cosf(pr) * cosf(yr);
  float fy = sinf(pr);
  float fz = cosf(pr) * sinf(yr);

  float sx = -fz;
  float sz = fx;
  float slen = sqrtf(sx * sx + sz * sz);
  if (slen > 1e-6f) {
    sx /= slen;
    sz /= slen;
  } else {
    sx = 1.0f;
    sz = 0.0f;
  }
  float ux = -sz * fy;
  float uy = sz * fx - sx * fz;
  float uz = sx * fy;

  GLfloat view[16] = {
      sx,  ux, -fx, 0.0f,
      0.0f, uy, -fy, 0.0f,
      sz,  uz, -fz, 0.0f,
      -(sx * eye_x + sz * eye_z),
      -(ux * eye_x + uy * eye_y + uz * eye_z),
      (fx * eye_x + fy * eye_y + fz * eye_z),
      1.0f,
  };

  float znear = 0.1f;
  float zfar = 120.0f;
  float t = 1.0f / tanf(65.0f * 0.5f * 0.0174532925f);
  GLfloat proj[16] = {
      t / (aspect > 0.01f ? aspect : 1.0f), 0.0f, 0.0f, 0.0f,
      0.0f, t, 0.0f, 0.0f,
      0.0f, 0.0f, -(zfar + znear) / (zfar - znear), -1.0f,
      0.0f, 0.0f, -(2.0f * zfar * znear) / (zfar - znear), 0.0f,
  };

  mat4_mul(proj, view, mvp_out);
}

/*
 * Steps along the eye ray up to `max_dist`, returning `1` with the first hit
 * solid voxel `(hit_*)` and the preceding air voxel `(prev_*)` for placement.
 */
static int raycast_voxel(VoxelWorld *w, float max_dist,
                         int *hit_x, int *hit_y, int *hit_z,
                         int *prev_x, int *prev_y, int *prev_z) {
  float ox = w->x;
  float oy = w->y + 1.5f;
  float oz = w->z;
  float yr = w->yaw * 0.0174532925f;
  float pr = w->pitch * 0.0174532925f;
  float dx = cosf(pr) * cosf(yr);
  float dy = sinf(pr);
  float dz = cosf(pr) * sinf(yr);

  int lx = (int)floorf(ox);
  int ly = (int)floorf(oy);
  int lz = (int)floorf(oz);
  float step = 0.05f;
  float dist;
  for (dist = 0.0f; dist <= max_dist; dist += step) {
    int bx = (int)floorf(ox + dx * dist);
    int by = (int)floorf(oy + dy * dist);
    int bz = (int)floorf(oz + dz * dist);
    if (bx != lx || by != ly || bz != lz) {
      if (is_solid_at(w, bx, by, bz)) {
        *hit_x = bx;
        *hit_y = by;
        *hit_z = bz;
        *prev_x = lx;
        *prev_y = ly;
        *prev_z = lz;
        return 1;
      }
      lx = bx;
      ly = by;
      lz = bz;
    }
  }
  return 0;
}

/* Axis-aligned bounding box check around player feet `(x, feet_y, z)`. */
static int box_collides(VoxelWorld *w, float x, float feet_y, float z) {
  float hw = 0.28f;
  float h = 1.65f;
  int x0 = (int)floorf(x - hw);
  int x1 = (int)floorf(x + hw);
  int y0 = (int)floorf(feet_y);
  int y1 = (int)floorf(feet_y + h);
  int z0 = (int)floorf(z - hw);
  int z1 = (int)floorf(z + hw);
  int bx, by, bz;
  for (by = y0; by <= y1; by++) {
    for (bz = z0; bz <= z1; bz++) {
      for (bx = x0; bx <= x1; bx++) {
        if (is_solid_at(w, bx, by, bz)) {
          return 1;
        }
      }
    }
  }
  return 0;
}

static void draw_chunk_cb(GenericEl *slot_el, void *ctx) {
  int *drawn = (int*)ctx;
  Hash *slot = as(Hash, slot_el);
  Gl *chunk_gl = as(Gl, $(slot)->get("gl"));
  if (chunk_gl->vertex_count > 0) {
    $(chunk_gl)->draw(GL_TRIANGLES, 0, chunk_gl->vertex_count);
    *drawn += chunk_gl->vertex_count;
  }
}

static void count_verts_cb(GenericEl *slot_el, void *ctx) {
  int *total = (int*)ctx;
  Hash *slot = as(Hash, slot_el);
  Gl *chunk_gl = as(Gl, $(slot)->get("gl"));
  *total += chunk_gl->vertex_count;
}

/**
 * Fluent `VoxelWorld` methods.
 */

static VoxelWorld* vw_move(float forward, float right, float up, int jump,
                           float dt) {
  VoxelWorld *w = slf;
  if (dt <= 0.0f || dt > 0.05f) {
    dt = 0.016f;
  }

  float yr = w->yaw * 0.0174532925f;
  float fwd_x = cosf(yr);
  float fwd_z = sinf(yr);
  float right_x = -fwd_z;
  float right_z = fwd_x;

  float move_x = fwd_x * forward + right_x * right;
  float move_z = fwd_z * forward + right_z * right;
  float mlen = sqrtf(move_x * move_x + move_z * move_z);
  float speed = w->fly ? 7.5f : 5.2f;

  if (mlen > 1e-5f) {
    float step_x = (move_x / mlen) * speed * dt;
    float step_z = (move_z / mlen) * speed * dt;

    /* Advance X, auto-stepping 1-block ledges when walking on terrain. */
    float nx = w->x + step_x;
    if (!box_collides(w, nx, w->y, w->z)) {
      w->x = nx;
    } else if (!w->fly) {
      float up_y = floorf(w->y) + 1.02f;
      if (up_y - w->y <= 1.05f && !box_collides(w, w->x, up_y, w->z) &&
          !box_collides(w, nx, up_y, w->z)) {
        w->x = nx;
        w->y = up_y;
        w->vy = 0.0f;
      }
    }

    /* Advance Z independently so the player slides smoothly along walls. */
    float nz = w->z + step_z;
    if (!box_collides(w, w->x, w->y, nz)) {
      w->z = nz;
    } else if (!w->fly) {
      float up_y = floorf(w->y) + 1.02f;
      if (up_y - w->y <= 1.05f && !box_collides(w, w->x, up_y, w->z) &&
          !box_collides(w, w->x, up_y, nz)) {
        w->z = nz;
        w->y = up_y;
        w->vy = 0.0f;
      }
    }
  }

  if (w->fly) {
    w->y += up * speed * dt;
  } else {
    int on_ground = box_collides(w, w->x, w->y - 0.05f, w->z);
    if (on_ground && jump) {
      w->vy = 7.4f;
    }
    w->vy -= 20.0f * dt;
    float ny = w->y + w->vy * dt;
    if (!box_collides(w, w->x, ny, w->z)) {
      w->y = ny;
    } else {
      if (w->vy < 0.0f) {
        w->y = floorf(w->y) + 0.01f;
      }
      w->vy = 0.0f;
    }
  }

  maybe_shift_player_chunk(w);
  return $(w);
}

static VoxelWorld* vw_set_angles(float yaw, float pitch) {
  VoxelWorld *w = slf;
  if (pitch > 85.0f) {
    pitch = 85.0f;
  } else if (pitch < -85.0f) {
    pitch = -85.0f;
  }
  w->yaw = yaw;
  w->pitch = pitch;
  return $(w);
}

static VoxelWorld* vw_look(double dx, double dy) {
  VoxelWorld *w = slf;
  return vw_set_angles(w->yaw + (float)(dx * 0.14),
                       w->pitch - (float)(dy * 0.14));
}

static VoxelWorld* vw_teleport(float x, float y, float z) {
  VoxelWorld *w = slf;
  w->x = x;
  w->y = y;
  w->z = z;
  w->vy = 0.0f;
  maybe_shift_player_chunk(w);
  return $(w);
}

static VoxelWorld* vw_toggle_fly(void) {
  VoxelWorld *w = slf;
  w->fly = !w->fly;
  w->vy = 0.0f;
  return $(w);
}

static VoxelWorld* vw_set_captured(int captured) {
  VoxelWorld *w = slf;
  w->captured = captured ? 1 : 0;
  return $(w);
}

static VoxelWorld* vw_select_block(int block_type) {
  VoxelWorld *w = slf;
  if (block_type >= 1 && block_type <= 6) {
    w->selected_block = block_type;
  }
  return $(w);
}

static VoxelWorld* vw_cycle_block(int delta) {
  VoxelWorld *w = slf;
  if (delta != 0) {
    int step = delta > 0 ? 1 : -1;
    w->selected_block = ((w->selected_block - 1 + step + 6) % 6) + 1;
  }
  return $(w);
}

static VoxelWorld* vw_raycast_break(void) {
  VoxelWorld *w = slf;
  int hx, hy, hz, ax, ay, az;
  if (raycast_voxel(w, 7.0f, &hx, &hy, &hz, &ax, &ay, &az) && hy > 0) {
    write_world_block(w, hx, hy, hz, CH_BLOCK_AIR);
  }
  return $(w);
}

static VoxelWorld* vw_raycast_place(void) {
  VoxelWorld *w = slf;
  int hx, hy, hz, ax, ay, az;
  if (raycast_voxel(w, 7.0f, &hx, &hy, &hz, &ax, &ay, &az) &&
      ay > 0 && ay < CH_VOXEL_CHUNK_Y) {
    int feet_y = (int)floorf(w->y);
    int head_y = (int)floorf(w->y + 1.5f);
    int px = (int)floorf(w->x);
    int pz = (int)floorf(w->z);
    if (!(ax == px && az == pz && (ay == feet_y || ay == head_y))) {
      write_world_block(w, ax, ay, az, w->selected_block);
    }
  }
  return $(w);
}

static VoxelWorld* vw_render(float width, float height) {
  VoxelWorld *w = slf;
  float aspect = height > 0.0f ? (width / height) : 1.0f;
  GLfloat mvp[16];
  int drawn = 0;

  build_mvp(w->x, w->y + 1.5f, w->z, w->yaw, w->pitch, aspect, mvp);

  /* Set GL state and shared uniforms once before drawing all 9 chunk meshes. */
  $(w->world_shader)
      ->depth_test(1)
      ->cull_face(1)
      ->uniform_mat4("u_mvp", mvp)
      ->uniform_3f("u_cam_pos", w->x, w->y + 1.5f, w->z)
      ->uniform_3f("u_sky", CH_VOXEL_SKY_R, CH_VOXEL_SKY_G, CH_VOXEL_SKY_B);

  $(w->chunks)->foreach_ctx(draw_chunk_cb, &drawn);

  /* Render crosshair on top with depth test/culling disabled. */
  $(w->hud_gl)
      ->depth_test(0)
      ->cull_face(0)
      ->draw(GL_LINES, 0, 4);

  return $(w);
}

static int vw_block_at(int wx, int wy, int wz) {
  return read_world_block(slf, wx, wy, wz);
}

static int vw_surface_height(int wx, int wz) {
  return compute_terrain_height(wx, wz);
}

static int vw_total_vertices(void) {
  int total = 0;
  $(slf->chunks)->foreach_ctx(count_verts_cb, &total);
  return total;
}

static Hash* vw_chunk_slot(int rx, int rz) {
  return slot_at(slf, rx, rz);
}

static StatusOr* vw_status(void) {
  VoxelWorld *w = slf;
  StatusOr *sh_st = $(w->world_shader)->status()->$$;
  if (!$(sh_st)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(sh_st)->error());
    release(sh_st);
    return err;
  }
  release(sh_st);

  StatusOr *hud_st = $(w->hud_gl)->status()->$$;
  if (!$(hud_st)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(hud_st)->error());
    release(hud_st);
    return err;
  }
  release(hud_st);

  return create(StatusOr,
                create(Hash, 4)
                    ->put("center_cx", create(Number, (double)w->center_cx))
                    ->put("center_cz", create(Number, (double)w->center_cz))
                    ->put("chunks", create(Number, (double)w->chunks->length))
                    ->put("vertices", create(Number, (double)vw_total_vertices())),
                NULL);
}

static char* to_chars(void) {
  VoxelWorld *w = slf;
  snprintf(w->repr, sizeof(w->repr),
           "<VoxelWorld pos=(%.2f,%.2f,%.2f) center=(%d,%d) shifts=%d>",
           w->x, w->y, w->z, w->center_cx, w->center_cz, w->chunk_shifts);
  return w->repr;
}

/**
 * Constructor.
 */

VoxelWorld* ch_new_VoxelWorld(ChGlLoader loader) {
  static const GLfloat crosshair[] = {
      -0.025f,  0.000f,
       0.025f,  0.000f,
       0.000f, -0.035f,
       0.000f,  0.035f,
  };

  VoxelWorld *w = malloc(sizeof(VoxelWorld));
  w->generic.magic = CH_MAGIC;
  w->generic.type_val = VOXEL_WORLD;
  w->generic.prev_self = NULL;
  w->generic.next_self = NULL;
  w->generic.refs = 0;

  /*
   * Retain internal `Gl`, `Hash`, `TypedF32Array`, and `Slice` sub-objects via
   * `->$$` and pop them off their `self` stacks so constructing a `VoxelWorld`
   * leaves callers' active chains undisturbed.
   */
  w->loader = loader;
  w->world_shader = create(Gl, loader)
      ->shader(WORLD_VERT_SRC, WORLD_FRAG_SRC)
      ->$$;
  w->world_shader->bind((GenericEl*)-1);

  w->hud_gl = create(Gl, loader)
      ->shader(HUD_VERT_SRC, HUD_FRAG_SRC)
      ->mesh(crosshair, 8, 2)
      ->attr(0, 2, 0)
      ->$$;
  w->hud_gl->bind((GenericEl*)-1);

  w->edits = create(Hash, 16)->$$;
  w->edits->bind((GenericEl*)-1);

  w->mesh_verts = create(TypedF32Array, 4096)->$$;
  w->mesh_verts->bind((GenericEl*)-1);

  int spawn_wx = 8;
  int spawn_wz = 8;
  w->x = (float)spawn_wx + 0.5f;
  w->y = (float)(compute_terrain_height(spawn_wx, spawn_wz) + 1) + 0.02f;
  w->z = (float)spawn_wz + 0.5f;
  w->vy = 0.0f;
  w->yaw = 35.0f;
  w->pitch = -18.0f;
  w->fly = 0;
  w->captured = 1;
  w->selected_block = CH_BLOCK_WOOD;
  w->center_cx = 0;
  w->center_cz = 0;
  w->chunk_shifts = 0;
  w->repr[0] = '\0';

  w->chunks = create(Slice, CH_VOXEL_GRID_SLOTS)->$$;
  int rz, rx;
  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      Hash *slot = create(Hash, 4)
          ->put("cx", create(Number, (double)(rx - 1)))
          ->put("cz", create(Number, (double)(rz - 1)))
          ->put("blocks",
                create(TypedCharArray, CH_VOXEL_CHUNK_VOLUME)
                    ->fill(CH_BLOCK_AIR))
          ->put("gl", create(Gl, loader)->share_program(w->world_shader));
      $(w->chunks)->add(slot);
    }
  }
  w->chunks->bind((GenericEl*)-1);

  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      generate_chunk_blocks(w, slot_at(w, rx, rz), rx - 1, rz - 1);
    }
  }
  for (rz = 0; rz < CH_VOXEL_GRID_SIZE; rz++) {
    for (rx = 0; rx < CH_VOXEL_GRID_SIZE; rx++) {
      rebuild_chunk_slot_mesh(w, rx, rz);
    }
  }

  w->bind = bind_voxel;
  w->generic.bind = (void* (*)(GenericEl*))bind_voxel;
  w->generic.save = save;
  w->generic.del = del;
  w->generic.to_chars = to_chars;

  w->move = vw_move;
  w->look = vw_look;
  w->set_angles = vw_set_angles;
  w->teleport = vw_teleport;
  w->toggle_fly = vw_toggle_fly;
  w->set_captured = vw_set_captured;
  w->select_block = vw_select_block;
  w->cycle_block = vw_cycle_block;
  w->raycast_break = vw_raycast_break;
  w->raycast_place = vw_raycast_place;
  w->render = vw_render;
  w->block_at = vw_block_at;
  w->surface_height = vw_surface_height;
  w->total_vertices = vw_total_vertices;
  w->chunk_slot = vw_chunk_slot;
  w->status = vw_status;

  self((GenericEl*)w);
  return w;
}
