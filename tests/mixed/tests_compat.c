/* The corpus for behavioural compatibility.
 *
 * The "Behavioral compatibility" section of doc/compatibility.md promises
 * that six things do not change inside the 1.x series:
 * - the retval that a given input gives, and which conditions are fatal
 * - which operations invalidate an iterator or a borrowed pointer
 * - the ownership rules for a key, a value and a node
 * - the order of the registered callbacks, and the thread that runs them
 * - the conditions in which the library calls an allocator that the caller
 *   gives
 * - the thread-safety class of each type.
 *
 * `make check_abi` covers the shape of the interface. Nothing else covers
 * the list above. A rewrite can keep each signature and each struct layout
 * and also make these changes:
 * - It frees a node that the caller owns.
 * - It runs a callback on a different thread.
 * - It allocates from the default allocator, and not from the allocator
 *   that it got.
 * The ABI gate and the suite of each module stay green. This binary makes
 * sure that such a change is not silent.
 *
 * Two rules control what this corpus holds. These rules prevent the corpus
 * from becoming a burden that somebody deletes when it is not convenient.
 *
 * This corpus pins only DOCUMENTED promises. Each assertion below matches a
 * sentence in a doc/ guide, in a man page, or in the doc comment of a public
 * header. The comment on each test names the promise that it pins. It pins
 * no incidental behaviour. The policy explicitly allows a faster
 * replacement of an algorithm or a data structure inside 1.x. An assertion
 * on an exact allocation count, a bucket order or a capacity progression
 * forbids what the policy allows.
 *
 * Each expectation uses terms that the library does not control. A test
 * that compares against a constant of the library passes for each value of
 * that constant. Such a test checks nothing. Therefore, each test compares a
 * retval against a numeric literal. A test of a promise about the source of
 * memory uses an allocator that this file owns.
 *
 * For invalidation, this corpus pins only positive guarantees. A test can
 * assert "a reference stays valid across X". A test cannot assert "Y
 * invalidates a reference". A read of a pointer that the library
 * invalidated is undefined behaviour. Therefore, such a test checks the
 * sanitizer, and not the contract. Do not "complete" the iterator section
 * with such a test.
 *
 * These tests are not vacuous. A file of assertions with no output of its
 * own looks the same when it checks something and when it checks nothing.
 * Therefore, each category below is known to fail against a library that breaks
 * it. Each of these five changes to a scratch copy of src/ makes the named
 * tests fail. When somebody adds a category, do these changes again. A
 * promise that has no failing change behind it is not proved checkable.
 *
 *   1. Call on_complete before task->fn in the worker loop of
 *      cthreadpool.c.
 *      -> compat_callbacks.on_complete_runs_after_its_task_and_on_a_worker
 *   2. Drop the __cjson_destroy(child) on the insert-failed path of
 *      cjson_dictionary_set().
 *      -> compat_ownership.a_json_attach_that_runs_out_of_memory_...
 *   3. Allocate the first backing store of a vector with malloc(), and not
 *      with _ccol_mem_alloc(mmgt_procs, ...).
 *      -> this stops the process in the allocator of this file, which
 *         refuses to free a block that it never gave out
 *   4. Skip __chmap_iterator_destroy() when an open-addressing iteration
 *      ends.
 *      -> compat_iterators.running_an_iterator_to_the_end_returns_its_memory
 *         and compat_allocator.an_iterator_is_allocated_from_the_container...
 *   5. Make chmap_get_elem_ref() return a pointer to one shared static copy,
 *      and not a pointer into the map.
 *      -> all three compat_pointer_stability tests
 *
 * In a script for those changes, make sure that the anchor matches exactly
 * one time before you build. Check the status of the build and the
 * timestamp of the binary separately from the run. A mutation that does not
 * compile leaves the previous binary in place. A run of that binary passes,
 * and it looks exactly like a test that found nothing.
 */

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
#include <time.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                    A COUNTING, IDENTIFYING ALLOCATOR                       */
/* ========================================================================== */

/* This allocator answers the two questions that the allocator promise
 * raises: did the library use this allocator, and did everything that it
 * gave out come back?
 *
 * It also identifies its own blocks: it checks every free against the live
 * set and stops the process on a pointer that it never made. That is what
 * turns "the container frees with the allocator that it allocated from" from
 * a claim into a check; without it, a container that allocated from malloc
 * and freed through these procs looks exactly like a correct one.
 *
 * This allocator is single-threaded by construction, because the registry
 * below is a plain array with no synchronisation. Give it only to objects
 * that this file drives from one thread, and never to a thread pool or to a
 * cache under concurrent use: a test-only counter that races is a sanitizer
 * finding of its own.
 */
