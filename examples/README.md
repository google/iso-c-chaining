<!-- disableFinding(LINK_RELATIVE_G3DOC) -->

# ISO C Chaining Examples

This directory contains runnable examples demonstrating core data structures,
macro-free chaining, contiguous `TypedArray`s, SQLite persistence (`Db`), vector
similarity search over Linux `whatis` man pages, a POSIX HTTP/1.1 REST server
(`Server`), and a Model Context Protocol (`Mcp`) stdio server.

## Quick Start

Build all examples from the repository root with CMake:

```bash
cmake -B build && cmake --build build
```

Every example also supports a `--test` flag (used by `ctest`) to run built-in
assertions via `Test` (`ext/test.h`):

-   [`chain.c`](chain.c) (`./build/chain`): String splitting, `Slice`
    concatenation, `sub_slice()`, and custom element types.
-   [`chain_nomacro.c`](chain_nomacro.c) (`./build/chain_nomacro`): The same
    fluent chains written in pure ANSI C89 without the `$` / `$$` / `create`
    convenience macros. Defines `CHAIN_NO_SHORT_NAMES` and compiles cleanly
    under `gcc -std=c89 -pedantic -Werror`.
-   [`chain_hash.c`](chain_hash.c) (`./build/chain_hash`): Key-value `Hash`
    maps, lookups, and iteration.
-   [`chain_nest.c`](chain_nest.c) (`./build/chain_nest`): Arbitrarily nested
    `Hash` and `Slice` trees, JSON serialization, and `from_json()` with
    `StatusOr`.
-   [`chain_typed_array.c`](chain_typed_array.c) (`./build/chain_typed_array`):
    Unboxed `TypedIntArray`, `TypedFloatArray`, and `TypedCharArray` (`map`,
    `filter`, `reduce`, `sort`, `subarray`).
-   [`chain_db.c`](chain_db.c) (`./build/chain_db`): Fluent SQLite `Db` queries,
    `Hash`-based `insert()`, and positional/named `params()`.
-   [`chain_vector_db.c`](chain_vector_db.c) (`./build/chain_vector_db`):
    Lightweight SQLite vector database indexing `/usr/bin/whatis` man pages with
    `TypedFloatArray` embeddings.
-   [`chain_server.c`](chain_server.c) (`./build/chain_server`): Zero-dependency
    HTTP/1.1 REST server (`Server`) backed by SQLite (`Db`).
-   [`chain_mcp.c`](chain_mcp.c) (`./build/chain_mcp`): Model Context Protocol
    (`Mcp`) JSON-RPC 2.0 server wrapping `/bin/uname`, `/bin/df`, and `/bin/ps`.

--------------------------------------------------------------------------------

## 1. Nested Containers and JSON Parsing (`chain_nest.c`)

[`chain_nest.c`](chain_nest.c) builds a nested tree of `Hash`, `Slice`,
`String`, `Number`, `Boolean`, and `Null` elements, serializes it to JSON via
`->chars()`, and parses JSON back into a `Hash` via `from_json()` (returning a
`StatusOr*`):

```c
Hash *hash = create(Hash, 10)
    ->put("Apple", create(String, "Foo"))
    ->put("Banana", create(Number, 42))
    ->put("Organic", create(Boolean, true))
    ->put("Discount", create(Null))
    ->put("Orange", create(Slice, 3)
        ->add(create(String, "Baz"))
        ->add(create(String, "Quu"))
        ->add(create(Boolean, false))
        ->add(create(Number, 123.456)))
    ->put("Strawberry", create(Hash, 10)
        ->put("Red", create(Number, 67))
        ->put("Green", create(Number, 123))
        ->put("Blue", create(Number, 456)))->$$;

puts("Result:");
char *json = $(hash)->chars();
puts(json);

Hash *parsed = as(Hash, from_json(json)->value())->$$;
puts("Parsed Strawberry.Red:");
puts(as(Hash, $(parsed)->get("Strawberry"))->get("Red")->to_chars());

StatusOr *bad = from_json("{\"Apple\": \"Foo\", \"Broken\": }")->$$;
if (!$(bad)->ok()) {
  puts("Parse error:");
  puts($(bad)->error());
}

release(bad);
release(parsed);
release(hash);
```

Running `./build/chain_nest` outputs:

```text
Result:
{"Apple":"Foo","Banana":42,"Organic":true,"Discount":null,"Orange":["Baz","Quu",false,123.456],"Strawberry":{"Red":67,"Green":123,"Blue":456}}
Parsed Strawberry.Red:
67
Parse error:
Unexpected character at offset 27
```

