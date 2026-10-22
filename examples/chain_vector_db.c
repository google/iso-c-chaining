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
 * Example lightweight vector database built on top of `Db` (SQLite) and
 * `TypedFloatArray`, indexing the system's Linux man-page dictionary
 * (`/usr/bin/whatis`).
 *
 * Demonstrates:
 *   1. Turning raw strings into L2-normalized `TypedFloatArray` embedding
 *      vectors via `embed_text()` using a zero-dependency FastText-style word +
 *      subword character-trigram feature-hashing embedder.
 *   2. Populating an in-memory SQLite database from `/usr/bin/whatis` (sections
 *      1, 2, and 3: CLI commands, Linux system calls, and C library functions)
 *      and storing `TypedFloatArray` embedding vectors via `Db::insert()`.
 *   3. Registering a zero-dependency `cosine_similarity(vec_a, vec_b)` SQL
 *      scalar function on `db->handle` via `sqlite3_create_function()`.
 *   4. Running natural-language vector similarity queries (plus optional
 *      man-page section filtering) by binding `embed_text("...")` directly in
 *      `Db::params()`.
 *   5. Aggregating similarity scores with `TypedFloatArray` (`filter` -> `sort`
 *      -> `at` / `subarray`).
 *
 * Build and run:
 *   cmake -B build && cmake --build build
 *   ./build/chain_vector_db
 *   ./build/chain_vector_db "What SQL tool should I use"
 *   ./build/chain_vector_db "create a child process" 2
 *   ./build/chain_vector_db --test
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sqlite3.h"
#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"
#include "ext/db.h"
#include "ext/test.h"

#define EMBED_DIM 256

static double square_double(double x) { return x * x; }
static double add_doubles(double acc, double x) { return acc + x; }
static int is_positive(double x) { return x > 0.0; }

/*
 * FNV-1a 32-bit hash over `len` bytes, used by the built-in word + subword
 * feature hasher to project tokens and character trigrams into `EMBED_DIM`
 * buckets.
 */
