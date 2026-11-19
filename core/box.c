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

static Box* require_slf(const char *method) {
  Box *b = slf;
  if (b == NULL) {
    fprintf(stderr,
            "Box error in %s(): no active Box on self stack "
            "(did you forget $(box)?)\n",
            method);
    abort();
  }
  return b;
}

static void* save(void) {
  Box *b = require_slf("save");
  b->generic.refs++;
  return b;
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
  Box *b = require_slf("get");
  void *raw = b->ptr;
  self((GenericEl*)-1);
  return raw;
}

static Box* reset(void *new_ptr, void (*new_dtor)(void*)) {
  Box *b = require_slf("reset");
  if (b->generic.refs > 1) {
    fprintf(stderr, "Type error: cannot reset() shared Box (refs=%d)\n",
            b->generic.refs);
    abort();
  }
  if (b->ptr != NULL && b->ptr != new_ptr && b->dtor != NULL) {
    b->dtor(b->ptr);
  }
  b->ptr = new_ptr;
  b->dtor = new_dtor;
  return b;
}

static void* take(void) {
  Box *b = require_slf("take");
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
   * `del()` pops `b` off `self`; otherwise pop `b` explicitly here.
   */
  if (b->generic.refs == 0) {
    release(b);
  } else {
    self((GenericEl*)-1);
  }
  return raw;
}

static void format_box_desc(char *buf, size_t cap, const char *tag,
                            const void *ptr) {
  const char *suffix;
  size_t suffix_len;
  size_t prefix_len = 4; /* "Box<" */
  size_t max_tag;
  size_t tag_len;
  if (cap == 0) {
    return;
  }
  if (tag == NULL) {
    const char *literal = ptr != NULL ? "Box" : "Box(NULL)";
    size_t lit_len = strlen(literal);
    if (lit_len >= cap) {
      lit_len = cap - 1;
    }
    memcpy(buf, literal, lit_len);
    buf[lit_len] = '\0';
    return;
  }
  suffix = ptr != NULL ? ">" : ">(NULL)";
  suffix_len = strlen(suffix);
  if (cap <= prefix_len + suffix_len) {
    buf[0] = '\0';
    return;
  }
  max_tag = cap - 1 - prefix_len - suffix_len;
  tag_len = strlen(tag);
  if (tag_len > max_tag) {
    tag_len = max_tag;
  }
  memcpy(buf, "Box<", prefix_len);
  memcpy(buf + prefix_len, tag, tag_len);
  memcpy(buf + prefix_len + tag_len, suffix, suffix_len);
  buf[prefix_len + tag_len + suffix_len] = '\0';
}

static char* to_chars(void) {
  Box *b = require_slf("to_chars");
  format_box_desc(b->desc, sizeof(b->desc), b->type_tag, b->ptr);
  self((GenericEl*)-1);
  return b->desc;
}

void ch_box_raw_init(ChBox *box, void *ptr, void (*dtor)(void*),
                     const char *type_tag) {
  if (box == NULL) {
    return;
  }
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
  if (expected_tag != NULL) {
    if (box->type_tag == NULL) {
      fprintf(stderr, "Type error: untagged Box cannot be accessed as %s\n",
              expected_tag);
      abort();
    }
    if (strcmp(box->type_tag, expected_tag) != 0) {
      fprintf(stderr, "Type error: Box<%s> cannot be accessed as %s\n",
              box->type_tag, expected_tag);
      abort();
    }
  }
  if (self(NULL) == box) {
    self((GenericEl*)-1);
  }
  return box->ptr;
}

/**
 * Constructor.
 */
Box* ch_new_Box(void *ptr, void (*dtor)(void*)) {
  Box *box = malloc(sizeof(Box));
  if (box == NULL) {
    if (ptr != NULL && dtor != NULL) {
      dtor(ptr);
    }
    return NULL;
  }
  ch_box_raw_init(box, ptr, dtor, NULL);
  return box;
}
