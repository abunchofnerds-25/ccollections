/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file bench_maps.c
 * @brief Benchmarks for chashmap and cbstmap.
 *
 * chashmap picks its implementation from the key and value types: an integral
 * key and value of at most eight bytes get open addressing, and everything
 * else, string keys included, gets separate chaining. Both paths are measured,
 * because a change can easily improve one and regress the other.
 *
 * Where GLib is installed, GHashTable and GTree are measured on the identical
 * workload. They are reported for scale rather than as a target: GHashTable
 * stores pointers and leaves key ownership to the caller, while chashmap
 * copies keys and values into its own storage, so the two are doing different
 * amounts of work for the same call.
 */

#include <cbstmap.h>
#include <chashmap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#ifdef BENCH_HAVE_GLIB
#include <glib.h>
#endif

#define BENCH_MAP_N 100000

/* Keys are drawn once, deterministically, and shared by every map case so that
 * the implementations are compared on identical input. */
typedef struct {
  int *keys;
  size_t count;
  char (*str_keys)[16];
} keyset_t;

static keyset_t *keyset_create(size_t n, bool with_strings) {
  keyset_t *ks = calloc(1, sizeof *ks);
  if (!ks) return NULL;
  ks->count = n;
  ks->keys = malloc(sizeof(int) * n);
  if (!ks->keys) {
    free(ks);
    return NULL;
  }
  uint64_t seed = 0xd1b54a32d192ed03ULL;
  for (size_t i = 0; i < n; i++)
    ks->keys[i] = (int)(bench_rand(&seed) & 0x7fffffff);
  if (with_strings) {
    ks->str_keys = malloc(sizeof(*ks->str_keys) * n);
    if (!ks->str_keys) {
      free(ks->keys);
      free(ks);
      return NULL;
    }
    for (size_t i = 0; i < n; i++)
      snprintf(ks->str_keys[i], sizeof(*ks->str_keys), "key_%08x", ks->keys[i]);
  }
  return ks;
}

static void keyset_destroy(keyset_t *ks) {
  if (!ks) return;
  free(ks->keys);
  free(ks->str_keys);
  free(ks);
}

/* ------------------------------------------------------------------------ */
/* chashmap, open addressing (integral key and value)                        */
/* ------------------------------------------------------------------------ */

typedef struct {
  chmap m;
  keyset_t *ks;
  long long expected_acc;
} hmap_state_t;

static void hmap_teardown(void *state) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_destroy(m);
  keyset_destroy(st->ks);
  free(st);
}

static void *hmap_int_empty_setup(size_t n) {
  hmap_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, false);
  if (!st->ks) {
    free(st);
    return NULL;
  }
  chmap_construct(m, int, int);
  st->m = m;
  return st;
}

static void hmap_int_insert_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, int, int);
  /* The value goes through a named local because the type-safe macros take
   * the address of both key and value, so neither may be an rvalue. */
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    chmap_insert(m, st->ks->keys[i], v);
  }
  bench_sink(m);
}

static void *hmap_int_filled_setup(size_t n) {
  hmap_state_t *st = hmap_int_empty_setup(n);
  if (!st) return NULL;
  chmap m = st->m;
  chmap_redeclare(m, int, int);
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    chmap_insert(m, st->ks->keys[i], v);
  }
  for (size_t i = 0; i < n; i++) {
    int *p = chmap_get_ptr(m, st->ks->keys[i]);
    if (p) st->expected_acc += *p;
  }
  return st;
}

/* Every lookup arm checks its accumulated sum against one computed in untimed
   setup by the same traversal. A miss is far cheaper than a hit, so a defect
   that stopped keys being found reports as a large speedup under a name
   promising the opposite, which is the shape of result nobody questions. The
   keys are random rather than sequential and so contain duplicates, which is
   why the expected sum is measured rather than derived in closed form: a
   duplicated key keeps only the last value inserted for it. */
static void hmap_int_lookup_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, int, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int *p = chmap_get_ptr(m, st->ks->keys[i]);
    if (p) acc += *p;
  }
  if (acc != st->expected_acc) bench_die("chashmap int lookup missed");
  bench_sink(&acc);
}

/* Every lookup misses, which is the path that probes the longest before it can
 * conclude the key is absent. */
static void hmap_int_lookup_miss_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, int, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int miss_key = -(int)i - 1;
    int *p = chmap_get_ptr(m, miss_key);
    if (p) acc += *p;
  }
  bench_sink(&acc);
}

