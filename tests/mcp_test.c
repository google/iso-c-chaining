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

#include "ext/mcp.h"

#include <stddef.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/db.h"
#include "ext/test.h"

static Db *test_db;

static GenericEl* handle_ping_tool(Hash *args) {
  (void)args;
  return (GenericEl*)create(String, "pong");
}

static GenericEl* handle_get_user_tool(Hash *args) {
  return (GenericEl*)$(test_db)
      ->params(args)
      ->query_one("SELECT id, name, role, score FROM users WHERE id = :id");
}

static GenericEl* handle_add_numbers_tool(Hash *args) {
  double a = as(Number, $(args)->get("a"))->value();
  double b = as(Number, $(args)->get("b"))->value();
  return (GenericEl*)create(Hash, 1)
      ->put("sum", create(Number, a + b));
}

static void test_mcp_server(Test *t) {
  Mcp *mcp = create(Mcp, "test-mcp", "0.1.0")
      ->tool("ping_tool", "Simple ping tool with default schema", NULL,
             handle_ping_tool)
      ->tool(
          "get_user",
          "Fetch user by ID",
          create(Hash, 3)
              ->put("type", create(String, "object"))
              ->put("properties", create(Hash, 1)
                  ->put("id", create(Hash, 1)
                      ->put("type", create(String, "integer"))))
              ->put("required", create(Slice, 1)
                  ->add(create(String, "id"))),
          handle_get_user_tool)
      ->tool(
          "add_numbers",
          "Add two numbers",
          create(Hash, 2)
              ->put("type", create(String, "object"))
              ->put("properties", create(Hash, 2)
                  ->put("a", create(Hash, 1)
                      ->put("type", create(String, "number")))
                  ->put("b", create(Hash, 1)
                      ->put("type", create(String, "number")))),
          handle_add_numbers_tool)->$$;

  $(t)->section("MCP initialize, notifications, ping, tools/list & tools/call")
      ->expect_str(mcp, "<Mcp>")
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}"),
          "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
          "\"protocolVersion\":\"2024-11-05\","
          "\"capabilities\":{\"tools\":{}},"
          "\"serverInfo\":{\"name\":\"test-mcp\",\"version\":\"0.1.0\"}}}")
      /* Notifications return Null (no response frame emitted over stdio). */
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"),
          "null")
      ->expect_str(
          $(mcp)->dispatch("{\"jsonrpc\":\"2.0\",\"id\":\"p1\",\"method\":\"ping\"}"),
          "{\"jsonrpc\":\"2.0\",\"id\":\"p1\",\"result\":{}}")
      ->expect_str(
          $(mcp)->dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"),
          "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
          "{\"name\":\"ping_tool\","
          "\"description\":\"Simple ping tool with default schema\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"get_user\",\"description\":\"Fetch user by ID\","
          "\"inputSchema\":{\"type\":\"object\","
          "\"properties\":{\"id\":{\"type\":\"integer\"}},"
          "\"required\":[\"id\"]}},"
          "{\"name\":\"add_numbers\",\"description\":\"Add two numbers\","
          "\"inputSchema\":{\"type\":\"object\","
          "\"properties\":{\"a\":{\"type\":\"number\"},"
          "\"b\":{\"type\":\"number\"}}}}]}}")
      /* tools/call: String return, Hash return, and StatusOr error return */
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"ping_tool\"}}"),
          "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{"
          "\"content\":[{\"type\":\"text\",\"text\":\"pong\"}],"
          "\"isError\":false}}")
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"get_user\",\"arguments\":{\"id\":1}}}"),
          "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{"
          "\"content\":[{\"type\":\"text\","
          "\"text\":\"{\\\"id\\\":1,\\\"name\\\":\\\"Alice\\\","
          "\\\"role\\\":\\\"admin\\\",\\\"score\\\":98.25}\"}],"
          "\"isError\":false}}")
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"get_user\",\"arguments\":{\"id\":999}}}"),
          "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{"
          "\"content\":[{\"type\":\"text\",\"text\":\"No rows returned\"}],"
          "\"isError\":true}}")
      /* Direct call_tool() helper */
      ->expect_str(
          $(mcp)->call_tool("add_numbers", create(Hash, 2)
              ->put("a", create(Number, 12.5))
              ->put("b", create(Number, 7.5))),
          "{\"sum\":20}")
      ->expect_err(
          $(mcp)->call_tool("missing_tool", NULL),
          "Unknown tool: missing_tool")
      /* JSON-RPC error codes: unknown tool, unknown method, invalid JSON */
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"no_such_tool\"}}"),
          "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{"
          "\"code\":-32602,\"message\":\"Unknown tool: no_such_tool\"}}")
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"unknown/method\"}"),
          "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{"
          "\"code\":-32601,\"message\":\"Method not found: unknown/method\"}}")
      ->expect_str(
          $(mcp)->dispatch("{bad json"),
          "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{"
          "\"code\":-32700,\"message\":\"Parse error\"}}");

  release(mcp);
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

  Test *t = create(Test, "MCP Server Extension Unit Tests")->$$;

  test_mcp_server(t);

  int rc = $(t)->summary();
  release(t);
  release(test_db);
  return rc;
}
