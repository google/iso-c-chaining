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
 * Demonstrates GCC/Clang opt-in RAII (`scoped` + `move_ref`) over `Box` and
 * `StatusOr` in a multi-step file parser with early error returns:
 *
 *   - `scoped` runs `release()` automatically on every return or block exit,
 *     replacing C's `goto cleanup;` ladder for `FILE*`, `malloc` scratch
 *     buffers, and `ChGenericEl` objects alike.
 *   - `move_ref(var)` transfers ownership out of a `scoped` variable (setting
 *     `var = NULL`) on the success path so cleanup skips the moved object.
 *
 * Requires GCC or Clang (`__attribute__((cleanup))` + statement expressions).
 *
 * Build & run with CMake:
 *   cmake -B build && cmake --build build
 *   ./build/chain_scoped
 *   ./build/chain_scoped --test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/box.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/test.h"
#include "core/types.h"

typedef struct {
  char host[32];
  int port;
  int workers;
  int *freed_counter;
} ServerConfig;

use_box(ServerConfig)

typedef struct {
  int files_closed;
  int buffers_freed;
  int configs_freed;
} CleanupStats;

static void close_tracked_file(void *raw) {
  FILE *fp = (FILE*)raw;
  if (fp != NULL) {
    fclose(fp);
  }
}

typedef struct {
  FILE *fp;
  CleanupStats *stats;
} TrackedFile;

static void close_tracked_handle(void *raw) {
  TrackedFile *tf = (TrackedFile*)raw;
  if (tf != NULL) {
    close_tracked_file(tf->fp);
    if (tf->stats != NULL) {
      tf->stats->files_closed++;
    }
    free(tf);
  }
}

typedef struct {
  char *data;
  CleanupStats *stats;
} ScratchLine;

static void free_scratch_line(void *raw) {
  ScratchLine *line = (ScratchLine*)raw;
  if (line != NULL) {
    if (line->stats != NULL) {
      line->stats->buffers_freed++;
    }
    free(line->data);
    free(line);
  }
}

static void free_server_config(void *raw) {
  ServerConfig *cfg = (ServerConfig*)raw;
  if (cfg != NULL) {
    if (cfg->freed_counter != NULL) {
      (*cfg->freed_counter)++;
    }
    free(cfg);
  }
}

static Box* open_temp_config(const char *contents, CleanupStats *stats) {
  TrackedFile *tf = malloc(sizeof(TrackedFile));
  tf->fp = tmpfile();
  tf->stats = stats;
  if (tf->fp != NULL) {
    fputs(contents, tf->fp);
    rewind(tf->fp);
  }
  return box_of(TrackedFile, tf, close_tracked_handle);
}

static Box* alloc_scratch_line(size_t cap, CleanupStats *stats) {
  ScratchLine *line = malloc(sizeof(ScratchLine));
  line->data = malloc(cap);
  line->stats = stats;
  return box_of(ScratchLine, line, free_scratch_line);
}

/*
 * Parses `"host=<str>\nport=<int>\nworkers=<int>\n"`.
 * Every early error return closes the temp `FILE*`, frees the scratch line
 * buffer, and discards the partially built `ServerConfigBox` automatically.
 */
