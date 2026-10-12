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

#include "core/status_or.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/string.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static StatusOr* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (StatusOr*)update_self_stack(&top, s, STATUS_OR);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static StatusOr* bind(GenericEl* status_or) {
  return self(status_or);
}

static void del(void* obj) {
  StatusOr* status_or = (StatusOr*)obj;
  /*
   * `self((GenericEl*)status_or)` verifies `obj` is a `StatusOr` and pushes it
   * onto the `StatusOr` `self` stack so `slf` resolves to `status_or`.
   */
  if (status_or != NULL && self((GenericEl*)status_or) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      if (slf->val != NULL) {
        slf->val->del(slf->val);
      }
      if (slf->err != NULL) {
        release(slf->err);
      }
      StatusOr *to_free = slf;
      /* Pop `status_or` off `self` (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * StatusOr operational functions.
 */

static int ok(void) {
  return slf->err == NULL && slf->val != NULL;
}

static char* error(void) {
  if (slf->err == NULL) {
    return "";
  }
  return $(slf->err)->chars();
}

static GenericEl* value(void) {
  if (slf->err != NULL || slf->val == NULL) {
    fprintf(stderr, "StatusOr error: %s\n", error());
    abort();
  }
  GenericEl *v = slf->val;
  /*
   * When `value()` is chained directly off an unowned `StatusOr` (such as
   * `as(Hash, from_json(s)->value())->$$`), detach `v`, undo the container ref
   * added by `claim_element`, and free the temporary `StatusOr` wrapper so the
   * caller doesn't leak the intermediate `StatusOr`.
   */
  if (slf->generic.refs == 0) {
    slf->val = NULL;
    v->refs--;
    release(slf);
  }
  return (GenericEl*)ch_bind_el(v);
}

static GenericEl* value_or(void* fallback) {
  GenericEl *fb = (GenericEl*)fallback;
  if (slf->err == NULL && slf->val != NULL) {
    if (fb != NULL && fb->refs == 0) {
      release(fb);
    }
    return value();
  }
  /* Free an unowned error `StatusOr` before returning `fallback` on `self`. */
  if (slf->generic.refs == 0) {
    release(slf);
  }
  return (GenericEl*)ch_bind_el(fb);
}

static char* to_chars(void) {
  StatusOr *status_or = slf;
  if (status_or->err != NULL) {
    return $(status_or->err)->chars();
  }
  if (status_or->val != NULL) {
    return ((GenericEl*)ch_bind_el(status_or->val))->to_chars();
  }
  return "";
}

/**
 * Constructor.
 */
StatusOr* ch_new_StatusOr(void* val, char* err_msg) {
  StatusOr* status_or = malloc(sizeof(StatusOr));
  status_or->generic.magic = CH_MAGIC;
  status_or->generic.type_val = STATUS_OR;
  status_or->generic.prev_self = NULL;
  status_or->generic.next_self = NULL;
  status_or->generic.refs = 0;

  if (err_msg != NULL) {
    status_or->val = NULL;
    /*
     * Pop the internal `err` `String` off the `String` `self` stack so
     * constructing an error `StatusOr` does not clobber an active `String`.
     */
    status_or->err = create(String, err_msg)->$$;
    status_or->err->bind((GenericEl*)-1);
  } else {
    claim_element(val, status_or);
    status_or->val = (GenericEl*)val;
    status_or->err = NULL;
  }

  status_or->ok = ok;
  status_or->value = value;
  status_or->value_or = value_or;
  status_or->error = error;
  status_or->generic.to_chars = to_chars;
  status_or->bind = bind;
  status_or->generic.bind = (void* (*)(GenericEl*))bind;
  status_or->generic.save = save;
  status_or->generic.del = del;

  self((GenericEl*)status_or);

  return status_or;
}
