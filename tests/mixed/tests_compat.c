/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// The behavioural compatibility corpus. doc/compatibility.md promises that,
// within the 2.x series, a given input gives the same retval, the same
// operations invalidate iterators and borrowed pointers, ownership rules stay,
// callbacks keep their order and their thread, a caller's allocator is used
// where it is today, and each type keeps its thread-safety class. The ABI gate
// can't see any of that, so this binary pins it.
//
// Please keep three rules when you add to it. Pin only what the docs promise
// (no exact allocation counts or bucket orders, since a faster algorithm may
// replace the current one). Compare against literals and against allocators
// this file owns, never against the library's own constants. And for
// invalidation pin only the positive guarantees, because reading a pointer
// the library invalidated is undefined behaviour.
//
// Every category here fails against a library that breaks it: Calling
// on_complete before the task, dropping the destroy of a child on a failed
// cjson insert, allocating a vector buffer with malloc(), leaking a finished
// iterator, or handing out a shared copy from chashmap_get_elem_ref(). Run
// those mutations again when you add a category.

#include <cbstmap.h>
#include <chashmap.h>
#include <cjson.h>
#include <clrucache.h>
#include <cthreadpool.h>
#include <cvector.h>
#include <cyaml.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <time.h>

TAU_MAIN()

// A counting allocator
//
// It counts what the library allocates and frees through it, and it knows its
// own blocks: A free of a pointer it never handed out stops the process, so a
// container that mixes allocators can't pass as correct. It's single-threaded
// (the registry has no lock), so give it only to objects driven from one
// thread.
//
// CA_MAX_LIVE is the size of the registry of live blocks, more than any test
// here keeps live at once.
#define CA_MAX_LIVE 8192

// State of the counting allocator. Once alloc_calls reaches fail_from, every
// request fails; SIZE_MAX means it never fails.
typedef struct {
  void *live[CA_MAX_LIVE];
  size_t live_count;
  size_t alloc_calls;  // malloc, calloc and realloc calls that gave memory
  size_t free_calls;
  size_t fail_from;
} counting_allocator_t;

// The one counting allocator of this file.
static counting_allocator_t g_ca;

// Clears the counts and the registry of g_ca and turns refusals off. Every test
// that uses the counting allocator starts with it.
static void ca_reset(void) {
  memset(&g_ca, 0, sizeof(g_ca));
  g_ca.fail_from = SIZE_MAX;
}

// Adds p to the live registry of g_ca, so a later free can be checked against
// it. It aborts when the registry is full.
static void ca_register(void *p) {
  if (!p) return;
  if (g_ca.live_count >= CA_MAX_LIVE) {
    fprintf(stderr, "counting allocator: live-block registry overflow\n");
    abort();
  }
  g_ca.live[g_ca.live_count++] = p;
}

// Removes p from the live registry of g_ca. It aborts for a block it never
// handed out, which is how a container that mixes allocators gets caught.
static void ca_forget(void *p) {
  for (size_t i = 0; i < g_ca.live_count; i++) {
    if (g_ca.live[i] == p) {
      g_ca.live[i] = g_ca.live[--g_ca.live_count];
      return;
    }
  }
  fprintf(stderr, "counting allocator: free of a foreign pointer %p\n", p);
  abort();
}

// Says whether g_ca refuses requests (alloc_calls reached fail_from), so a test
// can make the library run out of memory at a chosen point.
static bool ca_refusing(void) { return g_ca.alloc_calls >= g_ca.fail_from; }

// malloc, calloc, realloc and free of the counting allocator, handed to the
// library through g_ca_procs. Each counts in g_ca and registers or forgets the
// block, and the three allocating ones fail while ca_refusing() says so.
// ca_realloc retires the old block before the call (its value is indeterminate
// afterwards) and registers it again when realloc refuses.
static void *ca_malloc(size_t n) {
  if (ca_refusing()) return NULL;
  void *p = malloc(n);
  if (!p) return NULL;
  g_ca.alloc_calls++;
  ca_register(p);
  return p;
}

static void *ca_calloc(size_t a, size_t b) {
  if (ca_refusing()) return NULL;
  void *p = calloc(a, b);
  if (!p) return NULL;
  g_ca.alloc_calls++;
  ca_register(p);
  return p;
}

static void *ca_realloc(void *old, size_t n) {
  if (ca_refusing()) return NULL;
  if (old) ca_forget(old);
  void *p = realloc(old, n);
  if (!p) {
    if (old) ca_register(old);
    return NULL;
  }
  g_ca.alloc_calls++;
  ca_register(p);
  return p;
}