#define CA_MAX_LIVE 8192

typedef struct {
  void *live[CA_MAX_LIVE];
  size_t live_count;
  size_t alloc_calls; /* malloc, calloc and realloc that gave memory */
  size_t free_calls;
  /* Once alloc_calls reaches this value, the allocator refuses every further
   * request. SIZE_MAX keeps the allocator honest. */
  size_t fail_from;
} counting_allocator_t;

static counting_allocator_t g_ca;

static void ca_reset(void) {
  memset(&g_ca, 0, sizeof(g_ca));
  g_ca.fail_from = SIZE_MAX;
}

static void ca_register(void *p) {
  if (!p) return;
  if (g_ca.live_count >= CA_MAX_LIVE) {
    fprintf(stderr, "counting allocator: live-block registry overflow\n");
    abort();
  }
  g_ca.live[g_ca.live_count++] = p;
}

static void ca_forget(void *p) {
  for (size_t i = 0; i < g_ca.live_count; i++) {
    if (g_ca.live[i] == p) {
      g_ca.live[i] = g_ca.live[--g_ca.live_count];
      return;
    }
  }
  /* This is a block that this allocator never gave out, so the container
   * mixes two allocators, which is the defect that this registry exists to
   * catch. */
  fprintf(stderr, "counting allocator: free of a foreign pointer %p\n", p);
  abort();
}

