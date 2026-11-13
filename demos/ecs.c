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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static Ecs* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Ecs*)update_self_stack(&top, s, ECS);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Ecs* bind_ecs(GenericEl *ecs) {
  return self(ecs);
}

static void del(void *obj) {
  Ecs *ecs = (Ecs*)obj;
  if (ecs != NULL && self((GenericEl*)ecs) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      release(slf->f32_cols);
      release(slf->i32_cols);
      release(slf->alive);
      release(slf->free_ids);
      free(slf->systems);
      Ecs *to_free = slf;
      self((GenericEl*)-1);
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * Column capacity growth and slot initialization helpers.
 */

static void grow_f32_array(TypedF32Array *arr, int new_cap) {
  if (new_cap <= arr->capacity) {
    return;
  }
  int old_cap = arr->capacity;
  arr->data = realloc(arr->data, sizeof(float) * (size_t)new_cap);
  memset(arr->data + old_cap, 0, sizeof(float) * (size_t)(new_cap - old_cap));
  arr->capacity = new_cap;
}

static void grow_i32_array(TypedIntArray *arr, int new_cap) {
  if (new_cap <= arr->capacity) {
    return;
  }
  int old_cap = arr->capacity;
  arr->data = realloc(arr->data, sizeof(int) * (size_t)new_cap);
  memset(arr->data + old_cap, 0, sizeof(int) * (size_t)(new_cap - old_cap));
  arr->capacity = new_cap;
}

static void ensure_ecs_capacity(Ecs *ecs, int min_cap) {
  if (min_cap <= ecs->capacity) {
    return;
  }
  int new_cap = ecs->capacity > 0 ? ecs->capacity : 64;
  while (new_cap < min_cap) {
    new_cap *= 2;
  }

  int i;
  for (i = 0; i < ecs->f32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->f32_cols->key_slice->elements[i])->chars;
    TypedF32Array *arr = (TypedF32Array*)$(ecs->f32_cols)->get(k);
    grow_f32_array(arr, new_cap);
    arr->bind((GenericEl*)-1);
  }
  for (i = 0; i < ecs->i32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->i32_cols->key_slice->elements[i])->chars;
    TypedIntArray *arr = (TypedIntArray*)$(ecs->i32_cols)->get(k);
    grow_i32_array(arr, new_cap);
    arr->bind((GenericEl*)-1);
  }
  grow_i32_array(ecs->alive, new_cap);
  ecs->capacity = new_cap;
}

static void sync_column_lengths(Ecs *ecs) {
  int i;
  for (i = 0; i < ecs->f32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->f32_cols->key_slice->elements[i])->chars;
    TypedF32Array *arr = (TypedF32Array*)$(ecs->f32_cols)->get(k);
    arr->length = ecs->count;
    arr->bind((GenericEl*)-1);
  }
  for (i = 0; i < ecs->i32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->i32_cols->key_slice->elements[i])->chars;
    TypedIntArray *arr = (TypedIntArray*)$(ecs->i32_cols)->get(k);
    arr->length = ecs->count;
    arr->bind((GenericEl*)-1);
  }
  ecs->alive->length = ecs->count;
}

/**
 * Fluent `Ecs` methods.
 */

static Ecs* ecs_add_f32(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL || $(ecs->f32_cols)->get((char*)name) != NULL) {
    return $(ecs);
  }
  TypedF32Array *col = create(TypedF32Array, ecs->capacity)->fill(0.0f);
  col->length = ecs->count;
  $(ecs->f32_cols)->put((char*)name, col);
  return $(ecs);
}

static Ecs* ecs_add_i32(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL || $(ecs->i32_cols)->get((char*)name) != NULL) {
    return $(ecs);
  }
  TypedIntArray *col = create(TypedIntArray, ecs->capacity)->fill(0);
  col->length = ecs->count;
  $(ecs->i32_cols)->put((char*)name, col);
  return $(ecs);
}

static float* ecs_f32_col(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL) {
    return NULL;
  }
  GenericEl *el = $(ecs->f32_cols)->get((char*)name);
  if (el == NULL) {
    return NULL;
  }
  float *ptr = ((TypedF32Array*)el)->data;
  el->bind((GenericEl*)-1);
  return ptr;
}

static int* ecs_i32_col(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL) {
    return NULL;
  }
  GenericEl *el = $(ecs->i32_cols)->get((char*)name);
  if (el == NULL) {
    return NULL;
  }
  int *ptr = ((TypedIntArray*)el)->data;
  el->bind((GenericEl*)-1);
  return ptr;
}

static TypedF32Array* ecs_f32_arr(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL) {
    return NULL;
  }
  return as(TypedF32Array, $(ecs->f32_cols)->get((char*)name));
}

static TypedIntArray* ecs_i32_arr(const char *name) {
  Ecs *ecs = slf;
  if (name == NULL) {
    return NULL;
  }
  return as(TypedIntArray, $(ecs->i32_cols)->get((char*)name));
}

