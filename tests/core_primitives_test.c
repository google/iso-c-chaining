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
#include "core/test.h"

static void test_runtime_and_refcounting(Test *t) {
  /* Given a String saved twice (refs == 2). */
  String *multi_ref = create(String, "shared_ref")->$$;
  $(multi_ref)->$$;
  int refs_before = multi_ref->generic.refs;
  release(multi_ref);
  int refs_after_one_release = multi_ref->generic.refs;

  /* Given a single String shared across both a Slice and a Hash. */
  String *shared_item = create(String, "cross_container")->$$;
  Slice *holder_slice = create(Slice, 2)->add(shared_item)->$$;
  Hash *holder_hash = create(Hash, 2)->put("item", shared_item)->$$;
  int shared_refs_peak = shared_item->generic.refs;
  release(holder_slice);
  int shared_refs_after_slice = shared_item->generic.refs;

  /* Given three Strings on the String self stack, re-binding the bottom one. */
  String *s1 = create(String, "first")->$$;
  String *s2 = create(String, "second")->$$;
  String *s3 = create(String, "third")->$$;
  $(s1)->append("_rebound");

  /* Given a mismatched type or non-GenericEl pointer in update_self_stack. */
  GenericEl *stack_top = (GenericEl*)s1;
  Number *num = create(Number, 7)->$$;
  GenericEl *mismatch =
      update_self_stack(&stack_top, (GenericEl*)num, STRING);
  int raw_int = 12345;
  char raw_str[] = "raw_c_string";
  GenericEl *bad_magic =
      update_self_stack(&stack_top, (GenericEl*)&raw_int, STRING);

  /* Safe null operations should be no-ops. */
  release(NULL);

  /* Verify prefixed Ch* / ch_* API macros and types alongside short names. */
  ChString *prefixed = ch_create(String, "prefixed")->ch_save;
  ch_bind(prefixed)->append("_api");

  $(t)->section("Runtime type system, self stack & reference counting")
      ->expect_num(create(Number, refs_before), 2)
      ->expect_num(create(Number, refs_after_one_release), 1)
      ->expect_str(multi_ref, "shared_ref")
      ->expect_num(create(Number, shared_refs_peak), 3)
      ->expect_num(create(Number, shared_refs_after_slice), 2)
      ->expect_str($(holder_hash)->get("item"), "cross_container")
      ->expect_str(s1, "first_rebound")
      ->expect_str(s2, "second")
      ->expect_str(s3, "third")
      ->expect_bool(create(Boolean, mismatch == NULL), true)
      ->expect_bool(create(Boolean, bad_magic == NULL), true)
      ->expect_bool(create(Boolean, stack_top == (GenericEl*)s1), true)
      ->expect_bool(create(Boolean, is_generic_el(s1)), true)
      ->expect_bool(create(Boolean, is_generic_el(&raw_int)), false)
      ->expect_bool(create(Boolean, is_generic_el(raw_str)), false)
      ->expect_bool(create(Boolean, is_generic_el("")), false)
      ->expect_bool(create(Boolean, is_generic_el(NULL)), false)
      ->expect_bool(create(Boolean, is_type(s1, STRING)), true)
      ->expect_bool(create(Boolean, is_type(s1, NUMBER)), false)
      ->expect_bool(create(Boolean, is_type(&raw_int, NUMBER)), false)
      ->expect_bool(create(Boolean, is_type(raw_str, STRING)), false)
      ->expect_bool(create(Boolean, is_type(NULL, STRING)), false)
      ->expect_bool(create(Boolean, as(String, NULL) == NULL), true)
      ->expect_bool(create(Boolean, ch_bind_el(NULL) == NULL), true)
      ->expect_bool(
          create(Boolean, ch_is_type(prefixed, CH_STRING)),
          true)
      ->expect_str(
          create(String, ch_as(String, prefixed)->ch_chars()),
          "prefixed_api")
      ->expect_ok(ch_from_json("{\"prefixed\":true}"));

  ch_release(prefixed);
  release(num);
  release(s3);
  release(s2);
  release(s1);
  release(holder_hash);
  release(shared_item);
  release(multi_ref);
}

