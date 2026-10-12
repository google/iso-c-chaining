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

#ifndef CHAIN_SLICE_H_
#define CHAIN_SLICE_H_

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Slice, SLICE, int)

#include "core/string.h"

/*
 * Dynamically resized heterogeneous array of `ChGenericEl*` elements supporting
 * fluent method chaining, slicing, joining, and JSON array serialization.
 */
struct SLICE {
  ChGenericEl generic;
  ChGenericEl **elements;
  int length;
  int capacity;
  ChString* str;

  /* Binds this `Slice` as the active instance on the `Slice` `self` stack. */
  ChSlice* (*bind)(ChGenericEl*);

  /*
   * Returns element `n` (bound to its own type's `self` stack for immediate
   * chaining).
   */
  ChGenericEl* (*get)(int);

  /*
   * Appends an element (taking ownership and growing capacity if needed) and
   * returns this `Slice` for chaining.
   */
  ChSlice* (*add)(void*);

  /*
   * Replaces the element at index `i` (releasing the previous element) and
   * returns this `Slice` for chaining.
   */
  ChSlice* (*set)(int, void*);

  /*
   * Appends all elements from `other` into this `Slice` (sharing element
   * references) and returns this `Slice` for chaining.
   */
  ChSlice* (*append)(ChSlice*);

  /*
   * Returns a new `Slice` containing elements in the inclusive range
   * `[start, end]`, releasing this `Slice` if it was an unowned temporary.
   */
  ChSlice* (*sub_slice)(int, int);

  /*
   * Concatenates the string representations of all elements separated by
   * `delim` and returns a new `String`.
   */
  ChString* (*join)(char*);

  /*
   * Invokes `run(elem)` for each element in order. Safe across nested or
   * re-entrant `Slice` operations inside `run`.
   */
  void (*foreach)(void (*run)(ChGenericEl*));
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_SLICE_H_ */