static void ca_free(void *p) {
  if (!p) return;
  g_ca.free_calls++;
  ca_forget(p);
  free(p);
}

// The procs that hand the counting allocator to the library.
static ccol_memmgmt_procs g_ca_procs = {.malloc = ca_malloc,
                                        .calloc = ca_calloc,
                                        .realloc = ca_realloc,
                                        .free = ca_free};

// Gives &g_ca_procs, the procs a test passes to a _mp constructor.
static ccol_memmgmt_procs *ca_procs(void) { return &g_ca_procs; }

// Promise: No enumerator changes its numeric value within 1.x
//
// The expectations are literals on purpose: Comparing an enumerator with itself
// holds under any renumbering.
TEST(compat_enum_values, ccol_retval_t_enumerators_keep_their_numeric_values) {
  REQUIRE_EQ((int)ccol_success, 0);
  REQUIRE_EQ((int)ccol_not_enough_memory, -1);
  REQUIRE_EQ((int)ccol_key_already_present, -2);
  REQUIRE_EQ((int)ccol_key_not_found, -3);
  REQUIRE_EQ((int)ccol_invalid_args, -4);
  REQUIRE_EQ((int)ccol_not_permitted, -5);
  REQUIRE_EQ((int)ccol_timed_out, -6);
  REQUIRE_EQ((int)ccol_container_full, -7);
  REQUIRE_EQ((int)ccol_container_empty, -8);
  REQUIRE_EQ((int)ccol_unexpected_failure, -18);
}

TEST(compat_enum_values, ccol_data_type_enumerators_keep_their_numeric_values) {
  REQUIRE_EQ((int)ccol_char, 0);
  REQUIRE_EQ((int)ccol_int, 2);
  REQUIRE_EQ((int)ccol_unsigned_long_long, 9);
  REQUIRE_EQ((int)ccol_double, 11);
  REQUIRE_EQ((int)ccol_pointer, 13);
  REQUIRE_EQ((int)ccol_string, 14);
  REQUIRE_EQ((int)ccol_other_types, 15);
  REQUIRE_EQ((int)ccol_signed_char, 16);
}

// Promise: When the library calls an allocator that the caller gives
//
// These tests assert only that the count isn't zero and that it balances, never
// the count itself, because a new algorithm may change the counts.
TEST(compat_allocator, a_hash_map_allocates_and_frees_through_the_given_procs) {
  ca_reset();
  {
    chmap_construct_mp(m, int, int, ca_procs());
    for (int i = 0; i < 64; i++) {
      int k = i, v = i * 3;
      chmap_insert(m, k, v);
    }
    chmap_destroy(m);
  }
  bool used_it = g_ca.alloc_calls > 0;
  bool gave_it_all_back = (g_ca.live_count == 0);
  REQUIRE_TRUE(used_it);
  REQUIRE_TRUE(gave_it_all_back);
}

TEST(compat_allocator,
     an_ordered_map_allocates_and_frees_through_the_given_procs) {
  ca_reset();
  {
    cbmap_construct_mp(m, int, int, ca_procs());
    for (int i = 0; i < 64; i++) {
      int k = i, v = i * 3;
      cbmap_insert(m, k, v);
    }
    cbmap_destroy(m);
  }
  bool used_it = g_ca.alloc_calls > 0;
  REQUIRE_TRUE(used_it);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_allocator, a_vector_allocates_and_frees_through_the_given_procs) {
  ca_reset();
  {
    cvec_construct_mp(v, int, ca_procs());
    for (int i = 0; i < 256; i++) cvec_push(v, i);
    cvec_destroy(v);
  }
  bool used_it = g_ca.alloc_calls > 0;
  REQUIRE_TRUE(used_it);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_allocator, passing_null_procs_selects_the_standard_family) {
  ca_reset();
  {
    chmap_construct_mp(m, int, int, NULL);
    for (int i = 0; i < 64; i++) {
      int k = i, v = i;
      chmap_insert(m, k, v);
    }
    chmap_destroy(m);
  }
  REQUIRE_EQ(g_ca.alloc_calls, (size_t)0);
  REQUIRE_EQ(g_ca.free_calls, (size_t)0);
}

