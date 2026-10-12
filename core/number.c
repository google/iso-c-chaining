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

#include "core/number.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"

/**
 * Object handling functions.
 */

static Number* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Number*)update_self_stack(&top, s, NUMBER);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Number* bind(GenericEl* number) {
  return self(number);
}

static void del(void* obj) {
  Number* number = (Number*)obj;
  /*
   * `self((GenericEl*)number)` verifies `obj` is a `Number` and pushes it onto
   * the `Number` `self` stack so `slf` resolves to `number`.
   */
  if (number != NULL && self((GenericEl*)number) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      Number *to_free = slf;
      /* Pop `number` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Number operational functions.
 */

static double value(void) { return slf->number; }

static void set(double number) { slf->number = number; }

static void from_chars(char* s) { slf->number = atof(s); }

static char* to_chars(void) {
  Number* number = slf;
  if (number->number == floor(number->number)) {
    /* Format exact whole numbers without a fractional part. */
    snprintf(number->chars, sizeof(number->chars), "%.0f", number->number);
  } else {
    /*
     * Format fractional values to 8 decimal places, then strip trailing `'0'`s
     * and any orphaned trailing `'.'` (e.g. `1.000000001` -> `"1"`).
     */
    snprintf(number->chars, sizeof(number->chars), "%.8f", number->number);
    if (strchr(number->chars, '.') != NULL) {
      size_t len = strlen(number->chars);
      while (len > 0 && number->chars[len - 1] == '0') {
        number->chars[len - 1] = '\0';
        len--;
      }
      if (len > 0 && number->chars[len - 1] == '.') {
        number->chars[len - 1] = '\0';
      }
    }
  }
  /* Normalize negative zero (`"-0"`) to `"0"`. */
  if (strcmp(number->chars, "-0") == 0) {
    strcpy(number->chars, "0");
  }
  return number->chars;
}

/**
 * Constructor.
 */
Number* ch_new_Number(double num) {
  Number* number = malloc(sizeof(Number));
  number->generic.magic = CH_MAGIC;
  number->generic.type_val = NUMBER;
  number->generic.prev_self = NULL;
  number->generic.next_self = NULL;

  number->number = num;
  number->chars[0] = '\0';
  number->value = value;
  number->set = set;
  number->from_chars = from_chars;
  number->generic.refs = 0;
  number->generic.to_chars = to_chars;
  number->bind = bind;
  number->generic.bind = (void* (*)(GenericEl*))bind;
  number->generic.save = save;
  number->generic.del = del;

  self((GenericEl*)number);

  return number;
}
