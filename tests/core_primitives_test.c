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
#include "core/box.h"
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

typedef struct {
  int id;
  int score;
  int *freed_counter;
} BoxDemoItem;

use_box(BoxDemoItem)

static void free_box_demo_item(void *raw) {
  BoxDemoItem *item = (BoxDemoItem*)raw;
  if (item != NULL) {
    if (item->freed_counter != NULL) {
      (*item->freed_counter)++;
    }
    free(item);
  }
}

static BoxDemoItem* make_box_demo_item(int id, int score, int *freed_counter) {
  BoxDemoItem *item = malloc(sizeof(BoxDemoItem));
  item->id = id;
  item->score = score;
  item->freed_counter = freed_counter;
  return item;
}

#ifdef CH_HAS_SCOPED_
static BoxDemoItemBox* build_scoped_item(int id, int score, int *freed_counter,
                                         int fail_early) {
  scoped BoxDemoItemBox *tmp =
      create(BoxDemoItemBox,
             make_box_demo_item(id, score, freed_counter),
             free_box_demo_item)->$$;
  if (fail_early) {
    return NULL;
  }
  return move_ref(tmp);
}
#endif

static void test_box_and_smart_pointers(Test *t) {
  int freed = 0;

  /* Untyped Box + implicit void* conversion & box_of / box_as dereference. */
  Box *raw_box =
      create(Box, make_box_demo_item(1, 10, &freed), free_box_demo_item)->$$;
  BoxDemoItem *local_ptr = raw_box->ptr;
  local_ptr->score += 5;

  Box *tagged_box =
      box_of(BoxDemoItem, make_box_demo_item(2, 20, &freed), free_box_demo_item)
          ->$$;
  box_as(BoxDemoItem, tagged_box)->score += 22;

  /* Shared ownership across Hash + Slice with single destruction. */
  BoxDemoItemBox *typed_box =
      create(BoxDemoItemBox,
             make_box_demo_item(3, 30, &freed),
             free_box_demo_item)->$$;
  $(typed_box)->get()->score += 70;

  Hash *h = create(Hash, 4)
      ->put("raw", raw_box)
      ->put("tagged", tagged_box)
      ->put("typed", typed_box)
      ->$$;
  Slice *s = create(Slice, 2)->add(typed_box)->$$;

  int typed_refs_peak = typed_box->generic.refs;
  int h_score = box_as(BoxDemoItem, $(h)->get("typed"))->score;
  void *missing = box_as(BoxDemoItem, $(h)->get("missing"));

  release(typed_box);
  release(tagged_box);
  release(raw_box);
  int freed_before_containers = freed;

  release(s);
  int freed_after_slice = freed;

  release(h);
  int freed_after_hash = freed;

  /* reset() replaces old value immediately; take() detaches without dtor. */
  BoxDemoItemBox *swap =
      create(BoxDemoItemBox,
             make_box_demo_item(4, 40, &freed),
             free_box_demo_item)->$$;
  $(swap)->reset(make_box_demo_item(5, 50, &freed), free_box_demo_item);
  int freed_after_reset = freed;
  int reset_id = $(swap)->get()->id;
  BoxDemoItem *taken = $(swap)->take();
  int after_take_is_null = ($(swap)->get() == NULL);
  release(swap);
  int freed_after_taken_release = freed;
  free_box_demo_item(taken);
  int freed_after_manual = freed;

  /* Temporary take() cleans up the Box shell cleanly. */
  BoxDemoItem *temp_taken = box_of(BoxDemoItem,
      make_box_demo_item(6, 60, &freed), free_box_demo_item)->take();
  int temp_taken_score = temp_taken->score;
  free_box_demo_item(temp_taken);

  /* get(), chars(), take(), and box_as() pop their temporary Box self entry. */
  Box *outer_box =
      box_of(BoxDemoItem, make_box_demo_item(10, 100, &freed),
             free_box_demo_item)->$$;
  BoxDemoItemBox *nested_box =
      create(BoxDemoItemBox,
             make_box_demo_item(11, 110, &freed),
             free_box_demo_item)->$$;
  BoxDemoItem *nested_ptr = $(nested_box)->get();
  int get_restored_self = (nested_ptr != NULL && nested_ptr->id == 11 &&
                           outer_box->bind(NULL) == outer_box);
  $(outer_box)->chars();
  int chars_restored_self = (outer_box->bind(NULL) == NULL);
  BoxDemoItem *nested_taken = $(nested_box)->take();
  int take_restored_self = (nested_taken != NULL &&
                            nested_box->bind(NULL) == NULL);
  Box *tagged2 =
      box_of(BoxDemoItem, make_box_demo_item(12, 120, &freed),
             free_box_demo_item)->$$;
  int box_as_restored_self =
      (box_as(BoxDemoItem, tagged2)->id == 12 &&
       tagged2->bind(NULL) == NULL);
  free_box_demo_item(nested_taken);
  release(tagged2);
  release(nested_box);
  release(outer_box);

  /* Prefixed ChBox / ch_* C89 API. */
  ChBox *cbox = ch_new_Box(make_box_demo_item(7, 70, &freed),
                           free_box_demo_item);
  ch_bind(cbox)->generic.save();
  ch_release(cbox);
  int freed_before_scoped = freed;

#ifdef CH_HAS_SCOPED_
  BoxDemoItem *single_eval_taken = $(box_of(BoxDemoItem,
      make_box_demo_item(13, 130, &freed), free_box_demo_item))->take();
  int single_eval_clean = (single_eval_taken != NULL &&
                           single_eval_taken->score == 130 &&
                           ch_box_raw_bind(NULL) == NULL);
  free_box_demo_item(single_eval_taken);

  BoxDemoItemBox *discarded = build_scoped_item(8, 80, &freed, 1);
  int freed_after_scoped_early_return = freed;
  scoped BoxDemoItemBox *kept = build_scoped_item(9, 90, &freed, 0);
  int kept_score = $(kept)->get()->score;
#endif

  $(t)->section("Box smart pointers, box_as, use_box & scoped lifetimes")
      ->expect_num(create(Number, local_ptr == NULL ? 0 : 15), 15)
      ->expect_num(create(Number, h_score), 100)
      ->expect_bool(create(Boolean, missing == NULL), true)
      ->expect_num(create(Number, typed_refs_peak), 3)
      ->expect_num(create(Number, freed_before_containers), 0)
      ->expect_num(create(Number, freed_after_slice), 0)
      ->expect_num(create(Number, freed_after_hash), 3)
      ->expect_num(create(Number, freed_after_reset), 4)
      ->expect_num(create(Number, reset_id), 5)
      ->expect_bool(create(Boolean, after_take_is_null), true)
      ->expect_num(create(Number, freed_after_taken_release), 4)
      ->expect_num(create(Number, freed_after_manual), 5)
      ->expect_num(create(Number, temp_taken_score), 60)
      ->expect_bool(create(Boolean, get_restored_self), true)
      ->expect_bool(create(Boolean, chars_restored_self), true)
      ->expect_bool(create(Boolean, take_restored_self), true)
      ->expect_bool(create(Boolean, box_as_restored_self), true)
      ->expect_num(create(Number, freed_before_scoped), 10)
      ->expect_str(create(Box, NULL, NULL), "Box(NULL)")
      ->expect_str(box_of(BoxDemoItem, NULL, NULL), "Box<BoxDemoItem>(NULL)")
#ifdef CH_HAS_SCOPED_
      ->expect_bool(create(Boolean, single_eval_clean), true)
      ->expect_bool(create(Boolean, discarded == NULL), true)
      ->expect_num(create(Number, freed_after_scoped_early_return), 12)
      ->expect_num(create(Number, kept_score), 90)
#endif
      ;
}

int main(void) {
  Test *t = create(Test, "Core Primitives & Runtime Unit Tests")->$$;

  test_runtime_and_refcounting(t);
  test_string_operations(t);
  test_number_operations(t);
  test_boolean_and_null(t);
  test_runner_self_checks(t);
  test_box_and_smart_pointers(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
