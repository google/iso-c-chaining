## Function chaining in ISO C

I like the concept of function chaining, so that things like builders and
promises can be concisely expressed. I thought of course that with enough magic,
this could be done in C.

### The goal

The final concept should look something like this.

```c
Slice *more = newSlice(3)
        ->add(newString("Four"))
        ->add(newString("Five"))
        ->add(newString("Six"));
Slice *mid = newString("Zero,One,Two,Three")
        ->split(",")
        ->append(more)
        ->sub_slice(2, 5);
```

At this stage, *mid* should contain *["Two", "Three", "Four", "Five"]*.

How can we make this happen? C isn't equipped for such syntax, right? We have
some sort of class or type system going on to handle member functions, plus the
code shows a knowledge of the previous result to add to it. There would have to
be something like a Python-like *self* variable everywhere somehow. It can't be
done in straight C, right?

Well, with a couple of small tweaks this works as written, which you can see in
`examples/chain.c`.

If you just want to see the result in action run:

```bash
cmake -B build && cmake --build build
./build/chain
```

This should give you the thrilling result of:

```
["Two","Three","Four","Five"]
```

Slightly more thrilling is that there are no errors with MemorySanitizer or
AddressSanitizer:

```bash
cmake -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address -g" && \
  cmake --build build-asan
./build-asan/chain
```

### Look Ma, no preprocessor tricks!

Since C doesn't have the proper means to restrict any data to a struct or
function, or any modern object-oriented concepts whatsoever, we have to get
clever.

Structs with function pointers are common enough, but the functions themselves
have no context to go with them. So instead maybe the context could be brought
along as a function:

```c
static String* self(GenericEl *s) {
  static String *self = NULL;

  if (s != NULL) {
    self = (String*)s;
  }
  return self;
}
```

You may recognize this from the private variable pattern in JavaScript.

Here we have the rare case of both uses of the *static* keyword in C. The first
before the function keeps things within the local file as scope. The second on
the variable allows us to keep a scoped global context.

Inside a function we can implement a member function like this:

```c
static GenericEl* get(int n) {
  return self(NULL)->elements[n];
}
```

But wait! This function is static as well, so how will people ever call it? Well
we can expose it through a struct:

```c
struct SLICE {
  GenericEl **elements;
  GenericEl* (*get)(int);
};
```

Then we need to bind the function pointers to something, so we create a
*ch_new_Slice()* constructor.

```c
Slice* ch_new_Slice(int n) {
  Slice *slice = malloc(sizeof(Slice));

  slice->get = get;

  self((GenericEl*)slice);

  return slice;
}
```

Using the returned Slice, we can then perform all the member's operations on it.

### A small problem

So now you might be thinking, **Oh no, we have placed the self pointer in
scoped-global memory, so now I have no idea which Slice I'm working with!**

And you would be right, all new *get()* calls would just use whatever slice the
self pointer had been assigned to most recently.

#### Adding a wart

So how do we handle it? We introduce a *bind()* call to update the *self*
pointer.

```c
Slice *mid = string->bind((GenericEl*)string)->split(",");
```

Here, the String's *self* had been allocated to a different anonymous String in
the call before, so we have to re-bind it back to the String we want to work
with. So our *string->bind((GenericEl\*)string)* sets up the *split()* call to
actually work on our previously allocated String, rather than the anonymous one
created with *ch_new_String(...)* calls before.

#### ...And one more

There is another problem with the line above. The *mid* Slice was not created
directly with *ch_new_Slice(...)*, so we have no idea whether it's an anonymous
Slice that we need to get rid of after use, or if it's something we want to keep
for later on.

Inside the implementation is memory handling through reference counting.
Internally to the slice, memory is allocated and deleted as the chain is
executed. This allows for the simplicity shown in the `examples/chain.c` main
program, but it causes a problem with pre-deleted structs that we might want to
use later on.

To make sure the *mid* Slice is not deleted prematurely, we add our second wart,
the *save()* method.

```c
Slice *mid = string->bind((GenericEl*)string)->split(",")->generic.save();
```

This increments the reference count so that the struct stays around until it's
deleted. Which reminds me, we also need a delete method to clean up any pesky
*save()*d structs.

```c
mid->bind((GenericEl*)mid)->generic.del(mid);
```

### Adding a few helper macros and inline helpers

Borrowing some syntax from jQuery, we can use macros and inline helpers to
replace our warty calls in `core/types.h`:

```c
#define create(class, ...) ((class*)ch_new_ ## class(__VA_ARGS__))
#define $(obj) ((obj)->bind((GenericEl*)(obj)))
#define $$ generic.save()
#define chars() generic.to_chars()
#define slf self(NULL)

static void release(void *elem) {
  if (elem != NULL) {
    ((GenericEl*)elem)->del(elem);
  }
}
```