--------------------------------------------------------------------------------

## 2. Contiguous TypedArrays (`chain_typed_array.c`)

[`chain_typed_array.c`](chain_typed_array.c) demonstrates `TypedIntArray`,
`TypedFloatArray`, and `TypedCharArray` (`core/typed_array.h`), which store raw
C primitives (`int`, `double`, `char`) in a single contiguous heap buffer
instead of boxing each element in its own `GenericEl` struct:

```c
TypedIntArray *scores = create(TypedIntArray, 0)
    ->push_n(6, 42, 15, 88, 23, 16, 4)
    ->sort()
    ->$$; /* [4, 15, 16, 23, 42, 88] */
```

Typed arrays support `push`, `push_n`, `fill`, `from_buf`, `subarray`, `map`,
`filter`, `sort`, `reverse`, `reduce`, `foreach`, `join`, and `to_slice()`, and
can be placed directly inside a `Hash` or `Slice` for JSON serialization.

--------------------------------------------------------------------------------

## 3. SQLite Persistence (`chain_db.c`) and Vector Search (`chain_vector_db.c`)

`ext/db.h` and `ext/db.c` wrap SQLite (`<sqlite3.h>`) in a `GenericEl`-backed
`Db` object:

-   `exec(char *sql)` executes DDL/DML statements and returns `Db*` for
    chaining.
-   `insert(char *table, Hash *row)` builds and executes a parameterized
    `INSERT INTO <table> (...) VALUES (?, ...)` directly from a `Hash*`
    (including rows containing `TypedFloatArray*` embedding vectors).
-   `params(void *slice_or_hash)` binds either positional `?` parameters from a
    `Slice*` or named `:key` parameters from a `Hash*`.
-   `query(char *sql)` and `query_one(char *sql)` return a `StatusOr*` wrapping
    a `Slice*` of `Hash*` rows (or a single `Hash*` row), which can be
    serialized straight to JSON via `->chars()`.

### Linux `whatis` Man-Page Vector Database (`chain_vector_db.c`)

[`chain_vector_db.c`](chain_vector_db.c) indexes the system's Linux man-page
dictionary (`/usr/bin/whatis` across sections `1`, `2`, and `3`) into an
in-memory SQLite table, projecting each command/syscall/library entry into a
256-dimensional `TypedFloatArray` embedding using multi-probe word + character
trigram feature hashing and normalizing with `map` + `reduce`:

```c
double norm = sqrt(
    $(vec)->subarray(0, vec->length)->map(square_double)->reduce(add_doubles, 0.0));
```

It registers a custom `cosine_similarity(vec_a, vec_b)` SQL scalar function via
`sqlite3_create_function()` and queries the corpus by binding
`embed_text(query_text)` directly in `Db::params()`:

```text
$ ./build/chain_vector_db "What SQL tool should I use"
Indexed 4107 Linux man pages from /usr/bin/whatis (sections 1, 2, 3)
Query: "What SQL tool should I use"
Embedding preview (first 8 of 256 dims via TypedFloatArray::subarray):
[0,0,0,0,0,0,0,0]
Top semantic matches (JSON):
[{"name":"tinysparql-sql","section":"1","description":"Use SQL to query the TinySPARQL databases.","similarity":0.5616},{"name":"git-patch-id","section":"1","description":"Compute unique IDs for patches","similarity":0.3031},{"name":"perl5282delta","section":"1","description":"what is new for perl v5.28.2","similarity":0.2793},{"name":"wait3","section":"2","description":"wait for process to change state, BSD style","similarity":0.279},{"name":"perl5244delta","section":"1","description":"what is new for perl v5.24.4","similarity":0.2629}]
Max similarity score: 0.5616

Top 3 results:
  1. [0.5616] tinysparql-sql (1) - Use SQL to query the TinySPARQL databases.
  2. [0.3031] git-patch-id (1) - Compute unique IDs for patches
  3. [0.2793] perl5282delta (1) - what is new for perl v5.28.2
```

You can also pass an optional second argument (`1`, `2`, or `3`) to filter by
man-page section:

```bash
./build/chain_vector_db "create a child process" 2
```

--------------------------------------------------------------------------------

## 4. REST HTTP Server (`chain_server.c`)

`ext/server.h` and `ext/server.c` combine a native chaining router
(`dispatch()`) with a zero-dependency POSIX `<sys/socket.h>` HTTP/1.1 transport
(`start()`, `stop()`, `listen()`, `request()` in
[`chain_server.c`](chain_server.c)):

