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
 * Pure ISO C demonstration of `Box` smart pointers over existing C structs and
 * libc resources (`FILE*`, custom multi-allocation textures, and materials):
 *
 *   - `Box` wraps libc `FILE*` (`tmpfile()`) + raw buffers inside `Hash` and
 *     `Slice` containers so `release()` cleans up all resources uniformly.
 *   - `use_box(Texture)` creates a statically typed `TextureBox` facade so
 *     `$(tex)->get()->width` works without casts.
 *   - `box_of(Material, ...)` + `box_as(Material, $(cache)->get(...))` mutate
 *     cached C structs inline with runtime `#Type` tag checking.
 *   - `reset()` hot-reloads a texture in place; `take()` hands exclusive raw
 *     C ownership back to the caller.
 *
 * Build & run with CMake:
 *   cmake -B build && cmake --build build
 *   ./build/chain_box
 *   ./build/chain_box --test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/box.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/test.h"
#include "core/types.h"

typedef struct {
  char name[32];
  int width;
  int height;
  unsigned char *pixels;
  int *freed_counter;
} Texture;

typedef struct {
  char shader[32];
  float roughness;
  int *freed_counter;
} Material;

/* Declares `TextureBox` so `$(tex)->get()->field` is typed as `Texture*`. */
use_box(Texture)

static void free_texture(void *raw) {
  Texture *tex = (Texture*)raw;
  if (tex != NULL) {
    if (tex->freed_counter != NULL) {
      (*tex->freed_counter)++;
    }
    free(tex->pixels);
    free(tex);
  }
}

static void free_material(void *raw) {
  Material *mat = (Material*)raw;
  if (mat != NULL) {
    if (mat->freed_counter != NULL) {
      (*mat->freed_counter)++;
    }
    free(mat);
  }
}

static void close_temp_file(void *raw) {
  FILE *fp = (FILE*)raw;
  if (fp != NULL) {
    fclose(fp);
  }
}

static Texture* alloc_texture(const char *name, int w, int h,
                              unsigned char fill, int *freed_counter) {
  size_t byte_count = (size_t)(w * h * 4);
  Texture *tex = malloc(sizeof(Texture));
  snprintf(tex->name, sizeof(tex->name), "%s", name);
  tex->width = w;
  tex->height = h;
  tex->pixels = malloc(byte_count);
  memset(tex->pixels, fill, byte_count);
  tex->freed_counter = freed_counter;
  return tex;
}

static Material* alloc_material(const char *shader, float roughness,
                                int *freed_counter) {
  Material *mat = malloc(sizeof(Material));
  snprintf(mat->shader, sizeof(mat->shader), "%s", shader);
  mat->roughness = roughness;
  mat->freed_counter = freed_counter;
  return mat;
}

