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

#include <common.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
TAU_MAIN()  // sets up Tau (+ main function)

/* ---- counting allocator ---- */

static size_t g_malloc_count = 0;
static size_t g_free_count = 0;

static void *_counting_malloc(size_t size) {
  g_malloc_count++;
  return malloc(size);
}

static void _counting_free(void *ptr) {
  if (ptr) g_free_count++;
  free(ptr);
}

static void *_counting_calloc(size_t count, size_t size) {
  return calloc(count, size);
}

static void *_counting_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}

static void reset_counters(void) {
  g_malloc_count = 0;
  g_free_count = 0;
}

/* A single shared instance, returned by pointer from a function (rather
 * than callers taking the address of a local/global directly), so that
 * passing it to _mem_alloc/_mem_free/ccol_scoped_ptr_mp doesn't trip
 * -Waddress ("the address of X will always evaluate as true") the way a
 * literal &some_local_var would at the macro call site. */
static ccol_memmgmt_procs_t g_counting_procs_storage = {
    .malloc = _counting_malloc,
    .calloc = _counting_calloc,
    .realloc = _counting_realloc,
    .free = _counting_free};

static ccol_memmgmt_procs_t *counting_procs(void) {
  return &g_counting_procs_storage;
}

// CCOL_SCOPED_PTR TESTS

TEST(scoped_ptr, default_allocator_frees_on_scope_exit) {
  {
    ccol_scoped_ptr(buf, char);
    buf = malloc(16);
    REQUIRE_NE((void *)buf, NULL);
    strcpy(buf, "hello");
  }
  /* No counting hook exists for the default allocator; absence of a leak
   * or double free is confirmed separately under `make memtest`. This
   * test exists to exercise the non-_mp macro form itself compiling and
   * running correctly. */
}

TEST(scoped_ptr, custom_allocator_frees_via_provided_procs) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _mem_alloc(mp, sizeof(int) * 4);
    REQUIRE_NE((void *)buf, NULL);
    REQUIRE_EQ(g_malloc_count, (size_t)1);
    REQUIRE_EQ(g_free_count, (size_t)0);
    buf[0] = 42;
  }
  REQUIRE_EQ(g_free_count, (size_t)1);
}

TEST(scoped_ptr, unused_pointer_is_a_no_op) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    (void)buf;
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
}

TEST(scoped_ptr, only_final_value_is_freed_on_reassignment) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    /* Reassigning without freeing the earlier value first is the macro's
     * documented caveat, not something it tries to solve on its own. */
    _mem_free(mp, buf);
    buf = _mem_alloc(mp, sizeof(int) * 2);
    REQUIRE_NE((void *)buf, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, release_prevents_auto_free_and_transfers_ownership) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  int *released = NULL;
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    *buf = 7;
    released = ccol_scoped_ptr_release(buf);
    REQUIRE_EQ((void *)buf, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
  REQUIRE_NE((void *)released, NULL);
  REQUIRE_EQ(*released, 7);
  _mem_free(mp, released);
  REQUIRE_EQ(g_free_count, (size_t)1);
}

TEST(scoped_ptr, release_of_never_assigned_pointer_returns_null) {
  ccol_scoped_ptr(buf, int);
  int *out = ccol_scoped_ptr_release(buf);
  REQUIRE_EQ((void *)out, NULL);
  REQUIRE_EQ((void *)buf, NULL);
}

TEST(scoped_ptr, multiple_independent_pointers_in_same_scope) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(a, int, mp);
    ccol_scoped_ptr_mp(b, char, mp);
    a = _mem_alloc(mp, sizeof(int));
    b = _mem_alloc(mp, 32);
    REQUIRE_NE((void *)a, NULL);
    REQUIRE_NE((void *)b, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, nested_scope_frees_at_inner_block_exit) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(outer, int, mp);
    outer = _mem_alloc(mp, sizeof(int));
    {
      ccol_scoped_ptr_mp(inner, int, mp);
      inner = _mem_alloc(mp, sizeof(int));
      REQUIRE_EQ(g_free_count, (size_t)0);
    }
    REQUIRE_EQ(g_free_count, (size_t)1);
    REQUIRE_NE((void *)outer, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)2);
}

static bool process_may_fail(bool should_fail) {
  ccol_memmgmt_procs_t *mp = counting_procs();
  ccol_scoped_ptr_mp(buf, int, mp);
  buf = _mem_alloc(mp, sizeof(int) * 8);
  if (!buf) return false;
  if (should_fail) return false; /* early return, buf still freed */
  buf[0] = 1;
  return true;
}

TEST(scoped_ptr, early_return_from_multiple_paths_still_frees) {
  reset_counters();

  REQUIRE_FALSE(process_may_fail(true));
  REQUIRE_EQ(g_malloc_count, (size_t)1);
  REQUIRE_EQ(g_free_count, (size_t)1);

  reset_counters();
  REQUIRE_TRUE(process_may_fail(false));
  REQUIRE_EQ(g_malloc_count, (size_t)1);
  REQUIRE_EQ(g_free_count, (size_t)1);
}