static void hmap_int_remove_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, int, int);
  for (size_t i = 0; i < n; i++) chmap_remove(m, st->ks->keys[i]);
  bench_sink(m);
}

/* ------------------------------------------------------------------------ */
/* chashmap, separate chaining (string key)                                  */
/* ------------------------------------------------------------------------ */

static void *hmap_str_empty_setup(size_t n) {
  hmap_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, true);
  if (!st->ks) {
    free(st);
    return NULL;
  }
  chmap_construct(m, char *, int);
  st->m = m;
  return st;
}

static void hmap_str_insert_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, char *, int);
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    chmap_insert(m, st->ks->str_keys[i], v);
  }
  bench_sink(m);
}

static void *hmap_str_filled_setup(size_t n) {
  hmap_state_t *st = hmap_str_empty_setup(n);
  if (!st) return NULL;
  chmap m = st->m;
  chmap_redeclare(m, char *, int);
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    chmap_insert(m, st->ks->str_keys[i], v);
  }
  for (size_t i = 0; i < n; i++) {
    int *p = chmap_get_ptr(m, st->ks->str_keys[i]);
    if (p) st->expected_acc += *p;
  }
  return st;
}

static void hmap_str_lookup_run(void *state, size_t n) {
  hmap_state_t *st = state;
  chmap m = st->m;
  chmap_redeclare(m, char *, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int *p = chmap_get_ptr(m, st->ks->str_keys[i]);
    if (p) acc += *p;
  }
  if (acc != st->expected_acc) bench_die("chashmap string lookup missed");
  bench_sink(&acc);
}

/* ------------------------------------------------------------------------ */
/* cbstmap                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
  cbmap m;
  keyset_t *ks;
  long long expected_acc;
} bmap_state_t;

static void bmap_teardown(void *state) {
  bmap_state_t *st = state;
  cbmap m = st->m;
  cbmap_destroy(m);
  keyset_destroy(st->ks);
  free(st);
}

static void *bmap_empty_setup(size_t n) {
  bmap_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, false);
  if (!st->ks) {
    free(st);
    return NULL;
  }
  cbmap_construct(m, int, int);
  st->m = m;
  return st;
}

static void bmap_insert_run(void *state, size_t n) {
  bmap_state_t *st = state;
  cbmap m = st->m;
  cbmap_redeclare(m, int, int);
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    cbmap_insert(m, st->ks->keys[i], v);
  }
  bench_sink(m);
}

static void *bmap_filled_setup(size_t n) {
  bmap_state_t *st = bmap_empty_setup(n);
  if (!st) return NULL;
  cbmap m = st->m;
  cbmap_redeclare(m, int, int);
  for (size_t i = 0; i < n; i++) {
    int v = (int)i;
    cbmap_insert(m, st->ks->keys[i], v);
  }
  for (size_t i = 0; i < n; i++) {
    int *p = cbmap_get_ptr(m, st->ks->keys[i]);
    if (p) st->expected_acc += *p;
  }
  return st;
}

static void bmap_lookup_run(void *state, size_t n) {
  bmap_state_t *st = state;
  cbmap m = st->m;
  cbmap_redeclare(m, int, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int *p = cbmap_get_ptr(m, st->ks->keys[i]);
    if (p) acc += *p;
  }
  if (acc != st->expected_acc) bench_die("cbstmap lookup missed");
  bench_sink(&acc);
}

/* ------------------------------------------------------------------------ */
/* GLib comparisons                                                          */
/* ------------------------------------------------------------------------ */

#ifdef BENCH_HAVE_GLIB

typedef struct {
  GHashTable *ht;
  GTree *tree;
  keyset_t *ks;
  long long expected_acc;
} glib_state_t;

static void glib_teardown(void *state) {
  glib_state_t *st = state;
  if (st->ht) g_hash_table_destroy(st->ht);
  if (st->tree) g_tree_destroy(st->tree);
  keyset_destroy(st->ks);
  free(st);
}

static void *glib_ht_empty_setup(size_t n) {
  glib_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, false);
  if (!st->ks) {
    free(st);
    return NULL;
  }
  st->ht = g_hash_table_new(g_direct_hash, g_direct_equal);
  return st;
}

static void glib_ht_insert_run(void *state, size_t n) {
  glib_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    g_hash_table_insert(st->ht, GINT_TO_POINTER(st->ks->keys[i]),
                        GINT_TO_POINTER((int)i));
  bench_sink(st->ht);
}

