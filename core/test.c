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

#include "core/test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static Test* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Test*)update_self_stack(&top, s, TEST);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Test* bind(GenericEl* test) {
  return self(test);
}

static void del(void* obj) {
  Test* test = (Test*)obj;
  /*
   * `self((GenericEl*)test)` verifies `obj` is a `Test` and pushes it onto the
   * `Test` `self` stack so `slf` resolves to `test` during cleanup.
   */
  if (test != NULL && self((GenericEl*)test) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      release(slf->suite_name);
      release(slf->current_section);
      Test *to_free = slf;
      /* Pop `test` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Internal ownership, unwrapping, and reporting helpers.
 */

/*
 * Claims a temporary reference on `el` for the duration of a single assertion.
 * If `el` is an unowned inline expression (`refs == 0`), moves `el` to the top
 * of its type's `self` stack and pops it (`-1` sentinel) so `release(el)` at
 * the end of the assertion frees `el` cleanly without leaving a dangling stack
 * entry, even when two arguments of the same type are passed to `expect_eq()`.
 */
static void claim_arg(GenericEl *el) {
  if (ch_is_generic_el(el)) {
    if (el->refs == 0) {
      el->bind(el);
      el->bind((GenericEl*)-1);
    }
    el->refs++;
  }
}

/*
 * Serializes `el` via `to_chars()` and immediately pops `el` off its `self`
 * stack so inspecting `el` does not disturb any surrounding fluent chains.
 */
static char* inspect_chars(GenericEl *el) {
  if (el == NULL) {
    return "<NULL>";
  }
  char *s = ((GenericEl*)ch_bind_el(el))->to_chars();
  if (el->bind(NULL) == el) {
    el->bind((GenericEl*)-1);
  }
  return s != NULL ? s : "";
}

static void record_pass(Test *test) {
  test->passed++;
}

static void print_failure_prefix(Test *test) {
  const char *suite = test->suite_name->chars;
  const char *sec = test->current_section->chars;
  if (sec != NULL && sec[0] != '\0') {
    fprintf(stderr, "[FAIL] %s :: %s: ", suite, sec);
  } else {
    fprintf(stderr, "[FAIL] %s: ", suite);
  }
}

/*
 * Unwraps a `StatusOr` argument if `el` has type `STATUS_OR`:
 * - If `el` is NULL or lacks `CH_MAGIC`, logs a failure and returns `NULL`.
 * - If `el` is a failed `StatusOr`, logs a failure with its error message and
 *   returns `NULL`.
 * - If `el` is an `ok()` `StatusOr`, returns its inner `val` pointer without
 *   detaching it so releasing the outer `StatusOr` still cleans up `val`.
 * - For any non-`StatusOr` element, returns `el` unchanged.
 */
static GenericEl* unwrap_arg(Test *test, GenericEl *el, const char *label) {
  if (el == NULL) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "%s is NULL\n", label);
    return NULL;
  }
  if (!ch_is_generic_el(el)) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "%s (%p) is not a valid ChGenericEl\n", label, (void*)el);
    return NULL;
  }
  if (el->type_val == STATUS_OR) {
    StatusOr *so = (StatusOr*)el;
    if (so->err != NULL || so->val == NULL) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr, "%s returned unexpected StatusOr error: \"%s\"\n",
              label, so->err != NULL ? so->err->chars : "");
      return NULL;
    }
    return so->val;
  }
  return el;
}

/**
 * Test operational functions.
 */

static Test* section(char *name) {
  Test *test = slf;
  $(test->current_section)->set(name != NULL ? name : "");
  test->current_section->bind((GenericEl*)-1);
  return $(test);
}

static Test* expect_eq(void *actual_ptr, void *expected_ptr) {
  Test *test = slf;
  GenericEl *raw_actual = (GenericEl*)actual_ptr;
  GenericEl *raw_expected = (GenericEl*)expected_ptr;

  /*
   * Step 1: claim both arguments so unowned inline temporaries (`refs == 0`)
   * are freed by `release()` at the end of this assertion while saved objects
   * (`refs > 0`) retain their caller reference count.
   */
  claim_arg(raw_actual);
  claim_arg(raw_expected);

  /* Step 2: unwrap any `StatusOr` wrapper on either side before comparing. */
  GenericEl *actual = unwrap_arg(test, raw_actual, "actual");
  GenericEl *expected = NULL;
  if (actual != NULL) {
    expected = unwrap_arg(test, raw_expected, "expected");
  }

  /*
   * Step 3: verify that both unwrapped elements share the same runtime
   * `ChType` tag and serialize to identical strings via `to_chars()`.
   */
  if (actual != NULL && expected != NULL) {
    char *act_str = inspect_chars(actual);
    char *exp_str = inspect_chars(expected);
    if (actual->type_val != expected->type_val) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr,
              "type mismatch (actual \"%s\", expected \"%s\")\n",
              act_str, exp_str);
    } else if (strcmp(act_str, exp_str) != 0) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr, "expected %s, got %s\n", exp_str, act_str);
    } else {
      record_pass(test);
    }
  }

  if (ch_is_generic_el(raw_actual)) {
    release(raw_actual);
  }
  if (ch_is_generic_el(raw_expected)) {
    release(raw_expected);
  }
  return $(test);
}

