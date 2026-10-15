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

#include <stdio.h>
#include <stdlib.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"

static int double_int(int x) { return x * 2; }
static int is_even_int(int x) { return (x % 2) == 0; }
static int sum_ints(int acc, int x) { return acc + x; }

static char to_upper_ascii(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static double normalize_pct(double v) { return v / 100.0; }

int main(void) {
  /* 1. TypedIntArray: variadic push_n, sort, subarray, map, filter & reduce. */
  TypedIntArray *scores = create(TypedIntArray, 0)
      ->push_n(6, 42, 15, 88, 23, 16, 4)
      ->sort()
      ->$$;

  TypedIntArray *top_doubled = $(scores)
      ->subarray(3, 6)
      ->filter(is_even_int)
      ->map(double_int)
      ->reverse()
      ->$$;

  Slice *boxed_scores = $(scores)->to_slice()->$$;
  int total = $(scores)->reduce(sum_ints, 0);
  puts("Sorted scores:");
  puts($(scores)->chars());
  puts("Top even doubled (reversed):");
  puts($(top_doubled)->chars());
  puts("Boxed as Slice of Numbers:");
  puts($(boxed_scores)->chars());

  /* 2. TypedCharArray: variadic push_n, map & join. */
  TypedCharArray *letters = create(TypedCharArray, 0)
      ->push_n(5, 'c', 'h', 'a', 'i', 'n')
      ->map(to_upper_ascii)
      ->$$;

  String *banner = $(letters)->join("-")->$$;
  puts("Banner:");
  puts($(banner)->chars());
  puts($(letters)->chars());

  /* 3. TypedFloatArray: variadic push_n, map & nesting inside a Hash. */
  TypedFloatArray *weights = create(TypedFloatArray, 0)
      ->push_n(4, 12.5, 25.0, 37.5, 25.0)
      ->map(normalize_pct)
      ->$$;

  Hash *summary = create(Hash, 5)
      ->put("tag", $(letters)->join(""))
      ->put("scores", scores)
      ->put("total", create(Number, total))
      ->put("weights", weights)
      ->$$;

  puts("Summary JSON:");
  puts($(summary)->chars());

  release(summary);
  release(weights);
  release(banner);
  release(letters);
  release(boxed_scores);
  release(top_doubled);
  release(scores);

  return 0;
}
