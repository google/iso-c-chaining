# ISO C Chaining Unit Tests

This directory contains the unit test suite for `iso-c-chaining`, written using
the library's own chainable `Test` runner (`core/test.h`).

## Running the Tests

From the repository root, build and run the full test suite (all unit tests in
`tests/` and `demos/tests/`, plus `--test` self-checks across all binaries in
`examples/` and `demos/examples/`) via CMake
and CTest:

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

--------------------------------------------------------------------------------

## Fluent Unit Testing (`core/test.h`)

`core/test.h` and `core/test.c` provide a chainable `Test` (`ChTest`) element that
automatically claims and releases unowned inline temporaries (`refs == 0`)
passed to assertion methods while preserving caller-saved (`refs > 0`) fixtures:

-   `section(char *name)` sets the active test section label for failure
    diagnostics.
-   `expect_eq(void *actual, void *expected)` verifies that two elements share
    the same runtime `ChType` tag and `to_chars()` representation (unwrapping
    any `StatusOr*` arguments automatically).
-   `expect_str(void *actual, char *expected)`,
    `expect_num(void *actual, double expected)`, and
    `expect_bool(void *actual, int expected)` assert against C primitive
    literals.
-   `expect_ok(StatusOr *res)` and `expect_err(StatusOr *res, char *substr)`
    assert `StatusOr` success or error substring matches.
-   `summary()` prints the pass/fail report, frees the `Test` runner if unowned,
    and returns `0` (all passed) or `1` (any failed) for `main()`.

```c
int main(void) {
  return create(Test, "String & Slice Tests")
      ->section("split and sub_slice")
      ->expect_str(
          create(String, "a:b:c")->split(":")->sub_slice(0, 2),
          "[\"a\", \"b\"]")
      ->expect_num(
          create(String, "hello")->split(""),
          5)
      ->summary();
}
```

--------------------------------------------------------------------------------

## Test Suites

-   [`core_primitives_test.c`](core_primitives_test.c): `String`, `Number`,
    `Boolean`, `Null`, `GenericEl` runtime type checks (`CH_MAGIC`, `as()`,
    `is_type()`), reference counting (`refs`, `release()`), and `NOMACRO`
    equivalence.
-   [`containers_test.c`](containers_test.c): `Slice`, `Hash`, deeply nested
    containers, intrusive `self` stack re-entrancy, and `TypedArray`
    (`TypedIntArray`, `TypedFloatArray`, `TypedCharArray`, and `NOMACRO`
    equivalents).
-   [`json_status_test.c`](json_status_test.c): `from_json()` parser, JSON
    serialization (`to_chars()`), and `StatusOr` error propagation.
-   [`single_header_test.c`](single_header_test.c): End-to-end verification of
    the generated `chain_single.h` single-header amalgamation
    (`CHAIN_IMPLEMENTATION`).

Tests for the demo extensions live alongside them in
[`../demos/tests/`](../demos/tests/):

-   [`db_test.c`](../demos/tests/db_test.c): SQLite `Db` extension (`exec`,
    `query`, `query_row`, `insert`, positional `?` and named `:key` `params`).
-   [`server_test.c`](../demos/tests/server_test.c): `Server` routing (`GET`,
    `POST`, `PUT`, `DELETE`), URL `:id` segment parameters, query string
    parsing, and POSIX HTTP/1.1 loopback socket transport.
-   [`mcp_test.c`](../demos/tests/mcp_test.c): Model Context Protocol (`Mcp`)
    JSON-RPC 2.0 server (`initialize`, `ping`, `tools/list`, `tools/call`, and
    in-memory `call_tool`).
