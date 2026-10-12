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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/json.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/db.h"

int main(void) {
  /*
   * 1. Create an in-memory SQLite database, define schema, and insert rows
   *    using both inline Hash objects and parsed inbound JSON payloads.
   */
  Hash *inbound_user = as(
      Hash,
      from_json("{\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}")
          ->value())->$$;

  Db *db = create(Db, ":memory:")
      ->exec("CREATE TABLE users ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name TEXT NOT NULL,"
             "  role TEXT NOT NULL,"
             "  score REAL,"
             "  notes TEXT"
             ")")
      ->insert("users", create(Hash, 4)
          ->put("name", create(String, "Alice"))
          ->put("role", create(String, "admin"))
          ->put("score", create(Number, 98.25))
          ->put("notes", create(Null)))
      ->insert("users", create(Hash, 4)
          ->put("name", create(String, "Bob"))
          ->put("role", create(String, "viewer"))
          ->put("score", create(Number, 82))
          ->put("notes", create(String, "Read-only account")))
      ->insert("users", inbound_user)->$$;

  release(inbound_user);

  /*
   * 2. Query a single row using positional (?) Slice parameters.
   */
  Hash *alice = as(Hash, $(db)
      ->params(create(Slice, 1)->add(create(String, "Alice")))
      ->query_one("SELECT id, name, role, score, notes FROM users "
                  "WHERE name = ?")
      ->value())->$$;

  puts("Single row (Alice):");
  puts($(alice)->chars());
  release(alice);

  /*
   * 3. Bind named (:key) parameters directly from a parsed JSON request Hash
   *    and serialize the resulting Slice of Hash rows straight to JSON.
   */
  Hash *req = as(
      Hash,
      from_json("{\"role\":\"admin\",\"min_score\":90}")->value())->$$;

  Slice *admins = as(Slice, $(db)
      ->params(req)
      ->query("SELECT id, name, role, score FROM users "
              "WHERE role = :role AND score >= :min_score "
              "ORDER BY score DESC")
      ->value())->$$;

  puts("Filtered admins JSON response:");
  puts($(admins)->chars());

  release(admins);
  release(req);

  /*
   * 4. Error handling via StatusOr when a row is missing or SQL is invalid.
   */
  StatusOr *missing = $(db)
      ->params(create(Slice, 1)->add(create(String, "Nobody")))
      ->query_one("SELECT * FROM users WHERE name = ?")->$$;
  if (!$(missing)->ok()) {
    puts("Missing user status:");
    puts($(missing)->error());
  }
  release(missing);

  release(db);

  return 0;
}