TEST(compat_allocator, an_iterator_is_allocated_from_the_container_own_procs) {
  ca_reset();
  {
    chmap_construct_mp(m, int, int, ca_procs());
    for (int i = 0; i < 8; i++) {
      int k = i, v = i;
      chmap_insert(m, k, v);
    }
    size_t before_iteration = g_ca.alloc_calls;
    int seen = 0;
    {
      ccol_iter_declare(m, it);
      for (it = ccol_begin(m); it; it = ccol_iter_next(it)) seen++;
    }
    bool iterator_came_from_our_procs = g_ca.alloc_calls > before_iteration;
    chmap_destroy(m);
    REQUIRE_EQ(seen, 8);
    REQUIRE_TRUE(iterator_came_from_our_procs);
  }
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// On the raw function layer running out of memory is a returned value, never a
// call to ccol__fatal_err().
TEST(compat_allocator, a_refused_allocation_is_reported_rather_than_fatal) {
  ca_reset();
  chmap_construct_mp(m, int, int, ca_procs());

  g_ca.fail_from = g_ca.alloc_calls;
  ccol_retval worst = ccol_success;
  for (int i = 0; i < 4096 && worst == ccol_success; i++) {
    int k = i, v = i;
    cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    worst = chashmap_insert_elem(m, &kp, &vp);
  }
  g_ca.fail_from = SIZE_MAX;

  chmap_destroy(m);
  REQUIRE_EQ((int)worst, -1);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// Promise: The ownership rules for keys, values and nodes
//
// The test writes over the caller's buffers after the insert, so a map that
// kept the caller's pointer would give back the new text. It reads with
// chmap_get_ptr() because chmap_get() stops the whole binary on a miss.
TEST(compat_ownership,
     a_map_copies_a_string_key_and_value_into_its_own_storage) {
  char key[32];
  char val[32];
  snprintf(key, sizeof(key), "alice");
  snprintf(val, sizeof(val), "engineer");

  chmap_construct(m, char *, char *);
  chmap_insert(m, key, val);

  memset(key, 'x', sizeof(key) - 1);
  memset(val, 'y', sizeof(val) - 1);
  key[sizeof(key) - 1] = '\0';
  val[sizeof(val) - 1] = '\0';

  char *const *found = chmap_get_ptr(m, "alice");
  bool copied = found && *found && strcmp(*found, "engineer") == 0;
  chmap_destroy(m);
  REQUIRE_TRUE(copied);
}

// cjson and cyaml share one ownership rule for an attach: ccol_invalid_args
// leaves the child to the caller, and every other failure transfers it and
// deep-frees it. Each header states the rule on its own, so each module gets
// its own tests. In these tests destroying the parents alone must give back
// every block.
TEST(compat_ownership, attaching_a_json_child_transfers_it_to_the_parent) {
  ca_reset();
  cjson root = cjson_create_dictionary_mp(ca_procs());
  cjson child = cjson_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  ccol_retval r =
      built ? cjson_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  cjson_destroy(root);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     a_json_get_hands_back_a_reference_the_parent_still_owns) {
  ca_reset();
  cjson root = cjson_create_dictionary_mp(ca_procs());
  cjson child = cjson_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL) &&
               cjson_dictionary_set(root, "k", child) == ccol_success;

  cjson borrowed = built ? cjson_dictionary_get(root, "k") : NULL;
  bool borrowed_is_the_child = (borrowed == child);

  cjson_destroy(root);
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(borrowed_is_the_child);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// The child stays owned by its first parent, so destroying both parents frees
// it exactly once.
TEST(compat_ownership,
     an_already_attached_json_child_is_refused_and_left_alone) {
  ca_reset();
  cjson first = cjson_create_list_mp(ca_procs());
  cjson second = cjson_create_list_mp(ca_procs());
  cjson child = cjson_create_string_mp("v", ca_procs());
  bool built =
      first && second && child && cjson_list_push(first, child) == ccol_success;

  ccol_retval r = built ? cjson_list_push(second, child) : ccol_success;
  cjson_destroy(second);
  cjson_destroy(first);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -4);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// A failed allocation transfers the child too, so freeing it afterwards would
// be a double free.
TEST(compat_ownership,
     a_json_attach_that_runs_out_of_memory_destroys_the_child) {
  ca_reset();
  cjson root = cjson_create_dictionary_mp(ca_procs());
  cjson child = cjson_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  g_ca.fail_from = g_ca.alloc_calls;
  ccol_retval r =
      built ? cjson_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  g_ca.fail_from = SIZE_MAX;

  cjson_destroy(root);
  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -1);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership, attaching_a_yaml_child_transfers_it_to_the_parent) {
  ca_reset();
  cyaml root = cyaml_create_dictionary_mp(ca_procs());
  cyaml child = cyaml_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  ccol_retval r =
      built ? cyaml_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  cyaml_destroy(root);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     an_already_attached_yaml_child_is_refused_and_left_alone) {
  ca_reset();
  cyaml first = cyaml_create_list_mp(ca_procs());
  cyaml second = cyaml_create_list_mp(ca_procs());
  cyaml child = cyaml_create_string_mp("v", ca_procs());
  bool built =
      first && second && child && cyaml_list_push(first, child) == ccol_success;

  ccol_retval r = built ? cyaml_list_push(second, child) : ccol_success;
  cyaml_destroy(second);
  cyaml_destroy(first);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -4);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// A new list has room already, so the test pushes enough children to make the
// backing store grow while every allocation is refused.
TEST(compat_ownership,
     a_yaml_attach_that_runs_out_of_memory_destroys_the_child) {
  enum { KIDS = 64 };
  ca_reset();
  cyaml root = cyaml_create_list_mp(ca_procs());
  cyaml kids[KIDS];
  bool built = (root != NULL);
  for (int i = 0; i < KIDS && built; i++) {
    kids[i] = cyaml_create_string_mp("value", ca_procs());
    if (!kids[i]) built = false;
  }

  g_ca.fail_from = g_ca.alloc_calls;
  bool some_push_failed = false;
  if (built) {
    for (int i = 0; i < KIDS; i++) {
      if (cyaml_list_push(root, kids[i]) != ccol_success)
        some_push_failed = true;
    }
  }
  g_ca.fail_from = SIZE_MAX;

  cyaml_destroy(root);
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(some_push_failed);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// Promise: Which operations invalidate an iterator or a borrowed pointer
//
// Only the positive half can be tested: Reading a pointer that the library
// invalidated is undefined behaviour, so a test of that direction would check
// the sanitizer and not the contract.
TEST(compat_pointer_stability, a_reference_survives_lookups_of_other_keys) {
  chmap_construct(m, int, int);
  for (int i = 0; i < 32; i++) {
    int k = i, v = i * 7;
    chmap_insert(m, k, v);
  }

  int key = 5;
  cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
  cmap_pair held = {NULL, 0};
  ccol_retval r = chashmap_get_elem_ref(m, &kp, &held);

  for (int i = 0; i < 32; i++) {
    if (i == 5) continue;
    int other = i;
    cmap_pair okp = {.ptr = &other, .size = sizeof(other)};
    cmap_pair tmp;
    (void)chashmap_get_elem_ref(m, &okp, &tmp);
  }

  bool still_correct =
      (r == ccol_success) && held.ptr && (*(const int *)held.ptr == 35);
  chmap_destroy(m);
  REQUIRE_TRUE(still_correct);
}

TEST(compat_pointer_stability, several_references_may_be_held_at_once) {
  chmap_construct(m, int, int);
  for (int i = 0; i < 16; i++) {
    int k = i, v = i * 11;
    chmap_insert(m, k, v);
  }

  const int *refs[16];
  bool all_resolved = true;
  for (int i = 0; i < 16; i++) {
    int k = i;
    cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
    cmap_pair out;
    if (chashmap_get_elem_ref(m, &kp, &out) != ccol_success || !out.ptr) {
      all_resolved = false;
      refs[i] = NULL;
      continue;
    }
    refs[i] = (const int *)out.ptr;
  }

  bool all_still_correct = all_resolved;
  for (int i = 0; i < 16 && all_still_correct; i++) {
    if (!refs[i] || *refs[i] != i * 11) all_still_correct = false;
  }

  chmap_destroy(m);
  REQUIRE_TRUE(all_still_correct);
}

// Writing through a reference is allowed and invalidates nothing, so a
// reference to another key keeps reading its own value.
TEST(compat_pointer_stability, updating_a_key_in_place_keeps_other_references) {
  chmap_construct(m, int, int);
  for (int i = 0; i < 16; i++) {
    int k = i, v = i;
    chmap_insert(m, k, v);
  }

  int a = 3, b = 9;
  cmap_pair akp = {.ptr = &a, .size = sizeof(a)};
  cmap_pair bkp = {.ptr = &b, .size = sizeof(b)};
  cmap_pair aref = {NULL, 0}, bref = {NULL, 0};
  bool got = chashmap_get_elem_ref(m, &akp, &aref) == ccol_success &&
             chashmap_get_elem_ref(m, &bkp, &bref) == ccol_success;

  if (got) *(int *)aref.ptr = 1234;

  bool a_updated = got && *(const int *)aref.ptr == 1234;
  bool b_untouched = got && *(const int *)bref.ptr == 9;
  chmap_destroy(m);
  REQUIRE_TRUE(a_updated);
  REQUIRE_TRUE(b_untouched);
}

// ccol_begin() calls ccol__fatal_err() on an error, so a regression here stops
// the binary instead of failing one test.
TEST(compat_iterators, a_null_handle_iterates_as_empty_for_every_container) {
  int rounds = 0;
  {
    chmap_declare(hm, int, int) = NULL;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it; it = ccol_iter_next(it)) rounds++;
  }
  {
    cbmap_declare(bm, int, int) = NULL;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it; it = ccol_iter_next(it)) rounds++;
  }
  {
    cvec_declare(v, int) = NULL;
    ccol_iter_declare(v, it);
    for (it = ccol_begin(v); it; it = ccol_iter_next(it)) rounds++;
  }
  REQUIRE_EQ(rounds, 0);
}

