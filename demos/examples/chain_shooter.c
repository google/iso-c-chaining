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

/**
 * 3D Space Wave Shooter built on `Ecs` (Structure-of-Arrays over `TypedF32Array`
 * and `TypedIntArray`), OpenGL 3.3 hardware instancing (`Gl::instance_buf`,
 * `Gl::draw_instanced`, `Gl::share_program`), and `Glfw`.
 *
 * Controls:
 *   Mouse Move              Steer & aim toward cursor (or combine with keyboard)
 *   W / A / S / D / Arrows  Override flight direction inside the arena
 *   Space / Left Click      Fire twin 360-degree plasma cannons
 *   R                       Restart from Wave 1
 *   Esc                     Quit
 *
 * Build & run with CMake (requires GLFW3 + OpenGL development headers):
 *   cmake -B build && cmake --build build
 *   ./build/chain_shooter
 *   ./build/chain_shooter --test
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/test.h"
#include "core/typed_array.h"
#include "core/types.h"
#include "demos/ecs.h"
#include "demos/gl.h"
#include "demos/glfw.h"

#define VIEW_HALF_Z 12.0f
#define STAR_COUNT 110
#define INST_STRIDE 8

enum {
  KIND_PLAYER = 1, KIND_FIGHTER, KIND_CRUISER,
  KIND_PBULLET, KIND_EBULLET, KIND_PARTICLE, KIND_STAR
};
enum { TEAM_NONE = 0, TEAM_PLAYER = 1, TEAM_ENEMY = 2 };
enum { MESH_SHIP = 0, MESH_CRUISER, MESH_BOLT, MESH_PARTICLE, MESH_COUNT };

static const char *INST_VERT_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in float a_shade;\n"
    "layout(location = 2) in vec3 a_inst_pos;\n"
    "layout(location = 3) in vec2 a_inst_rot_scale;\n"
    "layout(location = 4) in vec3 a_inst_color;\n"
    "uniform vec2 u_view;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "  float c = cos(a_inst_rot_scale.x), s = sin(a_inst_rot_scale.x);\n"
    "  vec3 p = a_pos * a_inst_rot_scale.y;\n"
    "  vec3 w = vec3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c) + a_inst_pos;\n"
    "  gl_Position = vec4(w.x / u_view.x, -w.z / u_view.y,\n"
    "                     clamp(-w.y * 0.05, -0.9, 0.9), 1.0);\n"
    "  v_color = a_inst_color * a_shade;\n"
    "}\n";

static const char *INST_FRAG_SRC =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(v_color, 1.0); }\n";

typedef struct {
  int *alive, *kind, *team, *mesh;
  float *x, *y, *z, *vx, *vy, *vz, *yaw, *spin, *scale, *hp, *ttl, *cd, *rad;
  float *r, *g, *b;
} ShooterCols;

typedef struct {
  Glfw *win;
  Ecs *ecs;
  Gl *shader_gl;
  Gl *meshes[MESH_COUNT];
  TypedF32Array *inst_bufs[MESH_COUNT];
  ShooterCols cols;
  int cols_cap;
  int player_eid, wave, score, kills, enemies_alive, game_over, has_mouse;
  double mouse_x, mouse_y;
  float wave_delay, view_half_x;
  unsigned int rng;
} Game;

static Game *g_game = NULL;

static float rand_range(Game *g, float lo, float hi) {
  g->rng = g->rng * 1664525u + 1013904223u;
  return lo + (hi - lo) * ((float)(g->rng & 0xFFFFu) / 65535.0f);
}

static void sync_cols(Game *g) {
  if (g->cols_cap == g->ecs->capacity) return;
  Ecs *e = g->ecs;
  g->cols.alive = e->alive->data;
  g->cols.kind  = $(e)->i32_col("kind");   g->cols.team = $(e)->i32_col("team");
  g->cols.mesh  = $(e)->i32_col("mesh");   g->cols.x    = $(e)->f32_col("x");
  g->cols.y     = $(e)->f32_col("y");      g->cols.z    = $(e)->f32_col("z");
  g->cols.vx    = $(e)->f32_col("vx");     g->cols.vy   = $(e)->f32_col("vy");
  g->cols.vz    = $(e)->f32_col("vz");     g->cols.yaw  = $(e)->f32_col("yaw");
  g->cols.spin  = $(e)->f32_col("spin");   g->cols.scale = $(e)->f32_col("scale");
  g->cols.hp    = $(e)->f32_col("hp");     g->cols.ttl  = $(e)->f32_col("ttl");
  g->cols.cd    = $(e)->f32_col("cooldown"); g->cols.rad = $(e)->f32_col("radius");
  g->cols.r     = $(e)->f32_col("r");      g->cols.g    = $(e)->f32_col("g");
  g->cols.b     = $(e)->f32_col("b");
  g->cols_cap = e->capacity;
}

static void push_tri(TypedF32Array *v, float ax, float ay, float az,
                     float bx, float by, float bz,
                     float cx, float cy, float cz, float shade) {
  const float p[12] = {ax, ay, az, shade, bx, by, bz, shade, cx, cy, cz, shade};
  $(v)->append_buf(p, 12);
}

static Gl* make_mesh(Gl *shader_src, TypedF32Array *verts) {
  return create(Gl, (GlLoader)glfwGetProcAddress)
      ->share_program(shader_src)->mesh_buf(verts, 4)
      ->attr(0, 3, 0)->attr(1, 1, 3)
      ->instance_data(NULL, 0, INST_STRIDE)
      ->instance_attr(2, 3, 0, 1)->instance_attr(3, 2, 3, 1)
      ->instance_attr(4, 3, 5, 1);
}

static Gl* make_ship_gl(Gl *shader_src) {
  TypedF32Array *v = create(TypedF32Array, 72);
  push_tri(v,  0, 0, -1.2f, -1.05f, -0.05f, 0.8f, 0, 0.38f, 0.25f, 0.96f);
  push_tri(v,  0, 0, -1.2f,  0, 0.38f, 0.25f, 1.05f, -0.05f, 0.8f, 1.00f);
  push_tri(v, -1.05f, -0.05f, 0.8f, 0, 0, 0.7f, 0, 0.38f, 0.25f, 0.78f);
  push_tri(v,  1.05f, -0.05f, 0.8f, 0, 0.38f, 0.25f, 0, 0, 0.7f, 0.82f);
  push_tri(v,  0, 0, -1.2f, 0, -0.18f, 0.5f, -1.05f, -0.05f, 0.8f, 0.58f);
  push_tri(v,  0, 0, -1.2f, 1.05f, -0.05f, 0.8f, 0, -0.18f, 0.5f, 0.62f);
  return make_mesh(shader_src, v);
}

static Gl* make_octa_gl(Gl *shader_src, float sx, float sy, float sz) {
  TypedF32Array *v = create(TypedF32Array, 96);
  push_tri(v, 0,  sy, 0, -sx, 0, 0, 0, 0,  sz, 1.00f);
  push_tri(v, 0,  sy, 0, 0, 0,  sz, sx, 0, 0, 0.92f);
  push_tri(v, 0,  sy, 0, sx, 0, 0, 0, 0, -sz, 0.84f);
  push_tri(v, 0,  sy, 0, 0, 0, -sz, -sx, 0, 0, 0.88f);
  push_tri(v, 0, -sy, 0, 0, 0,  sz, -sx, 0, 0, 0.62f);
  push_tri(v, 0, -sy, 0, sx, 0, 0, 0, 0,  sz, 0.58f);
  push_tri(v, 0, -sy, 0, 0, 0, -sz, sx, 0, 0, 0.54f);
  push_tri(v, 0, -sy, 0, -sx, 0, 0, 0, 0, -sz, 0.60f);
  return make_mesh(shader_src, v);
}

static int spawn_entity(Game *g, int kind, int team, int mesh,
                        float x, float y, float z, float vx, float vy, float vz,
                        float yaw, float spin, float scale, float hp, float ttl,
                        float radius, float r, float gr, float b) {
  int id = $(g->ecs)->spawn();
  sync_cols(g);
  ShooterCols *c = &g->cols;
  c->kind[id] = kind; c->team[id] = team; c->mesh[id] = mesh;
  c->x[id] = x;       c->y[id] = y;       c->z[id] = z;
  c->vx[id] = vx;     c->vy[id] = vy;     c->vz[id] = vz;
  c->yaw[id] = yaw;   c->spin[id] = spin; c->scale[id] = scale;
  c->hp[id] = hp;     c->ttl[id] = ttl;   c->rad[id] = radius;
  c->r[id] = r;       c->g[id] = gr;      c->b[id] = b;
  return id;
}

static void spawn_burst(Game *g, float x, float z, int count,
                        float r, float gr, float b) {
  int k;
  for (k = 0; k < count; k++) {
    float a = rand_range(g, 0.0f, 6.283185f), s = rand_range(g, 4.0f, 12.0f);
    spawn_entity(g, KIND_PARTICLE, TEAM_NONE, MESH_PARTICLE,
                 x, rand_range(g, -0.3f, 0.3f), z,
                 cosf(a) * s, rand_range(g, -2.5f, 3.5f), sinf(a) * s,
                 0, rand_range(g, -10.0f, 10.0f), rand_range(g, 0.25f, 0.55f),
                 0, rand_range(g, 0.28f, 0.65f), 0, r, gr, b);
  }
}

static void spawn_wave(Game *g, int wave) {
  int count = 10 + wave * 4, k;
  for (k = 0; k < count; k++) {
    int cr = (wave >= 2 && (k % 4) == 0);
    float ang = (6.2831853f * (float)k) / (float)count +
                rand_range(g, -0.14f, 0.14f);
    float rad = rand_range(g, g->view_half_x + 2.0f, g->view_half_x + 6.0f);
    float ex = cosf(ang) * rad, ez = sinf(ang) * rad;
    float spd = (cr ? 3.2f : 4.6f) + 0.35f * (float)wave;
    int id = spawn_entity(
        g, cr ? KIND_CRUISER : KIND_FIGHTER, TEAM_ENEMY,
        cr ? MESH_CRUISER : MESH_SHIP, ex, 0, ez,
        (-ex / rad) * spd, 0, (-ez / rad) * spd, atan2f(ex, ez),
        cr ? 1.8f : 0.0f, cr ? 0.84f : 0.55f, cr ? 3.0f : 1.0f, 0,
        cr ? 0.77f : 0.54f, cr ? 0.95f : 1.00f, cr ? 0.35f : 0.34f,
        cr ? 0.95f : 0.24f);
    g->cols.cd[id] = rand_range(g, 1.0f, 3.0f) / (1.0f + 0.12f * (float)wave);
    g->enemies_alive++;
  }
}

/**
 * ECS systems executed in order each frame by `$(g->ecs)->tick(dt)`.
 */

