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

typedef struct {
  int value;
} SingleBoxPair;

use_box(SingleBoxPair)

static GenericEl* handle_ping(Hash *req) {
  Hash *params = as(Hash, $(req)->get("params"));
  String *name = as(String, $(params)->get("name"));
  return (GenericEl*)create(Hash, 4)
      ->put("pong", create(String, $(name)->chars()))
      ->put("ok", create(Boolean, 1));
}

static GenericEl* handle_mcp_echo(Hash *args) {
  return (GenericEl*)create(Hash, 1)
      ->put("echo", $(args)->get("msg"));
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

  SingleBoxPair *raw_pair = malloc(sizeof(SingleBoxPair));
  raw_pair->value = 123;
  SingleBoxPairBox *pbox = create(SingleBoxPairBox, raw_pair, free)->$$;
  $(pbox)->get()->value += 7;

  $(t)->section("Core types & JSON via single header")
      ->expect_num(create(Number, $(pbox)->get()->value), 130)
      ->expect_num(create(Number, box_as(SingleBoxPair, pbox)->value), 130)
      ->expect_num(create(Number, $(parts)->length), 3)
      ->expect_str(joined, "alpha+beta")
      ->expect_num(as(Slice, $(doc)->get("nums"))->get(1), 20)
      ->expect_bool($(doc)->get("flag"), 1)
      ->expect_str($(doc)->get("empty"), "null")
      ->expect_str(
          create(TypedIntArray, 0)->push_n(4, 40, 10, 30, 20)->sort()->reverse(),
          "[40,30,20,10]")
      ->expect_num(
          create(TypedIntArray, 0)->push_n(2, 11, 22)->to_slice()->get(1),
          22)
      ->expect_str(
          create(TypedCharArray, 0)->push_n(3, 'a', 'b', 'c')->join("-"),
          "a-b-c")
      ->expect_str(
          create(TypedFloatArray, 0)->push_n(2, 1.5, 2.25),
          "[1.5,2.25]")
      ->expect_str(
          create(TypedF32Array, 0)->push_n(2, 1.5f, 0.25f),
          "[1.5,0.25]");

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

  /* 4. MCP Server extension via single header. */
  Mcp *mcp = create(Mcp, "single-mcp", "1.0.0")
      ->tool("echo", "Echo a message", NULL, handle_mcp_echo)
      ->$$;

  $(t)->section("MCP Server extension via single header")
      ->expect_str(
          $(mcp)->call_tool("echo", create(Hash, 1)
              ->put("msg", create(String, "hi"))),
          "{\"echo\":\"hi\"}");

#ifdef CHAIN_HAS_GLFW_
  /* 5. GLFW3 Glfw + OpenGL 3.3 Gl extensions via CHAIN_ENABLE_GLFW in single header. */
  Glfw *win = create(Glfw, 40, 30, "single_hdr_glfw")
      ->gl_version(3, 3, GLFW_OPENGL_CORE_PROFILE)
      ->visible(0)
      ->vsync(0)
      ->clear_color(0.0f, 1.0f, 0.0f, 1.0f)
      ->step()
      ->$$;

  $(t)->section("GLFW3 Glfw & Gl extensions via single header")
      ->expect_ok($(win)->status())
      ->expect_str($(win)->read_pixel(20, 15), "[0,255,0,255]");

#ifdef CHAIN_HAS_GL_
  Gl *gl = create(Gl, (GlLoader)glfwGetProcAddress)->$$;
  $(t)->expect_ok($(gl)->status());
  release(gl);
#endif

  release(win);
#endif

  release(pbox);
  release(mcp);
  release(srv);
  release(doc);
  release(joined);
  release(parts);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
