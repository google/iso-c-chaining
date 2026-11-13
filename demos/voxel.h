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

#ifndef CHAIN_VOXEL_H_
#define CHAIN_VOXEL_H_

#include "core/hash.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/typed_array.h"
#include "core/types.h"
#include "demos/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CH_VOXEL_CHUNK_X 16
#define CH_VOXEL_CHUNK_Y 16
#define CH_VOXEL_CHUNK_Z 16
#define CH_VOXEL_CHUNK_VOLUME \
  (CH_VOXEL_CHUNK_X * CH_VOXEL_CHUNK_Y * CH_VOXEL_CHUNK_Z)
#define CH_VOXEL_GRID_SIZE 3
#define CH_VOXEL_GRID_SLOTS (CH_VOXEL_GRID_SIZE * CH_VOXEL_GRID_SIZE)

#define CH_VOXEL_SKY_R 0.53f
#define CH_VOXEL_SKY_G 0.77f
#define CH_VOXEL_SKY_B 0.96f

enum {
  CH_BLOCK_AIR = 0,
  CH_BLOCK_GRASS = 1,
  CH_BLOCK_DIRT = 2,
  CH_BLOCK_STONE = 3,
  CH_BLOCK_SAND = 4,
  CH_BLOCK_WOOD = 5,
  CH_BLOCK_LEAVES = 6,
  CH_BLOCK_WATER = 7
};

#ifndef CHAIN_NO_SHORT_NAMES
#define VOXEL_CHUNK_X CH_VOXEL_CHUNK_X
#define VOXEL_CHUNK_Y CH_VOXEL_CHUNK_Y
#define VOXEL_CHUNK_Z CH_VOXEL_CHUNK_Z
#define VOXEL_CHUNK_VOLUME CH_VOXEL_CHUNK_VOLUME
#define VOXEL_GRID_SIZE CH_VOXEL_GRID_SIZE
#define VOXEL_GRID_SLOTS CH_VOXEL_GRID_SLOTS
#define VOXEL_SKY_R CH_VOXEL_SKY_R
#define VOXEL_SKY_G CH_VOXEL_SKY_G
#define VOXEL_SKY_B CH_VOXEL_SKY_B
#define BLOCK_AIR CH_BLOCK_AIR
#define BLOCK_GRASS CH_BLOCK_GRASS
#define BLOCK_DIRT CH_BLOCK_DIRT
#define BLOCK_STONE CH_BLOCK_STONE
#define BLOCK_SAND CH_BLOCK_SAND
#define BLOCK_WOOD CH_BLOCK_WOOD
#define BLOCK_LEAVES CH_BLOCK_LEAVES
#define BLOCK_WATER CH_BLOCK_WATER
#endif

ch_use(VoxelWorld, VOXEL_WORLD, (ChGlLoader))

/*
 * Fluent 3D voxel world controller backed by `ChGenericEl`. Maintains a
 * sliding 3x3 grid of 16x16x16 procedural chunks sharing one compiled GLSL
 * shader program (`world_shader`), GPU mesh upload via `TypedF32Array`,
 * per-chunk persistent block edits, surface walking with 1-block auto-step,
 * and DDA raycast break/place.
 */
struct VOXEL_WORLD {
  ChGenericEl generic;
  ChGlLoader loader;
  ChGl *world_shader;
  ChGl *hud_gl;
  ChSlice *chunks;
  ChHash *edits;
  ChTypedF32Array *mesh_verts;

  float x;
  float y;
  float z;
  float vy;
  float yaw;
  float pitch;
  int fly;
  int captured;
  int selected_block;
  int center_cx;
  int center_cz;
  int chunk_shifts;
  char repr[160];

  /* Binds this `VoxelWorld` as active on the `VoxelWorld` `self` stack. */
  ChVoxelWorld* (*bind)(ChGenericEl*);

  /* Advances player physics/walking, AABB collision, and 3x3 chunk shifting. */
  ChVoxelWorld* (*move)(float forward, float right, float up, int jump,
                        float dt);

  /* Applies mouse look deltas `(dx, dy)` to `yaw` and clamped `pitch`. */
  ChVoxelWorld* (*look)(double dx, double dy);

  /* Sets camera `yaw` and `pitch` directly (clamping pitch to [-85, 85]). */
  ChVoxelWorld* (*set_angles)(float yaw, float pitch);

  /* Teleports the player to `(x, y, z)`, recentering the 3x3 chunk grid. */
  ChVoxelWorld* (*teleport)(float x, float y, float z);

  /* Toggles walking vs. free-fly mode and resets vertical velocity. */
  ChVoxelWorld* (*toggle_fly)(void);

  /* Updates whether pointer capture is active (`1` = captured, `0` = free). */
  ChVoxelWorld* (*set_captured)(int captured);

  /* Sets the active placeable block type (`1..6`). */
  ChVoxelWorld* (*select_block)(int block_type);

  /* Cycles the selected placeable block type (`delta = +1 / -1`). */
  ChVoxelWorld* (*cycle_block)(int delta);

  /* Breaks the solid voxel currently under the center crosshair ray. */
  ChVoxelWorld* (*raycast_break)(void);

  /* Places `selected_block` against the targeted voxel face if clear of body. */
  ChVoxelWorld* (*raycast_place)(void);

  /* Renders all 9 active chunk meshes and the crosshair HUD. */
  ChVoxelWorld* (*render)(float width, float height);

  /* Returns the block ID at world integer coordinates `(wx, wy, wz)`. */
  int (*block_at)(int wx, int wy, int wz);

  /* Returns the procedural terrain top block Y at `(wx, wz)`. */
  int (*surface_height)(int wx, int wz);

  /* Returns the sum of uploaded vertices across all 9 active chunk meshes. */
  int (*total_vertices)(void);

  /* Returns the chunk slot `Hash*` at grid coordinates `(rx, rz)` in [0..2]. */
  ChHash* (*chunk_slot)(int rx, int rz);

  /* Returns `StatusOr*` reflecting shader/HUD OpenGL health. */
  ChStatusOr* (*status)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_VOXEL_H_ */
