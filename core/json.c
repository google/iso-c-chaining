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

#include "core/json.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Cursor state for the recursive-descent JSON parser. `src` points to the start
 * of the input buffer so byte offsets (`p - src`) can be reported on error.
 */
typedef struct {
  const char *src;
  const char *p;
  char err[128];
} JsonParser;

static GenericEl* parse_value(JsonParser *jp);

/*
 * Records only the first syntax error encountered during recursive descent so
 * outer container parsers (`parse_array`, `parse_object`) do not overwrite the
 * inner root-cause error offset when unwinding.
 */
static void set_error(JsonParser *jp, const char *msg) {
  if (jp->err[0] == '\0') {
    snprintf(jp->err, sizeof(jp->err), "%s at offset %ld",
             msg, (long)(jp->p - jp->src));
  }
}

static void skip_ws(JsonParser *jp) {
  while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r') {
    jp->p++;
  }
}

static char* parse_raw_string(JsonParser *jp) {
  if (*jp->p != '"') {
    set_error(jp, "Expected '\"'");
    return NULL;
  }
  jp->p++;
  /*
   * Unescaping only shrinks or preserves length, so the remaining input length
   * is a safe upper bound for the decoded buffer.
   */
  char *buf = malloc(strlen(jp->p) + 1);
  int len = 0;
  while (*jp->p != '\0' && *jp->p != '"') {
    if (*jp->p == '\\') {
      jp->p++;
      if (*jp->p == '\0') {
        free(buf);
        set_error(jp, "Unterminated escape sequence");
        return NULL;
      }
      /* Decode standard JSON two-character escape sequences. */
      switch (*jp->p) {
        case '"':  buf[len++] = '"';  break;
        case '\\': buf[len++] = '\\'; break;
        case '/':  buf[len++] = '/';  break;
        case 'b':  buf[len++] = '\b'; break;
        case 'f':  buf[len++] = '\f'; break;
        case 'n':  buf[len++] = '\n'; break;
        case 'r':  buf[len++] = '\r'; break;
        case 't':  buf[len++] = '\t'; break;
        default:
          free(buf);
          set_error(jp, "Invalid escape sequence");
          return NULL;
      }
      jp->p++;
    } else {
      buf[len++] = *jp->p++;
    }
  }
  if (*jp->p != '"') {
    free(buf);
    set_error(jp, "Unterminated string");
    return NULL;
  }
  buf[len] = '\0';
  /* Consume the closing quote. */
  jp->p++;
  return buf;
}

static GenericEl* parse_string(JsonParser *jp) {
  char *raw = parse_raw_string(jp);
  if (raw == NULL) {
    return NULL;
  }
  String *s = create(String, raw);
  free(raw);
  return (GenericEl*)s;
}

static GenericEl* parse_number(JsonParser *jp) {
  char *end = NULL;
  double val = strtod(jp->p, &end);
  if (end == jp->p) {
    set_error(jp, "Invalid number");
    return NULL;
  }
  jp->p = end;
  return (GenericEl*)create(Number, val);
}

static GenericEl* parse_array(JsonParser *jp) {
  /* Consume opening `'['` and handle an empty array `'[]'` immediately. */
  jp->p++;
  Slice *slice = create(Slice, 4);
  skip_ws(jp);
  if (*jp->p == ']') {
    jp->p++;
    return (GenericEl*)$(slice);
  }
  while (*jp->p != '\0') {
    skip_ws(jp);
    GenericEl *elem = parse_value(jp);
    if (elem == NULL) {
      /* Releasing `slice` also frees all elements added so far. */
      release(slice);
      return NULL;
    }
    /*
     * Call `slice->add` without `$(slice)` so `elem` stays at `slf` when
     * `claim_element` runs (needed when `elem` is a nested `Slice`).
     */
    slice->add(elem);
    skip_ws(jp);
    if (*jp->p == ',') {
      jp->p++;
      skip_ws(jp);
      if (*jp->p == ']') {
        set_error(jp, "Trailing comma in array");
        release(slice);
        return NULL;
      }
    } else if (*jp->p == ']') {
      jp->p++;
      /*
       * Re-bind `slice` to the top of `self` so `parse_value` returns with the
       * completed container at `slf`, matching `claim_element` expectations in
       * parent containers or `StatusOr`.
       */
      return (GenericEl*)$(slice);
    } else if (*jp->p != '\0') {
      set_error(jp, "Expected ',' or ']' in array");
      release(slice);
      return NULL;
    }
  }
  set_error(jp, "Unterminated array");
  release(slice);
  return NULL;
}

