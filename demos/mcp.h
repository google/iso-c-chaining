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

#ifndef CHAIN_MCP_H_ /* NOLINT(build/header_guard) */
#define CHAIN_MCP_H_

#include "core/hash.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
ch_use(Mcp, MCP, (char*, char*))
    /* clang-format on */

    /*
     * Callback signature for MCP tool handlers. Receives `Hash *args`
     * containing the `"arguments"` object from `tools/call` (or an empty `Hash`
     * if omitted). May return any `GenericEl*` (`String*`, `Hash*`, `Slice*`,
     * `Number*`, etc.) or a `StatusOr*` (if `!ok()`, returned as an MCP tool
     * error with
     * `"isError": true`).
     */
    typedef ChGenericEl* (*ChMcpToolHandler)(ChHash* args);

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChMcpToolHandler McpToolHandler;
#endif

/*
 * Model Context Protocol (MCP) JSON-RPC 2.0 server supporting declarative tool
 * registration, in-memory `dispatch()` and `call_tool()` testing, and line-
 * delimited `serve_stdio()` transport.
 */
struct MCP {
  ChGenericEl generic;
  ChString* name;
  ChString* version;
  ChSlice* tools;
  ChMcpToolHandler* handlers;
  int tool_count;
  int tool_cap;

  /* Binds this `Mcp` as the active instance on the `Mcp` `self` stack. */
  ChMcp* (*bind)(ChGenericEl*);

  /*
   * Registers an MCP tool with `name`, `description`, JSON Schema `Hash*`
   * `input_schema` (or `NULL` for an empty object schema), and `handler`.
   */
  ChMcp* (*tool)(char*, char*, ChHash*, ChMcpToolHandler);

  /*
   * Invokes a registered tool directly in-memory by `name` with `args` (`Hash*`
   * or `NULL`), returning its unwrapped output or error in a `StatusOr*`.
   */
  ChStatusOr* (*call_tool)(char*, ChHash*);

  /*
   * Dispatches a single JSON-RPC 2.0 request line in-memory (`initialize`,
   * `notifications/initialized`, `ping`, `tools/list`, `tools/call`), returning
   * the JSON-RPC response `Hash*` (or `Null*` for notifications) wrapped in a
   * `StatusOr*`.
   */
  ChStatusOr* (*dispatch)(char*);

  /*
   * Runs a blocking line-delimited JSON-RPC 2.0 loop reading requests from
   * `stdin` and writing responses to `stdout` until `EOF`.
   */
  void (*serve_stdio)(void);
};

#ifdef __cplusplus
}
#endif

#endif /* CHAIN_MCP_H_ */ /* NOLINT(build/header_guard) */