/*
 * System 1: reads player input (`WASD`/Arrows or cursor follow + twin-cannon
 * firing), steers radial enemies inward toward the player with flanking
 * offsets, fires aimed enemy bolts, and advances to the next wave once cleared.
 */
static void sys_player_and_waves(Ecs *ecs, float dt, void *ctx) {
  Game *g = (Game*)ctx;
  sync_cols(g);
  ShooterCols *c = &g->cols;
  int p = g->player_eid;

  if (!g->game_over && p >= 0 && p < ecs->count && c->alive[p]) {
    Glfw *w = g->win;
    float mx = (float)($(w)->key_down(GLFW_KEY_D) + $(w)->key_down(GLFW_KEY_RIGHT) -
                       $(w)->key_down(GLFW_KEY_A) - $(w)->key_down(GLFW_KEY_LEFT));
    float mz = (float)($(w)->key_down(GLFW_KEY_S) + $(w)->key_down(GLFW_KEY_DOWN) -
                       $(w)->key_down(GLFW_KEY_W) - $(w)->key_down(GLFW_KEY_UP));
    float len = sqrtf(mx * mx + mz * mz);

    /* Map cursor window coordinates linearly onto orthographic arena `(X, Z)`. */
    float aim_x = c->x[p], aim_z = c->z[p] - 1.0f;
    if (g->has_mouse && w->width > 0 && w->height > 0) {
      aim_x = ((2.0f * (float)g->mouse_x / (float)w->width) - 1.0f) *
              g->view_half_x;
      aim_z = ((2.0f * (float)g->mouse_y / (float)w->height) - 1.0f) *
              VIEW_HALF_Z;
    }

    if (len > 1e-4f) {
      /* Keyboard overrides movement directly while letting mouse still aim. */
      c->vx[p] = (mx / len) * 13.5f;
      c->vz[p] = (mz / len) * 13.5f;
      if (!g->has_mouse) c->yaw[p] = atan2f(-mx, -mz);
    } else if (g->has_mouse) {
      /* Ease smoothly toward the cursor outside a small `0.35` deadzone. */
      float dx = aim_x - c->x[p], dz = aim_z - c->z[p];
      float d = sqrtf(dx * dx + dz * dz);
      float spd = d > 0.35f ? (d > 3.5f ? 13.5f : 13.5f * (d / 3.5f)) : 0.0f;
      c->vx[p] = spd > 0.0f ? (dx / d) * spd : 0.0f;
      c->vz[p] = spd > 0.0f ? (dz / d) * spd : 0.0f;
    } else {
      c->vx[p] = c->vz[p] = 0.0f;
    }
    if (g->has_mouse) {
      float adx = aim_x - c->x[p], adz = aim_z - c->z[p];
      if (adx * adx + adz * adz > 0.02f) c->yaw[p] = atan2f(-adx, -adz);
    }

    if (c->cd[p] > 0.0f) c->cd[p] -= dt;
    if (($(w)->key_down(GLFW_KEY_SPACE) ||
         $(w)->mouse_down(GLFW_MOUSE_BUTTON_LEFT)) && c->cd[p] <= 0.0f) {
      /* Forward `(-sin, -cos)` and right `(cos, -sin)` fire twin wing bolts. */
      float yaw = c->yaw[p], fx = -sinf(yaw), fz = -cosf(yaw);
      float rx = cosf(yaw) * 0.37f, rz = -sinf(yaw) * 0.37f;
      float px = c->x[p] + fx * 0.4f, pz = c->z[p] + fz * 0.4f;
      spawn_entity(g, KIND_PBULLET, TEAM_PLAYER, MESH_BOLT, px - rx, 0, pz - rz,
                   fx * 30.0f, 0, fz * 30.0f, yaw, 0, 1, 1, 2.2f, 0.45f, 0.35f, 1, 1);
      spawn_entity(g, KIND_PBULLET, TEAM_PLAYER, MESH_BOLT, px + rx, 0, pz + rz,
                   fx * 30.0f, 0, fz * 30.0f, yaw, 0, 1, 1, 2.2f, 0.45f, 0.35f, 1, 1);
      c->cd[p] = 0.14f;
    }
  }

  /* Snapshot `ecs->count` so newly fired bolts aren't visited in this pass. */
  int enemies = 0, n = ecs->count, i;
  float tx = (p >= 0 && p < n && c->alive[p]) ? c->x[p] : 0.0f;
  float tz = (p >= 0 && p < n && c->alive[p]) ? c->z[p] : 0.0f;
  for (i = 0; i < n; i++) {
    if (!c->alive[i] || (c->kind[i] != KIND_FIGHTER && c->kind[i] != KIND_CRUISER))
      continue;
    enemies++;
    float dx = tx - c->x[i], dz = tz - c->z[i], dist = sqrtf(dx * dx + dz * dz);
    if (dist > 0.2f) {
      /* Fighters add alternating +/-0.22 perpendicular flank so swarms arc in. */
      float ux = dx / dist, uz = dz / dist;
      float flank = (c->kind[i] == KIND_FIGHTER) ? ((i & 1) ? -0.22f : 0.22f) : 0.0f;
      float sx = ux - uz * flank, sz = uz + ux * flank, sl = sqrtf(sx * sx + sz * sz);
      float spd = (c->kind[i] == KIND_CRUISER ? 3.2f : 4.6f) + 0.35f * (float)g->wave;
      c->vx[i] = (sx / sl) * spd;
      c->vz[i] = (sz / sl) * spd;
      if (c->kind[i] == KIND_FIGHTER) c->yaw[i] = atan2f(-sx, -sz);
    }
    if ((c->cd[i] -= dt) <= 0.0f && fabsf(c->x[i]) <= g->view_half_x &&
        fabsf(c->z[i]) <= VIEW_HALF_Z && dist > 2.0f) {
      float bx = dx / dist, bz = dz / dist;
      spawn_entity(g, KIND_EBULLET, TEAM_ENEMY, MESH_BOLT,
                   c->x[i] + bx * 0.8f, 0, c->z[i] + bz * 0.8f,
                   bx * 11.5f, 0, bz * 11.5f, atan2f(-bx, -bz),
                   0, 1, 1, 2.2f, 0.45f, 1, 0.72f, 0.20f);
      c->cd[i] = rand_range(g, 1.5f, 3.0f);
    }
  }
  g->enemies_alive = enemies;
  if (!g->game_over && enemies == 0 && (g->wave_delay -= dt) <= 0.0f) {
    spawn_wave(g, ++g->wave);
    g->wave_delay = 0.8f;
  }
}

