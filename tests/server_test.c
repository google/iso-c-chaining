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

#include "ext/server.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/db.h"
#include "ext/test.h"

static Db *test_db;

static GenericEl* handle_get_user(Hash *req) {
  return (GenericEl*)$(test_db)
      ->params($(req)->get("params"))
      ->query_one("SELECT id, name, role, score FROM users WHERE id = :id");
}

static GenericEl* handle_list_users(Hash *req) {
  Hash *query = as(Hash, $(req)->get("query"));
  if ($(query)->get("role") != NULL) {
    return (GenericEl*)$(test_db)
        ->params(query)
        ->query("SELECT id, name, role, score FROM users "
                "WHERE role = :role ORDER BY id ASC");
  }
  return (GenericEl*)$(test_db)
      ->query("SELECT id, name, role, score FROM users ORDER BY id ASC");
}

static GenericEl* handle_create_user(Hash *req) {
  Hash *body = as(Hash, $(req)->get("body"));
  return (GenericEl*)$(test_db)
      ->insert("users", body)
      ->params(body)
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE name = :name ORDER BY id DESC");
}

static GenericEl* handle_update_user(Hash *req) {
  Hash *params = as(Hash, $(req)->get("params"));
  Hash *body = as(Hash, $(req)->get("body"));
  $(body)->put("id", $(params)->get("id"));
  return (GenericEl*)$(test_db)
      ->params(body)
      ->exec("UPDATE users SET role = :role, score = :score WHERE id = :id")
      ->params(params)
      ->query_one("SELECT id, name, role, score FROM users WHERE id = :id");
}

static GenericEl* handle_delete_user(Hash *req) {
  Hash *params = as(Hash, $(req)->get("params"));
  $(test_db)
      ->params(params)
      ->exec("DELETE FROM users WHERE id = :id");
  return (GenericEl*)create(Hash, 2)
      ->put("deleted", $(params)->get("id"));
}

static GenericEl* handle_echo_request(Hash *req) {
  return (GenericEl*)create(Hash, 5)
      ->put("method", $(req)->get("method"))
      ->put("path", $(req)->get("path"))
      ->put("params", $(req)->get("params"))
      ->put("query", $(req)->get("query"))
      ->put("body", $(req)->get("body"));
}

static GenericEl* handle_null_response(Hash *req) {
  (void)req;
  return NULL;
}

static GenericEl* handle_custom_error(Hash *req) {
  (void)req;
  return (GenericEl*)create(StatusOr, NULL, "Custom handler rejection");
}

static void test_http_server(Test *t) {
  Server *stopped_srv = create(Server, 0)->$$;

  /*
   * Register routes including a prefix-overlapping pattern
   * (`/items/:leaked_id/other`) before `/items/:id/summary` to verify that a
   * partial segment mismatch does not leak `:leaked_id` into `params`.
   */
  Server *server = create(Server, 0)
      ->get("/users", handle_list_users)
      ->get("/users/:id", handle_get_user)
      ->post("/users", handle_create_user)
      ->put("/users/:id", handle_update_user)
      ->delete_route("/users/:id", handle_delete_user)
      ->get("/items/:leaked_id/other", handle_echo_request)
      ->get("/items/:id/summary", handle_echo_request)
      ->post("/echo/:tag", handle_echo_request)
      ->get("/null", handle_null_response)
      ->get("/fail", handle_custom_error)
      ->start()->$$;

  $(t)->section("Server in-memory dispatch, routing, URL decoding & HTTP TCP")
      ->expect_str(server, "<Server>")
      ->expect_err(
          $(stopped_srv)->request("GET", "/users", NULL),
          "Server is not running")
      /* Route disambiguation, URL decoding (%XX and '+'), and type coercion */
      ->expect_str(
          $(server)->dispatch(
              "GET",
              "/items/42/summary",
              "verbose&name=Alice+Smith%21&active=true&disabled=false&score=9.5",
              NULL),
          "{\"method\":\"GET\",\"path\":\"/items/42/summary\","
          "\"params\":{\"id\":42},"
          "\"query\":{\"verbose\":true,\"name\":\"Alice Smith!\","
          "\"active\":true,\"disabled\":false,\"score\":9.5},"
          "\"body\":null}")
      ->expect_str(
          $(server)->dispatch(
              "POST",
              "/echo/hello%20world%2Ftest",
              NULL,
              "{\"nested\":[1,true]}"),
          "{\"method\":\"POST\",\"path\":\"/echo/hello%20world%2Ftest\","
          "\"params\":{\"tag\":\"hello world/test\"},"
          "\"query\":{},\"body\":{\"nested\":[1,true]}}")
      /* NULL handler return, custom error StatusOr, 404, and invalid JSON */
      ->expect_str($(server)->dispatch("GET", "/null", NULL, NULL), "null")
      ->expect_err(
          $(server)->dispatch("GET", "/fail", NULL, NULL),
          "Custom handler rejection")
      ->expect_err(
          $(server)->dispatch("GET", "/nonexistent", NULL, NULL),
          "Not found")
      ->expect_err(
          $(server)->dispatch("DELETE", "/null", NULL, NULL),
          "Not found")
      ->expect_err(
          $(server)->dispatch("POST", "/echo/tag", NULL, "{bad json"),
          "Expected string key in object")
      /* Full loopback HTTP/1.1 TCP requests (GET, POST, PUT, DELETE, 400, 404) */
      ->expect_str(
          $(server)->request("GET", "/users/1", NULL),
          "{\"id\":1,\"name\":\"Alice\",\"role\":\"admin\",\"score\":98.25}")
      ->expect_str(
          $(server)->request(
              "POST", "/users",
              "{\"name\":\"Charlie\",\"role\":\"viewer\",\"score\":88}"),
          "{\"id\":2,\"name\":\"Charlie\",\"role\":\"viewer\",\"score\":88}")
      ->expect_str(
          $(server)->request("GET", "/users?role=viewer", NULL),
          "[{\"id\":2,\"name\":\"Charlie\",\"role\":\"viewer\",\"score\":88}]")
      ->expect_str(
          $(server)->request(
              "PUT", "/users/2",
              "{\"role\":\"editor\",\"score\":91.5}"),
          "{\"id\":2,\"name\":\"Charlie\",\"role\":\"editor\",\"score\":91.5}")
      ->expect_str(
          $(server)->request("DELETE", "/users/2", NULL),
          "{\"deleted\":2}")
      ->expect_err(
          $(server)->request("GET", "/users/2", NULL),
          "No rows returned")
      ->expect_err(
          $(server)->request("GET", "/users/999", NULL),
          "No rows returned")
      ->expect_err(
          $(server)->request("GET", "/unknown_route", NULL),
          "Not found")
      ->expect_err(
          $(server)->request("POST", "/users", "{malformed"),
          "Expected string key in object")
      ->expect_err(
          $(server)->request("GET", "/fail", NULL),
          "Custom handler rejection");

  $(server)->stop();
  release(server);
  release(stopped_srv);
}

int main(void) {
  test_db = create(Db, ":memory:")
      ->exec("CREATE TABLE users ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name TEXT NOT NULL,"
             "  role TEXT NOT NULL,"
             "  score REAL"
             ")")
      ->insert("users", create(Hash, 3)
          ->put("name", create(String, "Alice"))
          ->put("role", create(String, "admin"))
          ->put("score", create(Number, 98.25)))->$$;

  Test *t = create(Test, "HTTP Server Extension Unit Tests")->$$;

  test_http_server(t);

  int rc = $(t)->summary();
  release(t);
  release(test_db);
  return rc;
}
