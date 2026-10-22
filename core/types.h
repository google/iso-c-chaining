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

#ifndef CHAIN_TYPES_H_
#define CHAIN_TYPES_H_

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__TINYC__)
#define CH_THREAD_LOCAL
#elif defined(__cplusplus) && __cplusplus >= 201103L
#define CH_THREAD_LOCAL thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
#define CH_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define CH_THREAD_LOCAL __thread
#else
#define CH_THREAD_LOCAL
#endif

typedef const void* ChType;

#define CH_GENERIC ((ChType)NULL)
#define CH_MAGIC 0x4348454Cu /* "CHEL" (Chaining Element) */

typedef struct ch_generic_el ChGenericEl;

/*
 * Common base header embedded as the first field of every object struct so any
 * instance pointer can be cast to `ChGenericEl*` for reference counting,
 * intrusive `self` stack linking, dynamic binding, and string serialization.
 * `magic` is initialized to `CH_MAGIC` by every constructor and verified before
 * dereferencing untyped `void*` element pointers.
 */
struct ch_generic_el {
  unsigned int magic;
  int refs;
  ChType type_val;
  ChGenericEl *prev_self;
  ChGenericEl *next_self;
  void* (*bind)(ChGenericEl *);
  void* (*save)(void);
  void (*del)(void *);
  char* (*to_chars)(void);
};

/*
 * Returns non-zero if `ptr` is non-NULL and begins with the 4-byte `CH_MAGIC`
 * header tag. Compares `unsigned char` bytes with short-circuit evaluation so
 * probing arbitrary `void*` pointers (even 1-byte string literals like `""`)
 * is safe under ISO C strict aliasing, ASan, and `-fsanitize=object-size`.
 */
static int ch_is_generic_el(const void *ptr) {
  if (ptr == NULL) {
    return 0;
  }
  const unsigned char *b = (const unsigned char*)ptr;
  const unsigned int expected = CH_MAGIC;
  const unsigned char *m = (const unsigned char*)&expected;
  return b[0] == m[0] && b[1] == m[1] && b[2] == m[2] && b[3] == m[3];
}

/*
 * Validates that a non-NULL `void*` passed into container or lifecycle APIs is
 * a genuine `ChGenericEl*` (carrying `CH_MAGIC`), aborting with an immediate
 * diagnostic if a raw C pointer (e.g., `char*` or `int*`) was passed instead.
 */
static ChGenericEl* ch_check_el(void *ptr, const char *context) {
  if (ptr == NULL) {
    return NULL;
  }
  if (!ch_is_generic_el(ptr)) {
    fprintf(stderr,
            "Type error in %s: pointer %p is not a valid ChGenericEl "
            "(missing CH_MAGIC 0x%x)\n",
            context, ptr, CH_MAGIC);
    abort();
  }
  return (ChGenericEl*)ptr;
}

/*
 * Maintains a per-type intrusive doubly-linked stack of active instances so
 * nested expressions of the same type (e.g., an inner `Hash` constructed inside
 * an outer `Hash::put()` argument list) can push and pop `self` in O(1) time:
 * - `s == (ChGenericEl*)-1`: pops the current top element off the stack.
 * - `s != NULL`: verifies `ch_is_generic_el(s)` and
 *   `s->type_val == expected_type`, unlinks `s` if it is already deeper in the
 *   stack, and pushes `s` onto `*top_ptr`.
 * - `s == NULL`: leaves the stack unchanged and returns the current `*top_ptr`.
 */
static ChGenericEl* ch_update_self_stack(ChGenericEl **top_ptr, ChGenericEl *s,
                                         ChType expected_type) {
  ChGenericEl *top = *top_ptr;
  if (s == (ChGenericEl*)-1) {
    if (top != NULL) {
      ChGenericEl *prev = top->prev_self;
      if (prev != NULL) {
        prev->next_self = NULL;
      }
      top->prev_self = NULL;
      top->next_self = NULL;
      *top_ptr = prev;
    }
  } else if (s != NULL) {
    if (!ch_is_generic_el(s) || s->type_val != expected_type) {
      fprintf(stderr, "Type error: cannot bind %p to type %p self stack\n",
              (void*)s, expected_type);
      return NULL;
    }
    if (s != top) {
      if (s->prev_self != NULL || s->next_self != NULL) {
        if (s->prev_self != NULL) {
          s->prev_self->next_self = s->next_self;
        }
        if (s->next_self != NULL) {
          s->next_self->prev_self = s->prev_self;
        }
      }
      s->prev_self = top;
      s->next_self = NULL;
      if (top != NULL) {
        top->next_self = s;
      }
      *top_ptr = s;
    }
  }
  return *top_ptr;
}

