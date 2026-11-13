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

#ifndef CHAIN_ECS_H_
#define CHAIN_ECS_H_

#include "core/hash.h"
#include "core/typed_array.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Ecs, ECS, (int))

/*
 * System callback invoked in registration order by `Ecs::tick(dt)`. Receives
 * the bound `ChEcs*`, frame step `dt`, and caller-supplied `ctx`.
 */
typedef void (*ChEcsSystem)(ChEcs *ecs, float dt, void *ctx);

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChEcsSystem EcsSystem;
#endif

typedef struct {
  ChEcsSystem fn;
  void *ctx;
} ChEcsSystemEntry;

/*
 * Fluent Structure-of-Arrays (SoA) Entity Component System backed by
 * `ChGenericEl`. Component columns are stored as contiguous `TypedF32Array`
 * (`float`) and `TypedIntArray` (`int`) buffers for cache-friendly inner loops
 * and zero-copy GPU instance buffer uploads, with O(1) free-list entity slot
 * recycling and cascading `release()` cleanup.
 */
struct ECS {
  ChGenericEl generic;
  int count;
  int alive_count;
  int capacity;
  ChHash *f32_cols;
  ChHash *i32_cols;
  ChTypedIntArray *alive;
  ChTypedIntArray *free_ids;
  ChEcsSystemEntry *systems;
  int system_count;
  int system_cap;
  char repr[128];

  /* Binds this `Ecs` as active on the `Ecs` `self` stack. */
  ChEcs* (*bind)(ChGenericEl*);

  /* Registers a zero-initialized `float` component column named `name`. */
  ChEcs* (*f32)(const char *name);

  /* Registers a zero-initialized `int` component column named `name`. */
  ChEcs* (*i32)(const char *name);

  /* Returns raw contiguous `float*` buffer for `name` (`[0..count)`). */
  float* (*f32_col)(const char *name);

  /* Returns raw contiguous `int*` buffer for `name` (`[0..count)`). */
  int* (*i32_col)(const char *name);

  /* Returns the bound `TypedF32Array*` column `name` for fluent array ops. */
  ChTypedF32Array* (*f32_arr)(const char *name);

  /* Returns the bound `TypedIntArray*` column `name` for fluent array ops. */
  ChTypedIntArray* (*i32_arr)(const char *name);

  /*
   * Allocates or recycles an entity ID, growing all registered columns if
   * needed and zeroing the new slot across all columns.
   */
  int (*spawn)(void);

  /* Marks entity `eid` dead and pushes its slot onto the free list. */
  ChEcs* (*kill)(int eid);

  /* Returns non-zero if `eid` is within range and currently alive. */
  int (*is_alive)(int eid);

  /* Appends a system callback to run in order on each `tick(dt)`. */
  ChEcs* (*system)(ChEcsSystem fn, void *ctx);

  /* Runs all registered systems in order with timestep `dt`. */
  ChEcs* (*tick)(float dt);

  /* Despawns all entities while preserving registered columns and systems. */
  ChEcs* (*clear)(void);

  /* Returns a fluent `Hash*` summary of entity counts, capacity, and columns. */
  ChHash* (*stats)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_ECS_H_ */
