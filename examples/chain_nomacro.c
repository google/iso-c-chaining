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

/*
 * Fluent chaining in strictly conforming ANSI C89 (ISO C90), with no macros
 * and no short-name aliases. This is the existence proof for the README's
 * central claim: the only requirements are structs of function pointers and
 * `static` storage.
 *
 * Compile this file with the strictest settings your compiler offers, then link
 * it against the library built as usual (the implementation files in `core/`
 * are C99 internally; only the public headers and this client need be C89):
 *
 *     gcc -std=c89 -pedantic -Werror -I. -c examples/chain_nomacro.c
 *     gcc -std=c99 -I. -c core/boolean.c core/hash.c core/number.c \
 *         core/slice.c core/string.c
 *     gcc chain_nomacro.o boolean.o hash.o number.o slice.o string.o \
 *         -lm -o chain_nomacro
 *
 * `CHAIN_NO_SHORT_NAMES` hides the `$`, `$$`, `create()`, and bare `Slice`
 * aliases (which lean on C99 variadic macros and the GNU `$`-in-identifier
 * extension) so that only the `Ch*` / `ch_*` API remains.
 */

#define CHAIN_NO_SHORT_NAMES

#include <stdio.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"

int main(void) {
  ChSlice *mid;
  ChHash *user;

  mid = ch_new_String("One_Two_Three_Four")
      ->append("_Five_Six")
      ->split("_")
      ->sub_slice(1, 4);

  puts(mid->generic.to_chars()); /* ["Two","Three","Four","Five"] */

  ch_release(mid);

  user = ch_new_Hash(3)
      ->put("name", ch_new_String("Ada"))
      ->put("role", ch_new_Hash(2)
          ->put("title", ch_new_String("Engineer"))
          ->put("level", ch_new_Number(5)))
      ->put("active", ch_new_Boolean(1));

  puts(user->generic.to_chars());
  /* {"name":"Ada","role":{"title":"Engineer","level":5},"active":true} */
  ch_release(user);

  return 0;
}