TEST(compat_iterators, running_an_iterator_to_the_end_returns_its_memory) {
  ca_reset();
  {
    chmap_construct_mp(m, int, int, ca_procs());
    for (int i = 0; i < 16; i++) {
      int k = i, v = i;
      chmap_insert(m, k, v);
    }
    int seen = 0;
    {
      ccol_iter_declare(m, it);
      for (it = ccol_begin(m); it; it = ccol_iter_next(it)) seen++;
    }
    chmap_destroy(m);
    REQUIRE_EQ(seen, 16);
  }
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// After a break the iterator is still live, and the scope-exit cleanup of its
// declaration frees it.
TEST(compat_iterators, abandoning_an_iterator_early_returns_its_memory) {
  ca_reset();
  {
    chmap_construct_mp(m, int, int, ca_procs());
    for (int i = 0; i < 16; i++) {
      int k = i, v = i;
      chmap_insert(m, k, v);
    }
    int seen = 0;
    {
      ccol_iter_declare(m, it);
      for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
        if (++seen == 4) break;
      }
    }
    chmap_destroy(m);
    REQUIRE_EQ(seen, 4);
  }
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

// Promise: The order callbacks run in, and the thread each runs on
//
// clrucache eviction: ev_cb records the evicted keys (at most EV_MAX), and
// whether it ever ran outside a set call or off the main thread. EV_MAX is how
// many evicted keys it keeps, more than any test here evicts.
#define EV_MAX 32

// The keys ev_cb saw, in order, and how many.
static int g_ev_keys[EV_MAX];
static size_t g_ev_count;

// lru_set_int() keeps g_ev_inside_a_set true for the length of the set.
static bool g_ev_inside_a_set;
static bool g_ev_ran_outside_a_set;

// The thread that ran ev_reset(), and whether ev_cb ever ran on another one.
static pthread_t g_ev_main_thread;
static bool g_ev_ran_off_thread;

// Clears the eviction records and takes the calling thread as the main one.
// Every eviction test starts with it.
static void ev_reset(void) {
  memset(g_ev_keys, 0, sizeof(g_ev_keys));
  g_ev_count = 0;
  g_ev_inside_a_set = false;
  g_ev_ran_outside_a_set = false;
  g_ev_ran_off_thread = false;
  g_ev_main_thread = pthread_self();
}

// The eviction callback of the cache tests. It appends key to g_ev_keys and
// notes in g_ev_ran_outside_a_set and g_ev_ran_off_thread where it ran.
static void ev_cb(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  if (!g_ev_inside_a_set) g_ev_ran_outside_a_set = true;
  if (!pthread_equal(pthread_self(), g_ev_main_thread))
    g_ev_ran_off_thread = true;
  if (g_ev_count < EV_MAX && key && key->ptr)
    g_ev_keys[g_ev_count++] = *(const int *)key->ptr;
}

// Sets the int key k to v in cache c through clrucache_set_full(), with
// g_ev_inside_a_set raised around the call so ev_cb can tell it ran inside.
// Gives the retval of the set.
static ccol_retval lru_set_int(ccache c, int k, int v) {
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  g_ev_inside_a_set = true;
  ccol_retval r = clrucache_set_full(c, &kp, &vp);
  g_ev_inside_a_set = false;
  return r;
}

// Looks up the int key k in cache c and copies the value to *out; gives the
// retval of the lookup. clrucache_get_full() gives a heap copy, freed here with
// free() since these caches use the default allocator.
static ccol_retval lru_get_int(ccache c, int k, int *out) {
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {0};
  ccol_retval r = clrucache_get_full(c, &kp, &vp);
  if (r == ccol_success && vp.ptr) {
    if (vp.size == sizeof(*out)) memcpy(out, vp.ptr, sizeof(*out));
    free(vp.ptr);
  }
  return r;
}

// Below a capacity of 128 a cache is one segment with one exact LRU order. Four
// keys go into a cache of three, and a lookup of key 1 in between makes key 2
// the only right answer.
TEST(compat_callbacks, a_small_cache_evicts_in_exact_global_lru_order) {
  ev_reset();
  char *err = NULL;
  ccache c = clrucache_create_full(3, ccol_int, ccol_int, NULL, NULL, ev_cb,
                                   NULL, &err);
  REQUIRE_TRUE(c != CCACHE_NULL);

  bool ok = lru_set_int(c, 1, 10) == ccol_success &&
            lru_set_int(c, 2, 20) == ccol_success &&
            lru_set_int(c, 3, 30) == ccol_success;

  int got = 0;
  ok = ok && lru_get_int(c, 1, &got) == ccol_success && got == 10;

  ok = ok && lru_set_int(c, 4, 40) == ccol_success;

  size_t evictions = g_ev_count;
  int first_evicted = evictions > 0 ? g_ev_keys[0] : -1;
  ccache_destroy(c);

  REQUIRE_TRUE(ok);
  REQUIRE_EQ(evictions, (size_t)1);
  REQUIRE_EQ(first_evicted, 2);
}

// Synchronously means inside the set that caused the eviction and on the
// caller's thread, so a deferred or background reclaimer would fail this.
TEST(compat_callbacks, the_eviction_callback_runs_synchronously_on_the_caller) {
  ev_reset();
  char *err = NULL;
  ccache c = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL, ev_cb,
                                   NULL, &err);
  REQUIRE_TRUE(c != CCACHE_NULL);

  bool ok = true;
  for (int i = 1; i <= 6 && ok; i++) ok = lru_set_int(c, i, i) == ccol_success;

  size_t evictions = g_ev_count;
  bool ran_outside = g_ev_ran_outside_a_set;
  bool ran_off_thread = g_ev_ran_off_thread;
  ccache_destroy(c);

  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(evictions > 0);
  REQUIRE_FALSE(ran_outside);
  REQUIRE_FALSE(ran_off_thread);
}

