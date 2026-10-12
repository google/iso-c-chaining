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

#ifndef CHAIN_BOOLEAN_H_
#define CHAIN_BOOLEAN_H_

#include <stdbool.h>

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Boolean, BOOLEAN, int)

/*
 * Boxed boolean value (`0` or `1`) supporting fluent mutation and `"true"` /
 * `"false"` JSON literal serialization.
 */
struct BOOLEAN {
  ChGenericEl generic;
  int boolean;

  /* Binds this `Boolean` as the active instance on the `Boolean` stack. */
  ChBoolean* (*bind)(ChGenericEl*);

  /* Returns `1` for true or `0` for false. */
  int (*value)(void);

  /* Sets the boolean state (`b != 0`) and returns this `Boolean`. */
  ChBoolean* (*set)(int);

  /* Flips the boolean state in place and returns this `Boolean`. */
  ChBoolean* (*negate)(void);

  /* Sets the boolean to `1` if `s` is `"true"`, or `0` otherwise. */
  void (*from_chars)(char*);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_BOOLEAN_H_ */
