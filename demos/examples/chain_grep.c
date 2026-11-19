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
 * Recursive POSIX regex file searcher (`DIR*`, `FILE*`, `regex_t`)
 * demonstrating how `Box` + `scoped` wrap external OS/libc handles in
 * memory-managed scopes and chaining containers without modifying those types
 * to embed `ChGenericEl`.
 *
 *   - `regex_t` (`regcomp`/`regfree`) lives inside a `Hash` of named rules via
 *     `box_of(regex_t, ...)` and unwraps via `box_as(regex_t, ...)`.
 *   - Recursive `opendir()` (`DIR*`) and `fopen()` (`FILE*`) handles use
 *     `scoped Box` so every early error return or recursion unwind closes open
 *     descriptors automatically without `goto cleanup`.
 *
 * Build & run with CMake:
 *   cmake -B build && cmake --build build
 *   ./build/chain_grep "box_as|scoped" core examples
 *   ./build/chain_grep --test
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/boolean.h"
#include "core/box.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/test.h"
#include "core/types.h"

static void close_file(void *raw) {
  if (raw != NULL) {
    fclose((FILE*)raw);
  }
}

static void close_dir(void *raw) {
  if (raw != NULL) {
    closedir((DIR*)raw);
  }
}

static void free_regex(void *raw) {
  if (raw != NULL) {
    regfree((regex_t*)raw);
    free(raw);
  }
}

static StatusOr* compile_regex(const char *pattern) {
  regex_t *re = malloc(sizeof(regex_t));
  int rc = regcomp(re, pattern, REG_EXTENDED | REG_NOSUB);
  if (rc != 0) {
    char err[128];
    regerror(rc, re, err, sizeof(err));
    free(re);
    return create(StatusOr, NULL, err);
  }
  return create(StatusOr, box_of(regex_t, re, free_regex), NULL);
}

/*
 * Scans `path` against every compiled `regex_t` in `rules`:
 *   - Binding `fclose()` to `scoped Box *fp_box` ties cleanup to declaration
 *     so early returns can never leak `FILE*` (no `goto cleanup` required).
 *   - Storing `regex_t` handles inside `Hash *rules` via `box_of`/`box_as`
 *     gives heterogeneous map storage and automatic `regfree()` teardown
 *     without building a bespoke regex-table struct and destructor loop.
 */
static void search_file(const char *path, Hash *rules, Slice *matches) {
  scoped Box *fp_box = create(Box, fopen(path, "r"), close_file)->$$;
  FILE *fp = fp_box->ptr;
  if (fp == NULL) {
    return;
  }

  Slice *rule_names = $(rules)->keys();
  char line[512];
  int lineno = 0;
  while (fgets(line, sizeof(line), fp) != NULL) {
    lineno++;
    line[strcspn(line, "\r\n")] = '\0';

    int i;
    for (i = 0; i < rule_names->length; i++) {
      char *rule_name = as(String, $(rule_names)->get(i))->chars;
      regex_t *re = box_as(regex_t, $(rules)->get(rule_name));
      if (regexec(re, line, 0, NULL, 0) == 0) {
        char fmt[768];
        snprintf(fmt, sizeof(fmt), "%s:%d [%s]: %s",
                 path, lineno, rule_name, line);
        $(matches)->add(create(String, fmt));
        break;
      }
    }
  }
}

static void search_path(const char *path, Hash *rules, Slice *matches) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return;
  }
  if (S_ISREG(st.st_mode)) {
    search_file(path, rules, matches);
    return;
  }
  if (!S_ISDIR(st.st_mode)) {
    return;
  }

  scoped Box *dir_box = create(Box, opendir(path), close_dir)->$$;
  DIR *dp = dir_box->ptr;
  if (dp == NULL) {
    return;
  }

  struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    char child[512];
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    search_path(child, rules, matches);
  }
}

static int run_tests(void) {
  Test *t = create(Test, "chain_grep POSIX Resource Box Tests")->$$;

  const char *tmp_base = getenv("TEST_TMPDIR");
  char dir_tmpl[256];
  snprintf(dir_tmpl, sizeof(dir_tmpl), "%s/chain_grep_XXXXXX",
           tmp_base != NULL ? tmp_base : "/tmp");
  char *root = mkdtemp(dir_tmpl);

  char sub_dir[300], f1[300], f2[300];
  snprintf(sub_dir, sizeof(sub_dir), "%s/sub", root);
  snprintf(f1, sizeof(f1), "%s/a.c", root);
  snprintf(f2, sizeof(f2), "%s/sub/b.c", root);
  mkdir(sub_dir, 0700);

  {
    scoped Box *fp1 = create(Box, fopen(f1, "w"), close_file)->$$;
    fputs("int ok = 1;\n/* TODO: handle overflow */\n", (FILE*)fp1->ptr);
    scoped Box *fp2 = create(Box, fopen(f2, "w"), close_file)->$$;
    fputs("void *p = malloc(64);\nfree(p);\n", (FILE*)fp2->ptr);
  }

  scoped StatusOr *bad_re = compile_regex("[unclosed")->$$;
  scoped Hash *rules = create(Hash, 4)
      ->put("todo", as(Box, compile_regex("TODO:")->value()))
      ->put("alloc", as(Box, compile_regex("malloc\\(")->value()))
      ->$$;
  scoped Slice *matches = create(Slice, 8)->$$;
  search_path(root, rules, matches);

  unlink(f2);
  unlink(f1);
  rmdir(sub_dir);
  rmdir(root);

  $(t)->section("POSIX regex_t, DIR*, FILE* with Box & scoped")
      ->expect_num(create(Number, $(bad_re)->ok()), 0)
      ->expect_num(create(Number, matches->length), 2)
      ->expect_bool(
          create(Boolean, strstr(as(String, $(matches)->get(0))->chars,
                                 "[todo]: /* TODO: handle overflow */") != NULL ||
                          strstr(as(String, $(matches)->get(1))->chars,
                                 "[todo]: /* TODO: handle overflow */") != NULL),
          1)
      ->expect_bool(
          create(Boolean, strstr(as(String, $(matches)->get(0))->chars,
                                 "[alloc]: void *p = malloc(64);") != NULL ||
                          strstr(as(String, $(matches)->get(1))->chars,
                                 "[alloc]: void *p = malloc(64);") != NULL),
          1);

  int rc = $(t)->summary();
  release(t);
  return rc;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--test") == 0) {
    return run_tests();
  }
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <extended-regex> [path...]\n"
                    "       %s --test\n",
            argv[0], argv[0]);
    return 1;
  }

  scoped StatusOr *re_st = compile_regex(argv[1])->$$;
  if (!$(re_st)->ok()) {
    fprintf(stderr, "Invalid regex: %s\n", $(re_st)->error());
    return 1;
  }

  scoped Hash *rules = create(Hash, 2)
      ->put("match", as(Box, $(re_st)->value()))
      ->$$;
  scoped Slice *matches = create(Slice, 32)->$$;

  if (argc == 2) {
    search_path(".", rules, matches);
  } else {
    int i;
    for (i = 2; i < argc; i++) {
      search_path(argv[i], rules, matches);
    }
  }

  int i;
  for (i = 0; i < matches->length; i++) {
    puts(as(String, $(matches)->get(i))->chars);
  }
  return 0;
}