static void test_string_operations(Test *t) {
  String *src = create(String, "Zero,One,Two")->$$;
  String *mutable_str = create(String, "initial_long_value")->$$;
  $(mutable_str)->set("short");
  int short_len = $(mutable_str)->length();
  $(mutable_str)->set("grown_past_original_capacity_value");
  int grown_len = $(mutable_str)->length();

  $(t)->section("String operations, escaping & non-destructive split")
      ->expect_num(create(Number, create(String, "")->length()), 0)
      ->expect_num(create(Number, $(src)->length()), 12)
      ->expect_num(create(Number, short_len), 5)
      ->expect_num(create(Number, grown_len), 34)
      ->expect_str(mutable_str, "grown_past_original_capacity_value")
      ->expect_str(
          create(String, "Hello")->append("")->append(", ")->append("World!"),
          "Hello, World!")
      ->expect_str(
          create(String, "")
              ->append_escaped("quote:\" slash:\\ nl:\n cr:\r tab:\t"),
          "quote:\\\" slash:\\\\ nl:\\n cr:\\r tab:\\t")
      ->expect_eq(
          $(src)->split(","),
          create(Slice, 3)
              ->add(create(String, "Zero"))
              ->add(create(String, "One"))
              ->add(create(String, "Two")))
      ->expect_str(src, "Zero,One,Two")
      ->expect_eq(
          create(String, ",,alpha,,beta,gamma,,")->split(","),
          create(Slice, 3)
              ->add(create(String, "alpha"))
              ->add(create(String, "beta"))
              ->add(create(String, "gamma")))
      ->expect_eq(
          create(String, "one;two,three:four")->split(",;:"),
          create(Slice, 4)
              ->add(create(String, "one"))
              ->add(create(String, "two"))
              ->add(create(String, "three"))
              ->add(create(String, "four")))
      ->expect_eq(
          create(String, "no_delims")->split(","),
          create(Slice, 1)->add(create(String, "no_delims")))
      ->expect_eq(create(String, "")->split(","), create(Slice, 0))
      ->expect_eq(create(String, ",,,")->split(","), create(Slice, 0));

  release(mutable_str);
  release(src);
}

static void test_number_operations(Test *t) {
  Number *num = create(Number, 10.0)->$$;
  $(num)->set(-25.5);
  double after_set = $(num)->value();
  $(num)->from_chars("3.14159265");

  Number *int_from_str = create(Number, 0)->$$;
  $(int_from_str)->from_chars("-1024");

  $(t)->section("Number mutation, parsing & formatting precision")
      ->expect_num(create(Number, 42.5), 42.5)
      ->expect_num(create(Number, after_set), -25.5)
      ->expect_num(num, 3.14159265)
      ->expect_str(num, "3.14159265")
      ->expect_num(int_from_str, -1024)
      ->expect_str(int_from_str, "-1024")
      ->expect_str(create(Number, 0.0), "0")
      ->expect_str(create(Number, -0.0), "0")
      ->expect_str(create(Number, -0.0000000001), "0")
      ->expect_str(create(Number, 1.000000001), "1")
      ->expect_str(create(Number, 1.25000000), "1.25")
      ->expect_str(create(Number, -99.75), "-99.75")
      ->expect_str(create(Number, 1700000000000.0), "1700000000000");

  release(int_from_str);
  release(num);
}

static void test_boolean_and_null(Test *t) {
  Boolean *b = create(Boolean, 42)->$$;
  int initial_val = $(b)->value();
  $(b)->set(0);
  int after_zero = $(b)->value();
  $(b)->set(99);
  int after_nonzero = $(b)->value();

  Boolean *parsed_true = create(Boolean, false)->$$;
  $(parsed_true)->from_chars("true");
  Boolean *parsed_false = create(Boolean, true)->$$;
  $(parsed_false)->from_chars("false");
  Boolean *parsed_null = create(Boolean, true)->$$;
  $(parsed_null)->from_chars(NULL);

  Null *null_el = create(Null)->$$;

  $(t)->section("Boolean & Null operations")
      ->expect_num(create(Number, initial_val), 1)
      ->expect_num(create(Number, after_zero), 0)
      ->expect_num(create(Number, after_nonzero), 1)
      ->expect_bool(create(Boolean, true)->negate(), false)
      ->expect_bool(create(Boolean, false)->negate(), true)
      ->expect_bool(create(Boolean, true)->negate()->negate(), true)
      ->expect_str(create(Boolean, true), "true")
      ->expect_str(create(Boolean, false), "false")
      ->expect_bool(parsed_true, true)
      ->expect_bool(parsed_false, false)
      ->expect_bool(parsed_null, false)
      ->expect_bool(create(Boolean, is_type(null_el, NULL_VAL)), true)
      ->expect_str(null_el, "null");

  release(null_el);
  release(parsed_null);
  release(parsed_false);
  release(parsed_true);
  release(b);
}

static void test_runner_self_checks(Test *t) {
  int non_el = 99;
  Test *inner = create(Test, "Nested Runner")
      ->section("Sub-assertions")
      ->expect_eq(create(Number, 1), create(Number, 1))
      ->expect_str(create(String, "ok"), "ok")
      ->expect_num(create(Number, 2.5), 2.5)
      ->expect_bool(create(Boolean, true), true)
      ->expect_ok(create(StatusOr, create(Null), NULL))
      ->expect_err(create(StatusOr, NULL, "boom"), "boom")
      /* Passing a non-GenericEl pointer records a failure without crashing. */
      ->expect_num(&non_el, 99)->$$;

  int inner_passed = inner->passed;
  int inner_failed = inner->failed;

  $(t)->section("Test runner nesting, counters & serialization")
      ->expect_bool(create(Boolean, is_type(inner, TEST)), true)
      ->expect_str(inner, "Nested Runner")
      ->expect_num(create(Number, inner_passed), 6)
      ->expect_num(create(Number, inner_failed), 1);

  release(inner);
}

int main(void) {
  Test *t = create(Test, "Core Primitives & Runtime Unit Tests")->$$;

  test_runtime_and_refcounting(t);
  test_string_operations(t);
  test_number_operations(t);
  test_boolean_and_null(t);
  test_runner_self_checks(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