static bool ca_refusing(void) { return g_ca.alloc_calls >= g_ca.fail_from; }

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
  /* The registry is updated before the call, not after it. Once realloc()
   * returns, the value of the old pointer is indeterminate, so this file
   * must not inspect it, not even to compare it against the registry. The
   * code retires the block while the block is still valid, and puts it back
   * only if realloc refuses. */
  if (old) ca_forget(old);
  void *p = realloc(old, n);
  if (!p) {
    if (old) ca_register(old); /* refused, so the original block is live */
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

static ccol_memmgmt_procs_t g_ca_procs = {.malloc = ca_malloc,
                                          .calloc = ca_calloc,
                                          .realloc = ca_realloc,
                                          .free = ca_free};

static ccol_memmgmt_procs_t *ca_procs(void) { return &g_ca_procs; }

/* ========================================================================== */
/*         PROMISE: no enumerator's numeric value changes within 1.x          */
/* ========================================================================== */

/* doc/compatibility.md and ccollections(7) state this promise. Both give
 * zero as the value of ccol_success, because call sites compare against
 * that value directly. The literals here are intentional: a comparison of
 * an enumerator against itself is true under any renumbering, and the
 * promise forbids a renumbering. */
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

/* ========================================================================== */
/*   PROMISE: the circumstances under which a caller-supplied allocator runs  */
/* ========================================================================== */

/* ccollections(7): each module that allocates accepts a
 * ccol_memmgmt_procs_t * when it creates an object, so the storage of the
 * container comes from those procs and goes back to them, and these tests
 * pin that. They assert only that the count is not zero and that it
 * balances, not the count itself, because the policy allows a change of
 * algorithm, and such a change changes the counts. */
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
  /* ccollections(7): a NULL pointer selects the standard malloc(3),
   * calloc(3), realloc(3) and free(3). This test makes sure that nothing
   * touches the allocator of the caller, which is the only half of the
   * promise that a test can see. */
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
  /* An iterator is memory that the caller never names, so only two
   * statements about it are possible: which allocator it comes from, and
   * that it goes back. Both are part of the same promise. */
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

TEST(compat_allocator, a_refused_allocation_is_reported_rather_than_fatal) {
  /* This is the companion of the promise about a retval: on the raw function
   * layer, a lack of memory is a returned value, never a call to
   * ccol_fatal_err(). A process that stopped here would fail this whole
   * binary instead of one test. */
  ca_reset();
  chmap_construct_mp(m, int, int, ca_procs());

  g_ca.fail_from = g_ca.alloc_calls; /* refuse the next request and the
                                        ones after it */
  ccol_retval_t worst = ccol_success;
  for (int i = 0; i < 4096 && worst == ccol_success; i++) {
    int k = i, v = i;
    cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    worst = chmap_insert_elem(m, &kp, &vp);
  }
  g_ca.fail_from = SIZE_MAX;

  chmap_destroy(m);
  REQUIRE_EQ((int)worst, -1); /* ccol_not_enough_memory */
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

/* ========================================================================== */
/*            PROMISE: the ownership rules for keys, values and nodes         */
/* ========================================================================== */

TEST(compat_ownership,
     a_map_copies_a_string_key_and_value_into_its_own_storage) {
  /* doc/chashmap.md and chmap_insert_elem(3): the map stores copies, so the
   * buffer of the caller stays the property of the caller, to use again or
   * to free. After the insert, the test writes over the source buffers to
   * show the difference between a copy and a kept pointer: a map that
   * aliased the caller would give back the text that the test wrote. */
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

  /* This uses get_ptr rather than get, because chmap_get() stops the process
   * when the key is absent, which is exactly what a regression here
   * produces, and such a stop takes every other test in this binary down
   * with it. */
  char *const *found = chmap_get_ptr(m, "alice");
  bool copied = found && *found && strcmp(*found, "engineer") == 0;
  chmap_destroy(m);
  REQUIRE_TRUE(copied);
}

TEST(compat_ownership, attaching_a_json_child_transfers_it_to_the_parent) {
  /* cjson.h: "cjson_list_push() and cjson_dictionary_set() transfer
   * ownership of the child to the parent; do not free it afterwards." A
   * destroy of the parent alone must return every block, which shows that
   * the parent took the child over. This test deliberately never destroys
   * the child. */
  ca_reset();
  cjson root = cjson_create_dictionary_mp(ca_procs());
  cjson child = cjson_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  ccol_retval_t r =
      built ? cjson_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  cjson_destroy(root);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     a_json_get_hands_back_a_reference_the_parent_still_owns) {
  /* cjson.h: "cjson_get() returns a NON-OWNING reference valid until the
   * tree is mutated or destroyed." The caller must not free it, so a destroy
   * of the parent alone must account for everything. */
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

TEST(compat_ownership,
     an_already_attached_json_child_is_refused_and_left_alone) {
  /* The doc comment of cjson_list_push(): a child that already has a parent
   * is "rejected with ccol_invalid_args and @p child is left completely
   * untouched, still owned by whatever it was already attached to". This
   * failure therefore does NOT transfer the ownership, and the first parent
   * frees the child. What a failed attach does depends on which failure it
   * was, and that fine point is worth a test. */
  ca_reset();
  cjson first = cjson_create_list_mp(ca_procs());
  cjson second = cjson_create_list_mp(ca_procs());
  cjson child = cjson_create_string_mp("v", ca_procs());
  bool built =
      first && second && child && cjson_list_push(first, child) == ccol_success;

  ccol_retval_t r = built ? cjson_list_push(second, child) : ccol_success;
  /* `first` still owns child, so a destroy of both parents, and of nothing
   * else, must account for every block exactly once. */
  cjson_destroy(second);
  cjson_destroy(first);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -4); /* ccol_invalid_args */
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     a_json_attach_that_runs_out_of_memory_destroys_the_child) {
  /* This is the other half of the rule above: the ownership also transfers
   * on the path where memory runs out. The child is gone once the call
   * returns, so freeing it here would be a double free, and a destroy of the
   * parent alone must balance. */
  ca_reset();
  cjson root = cjson_create_dictionary_mp(ca_procs());
  cjson child = cjson_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  g_ca.fail_from = g_ca.alloc_calls; /* the attach cannot allocate */
  ccol_retval_t r =
      built ? cjson_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  g_ca.fail_from = SIZE_MAX;

  cjson_destroy(root);
  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -1); /* ccol_not_enough_memory */
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

/* cjson and cyaml share one ownership rule: ccol_invalid_args means that the
 * function rejected the arguments, that nothing touched the child, and that
 * the caller still owns it, while every other failure transfers the
 * ownership and deep-frees the child. The tests above and below pin that for
 * each module separately, because each header states it separately.
 *
 * Both modules also refuse text that is not well-formed UTF-8, in their
 * parsers and in their direct API, and neither replaces anything with
 * U+FFFD. Each module's own suite pins that rule and its messages; this
 * corpus pins only the ownership rule. */

TEST(compat_ownership, attaching_a_yaml_child_transfers_it_to_the_parent) {
  /* cyaml has the same ownership rules as cjson. Its own header states them
   * separately, so these tests pin them separately. */
  ca_reset();
  cyaml root = cyaml_create_dictionary_mp(ca_procs());
  cyaml child = cyaml_create_string_mp("value", ca_procs());
  bool built = (root != NULL) && (child != NULL);

  ccol_retval_t r =
      built ? cyaml_dictionary_set(root, "k", child) : ccol_not_enough_memory;
  cyaml_destroy(root);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     an_already_attached_yaml_child_is_refused_and_left_alone) {
  /* cyaml splits its failures in the same way as cjson, and these tests pin
   * the split for each module separately instead of assuming that it carries
   * across. A child that already has a parent is rejected with
   * ccol_invalid_args and left alone: freeing it would tear a node out of
   * the tree that still owns it, and accepting it is a double free. A
   * borrowed reference from cyaml_dictionary_get() or cyaml_list_get() is
   * the way that the API itself names a node that already has a parent. */
  ca_reset();
  cyaml first = cyaml_create_list_mp(ca_procs());
  cyaml second = cyaml_create_list_mp(ca_procs());
  cyaml child = cyaml_create_string_mp("v", ca_procs());
  bool built =
      first && second && child && cyaml_list_push(first, child) == ccol_success;

  ccol_retval_t r = built ? cyaml_list_push(second, child) : ccol_success;
  /* `first` still owns child, so a destroy of both parents, and of nothing
   * else, must account for every block exactly once. */
  cyaml_destroy(second);
  cyaml_destroy(first);

  REQUIRE_TRUE(built);
  REQUIRE_EQ((int)r, -4); /* ccol_invalid_args */
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

TEST(compat_ownership,
     a_yaml_attach_that_runs_out_of_memory_destroys_the_child) {
  /* This is the other half of the split, and it matches that of cjson: a
   * failure that is not a rejection does transfer the ownership. The child
   * is gone once the call returns, so freeing it here would be a double
   * free. */
  enum { KIDS = 64 };
  ca_reset();
  cyaml root = cyaml_create_list_mp(ca_procs());
  cyaml kids[KIDS];
  bool built = (root != NULL);
  for (int i = 0; i < KIDS && built; i++) {
    kids[i] = cyaml_create_string_mp("value", ca_procs());
    if (!kids[i]) built = false;
  }

  /* A new list already has room, so refusing the next allocation does not
   * always reach a push; enough children to force the backing store to grow
   * do reach one. */
  g_ca.fail_from = g_ca.alloc_calls;
  bool some_push_failed = false;
  if (built) {
    for (int i = 0; i < KIDS; i++) {
      if (cyaml_list_push(root, kids[i]) != ccol_success)
        some_push_failed = true;
    }
  }
  g_ca.fail_from = SIZE_MAX;

  /* Nothing here frees a child: root owns a child that the code pushed, and
   * the push itself deep-freed a child that it refused. If either half of
   * that is wrong, the count of live blocks shows it. */
  cyaml_destroy(root);
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(some_push_failed);
  REQUIRE_EQ(g_ca.live_count, (size_t)0);
}

/* ========================================================================== */
/*    PROMISE: which operations invalidate an iterator or borrowed pointer    */
/* ========================================================================== */

/* Only the positive half can be asserted here. The header comment of this
 * file explains why a test of the invalidation direction would be undefined
 * behaviour rather than a test. */

TEST(compat_pointer_stability, a_reference_survives_lookups_of_other_keys) {
  /* chashmap.h: an insert, a delete or a resize invalidates a returned
   * pointer, but the pointer is invalidated "never by an unrelated
   * chmap_get_elem_ref/chmap_get/chmap_get_ptr call for a different
   * key". */
  chmap_construct(m, int, int);
  for (int i = 0; i < 32; i++) {
    int k = i, v = i * 7;
    chmap_insert(m, k, v);
  }

  int key = 5;
  cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
  const cmap_pair *held = NULL;
  ccol_retval_t r = chmap_get_elem_ref(m, &kp, &held);

  for (int i = 0; i < 32; i++) {
    if (i == 5) continue;
    int other = i;
    cmap_pair okp = {.ptr = &other, .size = sizeof(other)};
    const cmap_pair *tmp = NULL;
    (void)chmap_get_elem_ref(m, &okp, &tmp);
  }

  bool still_correct = (r == ccol_success) && held && held->ptr &&
                       (*(const int *)held->ptr == 35);
  chmap_destroy(m);
  REQUIRE_TRUE(still_correct);
}

TEST(compat_pointer_stability, several_references_may_be_held_at_once) {
  /* The same sentence: "two or more references returned for distinct keys
   * may be held concurrently and each remains valid independently". */
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
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success || !out || !out->ptr) {
      all_resolved = false;
      refs[i] = NULL;
      continue;
    }
    refs[i] = (const int *)out->ptr;
  }

  bool all_still_correct = all_resolved;
  for (int i = 0; i < 16 && all_still_correct; i++) {
    if (!refs[i] || *refs[i] != i * 11) all_still_correct = false;
  }

  chmap_destroy(m);
  REQUIRE_TRUE(all_still_correct);
}

TEST(compat_pointer_stability, updating_a_key_in_place_keeps_other_references) {
  /* A write through a reference is explicitly permitted ("Can modify value
   * in-place") and is not one of the operations that invalidate a pointer,
   * so a reference that a caller holds for another key goes on reading its
   * own value. */
  chmap_construct(m, int, int);
  for (int i = 0; i < 16; i++) {
    int k = i, v = i;
    chmap_insert(m, k, v);
  }

  int a = 3, b = 9;
  cmap_pair akp = {.ptr = &a, .size = sizeof(a)};
  cmap_pair bkp = {.ptr = &b, .size = sizeof(b)};
  const cmap_pair *aref = NULL, *bref = NULL;
  bool got = chmap_get_elem_ref(m, &akp, &aref) == ccol_success &&
             chmap_get_elem_ref(m, &bkp, &bref) == ccol_success;

  if (got) *(int *)aref->ptr = 1234;

  bool a_updated = got && *(const int *)aref->ptr == 1234;
  bool b_untouched = got && *(const int *)bref->ptr == 9;
  chmap_destroy(m);
  REQUIRE_TRUE(a_updated);
  REQUIRE_TRUE(b_untouched);
}

TEST(compat_iterators, a_null_handle_iterates_as_empty_for_every_container) {
  /* This is a deliberate contract across the modules: a NULL handle iterates
   * as empty instead of stopping the process, in the same way for all three
   * iterable containers. ccol_begin() calls ccol_fatal_err() when it gets an
   * error, so a regression here kills this binary instead of failing one
   * test. */
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
  /* The iterator destroys itself after _next_fn reports the end, so a loop
   * that runs to the end leaks nothing and needs no explicit destroy. */
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

TEST(compat_iterators, abandoning_an_iterator_early_returns_its_memory) {
  /* A break out of the loop before the end means that _next_fn never reports
   * the end, so the iterator is still live, and the scope-exit cleanup that
   * the declaration carries is what frees it. */
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

/* ========================================================================== */
/*        PROMISE: the order callbacks run in, and the thread each runs on    */
/* ========================================================================== */

/* clrucache eviction */

#define EV_MAX 32
static int g_ev_keys[EV_MAX];
static size_t g_ev_count;
static bool g_ev_inside_a_set;      /* true only while a set call is running */
static bool g_ev_ran_outside_a_set; /* set if the callback ever ran otherwise */
static pthread_t g_ev_main_thread;
static bool g_ev_ran_off_thread;

static void ev_reset(void) {
  memset(g_ev_keys, 0, sizeof(g_ev_keys));
  g_ev_count = 0;
  g_ev_inside_a_set = false;
  g_ev_ran_outside_a_set = false;
  g_ev_ran_off_thread = false;
  g_ev_main_thread = pthread_self();
}

static void ev_cb(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  if (!g_ev_inside_a_set) g_ev_ran_outside_a_set = true;
  if (!pthread_equal(pthread_self(), g_ev_main_thread))
    g_ev_ran_off_thread = true;
  if (g_ev_count < EV_MAX && key && key->ptr)
    g_ev_keys[g_ev_count++] = *(const int *)key->ptr;
}

static ccol_retval_t lru_set_int(clru_cache c, int k, int v) {
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  g_ev_inside_a_set = true;
  ccol_retval_t r = clrucache_set_full(c, &kp, &vp);
  g_ev_inside_a_set = false;
  return r;
}

static ccol_retval_t lru_get_int(clru_cache c, int k, int *out) {
  /* clrucache_get_full() gives back a heap COPY of the stored value, which
   * the caller becomes responsible for freeing, instead of filling a buffer
   * that the caller gives. These caches use the default allocator, so the
   * matching call is a plain free(). */
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {0};
  ccol_retval_t r = clrucache_get_full(c, &kp, &vp);
  if (r == ccol_success && vp.ptr) {
    if (vp.size == sizeof(*out)) memcpy(out, vp.ptr, sizeof(*out));
    free(vp.ptr);
  }
  return r;
}

TEST(compat_callbacks, a_small_cache_evicts_in_exact_global_lru_order) {
  /* clrucache_create_full(3): "Splitting begins at a capacity of 128 ...
   * Below that a cache is a single segment and evicts in one exact global
   * least-recently-used order." This test puts four keys into a cache of
   * three, with a lookup in between that moves one key back to the
   * most-recently-used end, so exactly one answer is correct for which key
   * leaves. */
  ev_reset();
  char *err = NULL;
  clru_cache c = clrucache_create_full(3, ccol_int, ccol_int, NULL, NULL, ev_cb,
                                       NULL, &err);
  REQUIRE_TRUE(c != CLRU_CACHE_INVALID);

  bool ok = lru_set_int(c, 1, 10) == ccol_success &&
            lru_set_int(c, 2, 20) == ccol_success &&
            lru_set_int(c, 3, 30) == ccol_success;

  /* Touch 1, so that 2 becomes the least recently used entry. */
  int got = 0;
  ok = ok && lru_get_int(c, 1, &got) == ccol_success && got == 10;

  /* Inserting a fourth key must evict 2, not 1. */
  ok = ok && lru_set_int(c, 4, 40) == ccol_success;

  size_t evictions = g_ev_count;
  int first_evicted = evictions > 0 ? g_ev_keys[0] : -1;
  clru_destroy(c);

  REQUIRE_TRUE(ok);
  REQUIRE_EQ(evictions, (size_t)1);
  REQUIRE_EQ(first_evicted, 2);
}

TEST(compat_callbacks, the_eviction_callback_runs_synchronously_on_the_caller) {
  /* clrucache.h: "Invoked synchronously (while the owning segment's lock is
   * held) when an entry is evicted". Synchronously means inside the call that
   * caused the eviction and on the thread of that caller. Moving the callback
   * to a deferred or background reclaimer breaks both halves, and that move
   * is exactly what this test exists to stop. */
  ev_reset();
  char *err = NULL;
  clru_cache c = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL, ev_cb,
                                       NULL, &err);
  REQUIRE_TRUE(c != CLRU_CACHE_INVALID);

  bool ok = true;
  for (int i = 1; i <= 6 && ok; i++) ok = lru_set_int(c, i, i) == ccol_success;

  size_t evictions = g_ev_count;
  bool ran_outside = g_ev_ran_outside_a_set;
  bool ran_off_thread = g_ev_ran_off_thread;
  clru_destroy(c);

  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(evictions > 0);
  REQUIRE_FALSE(ran_outside);
  REQUIRE_FALSE(ran_off_thread);
}

/* clrucache remote setter */

static bool g_setter_should_succeed;
static size_t g_setter_calls;
static clru_cache g_setter_cache;

static bool remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  g_setter_calls++;
  /* "the remote setter (if provided) is always called before the cache is
   * updated". A read of the key here must therefore still miss or find the
   * previous value, and never the value that the code is writing. A read
   * through the public API would deadlock against the segment lock, so the
   * check is the weaker one that the contract permits: the setter ran at
   * all, and its refusal left the cache alone, which the caller below
   * asserts. */
  (void)key;
  return g_setter_should_succeed;
}

TEST(compat_callbacks, a_refused_remote_setter_leaves_the_cache_unchanged) {
  /* clrucache.h: "the remote setter (if provided) is always called before
   * the cache is updated. If the remote call fails the cache is not updated
   * and clrucache_set_full() returns ccol_unexpected_failure." A test can
   * observe both halves from outside: the count of the calls and the absence
   * of the key. */
  ev_reset();
  g_setter_calls = 0;
  g_setter_should_succeed = false;
  char *err = NULL;
  g_setter_cache = clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                         remote_setter, NULL, NULL, &err);
  REQUIRE_TRUE(g_setter_cache != CLRU_CACHE_INVALID);

  ccol_retval_t r = lru_set_int(g_setter_cache, 7, 70);

  int got = 0;
  ccol_retval_t lookup = lru_get_int(g_setter_cache, 7, &got);
  size_t calls = g_setter_calls;
  clru_destroy(g_setter_cache);

  REQUIRE_EQ((int)r, -18);      /* ccol_unexpected_failure */
  REQUIRE_EQ(calls, (size_t)1); /* the setter ran */
  REQUIRE_NE((int)lookup, 0);   /* the key never reached the cache */
}