static void *glib_ht_filled_setup(size_t n) {
  glib_state_t *st = glib_ht_empty_setup(n);
  if (!st) return NULL;
  glib_ht_insert_run(st, n);
  for (size_t i = 0; i < n; i++)
    st->expected_acc += GPOINTER_TO_INT(
        g_hash_table_lookup(st->ht, GINT_TO_POINTER(st->ks->keys[i])));
  return st;
}

static void glib_ht_lookup_run(void *state, size_t n) {
  glib_state_t *st = state;
  long long acc = 0;
  for (size_t i = 0; i < n; i++)
    acc += GPOINTER_TO_INT(
        g_hash_table_lookup(st->ht, GINT_TO_POINTER(st->ks->keys[i])));
  if (acc != st->expected_acc) bench_die("GHashTable lookup missed");
  bench_sink(&acc);
}

static gint glib_int_cmp(gconstpointer a, gconstpointer b, gpointer u) {
  (void)u;
  int x = GPOINTER_TO_INT(a), y = GPOINTER_TO_INT(b);
  return (x > y) - (x < y);
}

static void *glib_tree_empty_setup(size_t n) {
  glib_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, false);
  if (!st->ks) {
    free(st);
    return NULL;
  }
  st->tree = g_tree_new_full(glib_int_cmp, NULL, NULL, NULL);
  return st;
}

static void glib_tree_insert_run(void *state, size_t n) {
  glib_state_t *st = state;
  for (size_t i = 0; i < n; i++)
    g_tree_insert(st->tree, GINT_TO_POINTER(st->ks->keys[i]),
                  GINT_TO_POINTER((int)i));
  bench_sink(st->tree);
}

static void *glib_tree_filled_setup(size_t n) {
  glib_state_t *st = glib_tree_empty_setup(n);
  if (!st) return NULL;
  glib_tree_insert_run(st, n);
  for (size_t i = 0; i < n; i++)
    st->expected_acc += GPOINTER_TO_INT(
        g_tree_lookup(st->tree, GINT_TO_POINTER(st->ks->keys[i])));
  return st;
}

static void glib_tree_lookup_run(void *state, size_t n) {
  glib_state_t *st = state;
  long long acc = 0;
  for (size_t i = 0; i < n; i++)
    acc += GPOINTER_TO_INT(
        g_tree_lookup(st->tree, GINT_TO_POINTER(st->ks->keys[i])));
  if (acc != st->expected_acc) bench_die("GTree lookup missed");
  bench_sink(&acc);
}

#endif /* BENCH_HAVE_GLIB */

/* ------------------------------------------------------------------------ */
/* uthash comparison                                                         */
/* ------------------------------------------------------------------------ */

#ifdef BENCH_HAVE_UTHASH
#include <uthash.h>

typedef struct uth_entry {
  int key;
  int val;
  UT_hash_handle hh;
} uth_entry_t;

/* A separate entry type for the string-keyed arm: uthash keys a table on one
   named member, so a table keyed by string needs its own. The key bytes are
   carried inline, matching how the map this is compared against stores a short
   key, so the key COPY is matched on both sides.

   The node allocation is not, and this is the one row where that matters. A
   string key puts chashmap on its separate-chaining backend, which allocates a
   chain node per insert inside the timed loop, where these entries come from a
   block obtained in untimed setup. The insert_str_int row therefore charges
   this library an allocation per operation that uthash is not charged, so it
   understates this library rather than flattering it. */
typedef struct uth_str_entry {
  char key[16];
  int val;
  UT_hash_handle hh;
} uth_str_entry_t;

typedef struct {
  uth_entry_t *head;
  uth_entry_t *pool;
  uth_str_entry_t *str_head;
  uth_str_entry_t *str_pool;
  keyset_t *ks;
  long long expected_acc;
} uth_state_t;

static void uth_teardown(void *state) {
  uth_state_t *st = state;
  uth_str_entry_t *se, *stmp;
  HASH_ITER(hh, st->str_head, se, stmp) { HASH_DEL(st->str_head, se); }
  free(st->str_pool);
  uth_entry_t *e, *tmp;
  HASH_ITER(hh, st->head, e, tmp) { HASH_DEL(st->head, e); }
  free(st->pool);
  keyset_destroy(st->ks);
  free(st);
}

/* The entry storage is preallocated as one block so that the measured loop
 * contains uthash's own work rather than a malloc per insertion. On the
 * integer-keyed rows neither side pays a per-entry allocation: both grow their
 * table inside the timed loop a handful of times and copy entries into it,
 * which is a cost proportional to the number of GROWTHS rather than to the
 * number of inserts. On the string-keyed rows the two do differ; see
 * uth_str_entry_t's own comment for which way that leans. */
