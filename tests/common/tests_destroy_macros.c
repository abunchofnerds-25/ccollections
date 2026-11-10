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

// Every public destroy, free and release macro must evaluate its handle
// argument once, call the destroy function once with the value the argument
// held, and then clear the argument. This binary checks only the macros: the
// destroy functions are stubs defined here that record their calls, so none
// of the library is linked.
//
// Each case walks an array backwards with `while (k) F(a[--k])`. A macro that
// evaluates its argument twice decrements k twice per call, destroys the
// wrong elements and skips others. The array sits at the end of a larger one,
// so an index driven below 0 still points at our own storage and we get an
// assertion failure instead of undefined behaviour. The binary is built with
// -Wshadow and every case nests one call in the argument of another, so a
// destroy macro whose temporary hides another one fails the build.

#include <cbstmap.h>
#include <chashmap.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <citerators.h>
#include <cjson.h>
#include <clrucache.h>
#include <cmempool.h>
#include <cstring.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <cvector.h>
#include <cyaml.h>
#include <stdint.h>
#include <tau/tau.h>
TAU_MAIN()

// The number of handles each case destroys; three is enough to see a skipped or
// repeated element.
#define DM_N 3

// What the stubs saw: the value of each call, in order, and the call count.
static uintptr_t dm_seen[16];
static int dm_calls;

// Records one call of a stub, so a case can check which values the macros
// passed and in what order. Appends v to dm_seen while it fits and increments
// dm_calls.
static void dm_record(uintptr_t v) {
  if (dm_calls < (int)(sizeof(dm_seen) / sizeof(dm_seen[0])))
    dm_seen[dm_calls] = v;
  dm_calls++;
}

// Clears dm_seen and dm_calls, so each case (and each half of DM_CASE) starts
// with no recorded calls.
static void dm_reset(void) {
  memset(dm_seen, 0, sizeof(dm_seen));
  dm_calls = 0;
}

// Stubs of the destroy functions that the macros call, with the signatures of
// the library functions. They take the place of the library, so only the macros
// run; each records the handle value it got through dm_record().
void chashmap__destroy(chmap h) { dm_record((uintptr_t)h); }
void cbstmap__destroy(cbmap h) { dm_record((uintptr_t)h); }
void chttpcli__destroy(chttpcli h) { dm_record((uintptr_t)h); }
void chttpsvr__destroy(chttpsvr h) { dm_record((uintptr_t)h); }
void cjson__destroy(cjson h) { dm_record((uintptr_t)h); }
void cyaml__destroy(cyaml h) { dm_record((uintptr_t)h); }
void clrucache__destroy(ccache h) { dm_record((uintptr_t)h); }
void cmempool__destroy(cmempool *h) { dm_record((uintptr_t)h); }
void crmempool__destroy(crmempool *h) { dm_record((uintptr_t)h); }
void ccircq__destroy(ccircq *h) { dm_record((uintptr_t)h); }
void cdynmq__destroy(cdynmq *h) { dm_record((uintptr_t)h); }
void cchan__destroy(cchan *h) { dm_record((uintptr_t)h); }
void ceventloop__destroy(ceventloop h) { dm_record((uintptr_t)h); }
void cthrpool__destroy(cthrpool h) { dm_record((uintptr_t)h); }
void cvector__destroy(cvec h) { dm_record((uintptr_t)h); }
void cstring__destroy(cstr h) { dm_record((uintptr_t)h); }

// The pool argument of the last free_entry call, so a case can check that the
// pool reached the call unchanged.
static uintptr_t dm_pool_seen;
// Stubs of the two free_entry functions, in place of the library. Each stores
// its pool in dm_pool_seen and records the entry through dm_record().
void cmempool__free_entry(cmempool *mp, void *entry) {
  dm_pool_seen = (uintptr_t)mp;
  dm_record((uintptr_t)entry);
}
void crmempool__free_entry(crmempool *rmp, void *entry) {
  dm_pool_seen = (uintptr_t)rmp;
  dm_record((uintptr_t)entry);
}

// Gives a fake handle value for element i, as a uintptr_t. The cases fill their
// arrays with it. It's never 0, so a macro that skips a NULL or invalid handle
// still calls the stub, and the stubs never dereference it. Evaluates i once.
#define DM_VAL(i) ((uintptr_t)0x1000u + (uintptr_t)(i) * 0x10u)