// clrucache remote setter: What it answers, how often it ran, and the cache.
static bool g_setter_should_succeed;
static size_t g_setter_calls;
static ccache g_setter_cache;

// The remote setter of the setter tests. It counts the call in g_setter_calls
// and gives g_setter_should_succeed. It can't read the cache (that would
// deadlock on the segment lock), so the tests check only the call count and
// what a refusal left behind.
static bool remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  g_setter_calls++;
  (void)key;
  return g_setter_should_succeed;
}

// The setter runs before the cache update; a refusal gives
// ccol_unexpected_failure and leaves the key out of the cache.
TEST(compat_callbacks, a_refused_remote_setter_leaves_the_cache_unchanged) {
  ev_reset();
  g_setter_calls = 0;
  g_setter_should_succeed = false;
  char *err = NULL;
  g_setter_cache = clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                         remote_setter, NULL, NULL, &err);
  REQUIRE_TRUE(g_setter_cache != CCACHE_NULL);

  ccol_retval r = lru_set_int(g_setter_cache, 7, 70);

  int got = 0;
  ccol_retval lookup = lru_get_int(g_setter_cache, 7, &got);
  size_t calls = g_setter_calls;
  ccache_destroy(g_setter_cache);

  REQUIRE_EQ((int)r, -18);
  REQUIRE_EQ(calls, (size_t)1);
  REQUIRE_NE((int)lookup, 0);
}

