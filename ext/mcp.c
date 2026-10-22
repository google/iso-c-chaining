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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/json.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

#define MCP_PROTOCOL_VERSION "2024-11-05"

/**
 * Object handling functions.
 */

static Mcp* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Mcp*)update_self_stack(&top, s, MCP);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Mcp* bind(GenericEl* mcp) {
  return self(mcp);
}

static void del(void* obj) {
  Mcp* mcp = (Mcp*)obj;
  if (mcp != NULL && self((GenericEl*)mcp) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      release(slf->name);
      release(slf->version);
      release(slf->tools);
      free(slf->handlers);
      Mcp *to_free = slf;
      self((GenericEl*)-1);
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * Internal helpers.
 */

/*
 * Searches `mcp->tools` for a registered tool whose `"name"` matches `name`.
 * Returns the 0-based index into `mcp->tools` / `mcp->handlers`, or `-1` if no
 * matching tool is registered. Pops any temporary `Hash` and `String` stack
 * bindings created during the lookup so caller chains are undisturbed.
 */
static int find_tool_index(Mcp *mcp, const char *name) {
  if (name == NULL) {
    return -1;
  }
  int i;
  for (i = 0; i < mcp->tool_count; i++) {
    Hash *tdef = (Hash*)mcp->tools->elements[i];
    GenericEl *tname = $(tdef)->get("name");
    tdef->bind((GenericEl*)-1);
    if (is_type(tname, STRING)) {
      tname->bind((GenericEl*)-1);
      if (strcmp(((String*)tname)->chars, name) == 0) {
        return i;
      }
    }
  }
  return -1;
}

/*
 * Wraps a successful method payload `result_el` in a JSON-RPC 2.0 response
 * envelope (`{"jsonrpc": "2.0", "id": id_el, "result": result_el}`).
 */
static Hash* make_jsonrpc_result(GenericEl *id_el, GenericEl *result_el) {
  GenericEl *id_val = id_el != NULL ? id_el : (GenericEl*)create(Null);
  return create(Hash, 3)
      ->put("jsonrpc", create(String, "2.0"))
      ->put("id", id_val)
      ->put("result", result_el);
}

/*
 * Builds a JSON-RPC 2.0 error response envelope with numeric `code` (e.g.,
 * `-32700` Parse error, `-32600` Invalid Request, `-32601` Method not found,
 * `-32602` Invalid params) and human-readable `msg`.
 */
static Hash* make_jsonrpc_error(GenericEl *id_el, int code, const char *msg) {
  GenericEl *id_val = id_el != NULL ? id_el : (GenericEl*)create(Null);
  return create(Hash, 3)
      ->put("jsonrpc", create(String, "2.0"))
      ->put("id", id_val)
      ->put("error", create(Hash, 2)
          ->put("code", create(Number, code))
          ->put("message", create(String, (char*)(msg != NULL ? msg : ""))));
}

/*
 * Looks up and executes the tool handler for `name`, passing `args` (or a
 * temporary empty `Hash` if `args == NULL`) and normalizing the handler's
 * return value into a `StatusOr*`. Claims and releases `args` so inline
 * `create(Hash, ...)` arguments passed to `call_tool()` are freed
 * automatically while caller-saved `Hash` objects retain their reference count.
 */
static StatusOr* invoke_tool_internal(Mcp *mcp, const char *name, Hash *args) {
  int idx = find_tool_index(mcp, name);
  if (idx < 0) {
    String *err = create(String, "Unknown tool: ")
        ->append((char*)(name != NULL ? name : ""));
    StatusOr *so = create(StatusOr, NULL, err->chars);
    release(err);
    return $(so);
  }

  Hash *call_args = args;
  if (call_args == NULL) {
    call_args = create(Hash, 0)->$$;
    call_args->bind((GenericEl*)-1);
  } else {
    claim_element(call_args, NULL);
  }

  GenericEl *ret = mcp->handlers[idx](call_args);
  release(call_args);

  if (ret == NULL) {
    return create(StatusOr, create(Null), NULL);
  }
  if (is_type(ret, STATUS_OR)) {
    return $((StatusOr*)ret);
  }
  return create(StatusOr, ret, NULL);
}

/**
 * Mcp operational functions.
 */

/*
 * Registers a tool descriptor (`{"name", "description", "inputSchema"}`) in
 * `mcp->tools` (reused directly by `tools/list`) and stores `handler` at the
 * matching index in `mcp->handlers`. If `input_schema` is `NULL`, substitutes a
 * default empty JSON Schema object (`{"type": "object", "properties": {}}`).
 */
static Mcp* tool(char *name, char *description, Hash *input_schema,
                 ChMcpToolHandler handler) {
  Mcp *mcp = slf;
  Hash *schema = input_schema;
  if (schema == NULL) {
    schema = create(Hash, 2)
        ->put("type", create(String, "object"))
        ->put("properties", create(Hash, 0));
  }

  Hash *tool_def = create(Hash, 3)
      ->put("name", create(String, name != NULL ? name : ""))
      ->put("description",
            create(String, description != NULL ? description : ""))
      ->put("inputSchema", schema);

  $(mcp->tools)->add(tool_def);
  mcp->tools->bind((GenericEl*)-1);

  if (mcp->tool_count >= mcp->tool_cap) {
    mcp->tool_cap = mcp->tool_cap > 0 ? mcp->tool_cap * 2 : 4;
    mcp->handlers = realloc(
        mcp->handlers, sizeof(ChMcpToolHandler) * (size_t)mcp->tool_cap);
  }
  mcp->handlers[mcp->tool_count++] = handler;
  return $(mcp);
}

/*
 * Invokes a registered tool by `name` in memory without constructing a
 * JSON-RPC request envelope, returning the handler's result wrapped in a
 * `StatusOr*`.
 */
static StatusOr* call_tool(char *name, Hash *args) {
  Mcp *mcp = slf;
  StatusOr *res = invoke_tool_internal(mcp, name, args);
  $(mcp);
  return $(res);
}

/*
 * Parses and dispatches a single JSON-RPC 2.0 request line according to the
 * Model Context Protocol specification, returning a `StatusOr*` that wraps the
 * response `Hash*` (or `Null*` for notifications that produce no response).
 */
static StatusOr* dispatch(char *line) {
  Mcp *mcp = slf;

  /*
   * Step 1: Reject empty lines and parse the incoming JSON-RPC 2.0 message.
   * Per JSON-RPC 2.0, malformed JSON or a non-object root returns `-32700`
   * ("Parse error") with `"id": null`.
   */
  if (line == NULL || line[0] == '\0') {
    Hash *err_resp = make_jsonrpc_error(NULL, -32700, "Empty request");
    $(mcp);
    return create(StatusOr, err_resp, NULL);
  }

  StatusOr *parsed_so = from_json(line)->$$;
  if (!$(parsed_so)->ok() || !is_type(parsed_so->val, HASH)) {
    Hash *err_resp = make_jsonrpc_error(NULL, -32700, "Parse error");
    release(parsed_so);
    $(mcp);
    return create(StatusOr, err_resp, NULL);
  }

  /*
   * Step 2: Extract `"id"`, `"method"`, and `"params"` from the parsed request
   * `Hash`, immediately popping each looked-up element off its `self` stack so
   * inspecting the request does not disturb surrounding fluent chains.
   */
  Hash *req = (Hash*)parsed_so->val;
  GenericEl *id_el = $(req)->get("id");
  if (id_el != NULL && id_el->bind(NULL) == id_el) {
    id_el->bind((GenericEl*)-1);
  }
  GenericEl *method_el = $(req)->get("method");
  if (method_el != NULL && method_el->bind(NULL) == method_el) {
    method_el->bind((GenericEl*)-1);
  }
  GenericEl *params_el = $(req)->get("params");
  if (params_el != NULL && params_el->bind(NULL) == params_el) {
    params_el->bind((GenericEl*)-1);
  }
  if (req->bind(NULL) == req) {
    req->bind((GenericEl*)-1);
  }

  /*
   * Step 3: Verify `"method"` is a string (`-32600` "Invalid Request" if
   * missing or non-string) and short-circuit JSON-RPC 2.0 notifications (such
   * as `"notifications/initialized"`), which omit `"id"` and must not emit a
   * response frame.
   */
  if (!is_type(method_el, STRING)) {
    Hash *err_resp = make_jsonrpc_error(id_el, -32600, "Invalid Request");
    release(parsed_so);
    $(mcp);
    return create(StatusOr, err_resp, NULL);
  }

  const char *method = ((String*)method_el)->chars;

  if (id_el == NULL || strncmp(method, "notifications/", 14) == 0) {
    release(parsed_so);
    $(mcp);
    return create(StatusOr, create(Null), NULL);
  }

  /*
   * Step 4: Route standard MCP methods:
   * - `initialize`: return protocol version, tool capability, and `serverInfo`.
   * - `ping`: return an empty result object `{}`.
   * - `tools/list`: return the registered `mcp->tools` slice.
   * - `tools/call`: extract `params.name` and `params.arguments`, invoke the
   *   registered handler, and wrap its output into an MCP `content` block.
   */
  Hash *resp = NULL;
  if (strcmp(method, "initialize") == 0) {
    Hash *result = create(Hash, 3)
        ->put("protocolVersion", create(String, MCP_PROTOCOL_VERSION))
        ->put("capabilities", create(Hash, 1)
            ->put("tools", create(Hash, 0)))
        ->put("serverInfo", create(Hash, 2)
            ->put("name", mcp->name)
            ->put("version", mcp->version));
    resp = make_jsonrpc_result(id_el, (GenericEl*)result);
  } else if (strcmp(method, "ping") == 0) {
    resp = make_jsonrpc_result(id_el, (GenericEl*)create(Hash, 0));
  } else if (strcmp(method, "tools/list") == 0) {
    Hash *result = create(Hash, 1)
        ->put("tools", mcp->tools);
    resp = make_jsonrpc_result(id_el, (GenericEl*)result);
  } else if (strcmp(method, "tools/call") == 0) {
    /* Extract `"name"` and optional `"arguments"` from `params`. */
    Hash *params = is_type(params_el, HASH) ? (Hash*)params_el : NULL;
    GenericEl *tname_el = params != NULL ? $(params)->get("name") : NULL;
    if (tname_el != NULL && tname_el->bind(NULL) == tname_el) {
      tname_el->bind((GenericEl*)-1);
    }
    GenericEl *targs_el = params != NULL ? $(params)->get("arguments") : NULL;
    if (targs_el != NULL && targs_el->bind(NULL) == targs_el) {
      targs_el->bind((GenericEl*)-1);
    }
    if (params != NULL && params->bind(NULL) == params) {
      params->bind((GenericEl*)-1);
    }

    const char *tname = is_type(tname_el, STRING) ?
        ((String*)tname_el)->chars : NULL;
    if (tname == NULL || find_tool_index(mcp, tname) < 0) {
      /* Unknown tool name maps to JSON-RPC `-32602` (Invalid params). */
      String *msg = create(String, "Unknown tool: ")
          ->append((char*)(tname != NULL ? tname : ""));
      resp = make_jsonrpc_error(id_el, -32602, msg->chars);
      release(msg);
    } else {
      /*
       * Execute the matched tool handler and format its result (or `StatusOr`
       * error message) into an MCP text content item:
       * `{"content": [{"type": "text", "text": "..."}], "isError": bool}`.
       */
      Hash *targs = is_type(targs_el, HASH) ? (Hash*)targs_el : NULL;
      StatusOr *tool_so = invoke_tool_internal(mcp, tname, targs)->$$;
      int is_err = !$(tool_so)->ok();
      char *text_out = "";
      if (is_err) {
        text_out = $(tool_so)->error();
      } else if (tool_so->val != NULL) {
        text_out = ((GenericEl*)ch_bind_el(tool_so->val))->to_chars();
        if (tool_so->val->bind(NULL) == tool_so->val) {
          tool_so->val->bind((GenericEl*)-1);
        }
      }
      Hash *result = create(Hash, 2)
          ->put("content", create(Slice, 1)
              ->add(create(Hash, 2)
                  ->put("type", create(String, "text"))
                  ->put("text", create(String, text_out))))
          ->put("isError", create(Boolean, is_err ? true : false));
      release(tool_so);
      resp = make_jsonrpc_result(id_el, (GenericEl*)result);
    }
  } else {
    /* Unrecognized method name maps to JSON-RPC `-32601` (Method not found). */
    String *msg = create(String, "Method not found: ")->append((char*)method);
    resp = make_jsonrpc_error(id_el, -32601, msg->chars);
    release(msg);
  }

  /*
   * Step 5: Release the parsed request tree (any `id_el` reused in `resp` has
   * already had its reference count incremented by `Hash::put`) and return the
   * response wrapped in a `StatusOr*`.
   */
  release(parsed_so);
  $(mcp);
  return create(StatusOr, resp, NULL);
}

/*
 * Reads newline-delimited JSON-RPC 2.0 messages from `stdin`, dispatches each
 * line through `dispatch()`, and writes non-notification JSON responses to
 * `stdout` (flushing after every line) until `EOF`. Automatically releases
 * `mcp` if it was chained inline without `->$$` (`refs == 0`).
 */
static void serve_stdio(void) {
  Mcp *mcp = slf;
  char buf[65536];
  while (fgets(buf, sizeof(buf), stdin) != NULL) {
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    StatusOr *res = $(mcp)->dispatch(buf)->$$;
    if ($(res)->ok() && res->val != NULL && !is_type(res->val, NULL_VAL)) {
      char *json = ((GenericEl*)ch_bind_el(res->val))->to_chars();
      if (res->val->bind(NULL) == res->val) {
        res->val->bind((GenericEl*)-1);
      }
      puts(json);
      fflush(stdout);
    }
    release(res);
  }
  if (mcp->generic.refs == 0) {
    release(mcp);
  }
}

static char* to_chars(void) {
  return "<Mcp>";
}

/**
 * Constructor.
 */
Mcp* ch_new_Mcp(char *name, char *version) {
  Mcp *mcp = malloc(sizeof(Mcp));
  mcp->generic.magic = CH_MAGIC;
  mcp->generic.type_val = MCP;
  mcp->generic.prev_self = NULL;
  mcp->generic.next_self = NULL;
  mcp->generic.refs = 0;

  mcp->name = create(String, name != NULL ? name : "chain-mcp")->$$;
  mcp->name->bind((GenericEl*)-1);
  mcp->version = create(String, version != NULL ? version : "1.0.0")->$$;
  mcp->version->bind((GenericEl*)-1);
  mcp->tools = create(Slice, 4)->$$;
  mcp->tools->bind((GenericEl*)-1);
  mcp->tool_count = 0;
  mcp->tool_cap = 4;
  mcp->handlers = malloc(sizeof(ChMcpToolHandler) * (size_t)mcp->tool_cap);

  mcp->bind = bind;
  mcp->generic.bind = (void* (*)(GenericEl*))bind;
  mcp->generic.save = save;
  mcp->generic.del = del;
  mcp->generic.to_chars = to_chars;
  mcp->tool = tool;
  mcp->call_tool = call_tool;
  mcp->dispatch = dispatch;
  mcp->serve_stdio = serve_stdio;

  self((GenericEl*)mcp);
  return mcp;
}