/*
 * System 2: integrates position/spin for every alive entity, clamps player to
 * visible screen bounds, wraps background stars, and culls off-screen bolts.
 */
static void sys_physics(Ecs *ecs, float dt, void *ctx) {
  Game *g = (Game*)ctx;
  sync_cols(g);
  ShooterCols *c = &g->cols;
  int i;
  for (i = 0; i < ecs->count; i++) {
    if (!c->alive[i]) continue;
    c->x[i] += c->vx[i] * dt; c->y[i] += c->vy[i] * dt;
    c->z[i] += c->vz[i] * dt; c->yaw[i] += c->spin[i] * dt;
    if (c->kind[i] == KIND_PLAYER) {
      float bx = g->view_half_x - 0.8f, bz = VIEW_HALF_Z - 0.8f;
      if (c->x[i] < -bx) c->x[i] = -bx; else if (c->x[i] > bx) c->x[i] = bx;
      if (c->z[i] < -bz) c->z[i] = -bz; else if (c->z[i] > bz) c->z[i] = bz;
    } else if (c->kind[i] == KIND_STAR && c->z[i] > VIEW_HALF_Z + 2.0f) {
      c->z[i] = -VIEW_HALF_Z - 2.0f;
    } else if ((c->kind[i] == KIND_PBULLET || c->kind[i] == KIND_EBULLET) &&
               (fabsf(c->x[i]) > g->view_half_x + 8.0f ||
                fabsf(c->z[i]) > VIEW_HALF_Z + 8.0f)) {
      /* Margin `+ 8.0f` lets player shots hit enemies spawning outside view. */
      $(ecs)->kill(i);
    }
  }
}