This way, instead of the verbose lines in the last section, we can write:

```c
String *string = create(String, "Zero,One,Two,Three")->$$;
Slice *mid = $(string)->split(",")->append(more)->sub_slice(2, 5)->$$;
puts($(mid)->chars());
release(mid);
```

(Under the hood, `core/types.h` defines all symbols with a `ch_`/`Ch`/`CH_`
prefix—`ch_create`, `ch_bind`, `ch_save`, `ch_chars`, `ch_release`, `ChString`,
`CH_STRING`, etc.—and aliases the short names above by default. If embedding the
library alongside headers that collide with unprefixed names, define `#define
CHAIN_NO_SHORT_NAMES` before including the headers to expose only the prefixed
symbols.)

### Generic elements, runtime type safety, and heterogeneous containers

To let `Slice` and `Hash` hold arbitrary object types (`String`, `Number`,
`Boolean`, `Null`, `Slice`, `Hash`, or any user-defined class), `core/types.h`
defines `ChType` (`Type`) as a unique pointer tag (`(const
void*)&ch_new_<Class>`) and a common `ChGenericEl` (`GenericEl`) header struct:

```c
typedef const void* ChType;

#define CH_GENERIC ((ChType)NULL)
#define CH_MAGIC 0x4348454Cu /* "CHEL" (Chaining Element) */

struct ch_generic_el {
  unsigned int magic;
  int refs;
  ChType type_val;
  ChGenericEl *prev_self;
  ChGenericEl *next_self;
  void* (*bind)(ChGenericEl *);
  void* (*save)(void);
  void (*del)(void *);
  char* (*to_chars)(void);
};
```

To declare a class's typedef, constructor prototype, and unique `ChType` tag in
one step without maintaining a central enum in `core/types.h`, `ch_use()` (or
`use()`) expands to:

```c
#define ch_use(class, type, ...) \
  typedef struct type Ch ## class; \
  typedef struct type ## _ Ch ## class ## Element; \
  Ch ## class* ch_new_ ## class(__VA_ARGS__); \
  static const ChType CH_ ## type = (const void*)&ch_new_ ## class; \
  ch_use_short_(class, type)
```

Using this just means adding a `ch_use()` line in the class's header (or
directly in an application file for custom types), and placing `GenericEl
generic;` as the first member of its struct:

```c
ch_use(String, STRING, char*)

struct STRING {
  GenericEl generic;
  String* (*bind)(GenericEl*);
  char *chars;
  int size;
  void (*set)(char*);
  int (*length)(void);
  Slice* (*split)(char*);
  String* (*append)(char*);
  String* (*append_escaped)(char*);
};
```

Because `generic` is the first field, any `String*`, `Number*`, `Boolean*`,
`Null*`, `Slice*`, `Hash*`, `StatusOr*`, `Db*`, or `Server*` can be safely cast
to `GenericEl*` for reference counting, deletion, rebinding (`generic.bind`),
and string serialization (`to_chars()`), without containers needing hardcoded
`switch` statements over concrete types.

At the same time, `magic` and `type_val` provide two layers of runtime type
safety:

-   `is_generic_el(ptr)` and `check_el(ptr, context)` verify that any non-`NULL`
    pointer begins with the 4-byte `CH_MAGIC` sentinel (`"CHEL"`) before
    dereferencing it as a `GenericEl*` (for example in `release()`,
    `Slice::add()`, or `Hash::put()`), aborting immediately with a diagnostic if
    a raw C pointer such as `char*` or `int*` was passed by mistake.
-   `as(Class, elem)` verifies both `CH_MAGIC` and `elem->type_val`, performing
    a runtime-checked downcast from `GenericEl*` to `Class*` (and binding the
    instance for immediate chaining) or aborting with a diagnostic on mismatch.
-   `is_type(elem, TYPE)` safely checks whether an arbitrary pointer is a valid
    `GenericEl*` of the given `Type`.

### Nesting objects with an intrusive `self` stack

What happens when we nest containers of the same type—such as a `Hash` inside
another `Hash`, or a `Slice` inside a `Slice`?

If `self()` only holds a single static pointer, creating an inner `Hash` inside
an outer `Hash`'s `put()` arguments overwrites `self` with the inner `Hash`. By
the time the outer `put()` executes, it has forgotten the outer `Hash`! The same
problem occurs during recursive deletion when an inner container is freed
mid-loop.

To solve this in $O(1)$ time with no depth limit, `GenericEl` embeds intrusive
`prev_self` and `next_self` links, and each type's `self()` delegates to
`update_self_stack()`:

```c
static Hash* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Hash*)update_self_stack(&top, s, HASH);
}
```