static unsigned int fnv1a_bytes(const char *bytes, size_t len) {
  unsigned int h = 2166136261u;
  size_t i;
  for (i = 0; i < len; i++) {
    h ^= (unsigned int)(unsigned char)bytes[i];
    h *= 16777619u;
  }
  /* Finalize with Murmur3 32-bit avalanche so low bits mix all 32 bits. */
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

static void accumulate_feature(TypedFloatArray *vec, const char *feat,
                               size_t len, double weight, int probes) {
  unsigned int base = fnv1a_bytes(feat, len);
  double per_probe = weight / sqrt((double)probes);
  int p;
  for (p = 0; p < probes; p++) {
    unsigned int h = base ^ ((unsigned int)p * 0x9e3779b9u);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    int bucket = (int)(h % (unsigned int)vec->length);
    double sign = (h & 0x40000000u) != 0 ? 1.0 : -1.0;
    vec->data[bucket] += sign * per_probe;
  }
}

/*
 * Generates an L2-normalized `TypedFloatArray*` embedding vector from `text`
 * using a FastText-style word + character-trigram feature-hashing projection
 * over tokens produced by `String::split()`.
 */
static TypedFloatArray* embed_text(const char *text) {
  TypedFloatArray *vec = create(TypedFloatArray, EMBED_DIM)->fill(0.0)->$$;
  Slice *tokens = create(String, (char*)(text != NULL ? text : ""))
      ->split(" ,.?!:;()-\t\n")->$$;

  int t;
  for (t = 0; t < tokens->length; t++) {
    const char *raw = ((String*)tokens->elements[t])->chars;
    char word[68];
    size_t wlen = 0;
    int upper_count = 0;
    word[wlen++] = '<';
    size_t j;
    for (j = 0; raw[j] != '\0' && wlen < sizeof(word) - 2; j++) {
      unsigned char uc = (unsigned char)raw[j];
      if (isupper(uc)) {
        upper_count++;
      }
      word[wlen++] = (char)tolower(uc);
    }
    word[wlen++] = '>';
    word[wlen] = '\0';

    if (wlen < 5) {
      continue;
    }

    /*
     * Information-content scaling (without a stopword list):
     * - Multi-uppercase tokens (acronyms like "SQL", "URL", "POSIX", "IPC")
     *   carry high technical specificity.
     * - Otherwise weight scales with character length so longer technical terms
     *   naturally outweigh short function words.
     */
    size_t raw_len = wlen - 2;
    double word_weight = upper_count >= 2 ? 3.5 : (raw_len >= 5 ? 1.5 : 1.0);

    accumulate_feature(vec, word, wlen, word_weight, 4);
    size_t trigram_count = wlen - 2;
    double scale = (0.3 * word_weight) / sqrt((double)trigram_count);
    for (j = 0; j + 3 <= wlen; j++) {
      accumulate_feature(vec, word + j, 3, scale, 2);
    }
  }
  release(tokens);

  /* L2-normalize `vec` using `TypedFloatArray::map` and `reduce`. */
  double norm = sqrt(
      $(vec)->subarray(0, vec->length)->map(square_double)->reduce(add_doubles, 0.0));
  if (norm > 0.0) {
    int i;
    for (i = 0; i < vec->length; i++) {
      vec->data[i] = round((vec->data[i] / norm) * 10000.0) / 10000.0;
    }
  }

  vec->generic.refs--;
  return $(vec);
}

/*
 * Parses a JSON float array string (`"[0.15, 0.22, ...]"`) back into a
 * contiguous `TypedFloatArray*`.
 */
static TypedFloatArray* parse_vector_text(const char *text) {
  TypedFloatArray *vec = create(TypedFloatArray, EMBED_DIM);
  if (text == NULL) {
    return vec;
  }
  const char *p = text;
  while (*p != '\0') {
    while (*p == '[' || *p == ']' || *p == ',' || *p == ' ' || *p == '\t') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    char *end = NULL;
    double val = strtod(p, &end);
    if (end == p) {
      break;
    }
    $(vec)->push(val);
    p = end;
  }
  return $(vec);
}

static void free_cached_vec(void *p) {
  if (p != NULL) {
    release((TypedFloatArray*)p);
  }
}

/*
 * Computes cosine similarity between two `TypedFloatArray` vectors.
 */
static double cosine_similarity_vecs(TypedFloatArray *a, TypedFloatArray *b) {
  int n = a->length < b->length ? a->length : b->length;
  if (n == 0) {
    return 0.0;
  }
  double dot = 0.0;
  double norm_a = 0.0;
  double norm_b = 0.0;
  int i;
  for (i = 0; i < n; i++) {
    double va = a->data[i];
    double vb = b->data[i];
    dot += va * vb;
    norm_a += va * va;
    norm_b += vb * vb;
  }
  if (norm_a == 0.0 || norm_b == 0.0) {
    return 0.0;
  }
  return dot / (sqrt(norm_a) * sqrt(norm_b));
}

/*
 * Custom SQLite scalar function `cosine_similarity(vec_a_json, vec_b_json)`.
 * Caches the parsed query vector (`argv[1]`) across rows via
 * `sqlite3_get_auxdata()` / `sqlite3_set_auxdata()`.
 */
static void sql_cosine_similarity(sqlite3_context *ctx, int argc,
                                  sqlite3_value **argv) {
  if (argc != 2 || sqlite3_value_type(argv[0]) == SQLITE_NULL ||
      sqlite3_value_type(argv[1]) == SQLITE_NULL) {
    sqlite3_result_double(ctx, 0.0);
    return;
  }
  const char *text_a = (const char*)sqlite3_value_text(argv[0]);
  TypedFloatArray *vec_a = parse_vector_text(text_a)->$$;

  TypedFloatArray *vec_b = (TypedFloatArray*)sqlite3_get_auxdata(ctx, 1);
  if (vec_b == NULL) {
    const char *text_b = (const char*)sqlite3_value_text(argv[1]);
    vec_b = parse_vector_text(text_b)->$$;
    vec_b->bind((GenericEl*)-1);
    sqlite3_set_auxdata(ctx, 1, vec_b, free_cached_vec);
  }

  double sim = cosine_similarity_vecs(vec_a, vec_b);
  release(vec_a);

  sqlite3_result_double(ctx, sim);
}

static void insert_man_page(Db *db, const char *name, const char *section,
                            const char *description) {
  char combined[512];
  snprintf(combined, sizeof(combined), "%s %s", name, description);
  $(db)->insert("man_pages", create(Hash, 4)
      ->put("name", create(String, (char*)name))
      ->put("section", create(String, (char*)section))
      ->put("description", create(String, (char*)description))
      ->put("embedding", embed_text(combined)));
}

/*
 * Populates `man_pages` by executing `/usr/bin/whatis -l -s 1,2,3 -w *` and
 * inserting each entry with its `TypedFloatArray` embedding. Falls back to a
 * built-in seed list if `/usr/bin/whatis` is unavailable.
 */
static int populate_from_whatis(Db *db) {
  int pipefd[2];
  int count = 0;

  $(db)->exec("BEGIN TRANSACTION");

  if (pipe(pipefd) == 0) {
    pid_t pid = fork();
    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      close(pipefd[1]);
      char *const argv[] = {
          "/usr/bin/whatis", "-l", "-s", "1,2,3", "-w", "*", NULL};
      execv(argv[0], argv);
      _exit(127);
    } else if (pid > 0) {
      close(pipefd[1]);
      FILE *fp = fdopen(pipefd[0], "r");
      if (fp != NULL) {
        char line[1024];
        char prev_desc[512] = "";
        while (fgets(line, sizeof(line), fp) != NULL) {
          if (!islower((unsigned char)line[0])) {
            continue;
          }
          char *space = strchr(line, ' ');
          char *open_paren = space != NULL ? strchr(space, '(') : NULL;
          char *close_paren = open_paren != NULL ? strchr(open_paren, ')') : NULL;
          char *sep = close_paren != NULL ? strstr(close_paren, " - ") : NULL;
          if (space == NULL || open_paren == NULL || close_paren == NULL ||
              sep == NULL) {
            continue;
          }

          *space = '\0';
          *close_paren = '\0';
          const char *name = line;
          const char *sec = open_paren + 1;
          char *desc = sep + 3;

          /* Keep standard sections 1 (CLI), 2 (syscalls), and 3 (libc). */
          if (strcmp(sec, "1") != 0 && strcmp(sec, "2") != 0 &&
              strcmp(sec, "3") != 0) {
            continue;
          }

          size_t dlen = strlen(desc);
          while (dlen > 0 &&
                 (desc[dlen - 1] == '\n' || desc[dlen - 1] == '\r')) {
            desc[--dlen] = '\0';
          }
          if (dlen < 4 || strcmp(desc, prev_desc) == 0 ||
              strstr(desc, "DEPRECATED") != NULL ||
              strstr(desc, "unimplemented") != NULL) {
            continue;
          }

          strncpy(prev_desc, desc, sizeof(prev_desc) - 1);
          prev_desc[sizeof(prev_desc) - 1] = '\0';
          insert_man_page(db, name, sec, desc);
          count++;
        }
        fclose(fp);
      } else {
        close(pipefd[0]);
      }
      int status = 0;
      waitpid(pid, &status, 0);
    } else {
      close(pipefd[0]);
      close(pipefd[1]);
    }
  }

  if (count == 0) {
    insert_man_page(db, "malloc", "3", "allocate and free dynamic memory");
    insert_man_page(db, "socket", "2", "create an endpoint for communication");
    insert_man_page(db, "fork", "2", "create a child process");
    insert_man_page(db, "select", "2", "synchronous I/O multiplexing");
    insert_man_page(db, "epoll_wait", "2",
                    "wait for an I/O event on an epoll file descriptor");
    insert_man_page(db, "curl", "1", "transfer a URL");
    insert_man_page(db, "tinysparql-sql", "1",
                    "Use SQL to query the TinySPARQL databases.");
    count = 7;
  }

  $(db)->exec("COMMIT");
  return count;
}