/*
 * System 3: resolves radial circle hits between player bolts and enemies, plus
 * enemy ships/bolts against the player, spawning explosion particle bursts.
 */
static void sys_collisions(Ecs *ecs, float dt, void *ctx) {
  (void)dt;
  Game *g = (Game*)ctx;
  sync_cols(g);
  ShooterCols *c = &g->cols;
  int n = ecs->count, i, j;

  for (i = 0; i < n; i++) {
    if (!c->alive[i] || c->kind[i] != KIND_PBULLET) continue;
    for (j = 0; j < n; j++) {
      if (!c->alive[j] || c->team[j] != TEAM_ENEMY) continue;
      float dx = c->x[i] - c->x[j], dz = c->z[i] - c->z[j];
      float hr = c->rad[i] + c->rad[j];
      if (dx * dx + dz * dz <= hr * hr) {
        $(ecs)->kill(i);
        if ((c->hp[j] -= 1.0f) <= 0.0f) {
          /* Save enemy coords/tint before `kill(j)` + `spawn_burst()` reuse slot `j`. */
          float ex = c->x[j], ez = c->z[j], cr = c->r[j], cg = c->g[j], cb = c->b[j];
          int pts = (c->kind[j] == KIND_CRUISER) ? 250 : 100;
          $(ecs)->kill(j);
          spawn_burst(g, ex, ez, 10, cr, cg, cb);
          g->score += pts; g->kills++;
        } else {
          spawn_burst(g, c->x[i], c->z[i], 3, 1.0f, 0.9f, 0.5f);
        }
        break;
      }
    }
  }

  int p = g->player_eid;
  if (p >= 0 && p < ecs->count && c->alive[p] && !g->game_over) {
    for (i = 0; i < ecs->count; i++) {
      if (!c->alive[i] || c->team[i] != TEAM_ENEMY) continue;
      float dx = c->x[i] - c->x[p], dz = c->z[i] - c->z[p];
      float hr = c->rad[i] + c->rad[p];
      if (dx * dx + dz * dz <= hr * hr) {
        $(ecs)->kill(i);
        spawn_burst(g, c->x[p], c->z[p], 8, 0.4f, 0.95f, 1.0f);
        if ((c->hp[p] -= 1.0f) <= 0.0f) {
          $(ecs)->kill(p);
          spawn_burst(g, c->x[p], c->z[p], 22, 1.0f, 0.55f, 0.25f);
          g->game_over = 1;
        }
        break;
      }
    }
  }
}

