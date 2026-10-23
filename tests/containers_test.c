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
#include "core/typed_array.h"
#include "core/types.h"
#include "core/test.h"

static int slice_foreach_sum;
static int hash_foreach_sum;
static int hash_foreach_keys_len;
static int typed_int_foreach_sum;

static int double_int(int v) {
  return v * 2;
}

static int is_even_int(int v) {
  return (v % 2) == 0;
}

static int sum_int_reduce(int acc, int v) {
  return acc + v;
}

static void sum_typed_int_foreach(int v, int idx) {
  /* Create a temporary inner TypedIntArray to verify outer iteration safety. */
  TypedIntArray *tmp = create(TypedIntArray, 0)->push_n(2, 1, 2);
  release(tmp);
  typed_int_foreach_sum += v + idx;
}

static char rot1_char(char c) {
  return (char)(c + 1);
}

static double scale_float(double v) {
  return v * 2.0;
}

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
  /* Also exercise foreach on an unowned temporary Slice (refs == 0). */
  create(Slice, 2)
      ->add(create(Number, 4))
      ->add(create(Number, 6))
      ->foreach(sum_slice_callback);

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
      ->expect_num(create(Number, slice_foreach_sum), 100 + 55 + 999 + 10)
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
          "[[[1,2],[3]],[]]")
      /*
       * Slice::join() nested inside an outer Slice (non-empty, empty, and
       * saved inner Slices), followed by subsequent .add() calls on the outer
       * Slice.
       */
      ->expect_str(
          create(Slice, 4)
              ->add(create(Slice, 2)
                  ->add(create(String, "a"))
                  ->add(create(String, "b"))
                  ->join("-"))
              ->add(create(Slice, 0)->join("-"))
              ->add($(tail_slice)->join("+"))
              ->add(create(String, "c")),
          "[\"a-b\",\"\",\"e+f\",\"c\"]")
      /*
       * Root Slice -> Hash -> Slice alternating nesting with subsequent .put()
       * on the middle Hash and .add() on the outer Slice.
       */
      ->expect_str(
          create(Slice, 3)
              ->add(create(Hash, 2)
                  ->put("items", create(Slice, 2)
                      ->add(create(Number, 1))
                      ->add(create(Number, 2)))
                  ->put("ok", create(Boolean, true)))
              ->add(create(Hash, 1)
                  ->put("empty", create(Slice, 0)))
              ->add(create(String, "done")),
          "[{\"items\":[1,2],\"ok\":true},{\"empty\":[]},\"done\"]");

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
  /* Also exercise foreach on an unowned temporary Hash (refs == 0). */
  create(Hash, 1)->put("tmp", create(Number, 5))->foreach(sum_hash_callback);

  $(ord)->remove("second");

  Slice *fmt_slice = create(Slice, 2)
      ->add(create(String, "x"))
      ->add(create(Number, 9))->$$;
  String *csv_str = create(String, "p,q,r")->$$;

  $(t)->section("Hash resizing, probing, overwrite, remove & deep nesting")
      ->expect_num(create(Number, $(many)->keys()->length), 17)
      ->expect_bool(create(Boolean, $(many)->get("key_0") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("key_10") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("key_19") == NULL), true)
      ->expect_bool(create(Boolean, $(many)->get("missing") == NULL), true)
      ->expect_num($(many)->get("key_1"), 10)
      ->expect_num($(many)->get("key_5"), 555)
      ->expect_num($(many)->get("key_18"), 180)
      ->expect_num(create(Number, hash_foreach_sum), 240 + 5)
      ->expect_num(create(Number, hash_foreach_keys_len), 5 + 6 + 5 + 3)
      ->expect_str($(ord)->keys()->join(","), "first,third")
      ->expect_str(ord, "{\"first\":10,\"third\":30}")
      ->expect_str(
          create(Hash, 2)
              ->put("a", create(Number, 1))
              ->put("b", create(Slice, 2)
                  ->add(create(Boolean, true))
                  ->add(create(Null))),
          "{\"a\":1,\"b\":[true,null]}")
      /*
       * Continuing .put() on an outer Hash after inserting nested Hash
       * containers at multiple levels.
       */
      ->expect_str(
          create(Hash, 3)
              ->put("k\"ey", create(String, "val\nue"))
              ->put("nested", create(Hash, 2)
                  ->put("inner", create(Hash, 1)
                      ->put("leaf", create(Boolean, true)))
                  ->put("after_inner", create(Number, 2)))
              ->put("after_nested", create(String, "tail")),
          "{\"k\\\"ey\":\"val\\nue\","
          "\"nested\":{\"inner\":{\"leaf\":true},\"after_inner\":2},"
          "\"after_nested\":\"tail\"}")
      /*
       * Hash::to_chars(), Slice::to_chars(), and String::split()->join()
       * invoked inside an active String chain, followed by subsequent
       * .append() calls on the outer String.
       */
      ->expect_str(
          create(String, "h=")
              ->append($(ord)->chars())
              ->append(";s=")
              ->append($(fmt_slice)->chars())
              ->append(";c=")
              ->append($(csv_str)->split(",")->join("-")->chars())
              ->append("!"),
          "h={\"first\":10,\"third\":30};s=[\"x\",9];c=p-q-r!");

  release(csv_str);
  release(fmt_slice);
  release(ord);
  release(many);
}