-   Calling `self(s)` verifies `is_generic_el(s)` and `s->type_val == HASH` (so
    cross-type `bind()` calls cannot corrupt another type's stack) and pushes
    `s` onto the intrusive doubly-linked stack, or unlinks and moves an
    already-present instance to the top in $O(1)$ time so repeated `$(...)`
    binds don't duplicate entries.
-   Calling `self((GenericEl*)-1)` pops the current instance off the stack and
    restores the previous instance.
-   When an inline child object finishes its chain and is passed into an outer
    `put()` or `add()`, `claim_element()` pops the child's stack if needed and
    increments `refs` so `slf` automatically resolves back to the enclosing
    instance.

This allows arbitrary nesting and JSON parsing with `StatusOr` in
`examples/chain_nest.c`:

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

```
Result:
{"Apple":"Foo","Banana":42,"Organic":true,"Discount":null,"Orange":["Baz","Quu",false,123.456],"Strawberry":{"Red":67,"Green":123,"Blue":456}}
Parsed Strawberry.Red:
67
Parse error:
Unexpected character at offset 27
```

### Pulling back a little performance

You might look at this and think, "Wow, this is wrapping every single int in an
object like I'm a lowly Java developer." And you would be right. But we can take
a page from JavaScript's TypedArrays and not have a bunch of function pointers
attached to every element, giving us back the performance we expect from C.

Let's look at `examples/chain_typed_array.c` as it pushes 6 elements into a
dynamic buffer:

```c
TypedIntArray *scores = create(TypedIntArray, 0)
      ->push_n(6, 42, 15, 88, 23, 16, 4)
      ->sort()
      ->$$; /* [4, 15, 16, 23, 42, 88] */
```

We now only have a single `TypedIntArray` object, so our massive array of
integers isn't attached to an even more massive number of function pointers. We
can do typed arrays of any fixed-width type, but ints, chars, and doubles are
included by default. We add typed arrays like this:

```c
CH_DECLARE_TYPED_ARRAY(TypedIntArray, TYPED_INT_ARRAY, int)
```

TypedArrays can be sorted, mapped, filtered, and more, but if you really have to
wrap every element in a `GenericEl` you can always use `->to_slice()`.

### Fluent SQLite persistence with `Db`

`ext/db.h` and `ext/db.c` wrap SQLite (`<sqlite3.h>`) in a `GenericEl`-backed
`Db` object (`examples/chain_db.c` and `examples/chain_vector_db.c`):

-   `exec(char *sql)` executes DDL/DML statements and returns `Db*` for
    chaining.
-   `insert(char *table, Hash *row)` builds and executes a parameterized `INSERT
    INTO <table> (...) VALUES (?, ...)` directly from a `Hash*` (such as an
    inline `create(Hash, ...)`, a parsed `from_json(...)` payload, or a row
    containing a `TypedFloatArray*` embedding vector).
-   `params(void *slice_or_hash)` binds either positional `?` parameters from a
    `Slice*` or named `:key` parameters from a `Hash*` by inspecting each
    element's `type_val` (`NUMBER`, `STRING`, `BOOLEAN`, `NULL_VAL`, or nested
    containers / `TypedArray`s).
-   `query(char *sql)` and `query_one(char *sql)` return a `StatusOr*` wrapping
    a `Slice*` of `Hash*` rows (or a single `Hash*` row), which can be
    serialized straight to JSON via `->chars()`.
-   `examples/chain_vector_db.c` demonstrates building a lightweight vector
    database on top of `Db` and `TypedFloatArray`—indexing the system's Linux
    `whatis` man-page dictionary (`sections 1, 2, 3`), storing `TypedFloatArray`
    embeddings directly via `insert()`, registering a custom
    `cosine_similarity(a, b)` SQL function with `sqlite3_create_function()`, and
    running hybrid metadata + top-$K$ vector similarity queries.

### REST HTTP Server with `Server`

`ext/server.h` and `ext/server.c` combine a native chaining router
(`dispatch()`) with a zero-dependency POSIX `<sys/socket.h>` HTTP/1.1 transport
(`start()`, `stop()`, `listen()`, `request()` in `examples/chain_server.c`):

-   Route registration chains `.get(pattern, handler)`, `.post(pattern,
    handler)`, `.put(pattern, handler)`, and `.delete_route(pattern, handler)`.
-   `dispatch(method, path, query, body)` splits URL segments (`:id` parameters)
    and query strings into `Hash*` maps, parses JSON bodies via `from_json()`,
    and invokes the matched `RouteHandler(Hash *req)`.
-   Handlers return any `GenericEl*` (`StatusOr*`, `Hash*`, or `Slice*`), which
    the transport serializes to JSON with automatic HTTP `200`/`400`/`404`
    status mapping.