static void *uth_empty_setup(size_t n) {
  uth_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, false);
  st->pool = calloc(n, sizeof(uth_entry_t));
  if (!st->ks || !st->pool) {
    uth_teardown(st);
    return NULL;
  }
  return st;
}

static void *uth_str_empty_setup(size_t n) {
  uth_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->ks = keyset_create(n, true);
  st->str_pool = calloc(n, sizeof(uth_str_entry_t));
  if (!st->ks || !st->str_pool) {
    uth_teardown(st);
    return NULL;
  }
  return st;
}

static void uth_str_insert_run(void *state, size_t n) {
  uth_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    uth_str_entry_t *e = &st->str_pool[i];
    /* memcpy of the whole fixed-width key, not snprintf. uthash keys on a
       member of the caller's own struct, so the key has to be copied in; what
       the arm must not do is copy it by a route the arm it is compared against
       does not use. The map copies a key's bytes with a plain memcpy, and a
       formatted copy of the same bytes costs on the order of the whole
       operation being measured, which lands entirely on this side of the
       comparison. */
    memcpy(e->key, st->ks->str_keys[i], strlen(st->ks->str_keys[i]) + 1);
    e->val = (int)i;
    /* Hashes the key once, matching chmap_insert's own find-or-insert. A
       find followed by an add hashes it twice, which the other side of this
       comparison does not do. */
    uth_str_entry_t *replaced = NULL;
    HASH_REPLACE_STR(st->str_head, key, e, replaced);
    (void)replaced;
  }
  bench_sink(st->str_head);
}

static void *uth_str_filled_setup(size_t n) {
  uth_state_t *st = uth_str_empty_setup(n);
  if (!st) return NULL;
  uth_str_insert_run(st, n);
  for (size_t i = 0; i < n; i++) {
    uth_str_entry_t *found = NULL;
    HASH_FIND_STR(st->str_head, st->ks->str_keys[i], found);
    if (found) st->expected_acc += found->val;
  }
  return st;
}

static void uth_str_lookup_run(void *state, size_t n) {
  uth_state_t *st = state;
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    uth_str_entry_t *found = NULL;
    HASH_FIND_STR(st->str_head, st->ks->str_keys[i], found);
    if (found) acc += found->val;
  }
  if (acc != st->expected_acc) bench_die("uthash string lookup missed");
  bench_sink(&acc);
}

static void uth_insert_run(void *state, size_t n) {
  uth_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    uth_entry_t *e = &st->pool[i];
    e->key = st->ks->keys[i];
    e->val = (int)i;
    /* One hash; see uth_str_insert_run for why a find-then-add is not a
       like-for-like comparison against chmap_insert. */
    uth_entry_t *replaced = NULL;
    HASH_REPLACE_INT(st->head, key, e, replaced);
    (void)replaced;
  }
  bench_sink(st->head);
}

static void *uth_filled_setup(size_t n) {
  uth_state_t *st = uth_empty_setup(n);
  if (!st) return NULL;
  uth_insert_run(st, n);
  for (size_t i = 0; i < n; i++) {
    uth_entry_t *found = NULL;
    HASH_FIND_INT(st->head, &st->ks->keys[i], found);
    if (found) st->expected_acc += found->val;
  }
  return st;
}

static void uth_lookup_run(void *state, size_t n) {
  uth_state_t *st = state;
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    uth_entry_t *found = NULL;
    HASH_FIND_INT(st->head, &st->ks->keys[i], found);
    if (found) acc += found->val;
  }
  if (acc != st->expected_acc) bench_die("uthash int lookup missed");
  bench_sink(&acc);
}

#endif /* BENCH_HAVE_UTHASH */

/* ------------------------------------------------------------------------ */

/* chashmap and cbstmap hold no internal locks by design, so a shared instance
 * driven from several threads would be a data race rather than a benchmark.
 * Each thread builds and drives its own map, which is how they are meant to be
 * used concurrently and measures whether that use scales. */
BENCH_MT_SETUP(hmap_int_empty_setup)
BENCH_MT_SETUP(hmap_int_filled_setup)
BENCH_MT_SETUP(hmap_str_empty_setup)
BENCH_MT_SETUP(bmap_empty_setup)
BENCH_MT_SETUP(bmap_filled_setup)