// Runs the destroy macro F over an array of DM_N handles of type T, last
// element first, and checks the result with REQUIRE_* (it returns from the test
// on a failure). It is the body of every macro case: exactly DM_N calls in
// order with each element's value, every element cleared to CLEARED, and k at 0
// after DM_N calls. The array below dm_a holds live guard handles, so an index
// driven below 0 stays in our storage. A second part nests one F call in the
// argument of another to check that two expansions don't collide. Reads and
// resets dm_seen and dm_calls through the stubs.
#define DM_CASE(F, T, CLEARED)                                            \
  do {                                                                    \
    T dm_all[2 * DM_N];                                                   \
    for (int dm_i = 0; dm_i < 2 * DM_N; dm_i++)                           \
      dm_all[dm_i] = (T)DM_VAL(dm_i - DM_N + 16);                         \
    T *dm_a = &dm_all[DM_N];                                              \
    for (int dm_i = 0; dm_i < DM_N; dm_i++) dm_a[dm_i] = (T)DM_VAL(dm_i); \
    dm_reset();                                                           \
    int dm_k = DM_N;                                                      \
    int dm_iters = 0;                                                     \
    while (dm_k > 0 && dm_iters < DM_N + 1) {                             \
      F(dm_a[--dm_k]);                                                    \
      dm_iters++;                                                         \
    }                                                                     \
    REQUIRE_EQ(dm_iters, DM_N);                                           \
    REQUIRE_EQ(dm_k, 0);                                                  \
    REQUIRE_EQ(dm_calls, DM_N);                                           \
    for (int dm_i = 0; dm_i < DM_N; dm_i++) {                             \
      REQUIRE_EQ(dm_seen[dm_i], DM_VAL(DM_N - 1 - dm_i));                 \
      REQUIRE_TRUE(dm_a[dm_i] == (CLEARED));                              \
    }                                                                     \
    T dm_outer = (T)DM_VAL(7);                                            \
    T dm_inner = (T)DM_VAL(8);                                            \
    T *dm_outer_p[1] = {&dm_outer};                                       \
    dm_reset();                                                           \
    F(*dm_outer_p[({                                                      \
      F(dm_inner);                                                        \
      0;                                                                  \
    })]);                                                                 \
    REQUIRE_EQ(dm_calls, 2);                                              \
    REQUIRE_EQ(dm_seen[0], DM_VAL(8));                                    \
    REQUIRE_EQ(dm_seen[1], DM_VAL(7));                                    \
    REQUIRE_TRUE(dm_outer == (CLEARED));                                  \
    REQUIRE_TRUE(dm_inner == (CLEARED));                                  \
  } while (0)

TEST(destroy_macros, chmap_destroy) { DM_CASE(chmap_destroy, chmap, NULL); }
TEST(destroy_macros, cbmap_destroy) { DM_CASE(cbmap_destroy, cbmap, NULL); }
TEST(destroy_macros, cjson_destroy) { DM_CASE(cjson_destroy, cjson, NULL); }
TEST(destroy_macros, cyaml_destroy) { DM_CASE(cyaml_destroy, cyaml, NULL); }
TEST(destroy_macros, cvec_destroy) { DM_CASE(cvec_destroy, cvec, NULL); }
TEST(destroy_macros, cvector_destroy) { DM_CASE(cvector_destroy, cvec, NULL); }
TEST(destroy_macros, cstr_destroy) { DM_CASE(cstr_destroy, cstr, NULL); }
TEST(destroy_macros, cstring_destroy) { DM_CASE(cstring_destroy, cstr, NULL); }
TEST(destroy_macros, chttpcli_destroy) {
  DM_CASE(chttpcli_destroy, chttpcli, CHTTPCLI_NULL);
}
TEST(destroy_macros, chttpsvr_destroy) {
  DM_CASE(chttpsvr_destroy, chttpsvr, CHTTPSVR_NULL);
}
TEST(destroy_macros, ccache_destroy) {
  DM_CASE(ccache_destroy, ccache, CCACHE_NULL);
}
TEST(destroy_macros, cthrpool_destroy) {
  DM_CASE(cthrpool_destroy, cthrpool, CTHRPOOL_NULL);
}
TEST(destroy_macros, ceventloop_destroy) {
  DM_CASE(ceventloop_destroy, ceventloop, CEVENTLOOP_NULL);
}
TEST(destroy_macros, cmempool_destroy) {
  DM_CASE(cmempool_destroy, cmempool *, NULL);
}
TEST(destroy_macros, crmempool_destroy) {
  DM_CASE(crmempool_destroy, crmempool *, NULL);
}
TEST(destroy_macros, ccircq_destroy) {
  DM_CASE(ccircq_destroy, ccircq *, NULL);
}
TEST(destroy_macros, cdynmq_destroy) {
  DM_CASE(cdynmq_destroy, cdynmq *, NULL);
}
TEST(destroy_macros, cchan_destroy) { DM_CASE(cchan_destroy, cchan *, NULL); }