TEST(compat_callbacks, an_accepted_remote_setter_runs_before_the_cache_update) {
  ev_reset();
  g_setter_calls = 0;
  g_setter_should_succeed = true;
  char *err = NULL;
  g_setter_cache = clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                         remote_setter, NULL, NULL, &err);
  REQUIRE_TRUE(g_setter_cache != CLRU_CACHE_INVALID);

  ccol_retval_t r = lru_set_int(g_setter_cache, 7, 70);
  int got = 0;
  ccol_retval_t lookup = lru_get_int(g_setter_cache, 7, &got);
  size_t calls = g_setter_calls;
  clru_destroy(g_setter_cache);

  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(calls, (size_t)1);
  REQUIRE_EQ((int)lookup, 0);
  REQUIRE_EQ(got, 70);
}

/* cthreadpool task and completion */

/* The worker threads share these, so every one of them is atomic. A plain
 * counter here would be a true data race, and a sanitizer finding in a file
 * whose whole job is to be trustworthy. */
static atomic_int g_seq;
static atomic_int g_task_order;
static atomic_int g_complete_order;
static atomic_int g_complete_on_submitter;
static atomic_int g_complete_saw_task_done;
static atomic_int g_task_done;
static atomic_int g_complete_ran;
static pthread_t g_submitter_thread;

