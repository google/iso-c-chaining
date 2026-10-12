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

#include "ext/db.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/test.h"

static void test_sqlite_db(Test *t) {
  Db *local_db = create(Db, ":memory:")
      ->exec("CREATE TABLE items ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  label TEXT NOT NULL,"
             "  qty INTEGER,"
             "  price REAL,"
             "  active INTEGER,"
             "  notes TEXT,"
             "  meta TEXT"
             ");"
             "INSERT INTO items (label, qty, price, active, notes, meta) "
             "VALUES ('seed', 1, 9.5, 1, NULL, '[]');   ")
      ->insert("items", create(Hash, 6)
          ->put("label", create(String, "widget"))
          ->put("qty", create(Number, 10))
          ->put("price", create(Number, 19.75))
          ->put("active", create(Boolean, false))
          ->put("notes", create(Null))
          ->put("meta", create(Slice, 2)
              ->add(create(String, "tag1"))
              ->add(create(String, "tag2"))))
      /* Empty Hash insert should be a safe no-op. */
      ->insert("items", create(Hash, 0))->$$;

  $(t)->section("SQLite Db: parameter bindings, types & error recovery")
      ->expect_str(local_db, "<Db ok>")
      /* Positional '?' parameters via Slice */
      ->expect_str(
          $(local_db)
              ->params(create(Slice, 2)
                  ->add(create(String, "widget"))
                  ->add(create(Number, 5)))
              ->query_one("SELECT label, qty, price, active, notes, meta "
                          "FROM items WHERE label = ? AND qty > ?"),
          "{\"label\":\"widget\",\"qty\":10,\"price\":19.75,"
          "\"active\":0,\"notes\":null,\"meta\":\"[\\\"tag1\\\",\\\"tag2\\\"]\"}")
      /* Named ':key', '@key', and '$key' parameters + missing param as NULL */
      ->expect_str(
          $(local_db)
              ->params(create(Hash, 3)
                  ->put("lbl", create(String, "seed"))
                  ->put("min_q", create(Number, 1))
                  ->put("max_p", create(Number, 20.0)))
              ->query_one("SELECT label, qty, :missing_param AS unbound "
                          "FROM items WHERE label = :lbl AND qty >= @min_q "
                          "AND price <= $max_p"),
          "{\"label\":\"seed\",\"qty\":1,\"unbound\":null}")
      /* Multi-row query and empty result set */
      ->expect_num(
          create(Number,
                 as(Slice,
                    $(local_db)->query("SELECT id FROM items")->value())
                     ->length),
          2)
      ->expect_str(
          $(local_db)->query("SELECT id FROM items WHERE qty > 1000"),
          "[]")
      ->expect_err(
          $(local_db)->query_one("SELECT id FROM items WHERE qty > 1000"),
          "No rows returned")
      /* Direct SQL syntax errors and NULL SQL */
      ->expect_err($(local_db)->query("SELECT * FROM missing_table"),
                   "no such table")
      ->expect_err($(local_db)->query_one("SELECT * FROM missing_table"),
                   "no such table")
      ->expect_err($(local_db)->query(NULL), "Invalid database handle or SQL")
      ->expect_err($(local_db)->query_one(NULL),
                   "Invalid database handle or SQL");

  /* Deferred error from exec() surfaces on next query() and then clears. */
  $(local_db)->exec("INVALID SQL STATEMENT");
  $(t)->expect_str(local_db, "near \"INVALID\": syntax error")
      ->expect_err($(local_db)->query("SELECT 1"), "syntax error")
      ->expect_str(local_db, "<Db ok>")
      ->expect_str($(local_db)->query_one("SELECT 1 AS ok"), "{\"ok\":1}");

  /* Deferred error from insert() surfaces on next query_one() and clears. */
  $(local_db)->insert(
      "nonexistent_table",
      create(Hash, 1)->put("k", create(String, "v")));
  $(t)->expect_err(
          $(local_db)->query_one("SELECT 1 AS ok"),
          "no such table: nonexistent_table")
      ->expect_str($(local_db)->query_one("SELECT 1 AS ok"), "{\"ok\":1}");

  release(local_db);
}

int main(void) {
  Test *t = create(Test, "SQLite Db Extension Unit Tests")->$$;

  test_sqlite_db(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
