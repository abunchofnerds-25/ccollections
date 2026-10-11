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
 * chashmap chooses its implementation from the key type and the value type: an
 * integral key and an integral value of eight bytes or fewer get open
 * addressing, and everything else, including every string key, gets separate
 * chaining. These benchmarks measure both paths, because one change can easily
 * make one path faster and the other path slower.
 *
 * Where GLib is on the machine, these benchmarks also measure GHashTable and
 * GTree on the same workload. Those figures give you a scale, not a target:
 * GHashTable stores pointers and leaves ownership of a key to the caller,
 * while chashmap copies a key and a value into its own storage, so the two do
 * different amounts of work for the same call.
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

/* The harness draws the keys once, in the same way on every run, and every map
 * case shares them, so the comparison between the implementations uses
 * identical input. */
typedef struct {
  int *keys;
  size_t count;
  char (*str_keys)[16];
} keyset_t;

static keyset_t *keyset_create(size_t n, bool with_strings) {
  keyset_t *ks = calloc(1, sizeof(*ks));
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
  hmap_state_t *st = calloc(1, sizeof(*st));
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
  /* The value is in a local of the value type, so the timed loop does no
   * conversion. */
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

/* Every lookup arm compares the sum that it accumulates against a sum that the
   setup, which the harness does not time, computes with the same traversal. A
   miss costs much less than a hit, so a defect that stopped the map from
   finding a key would report a large speed increase under a name that promises
   the opposite, and nobody questions a result of that shape. The keys are
   random and not in order, so the set holds duplicates; this is why the
   harness measures the expected sum instead of computing it from a formula,
   because for a duplicated key the map keeps only the value from the last
   insert. */
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
  bench_sink_value((long long)acc);
}

/* Every lookup here misses, which is the path that probes for the longest time
 * before it can conclude that the key is absent. */
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
  bench_sink_value((long long)acc);
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
  hmap_state_t *st = calloc(1, sizeof(*st));
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
  bench_sink_value((long long)acc);
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
  bmap_state_t *st = calloc(1, sizeof(*st));
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
  bench_sink_value((long long)acc);
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
  glib_state_t *st = calloc(1, sizeof(*st));
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
  bench_sink_value((long long)acc);
}

static gint glib_int_cmp(gconstpointer a, gconstpointer b, gpointer u) {
  (void)u;
  int x = GPOINTER_TO_INT(a), y = GPOINTER_TO_INT(b);
  return (x > y) - (x < y);
}

static void *glib_tree_empty_setup(size_t n) {
  glib_state_t *st = calloc(1, sizeof(*st));
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
  bench_sink_value((long long)acc);
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

/* The arm with a string key needs its own entry type, because uthash keys a
   table on one named member. The key bytes sit inside the entry, which
   matches how the map on the other side of this comparison stores a short
   key, so both sides pay for the same key COPY.

   The two sides do not pay for the same node allocation, and this is the one
   row where that matters. A string key puts chashmap on its separate chaining
   backend, which allocates one chain node for each insert inside the timed
   loop, while these entries come from a block that the setup obtains and the
   harness does not time the setup. The insert_str_int row therefore charges
   this library for one allocation in each operation that uthash does not pay,
   so that row understates this library instead of flattering it. */
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

/* The setup allocates the entry storage as one block, so the measured loop
 * holds the work of uthash itself and not one malloc for each insert. On the
 * rows with an integer key, neither side pays an allocation for each entry:
 * both grow their table a few times inside the timed loop and copy the entries
 * into it, a cost that follows the number of GROWTHS and not the number of
 * inserts. On the rows with a string key the two sides do differ; see the
 * comment on uth_str_entry_t for which way that difference goes. */
static void *uth_empty_setup(size_t n) {
  uth_state_t *st = calloc(1, sizeof(*st));
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
  uth_state_t *st = calloc(1, sizeof(*st));
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
    /* This hashes the key once, which matches the find-or-insert of
       chmap_insert. A find and then an add hashes the key twice, which the
       other side of this comparison does not do. */
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
  bench_sink_value((long long)acc);
}

static void uth_insert_run(void *state, size_t n) {
  uth_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    uth_entry_t *e = &st->pool[i];
    e->key = st->ks->keys[i];
    e->val = (int)i;
    /* This hashes the key once. See uth_str_insert_run for why a find and then
       an add is not an equal comparison against chmap_insert. */
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
  bench_sink_value((long long)acc);
}

#endif /* BENCH_HAVE_UTHASH */

/* ------------------------------------------------------------------------ */

/* chashmap and cbstmap hold no lock of their own, by design, so one shared map
 * that several threads drive would be a data race and not a benchmark. Each
 * thread builds and drives its own map instead, which is how you use these
 * containers from several threads, and this case measures whether that use
 * scales. */
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
  /* The cases with a string key reach a different backend of the map under
     test, separate chaining instead of open addressing, so they need a
     reference of their own; without one, they report a count in nanoseconds
     with nothing to compare it against. */
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