TEST(compat_callbacks, an_accepted_remote_setter_runs_before_the_cache_update) {
  ev_reset();
  g_setter_calls = 0;
  g_setter_should_succeed = true;
  char *err = NULL;
  g_setter_cache = clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                         remote_setter, NULL, NULL, &err);
  REQUIRE_TRUE(g_setter_cache != CCACHE_NULL);

  ccol_retval r = lru_set_int(g_setter_cache, 7, 70);
  int got = 0;
  ccol_retval lookup = lru_get_int(g_setter_cache, 7, &got);
  size_t calls = g_setter_calls;
  ccache_destroy(g_setter_cache);

  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(calls, (size_t)1);
  REQUIRE_EQ((int)lookup, 0);
  REQUIRE_EQ(got, 70);
}

// cthreadpool task and completion. The workers write these, so they're atomic.
// g_seq hands out the order in which pool_task and pool_complete ran.
static atomic_int g_seq;
static atomic_int g_task_order;
static atomic_int g_complete_order;
static atomic_int g_complete_on_submitter;
static atomic_int g_complete_saw_task_done;
static atomic_int g_task_done;
static atomic_int g_complete_ran;

// The thread that submits; pool callbacks compare themselves with it.
static pthread_t g_submitter_thread;

// The task of the on_complete test. It records its place in g_seq in
// g_task_order and sets g_task_done.
static void pool_task(void *arg) {
  (void)arg;
  atomic_store(&g_task_order, atomic_fetch_add(&g_seq, 1));
  atomic_store(&g_task_done, 1);
}

