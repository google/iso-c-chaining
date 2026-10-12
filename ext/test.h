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

#ifndef CHAIN_TEST_H_
#define CHAIN_TEST_H_

#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Test, TEST, char*)

/*
 * Fluent unit-testing runner and assertion element. Automatically claims and
 * releases temporary `ChGenericEl*` values passed to assertion methods so unit
 * tests can chain inline expressions without manual `->$$` or `release()`
 * calls, while preserving caller-saved (`refs > 0`) fixtures.
 */
struct TEST {
  ChGenericEl generic;
  ChString *suite_name;
  ChString *current_section;
  int passed;
  int failed;

  /* Re-binds this `Test` instance as the active receiver on `self`. */
  ChTest* (*bind)(ChGenericEl*);

  /*
   * Sets the active test section label used in failure diagnostics and status
   * output, and returns this `Test` for chaining.
   */
  ChTest* (*section)(char*);

  /*
   * Asserts that `actual` and `expected` have matching runtime `ChType` tags
   * and serialized `to_chars()` representations (automatically unwrapping any
   * `StatusOr` argument). Releases unowned temporaries and returns this `Test`.
   */
  ChTest* (*expect_eq)(void*, void*);

  /*
   * Asserts that `actual->to_chars()` (or its unwrapped `StatusOr` value)
   * equals the C string `expected`. Releases `actual` if unowned and returns
   * this `Test`.
   */
  ChTest* (*expect_str)(void*, char*);

  /*
   * Asserts that `actual` (or its unwrapped `StatusOr` value) is a `Number`
   * whose value equals `expected`. Releases `actual` if unowned and returns
   * this `Test`.
   */
  ChTest* (*expect_num)(void*, double);

  /*
   * Asserts that `actual` (or its unwrapped `StatusOr` value) is a `Boolean`
   * whose truth value matches `expected` (non-zero vs 0). Releases `actual` if
   * unowned and returns this `Test`.
   */
  ChTest* (*expect_bool)(void*, int);

  /*
   * Asserts that `status_or` is non-NULL and `ok()` is non-zero. Releases
   * `status_or` if unowned and returns this `Test`.
   */
  ChTest* (*expect_ok)(ChStatusOr*);

  /*
   * Asserts that `status_or` is a failed `StatusOr` (`!ok()`) and, when
   * `expected_substr` is non-NULL, that `error()` contains `expected_substr`.
   * Releases `status_or` if unowned and returns this `Test`.
   */
  ChTest* (*expect_err)(ChStatusOr*, char*);

  /*
   * Prints the pass/fail summary for the suite, releases this `Test` instance
   * if it is unowned (`refs == 0`), and returns `0` when all assertions passed
   * or `1` if any assertion failed.
   */
  int (*summary)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_TEST_H_ */