static Test* expect_str(void *actual_ptr, char *expected) {
  Test *test = slf;
  GenericEl *raw_actual = (GenericEl*)actual_ptr;
  claim_arg(raw_actual);

  GenericEl *actual = unwrap_arg(test, raw_actual, "actual");
  if (actual != NULL) {
    char *act_str = inspect_chars(actual);
    const char *exp_str = expected != NULL ? expected : "";
    if (strcmp(act_str, exp_str) != 0) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr, "expected \"%s\", got \"%s\"\n", exp_str, act_str);
    } else {
      record_pass(test);
    }
  }

  if (ch_is_generic_el(raw_actual)) {
    release(raw_actual);
  }
  return $(test);
}

static Test* expect_num(void *actual_ptr, double expected) {
  Test *test = slf;
  GenericEl *raw_actual = (GenericEl*)actual_ptr;
  claim_arg(raw_actual);

  GenericEl *actual = unwrap_arg(test, raw_actual, "actual");
  if (actual != NULL) {
    if (actual->type_val != NUMBER) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr, "expected Number(%g), got non-Number \"%s\"\n",
              expected, inspect_chars(actual));
    } else {
      double val = ((Number*)actual)->number;
      if (fabs(val - expected) > 1e-9) {
        test->failed++;
        print_failure_prefix(test);
        fprintf(stderr, "expected %g, got %g\n", expected, val);
      } else {
        record_pass(test);
      }
    }
  }

  if (ch_is_generic_el(raw_actual)) {
    release(raw_actual);
  }
  return $(test);
}

static Test* expect_bool(void *actual_ptr, int expected) {
  Test *test = slf;
  GenericEl *raw_actual = (GenericEl*)actual_ptr;
  claim_arg(raw_actual);

  GenericEl *actual = unwrap_arg(test, raw_actual, "actual");
  if (actual != NULL) {
    if (actual->type_val != BOOLEAN) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr, "expected Boolean(%s), got non-Boolean \"%s\"\n",
              expected ? "true" : "false", inspect_chars(actual));
    } else {
      int actual_bit = ((Boolean*)actual)->boolean ? 1 : 0;
      int expected_bit = expected ? 1 : 0;
      if (actual_bit != expected_bit) {
        test->failed++;
        print_failure_prefix(test);
        fprintf(stderr, "expected %s, got %s\n",
                expected_bit ? "true" : "false",
                actual_bit ? "true" : "false");
      } else {
        record_pass(test);
      }
    }
  }

  if (ch_is_generic_el(raw_actual)) {
    release(raw_actual);
  }
  return $(test);
}

static Test* expect_ok(StatusOr *status_or) {
  Test *test = slf;
  claim_arg((GenericEl*)status_or);

  if (status_or == NULL) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "expected ok StatusOr, got NULL\n");
  } else if (status_or->err != NULL || status_or->val == NULL) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "expected ok StatusOr, got error: \"%s\"\n",
            status_or->err != NULL ? status_or->err->chars : "");
  } else {
    record_pass(test);
  }

  release(status_or);
  return $(test);
}

static Test* expect_err(StatusOr *status_or, char *expected_substr) {
  Test *test = slf;
  claim_arg((GenericEl*)status_or);

  if (status_or == NULL) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "expected error StatusOr, got NULL\n");
  } else if (status_or->err == NULL && status_or->val != NULL) {
    test->failed++;
    print_failure_prefix(test);
    fprintf(stderr, "expected error StatusOr, got ok value: %s\n",
            inspect_chars(status_or->val));
  } else {
    const char *err_msg = status_or->err != NULL ? status_or->err->chars : "";
    if (expected_substr != NULL && strstr(err_msg, expected_substr) == NULL) {
      test->failed++;
      print_failure_prefix(test);
      fprintf(stderr,
              "expected error containing \"%s\", got \"%s\"\n",
              expected_substr, err_msg);
    } else {
      record_pass(test);
    }
  }

  release(status_or);
  return $(test);
}

static int summary(void) {
  Test *test = slf;
  int failed = test->failed;
  int total = test->passed + test->failed;
  printf("[%s] %s: %d/%d assertions passed\n",
         failed == 0 ? "PASS" : "FAIL",
         test->suite_name->chars,
         test->passed,
         total);
  fflush(stdout);

  /*
   * If the `Test` runner was chained inline without `->$$` (`refs == 0`),
   * release it automatically so a single-expression `main()` leaves zero leaks.
   */
  if (test->generic.refs == 0) {
    release(test);
  }
  return failed == 0 ? 0 : 1;
}

static char* to_chars(void) {
  return slf->suite_name->chars;
}

/**
 * Constructor.
 */
Test* ch_new_Test(char *suite_name) {
  Test *test = malloc(sizeof(Test));
  test->generic.magic = CH_MAGIC;
  test->generic.type_val = TEST;
  test->generic.prev_self = NULL;
  test->generic.next_self = NULL;
  test->generic.refs = 0;
  test->passed = 0;
  test->failed = 0;

  /*
   * Pop internal `suite_name` and `current_section` strings off the `String`
   * `self` stack immediately so constructing `Test` does not disturb `String`.
   */
  test->suite_name =
      create(String, suite_name != NULL ? suite_name : "Test Suite")->$$;
  test->suite_name->bind((GenericEl*)-1);
  test->current_section = create(String, "")->$$;
  test->current_section->bind((GenericEl*)-1);

  test->bind = bind;
  test->generic.bind = (void* (*)(GenericEl*))bind;
  test->generic.save = save;
  test->generic.del = del;
  test->generic.to_chars = to_chars;
  test->section = section;
  test->expect_eq = expect_eq;
  test->expect_str = expect_str;
  test->expect_num = expect_num;
  test->expect_bool = expect_bool;
  test->expect_ok = expect_ok;
  test->expect_err = expect_err;
  test->summary = summary;

  self((GenericEl*)test);

  return test;
}
