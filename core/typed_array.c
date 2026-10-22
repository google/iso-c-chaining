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

#include "core/typed_array.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"

static void format_int_elem(char *buf, size_t size, int v) {
  snprintf(buf, size, "%d", v);
}

static void format_char_join(char *buf, size_t size, char v) {
  snprintf(buf, size, "%c", v);
}

static void format_char_json(char *buf, size_t size, char v) {
  unsigned char uc = (unsigned char)v;
  if (v == '"') {
    snprintf(buf, size, "\"\\\"\"");
  } else if (v == '\\') {
    snprintf(buf, size, "\"\\\\\"");
  } else if (v == '\n') {
    snprintf(buf, size, "\"\\n\"");
  } else if (v == '\r') {
    snprintf(buf, size, "\"\\r\"");
  } else if (v == '\t') {
    snprintf(buf, size, "\"\\t\"");
  } else if (uc < 0x20) {
    snprintf(buf, size, "\"\\u%04x\"", (unsigned int)uc);
  } else {
    snprintf(buf, size, "\"%c\"", v);
  }
}

static void format_float_elem(char *buf, size_t size, double v) {
  if (v == floor(v)) {
    snprintf(buf, size, "%.0f", v);
  } else {
    snprintf(buf, size, "%.8f", v);
    if (strchr(buf, '.') != NULL) {
      size_t len = strlen(buf);
      while (len > 0 && buf[len - 1] == '0') {
        buf[len - 1] = '\0';
        len--;
      }
      if (len > 0 && buf[len - 1] == '.') {
        buf[len - 1] = '\0';
      }
    }
  }
  if (strcmp(buf, "-0") == 0) {
    strcpy(buf, "0");
  }
}

/*
 * Generates the complete method table and constructor for a contiguous typed
 * array. `VaPromotedT` is the C default argument promotion type for `<stdarg.h>`
 * (`int` for `int` and `char`, `double` for `double`).
 */
