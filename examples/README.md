# ISO C Chaining Examples

This directory contains runnable examples demonstrating the core data
structures, macro-free chaining, and contiguous `TypedArray`s. Larger programs
built on the library (an HTTP server, an MCP server, and a SQLite wrapper) live
in [`../demos/`](../demos/README.md).

## Quick Start

Build all examples from the repository root with CMake:

```bash
cmake -B build && cmake --build build
```

Every example also supports a `--test` flag (used by `ctest`) to run built-in
assertions via `Test` (`core/test.h`):

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
