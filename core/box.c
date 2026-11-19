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

#include "core/box.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"

/**
 * Object handling functions.
 */

static Box* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Box*)update_self_stack(&top, s, BOX);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Box* bind(GenericEl *box) {
  return self(box);
}

static void del(void *obj) {
  Box *box = (Box*)obj;
  if (box != NULL && self((GenericEl*)box) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      void *raw = slf->ptr;
      void (*dtor)(void*) = slf->dtor;
      Box *to_free = slf;
      slf->ptr = NULL;
      slf->dtor = NULL;
      self((GenericEl*)-1);
      if (raw != NULL && dtor != NULL) {
        dtor(raw);
      }
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * Box operational functions.
 */

static void* get(void) {
  return slf->ptr;
}

static Box* reset(void *new_ptr, void (*new_dtor)(void*)) {
  Box *b = slf;
  if (b->ptr != NULL && b->ptr != new_ptr && b->dtor != NULL) {
    b->dtor(b->ptr);
  }
  b->ptr = new_ptr;
  b->dtor = new_dtor;
  return b;
}

static void* take(void) {
  Box *b = slf;
  void *raw;
  if (b->generic.refs > 1) {
    fprintf(stderr, "Type error: cannot take() shared Box (refs=%d)\n",
            b->generic.refs);
    abort();
  }
  raw = b->ptr;
  b->ptr = NULL;
  b->dtor = NULL;
  /*
   * When called on an unowned temporary (`refs == 0`), free the `Box` shell so
   * `$(box_of(...))->take()` returns the raw pointer without leaking the shell.
   */
  if (b->generic.refs == 0) {
    release(b);
  } else if (b->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return raw;
}

static char* to_chars(void) {
  Box *b = slf;
  if (b->type_tag != NULL) {
    if (b->ptr != NULL) {
      snprintf(b->desc, sizeof(b->desc), "Box<%s>", b->type_tag);
    } else {
      snprintf(b->desc, sizeof(b->desc), "Box<%s>(NULL)", b->type_tag);
    }
  } else if (b->ptr != NULL) {
    snprintf(b->desc, sizeof(b->desc), "Box");
  } else {
    snprintf(b->desc, sizeof(b->desc), "Box(NULL)");
  }
  if (b->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return b->desc;
}

void ch_box_raw_init(ChBox *box, void *ptr, void (*dtor)(void*),
                     const char *type_tag) {
  box->generic.magic = CH_MAGIC;
  box->generic.type_val = BOX;
  box->generic.prev_self = NULL;
  box->generic.next_self = NULL;
  box->generic.refs = 0;

  box->ptr = ptr;
  box->dtor = dtor;
  box->type_tag = type_tag;
  box->desc[0] = '\0';

  box->bind = bind;
  box->generic.bind = (void* (*)(GenericEl*))bind;
  box->generic.save = save;
  box->generic.del = del;
  box->generic.to_chars = to_chars;
  box->get = get;
  box->reset = reset;
  box->take = take;

  self((GenericEl*)box);
}

ChBox* ch_box_raw_tag(ChBox *box, const char *type_tag) {
  if (box != NULL) {
    box->type_tag = type_tag;
  }
  return box;
}

ChBox* ch_box_raw_bind(ChGenericEl *el) {
  return bind(el);
}

void* ch_box_raw_get(void) {
  return get();
}

void* ch_box_raw_take(void) {
  return take();
}

ChBox* ch_box_raw_reset(void *ptr, void (*dtor)(void*)) {
  return reset(ptr, dtor);
}

void* ch_box_raw_as(ChGenericEl *el, const char *expected_tag) {
  ChGenericEl *checked = ch_check_el(el, "box_as");
  ChBox *box;
  if (checked == NULL) {
    return NULL;
  }
  if (checked->type_val != BOX) {
    fprintf(stderr, "Type error: %p is not a Box\n", (void*)checked);
    abort();
  }
  box = (ChBox*)checked;
  if (expected_tag != NULL && box->type_tag != NULL &&
      strcmp(box->type_tag, expected_tag) != 0) {
    fprintf(stderr, "Type error: Box<%s> cannot be accessed as %s\n",
            box->type_tag, expected_tag);
    abort();
  }
  return box->ptr;
}

/**
 * Constructor.
 */
Box* ch_new_Box(void *ptr, void (*dtor)(void*)) {
  Box *box = malloc(sizeof(Box));
  ch_box_raw_init(box, ptr, dtor, NULL);
  return box;
}
