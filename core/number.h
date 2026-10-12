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

#ifndef CHAIN_NUMBER_H_
#define CHAIN_NUMBER_H_

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Number, NUMBER, double)

/*
 * Boxed double-precision numeric value supporting fluent chaining and compact
 * integer/decimal formatting via `chars()`.
 */
struct NUMBER {
  ChGenericEl generic;
  double number;
  char chars[64];

  /* Binds this `Number` as the active instance on the `Number` `self` stack. */
  ChNumber* (*bind)(ChGenericEl*);

  /* Returns the underlying `double` value. */
  double (*value)(void);

  /* Replaces the underlying numeric value with `number`. */
  void (*set)(double);

  /* Parses a numeric value from a decimal C string `s`. */
  void (*from_chars)(char *);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_NUMBER_H_ */
