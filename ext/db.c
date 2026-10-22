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

#include "ext/db.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static Db* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Db*)update_self_stack(&top, s, DB);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Db* bind(GenericEl* db) {
  return self(db);
}

static void set_error(Db *db, const char *msg) {
  if (db->last_err != NULL) {
    release(db->last_err);
    db->last_err = NULL;
  }
  if (msg != NULL) {
    db->last_err = create(String, (char*)msg)->$$;
    db->last_err->bind((GenericEl*)-1);
  }
}

static void clear_bound_params(Db *db) {
  if (db->bound_params != NULL) {
    release(db->bound_params);
    db->bound_params = NULL;
  }
}

static void del(void* obj) {
  Db* db = (Db*)obj;
  /*
   * `self((GenericEl*)db)` verifies `obj` is a `Db` and pushes it onto the
   * `Db` `self` stack so `slf` resolves to `db` during cleanup.
   */
  if (db != NULL && self((GenericEl*)db) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      clear_bound_params(slf);
      if (slf->last_err != NULL) {
        release(slf->last_err);
        slf->last_err = NULL;
      }
      if (slf->handle != NULL) {
        sqlite3_close(slf->handle);
        slf->handle = NULL;
      }
      Db *to_free = slf;
      /* Pop `db` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Internal SQLite parameter binding and row mapping helpers.
 */

static int bind_single_value(sqlite3_stmt *stmt, int idx, GenericEl *val) {
  /* Missing or `Null` values bind as SQL `NULL`. */
  if (val == NULL || val->type_val == NULL_VAL) {
    return sqlite3_bind_null(stmt, idx);
  }
  if (val->type_val == NUMBER) {
    double d = ((Number*)val)->number;
    sqlite3_int64 iv = (sqlite3_int64)d;
    /*
     * Bind exact whole numbers as 64-bit integers so SQLite integer primary
     * keys and `INTEGER` affinity columns receive an integer rather than float.
     */
    if ((double)iv == d) {
      return sqlite3_bind_int64(stmt, idx, iv);
    }
    return sqlite3_bind_double(stmt, idx, d);
  }
  if (val->type_val == BOOLEAN) {
    return sqlite3_bind_int(stmt, idx, ((Boolean*)val)->boolean ? 1 : 0);
  }
  if (val->type_val == STRING) {
    char *s = ((String*)val)->chars;
    return sqlite3_bind_text(
        stmt, idx, s != NULL ? s : "", -1, SQLITE_TRANSIENT);
  }
  /*
   * Fall back to JSON serialization (`to_chars()`) for containers (`Slice`,
   * `Hash`), popping `val` back off its `self` stack afterwards.
   */
  char *serialized = ((GenericEl*)ch_bind_el(val))->to_chars();
  if (val->bind(NULL) == val) {
    val->bind((GenericEl*)-1);
  }
  return sqlite3_bind_text(
      stmt, idx, serialized != NULL ? serialized : "", -1, SQLITE_TRANSIENT);
}

/*
 * Looks up `key` in `h` without leaving `h` or the returned element on their
 * `self` stacks, so internal parameter binding does not disturb caller chains.
 */
static GenericEl* hash_lookup_raw(Hash *h, char *key) {
  GenericEl *val = $(h)->get(key);
  if (val != NULL) {
    val->bind((GenericEl*)-1);
  }
  if (h->bind(NULL) == h) {
    h->bind((GenericEl*)-1);
  }
  return val;
}

static int bind_statement_params(sqlite3_stmt *stmt, GenericEl *params) {
  if (params == NULL) {
    return SQLITE_OK;
  }
  if (params->type_val == SLICE) {
    /*
     * Positional binding: map 0-based `Slice` elements to 1-based SQLite `?`
     * parameter slots.
     */
    Slice *slice = (Slice*)params;
    int count = sqlite3_bind_parameter_count(stmt);
    int i;
    for (i = 0; i < slice->length && i < count; i++) {
      int rc = bind_single_value(stmt, i + 1, slice->elements[i]);
      if (rc != SQLITE_OK) {
        return rc;
      }
    }
    return SQLITE_OK;
  }
  if (params->type_val == HASH) {
    /*
     * Named binding: inspect each 1-based parameter name in the prepared
     * statement (`:key`, `@key`, or `$key`), strip its prefix character, and
     * bind the corresponding entry from `hash` (or `NULL` if absent).
     */
    Hash *hash = (Hash*)params;
    int count = sqlite3_bind_parameter_count(stmt);
    int idx;
    for (idx = 1; idx <= count; idx++) {
      const char *name = sqlite3_bind_parameter_name(stmt, idx);
      if (name != NULL &&
          (name[0] == ':' || name[0] == '@' || name[0] == '$')) {
        GenericEl *val = hash_lookup_raw(hash, (char*)(name + 1));
        int rc = bind_single_value(stmt, idx, val);
        if (rc != SQLITE_OK) {
          return rc;
        }
      }
    }
    return SQLITE_OK;
  }
  return SQLITE_MISUSE;
}

static Hash* row_to_hash(sqlite3_stmt *stmt) {
  int col_count = sqlite3_column_count(stmt);
  Hash *row = create(Hash, col_count > 0 ? col_count : 4);
  int c;
  /*
   * Inspect each column's runtime SQLite storage class in the current row and
   * wrap it in the matching fluent value type (`Number`, `Null`, or `String`).
   */
  for (c = 0; c < col_count; c++) {
    const char *name = sqlite3_column_name(stmt, c);
    char *key = (char*)(name != NULL ? name : "");
    int col_type = sqlite3_column_type(stmt, c);
    if (col_type == SQLITE_INTEGER) {
      double v = (double)sqlite3_column_int64(stmt, c);
      $(row)->put(key, create(Number, v));
    } else if (col_type == SQLITE_FLOAT) {
      double v = sqlite3_column_double(stmt, c);
      $(row)->put(key, create(Number, v));
    } else if (col_type == SQLITE_NULL) {
      $(row)->put(key, create(Null));
    } else {
      const unsigned char *txt = sqlite3_column_text(stmt, c);
      char *s = (char*)(txt != NULL ? (const char*)txt : "");
      $(row)->put(key, create(String, s));
    }
  }
  return row;
}

/**
 * Db operational functions.
 */

static Db* params(void *slice_or_hash) {
  Db *db = slf;
  claim_element(slice_or_hash, db);
  clear_bound_params(db);
  db->bound_params = (GenericEl*)slice_or_hash;
  return $(db);
}

static Db* exec_sql(char *sql) {
  Db *db = slf;
  if (db->handle == NULL || sql == NULL) {
    clear_bound_params(db);
    return $(db);
  }
  /*
   * Advance `tail` across the input string so semicolon-separated SQL
   * statements execute sequentially in a single `exec()` call.
   */
  const char *tail = sql;
  while (*tail != '\0') {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, tail, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
      set_error(db, sqlite3_errmsg(db->handle));
      break;
    }
    /* Trailing whitespace/comments after the last `;` yield `stmt == NULL`. */
    if (stmt == NULL) {
      continue;
    }
    /* Bind any parameters staged by a preceding `params()` call. */
    if (db->bound_params != NULL) {
      rc = bind_statement_params(stmt, db->bound_params);
      if (rc != SQLITE_OK) {
        set_error(db, sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        break;
      }
    }
    /* Step the statement to completion, discarding any intermediate rows. */
    do {
      rc = sqlite3_step(stmt);
    } while (rc == SQLITE_ROW);
    if (rc != SQLITE_DONE) {
      set_error(db, sqlite3_errmsg(db->handle));
      sqlite3_finalize(stmt);
      break;
    }
    sqlite3_finalize(stmt);
  }
  /* Parameter bindings are single-use; release them before returning. */
  clear_bound_params(db);
  return $(db);
}

static Db* insert_row(char *table, Hash *row) {
  Db *db = slf;
  claim_element(row, db);
  if (db->handle == NULL || table == NULL || row == NULL ||
      row->key_slice->length == 0) {
    release(row);
    return $(db);
  }

  /*
   * Phase 1: dynamically build `INSERT INTO <table> (k1, ...) VALUES (?, ...)`
   * in a scratch `String` detached from the `String` `self` stack.
   */
  String *sql = create(String, "INSERT INTO ")->$$;
  sql->bind((GenericEl*)-1);
  $(sql)->append(table)->append(" (");
  int i;
  for (i = 0; i < row->key_slice->length; i++) {
    if (i > 0) {
      $(sql)->append(", ");
    }
    char *k = ((String*)row->key_slice->elements[i])->chars;
    $(sql)->append(k);
  }
  $(sql)->append(") VALUES (");
  for (i = 0; i < row->key_slice->length; i++) {
    if (i > 0) {
      $(sql)->append(", ");
    }
    $(sql)->append("?");
  }
  $(sql)->append(")");
  sql->bind((GenericEl*)-1);

  /* Phase 2: compile the parameterized `INSERT` statement and free `sql`. */
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db->handle, sql->chars, -1, &stmt, NULL);
  release(sql);
  if (rc != SQLITE_OK || stmt == NULL) {
    set_error(db, sqlite3_errmsg(db->handle));
    release(row);
    return $(db);
  }

  /*
   * Phase 3: bind each value from `row` in the same `key_slice` order used to
   * build the column and `?` placeholder lists above.
   */
  for (i = 0; i < row->key_slice->length; i++) {
    char *k = ((String*)row->key_slice->elements[i])->chars;
    GenericEl *val = hash_lookup_raw(row, k);
    rc = bind_single_value(stmt, i + 1, val);
    if (rc != SQLITE_OK) {
      set_error(db, sqlite3_errmsg(db->handle));
      sqlite3_finalize(stmt);
      release(row);
      return $(db);
    }
  }

  /* Phase 4: run the `INSERT`, finalize the statement, and release `row`. */
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    set_error(db, sqlite3_errmsg(db->handle));
  }
  sqlite3_finalize(stmt);
  release(row);
  return $(db);
}