// The pools that the free_entry cases pass; a pool is an ordinary argument, so
// it must reach the call once and unchanged.
static cmempool *const dm_mp = (cmempool *)DM_VAL(9);
static crmempool *const dm_rmp = (crmempool *)DM_VAL(10);
// Single-argument wrappers of cmempool_free_entry and crmempool_free_entry, so
// DM_CASE can drive them like a destroy macro. e is the entry; the pool is the
// fixed dm_mp or dm_rmp. They expand to the call of the wrapped macro, whose
// stub records the entry and stores the pool in dm_pool_seen.
#define DM_FREE_ENTRY(e) cmempool_free_entry(dm_mp, e)
#define DM_R_FREE_ENTRY(e) crmempool_free_entry(dm_rmp, e)

TEST(destroy_macros, cmempool_free_entry) {
  DM_CASE(DM_FREE_ENTRY, void *, NULL);
  REQUIRE_EQ(dm_pool_seen, (uintptr_t)dm_mp);
}
TEST(destroy_macros, crmempool_free_entry) {
  DM_CASE(DM_R_FREE_ENTRY, void *, NULL);
  REQUIRE_EQ(dm_pool_seen, (uintptr_t)dm_rmp);
}

// The ccol__free_fn of the test iterators, which ccol_iter_destroy calls. It
// records the iterator through dm_record(), so the case can check the order.
static void dm_iter_free(cmap_iterator *it) { dm_record((uintptr_t)it); }

// DM_CASE written out for iterators, whose destroy goes through a function
// pointer of the iterator itself. The guard iterators below dm_a are live.
TEST(destroy_macros, ccol_iter_destroy) {
  cmap_iterator dm_its[DM_N + 2];
  memset(dm_its, 0, sizeof(dm_its));
  for (int i = 0; i < DM_N + 2; i++) dm_its[i].ccol__free_fn = dm_iter_free;
  cmap_iterator dm_guard_its[DM_N];
  memset(dm_guard_its, 0, sizeof(dm_guard_its));
  cmap_iterator *dm_all[2 * DM_N];
  for (int i = 0; i < DM_N; i++) {
    dm_guard_its[i].ccol__free_fn = dm_iter_free;
    dm_all[i] = &dm_guard_its[i];
  }
  cmap_iterator **dm_a = &dm_all[DM_N];
  for (int i = 0; i < DM_N; i++) dm_a[i] = &dm_its[i];
  dm_reset();
  int dm_k = DM_N;
  int dm_iters = 0;
  while (dm_k > 0 && dm_iters < DM_N + 1) {
    ccol_iter_destroy(dm_a[--dm_k]);
    dm_iters++;
  }
  REQUIRE_EQ(dm_iters, DM_N);
  REQUIRE_EQ(dm_k, 0);
  REQUIRE_EQ(dm_calls, DM_N);
  for (int i = 0; i < DM_N; i++) {
    REQUIRE_EQ(dm_seen[i], (uintptr_t)&dm_its[DM_N - 1 - i]);
    REQUIRE_EQ((void *)dm_a[i], NULL);
  }
  cmap_iterator *dm_outer = &dm_its[DM_N];
  cmap_iterator *dm_inner = &dm_its[DM_N + 1];
  cmap_iterator **dm_outer_p[1] = {&dm_outer};
  dm_reset();
  ccol_iter_destroy(*dm_outer_p[({
    ccol_iter_destroy(dm_inner);
    0;
  })]);
  REQUIRE_EQ(dm_calls, 2);
  REQUIRE_EQ((void *)dm_outer, NULL);
  REQUIRE_EQ((void *)dm_inner, NULL);
}