int main(int argc, char **argv) {
  int test_mode = (argc > 1 && strcmp(argv[1], "--test") == 0);
  const char *custom_query = (!test_mode && argc > 1) ? argv[1] : NULL;
  const char *custom_sec = (!test_mode && argc > 2) ? argv[2] : NULL;

  /*
   * 1. Open an in-memory SQLite database, register `cosine_similarity()`, and
   *    populate `man_pages` from `/usr/bin/whatis` (with `embed_text()`).
   */
  Db *db = create(Db, ":memory:")->$$;
  sqlite3_create_function(
      db->handle, "cosine_similarity", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
      NULL, sql_cosine_similarity, NULL, NULL);

  $(db)->exec("CREATE TABLE man_pages ("
              "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "  name TEXT NOT NULL,"
              "  section TEXT NOT NULL,"
              "  description TEXT NOT NULL,"
              "  embedding TEXT NOT NULL"
              ")");

  int indexed_count = populate_from_whatis(db);

  /*
   * 2. Embed a natural-language search query string (either user-supplied via
   *    `argv[1]` or the default query) with `embed_text()` and bind the
   *    resulting `TypedFloatArray` directly via `Db::params()`.
   */
  const char *query_text = custom_query != NULL ?
      custom_query : "allocate and free dynamic memory";
  TypedFloatArray *query_vec = embed_text(query_text)->$$;

  Slice *top_k = as(Slice, $(db)
      ->params(create(Hash, 1)
          ->put("query_vec", query_vec))
      ->query("SELECT name, section, description, "
              "       ROUND(sim, 4) AS similarity "
              "FROM ("
              "  SELECT name, section, description, "
              "         cosine_similarity(embedding, :query_vec) AS sim "
              "  FROM man_pages"
              ") "
              "WHERE sim > 0.0 "
              "ORDER BY sim DESC "
              "LIMIT 5")
      ->value())->$$;

  /*
   * 3. Hybrid metadata + vector query: filter by man-page `section` (`1` = CLI,
   *    `2` = syscall, `3` = libc) and return the closest match for `query_vec`.
   */
  const char *sec_filter = custom_sec != NULL ? custom_sec : "3";
  Hash *best_in_sec = as(Hash, $(db)
      ->params(create(Hash, 2)
          ->put("section", create(String, (char*)sec_filter))
          ->put("query_vec", query_vec))
      ->query_one("SELECT name, section, description, "
                  "       ROUND(cosine_similarity(embedding, :query_vec), 4) "
                  "         AS similarity "
                  "FROM man_pages "
                  "WHERE section = :section "
                  "ORDER BY similarity DESC "
                  "LIMIT 1")
      ->value())->$$;

  /*
   * 4. Collect similarity scores into a `TypedFloatArray` and run a functional
   *    aggregation chain (`filter` -> `sort` -> `at`).
   */
  TypedFloatArray *scores = create(TypedFloatArray, top_k->length)->$$;
  int i;
  for (i = 0; i < top_k->length; i++) {
    Hash *row = (Hash*)top_k->elements[i];
    double s = as(Number, $(row)->get("similarity"))->value();
    row->bind((GenericEl*)-1);
    $(scores)->push(s);
  }
  double max_sim = $(scores)
      ->filter(is_positive)
      ->sort()
      ->at(-1);

  Hash *top_hit = (Hash*)top_k->elements[0];
  GenericEl *top_desc = $(top_hit)->get("description");
  top_hit->bind((GenericEl*)-1);
  GenericEl *best_sec_desc = $(best_in_sec)->get("description");
  best_in_sec->bind((GenericEl*)-1);

  int rc = 0;
  if (test_mode) {
    rc = create(Test, "Vector DB (whatis Man-Page Dictionary) Self-Test")
        ->section("Top-K semantic search over Linux man pages")
        ->expect_str(top_desc, "allocate and free dynamic memory")
        ->section("Hybrid section metadata + vector query")
        ->expect_str(best_sec_desc, "allocate and free dynamic memory")
        ->section("TypedFloatArray positive similarity score")
        ->expect_bool(create(Boolean, max_sim > 0.5), 1)
        ->summary();
  } else {
    TypedFloatArray *preview = $(query_vec)->subarray(0, 8)->$$;
    printf("Indexed %d Linux man pages from /usr/bin/whatis (sections 1, 2, 3)\n",
           indexed_count);
    printf("Query: \"%s\"\n", query_text);
    puts("Embedding preview (first 8 of 256 dims via TypedFloatArray::subarray):");
    puts($(preview)->chars());
    release(preview);
    puts("Top semantic matches (JSON):");
    puts($(top_k)->chars());
    if (custom_sec != NULL) {
      printf("Best match in man section (%s):\n", sec_filter);
      puts($(best_in_sec)->chars());
    }
    printf("Max similarity score: %.4f\n", max_sim);
    puts("\nTop 3 results:");
    for (i = 0; i < top_k->length && i < 3; i++) {
      Hash *row = (Hash*)top_k->elements[i];
      const char *name = $(row)->get("name")->to_chars();
      row->bind((GenericEl*)-1);
      const char *sec = $(row)->get("section")->to_chars();
      row->bind((GenericEl*)-1);
      const char *desc = $(row)->get("description")->to_chars();
      row->bind((GenericEl*)-1);
      double sim = as(Number, $(row)->get("similarity"))->value();
      row->bind((GenericEl*)-1);
      printf("  %d. [%.4f] %s (%s) - %s\n", i + 1, sim, name, sec, desc);
    }
  }

  release(scores);
  release(best_in_sec);
  release(top_k);
  release(query_vec);
  release(db);
  return rc;
}