static StatusOr* query_sql(char *sql) {
  Db *db = slf;
  /*
   * If a prior `exec()` or `insert()` in the same chain failed, surface that
   * deferred error immediately and clear `last_err`.
   */
  if (db->last_err != NULL) {
    clear_bound_params(db);
    StatusOr *err_res = create(StatusOr, NULL, db->last_err->chars);
    set_error(db, NULL);
    return err_res;
  }
  if (db->handle == NULL || sql == NULL) {
    clear_bound_params(db);
    return create(StatusOr, NULL, "Invalid database handle or SQL query");
  }

  /* Compile the `SELECT` statement. */
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK || stmt == NULL) {
    const char *err = sqlite3_errmsg(db->handle);
    clear_bound_params(db);
    return create(StatusOr, NULL, (char*)err);
  }

  /* Bind and consume any positional (`Slice`) or named (`Hash`) parameters. */
  if (db->bound_params != NULL) {
    rc = bind_statement_params(stmt, db->bound_params);
    clear_bound_params(db);
    if (rc != SQLITE_OK) {
      const char *err = sqlite3_errmsg(db->handle);
      sqlite3_finalize(stmt);
      return create(StatusOr, NULL, (char*)err);
    }
  }

  /* Step through all result rows, converting each row into a `Hash`. */
  Slice *rows = create(Slice, 4);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    Hash *row = row_to_hash(stmt);
    $(rows)->add(row);
  }

  /* If stepping stopped due to an error rather than `SQLITE_DONE`, clean up. */
  if (rc != SQLITE_DONE) {
    const char *err = sqlite3_errmsg(db->handle);
    release(rows);
    sqlite3_finalize(stmt);
    return create(StatusOr, NULL, (char*)err);
  }

  sqlite3_finalize(stmt);
  return create(StatusOr, rows, NULL);
}