#define CH_IMPL_TYPED_ARRAY(                                                   \
    ChType, TYPE_TAG, pfx, ElemT, VaPromotedT, join_fmt_fn, json_fmt_fn)       \
  static ChType* pfx##_self(GenericEl *s) {                                    \
    static CH_THREAD_LOCAL GenericEl *top = NULL;                              \
    return (ChType*)update_self_stack(&top, s, TYPE_TAG);                      \
  }                                                                            \
                                                                               \
  static ChType* pfx##_bind(GenericEl *arr) {                                  \
    return pfx##_self(arr);                                                    \
  }                                                                            \
                                                                               \
  static void* pfx##_save(void) {                                              \
    ChType *arr = pfx##_self(NULL);                                            \
    arr->generic.refs++;                                                       \
    return arr;                                                                \
  }                                                                            \
                                                                               \
  static void pfx##_del(void *obj) {                                           \
    ChType *arr = (ChType*)obj;                                                \
    if (arr != NULL && pfx##_self((GenericEl*)arr) != NULL) {                  \
      ChType *cur = pfx##_self(NULL);                                          \
      cur->generic.refs--;                                                     \
      if (cur->generic.refs <= 0) {                                            \
        release(cur->str);                                                     \
        pfx##_self((GenericEl*)cur);                                           \
        pfx##_self((GenericEl*)-1);                                            \
        free(cur->data);                                                       \
        free(cur);                                                             \
      } else {                                                                 \
        pfx##_self((GenericEl*)-1);                                            \
      }                                                                        \
    }                                                                          \
  }                                                                            \
                                                                               \
  static void pfx##_ensure_cap(ChType *arr, int min_cap) {                     \
    if (min_cap <= arr->capacity) {                                            \
      return;                                                                  \
    }                                                                          \
    int new_cap = arr->capacity > 0 ? arr->capacity : 4;                       \
    while (new_cap < min_cap) {                                                \
      new_cap *= 2;                                                            \
    }                                                                          \
    arr->capacity = new_cap;                                                   \
    arr->data = realloc(arr->data, sizeof(ElemT) * (size_t)arr->capacity);     \
  }                                                                            \
                                                                               \
  static ElemT pfx##_at(int index) {                                           \
    ChType *arr = pfx##_self(NULL);                                            \
    int idx = index < 0 ? arr->length + index : index;                         \
    ElemT res = (idx < 0 || idx >= arr->length) ? (ElemT)0 : arr->data[idx];   \
    if (arr->generic.refs == 0) {                                              \
      release(arr);                                                            \
    }                                                                          \
    return res;                                                                \
  }                                                                            \
                                                                               \
  static ChType* pfx##_set(int index, ElemT val) {                             \
    ChType *arr = pfx##_self(NULL);                                            \
    int idx = index < 0 ? arr->length + index : index;                         \
    if (idx >= 0 && idx < arr->length) {                                       \
      arr->data[idx] = val;                                                    \
    }                                                                          \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_push(ElemT val) {                                       \
    ChType *arr = pfx##_self(NULL);                                            \
    pfx##_ensure_cap(arr, arr->length + 1);                                    \
    arr->data[arr->length++] = val;                                            \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_push_n(int count, ...) {                                \
    ChType *arr = pfx##_self(NULL);                                            \
    if (count <= 0) {                                                          \
      return $(arr);                                                           \
    }                                                                          \
    pfx##_ensure_cap(arr, arr->length + count);                                \
    va_list ap;                                                                \
    va_start(ap, count);                                                       \
    int i;                                                                     \
    for (i = 0; i < count; i++) {                                              \
      arr->data[arr->length++] = (ElemT)va_arg(ap, VaPromotedT);               \
    }                                                                          \
    va_end(ap);                                                                \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_fill(ElemT val) {                                       \
    ChType *arr = pfx##_self(NULL);                                            \
    int target_len = arr->length > 0 ? arr->length : arr->capacity;            \
    int i;                                                                     \
    for (i = 0; i < target_len; i++) {                                         \
      arr->data[i] = val;                                                      \
    }                                                                          \
    arr->length = target_len;                                                  \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_from_buf(const ElemT *buf, int count) {                 \
    ChType *arr = pfx##_self(NULL);                                            \
    if (buf == NULL || count <= 0) {                                           \
      arr->length = 0;                                                         \
      return $(arr);                                                           \
    }                                                                          \
    pfx##_ensure_cap(arr, count);                                              \
    memcpy(arr->data, buf, sizeof(ElemT) * (size_t)count);                     \
    arr->length = count;                                                       \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_subarray(int begin, int end) {                          \
    ChType *old = pfx##_self(NULL)->generic.save();                            \
    int b = begin < 0 ? old->length + begin : begin;                           \
    int e = end < 0 ? old->length + end : end;                                 \
    if (b < 0) {                                                               \
      b = 0;                                                                   \
    }                                                                          \
    if (e > old->length) {                                                     \
      e = old->length;                                                         \
    }                                                                          \
    int span = e > b ? e - b : 0;                                              \
    ChType *sub = ch_new_##ChType(span > 0 ? span : 1);                        \
    if (span > 0) {                                                            \
      memcpy(sub->data, old->data + b, sizeof(ElemT) * (size_t)span);          \
      sub->length = span;                                                      \
    }                                                                          \
    release(old);                                                              \
    return $(sub);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_map(ElemT (*fn)(ElemT)) {                               \
    ChType *arr = pfx##_self(NULL);                                            \
    if (fn != NULL) {                                                          \
      int i;                                                                   \
      for (i = 0; i < arr->length; i++) {                                      \
        arr->data[i] = fn(arr->data[i]);                                       \
      }                                                                        \
    }                                                                          \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_filter(int (*pred)(ElemT)) {                            \
    ChType *old = pfx##_self(NULL)->generic.save();                            \
    ChType *out = ch_new_##ChType(old->length > 0 ? old->length : 1);          \
    if (pred != NULL) {                                                        \
      int i;                                                                   \
      for (i = 0; i < old->length; i++) {                                      \
        if (pred(old->data[i])) {                                              \
          out->data[out->length++] = old->data[i];                             \
        }                                                                      \
      }                                                                        \
    }                                                                          \
    release(old);                                                              \
    return $(out);                                                             \
  }                                                                            \
                                                                               \
  static int pfx##_cmp(const void *a, const void *b) {                         \
    ElemT va = *(const ElemT*)a;                                               \
    ElemT vb = *(const ElemT*)b;                                               \
    return (va > vb) - (va < vb);                                              \
  }                                                                            \
                                                                               \
  static ChType* pfx##_sort(void) {                                            \
    ChType *arr = pfx##_self(NULL);                                            \
    if (arr->length > 1) {                                                     \
      qsort(arr->data, (size_t)arr->length, sizeof(ElemT), pfx##_cmp);         \
    }                                                                          \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ChType* pfx##_reverse(void) {                                         \
    ChType *arr = pfx##_self(NULL);                                            \
    int i;                                                                     \
    int half = arr->length / 2;                                                \
    for (i = 0; i < half; i++) {                                               \
      ElemT tmp = arr->data[i];                                                \
      arr->data[i] = arr->data[arr->length - 1 - i];                           \
      arr->data[arr->length - 1 - i] = tmp;                                    \
    }                                                                          \
    return $(arr);                                                             \
  }                                                                            \
                                                                               \
  static ElemT pfx##_reduce(ElemT (*fn)(ElemT acc, ElemT val),                 \
                            ElemT initial) {                                   \
    ChType *arr = pfx##_self(NULL);                                            \
    ElemT acc = initial;                                                       \
    if (fn != NULL) {                                                          \
      int i;                                                                   \
      for (i = 0; i < arr->length; i++) {                                      \
        acc = fn(acc, arr->data[i]);                                           \
      }                                                                        \
    }                                                                          \
    if (arr->generic.refs == 0) {                                              \
      release(arr);                                                            \
    }                                                                          \
    return acc;                                                                \
  }                                                                            \
                                                                               \
  static void pfx##_foreach(void (*run)(ElemT val, int index)) {               \
    ChType *arr = pfx##_self(NULL);                                            \
    if (run != NULL) {                                                         \
      int i;                                                                   \
      for (i = 0; i < arr->length; i++) {                                      \
        run(arr->data[i], i);                                                  \
      }                                                                        \
    }                                                                          \
    if (arr->generic.refs == 0) {                                              \
      release(arr);                                                            \
    } else {                                                                   \
      $(arr);                                                                  \
    }                                                                          \
  }                                                                            \
                                                                               \
  static String* pfx##_join(char *delim) {                                     \
    ChType *arr = pfx##_self(NULL)->generic.save();                            \
    String *str = create(String, "");                                          \
    char item_buf[64];                                                         \
    const char *sep = delim != NULL ? delim : ",";                             \
    int i;                                                                     \
    for (i = 0; i < arr->length; i++) {                                        \
      if (i > 0) {                                                             \
        $(str)->append((char*)sep);                                            \
      }                                                                        \
      join_fmt_fn(item_buf, sizeof(item_buf), arr->data[i]);                   \
      $(str)->append(item_buf);                                                \
    }                                                                          \
    release(arr);                                                              \
    return $(str);                                                             \
  }                                                                            \
                                                                               \
  static Slice* pfx##_to_slice(void) {                                         \
    ChType *arr = pfx##_self(NULL)->generic.save();                            \
    Slice *slice = create(Slice, arr->length > 0 ? arr->length : 1);           \
    int i;                                                                     \
    for (i = 0; i < arr->length; i++) {                                        \
      $(slice)->add(create(Number, (double)arr->data[i]));                     \
    }                                                                          \
    release(arr);                                                              \
    return $(slice);                                                           \
  }                                                                            \
                                                                               \
  static char* pfx##_to_chars(void) {                                          \
    ChType *arr = pfx##_self(NULL);                                            \
    char item_buf[64];                                                         \
    $(arr->str)->set("[");                                                     \
    int i;                                                                     \
    for (i = 0; i < arr->length; i++) {                                        \
      if (i > 0) {                                                             \
        $(arr->str)->append(",");                                              \
      }                                                                        \
      json_fmt_fn(item_buf, sizeof(item_buf), arr->data[i]);                   \
      $(arr->str)->append(item_buf);                                           \
    }                                                                          \
    $(arr->str)->append("]");                                                  \
    char *res = arr->str->chars;                                               \
    arr->str->bind((GenericEl*)-1);                                            \
    if (arr->generic.prev_self != NULL) {                                      \
      pfx##_self((GenericEl*)-1);                                              \
    }                                                                          \
    return res;                                                                \
  }                                                                            \
                                                                               \
  ChType* ch_new_##ChType(int n) {                                             \
    int cap = n > 0 ? n : 4;                                                   \
    ChType *arr = malloc(sizeof(ChType));                                      \
    arr->generic.magic = CH_MAGIC;                                             \
    arr->generic.type_val = TYPE_TAG;                                          \
    arr->generic.prev_self = NULL;                                             \
    arr->generic.next_self = NULL;                                             \
    arr->generic.refs = 0;                                                     \
                                                                               \
    arr->data = calloc((size_t)cap, sizeof(ElemT));                            \
    arr->length = 0;                                                           \
    arr->capacity = cap;                                                       \
    arr->str = create(String, "")->$$;                                         \
    arr->str->bind((GenericEl*)-1);                                            \
                                                                               \
    arr->bind = pfx##_bind;                                                    \
    arr->generic.bind = (void* (*)(GenericEl*))pfx##_bind;                     \
    arr->generic.save = pfx##_save;                                            \
    arr->generic.del = pfx##_del;                                              \
    arr->generic.to_chars = pfx##_to_chars;                                    \
                                                                               \
    arr->at = pfx##_at;                                                        \
    arr->set = pfx##_set;                                                      \
    arr->push = pfx##_push;                                                    \
    arr->push_n = pfx##_push_n;                                                \
    arr->fill = pfx##_fill;                                                    \
    arr->from_buf = pfx##_from_buf;                                            \
    arr->subarray = pfx##_subarray;                                            \
    arr->map = pfx##_map;                                                      \
    arr->filter = pfx##_filter;                                                \
    arr->sort = pfx##_sort;                                                    \
    arr->reverse = pfx##_reverse;                                              \
    arr->reduce = pfx##_reduce;                                                \
    arr->foreach = pfx##_foreach;                                              \
    arr->join = pfx##_join;                                                    \
    arr->to_slice = pfx##_to_slice;                                            \
                                                                               \
    pfx##_self((GenericEl*)arr);                                               \
    return arr;                                                                \
  }

CH_IMPL_TYPED_ARRAY(
    TypedIntArray, TYPED_INT_ARRAY, int_arr, int, int,
    format_int_elem, format_int_elem)
CH_IMPL_TYPED_ARRAY(
    TypedCharArray, TYPED_CHAR_ARRAY, char_arr, char, int,
    format_char_join, format_char_json)
CH_IMPL_TYPED_ARRAY(
    TypedFloatArray, TYPED_FLOAT_ARRAY, float_arr, double, double,
    format_float_elem, format_float_elem)