// The on_complete of the on_complete test. It records ran, whether the task had
// finished, its place in g_seq, and whether it runs on the submitter (with
// pthread_equal, since a pthread_t has no portable atomic form).
static void pool_complete(void *arg, bool ran) {
  (void)arg;
  atomic_store(&g_complete_ran, ran ? 1 : 0);
  atomic_store(&g_complete_saw_task_done, atomic_load(&g_task_done));
  atomic_store(&g_complete_order, atomic_fetch_add(&g_seq, 1));
  atomic_store(&g_complete_on_submitter,
               pthread_equal(pthread_self(), g_submitter_thread) ? 1 : 0);
}

// One sentence of cthrpool_submit(3) promises an order, a thread and the ran
// flag; this test pins all three. cthrpool_wait() synchronizes, no sleeps.
TEST(compat_callbacks, on_complete_runs_after_its_task_and_on_a_worker) {
  atomic_store(&g_seq, 0);
  atomic_store(&g_task_order, -1);
  atomic_store(&g_complete_order, -1);
  atomic_store(&g_complete_on_submitter, -1);
  atomic_store(&g_complete_saw_task_done, -1);
  atomic_store(&g_task_done, 0);
  atomic_store(&g_complete_ran, -1);
  g_submitter_thread = pthread_self();

  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(2, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTHRPOOL_NULL);

  ccol_retval r = cthrpool_submit(pool, pool_task, NULL, pool_complete);
  cthrpool_wait(pool);
  cthrpool_destroy(pool);

  int task_order = atomic_load(&g_task_order);
  int complete_order = atomic_load(&g_complete_order);

  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(task_order, 0);
  REQUIRE_EQ(complete_order, 1);
  REQUIRE_EQ(atomic_load(&g_complete_saw_task_done), 1);
  REQUIRE_EQ(atomic_load(&g_complete_on_submitter), 0);
  REQUIRE_EQ(atomic_load(&g_complete_ran), 1);
}

// State of the discard test: Counts of on_complete calls, the gate that holds
// the only worker, and whether the blocker task started.
static atomic_int g_discard_calls;
static atomic_int g_discard_ran_true;
static atomic_int g_discard_on_submitter;
static atomic_int g_discard_gate;
static atomic_int g_discard_blocker_running;

// Holds the only worker of the discard test until g_discard_gate opens, so the
// tasks behind it stay queued for the shutdown to discard. It sets
// g_discard_blocker_running first. Its 120 s bound is a hang guard, longer than
// the releaser's, so the worker never takes a queued task before the shutdown
// could discard it.
static void discard_blocker_task(void *arg) {
  (void)arg;
  atomic_store(&g_discard_blocker_running, 1);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; i < 120000 && !atomic_load(&g_discard_gate); i++)
    nanosleep(&ts, NULL);
}

// The task the discard test queues. It does nothing and must never run.
static void discard_task(void *arg) { (void)arg; }

// The on_complete of the discarded tasks. It counts the call, notes a ran or
// off-thread call, and frees arg, the task's block. The first call with ran
// false comes after the shutdown took the tasks out of the queue, so it opens
// the worker's gate and the shutdown can join the worker.
static void discard_complete(void *arg, bool ran) {
  if (ran) atomic_fetch_add(&g_discard_ran_true, 1);
  if (!pthread_equal(pthread_self(), g_submitter_thread))
    atomic_store(&g_discard_on_submitter, 0);
  atomic_fetch_add(&g_discard_calls, 1);
  if (!ran) atomic_store(&g_discard_gate, 1);
  free(arg);
}