void bench_register_maps(void) {
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "insert_int_int",
                            .setup = hmap_int_empty_setup,
                            .run = hmap_int_insert_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add_mt(&(bench_case_t){.group = "chashmap",
                               .name = "insert_int_int",
                               .setup_mt = hmap_int_empty_setup_mt,
                               .run = hmap_int_insert_run,
                               .teardown = hmap_teardown,
                               .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_int_int_hit",
                            .setup = hmap_int_filled_setup,
                            .run = hmap_int_lookup_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add_mt(&(bench_case_t){.group = "chashmap",
                               .name = "lookup_int_int_hit",
                               .setup_mt = hmap_int_filled_setup_mt,
                               .run = hmap_int_lookup_run,
                               .teardown = hmap_teardown,
                               .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_int_int_miss",
                            .setup = hmap_int_filled_setup,
                            .run = hmap_int_lookup_miss_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "remove_int_int",
                            .setup = hmap_int_filled_setup,
                            .run = hmap_int_remove_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "insert_str_int",
                            .setup = hmap_str_empty_setup,
                            .run = hmap_str_insert_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add_mt(&(bench_case_t){.group = "chashmap",
                               .name = "insert_str_int",
                               .setup_mt = hmap_str_empty_setup_mt,
                               .run = hmap_str_insert_run,
                               .teardown = hmap_teardown,
                               .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_str_int_hit",
                            .setup = hmap_str_filled_setup,
                            .run = hmap_str_lookup_run,
                            .teardown = hmap_teardown,
                            .n = BENCH_MAP_N});
#ifdef BENCH_HAVE_UTHASH
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "insert_int_int",
                            .vs = "uthash",
                            .setup = uth_empty_setup,
                            .run = uth_insert_run,
                            .teardown = uth_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_int_int_hit",
                            .vs = "uthash",
                            .setup = uth_filled_setup,
                            .run = uth_lookup_run,
                            .teardown = uth_teardown,
                            .n = BENCH_MAP_N});
  /* The string-keyed cases reach a different backend of the map under test
     (separate chaining rather than open addressing), so they need a reference
     of their own; without one they report a nanosecond count with nothing to
     judge it against. */
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "insert_str_int",
                            .vs = "uthash",
                            .setup = uth_str_empty_setup,
                            .run = uth_str_insert_run,
                            .teardown = uth_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_str_int_hit",
                            .vs = "uthash",
                            .setup = uth_str_filled_setup,
                            .run = uth_str_lookup_run,
                            .teardown = uth_teardown,
                            .n = BENCH_MAP_N});
#endif
#ifdef BENCH_HAVE_GLIB
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "insert_int_int",
                            .vs = "GHashTable",
                            .setup = glib_ht_empty_setup,
                            .run = glib_ht_insert_run,
                            .teardown = glib_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "chashmap",
                            .name = "lookup_int_int_hit",
                            .vs = "GHashTable",
                            .setup = glib_ht_filled_setup,
                            .run = glib_ht_lookup_run,
                            .teardown = glib_teardown,
                            .n = BENCH_MAP_N});
#endif

  bench_add(&(bench_case_t){.group = "cbstmap",
                            .name = "insert_int_int",
                            .setup = bmap_empty_setup,
                            .run = bmap_insert_run,
                            .teardown = bmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add_mt(&(bench_case_t){.group = "cbstmap",
                               .name = "insert_int_int",
                               .setup_mt = bmap_empty_setup_mt,
                               .run = bmap_insert_run,
                               .teardown = bmap_teardown,
                               .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "cbstmap",
                            .name = "lookup_int_int_hit",
                            .setup = bmap_filled_setup,
                            .run = bmap_lookup_run,
                            .teardown = bmap_teardown,
                            .n = BENCH_MAP_N});
  bench_add_mt(&(bench_case_t){.group = "cbstmap",
                               .name = "lookup_int_int_hit",
                               .setup_mt = bmap_filled_setup_mt,
                               .run = bmap_lookup_run,
                               .teardown = bmap_teardown,
                               .n = BENCH_MAP_N});
#ifdef BENCH_HAVE_GLIB
  bench_add(&(bench_case_t){.group = "cbstmap",
                            .name = "insert_int_int",
                            .vs = "GTree",
                            .setup = glib_tree_empty_setup,
                            .run = glib_tree_insert_run,
                            .teardown = glib_teardown,
                            .n = BENCH_MAP_N});
  bench_add(&(bench_case_t){.group = "cbstmap",
                            .name = "lookup_int_int_hit",
                            .vs = "GTree",
                            .setup = glib_tree_filled_setup,
                            .run = glib_tree_lookup_run,
                            .teardown = glib_teardown,
                            .n = BENCH_MAP_N});
#endif
}