static StatusOr* parse_server_config(const char *raw_text,
                                     CleanupStats *stats) {
  scoped Box *file_box = open_temp_config(raw_text, stats)->$$;
  FILE *fp = box_as(TrackedFile, file_box)->fp;
  if (fp == NULL) {
    return create(StatusOr, NULL, "failed to open temp config stream");
  }

  scoped Box *line_box = alloc_scratch_line(128, stats)->$$;
  char *line = box_as(ScratchLine, line_box)->data;

  ServerConfig *raw_cfg = malloc(sizeof(ServerConfig));
  memset(raw_cfg, 0, sizeof(ServerConfig));
  raw_cfg->freed_counter = &stats->configs_freed;

  /* Unowned `refs == 0` so caller can use standard `->value()->$$`. */
  scoped ServerConfigBox *cfg =
      create(ServerConfigBox, raw_cfg, free_server_config);

  int has_host = 0;
  int has_port = 0;
  while (fgets(line, 128, fp) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }
    if (strncmp(line, "host=", 5) == 0) {
      snprintf($(cfg)->get()->host, sizeof($(cfg)->get()->host), "%s",
               line + 5);
      has_host = 1;
    } else if (strncmp(line, "port=", 5) == 0) {
      int port = atoi(line + 5);
      if (port <= 0 || port > 65535) {
        /* Early return: `cfg`, `line_box`, and `file_box` all auto-free. */
        return create(StatusOr, NULL, "port out of range (1..65535)");
      }
      $(cfg)->get()->port = port;
      has_port = 1;
    } else if (strncmp(line, "workers=", 8) == 0) {
      int workers = atoi(line + 8);
      if (workers <= 0) {
        return create(StatusOr, NULL, "workers must be positive");
      }
      $(cfg)->get()->workers = workers;
    } else {
      return create(StatusOr, NULL, "unknown config directive");
    }
  }

  if (!has_host || !has_port) {
    return create(StatusOr, NULL, "missing required host or port");
  }
  if ($(cfg)->get()->workers == 0) {
    $(cfg)->get()->workers = 4;
  }

  /* Transfer `cfg` out; `file_box` and `line_box` still auto-clean on exit. */
  return create(StatusOr, move_ref(cfg), NULL);
}

static int run_demo(int quiet, int test_mode) {
  CleanupStats stats = {0, 0, 0};
  int bad_ok = 0;
  char bad_err[128] = "";
  char parsed_host[32] = "";
  int parsed_port = 0;
  int parsed_workers = 0;

  /* 1. Failing parse: `scoped` closes `FILE*` and frees partial `ServerConfig`. */
  {
    scoped StatusOr *bad =
        parse_server_config("host=api.internal\nport=99999\n", &stats)->$$;
    bad_ok = $(bad)->ok();
    snprintf(bad_err, sizeof(bad_err), "%s", $(bad)->error());
  }
  CleanupStats after_bad = stats;

  /* 2. Valid parse: `move_ref` keeps config alive until outer block exits. */
  {
    scoped StatusOr *good =
        parse_server_config("host=127.0.0.1\nport=8080\nworkers=8\n", &stats)
            ->$$;
    scoped ServerConfigBox *cfg =
        (ServerConfigBox*)as(Box, $(good)->value())->$$;
    snprintf(parsed_host, sizeof(parsed_host), "%s", $(cfg)->get()->host);
    parsed_port = $(cfg)->get()->port;
    parsed_workers = $(cfg)->get()->workers;
  }
  CleanupStats after_good = stats;

  if (!quiet) {
    printf("Invalid config error: %s\n", bad_err);
    printf("After invalid parse — closed files: %d, freed buffers: %d, freed configs: %d\n",
           after_bad.files_closed, after_bad.buffers_freed,
           after_bad.configs_freed);
    printf("Parsed valid config: host=%s port=%d workers=%d\n",
           parsed_host, parsed_port, parsed_workers);
    printf("After valid block exit — closed files: %d, freed buffers: %d, freed configs: %d\n",
           after_good.files_closed, after_good.buffers_freed,
           after_good.configs_freed);
  }

  if (test_mode) {
    Test *t = create(Test, "Scoped RAII & move_ref Example")->$$;
    $(t)->section("Automatic cleanup across early returns and move_ref")
        ->expect_num(create(Number, bad_ok), 0)
        ->expect_str(create(String, bad_err), "port out of range (1..65535)")
        ->expect_num(create(Number, after_bad.files_closed), 1)
        ->expect_num(create(Number, after_bad.buffers_freed), 1)
        ->expect_num(create(Number, after_bad.configs_freed), 1)
        ->expect_str(create(String, parsed_host), "127.0.0.1")
        ->expect_num(create(Number, parsed_port), 8080)
        ->expect_num(create(Number, parsed_workers), 8)
        ->expect_num(create(Number, after_good.files_closed), 2)
        ->expect_num(create(Number, after_good.buffers_freed), 2)
        ->expect_num(create(Number, after_good.configs_freed), 2);
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
