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
 * Benchmark for the SQLite-backed REST server (`Server` + `Db`).
 *
 * Build in Release mode (-O3) and run:
 *   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
 *   ./build/server_bench
 *
 * Compare with the equivalent Django benchmark:
 *   python3 benchmarks/django_bench.py
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/db.h"
#include "ext/server.h"

static Db *db;

/* GET /users (optional ?role=admin&min_score=90) */
static GenericEl* list_users(Hash *req) {
  return (GenericEl*)$(db)
      ->params($(req)->get("query"))
      ->query("SELECT id, name, role, score FROM users "
              "WHERE (:role IS NULL OR role = :role) "
              "  AND (:min_score IS NULL OR score >= :min_score) "
              "ORDER BY score DESC");
}

/* GET /users/:id */
static GenericEl* get_user(Hash *req) {
  return (GenericEl*)$(db)
      ->params($(req)->get("params"))
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE id = :id");
}

/* POST /users */
static GenericEl* create_user(Hash *req) {
  Hash *body = as(Hash, $(req)->get("body"));
  return (GenericEl*)$(db)
      ->insert("users", body)
      ->params(body)
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE name = :name ORDER BY id DESC");
}

/* PUT /users/:id */
static GenericEl* update_user(Hash *req) {
  GenericEl *id = as(Hash, $(req)->get("params"))->get("id");
  Hash *body = as(Hash, $(req)->get("body"));
  $(body)->put("id", id);
  return (GenericEl*)$(db)
      ->params(body)
      ->exec("UPDATE users SET "
             "  name = COALESCE(:name, name), "
             "  role = COALESCE(:role, role), "
             "  score = COALESCE(:score, score) "
             "WHERE id = :id")
      ->params(body)
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE id = :id");
}

/* DELETE /users/:id */
static GenericEl* delete_user(Hash *req) {
  GenericEl *id = as(Hash, $(req)->get("params"))->get("id");
  $(db)
      ->params($(req)->get("params"))
      ->exec("DELETE FROM users WHERE id = :id");
  return (GenericEl*)create(Hash, 2)
      ->put("deleted", create(Boolean, true))
      ->put("id", id);
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void run_one(Server *server, char *method, char *path, char *query,
                    char *body, size_t *bytes_out) {
  StatusOr *res = $(server)->dispatch(method, path, query, body)->$$;
  char *json = $(res)->chars();
  *bytes_out += strlen(json);
  release(res);
}

int main(void) {
  db = create(Db, ":memory:")
      ->exec("CREATE TABLE users ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name TEXT NOT NULL,"
             "  role TEXT NOT NULL,"
             "  score REAL"
             ")")
      ->insert("users", create(Hash, 3)
          ->put("name", create(String, "Alice"))
          ->put("role", create(String, "admin"))
          ->put("score", create(Number, 98.25)))
      ->insert("users", create(Hash, 3)
          ->put("name", create(String, "Bob"))
          ->put("role", create(String, "viewer"))
          ->put("score", create(Number, 82)))->$$;

  Server *server = create(Server, 0)
      ->get("/users", list_users)
      ->get("/users/:id", get_user)
      ->post("/users", create_user)
      ->put("/users/:id", update_user)
      ->delete_route("/users/:id", delete_user)->$$;

  const int n_reqs = 10000;
  const int crud_cycles = 2000;
  size_t total_bytes = 0;

  /* Workload 1: GET /users?role=admin&min_score=90 */
  double t0 = now_sec();
  for (int i = 0; i < n_reqs; i++) {
    run_one(server, "GET", "/users", "role=admin&min_score=90", NULL,
            &total_bytes);
  }
  double dt_list = now_sec() - t0;

  /* Workload 2: GET /users/1 */
  t0 = now_sec();
  for (int i = 0; i < n_reqs; i++) {
    run_one(server, "GET", "/users/1", NULL, NULL, &total_bytes);
  }
  double dt_get = now_sec() - t0;

  /* Workload 3: 5-step CRUD cycle (2,000 cycles = 10,000 requests) */
  t0 = now_sec();
  for (int i = 0; i < crud_cycles; i++) {
    StatusOr *post_res = $(server)->dispatch(
        "POST", "/users", NULL,
        "{\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}")->$$;
    Hash *created = as(Hash, $(post_res)->value());
    int new_id = (int)as(Number, $(created)->get("id"))->value();
    total_bytes += strlen($(post_res)->chars());
    release(post_res);

    char path_buf[64];
    snprintf(path_buf, sizeof(path_buf), "/users/%d", new_id);

    run_one(server, "GET", "/users", "role=admin&min_score=90", NULL,
            &total_bytes);
    run_one(server, "PUT", path_buf, NULL, "{\"score\":99.75}", &total_bytes);
    run_one(server, "GET", path_buf, NULL, NULL, &total_bytes);
    run_one(server, "DELETE", path_buf, NULL, NULL, &total_bytes);
  }
  double dt_crud = now_sec() - t0;

  printf("iso-c-chaining (Server + Db, SQLite :memory:):\n");
  printf("  GET /users?role=admin&min_score=90 : %7.2f ms for %d reqs "
         "(%9.0f req/s, %6.2f us/req)\n",
         dt_list * 1000.0, n_reqs, n_reqs / dt_list, (dt_list * 1e6) / n_reqs);
  printf("  GET /users/1                       : %7.2f ms for %d reqs "
         "(%9.0f req/s, %6.2f us/req)\n",
         dt_get * 1000.0, n_reqs, n_reqs / dt_get, (dt_get * 1e6) / n_reqs);
  printf("  5-step CRUD mix (POST/GET/PUT/DEL) : %7.2f ms for %d reqs "
         "(%9.0f req/s, %6.2f us/req)\n",
         dt_crud * 1000.0, crud_cycles * 5, (crud_cycles * 5) / dt_crud,
         (dt_crud * 1e6) / (crud_cycles * 5));
  printf("  (serialized JSON bytes: %zu)\n", total_bytes);

  release(server);
  release(db);
  return 0;
}