static int run_demo(int quiet, int test_mode) {
  int textures_freed = 0;
  int materials_freed = 0;
  Test *t = test_mode ? create(Test, "Box Smart Pointer Example")->$$ : NULL;

  /* 1. Typed `TextureBox` + untyped `Box(FILE*)` + `box_of(Material)` cache. */
  TextureBox *albedo = create(
      TextureBox, alloc_texture("hull_albedo", 64, 64, 0x80, &textures_freed),
      free_texture)->$$;
  TextureBox *normal = create(
      TextureBox, alloc_texture("hull_normal", 64, 64, 0xFF, &textures_freed),
      free_texture)->$$;

  /* `Box` lets a standard libc `FILE*` live inside the same `Hash` tree. */
  Box *trace_log = create(Box, tmpfile(), close_temp_file)->$$;
  FILE *log_fp = trace_log->ptr; /* implicit void* -> FILE* in ISO C */
  if (log_fp != NULL) {
    fputs("texture_cache=initialized\n", log_fp);
    rewind(log_fp);
  }

  Hash *cache = create(Hash, 8)
      ->put("albedo", albedo)
      ->put("normal", normal)
      ->put("log", trace_log)
      ->put("hull_mat",
            box_of(Material, alloc_material("pbr_metal", 0.65f,
                                            &materials_freed),
                   free_material))
      ->$$;

  /* Active draw queue also shares ownership of `albedo` and `normal`. */
  Slice *draw_queue = create(Slice, 4)->add(albedo)->add(normal)->$$;
  int albedo_refs_peak = albedo->generic.refs; /* local + cache + draw_queue */

  /* Static typed mutation via `use_box(Texture)` and tag-checked `box_as`. */
  $(albedo)->get()->width = 128;
  box_as(Material, $(cache)->get("hull_mat"))->roughness = 0.25f;
  float updated_roughness = box_as(Material, $(cache)->get("hull_mat"))->roughness;

  char log_line[64] = "";
  if (box_as(FILE, $(cache)->get("log")) != NULL) {
    if (fgets(log_line, sizeof(log_line), (FILE*)$(trace_log)->get()) != NULL) {
      log_line[strcspn(log_line, "\n")] = '\0';
    }
  }

  /* Drop local refs; `cache` and `draw_queue` keep both textures alive. */
  release(trace_log);
  release(normal);
  release(albedo);
  int freed_while_queued = textures_freed;

  /* Drop `draw_queue`; `cache` still retains both textures (refs == 1). */
  release(draw_queue);
  int freed_after_queue = textures_freed;

  /* Drop `cache`: closes `FILE*`, frees both textures and the material. */
  release(cache);
  int freed_after_cache = textures_freed;

  /* 2. `reset()` swaps texture in place; `take()` hands raw C pointer back. */
  TextureBox *scratch = create(
      TextureBox, alloc_texture("preview", 16, 16, 0x10, &textures_freed),
      free_texture)->$$;
  $(scratch)->reset(
      alloc_texture("final_mip", 256, 256, 0x40, &textures_freed),
      free_texture);
  int freed_after_reset = textures_freed;

  Texture *raw_tex = $(scratch)->take();
  int freed_after_take = textures_freed;
  release(scratch);
  int freed_after_empty_release = textures_freed;

  int final_width = raw_tex->width;
  free_texture(raw_tex);
  int freed_at_end = textures_freed;

  if (!quiet) {
    printf("Log line read back from Box(FILE*): %s\n", log_line);
    printf("Albedo peak refs: %d, updated roughness: %.2f\n",
           albedo_refs_peak, updated_roughness);
    printf("Textures freed — queued: %d, queue dropped: %d, cache dropped: %d\n",
           freed_while_queued, freed_after_queue, freed_after_cache);
    printf("Reset freed preview (%d -> %d); take() kept final_mip (%dx%d) until manual free (%d -> %d)\n",
           freed_after_cache, freed_after_reset, final_width, final_width,
           freed_after_empty_release, freed_at_end);
  }

  if (test_mode) {
    $(t)->section("Box asset cache, FILE* handle, reset & take")
        ->expect_str(create(String, log_line), "texture_cache=initialized")
        ->expect_num(create(Number, albedo_refs_peak), 3)
        ->expect_num(create(Number, updated_roughness), 0.25)
        ->expect_num(create(Number, freed_while_queued), 0)
        ->expect_num(create(Number, freed_after_queue), 0)
        ->expect_num(create(Number, freed_after_cache), 2)
        ->expect_num(create(Number, materials_freed), 1)
        ->expect_num(create(Number, freed_after_reset), 3)
        ->expect_num(create(Number, freed_after_take), 3)
        ->expect_num(create(Number, freed_after_empty_release), 3)
        ->expect_num(create(Number, final_width), 256)
        ->expect_num(create(Number, freed_at_end), 4);
    int rc = $(t)->summary();
    release(t);
    return rc;
  }

  return 0;
}

int main(int argc, char **argv) {
  int test_mode = (argc > 1 && strcmp(argv[1], "--test") == 0);
  return run_demo(test_mode, test_mode);
}