static StatusOr* query_one_sql(char *sql) {
  Db *db = slf;
  /*
   * Surface and clear any deferred error recorded by an earlier `exec()` or
   * `insert()` in the same fluent chain.
   */
  if (db->last_err != NULL) {
    clear_bound_params(db);
    StatusOr *err_res = create(StatusOr, NULL, db->last_err->chars);
    set_error(db, NULL);
    return err_res;
  }
  if (db->handle == NULL || sql == NULL) {
    clear_bound_params(db);
    return create(StatusOr, NULL, "Invalid database handle or SQL query");
  }

  /* Compile the `SELECT` statement. */
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK || stmt == NULL) {
    const char *err = sqlite3_errmsg(db->handle);
    clear_bound_params(db);
    return create(StatusOr, NULL, (char*)err);
  }

  /* Bind and consume any positional (`Slice`) or named (`Hash`) parameters. */
  if (db->bound_params != NULL) {
    rc = bind_statement_params(stmt, db->bound_params);
    clear_bound_params(db);
    if (rc != SQLITE_OK) {
      const char *err = sqlite3_errmsg(db->handle);
      sqlite3_finalize(stmt);
      return create(StatusOr, NULL, (char*)err);
    }
  }

  /*
   * Step the query once:
   * - `SQLITE_ROW`: convert the first matching row into a `Hash` and return it.
   * - `SQLITE_DONE`: the query succeeded but matched 0 rows; return a not-found
   *   error `StatusOr` (which `Server` maps to HTTP 404).
   * - Otherwise: return the underlying SQLite execution error.
   */
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    Hash *row = row_to_hash(stmt);
    sqlite3_finalize(stmt);
    return create(StatusOr, row, NULL);
  }
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return create(StatusOr, NULL, "No rows returned");
  }

  const char *err = sqlite3_errmsg(db->handle);
  sqlite3_finalize(stmt);
  return create(StatusOr, NULL, (char*)err);
}

static char* to_chars(void) {
  Db *db = slf;
  if (db->last_err != NULL) {
    return db->last_err->chars;
  }
  return "<Db ok>";
}

/**
 * Constructor.
 */
Db* ch_new_Db(char *path) {
  Db *db = malloc(sizeof(Db));
  db->generic.magic = CH_MAGIC;
  db->generic.type_val = DB;
  db->generic.prev_self = NULL;
  db->generic.next_self = NULL;
  db->generic.refs = 0;
  db->handle = NULL;
  db->bound_params = NULL;
  db->last_err = NULL;

  int rc = sqlite3_open(path != NULL ? path : ":memory:", &db->handle);
  if (rc != SQLITE_OK) {
    set_error(db, db->handle != NULL ? sqlite3_errmsg(db->handle)
                                     : "Failed to open SQLite database");
  }

  db->bind = bind;
  db->generic.bind = (void* (*)(GenericEl*))bind;
  db->generic.save = save;
  db->generic.del = del;
  db->generic.to_chars = to_chars;
  db->exec = exec_sql;
  db->params = params;
  db->insert = insert_row;
  db->query = query_sql;
  db->query_one = query_one_sql;

  self((GenericEl*)db);

  return db;
}