static int ecs_spawn(void) {
  Ecs *ecs = slf;
  int eid;
  if (ecs->free_ids->length > 0) {
    eid = ecs->free_ids->data[--ecs->free_ids->length];
  } else {
    if (ecs->count >= ecs->capacity) {
      ensure_ecs_capacity(ecs, ecs->count + 1);
    }
    eid = ecs->count++;
    sync_column_lengths(ecs);
  }

  /* Zero all component slots for `eid` so recycled slots start clean. */
  int i;
  for (i = 0; i < ecs->f32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->f32_cols->key_slice->elements[i])->chars;
    TypedF32Array *arr = (TypedF32Array*)$(ecs->f32_cols)->get(k);
    arr->data[eid] = 0.0f;
    arr->bind((GenericEl*)-1);
  }
  for (i = 0; i < ecs->i32_cols->key_slice->length; i++) {
    char *k = ((String*)ecs->i32_cols->key_slice->elements[i])->chars;
    TypedIntArray *arr = (TypedIntArray*)$(ecs->i32_cols)->get(k);
    arr->data[eid] = 0;
    arr->bind((GenericEl*)-1);
  }

  ecs->alive->data[eid] = 1;
  ecs->alive_count++;
  return eid;
}

static Ecs* ecs_kill(int eid) {
  Ecs *ecs = slf;
  if (eid >= 0 && eid < ecs->count && ecs->alive->data[eid]) {
    ecs->alive->data[eid] = 0;
    ecs->alive_count--;
    $(ecs->free_ids)->push(eid)->bind((GenericEl*)-1);
  }
  return $(ecs);
}

static int ecs_is_alive(int eid) {
  Ecs *ecs = slf;
  return eid >= 0 && eid < ecs->count && ecs->alive->data[eid] != 0;
}

static Ecs* ecs_add_system(ChEcsSystem fn, void *ctx) {
  Ecs *ecs = slf;
  if (fn == NULL) {
    return $(ecs);
  }
  if (ecs->system_count >= ecs->system_cap) {
    ecs->system_cap = ecs->system_cap > 0 ? ecs->system_cap * 2 : 8;
    ecs->systems = realloc(
        ecs->systems, sizeof(ChEcsSystemEntry) * (size_t)ecs->system_cap);
  }
  ecs->systems[ecs->system_count].fn = fn;
  ecs->systems[ecs->system_count].ctx = ctx;
  ecs->system_count++;
  return $(ecs);
}

static Ecs* ecs_tick(float dt) {
  Ecs *ecs = slf;
  int i;
  for (i = 0; i < ecs->system_count; i++) {
    ecs->systems[i].fn($(ecs), dt, ecs->systems[i].ctx);
  }
  return $(ecs);
}

static Ecs* ecs_clear(void) {
  Ecs *ecs = slf;
  ecs->count = 0;
  ecs->alive_count = 0;
  ecs->free_ids->length = 0;
  sync_column_lengths(ecs);
  return $(ecs);
}

static Hash* ecs_stats(void) {
  Ecs *ecs = slf;
  return create(Hash, 6)
      ->put("alive", create(Number, (double)ecs->alive_count))
      ->put("high_water", create(Number, (double)ecs->count))
      ->put("capacity", create(Number, (double)ecs->capacity))
      ->put("f32_columns",
            create(Number, (double)ecs->f32_cols->key_slice->length))
      ->put("i32_columns",
            create(Number, (double)ecs->i32_cols->key_slice->length))
      ->put("systems", create(Number, (double)ecs->system_count));
}

static char* to_chars(void) {
  Ecs *ecs = slf;
  snprintf(ecs->repr, sizeof(ecs->repr),
           "<Ecs alive=%d/%d cap=%d cols=%d+%d>",
           ecs->alive_count, ecs->count, ecs->capacity,
           ecs->f32_cols->key_slice->length,
           ecs->i32_cols->key_slice->length);
  return ecs->repr;
}

/**
 * Constructor.
 */

Ecs* ch_new_Ecs(int initial_capacity) {
  int cap = initial_capacity > 0 ? initial_capacity : 64;
  Ecs *ecs = malloc(sizeof(Ecs));
  ecs->generic.magic = CH_MAGIC;
  ecs->generic.type_val = ECS;
  ecs->generic.prev_self = NULL;
  ecs->generic.next_self = NULL;
  ecs->generic.refs = 0;

  ecs->count = 0;
  ecs->alive_count = 0;
  ecs->capacity = cap;

  ecs->f32_cols = create(Hash, 16)->$$;
  ecs->f32_cols->bind((GenericEl*)-1);

  ecs->i32_cols = create(Hash, 8)->$$;
  ecs->i32_cols->bind((GenericEl*)-1);

  ecs->alive = create(TypedIntArray, cap)->fill(0)->$$;
  ecs->alive->length = 0;
  ecs->alive->bind((GenericEl*)-1);

  ecs->free_ids = create(TypedIntArray, 64)->$$;
  ecs->free_ids->bind((GenericEl*)-1);

  ecs->systems = NULL;
  ecs->system_count = 0;
  ecs->system_cap = 0;
  ecs->repr[0] = '\0';

  ecs->bind = bind_ecs;
  ecs->generic.bind = (void* (*)(GenericEl*))bind_ecs;
  ecs->generic.save = save;
  ecs->generic.del = del;
  ecs->generic.to_chars = to_chars;

  ecs->f32 = ecs_add_f32;
  ecs->i32 = ecs_add_i32;
  ecs->f32_col = ecs_f32_col;
  ecs->i32_col = ecs_i32_col;
  ecs->f32_arr = ecs_f32_arr;
  ecs->i32_arr = ecs_i32_arr;
  ecs->spawn = ecs_spawn;
  ecs->kill = ecs_kill;
  ecs->is_alive = ecs_is_alive;
  ecs->system = ecs_add_system;
  ecs->tick = ecs_tick;
  ecs->clear = ecs_clear;
  ecs->stats = ecs_stats;

  self((GenericEl*)ecs);
  return ecs;
}
