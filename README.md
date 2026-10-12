## Function chaining in ISO C experiment

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
        ->subSlice(2, 5);
```

At this stage, *mid* should contain *["Two", "Three", "Four", "Five"]*.

How can we make this happen? C isn't equipped for such syntax, right? We have
some sort of class or type system going on to handle member functions, plus the
code shows a knowledge of the previous result to add to it. There would have to
be something like a Python-like *self* variable everywhere somehow. It can't be
done in straight C, right?

Well, this works exactly as written, which you can see in `examples/chain.c`.

If you just want to see the result in action run:

```
blaze run experimental/users/danielmi/chaining:chain
```

This should give you the thrilling result of:

```
["Two","Three","Four","Five"]
Two_Three_Four_Five
```

Slightly more thrilling is that there are no errors with go/msan or go/asan:

```
blaze run --config=msan experimental/users/danielmi/chaining:chain; \
blaze run --config=asan experimental/users/danielmi/chaining:chain
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
*newSlice()* constructor.

```c
Slice* __newSlice(int n) {
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
created with *create(String, ...)* calls before.

#### ...And one more

There is another problem with the line above. The *mid* Slice was not created
with a *create(Slice, ...)*, so we have no idea whether it's an anonymous Slice
that we need to get rid of after use, or if it's something we want to keep for
later on.

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

return 0;
```

### Adding a few helper macros and inline helpers

Borrowing some syntax from jQuery, we can use macros and inline helpers to
replace our warty calls in `core/types.h`:

```c
static void* ch_bind_el(ChGenericEl *el) {
  return el != NULL ? el->bind(el) : NULL;
}

static void ch_release(void *elem) {
  if (elem != NULL) {
    ((ChGenericEl*)elem)->del(elem);
  }
}

#define ch_create(class, ...) ((Ch ## class*)ch_new_ ## class(__VA_ARGS__))
#define ch_bind(obj) ((obj)->bind((ChGenericEl*)(obj)))
#define ch_save generic.save()
#define ch_as(class, elem) \
  ((Ch ## class*)ch_cast_el((ChGenericEl*)(elem), \
                            (ChType)&ch_new_ ## class, #class))
#define ch_is_type(elem, type) \
  ((elem) != NULL && ((ChGenericEl*)(elem))->type_val == (type))
#define ch_chars() generic.to_chars()
#define ch_slf self(NULL)
```

By default, `core/types.h` also defines short aliases (`create`, `$()`, `$$`,
`as`, `is_type`, `chars`, `release`, `use`, `String`, `Slice`, `Hash`, etc.). If
embedding the library alongside headers that collide with unprefixed names,
define `#define CHAIN_NO_SHORT_NAMES` before including the headers to expose
only `ch_`/`Ch`/`CH_` prefixed symbols.

This way, instead of the verbose lines in the last section, we can write:

```c
String *string = create(String, "Zero,One,Two,Three")->$$;
Slice *mid = $(string)->split(",")->append(more)->sub_slice(2, 5)->$$;
puts($(mid)->chars());
release(mid);
```

To declare the typedef, constructor prototype, and unique runtime `ChType` tag
in one step without maintaining a central enum in `core/types.h`:

```c
#define ch_use(class, type, ...) \
  typedef struct type Ch ## class; \
  typedef struct type ## _ Ch ## class ## Element; \
  Ch ## class* ch_new_ ## class(__VA_ARGS__); \
  static const ChType CH_ ## type = (const void*)&ch_new_ ## class; \
  ch_use_short_(class, type)
```

Using this just means adding a `ch_use()` (or `use()`) line in the class's
header (or directly in an application file for custom types):

```c
ch_use(String, STRING, char*)
```

### Generic elements, runtime type safety, and heterogeneous containers

To let `Slice` and `Hash` hold arbitrary object types (`String`, `Number`,
`Boolean`, `Null`, `Slice`, `Hash`, or any user-defined class), `core/types.h`
defines `ChType` as a pointer tag (`(const void*)&ch_new_<Class>`) and a common
`ChGenericEl` (`GenericEl`) header struct:

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

Each class places `GenericEl generic;` as the first member of its struct:

```c
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

At the same time, `type_val` provides runtime type safety:

-   `update_self_stack(&top, s, EXPECTED_TYPE)` verifies that `s->type_val`
    matches the target stack's type, preventing cross-type `bind()` calls from
    corrupting another type's `self` stack.
-   `as(Class, elem)` performs a runtime-checked downcast from `GenericEl*` to
    `Class*` (and binds the instance for immediate chaining), aborting with a
    diagnostic if the runtime type doesn't match.
-   `is_type(elem, TYPE)` checks whether a `GenericEl*` has a given `Type`.

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

-   Calling `self(s)` verifies `s->type_val` and pushes a new instance onto the
    intrusive doubly-linked stack (or unlinks and moves an already-present
    instance to the top in $O(1)$ time so repeated `$(...)` binds don't
    duplicate entries).
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

Running `blaze run experimental/users/danielmi/chaining:chain_nest` outputs:

```
Result:
{"Apple":"Foo","Banana":42,"Organic":true,"Discount":null,"Orange":["Baz","Quu",false,123.456],"Strawberry":{"Red":67,"Green":123,"Blue":456}}
Parsed Strawberry.Red:
67
Parse error:
Unexpected character at offset 27
```

### Fluent SQLite persistence with `Db`

`ext/db.h` and `ext/db.c` wrap SQLite (`//third_party/sqlite`) in a
`GenericEl`-backed `Db` object (`examples/chain_db.c`):

-   `exec(char *sql)` executes DDL/DML statements and returns `Db*` for chaining.
-   `insert(char *table, Hash *row)` builds and executes a parameterized
    `INSERT INTO <table> (...) VALUES (?, ...)` directly from a `Hash*` (such as
    an inline `create(Hash, ...)` or a parsed `from_json(...)` payload).
-   `params(void *slice_or_hash)` binds either positional `?` parameters from a
    `Slice*` or named `:key` parameters from a `Hash*` by inspecting each
    element's `type_val` (`NUMBER`, `STRING`, `BOOLEAN`, `NULL_VAL`, or nested
    containers).
-   `query(char *sql)` and `query_one(char *sql)` return a `StatusOr*` wrapping
    a `Slice*` of `Hash*` rows (or a single `Hash*` row), which can be
    serialized straight to JSON via `->chars()`.

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

In addition to Blaze (`BUILD`), the project includes a `CMakeLists.txt` and an
stb-style single-header amalgamation generator (`tools/make_single_header.py`):

-   **CMake Build & CTest**:
    ```bash
    cmake -B build
    cmake --build build
    ctest --test-dir build --output-on-failure
    ```
-   **Generating `chain_single.h`**:
    -   With Blaze:
        `blaze build experimental/users/danielmi/chaining:chain_single_header`
    -   With CMake: `cmake --build build --target single_header`
    -   Directly: `python3 tools/make_single_header.py --output chain_single.h`
-   **Using `chain_single.h`**:
    In one C/C++ source file, define `CHAIN_IMPLEMENTATION` before including
    `chain_single.h` (and optionally `CHAIN_ENABLE_DB` when linking
    `-lsqlite3`):
    ```c
    #define CHAIN_IMPLEMENTATION
    #include "chain_single.h"
    ```

### Security and Vulnerability Reporting

Eligibility for the [Google Open Source Software Vulnerability Rewards Program](https://bughunters.google.com/open-source-security) is determined by the [Google Open Source Software Vulnerability Reward Program Rules](https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules).