/*
 * System 4: decays `ttl` on temporary entities (bolts, particles), shrinks
 * explosion shards smoothly, and frees expired slots back into `ecs->free_ids`.
 */
static void sys_lifetime(Ecs *ecs, float dt, void *ctx) {
  Game *g = (Game*)ctx;
  sync_cols(g);
  ShooterCols *c = &g->cols;
  int i;
  for (i = 0; i < ecs->count; i++) {
    if (!c->alive[i] || c->ttl[i] <= 0.0f) continue;
    if (c->kind[i] == KIND_PARTICLE) {
      c->scale[i] *= (1.0f - 1.2f * dt);
      if (c->scale[i] < 0.05f) c->scale[i] = 0.05f;
    }
    if ((c->ttl[i] -= dt) <= 0.0f) $(ecs)->kill(i);
  }
}

static void restart_game(Game *g) {
  int i;
  $(g->ecs)->clear();
  g->cols_cap = 0;
  g->wave = 1; g->score = 0; g->kills = 0;
  g->enemies_alive = 0; g->game_over = 0; g->wave_delay = 0.8f;
  for (i = 0; i < STAR_COUNT; i++) {
    float b = rand_range(g, 0.35f, 0.95f);
    spawn_entity(g, KIND_STAR, TEAM_NONE, MESH_PARTICLE,
                 rand_range(g, -g->view_half_x - 2, g->view_half_x + 2),
                 rand_range(g, -10.0f, -2.0f),
                 rand_range(g, -VIEW_HALF_Z - 2, VIEW_HALF_Z + 2),
                 0, 0, rand_range(g, 4.0f, 11.0f), 0, 1.5f,
                 rand_range(g, 0.14f, 0.30f), 0, 0, 0, b * 0.85f, b * 0.90f, b);
  }
  g->player_eid = spawn_entity(g, KIND_PLAYER, TEAM_PLAYER, MESH_SHIP,
                               0, 0, 0, 0, 0, 0, 0, 0, 0.67f, 5.0f, 0, 0.57f,
                               0.25f, 0.92f, 1.00f);
  spawn_wave(g, g->wave);
}