static void pool_task(void *arg) {
  (void)arg;
  atomic_store(&g_task_order, atomic_fetch_add(&g_seq, 1));
  atomic_store(&g_task_done, 1);
}

static void pool_complete(void *arg, bool ran) {
  (void)arg;
  atomic_store(&g_complete_ran, ran ? 1 : 0);
  atomic_store(&g_complete_saw_task_done, atomic_load(&g_task_done));
  atomic_store(&g_complete_order, atomic_fetch_add(&g_seq, 1));
  /* This compares with pthread_equal against a value that the code captured
   * before the submit, instead of publishing a pthread_t across threads,
   * because a pthread_t has no portable atomic form. */
  atomic_store(&g_complete_on_submitter,
               pthread_equal(pthread_self(), g_submitter_thread) ? 1 : 0);
}

TEST(compat_callbacks, on_complete_runs_after_its_task_and_on_a_worker) {
  /* cthreadpool.h, ctpool_submit(): "A worker calls on_complete(arg, true)
   * right after fn returns, on that worker thread". That one sentence is a
   * promise about an order, about a thread and about the ran flag, and this
   * test pins all three. ctpool_wait() gives the synchronisation, so nothing
   * here depends on a sleep or on a margin. */
  atomic_store(&g_seq, 0);
  atomic_store(&g_task_order, -1);
  atomic_store(&g_complete_order, -1);
  atomic_store(&g_complete_on_submitter, -1);
  atomic_store(&g_complete_saw_task_done, -1);
  atomic_store(&g_task_done, 0);
  atomic_store(&g_complete_ran, -1);
  g_submitter_thread = pthread_self();

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(2, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTPOOL_INVALID);

  ccol_retval_t r = ctpool_submit(pool, pool_task, NULL, pool_complete);
  ctpool_wait(pool);
  ctpool_destroy(pool);

  int task_order = atomic_load(&g_task_order);
  int complete_order = atomic_load(&g_complete_order);

  REQUIRE_EQ((int)r, 0);
  REQUIRE_EQ(task_order, 0);
  REQUIRE_EQ(complete_order, 1);
  REQUIRE_EQ(atomic_load(&g_complete_saw_task_done), 1);
  REQUIRE_EQ(atomic_load(&g_complete_on_submitter), 0);
  REQUIRE_EQ(atomic_load(&g_complete_ran), 1);
}

