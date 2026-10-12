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

#include "core/boolean.h"

#include <stdlib.h>
#include <string.h>

#include "core/types.h"

/**
 * Object handling functions.
 */

static Boolean* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Boolean*)update_self_stack(&top, s, BOOLEAN);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Boolean* bind(GenericEl* boolean) {
  return self(boolean);
}

static void del(void* obj) {
  Boolean* boolean = (Boolean*)obj;
  /*
   * `self((GenericEl*)boolean)` verifies `obj` is a `Boolean` and pushes it
   * onto the `Boolean` `self` stack so `slf` resolves to `boolean`.
   */
  if (boolean != NULL && self((GenericEl*)boolean) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      Boolean *to_free = slf;
      /* Pop `boolean` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Boolean operational functions.
 */

static int value(void) { return slf->boolean; }

static Boolean* set(int b) {
  slf->boolean = b ? 1 : 0;
  return $(slf);
}

static Boolean* negate(void) {
  slf->boolean = !slf->boolean;
  return $(slf);
}

static void from_chars(char* s) {
  slf->boolean = (s != NULL && strcmp(s, "true") == 0) ? 1 : 0;
}

static char* to_chars(void) {
  return slf->boolean ? "true" : "false";
}

/**
 * Constructor.
 */
Boolean* ch_new_Boolean(int b) {
  Boolean* boolean = malloc(sizeof(Boolean));
  boolean->generic.magic = CH_MAGIC;
  boolean->generic.type_val = BOOLEAN;
  boolean->generic.prev_self = NULL;
  boolean->generic.next_self = NULL;

  boolean->boolean = b ? 1 : 0;
  boolean->value = value;
  boolean->set = set;
  boolean->negate = negate;
  boolean->from_chars = from_chars;
  boolean->generic.refs = 0;
  boolean->generic.to_chars = to_chars;
  boolean->bind = bind;
  boolean->generic.bind = (void* (*)(GenericEl*))bind;
  boolean->generic.save = save;
  boolean->generic.del = del;

  self((GenericEl*)boolean);

  return boolean;
}
