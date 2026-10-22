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

#include "core/slice.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/string.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static Slice* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Slice*)update_self_stack(&top, s, SLICE);
}

static Slice* bind(GenericEl* slice) {
  return self(slice);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

/*
 * Decrements `slice`'s reference count and frees the `Slice` and its elements
 * when `refs` reaches 0.
 */
static void del(void* obj) {
  Slice* slice = (Slice*)obj;
  int i;
  /*
   * `self((GenericEl*)slice)` verifies `obj` is a `Slice` and pushes it onto
   * the `Slice` `self` stack so `slf` (and `slf->get(i)`) operate on `slice`.
   */
  if (slice != NULL && self((GenericEl*)slice) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      for (i = 0; i < slice->length; i++) {
        GenericEl *el = slice->elements[i];
        if (el != NULL) {
          el->del(el);
        }
      }
      release(slice->str);
      /*
       * Pass the `-1` sentinel to pop `slice` off the `self` stack before
       * freeing its memory so the stack never holds a dangling pointer.
       */
      self((GenericEl*)slice);
      self((GenericEl*)-1);
      free(slice->elements);
      free(slice);
    } else {
      /*
       * `slice` still has active references; pop the temporary `self` stack
       * entry pushed by the `if` condition before returning.
       */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Slice operational functions.
 */

static Slice* subSlice(int start, int end) {
  int i;
  GenericEl **elements = slf->elements;
  /*
   * Temporarily increment `oldSlice` refs (`save()`) and `release(oldSlice)`
   * after copying so that an unowned intermediate `Slice` (e.g. from `split()`
   * or `append()`) is automatically freed once `sub_slice()` finishes, while a
   * caller-saved `Slice` (`refs > 0`) has its original refcount restored.
   */
  Slice *oldSlice = slf->generic.save();
  Slice *slice = create(Slice, end - start + 1);

  for (i = start; i <= end; i++) {
    slice->add(elements[i]);
  }
  release(oldSlice);

  return $(slice);
}

static GenericEl* get(int n) {
  Slice *s = slf;
  if (s->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return (GenericEl*)ch_bind_el(s->elements[n]);
}

static Slice* add(void* generic) {
  /*
   * `claim_element` pops `generic` off its `self` stack if it is an unowned
   * temporary or nested `Slice`, ensuring `slf` below resolves to the outer
   * `Slice` receiving the element.
   */
  claim_element(generic, slf);
  Slice *slice = slf;
  if (slice->length >= slice->capacity) {
    slice->capacity = slice->capacity > 0 ? slice->capacity * 2 : 1;
    slice->elements =
        realloc(slice->elements, sizeof(GenericEl*) * slice->capacity);
  }
  slice->elements[slice->length] = (GenericEl*)generic;
  slice->length++;

  return slice;
}

static Slice* set(int i, void *generic) {
  claim_element(generic, slf);
  Slice *slice = slf;
  if (slice->elements[i] != NULL) {
    slice->elements[i]->del(slice->elements[i]);
  }
  slice->elements[i] = (GenericEl*)generic;

  return slice;
}

static Slice* append(Slice* other) {
  if (other == NULL) {
    return slf;
  }
  if (other == slf && other->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  Slice* old_slice = slf;
  int i;
  int len = other->length;

  for (i = 0; i < len; i++) {
    old_slice->add(other->elements[i]);
  }
  if (other != old_slice && other->generic.refs == 0) {
    release(other);
  }

  return $(old_slice);
}

static String* join(char* delim) {
  int i;
  Slice *slice = slf->generic.save();
  String* str = create(String, "");
  for (i = 0; i < slice->length; i++) {
    if (i > 0) {
      $(str)->append(delim);
    }
    GenericEl *el = slice->elements[i];
    char *chars = ((GenericEl*)ch_bind_el(el))->to_chars();
    if (el->bind(NULL) == el) {
      el->bind((GenericEl*)-1);
    }
    $(str)->append(chars);
  }
  release(slice);
  return $(str);
}

static void foreach(void (*run)(GenericEl*)) {
  int i;
  Slice *slice = slf;
  for (i = 0; i < slice->length; i++) {
    /* Re-bind `slice` each pass in case `run()` touches another `Slice`. */
    run($(slice)->get(i));
  }
  if (slice->generic.refs == 0) {
    release(slice);
  } else {
    $(slice);
  }
}

static char* to_chars(void) {
  int i;
  Slice *slice = slf;
  /* Reset the slice's reusable scratch buffer before building JSON output. */
  $(slice->str)->set("");
  $(slice->str)->append("[");
  for (i = 0; i < slice->length; i++) {
    if (i > 0) {
      $(slice->str)->append(",");
    }
    GenericEl *el = slice->elements[i];
    char *chars = ((GenericEl*)ch_bind_el(el))->to_chars();
    if (el->bind(NULL) == el) {
      el->bind((GenericEl*)-1);
    }
    /*
     * `String->to_chars()` returns raw unquoted text, so wrap and JSON-escape
     * `STRING` elements while appending other types' JSON text directly.
     */
    if (is_type(el, STRING)) {
      $(slice->str)->append("\"")
        ->append_escaped(chars)
        ->append("\"");
    } else {
      $(slice->str)->append(chars);
    }
  }
  $(slice->str)->append("]");
  char *res = slice->str->chars;
  slice->str->bind((GenericEl*)-1);
  if (slice->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return res;
}

/**
 * Constructor.
 */
Slice* ch_new_Slice(int n) {
  int cap = n > 0 ? n : 1;
  Slice *slice = malloc(sizeof(Slice));
  slice->generic.magic = CH_MAGIC;
  slice->generic.type_val = SLICE;
  slice->generic.prev_self = NULL;
  slice->generic.next_self = NULL;

  slice->elements = malloc(sizeof(GenericEl*) * cap);
  slice->capacity = cap;
  slice->length = 0;
  slice->generic.refs = 0;
  /*
   * Pop internal scratch `str` off the `String` `self` stack immediately so
   * constructing a `Slice` mid-chain does not clobber an active `String`.
   */
  slice->str = create(String, "")->$$;
  slice->str->bind((GenericEl*)-1);
  slice->bind = bind;
  slice->generic.bind = (void* (*)(GenericEl*))bind;
  slice->generic.save = save;
  slice->generic.del = del;
  slice->sub_slice = subSlice;
  slice->add = add;
  slice->set = set;
  slice->append = append;
  slice->get = get;
  slice->join = join;
  slice->foreach = foreach;
  slice->generic.to_chars = to_chars;

  self((GenericEl*)slice);

  return slice;
}
