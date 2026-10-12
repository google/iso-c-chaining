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

#include <stdlib.h>
#include <string.h>

#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/string.h"
#include "core/types.h"


/**
 * Internal functions.
 */

/*
 * Daniel J. Bernstein's "djb2" string hash function (originally posted to
 * comp.lang.c in 1991), using the XOR variant (`djb2a`). Chosen for its tiny
 * footprint and good avalanche distribution across short ASCII keys.
 */
static unsigned long djb2hash(char *s) {
  unsigned long hash = 5381;
  int c;

  while ((c = *s++)) {
    hash = hash * 33 ^ c;
  }

  return hash;
}

/*
 * Open-addressed linear probing. Because `put()` resizes the table before load
 * reaches 50%, at least half the buckets are always `NULL`, guaranteeing that
 * this search terminates on either a matching key or an empty insertion slot.
 */
static int find_slot(Hash *h, char *key) {
  unsigned long index = djb2hash(key) % h->capacity;
  while (h->bucket_keys[index] != NULL &&
         strcmp(h->bucket_keys[index], key) != 0) {
    index = (index + 1) % h->capacity;
  }
  return (int)index;
}

/*
 * Key strings are owned by `h->key_slice`, so `bucket_keys` only stores
 * non-owning aliases to `String->chars` and can be re-indexed on resize
 * without copying or freeing string buffers.
 */
static void resize_hash(Hash *h) {
  int old_cap = h->capacity;
  GenericEl **old_elements = h->elements;
  char **old_keys = h->bucket_keys;
  int i;

  h->capacity = old_cap * 2;
  h->elements = calloc(h->capacity, sizeof(GenericEl*));
  h->bucket_keys = calloc(h->capacity, sizeof(char*));

  for (i = 0; i < old_cap; i++) {
    if (old_keys[i] != NULL && old_elements[i] != NULL) {
      int idx = find_slot(h, old_keys[i]);
      h->bucket_keys[idx] = old_keys[i];
      h->elements[idx] = old_elements[i];
    }
  }
  free(old_elements);
  free(old_keys);
}

/**
 * Object handling functions.
 */

static Hash* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Hash*)update_self_stack(&top, s, HASH);
}

