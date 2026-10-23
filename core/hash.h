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

#ifndef CHAIN_HASH_H_
#define CHAIN_HASH_H_

#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Hash, HASH, (int))

/*
 * Open-addressed string-keyed hash table supporting heterogeneous `ChGenericEl`
 * values, insertion-order iteration, fluent method chaining, and JSON
 * serialization via `chars()`.
 */
struct HASH {
  ChGenericEl generic;
  ChGenericEl **elements;
  char **bucket_keys;
  int capacity;
  ChSlice *key_slice;
  ChString* str;

  /* Binds this `Hash` as the active instance on the `Hash` `self` stack. */
  ChHash* (*bind)(ChGenericEl*);

  /*
   * Returns the element stored under `key` (bound to its own type's `self`
   * stack for immediate chaining), or `NULL` if `key` is not present.
   */
  ChGenericEl* (*get)(char*);

  /*
   * Associates `key` with `val`, taking ownership of `val` (and releasing any
   * previous value stored under `key`). Returns this `Hash` for chaining.
   */
  ChHash* (*put)(char*, void*);

  /*
   * Returns the `Slice` of keys in insertion order, bound to the `Slice` `self`
   * stack for immediate chaining (e.g., `keys()->length`, `keys()->get(i)`).
   */
  ChSlice* (*keys)(void);

  /*
   * Removes `key` and releases its associated value if present.
   */
  void (*remove)(char*);

  /*
   * Invokes `run(key, val)` for each entry in insertion order. Safe across
   * nested or re-entrant `Hash` operations inside `run`.
   */
  void (*foreach)(void (*run)(char*, ChGenericEl*));
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_HASH_H_ */
