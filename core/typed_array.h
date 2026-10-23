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

#ifndef CHAIN_TYPED_ARRAY_H_
#define CHAIN_TYPED_ARRAY_H_

#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Declares a JavaScript-style contiguous unboxed typed array (`TypedIntArray`,
 * `TypedCharArray`, `TypedFloatArray`) backed by `ChGenericEl` so instances
 * participate in reference counting, `Hash`/`Slice` nesting, and JSON
 * serialization while storing raw C primitives (`int`, `char`, `double`) in a
 * single allocation.
 */
#define CH_DECLARE_TYPED_ARRAY(ChType, TYPE_TAG, ElemT)                        \
  ch_use(ChType, TYPE_TAG, (int))                                               \
                                                                               \
  struct TYPE_TAG {                                                            \
    ChGenericEl generic;                                                       \
    ElemT *data;                                                               \
    int length;                                                                \
    int capacity;                                                              \
    ChString *str;                                                             \
                                                                               \
    /* Binds this array as the active instance on its type's `self` stack. */  \
    Ch##ChType* (*bind)(ChGenericEl*);                                         \
                                                                               \
    /* Returns the element at `index` (supports negative indices from end). */ \
    ElemT (*at)(int index);                                                    \
                                                                               \
    /* Sets `data[index] = val` in place and returns this array. */            \
    Ch##ChType* (*set)(int index, ElemT val);                                  \
                                                                               \
    /* Appends a single `val` and returns this array for chaining. */          \
    Ch##ChType* (*push)(ElemT val);                                            \
                                                                               \
    /* Appends `count` variadic values in one call and returns this array. */  \
    Ch##ChType* (*push_n)(int count, ...);                                     \
                                                                               \
    /* Fills `[0, length)` (or `[0, capacity)` if empty) with `val`. */        \
    Ch##ChType* (*fill)(ElemT val);                                            \
                                                                               \
    /* Copies `count` elements from `buf` into the array and returns `slf`. */ \
    Ch##ChType* (*from_buf)(const ElemT *buf, int count);                      \
                                                                               \
    /* Returns a new array over `[begin, end)`, releasing `slf` if unowned. */ \
    Ch##ChType* (*subarray)(int begin, int end);                               \
                                                                               \
    /* Applies `fn` to each element in place and returns this array. */        \
    Ch##ChType* (*map)(ElemT (*fn)(ElemT));                                    \
                                                                               \
    /* Returns a new array of elements where `pred(x)` is non-zero. */         \
    Ch##ChType* (*filter)(int (*pred)(ElemT));                                 \
                                                                               \
    /* Sorts the contiguous buffer in ascending order in place. */             \
    Ch##ChType* (*sort)(void);                                                 \
                                                                               \
    /* Reverses the contiguous buffer in place and returns this array. */      \
    Ch##ChType* (*reverse)(void);                                              \
                                                                               \
    /* Folds the array left-to-right from `initial` into a single `ElemT`. */  \
    ElemT (*reduce)(ElemT (*fn)(ElemT acc, ElemT val), ElemT initial);         \
                                                                               \
    /* Invokes `run(val, index)` for each element in order. */                 \
    void (*foreach)(void (*run)(ElemT val, int index));                        \
                                                                               \
    /* Joins formatted elements with `delim` into a new `String*`. */          \
    ChString* (*join)(char *delim);                                            \
                                                                               \
    /* Boxes each element as a `Number` into a new `Slice*`. */                \
    ChSlice* (*to_slice)(void);                                                \
  };

CH_DECLARE_TYPED_ARRAY(TypedIntArray, TYPED_INT_ARRAY, int)
CH_DECLARE_TYPED_ARRAY(TypedCharArray, TYPED_CHAR_ARRAY, char)
CH_DECLARE_TYPED_ARRAY(TypedFloatArray, TYPED_FLOAT_ARRAY, double)

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_TYPED_ARRAY_H_ */