/* Each discarded task of the test below owns one heap block, which its
 * on_complete frees. */
static atomic_int g_discard_calls;
static atomic_int g_discard_ran_true;
static atomic_int g_discard_on_submitter;
static atomic_int g_discard_gate;
static atomic_int g_discard_blocker_running;

static void discard_blocker_task(void *arg) {
  (void)arg;
  atomic_store(&g_discard_blocker_running, 1);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; i < 10000 && !atomic_load(&g_discard_gate); i++)
    nanosleep(&ts, NULL);
}

static void discard_task(void *arg) { (void)arg; }

static void discard_complete(void *arg, bool ran) {
  if (ran) atomic_fetch_add(&g_discard_ran_true, 1);
  if (!pthread_equal(pthread_self(), g_submitter_thread))
    atomic_store(&g_discard_on_submitter, 0);
  atomic_fetch_add(&g_discard_calls, 1);
  free(arg);
}

static void *discard_release_gate(void *arg) {
  (void)arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
  nanosleep(&ts, NULL);
  atomic_store(&g_discard_gate, 1);
  return NULL;
}

TEST(compat_callbacks, a_discarded_task_gets_on_complete_with_ran_false) {
  /* cthreadpool.h, ctpool_submit(): "on_complete runs exactly once for every
   * task that a submit accepts" and "The on_complete(arg, false) calls run on
   * the thread that makes that call, before the call returns". A caller that
   * hands ownership of arg to the task relies on both: each queued task below
   * owns a heap block that only its on_complete frees, so a missed call is a
   * leak and a second call is a double free. */
  enum { DISCARDED = 8 };
  atomic_store(&g_discard_calls, 0);
  atomic_store(&g_discard_ran_true, 0);
  atomic_store(&g_discard_on_submitter, 1);
  atomic_store(&g_discard_gate, 0);
  atomic_store(&g_discard_blocker_running, 0);
  g_submitter_thread = pthread_self();

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(1, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTPOOL_INVALID);

  bool blocker_ok =
      ctpool_submit(pool, discard_blocker_task, NULL, NULL) == ccol_success;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0;
       blocker_ok && i < 10000 && !atomic_load(&g_discard_blocker_running); i++)
    nanosleep(&ts, NULL);
  int accepted = 0;
  for (int i = 0; i < DISCARDED; i++) {
    void *block = malloc(16);
    if (!block) break;
    if (ctpool_submit(pool, discard_task, block, discard_complete) !=
        ccol_success) {
      free(block);
      break;
    }
    accepted++;
  }
  pthread_t releaser;
  int created = pthread_create(&releaser, NULL, discard_release_gate, NULL);
  if (created != 0) atomic_store(&g_discard_gate, 1);
  ctpool_shutdown_immediate(pool);
  int calls_at_return = atomic_load(&g_discard_calls);
  if (created == 0) pthread_join(releaser, NULL);
  ctpool_destroy(pool);

  REQUIRE_TRUE(blocker_ok);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(accepted, DISCARDED);
  REQUIRE_EQ(calls_at_return, DISCARDED);
  REQUIRE_EQ(atomic_load(&g_discard_calls), DISCARDED);
  REQUIRE_EQ(atomic_load(&g_discard_ran_true), 0);
  REQUIRE_EQ(atomic_load(&g_discard_on_submitter), 1);
}