// Thread body that opens g_discard_gate after 60 s when no discarded
// on_complete came, so the test fails instead of hanging. A shorter bound would
// let the worker run the queued tasks on a slow machine.
static void *discard_release_gate(void *arg) {
  (void)arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; i < 60000 && !atomic_load(&g_discard_gate); i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_discard_gate, 1);
  return NULL;
}

// on_complete runs exactly once for every accepted task, and a shutdown runs
// the ran == false calls on its own thread before it returns. Each task owns a
// heap block that only its on_complete frees, so a missed call leaks and a
// second one is a double free.
TEST(compat_callbacks, a_discarded_task_gets_on_complete_with_ran_false) {
  enum { DISCARDED = 8 };
  atomic_store(&g_discard_calls, 0);
  atomic_store(&g_discard_ran_true, 0);
  atomic_store(&g_discard_on_submitter, 1);
  atomic_store(&g_discard_gate, 0);
  atomic_store(&g_discard_blocker_running, 0);
  g_submitter_thread = pthread_self();

  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(1, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTHRPOOL_NULL);

  bool blocker_ok =
      cthrpool_submit(pool, discard_blocker_task, NULL, NULL) == ccol_success;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0;
       blocker_ok && i < 60000 && !atomic_load(&g_discard_blocker_running); i++)
    nanosleep(&ts, NULL);
  int accepted = 0;
  for (int i = 0; i < DISCARDED; i++) {
    void *block = malloc(16);
    if (!block) break;
    if (cthrpool_submit(pool, discard_task, block, discard_complete) !=
        ccol_success) {
      free(block);
      break;
    }
    accepted++;
  }
  pthread_t releaser;
  int created = pthread_create(&releaser, NULL, discard_release_gate, NULL);
  if (created != 0) atomic_store(&g_discard_gate, 1);
  cthrpool_shutdown_immediate(pool);
  int calls_at_return = atomic_load(&g_discard_calls);
  if (created == 0) pthread_join(releaser, NULL);
  cthrpool_destroy(pool);

  REQUIRE_TRUE(blocker_ok);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(accepted, DISCARDED);
  REQUIRE_EQ(calls_at_return, DISCARDED);
  REQUIRE_EQ(atomic_load(&g_discard_calls), DISCARDED);
  REQUIRE_EQ(atomic_load(&g_discard_ran_true), 0);
  REQUIRE_EQ(atomic_load(&g_discard_on_submitter), 1);
}

// ORDER_N is how many tasks the submission-order test runs. Each task writes
// its argument into the next free slot, so the slot order is the execution
// order.
#define ORDER_N 16
static atomic_int g_order_observed[ORDER_N];
static atomic_int g_order_slot;
static int g_order_args[ORDER_N];

// The task of the submission-order test. It writes *arg into the next free slot
// of g_order_observed, taken from g_order_slot.
static void order_task(void *arg) {
  int which = atomic_fetch_add(&g_order_slot, 1);
  if (which >= 0 && which < ORDER_N)
    atomic_store(&g_order_observed[which], *(const int *)arg);
}

// One worker on a FIFO queue runs tasks in submission order. With more workers
// the pool may interleave them, and nothing is asserted about that.
TEST(compat_callbacks,
     a_single_worker_pool_runs_its_tasks_in_submission_order) {
  atomic_store(&g_order_slot, 0);
  for (int i = 0; i < ORDER_N; i++) atomic_store(&g_order_observed[i], -1);

  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(1, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTHRPOOL_NULL);

  bool submitted_all = true;
  for (int i = 0; i < ORDER_N; i++) {
    g_order_args[i] = i;
    if (cthrpool_submit(pool, order_task, &g_order_args[i], NULL) !=
        ccol_success) {
      submitted_all = false;
      break;
    }
  }
  cthrpool_wait(pool);
  cthrpool_destroy(pool);

  bool in_order = submitted_all;
  for (int i = 0; i < ORDER_N && in_order; i++) {
    if (atomic_load(&g_order_observed[i]) != i) in_order = false;
  }
  REQUIRE_TRUE(submitted_all);
  REQUIRE_TRUE(in_order);
}
