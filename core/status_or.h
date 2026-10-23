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

#ifndef CHAIN_STATUS_OR_H_
#define CHAIN_STATUS_OR_H_

#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(StatusOr, STATUS_OR, (void*, char*))

/*
 * Result wrapper holding either a non-NULL `ChGenericEl*` value on success or
 * an error message `String*` on failure, enabling fluent unwrapping (`value()`,
 * `value_or()`) without out-parameters.
 */
struct STATUS_OR {
  ChGenericEl generic;
  ChGenericEl *val;
  ChString *err;

  /* Binds this `StatusOr` as the active instance on the `StatusOr` stack. */
  ChStatusOr* (*bind)(ChGenericEl*);

  /* Returns `1` if the operation succeeded (`err == NULL && val != NULL`). */
  int (*ok)(void);

  /*
   * Returns the wrapped `ChGenericEl*` (bound to its own type's `self` stack)
   * or aborts with the error message if `!ok()`. If this `StatusOr` is an
   * unowned temporary (`refs == 0`), transfers ownership of the inner value to
   * the caller and releases the `StatusOr` wrapper automatically.
   */
  ChGenericEl* (*value)(void);

  /*
   * Returns `value()` if `ok()`, or binds and returns `fallback` on error
   * (automatically releasing this `StatusOr` if `refs == 0`).
   */
  ChGenericEl* (*value_or)(void*);

  /* Returns the error message string, or `""` if `ok()`. */
  char* (*error)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_STATUS_OR_H_ */