-   Route registration chains `.get(pattern, handler)`,
    `.post(pattern, handler)`, `.put(pattern, handler)`, and
    `.delete_route(pattern, handler)`.
-   `dispatch(method, path, query, body)` splits URL segments (`:id` parameters)
    and query strings into `Hash*` maps, parses JSON bodies via `from_json()`,
    and invokes the matched `RouteHandler(Hash *req)`.
-   Handlers return any `GenericEl*` (`StatusOr*`, `Hash*`, or `Slice*`), which
    the transport serializes to JSON with automatic HTTP `200`/`400`/`404`
    status mapping.
-   See [`../benchmarks/`](../benchmarks/README.md) for a head-to-head
    throughput and code comparison of `Server` + `Db` against equivalent
    Node.js/Express and Python/Django + SQLite setups.

--------------------------------------------------------------------------------

## 5. Model Context Protocol (MCP) Server (`chain_mcp.c`)

`ext/mcp.h` and `ext/mcp.c` provide a fluent JSON-RPC 2.0 MCP server wrapper
([`chain_mcp.c`](chain_mcp.c) and `tests/mcp_test.c`):

-   Tool registration chains `.tool(name, description, input_schema, handler)`
    where `input_schema` is a nested `Hash*` JSON Schema (or `NULL` for an empty
    object schema) and `handler(Hash *args)` returns any `GenericEl*` or
    `StatusOr*`.
-   `dispatch(json_rpc_line)` handles `initialize`, `notifications/initialized`,
    `ping`, `tools/list`, and `tools/call` in memory.
-   `call_tool(name, args)` invokes a registered tool directly in memory for
    unit tests.
-   `serve_stdio()` runs a line-delimited JSON-RPC 2.0 loop over
    `stdin`/`stdout` for local MCP clients.

For example, [`chain_mcp.c`](chain_mcp.c) wraps `/bin/uname`, `/bin/df`, and
`/bin/ps` into structured JSON tools on a stdio MCP server:

```c
static GenericEl* system_info_tool(Hash *args) {
  (void)args;
  char *const argv[] = {"/bin/uname", "-s", "-n", "-r", "-m", NULL};
  StatusOr *cmd = run_cmd(argv)->$$;
  if (!$(cmd)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(cmd)->error());
    release(cmd);
    return (GenericEl*)$(err);
  }

  Slice *parts = $((String*)cmd->val)->split(" \t\n")->$$;
  Hash *info = create(Hash, 4)
      ->put("sysname", create(String, slice_str(parts, 0)))
      ->put("nodename", create(String, slice_str(parts, 1)))
      ->put("release", create(String, slice_str(parts, 2)))
      ->put("machine", create(String, slice_str(parts, 3)));
  release(parts);
  release(cmd);
  return (GenericEl*)$(info);
}
```

```c
create(Mcp, "sys-inspector", "1.0.0")
    ->tool("system_info", "Return kernel name, hostname, release, and arch",
           NULL, system_info_tool)
    ->tool("disk_usage", "Return filesystem capacity and free space in KB",
           disk_usage_schema, disk_usage_tool)
    ->tool("top_processes", "Return the top CPU-consuming processes",
           top_processes_schema, top_processes_tool)
    ->serve_stdio();
```

Pointing any MCP client's `mcpServers` configuration at the compiled binary
exposes `system_info`, `disk_usage`, and `top_processes` directly to an AI
agent:

```json
{
  "mcpServers": {
    "sys-inspector": {
      "command": "/path/to/build/chain_mcp",
      "args": []
    }
  }
}
```

```text
● Called 3 MCP tools (sys-inspector/system_info, sys-inspector/disk_usage, sys-inspector/top_processes)

  1. Kernel & Architecture (system_info)
     • OS / Kernel Name: Linux
     • Kernel Release: 6.8.0-generic
     • Architecture: x86_64

  2. Disk Space on /tmp (disk_usage)
     • Filesystem: tmpfs (mounted on /tmp)
     • Total Space: 16,777,216 KB (~16.0 GiB)
     • Available Space: 16,515,072 KB (~15.75 GiB, 2% used)

  3. Top Processes (top_processes)
     PID       | Command          | CPU %   | Memory %
     ----------|------------------|---------|---------
     1420      | postgres         | 4.2%    | 1.8%
     1184      | nginx            | 1.5%    | 0.3%
     2048      | python3          | 0.8%    | 0.6%
```
