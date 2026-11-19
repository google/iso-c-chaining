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
 * Pure ISO C demonstration of `Box` smart pointers (`core/box.h`):
 *
 *   1. External handles in containers (`box_of` / `box_as`): `Hash` and `Slice`
 *      require `ChGenericEl*` objects, so plain libc `FILE*` streams cannot be
 *      stored directly. Wrapping `tmpfile()` in `Box` lets a `Hash` and `Slice`
 *      co-own the same open `FILE*` and call `fclose()` exactly once when the
 *      last container releases it.
 *   2. Typed struct wrapper (`use_box`), `reset()`, and `take()`: `BufferBox`
 *      gives cast-free `->get()->len` access over a plain C struct, swaps its
 *      payload in place via `reset()`, and hands exclusive ownership back to C
 *      via `take()`.
 *
 * Build & run with CMake:
 *   cmake -B build && cmake --build build
 *   ./build/chain_box
 *   ./build/chain_box --test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/box.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/test.h"
#include "core/types.h"

typedef struct {
  char *data;
  size_t len;
} Buffer;

use_box(Buffer)

static int g_files_closed = 0;
static int g_buffers_freed = 0;

static void close_temp_file(void *raw) {
  if (raw != NULL) {
    fclose((FILE*)raw);
    g_files_closed++;
  }
}

static Buffer* new_buffer(const char *text) {
  Buffer *b = malloc(sizeof(Buffer));
  b->len = strlen(text);
  b->data = malloc(b->len + 1);
  memcpy(b->data, text, b->len + 1);
  return b;
}

static void free_buffer(void *raw) {
  Buffer *b = (Buffer*)raw;
  if (b != NULL) {
    free(b->data);
    free(b);
    g_buffers_freed++;
  }
}

static int run_demo(int quiet, int test_mode) {
  char read_back[64] = "";

  g_files_closed = 0;
  g_buffers_freed = 0;

  /* 1. Share an unmodified libc `FILE*` across a `Hash` and a `Slice`. */
  Box *err_stream = box_of(FILE, tmpfile(), close_temp_file)->$$;
  Hash *channels = create(Hash, 4)
      ->put("audit", box_of(FILE, tmpfile(), close_temp_file))
      ->put("error", err_stream)
      ->$$;
  Slice *mirrored = create(Slice, 2)->add(err_stream)->$$;
  release(err_stream); /* `channels` + `mirrored` now co-own `err_stream` */

  FILE *efp = box_as(FILE, $(channels)->get("error"));
  if (efp != NULL) {
    fputs("disk quota exceeded\n", efp);
  }

  /* Dropping `mirrored` keeps `err_stream` open because `channels` owns it. */
  release(mirrored);
  int closed_after_mirror = g_files_closed;

  efp = box_as(FILE, $(channels)->get("error"));
  if (efp != NULL) {
    rewind(efp);
    if (fgets(read_back, sizeof(read_back), efp) != NULL) {
      read_back[strcspn(read_back, "\n")] = '\0';
    }
  }

  /* Dropping `channels` closes both `"audit"` and `"error"` automatically. */
  release(channels);
  int closed_after_channels = g_files_closed;

  /* 2. Typed `BufferBox` (`use_box`), in-place `reset()`, and `take()`. */
  BufferBox *buf = create(BufferBox, new_buffer("draft-v1"), free_buffer)->$$;
  size_t first_len = $(buf)->get()->len;

  $(buf)->reset(new_buffer("published-v2"), free_buffer);
  int freed_after_reset = g_buffers_freed;

  Buffer *raw = $(buf)->take();
  release(buf); /* empty `Box` shell freed; `raw` stays alive */
  int freed_after_empty_box = g_buffers_freed;

  size_t final_len = raw->len;
  if (!quiet) {
    printf("Shared FILE* read after dropping Slice: \"%s\" "
           "(closed: %d -> %d)\n",
           read_back, closed_after_mirror, closed_after_channels);
    printf("BufferBox len %lu -> %lu (\"%s\"); buffers freed on reset=%d, "
           "after empty release=%d\n",
           (unsigned long)first_len, (unsigned long)final_len, raw->data,
           freed_after_reset, freed_after_empty_box);
  }
  free_buffer(raw);
  int freed_at_end = g_buffers_freed;

  if (test_mode) {
    Test *t = create(Test, "Box Smart Pointer Example")->$$;
    $(t)->section("Shared FILE* handles, BufferBox, reset() & take()")
        ->expect_str(create(String, read_back), "disk quota exceeded")
        ->expect_num(create(Number, closed_after_mirror), 0)
        ->expect_num(create(Number, closed_after_channels), 2)
        ->expect_num(create(Number, (double)first_len), 8)
        ->expect_num(create(Number, (double)final_len), 12)
        ->expect_num(create(Number, freed_after_reset), 1)
        ->expect_num(create(Number, freed_after_empty_box), 1)
        ->expect_num(create(Number, freed_at_end), 2);
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
