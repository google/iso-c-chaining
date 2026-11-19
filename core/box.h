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

#ifndef CHAIN_BOX_H_
#define CHAIN_BOX_H_

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Box, BOX, (void*, void (*)(void*)))

/*
 * Reference-counted smart-pointer wrapper for arbitrary C pointers and OS
 * handles (`void*` + optional destructor callback `dtor`). Embedding `Box`
 * lets raw C allocations participate in `Slice`, `Hash`, `StatusOr`, and
 * `ch_scoped` lifetimes without writing a full `ChGenericEl` class per type.
 */
struct BOX {
  ChGenericEl generic;
  void *ptr;
  void (*dtor)(void*);
  const char *type_tag;
  char desc[96];

  /* Binds this `Box` as the active instance on the `Box` `self` stack. */
  ChBox* (*bind)(ChGenericEl*);

  /* Returns the wrapped pointer (`ptr`), or `NULL` if empty or taken. */
  void* (*get)(void);

  /*
   * Frees any previously wrapped pointer (calling `dtor(ptr)` if `ptr` is
   * non-NULL and differs from `new_ptr`), stores `(new_ptr, new_dtor)`, and
   * returns this `Box` for chaining. Aborts if this `Box` is shared
   * (`refs > 1`).
   */
  ChBox* (*reset)(void*, void (*)(void*));

  /*
   * Detaches and returns the wrapped pointer without invoking `dtor`, setting
   * `ptr = NULL` and `dtor = NULL`. Aborts if this `Box` is shared
   * (`refs > 1`). If called on an unowned temporary (`refs == 0`), releases the
   * temporary `Box` shell automatically.
   */
  void* (*take)(void);
};

/* Helpers backing `ch_box_of`, `ch_box_as`, and `ch_use_box`. */
void ch_box_raw_init(ChBox *box, void *ptr, void (*dtor)(void*),
                     const char *type_tag);
ChBox* ch_box_raw_tag(ChBox *box, const char *type_tag);
ChBox* ch_box_raw_bind(ChGenericEl *el);
void* ch_box_raw_get(void);
void* ch_box_raw_take(void);
ChBox* ch_box_raw_reset(void *ptr, void (*dtor)(void*));
void* ch_box_raw_as(ChGenericEl *el, const char *expected_tag);

#define ch_box_of(Type, ptr, dtor) \
  ch_box_raw_tag( \
      ch_new_Box((void*)(1 ? (ptr) : (Type*)0), (dtor)), \
      #Type)

#define ch_box_as(Type, elem) \
  ((Type*)ch_box_raw_as((ChGenericEl*)(elem), #Type))

#ifndef CHAIN_NO_SHORT_NAMES
#define ch_use_box_short_(Type) typedef Ch##Type##Box Type##Box;
#else
#define ch_use_box_short_(Type)
#endif

/*
 * Declares a zero-overhead, statically typed `<Type>Box` / `Ch<Type>Box`
 * facade over `Box` whose `get()`, `take()`, and `reset()` methods return and
 * accept `Type*` directly, eliminating casts at dereference sites (e.g.,
 * `$(mesh)->get()->visible = 1`) while staying 100% compatible with `Hash`,
 * `Slice`, `StatusOr`, and `box_as`.
 */
#define ch_use_box(Type) \
  typedef struct Ch##Type##Box_ { \
    ChGenericEl generic; \
    void *ptr; \
    void (*dtor)(void*); \
    const char *type_tag; \
    char desc[96]; \
    struct Ch##Type##Box_* (*bind)(ChGenericEl*); \
    Type* (*get)(void); \
    struct Ch##Type##Box_* (*reset)(Type*, void (*)(void*)); \
    Type* (*take)(void); \
  } Ch##Type##Box; \
  static struct Ch##Type##Box_* ch_bind_##Type##Box_(ChGenericEl *el) { \
    return (Ch##Type##Box*)ch_box_raw_bind(el); \
  } \
  static Type* ch_get_##Type##Box_(void) { \
    return (Type*)ch_box_raw_get(); \
  } \
  static struct Ch##Type##Box_* ch_reset_##Type##Box_( \
      Type *p, void (*dtor)(void*)) { \
    return (Ch##Type##Box*)ch_box_raw_reset((void*)p, dtor); \
  } \
  static Type* ch_take_##Type##Box_(void) { \
    return (Type*)ch_box_raw_take(); \
  } \
  static Ch##Type##Box* ch_new_##Type##Box(Type *ptr, void (*dtor)(void*)) { \
    Ch##Type##Box *b = (Ch##Type##Box*)malloc(sizeof(Ch##Type##Box)); \
    if (b == NULL) { \
      if (ptr != NULL && dtor != NULL) { \
        dtor((void*)ptr); \
      } \
      return NULL; \
    } \
    ch_box_raw_init((ChBox*)b, (void*)ptr, dtor, #Type); \
    b->bind = ch_bind_##Type##Box_; \
    b->generic.bind = (void* (*)(ChGenericEl*))ch_bind_##Type##Box_; \
    b->get = ch_get_##Type##Box_; \
    b->reset = ch_reset_##Type##Box_; \
    b->take = ch_take_##Type##Box_; \
    return b; \
  } \
  ch_use_box_short_(Type)

#ifndef CHAIN_NO_SHORT_NAMES
#define box_of(Type, ptr, dtor) ch_box_of(Type, ptr, dtor)
#define box_as(Type, elem) ch_box_as(Type, elem)
#define use_box(Type) ch_use_box(Type)
#endif

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_BOX_H_ */
