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

#include "core/null.h"

#include <stdlib.h>

#include "core/types.h"

/**
 * Object handling functions.
 */

static Null* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Null*)update_self_stack(&top, s, NULL_VAL);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Null* bind(GenericEl* n) {
  return self(n);
}

static void del(void* obj) {
  Null* n = (Null*)obj;
  /*
   * `self((GenericEl*)n)` verifies `obj` is a `Null` and pushes it onto the
   * `Null` `self` stack so `slf` resolves to `n`.
   */
  if (n != NULL && self((GenericEl*)n) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      Null *to_free = slf;
      /* Pop `n` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

static char* to_chars(void) {
  return "null";
}

/**
 * Constructor.
 */
Null* ch_new_Null(void) {
  Null* n = malloc(sizeof(Null));
  n->generic.magic = CH_MAGIC;
  n->generic.type_val = NULL_VAL;
  n->generic.prev_self = NULL;
  n->generic.next_self = NULL;

  n->generic.refs = 0;
  n->generic.to_chars = to_chars;
  n->bind = bind;
  n->generic.bind = (void* (*)(GenericEl*))bind;
  n->generic.save = save;
  n->generic.del = del;

  self((GenericEl*)n);

  return n;
}