static void on_cursor(Glfw *win, Hash *ev) {
  (void)win;
  if (g_game != NULL) {
    g_game->mouse_x = as(Number, $(ev)->get("x"))->value();
    g_game->mouse_y = as(Number, $(ev)->get("y"))->value();
    g_game->has_mouse = 1;
  }
}

static void on_key(Glfw *win, Hash *ev) {
  if (!as(Boolean, $(ev)->get("pressed"))->value()) return;
  int key = (int)as(Number, $(ev)->get("key"))->value();
  if (key == GLFW_KEY_ESCAPE) $(win)->close();
  else if (key == GLFW_KEY_R && g_game != NULL) restart_game(g_game);
}

static void on_render(Glfw *win, Hash *frame) {
  Game *g = g_game;
  float dt = (float)as(Number, $(frame)->get("dt"))->value();
  float w = (float)as(Number, $(frame)->get("width"))->value();
  float h = (float)as(Number, $(frame)->get("height"))->value();
  if (dt <= 0.0f || dt > 0.05f) dt = 0.016f;
  g->view_half_x = VIEW_HALF_Z * (h > 0.0f ? (w / h) : (1024.0f / 720.0f));

  $(g->ecs)->tick(dt);

  /* Pack all active instances per mesh in a single SoA pass. */
  int m, i;
  for (m = 0; m < MESH_COUNT; m++) {
    $(g->inst_bufs[m])->from_buf(NULL, 0)->bind((GenericEl*)-1);
  }
  sync_cols(g);
  ShooterCols *c = &g->cols;
  for (i = 0; i < g->ecs->count; i++) {
    if (c->alive[i]) {
      const float row[INST_STRIDE] = {
          c->x[i], c->y[i], c->z[i], c->yaw[i],
          c->scale[i], c->r[i], c->g[i], c->b[i],
      };
      $(g->inst_bufs[c->mesh[i]])->append_buf(row, INST_STRIDE)
                                 ->bind((GenericEl*)-1);
    }
  }

  $(g->shader_gl)->depth_test(1)->cull_face(1)
                 ->uniform_2f("u_view", g->view_half_x, VIEW_HALF_Z);
  for (m = 0; m < MESH_COUNT; m++) {
    int cnt = g->inst_bufs[m]->length / INST_STRIDE;
    if (cnt > 0) {
      $(g->meshes[m])->instance_buf(g->inst_bufs[m], INST_STRIDE)
                     ->draw_instanced(GL_TRIANGLES, 0,
                                      g->meshes[m]->vertex_count, cnt);
    }
  }
  (void)win;
}

static int init_game(Game *g, int w, int h, char *title, int visible) {
  memset(g, 0, sizeof(*g));
  g->rng = 0x13572468u;
  g->view_half_x = VIEW_HALF_Z * ((float)w / (float)h);
  g_game = g;

  g->win = create(Glfw, w, h, title)
      ->gl_version(3, 3, GLFW_OPENGL_CORE_PROFILE)
#ifdef __APPLE__
      ->hint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE)
