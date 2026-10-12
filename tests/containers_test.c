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

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"
#include "ext/test.h"

static int slice_foreach_sum;
static int hash_foreach_sum;
static int hash_foreach_keys_len;

static void sum_slice_callback(GenericEl *el) {
  /* Create a temporary inner Slice to verify outer Slice iteration is safe. */
  Slice *tmp = create(Slice, 1)->add(create(Number, 1));
  release(tmp);
  if (is_type(el, NUMBER)) {
    slice_foreach_sum += (int)as(Number, el)->value();
  }
}

static void sum_hash_callback(char *key, GenericEl *el) {
  /* Create a temporary inner Hash to verify outer Hash iteration is safe. */
  Hash *tmp = create(Hash, 1)->put("k", create(Number, 1));
  release(tmp);
  hash_foreach_keys_len += (int)strlen(key);
  if (is_type(el, NUMBER)) {
    hash_foreach_sum += (int)as(Number, el)->value();
  }
}

static void test_slice_operations(Test *t) {
  /* Given a Slice starting with capacity 0 that grows dynamically. */
  Slice *grown = create(Slice, 0)->$$;
  int i;
  for (i = 0; i < 12; i++) {
    $(grown)->add(create(Number, i));
  }
  $(grown)->set(0, create(Number, 100));
  $(grown)->set(11, create(Number, 999));

  slice_foreach_sum = 0;
  $(grown)->foreach(sum_slice_callback);

  /* Saved Slice for non-destructive sub_slice and append checks. */
  Slice *saved_slice = create(Slice, 4)
      ->add(create(String, "a"))
      ->add(create(String, "b"))
      ->add(create(String, "c"))
      ->add(create(String, "d"))->$$;
  Slice *tail_slice = create(Slice, 2)
      ->add(create(String, "e"))
      ->add(create(String, "f"))->$$;

  $(t)->section("Slice growth, mutation, sub_slice, join & nesting")
      ->expect_num(create(Number, grown->length), 12)
      ->expect_num($(grown)->get(0), 100)
      ->expect_num($(grown)->get(5), 5)
      ->expect_num($(grown)->get(11), 999)
      ->expect_num(create(Number, slice_foreach_sum), 100 + 55 + 999)
      ->expect_str($(saved_slice)->sub_slice(1, 2)->join("-"), "b-c")
      ->expect_str($(saved_slice)->sub_slice(0, 0)->join("-"), "a")
      ->expect_str($(saved_slice)->join(","), "a,b,c,d")
      ->expect_str(
          create(String, "Zero,One,Two,Three")
              ->split(",")
              ->sub_slice(1, 2)
              ->join("_"),
          "One_Two")
      ->expect_str(
          create(Slice, 1)
              ->add(create(Number, 1))
              ->append(create(Slice, 2)
                  ->add(create(Number, 2))
                  ->add(create(Number, 3))),
          "[1,2,3]")
      ->expect_str(
          create(Slice, 2)
              ->add(create(String, "x"))
              ->append(tail_slice)
              ->append(NULL)
              ->join("::"),
          "x::e::f")
      ->expect_str(create(Slice, 0)->join(","), "")
      ->expect_str(create(Slice, 0), "[]")
      ->expect_str(
          create(Slice, 4)
              ->add(create(String, "a\"b"))
              ->add(create(Number, 7))
              ->add(create(Boolean, false))
              ->add(create(Null)),
          "[\"a\\\"b\",7,false,null]")
      ->expect_str(
          create(Slice, 2)
              ->add(create(Slice, 2)
                  ->add(create(Slice, 2)
                      ->add(create(Number, 1))
                      ->add(create(Number, 2)))
                  ->add(create(Slice, 1)
                      ->add(create(Number, 3))))
              ->add(create(Slice, 0)),
          "[[[1,2],[3]],[]]");

  release(tail_slice);
  release(saved_slice);
  release(grown);
}

static void test_hash_operations(Test *t) {
  /*
   * Given a Hash with initial capacity 1, inserting 20 keys forces multiple
   * resize_hash() table doublings and exercises linear probing.
   */
  Hash *many = create(Hash, 1)->$$;
  char key_buf[16];
  int i;
  for (i = 0; i < 20; i++) {
    snprintf(key_buf, sizeof(key_buf), "key_%d", i);
    $(many)->put(key_buf, create(Number, i * 10));
  }
  /* Overwrite an existing key and remove several keys (including missing). */
  $(many)->put("key_5", create(Number, 555));
  $(many)->remove("key_0");
  $(many)->remove("key_10");
  $(many)->remove("key_19");
  $(many)->remove("non_existent_key");

  /* Given a small Hash for key order, overwrite, and foreach verification. */
  Hash *ord = create(Hash, 2)
      ->put("first", create(Number, 10))
      ->put("second", create(Number, 20))
      ->put("third", create(Number, 30))
      ->put("second", create(Number, 200))->$$;

  hash_foreach_sum = 0;
  hash_foreach_keys_len = 0;
  $(ord)->foreach(sum_hash_callback);

  $(ord)->remove("second");

  $(t)->section("Hash resizing, probing, overwrite, remove & deep nesting")
      ->expect_num(create(Number, $(many)->keys()->length), 17)
      ->expect_bool(create(Boolean, $(many)->get("key_0") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("key_10") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("key_19") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("missing") == NULL), true)
      ->expect_num($(many)->get("key_1"), 10)
      ->expect_num($(many)->get("key_5"), 555)
      ->expect_num($(many)->get("key_18"), 180)
      ->expect_num(create(Number, hash_foreach_sum), 240)
      ->expect_num(create(Number, hash_foreach_keys_len), 5 + 6 + 5)
      ->expect_str($(ord)->keys()->join(","), "first,third")
      ->expect_str(ord, "{\"first\":10,\"third\":30}")
      ->expect_str(
          create(Hash, 2)
              ->put("a", create(Number, 1))
              ->put("b", create(Slice, 2)
                  ->add(create(Boolean, true))
                  ->add(create(Null))),
          "{\"a\":1,\"b\":[true,null]}")
      ->expect_str(
          create(Hash, 2)
              ->put("k\"ey", create(String, "val\nue"))
              ->put("nested", create(Hash, 1)
                  ->put("inner", create(Hash, 1)
                      ->put("leaf", create(Boolean, true)))),
          "{\"k\\\"ey\":\"val\\nue\",\"nested\":{\"inner\":{\"leaf\":true}}}");

  release(ord);
  release(many);
}

int main(void) {
  Test *t = create(Test, "Containers (Slice & Hash) Unit Tests")->$$;

  test_slice_operations(t);
  test_hash_operations(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
