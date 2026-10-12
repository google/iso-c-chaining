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

#include <stdbool.h>
#include <stddef.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/json.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/test.h"

static void test_status_or_operations(Test *t) {
  StatusOr *ok_saved = create(StatusOr, create(String, "good_val"), NULL)->$$;
  StatusOr *err_saved = create(StatusOr, NULL, "something broke")->$$;
  String *fallback = create(String, "fallback_val")->$$;

  $(t)->section("StatusOr ok, error, value & value_or ownership")
      ->expect_ok(ok_saved)
      ->expect_bool(create(Boolean, $(ok_saved)->ok()), true)
      ->expect_str(create(String, $(ok_saved)->error()), "")
      ->expect_str($(ok_saved)->value(), "good_val")
      ->expect_str(
          $(ok_saved)->value_or(create(String, "unused_temp")),
          "good_val")
      ->expect_str(ok_saved, "good_val")
      ->expect_err(err_saved, "something broke")
      ->expect_bool(create(Boolean, $(err_saved)->ok()), false)
      ->expect_str(create(String, $(err_saved)->error()), "something broke")
      ->expect_str($(err_saved)->value_or(fallback), "fallback_val")
      ->expect_str(create(String, $(err_saved)->chars()), "something broke")
      ->expect_str(
          create(StatusOr, create(Number, 99), NULL)
              ->value_or(create(Number, 0)),
          "99")
      ->expect_str(
          create(StatusOr, NULL, "transient error")
              ->value_or(create(String, "recovered")),
          "recovered");

  release(fallback);
  release(err_saved);
  release(ok_saved);
}

static void test_json_parser(Test *t) {
  Hash *complex_doc = create(Hash, 5)
      ->put("str", create(String, "line1\nline2\t\"quoted\"\\slash"))
      ->put("num", create(Number, -42.5))
      ->put("bool", create(Boolean, true))
      ->put("nothing", create(Null))
      ->put("arr", create(Slice, 3)
          ->add(create(Hash, 1)->put("k", create(String, "v")))
          ->add(create(Slice, 0))
          ->add(create(Hash, 0)))->$$;

  $(t)->section("JSON parser: primitives, escapes, round-trip & error paths")
      /* Root primitives and whitespace */
      ->expect_bool(from_json("  true \n\t "), true)
      ->expect_bool(from_json("false"), false)
      ->expect_str(from_json("null"), "null")
      ->expect_num(from_json("0"), 0)
      ->expect_num(from_json("-123.45"), -123.45)
      ->expect_str(from_json("\"hello world\""), "hello world")
      ->expect_str(from_json("\"\""), "")
      /* Escape sequences */
      ->expect_str(
          from_json("\"\\\"\\\\\\/\\n\\r\\t\""),
          "\"\\/\n\r\t")
      ->expect_num(
          create(Number,
                 as(String, from_json("\"\\b\\f\"")->value())->length()),
          2)
      /* Empty and nested containers + round-trip */
      ->expect_str(from_json(" [ ] "), "[]")
      ->expect_str(from_json(" { } "), "{}")
      ->expect_eq(from_json($(complex_doc)->chars()), complex_doc)
      /* Comprehensive syntax errors */
      ->expect_err(from_json(NULL), "NULL JSON input")
      ->expect_err(from_json(""), "Unexpected end of JSON input")
      ->expect_err(from_json("   \t\r\n  "), "Unexpected end of JSON input")
      ->expect_err(from_json("@"), "Unexpected character")
      ->expect_err(from_json("\"unterminated"), "Unterminated string")
      ->expect_err(from_json("\"trailing\\"), "Unterminated escape sequence")
      ->expect_err(from_json("\"bad\\xescape\""), "Invalid escape sequence")
      ->expect_err(from_json("-"), "Invalid number")
      ->expect_err(from_json("tru"), "Invalid literal (expected 'true')")
      ->expect_err(from_json("fals"), "Invalid literal (expected 'false')")
      ->expect_err(from_json("nul"), "Invalid literal (expected 'null')")
      ->expect_err(from_json("[1, 2,]"), "Trailing comma in array")
      ->expect_err(from_json("[1 2]"), "Expected ',' or ']' in array")
      ->expect_err(from_json("[1, 2"), "Unterminated array")
      ->expect_err(from_json("{123: 1}"), "Expected string key in object")
      ->expect_err(from_json("{\"a\" 1}"), "Expected ':' after object key")
      ->expect_err(from_json("{\"a\": }"), "Unexpected character")
      ->expect_err(from_json("{\"a\": 1,}"), "Trailing comma in object")
      ->expect_err(
          from_json("{\"a\": 1 \"b\": 2}"),
          "Expected ',' or '}' in object")
      ->expect_err(from_json("{\"a\": 1"), "Unterminated object")
      ->expect_err(from_json("true false"), "Unexpected trailing characters");

  release(complex_doc);
}

int main(void) {
  Test *t = create(Test, "JSON Parser & StatusOr Unit Tests")->$$;

  test_status_or_operations(t);
  test_json_parser(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