-   See [`benchmarks/`](benchmarks/README.md) (`benchmarks/server_bench.c`,
    `benchmarks/express_bench.js`, and `benchmarks/django_bench.py`) for a
    head-to-head throughput and code comparison of `Server` + `Db` against
    equivalent Node.js/Express and Python/Django + SQLite setups.

### Model Context Protocol (MCP) Server with `Mcp`

`ext/mcp.h` and `ext/mcp.c` provide a fluent JSON-RPC 2.0 MCP server wrapper
(`examples/chain_mcp.c` and `tests/mcp_test.c`):

-   Tool registration chains `.tool(name, description, input_schema, handler)`
    where `input_schema` is a nested `Hash*` JSON Schema (or `NULL` for an empty
    object schema) and `handler(Hash *args)` returns any `GenericEl*` or
    `StatusOr*`.
-   `examples/chain_mcp.c` demonstrates wrapping standard Linux CLI tools
    (`/bin/uname`, `/bin/df`, and `/bin/ps`), parsing their tabular text output
    with `String::split()` and `Slice`, and returning structured JSON objects
    via `Hash` over MCP.
-   `dispatch(json_rpc_line)` handles `initialize`, `notifications/initialized`,
    `ping`, `tools/list`, and `tools/call` in memory.
-   `call_tool(name, args)` invokes a registered tool directly in memory for
    unit tests.
-   `serve_stdio()` runs a line-delimited JSON-RPC 2.0 loop over
    `stdin`/`stdout` for local MCP clients.

For example, `examples/chain_mcp.c` wraps `/bin/uname` into a structured
`system_info` tool and registers three host-inspection tools on a stdio MCP
server:

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
     • Kernel Release: 7.1.6-1rodete1-amd64
     • Architecture: x86_64

  2. Disk Space on /tmp (disk_usage)
     • Filesystem: tmpfs (mounted on /tmp)
     • Total Space: 65,507,828 KB (~62.47 GiB)
     • Available Space: 65,288,520 KB (~62.26 GiB, 1% used)

  3. Top Processes (top_processes)
     PID       | Command          | CPU %   | Memory %
     ----------|------------------|---------|---------
     3293590   | chrome           | 8.9%    | 0.4%
     9019      | gnome-shell      | 6.4%    | 1.9%
     3905      | wireplumber      | 6.3%    | 0.0%
```

### Fluent Unit Testing with `Test`

`ext/test.h` and `ext/test.c` provide a chainable `Test` (`ChTest`) element
(`tests/*_test.c`) that automatically claims and releases unowned inline
temporaries (`refs == 0`) passed to assertion methods while preserving
caller-saved (`refs > 0`) fixtures:

-   `section(char *name)` sets the active test section label for failure
    diagnostics.
-   `expect_eq(void *actual, void *expected)` verifies that two elements share
    the same runtime `ChType` tag and `to_chars()` representation (unwrapping
    any `StatusOr*` arguments automatically).
-   `expect_str(void *actual, char *expected)`, `expect_num(void *actual, double
    expected)`, and `expect_bool(void *actual, int expected)` assert against C
    primitive literals.
-   `expect_ok(StatusOr *res)` and `expect_err(StatusOr *res, char *substr)`
    assert `StatusOr` success or error substring matches.
-   `summary()` prints the pass/fail report, frees the `Test` runner if unowned,
    and returns `0` (all passed) or `1` (any failed) for `main()`.

### Building with CMake and Single-Header Distribution

The project includes a `CMakeLists.txt` and an stb-style single-header
amalgamation generator (`tools/make_single_header.py`):

-   **CMake Build & CTest**:

    ```bash
    cmake -B build
    cmake --build build
    ctest --test-dir build --output-on-failure
    ```
-   **Generating `chain_single.h`**:

    -   With CMake: `cmake --build build --target single_header`
    -   Directly: `python3 tools/make_single_header.py --output chain_single.h`
-   **Using `chain_single.h`**: In one C/C++ source file, define
    `CHAIN_IMPLEMENTATION` before including `chain_single.h` (and optionally
    `CHAIN_ENABLE_DB` when linking `-lsqlite3`):

    ```c
    #define CHAIN_IMPLEMENTATION
    #include "chain_single.h"
    ```

### Security and Vulnerability Reporting

Eligibility for the
[Google Open Source Software Vulnerability Rewards Program](https://bughunters.google.com/open-source-security)
is determined by the
[Google Open Source Software Vulnerability Reward Program Rules](https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules).

### Disclaimer

This is not an officially supported Google product. This project is intended for
demonstration purposes only. It is not intended for use in a production
environment. This project is not intended for active development; issues and
contributions are handled on a best effort basis.