static Hash* bind(GenericEl* hash) {
  return self(hash);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static void del(void* obj) {
  Hash* hash = (Hash*)obj;
  int i;
  /*
   * `self((GenericEl*)hash)` verifies `obj` is a `Hash` and pushes it onto the
   * `Hash` `self` stack so `slf` resolves to `hash` during cleanup.
   */
  if (hash != NULL && self((GenericEl*)hash) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      /* Release every stored value, then the key list and scratch buffer. */
      for (i = 0; i < hash->capacity; i++) {
        if (hash->bucket_keys[i] != NULL && hash->elements[i] != NULL) {
          GenericEl *el = hash->elements[i];
          el->del(el);
        }
      }
      release(hash->key_slice);
      release(hash->str);
      /* Pop `hash` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)hash);
      self((GenericEl*)-1);
      free(hash->elements);
      free(hash->bucket_keys);
      free(hash);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Hash operational functions.
 */

static GenericEl* get(char* key) {
  Hash *h = slf;
  int index = find_slot(h, key);
  if (h->generic.prev_self != NULL) {
    self((GenericEl*)-1);
  }
  return (GenericEl*)ch_bind_el(h->elements[index]);
}

static Hash* put(char* key, void* s) {
  /*
   * If `s` was created inline (e.g. an inner `create(Hash, ...)` inside
   * `put()` arguments), `claim_element` pops `s` off its `self` stack so `slf`
   * below refers back to the outer `Hash` rather than the inner one.
   */
  claim_element(s, slf);
  /* Grow the hash table before load factor reaches 50%. */
  if ((slf->key_slice->length + 1) * 2 >= slf->capacity) {
    resize_hash(slf);
  }
  int index = find_slot(slf, key);
  if (slf->bucket_keys[index] == NULL) {
    /* New key: append a `String` copy to `key_slice` and alias its `chars`. */
    String *key_str = create(String, key);
    slf->keys()->add(key_str);
    /*
     * `slf->keys()` bound `key_slice` to the `Slice` `self` stack; pop it so
     * `Hash::put` does not clobber an outer `Slice` chain.
     */
    slf->key_slice->bind((GenericEl*)-1);
    slf->bucket_keys[index] = key_str->chars;
  } else if (slf->elements[index] != NULL) {
    /* Existing key: release the old value before overwriting the slot. */
    slf->elements[index]->del(slf->elements[index]);
  }
  slf->elements[index] = s;
  return $(slf);
}

static void remove_key(char* key) {
  int index = find_slot(slf, key);
  if (slf->bucket_keys[index] == NULL) {
    return;
  }
  /* Release the value stored at `index` and clear the bucket slot. */
  if (slf->elements[index] != NULL) {
    slf->elements[index]->del(slf->elements[index]);
    slf->elements[index] = NULL;
  }
  slf->bucket_keys[index] = NULL;

  /*
   * Remove and release the key String from key_slice, shifting remaining keys
   * left to keep the key list contiguous.
   */
  int i;
  for (i = 0; i < slf->key_slice->length; i++) {
    String *k = (String*)slf->key_slice->elements[i];
    if (strcmp(k->chars, key) == 0) {
      release(k);
      for (; i < slf->key_slice->length - 1; i++) {
        slf->key_slice->elements[i] = slf->key_slice->elements[i + 1];
      }
      slf->key_slice->length--;
      break;
    }
  }

  /*
   * Re-insert any subsequent keys in the same linear-probe cluster so that
   * colliding keys displaced past the removed slot remain reachable by
   * find_slot() without leaving a gap in the probe chain.
   */
  int next = (index + 1) % slf->capacity;
  while (slf->bucket_keys[next] != NULL) {
    char *rehash_key = slf->bucket_keys[next];
    GenericEl *rehash_el = slf->elements[next];
    slf->bucket_keys[next] = NULL;
    slf->elements[next] = NULL;
    int new_idx = find_slot(slf, rehash_key);
    slf->bucket_keys[new_idx] = rehash_key;
    slf->elements[new_idx] = rehash_el;
    next = (next + 1) % slf->capacity;
  }
}

static Slice* keys(void) {
  return $(slf->key_slice);
}

static void foreach(void (*run)(char*, GenericEl*)) {
  int i;
  Hash *hash = slf;
  for (i = 0; i < hash->key_slice->length; i++) {
    char *key = ((String*)hash->key_slice->elements[i])->chars;
    /* Re-bind `hash` each iteration in case `run()` touches another `Hash`. */
    run(key, $(hash)->get(key));
  }
  $(hash);
}

static char* to_chars(void) {
  int i;
  Hash *hash = slf;
  /* Reset the reusable scratch buffer before formatting the JSON object. */
  $(hash->str)->set("");
  $(hash->str)->append("{");
  for (i = 0; i < hash->key_slice->length; i++) {
    if (i > 0) {
      $(hash->str)->append(",");
    }
    /*
     * Re-bind `hash` before each lookup because nested `Hash` values share the
     * `Hash` `self` stack when their own `to_chars()` runs.
     */
    char *key = ((String*)hash->key_slice->elements[i])->chars;
    char *value = $(hash)->get(key)->to_chars();
    $(hash->str)->append("\"")->append_escaped(key)->append("\"")
        ->append(":");
    /* Wrap `STRING` values in quotes; other types already format as JSON. */
    if (is_type($(hash)->get(key), STRING)) {
      $(hash->str)->append("\"")->append_escaped(value)->append("\"");
    } else {
      $(hash->str)->append(value);
    }
  }
  return $(hash->str)->append("}")->generic.to_chars();
}

/**
 * Constructor.
 */
Hash* ch_new_Hash(int n) {
  /* Double requested capacity so `n` items fit under the 50% load threshold. */
  int cap = (n > 0 ? n : 4) * 2;
  Hash *hash = malloc(sizeof(Hash));
  hash->generic.magic = CH_MAGIC;
  hash->generic.type_val = HASH;
  hash->generic.prev_self = NULL;
  hash->generic.next_self = NULL;

  hash->elements = calloc(cap, sizeof(GenericEl*));
  hash->bucket_keys = calloc(cap, sizeof(char*));
  hash->capacity = cap;
  /*
   * Pop internal `key_slice` and `str` off the `Slice` and `String` `self`
   * stacks immediately after creation so constructing a `Hash` mid-chain does
   * not overwrite the caller's active `Slice` or `String`.
   */
  hash->key_slice = create(Slice, n)->$$;
  hash->key_slice->bind((GenericEl*)-1);
  hash->str = create(String, "")->$$;
  hash->str->bind((GenericEl*)-1);
  hash->generic.refs = 0;
  hash->keys = keys;
  hash->bind = bind;
  hash->generic.bind = (void* (*)(GenericEl*))bind;
  hash->generic.save = save;
  hash->generic.del = del;
  hash->put = put;
  hash->get = get;
  hash->remove = remove_key;
  hash->foreach = foreach;
  hash->generic.to_chars = to_chars;

  self((GenericEl*)hash);

  return hash;
}
