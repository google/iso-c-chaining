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

#include "demos/ecs.h"

#include <stddef.h>

#include "core/boolean.h"
#include "core/number.h"
#include "core/test.h"
#include "core/typed_array.h"
#include "core/types.h"

typedef struct {
  int ticks;
  float last_dt;
} SysCounterCtx;

static void move_system(Ecs *ecs, float dt, void *ctx_ptr) {
  SysCounterCtx *ctx = (SysCounterCtx*)ctx_ptr;
  ctx->ticks++;
  ctx->last_dt = dt;

  int *alive = ecs->alive->data;
  float *x = $(ecs)->f32_col("x");
  float *vx = $(ecs)->f32_col("vx");
  int i;
  for (i = 0; i < ecs->count; i++) {
    if (alive[i]) {
      x[i] += vx[i] * dt;
    }
  }
}

int main(void) {
  Test *t = create(Test, "Structure-of-Arrays Ecs Unit Tests")->$$;
  SysCounterCtx ctx = {0, 0.0f};

  /* Small initial capacity (4) forces dynamic column growth past capacity. */
  Ecs *ecs = create(Ecs, 4)
      ->f32("x")
      ->f32("vx")
      ->i32("kind")
      ->system(move_system, &ctx)
      ->$$;

  int e0 = $(ecs)->spawn();
  int e1 = $(ecs)->spawn();
  int e2 = $(ecs)->spawn();
  int e3 = $(ecs)->spawn();
  int e4 = $(ecs)->spawn(); /* Triggers capacity growth from 4 to 8. */

  float *x = $(ecs)->f32_col("x");
  float *vx = $(ecs)->f32_col("vx");
  int *kind = $(ecs)->i32_col("kind");

  x[e0] = 1.0f;  vx[e0] = 10.0f; kind[e0] = 7;
  x[e4] = -2.0f; vx[e4] = 4.0f;  kind[e4] = 9;

  $(ecs)->tick(0.5f);

  $(t)->section("SoA column allocation, growth & system execution")
      ->expect_num($(ecs)->stats()->get("alive"), 5)
      ->expect_num($(ecs)->stats()->get("capacity"), 8)
      ->expect_num($(ecs)->stats()->get("systems"), 1)
      ->expect_num(create(Number, (double)ctx.ticks), 1)
      ->expect_num(create(Number, (double)x[e0]), 6.0)
      ->expect_num(create(Number, (double)x[e4]), 0.0);

  /* Killing e1 & e2 returns their slots to the free list; spawn reuses and zeroes them. */
  x[e2] = 999.0f;
  kind[e2] = 42;
  $(ecs)->kill(e1)->kill(e2);
  int recycled = $(ecs)->spawn();

  /* Re-fetch column pointers after potential future growths. */
  x = $(ecs)->f32_col("x");
  kind = $(ecs)->i32_col("kind");

  $(t)->section("O(1) slot recycling & zero-initialization")
      ->expect_bool(create(Boolean, recycled == e2), 1)
      ->expect_bool(create(Boolean, $(ecs)->is_alive(e1)), 0)
      ->expect_bool(create(Boolean, $(ecs)->is_alive(e3)), 1)
      ->expect_bool(create(Boolean, $(ecs)->is_alive(recycled)), 1)
      ->expect_num(create(Number, (double)ecs->alive_count), 4)
      ->expect_num(create(Number, (double)x[recycled]), 0.0)
      ->expect_num(create(Number, (double)kind[recycled]), 0)
      ->expect_num(
          create(Number, (double)$(ecs)->f32_arr("x")->length),
          (double)ecs->count);

  $(ecs)->clear();
  $(t)->expect_num(create(Number, (double)ecs->alive_count), 0)
      ->expect_num(create(Number, (double)ecs->count), 0);

  release(ecs);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