static GenericEl* parse_object(JsonParser *jp) {
  /* Consume opening `'{'` and handle an empty object `'{}'` immediately. */
  jp->p++;
  Hash *hash = create(Hash, 4);
  skip_ws(jp);
  if (*jp->p == '}') {
    jp->p++;
    return (GenericEl*)$(hash);
  }
  while (*jp->p != '\0') {
    /* Parse the quoted property key. */
    skip_ws(jp);
    if (*jp->p != '"') {
      set_error(jp, "Expected string key in object");
      release(hash);
      return NULL;
    }
    char *key = parse_raw_string(jp);
    if (key == NULL) {
      release(hash);
      return NULL;
    }
    /* Expect `':'` between the key and value. */
    skip_ws(jp);
    if (*jp->p != ':') {
      free(key);
      set_error(jp, "Expected ':' after object key");
      release(hash);
      return NULL;
    }
    jp->p++;
    /* Recursively parse the property value. */
    skip_ws(jp);
    GenericEl *val = parse_value(jp);
    if (val == NULL) {
      free(key);
      release(hash);
      return NULL;
    }
    /*
     * Call `hash->put` without `$(hash)` so `val` remains at the top of its
     * type's `self` stack when `claim_element` runs (critical when `val` is a
     * nested `Hash` sharing `hash.c`'s `self` stack).
     */
    hash->put(key, val);
    free(key);
    /* Expect either `','` (followed by another key) or closing `'}'`. */
    skip_ws(jp);
    if (*jp->p == ',') {
      jp->p++;
      skip_ws(jp);
      if (*jp->p == '}') {
        set_error(jp, "Trailing comma in object");
        release(hash);
        return NULL;
      }
    } else if (*jp->p == '}') {
      jp->p++;
      return (GenericEl*)$(hash);
    } else if (*jp->p != '\0') {
      set_error(jp, "Expected ',' or '}' in object");
      release(hash);
      return NULL;
    }
  }
  set_error(jp, "Unterminated object");
  release(hash);
  return NULL;
}

static GenericEl* parse_value(JsonParser *jp) {
  skip_ws(jp);
  if (*jp->p == '\0') {
    set_error(jp, "Unexpected end of JSON input");
    return NULL;
  }
  /* Dispatch on the first non-whitespace token character. */
  switch (*jp->p) {
    case '{':
      return parse_object(jp);
    case '[':
      return parse_array(jp);
    case '"':
      return parse_string(jp);
    case 't':
      if (strncmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        return (GenericEl*)create(Boolean, true);
      }
      set_error(jp, "Invalid literal (expected 'true')");
      return NULL;
    case 'f':
      if (strncmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        return (GenericEl*)create(Boolean, false);
      }
      set_error(jp, "Invalid literal (expected 'false')");
      return NULL;
    case 'n':
      if (strncmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        return (GenericEl*)create(Null);
      }
      set_error(jp, "Invalid literal (expected 'null')");
      return NULL;
    default:
      if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) {
        return parse_number(jp);
      }
      set_error(jp, "Unexpected character");
      return NULL;
  }
}

StatusOr* ch_from_json(char *json) {
  if (json == NULL) {
    return create(StatusOr, NULL, "NULL JSON input");
  }
  JsonParser jp;
  jp.src = json;
  jp.p = json;
  jp.err[0] = '\0';

  GenericEl *val = parse_value(&jp);
  if (val == NULL) {
    return create(StatusOr, NULL, jp.err[0] ? jp.err : "Invalid JSON");
  }
  /* Reject any non-whitespace tokens trailing after the root JSON value. */
  skip_ws(&jp);
  if (*jp.p != '\0') {
    set_error(&jp, "Unexpected trailing characters");
    release(val);
    return create(StatusOr, NULL, jp.err);
  }
  return create(StatusOr, val, NULL);
}
