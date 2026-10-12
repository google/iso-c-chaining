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

#include "core/boolean.h"
#include "core/hash.h"
#include "core/json.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

int main(void) {
  Hash *hash = create(Hash, 10)
      ->put("Apple", create(String, "Foo"))
      ->put("Banana", create(Number, 42))
      ->put("Organic", create(Boolean, true))
      ->put("Discount", create(Null))
      ->put("Orange", create(Slice, 3)
          ->add(create(String, "Baz"))
          ->add(create(String, "Quu"))
          ->add(create(Boolean, false))
          ->add(create(Number, 123.456)))
      ->put("Strawberry", create(Hash, 10)
          ->put("Red", create(Number, 67))
          ->put("Green", create(Number, 123))
          ->put("Blue", create(Number, 456)))->$$;

  puts("Result:");
  char *json = $(hash)->chars();
  puts(json);

  Hash *parsed = as(Hash, from_json(json)->value())->$$;
  puts("Parsed Strawberry.Red:");
  puts(as(Hash, $(parsed)->get("Strawberry"))->get("Red")->to_chars());

  StatusOr *bad = from_json("{\"Apple\": \"Foo\", \"Broken\": }")->$$;
  if (!$(bad)->ok()) {
    puts("Parse error:");
    puts($(bad)->error());
  }

  release(bad);
  release(parsed);
  release(hash);

  return 0;
}