/* Each task records its own argument in the next free slot, so the order of
 * the slots is the order of execution. */
#define ORDER_N 16
static atomic_int g_order_observed[ORDER_N];
static atomic_int g_order_slot;
static int g_order_args[ORDER_N];

static void order_task(void *arg) {
  int which = atomic_fetch_add(&g_order_slot, 1);
  if (which >= 0 && which < ORDER_N)
    atomic_store(&g_order_observed[which], *(const int *)arg);
}

TEST(compat_callbacks,
     a_single_worker_pool_runs_its_tasks_in_submission_order) {
  /* The queue is FIFO and one worker takes tasks from it, so the tasks run in
   * the order in which the test submitted them. This is the weakest form of
   * the promise that still says something: with more than one worker, the
   * pool is explicitly free to interleave the tasks, so this test asserts
   * nothing about that case. */
  atomic_store(&g_order_slot, 0);
  for (int i = 0; i < ORDER_N; i++) atomic_store(&g_order_observed[i], -1);

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(1, 0, NULL, &err);
  REQUIRE_TRUE(pool != CTPOOL_INVALID);

  bool submitted_all = true;
  for (int i = 0; i < ORDER_N; i++) {
    g_order_args[i] = i;
    if (ctpool_submit(pool, order_task, &g_order_args[i], NULL) !=
        ccol_success) {
      submitted_all = false;
      break;
    }
  }
  ctpool_wait(pool);
  ctpool_destroy(pool);

  bool in_order = submitted_all;
  for (int i = 0; i < ORDER_N && in_order; i++) {
    if (atomic_load(&g_order_observed[i]) != i) in_order = false;
  }
  REQUIRE_TRUE(submitted_all);
  REQUIRE_TRUE(in_order);
}