#endif
      ->resizable(1)->visible(visible)->vsync(visible ? 1 : 0)
      ->clear_color(0.03f, 0.04f, 0.09f, 1.0f)
      ->open()->$$;
  if (g->win->handle == NULL) return 0;

  /* Compile one shared instanced shader and 4 low-poly GPU mesh VAOs/VBOs. */
  g->shader_gl = create(Gl, (GlLoader)glfwGetProcAddress)
      ->shader(INST_VERT_SRC, INST_FRAG_SRC)->$$;
  g->meshes[MESH_SHIP]     = make_ship_gl(g->shader_gl);
  g->meshes[MESH_CRUISER]  = make_octa_gl(g->shader_gl, 1.1f, 0.55f, 1.1f);
  g->meshes[MESH_BOLT]     = make_octa_gl(g->shader_gl, 0.14f, 0.14f, 0.65f);
  g->meshes[MESH_PARTICLE] = make_octa_gl(g->shader_gl, 0.35f, 0.35f, 0.35f);

  Slice *gl_objs = create(Slice, 6)->add(g->shader_gl);
  release(g->shader_gl);
  Slice *buf_objs = create(Slice, MESH_COUNT);
  int m;
  for (m = 0; m < MESH_COUNT; m++) {
    g->inst_bufs[m] = create(TypedF32Array, 256);
    $(gl_objs)->add(g->meshes[m]);
    $(buf_objs)->add(g->inst_bufs[m]);
  }

  /*
   * Key win of `iso-c-chaining` for ECS design:
   *  - High-level ergonomics without preprocessor DSLs: plain C function pointers
   *    declare all 19 component columns and the ordered 4-system loop in one
   *    readable chain instead of macro codegen or multi-page setup boilerplate.
   *  - Unboxed SoA performance with automatic cleanup: each `->f32()`/`->i32()`
   *    column stays a flat contiguous `TypedF32Array`/`TypedIntArray` buffer for
   *    cache-friendly hot loops, O(1) slot recycling, and direct GPU uploads,
   *    yet participates in `ChGenericEl` refcounting so `release(win)` frees
   *    every column, buffer, and GL resource cleanly in one shot.
   */
  g->ecs = create(Ecs, 128)
      ->f32("x")->f32("y")->f32("z")->f32("vx")->f32("vy")->f32("vz")
      ->f32("yaw")->f32("spin")->f32("scale")->f32("hp")->f32("ttl")
      ->f32("cooldown")->f32("r")->f32("g")->f32("b")->f32("radius")
      ->i32("kind")->i32("team")->i32("mesh")
      ->system(sys_player_and_waves, g)->system(sys_physics, g)
      ->system(sys_collisions, g)->system(sys_lifetime, g);

  Hash *res = create(Hash, 3)
      ->put("gl", gl_objs)->put("bufs", buf_objs)->put("ecs", g->ecs);
  restart_game(g);
  $(g->win)->user_data(res)->on_key(on_key)
           ->on_cursor(on_cursor)->on_render(on_render);
  return 1;
}

