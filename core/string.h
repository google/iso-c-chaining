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

#ifndef CHAIN_STRING_H_
#define CHAIN_STRING_H_

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(String, STRING, (char*))

#include "core/slice.h"

/*
 * Dynamically resized heap string supporting fluent method chaining, non-
 * destructive tokenization into a `Slice`, and JSON string escaping.
 */
struct STRING {
  ChGenericEl generic;
  /* Binds this `String` as the active instance on the `String` `self` stack. */
  ChString* (*bind)(ChGenericEl*);
  char *chars;
  int size;

  /* Replaces the string contents with `s`, growing the buffer if needed. */
  void (*set)(char*);

  /* Returns the byte length of the null-terminated string. */
  int (*length)(void);

  /*
   * Non-destructively splits the string on any delimiter character in `delim`
   * and returns a new `Slice` of `String` tokens. If called on an unowned
   * temporary (`refs == 0`), automatically releases the temporary `String`.
   */
  ChSlice* (*split)(char*);

  /* Appends `suffix` in place and returns this `String` for chaining. */
  ChString* (*append)(char*);

  /*
   * Appends `suffix` with JSON special characters (`"`, `\`, `\n`, `\r`, `\t`)
   * backslash-escaped, and returns this `String` for chaining.
   */
  ChString* (*append_escaped)(char*);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_STRING_H_ */
