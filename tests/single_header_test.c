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

#ifndef CHAIN_NO_DB
#define CHAIN_ENABLE_DB
#endif
#define CHAIN_IMPLEMENTATION
#include "chain_single.h"

static GenericEl* handle_ping(Hash *req) {
  Hash *params = as(Hash, $(req)->get("params"));
  String *name = as(String, $(params)->get("name"));
  return (GenericEl*)create(Hash, 4)
      ->put("pong", create(String, $(name)->chars()))
      ->put("ok", create(Boolean, 1));
}

int main(void) {
  Test *t = create(Test, "Single-Header Amalgamation Unit Tests")->$$;

  /* 1. Core primitives, containers, and JSON round-trip. */
  Slice *parts = create(String, "alpha,beta,gamma")->split(",")->$$;
  String *joined = $(parts)->sub_slice(0, 1)->join("+")->$$;

  Hash *doc = as(
      Hash,
      from_json("{\"nums\":[10,20],\"flag\":true,\"empty\":null}")->value()
  )->$$;

  $(t)->section("Core types & JSON via single header")
      ->expect_num(create(Number, $(parts)->length), 3)
      ->expect_str(joined, "alpha+beta")
      ->expect_num(as(Slice, $(doc)->get("nums"))->get(1), 20)
      ->expect_bool($(doc)->get("flag"), 1)
      ->expect_str($(doc)->get("empty"), "null");

#ifdef CHAIN_HAS_DB_
  /* 2. SQLite Db extension via CHAIN_ENABLE_DB in single header. */
  Db *db = create(Db, ":memory:")
      ->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, label TEXT);")
      ->insert("items",
               create(Hash, 2)->put("label", create(String, "bundled")))
      ->$$;

  $(t)->section("SQLite Db extension via single header")
      ->expect_str(
          $(db)->query_one("SELECT label FROM items WHERE id = 1;"),
          "{\"label\":\"bundled\"}");

  release(db);
#endif

  /* 3. HTTP Server extension via single header. */
  Server *srv = create(Server, 0)
      ->get("/ping/:name", handle_ping)
      ->$$;

  $(t)->section("HTTP Server router via single header")
      ->expect_str(
          $(srv)->dispatch("GET", "/ping/single_hdr", NULL, NULL),
          "{\"pong\":\"single_hdr\",\"ok\":true}");

  release(srv);
  release(doc);
  release(joined);
  release(parts);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
