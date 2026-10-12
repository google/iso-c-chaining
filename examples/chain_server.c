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
 * Example SQLite-backed JSON REST API server using fluent C chaining.
 *
 * 1. Build and start the interactive HTTP server (defaults to 127.0.0.1:8080,
 *    or pass a custom port number):
 *
 *      cmake -B build && cmake --build build
 *      ./build/chain_server
 *      ./build/chain_server 9000
 *
 * 2. Interact with the running server from another terminal using curl:
 *
 *      # List all users
 *      curl http://127.0.0.1:8080/users
 *
 *      # Filter users by query parameters
 *      curl "http://127.0.0.1:8080/users?role=admin&min_score=90"
 *
 *      # Fetch a single user by ID
 *      curl http://127.0.0.1:8080/users/1
 *
 *      # Create a new user (POST JSON body)
 *      curl -X POST http://127.0.0.1:8080/users \
 *        -d '{"name":"Charlie","role":"admin","score":95.5}'
 *
 *      # Update an existing user (PUT JSON body)
 *      curl -X PUT http://127.0.0.1:8080/users/1 \
 *        -d '{"score":99.75}'
 *
 *      # Delete a user by ID
 *      curl -X DELETE http://127.0.0.1:8080/users/2
 *
 *    Press Ctrl-C in the server terminal to stop the server cleanly.
 *
 * 3. Run the non-interactive loopback self-test suite (e.g. under ASan/MSan):
 *
 *      ./build/chain_server --test
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "ext/test.h"

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

static int run_self_test(Server *server) {
  return create(Test, "Server Loopback Self-Test")
      ->section("POST /users")
      ->expect_str(
          $(server)->request(
              "POST", "/users",
              "{\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}"),
          "{\"id\":3,\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}")
      ->section("GET /users?role=admin&min_score=90")
      ->expect_str(
          $(server)->request("GET", "/users?role=admin&min_score=90", NULL),
          "[{\"id\":1,\"name\":\"Alice\",\"role\":\"admin\",\"score\":98.25},"
          "{\"id\":3,\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}]")
      ->section("PUT /users/1")
      ->expect_str(
          $(server)->request("PUT", "/users/1", "{\"score\":99.75}"),
          "{\"id\":1,\"name\":\"Alice\",\"role\":\"admin\",\"score\":99.75}")
      ->section("DELETE /users/2")
      ->expect_str(
          $(server)->request("DELETE", "/users/2", NULL),
          "{\"deleted\":true,\"id\":2}")
      ->section("GET /users/999 (404 Not Found)")
      ->expect_err(
          $(server)->request("GET", "/users/999", NULL),
          "No rows returned")
      ->summary();
}

int main(int argc, char **argv) {
  int test_mode = (argc > 1 && strcmp(argv[1], "--test") == 0);
  int port = 8080;
  if (test_mode) {
    port = 0;
  } else if (argc > 1) {
    port = atoi(argv[1]);
  }

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

  Server *server = create(Server, port)
      ->get("/users", list_users)
      ->get("/users/:id", get_user)
      ->post("/users", create_user)
      ->put("/users/:id", update_user)
      ->delete_route("/users/:id", delete_user)->$$;

  int rc = 0;
  if (test_mode) {
    $(server)->start();
    rc = run_self_test(server);
    $(server)->stop();
  } else {
    printf("Try with curl in another terminal:\n"
           "  curl http://127.0.0.1:%d/users\n"
           "  curl \"http://127.0.0.1:%d/users?role=admin&min_score=90\"\n"
           "  curl http://127.0.0.1:%d/users/1\n"
           "  curl -X POST http://127.0.0.1:%d/users "
           "-d '{\"name\":\"Charlie\",\"role\":\"admin\",\"score\":95.5}'\n"
           "  curl -X PUT http://127.0.0.1:%d/users/1 "
           "-d '{\"score\":99.75}'\n"
           "  curl -X DELETE http://127.0.0.1:%d/users/2\n\n",
           port, port, port, port, port, port);
    $(server)->listen();
  }

  release(server);
  release(db);

  return rc;
}