static int run_tests(void) {
  Test *t = create(Test, "chain_shooter ECS + Instanced 3D Wave Shooter Tests")->$$;
  Game g;
  int opened = init_game(&g, 320, 240, "chain_shooter --test", 0);
  StatusOr *win_st = $(g.win)->status()->$$;
  $(t)->section("Glfw, instanced meshes, center spawn & 360-deg Wave 1")
      ->expect_bool(create(Boolean, opened), 1)->expect_ok(win_st);

  if ($(win_st)->ok()) {
    int quad_mask = 0, i, saw_ship = 0, py;
    $(t)->expect_ok($(g.shader_gl)->status());
    for (i = 0; i < MESH_COUNT; i++) $(t)->expect_ok($(g.meshes[i])->status());
    for (i = 0; i < g.ecs->count; i++) {
      if (g.cols.alive[i] &&
          (g.cols.kind[i] == KIND_FIGHTER || g.cols.kind[i] == KIND_CRUISER)) {
        quad_mask |= 1 << ((g.cols.x[i] >= 0 ? 1 : 0) | (g.cols.z[i] >= 0 ? 2 : 0));
      }
    }
    $(t)->expect_num(create(Number, (double)g.enemies_alive), 14)
        ->expect_num(create(Number, (double)g.cols.x[g.player_eid]), 0.0)
        ->expect_num(create(Number, (double)g.cols.z[g.player_eid]), 0.0)
        ->expect_num(create(Number, (double)quad_mask), 15)
        ->expect_bool(create(Boolean, g.ecs->alive_count == 1 + STAR_COUNT + 14), 1);

    $(g.win)->max_frames(2)->run();
    TypedIntArray *bg = $(g.win)->read_pixel(4, 235)->$$;
    for (py = 100; py <= 140 && !saw_ship; py += 2) {
      TypedIntArray *px = $(g.win)->read_pixel(160, py)->$$;
      if ($(px)->at(2) > 160 && $(px)->at(1) > 130) saw_ship = 1;
      release(px);
    }
    $(t)->expect_bool(create(Boolean, $(bg)->at(0) < 30 && $(bg)->at(2) < 45), 1)
        ->expect_bool(create(Boolean, saw_ship), 1);
    release(bg);

    $(t)->section("Mouse steering, bullet collision, bursts & slot recycling");
    $(g.win)->dispatch_event("cursor", create(Hash, 2)
        ->put("x", create(Number, 280.0))->put("y", create(Number, 40.0)));
    for (i = 0; i < 4; i++) $(g.ecs)->tick(0.016f);
    $(t)->expect_bool(create(Boolean, g.cols.x[g.player_eid] > 0.1f &&
                                      g.cols.z[g.player_eid] < -0.1f &&
                                      g.cols.yaw[g.player_eid] != 0.0f), 1);

    int e0 = -1;
    for (i = 0; i < g.ecs->count && e0 < 0; i++)
      if (g.cols.alive[i] && g.cols.kind[i] == KIND_FIGHTER) e0 = i;
    spawn_entity(&g, KIND_PBULLET, TEAM_PLAYER, MESH_BOLT, g.cols.x[e0], 0,
                 g.cols.z[e0], 0, 0, 0, 0, 0, 1, 1, 2.2f, 0.45f, 0.35f, 1, 1);
    $(g.win)->step();
    $(t)->expect_num(create(Number, (double)g.kills), 1)
        ->expect_num(create(Number, (double)g.score), 100);

    int saw_freed = 0;
    for (i = 0; i < 60; i++) {
      $(g.ecs)->tick(0.016f);
      if (g.ecs->free_ids->length > 0) saw_freed = 1;
    }
    int probe = $(g.ecs)->spawn(), before = g.ecs->count;
    sync_cols(&g);
    $(g.ecs)->kill(probe);
    int reused = $(g.ecs)->spawn();
    sync_cols(&g);
    $(t)->expect_bool(create(Boolean, saw_freed && reused == probe &&
                                      g.ecs->count == before), 1);
    $(g.ecs)->kill(reused);

    $(t)->section("Wave 2 cruisers, Ecs capacity growth & close event");
    for (i = 0; i < g.ecs->count; i++)
      if (g.cols.alive[i] &&
          (g.cols.kind[i] == KIND_FIGHTER || g.cols.kind[i] == KIND_CRUISER))
        $(g.ecs)->kill(i);
    for (i = 0; i < 65; i++) $(g.ecs)->tick(0.016f);
    for (i = 0; i < 40; i++)
      spawn_entity(&g, KIND_PBULLET, TEAM_PLAYER, MESH_BOLT,
                   -10.0f + (float)i * 0.5f, 0, 0, 0, 0, -20.0f,
                   0, 0, 1, 1, 2.2f, 0.45f, 0.35f, 1, 1);
    $(g.win)->step();
    $(t)->expect_num(create(Number, (double)g.wave), 2)
        ->expect_bool(create(Boolean, g.enemies_alive > 12 &&
                                      g.ecs->capacity >= 256), 1);

    $(g.win)->dispatch_event("key", create(Hash, 2)
        ->put("key", create(Number, GLFW_KEY_ESCAPE))
        ->put("pressed", create(Boolean, 1)));
    $(t)->expect_bool(create(Boolean, $(g.win)->should_close()), 1);
  }

  release(win_st);
  release(g.win);
  g_game = NULL;
  int rc = $(t)->summary();
  release(t);
  return rc;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--test") == 0) return run_tests();

  Game g;
  if (!init_game(&g, 1024, 720,
                 "ISO C Chaining + ECS Instanced Space Shooter", 1)) {
    StatusOr *st = $(g.win)->status()->$$;
    fprintf(stderr, "Failed to open GLFW OpenGL 3.3 window: %s\n",
            $(st)->error());
    release(st);
    release(g.win);
    g_game = NULL;
    return 1;
  }

  $(g.win)->run();
  release(g.win);
  g_game = NULL;
  return 0;
}