static void test_typed_array_operations(Test *t) {
  /*
   * 1. TypedIntArray: variadic push_n, push, fill, set, at, subarray, map,
   *    filter, sort, reduce, foreach, join, and JSON serialization.
   */
  TypedIntArray *ints = create(TypedIntArray, 0)
      ->push_n(5, 40, 10, 50, 20, 30)
      ->push(60)
      ->push_n(0)
      ->sort()
      ->$$;

  typed_int_foreach_sum = 0;
  $(ints)->foreach(sum_typed_int_foreach);
  /* Also exercise foreach on an unowned temporary array (refs == 0). */
  create(TypedIntArray, 0)->push_n(3, 1, 2, 3)->foreach(sum_typed_int_foreach);
  int reduced_sum = $(ints)->reduce(sum_int_reduce, 0);

  int raw_ints[4] = {7, 8, 9, 10};
  TypedIntArray *from_raw = create(TypedIntArray, 0)
      ->from_buf(raw_ints, 4)
      ->set(0, 70)
      ->$$;

  /*
   * 2. TypedCharArray: variadic push_n (exercising C char -> int default
   *    argument promotion), map, subarray, and join.
   */
  TypedCharArray *chars = create(TypedCharArray, 0)
      ->push_n(5, 'h', 'e', 'l', 'l', 'o')
      ->$$;

  TypedCharArray *shifted_chars = $(chars)->subarray(0, 4)->map(rot1_char)->$$;

  /*
   * 3. TypedFloatArray: variadic push_n, fill, map, sort, and formatting.
   */
  TypedFloatArray *floats = create(TypedFloatArray, 0)
      ->push_n(4, 3.5, 1.25, 4.0, 2.5)
      ->sort()
      ->$$;

  TypedFloatArray *filled_floats = create(TypedFloatArray, 3)
      ->fill(1.5)
      ->map(scale_float)
      ->$$;

  $(t)->section("TypedIntArray, TypedCharArray & TypedFloatArray operations")
      ->expect_num(create(Number, $(ints)->length), 6)
      ->expect_num(create(Number, $(ints)->at(0)), 10)
      ->expect_num(create(Number, $(ints)->at(-1)), 60)
      ->expect_num(create(Number, $(ints)->at(99)), 0)
      ->expect_str(ints, "[10,20,30,40,50,60]")
      ->expect_str(
          $(ints)->subarray(1, 4)->map(double_int)->reverse(),
          "[80,60,40]")
      ->expect_num(
          create(TypedIntArray, 0)->push_n(3, 10, 20, 30)->to_slice()->get(1),
          20)
      ->expect_str(
          create(TypedFloatArray, 0)
              ->push_n(3, 1.5, 2.5, 3.5)
              ->reverse()
              ->to_slice(),
          "[3.5,2.5,1.5]")
      ->expect_str(
          create(TypedCharArray, 0)->push_n(2, 'A', 'B')->to_slice(),
          "[65,66]")
      ->expect_str(
          create(TypedIntArray, 0)
              ->push_n(6, 1, 2, 3, 4, 5, 6)
              ->filter(is_even_int)
              ->join(":"),
          "2:4:6")
      ->expect_num(create(Number, reduced_sum), 210)
      ->expect_num(create(Number, typed_int_foreach_sum), 210 + 15 + 6 + 3)
      ->expect_str(from_raw, "[70,8,9,10]")
      ->expect_str($(chars)->join(""), "hello")
      ->expect_str(chars, "[\"h\",\"e\",\"l\",\"l\",\"o\"]")
      ->expect_str($(shifted_chars)->join("-"), "i-f-m-m")
      ->expect_str(floats, "[1.25,2.5,3.5,4]")
      ->expect_str(filled_floats, "[3,3,3]")
      ->expect_str(
          create(Hash, 2)
              ->put("scores", create(TypedIntArray, 0)->push_n(3, 100, 95, 88))
              ->put("weights", create(TypedFloatArray, 0)->push_n(2, 0.5, 1.5)),
          "{\"scores\":[100,95,88],\"weights\":[0.5,1.5]}")
      /*
       * TypedArray nested directly inside a Slice, plus TypedArray->to_slice()
       * and TypedArray->join() nested inside an outer Slice with subsequent
       * .add() calls on the outer Slice.
       */
      ->expect_str(
          create(Slice, 4)
              ->add(create(TypedIntArray, 0)->push_n(2, 10, 20))
              ->add(create(TypedIntArray, 0)->push_n(2, 30, 40)->to_slice())
              ->add(create(TypedIntArray, 0)->push_n(2, 50, 60)->join("/"))
              ->add(create(String, "end")),
          "[[10,20],[30,40],\"50/60\",\"end\"]");

  release(filled_floats);
  release(floats);
  release(shifted_chars);
  release(chars);
  release(from_raw);
  release(ints);
}

int main(void) {
  Test *t = create(Test, "Containers (Slice, Hash & TypedArray) Unit Tests")->$$;

  test_slice_operations(t);
  test_hash_operations(t);
  test_typed_array_operations(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
