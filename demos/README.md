# Demos: Applications Built on the Core

The programs in this directory exist to show that the chaining technique is not
limited to toy examples—the same `self`-stack mechanism carries a SQLite
wrapper, an HTTP/1.1 REST server, and a Model Context Protocol server without
special cases. Each one is a few hundred lines and reads top to bottom.

They are demonstrations, not infrastructure. They are single-threaded, do no
input hardening beyond what the examples need, bind only to loopback, and
should not be exposed to a network. If you want to understand the technique,
read [`../README.md`](../README.md) and [`../examples/`](../examples/README.md)
first; come here to see it stretch.

## Layout

-   [`db.h`](db.h) / [`db.c`](db.c): `Db`—fluent SQLite wrapper with `exec()`,
    `query()`, `query_one()`, `insert()`, and positional/named `params()`.
-   [`server.h`](server.h) / [`server.c`](server.c): `Server`—POSIX-socket
    HTTP/1.1 router with `:param` captures, query-string parsing, and JSON
    bodies.
-   [`mcp.h`](mcp.h) / [`mcp.c`](mcp.c): `Mcp`—JSON-RPC 2.0 stdio server
    implementing the Model Context Protocol `initialize` / `tools/list` /
    `tools/call` handshake.
-   [`examples/`](examples/): `chain_db.c`, `chain_vector_db.c`,
    `chain_server.c`, `chain_mcp.c` (each supports `--test`).
-   [`tests/`](tests/): `db_test.c`, `server_test.c`, `mcp_test.c`.
-   [`benchmarks/`](benchmarks/README.md): `Server` + `Db` throughput against
    equivalent Node.js/Express and Python/Django apps.

## Quick Start

```bash
cmake -B build && cmake --build build
./build/chain_db
./build/chain_server            # then: curl http://127.0.0.1:8080/users
./build/chain_mcp --test
```

--------------------------------------------------------------------------------

## 1. SQLite Persistence (`chain_db.c`) and Vector Search (`chain_vector_db.c`)

`demos/db.h` and `demos/db.c` wrap SQLite (`<sqlite3.h>`) in a `GenericEl`-backed
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

[`chain_vector_db.c`](examples/chain_vector_db.c) indexes the system's Linux man-page
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

## 2. REST HTTP Server (`chain_server.c`)

`demos/server.h` and `demos/server.c` combine a native chaining router
(`dispatch()`) with a zero-dependency POSIX `<sys/socket.h>` HTTP/1.1 transport
(`start()`, `stop()`, `listen()`, `request()` in
[`chain_server.c`](examples/chain_server.c)):

-   Route registration chains `.get(pattern, handler)`,
    `.post(pattern, handler)`, `.put(pattern, handler)`, and
    `.delete_route(pattern, handler)`.
-   `dispatch(method, path, query, body)` splits URL segments (`:id` parameters)
    and query strings into `Hash*` maps, parses JSON bodies via `from_json()`,
    and invokes the matched `RouteHandler(Hash *req)`.
-   Handlers return any `GenericEl*` (`StatusOr*`, `Hash*`, or `Slice*`), which
    the transport serializes to JSON with automatic HTTP `200`/`400`/`404`
    status mapping.
-   See [`benchmarks/`](benchmarks/README.md) for a head-to-head
    throughput and code comparison of `Server` + `Db` against equivalent
    Node.js/Express and Python/Django + SQLite setups.

--------------------------------------------------------------------------------

## 3. Model Context Protocol (MCP) Server (`chain_mcp.c`)

`demos/mcp.h` and `demos/mcp.c` provide a fluent JSON-RPC 2.0 MCP server wrapper
([`chain_mcp.c`](examples/chain_mcp.c) and `tests/mcp_test.c`):

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

For example, [`chain_mcp.c`](examples/chain_mcp.c) wraps `/bin/uname`, `/bin/df`, and
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
