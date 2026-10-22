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

#include "core/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/slice.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static String* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (String*)update_self_stack(&top, s, STRING);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static String* bind(GenericEl* string) {
  return self(string);
}

static void del(void* obj) {
  String* string = (String*)obj;
  /*
   * `self((GenericEl*)string)` verifies `obj` is a `String` and pushes it onto
   * the `String` `self` stack so `slf` resolves to `string`.
   */
  if (string != NULL && self((GenericEl*)string) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      String *to_free = slf;
      /* Pop `string` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free->chars);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * String operational functions.
 */

static int length(void) { return strlen(slf->chars); }

static void set(char *s) {
  String *str = slf;
  if (strlen(str->chars) < strlen(s)) {
    str->chars = realloc(str->chars, sizeof(char) * (strlen(s) + 1));
  }
  strcpy(str->chars, s);
  str->size = strlen(s) + 1;
}

static Slice* split(char *delim) {
  String *str = slf->generic.save();
  /*
   * Copy `str->chars` into a scratch buffer and scan with `strspn`/`strcspn`
   * instead of `strtok` so `split()` is re-entrant and never mutates the source
   * `String` in place.
   */
  char *copy = malloc(strlen(str->chars) + 1);
  strcpy(copy, str->chars);
  /*
   * Release `str` now that its characters have been copied: if `split()` was
   * chained off an unowned temporary (`refs` was 0), this frees `str`; if
   * `str` was a saved `String` (`refs > 0`), this pops its temporary `self`
   * stack entry while preserving its caller reference count.
   */
  release(str);

  Slice *slice = create(Slice, 10);
  char *p = copy;
  while (*p != '\0') {
    /* Skip any run of leading delimiter characters. */
    p += strspn(p, delim);
    if (*p == '\0') {
      break;
    }
    /* Find the end of the current token and temporarily null-terminate it. */
    size_t tok_len = strcspn(p, delim);
    char saved = p[tok_len];
    p[tok_len] = '\0';
    slice->add(create(String, p));
    p += tok_len;
    if (saved != '\0') {
      p++;
    }
  }
  free(copy);

  return $(slice);
}

static String* append(char *suffix) {
  /* Attempt to realloc the string to avoid reallocation. */
  int newLength = slf->length() + strlen(suffix) + 1;
  String *str = slf;
  char *newchars = realloc(str->chars, sizeof(char) * newLength);
  if (newchars == NULL) {
    char *newchars = malloc(sizeof(char) * newLength);
    strcpy(newchars, str->chars);
    strcat(newchars, suffix);
    free(str->chars);
    str->chars = newchars;
  } else {
    str->chars = newchars;
    strcat(str->chars, suffix);
  }
  str->size = newLength;
  return str;
}

static String* append_escaped(char *suffix) {
  String *str = slf;
  int orig_len = str->length();
  int extra_len = 0;
  char *p;
  /*
   * Pass 1: compute the exact byte length of `suffix` after JSON escaping so
   * `str->chars` only needs a single `realloc`.
   */
  for (p = suffix; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') {
      extra_len += 2;
    } else {
      extra_len += 1;
    }
  }
  int new_size = orig_len + extra_len + 1;
  str->chars = realloc(str->chars, sizeof(char) * new_size);

  /* Pass 2: append `suffix` to `str->chars`, expanding escape sequences. */
  char *dst = str->chars + orig_len;
  for (p = suffix; *p != '\0'; p++) {
    switch (*p) {
      case '"':  *dst++ = '\\'; *dst++ = '"';  break;
      case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
      case '\n': *dst++ = '\\'; *dst++ = 'n';  break;
      case '\r': *dst++ = '\\'; *dst++ = 'r';  break;
      case '\t': *dst++ = '\\'; *dst++ = 't';  break;
      default:   *dst++ = *p;                  break;
    }
  }
  *dst = '\0';
  str->size = new_size;
  return str;
}

static char* to_chars(void) {
  String *str = slf;
  if (str->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return str->chars;
}

/**
 * Constructor.
 */
String *ch_new_String(char *s) {
    String *str = malloc(sizeof(String));
    str->generic.magic = CH_MAGIC;
    str->generic.type_val = STRING;
    str->generic.prev_self = NULL;
    str->generic.next_self = NULL;

    str->chars = malloc(sizeof(char) * (strlen(s) + 1));
    str->size = strlen(s) + 1;
    str->length = length;
    str->set = set;
    str->generic.refs = 0;
    str->split = split;
    str->append = append;
    str->append_escaped = append_escaped;
    str->generic.to_chars = to_chars;
    str->bind = bind;
    str->generic.bind = (void* (*)(GenericEl*))bind;
    str->generic.save = save;
    str->generic.del = del;
    strcpy(str->chars, s);

    self((GenericEl*)str);

    return str;
}
