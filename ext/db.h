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

#ifndef CHAIN_DB_H_
#define CHAIN_DB_H_

#include <sqlite3.h>

#include "core/hash.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Db, DB, (char*))

/*
 * Fluent SQLite3 database wrapper supporting parameterized DDL/DML execution,
 * `Hash`-based row insertion, and automatic mapping of result sets into
 * `Slice`s of `Hash` rows.
 */
struct DB {
  ChGenericEl generic;
  sqlite3 *handle;
  ChGenericEl *bound_params;
  ChString *last_err;

  /* Binds this `Db` as the active instance on the `Db` `self` stack. */
  ChDb* (*bind)(ChGenericEl*);

  /*
   * Executes one or more semicolon-separated SQL statements, consuming any
   * parameters staged via `params()`, and returns this `Db` for chaining.
   * Any SQLite error is recorded in `last_err` and surfaced on the next
   * `query()` / `query_one()` or `chars()` call.
   */
  ChDb* (*exec)(char*);

  /*
   * Stages positional (`Slice*` for `?`) or named (`Hash*` for `:key`, `@key`,
   * `$key`) parameters to be bound and consumed by the next `exec()`,
   * `query()`, or `query_one()` call. Returns this `Db` for chaining.
   */
  ChDb* (*params)(void*);

  /*
   * Builds and executes a parameterized `INSERT INTO <table> (...) VALUES`
   * statement from the keys and values of `row` (taking ownership of `row`),
   * and returns this `Db` for chaining.
   */
  ChDb* (*insert)(char*, ChHash*);

  /*
   * Executes a `SELECT` query (consuming any staged `params()`) and returns a
   * `StatusOr*` wrapping a `Slice*` of `Hash*` rows.
   */
  ChStatusOr* (*query)(char*);

  /*
   * Executes a `SELECT` query (consuming any staged `params()`) and returns a
   * `StatusOr*` wrapping the first row as a `Hash*`, or a failed `StatusOr*`
   * (`"No rows returned"`) if the result set is empty.
   */
  ChStatusOr* (*query_one)(char*);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_DB_H_ */