/*
 * Dynamically binds a generic element to its concrete type's `self` stack via
 * its virtual `bind` pointer so callers can chain methods on container lookups.
 */
static void* ch_bind_el(ChGenericEl *el) {
  ChGenericEl *checked = ch_check_el(el, "bind");
  return checked != NULL ? checked->bind(checked) : NULL;
}

/*
 * Runtime-checked downcast used by `ch_as(Class, elem)`: verifies `el` has the
 * expected runtime `ChType` tag, aborts with a diagnostic on mismatch, and
 * binds `el` to its concrete type's `self` stack.
 */
static void* ch_cast_el(ChGenericEl *el, ChType expected, const char *name) {
  ChGenericEl *checked = ch_check_el(el, name);
  if (checked == NULL) {
    return NULL;
  }
  if (checked->type_val != expected) {
    fprintf(stderr, "Type error: expected %s (%p), got %p\n",
            name, expected, checked->type_val);
    abort();
  }
  return checked->bind(checked);
}

/*
 * Transfers or shares ownership of `elem` when inserting it into a container:
 * if `elem` is an unowned temporary (`refs == 0`) or just displaced
 * `container_slf` on the same type's `self` stack (e.g., nesting a `Hash`
 * inside a `Hash`), pops `elem` off its `self` stack so the outer container
 * becomes `slf` again before incrementing `elem->refs`.
 */
static void ch_claim_element(void *elem, void *container_slf) {
  ChGenericEl *el = ch_check_el(elem, "claim_element");
  if (el != NULL) {
    if (elem == container_slf || el->refs == 0) {
      el->bind(el);
      el->bind((ChGenericEl*)-1);
    }
    el->refs++;
  }
}

/*
 * Decrements `elem`'s reference count via its virtual `del` destructor and
 * frees it once `refs` reaches 0.
 */
static void ch_release(void *elem) {
  ChGenericEl *el = ch_check_el(elem, "release");
  if (el != NULL) {
    el->del(el);
  }
}

#ifndef CHAIN_NO_SHORT_NAMES
#define ch_use_short_(class, type) \
  typedef struct type class; \
  typedef struct type ## _ class ## Element; \
  static const ChType type = (const void*)&ch_new_ ## class;
#else
#define ch_use_short_(class, type)
#endif

#define ch_create(class, ...) ((Ch ## class*)ch_new_ ## class(__VA_ARGS__))
#define ch_bind(obj) ((obj)->bind((ChGenericEl*)(obj)))
#define ch_save generic.save()
#define ch_as(class, elem) \
  ((Ch ## class*)ch_cast_el((ChGenericEl*)(elem), \
                            (ChType)&ch_new_ ## class, #class))
#define ch_is_type(elem, type) \
  (ch_is_generic_el(elem) && ((ChGenericEl*)(elem))->type_val == (type))
#define ch_chars() generic.to_chars()
#define ch_slf self(NULL)
#define ch_use(class, type, ...) \
  typedef struct type Ch ## class; \
  typedef struct type ## _ Ch ## class ## Element; \
  Ch ## class* ch_new_ ## class(__VA_ARGS__); \
  static const ChType CH_ ## type = (const void*)&ch_new_ ## class; \
  ch_use_short_(class, type)

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChType Type;
typedef ChGenericEl GenericEl;
#define GENERIC CH_GENERIC
#define is_generic_el ch_is_generic_el
#define check_el ch_check_el
#define update_self_stack ch_update_self_stack
#define claim_element ch_claim_element
#define release ch_release
#define create(class, ...) ((class*)ch_new_ ## class(__VA_ARGS__))
#define $(obj) ch_bind(obj)
#define $$ ch_save
#define as(class, elem) \
  ((class*)ch_cast_el((ChGenericEl*)(elem), (ChType)&ch_new_ ## class, #class))
#define is_type(elem, type) ch_is_type(elem, type)
#define chars() ch_chars()
#define slf ch_slf
#define use(class, type, ...) ch_use(class, type, __VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_TYPES_H_ */
