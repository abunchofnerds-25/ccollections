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

/* Every public destroy, free and release macro has to evaluate its handle
 * argument exactly once, call the destroy function once with the value the
 * argument held, and then clear the argument. This binary checks only the
 * macros. The destroy functions they call are defined here as stubs that
 * record their calls, so none of the library is linked and no module code
 * runs.
 *
 * Each case walks an array backwards with `while (k) F(a[--k])`. A macro
 * that evaluates its argument twice decrements k twice per call, so it
 * destroys the wrong elements and skips others. The array sits at the end of
 * a larger one, which means an index such a macro drives below 0 still
 * points at the test's own storage, and we get an assertion failure instead
 * of undefined behaviour.
 *
 * The binary is built with -Wshadow and every case also nests one call in
 * the argument of another, so if a destroy macro's temporary hides another
 * one the build fails. */

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
TAU_MAIN()  // sets Tau up, and gives the main function

#define DM_N 3

/* What the stubs saw: the value of each call, in order. */
static uintptr_t dm_seen[16];
static int dm_calls;

static void dm_record(uintptr_t v) {
  if (dm_calls < (int)(sizeof(dm_seen) / sizeof(dm_seen[0])))
    dm_seen[dm_calls] = v;
  dm_calls++;
}

static void dm_reset(void) {
  memset(dm_seen, 0, sizeof(dm_seen));
  dm_calls = 0;
}

/* Stubs that record their calls, with the library functions' signatures. */
void __chashmap_destroy(chmap h) { dm_record((uintptr_t)h); }
void __cbstmap_destroy(cbmap h) { dm_record((uintptr_t)h); }
void __chttpcli_destroy(chttpcli h) { dm_record((uintptr_t)h); }
void __chttpsvr_destroy(chttpsvr h) { dm_record((uintptr_t)h); }
void __cjson_destroy(cjson h) { dm_record((uintptr_t)h); }
void __cyaml_destroy(cyaml h) { dm_record((uintptr_t)h); }
void __clrucache_destroy(ccache h) { dm_record((uintptr_t)h); }
void _cmempool_destroy(cmempool *h) { dm_record((uintptr_t)h); }
void _crmempool_destroy(crmempool *h) { dm_record((uintptr_t)h); }
void __ccircq_destroy(ccircq *h) { dm_record((uintptr_t)h); }
void __cdynmq_destroy(cdynmq *h) { dm_record((uintptr_t)h); }
void __cchan_destroy(cchan *h) { dm_record((uintptr_t)h); }
void __ceventloop_destroy(ceventloop h) { dm_record((uintptr_t)h); }
void __cthrpool_destroy(cthrpool h) { dm_record((uintptr_t)h); }
void __cvector_destroy(cvec h) { dm_record((uintptr_t)h); }
void __cstring_destroy(cstr h) { dm_record((uintptr_t)h); }

/* We record the pool argument of a free_entry macro too, so a case can
 * check that the pool reached the call unchanged. */
static uintptr_t dm_pool_seen;
void _cmempool_free_entry(cmempool *mp, void *entry) {
  dm_pool_seen = (uintptr_t)mp;
  dm_record((uintptr_t)entry);
}
void _crmempool_free_entry(crmempool *rmp, void *entry) {
  dm_pool_seen = (uintptr_t)rmp;
  dm_record((uintptr_t)entry);
}

/* A fake handle value for element i. It's never 0, so a macro that skips a
 * NULL or invalid handle calls the stub anyway, and the stubs never
 * dereference it. */
#define DM_VAL(i) ((uintptr_t)0x1000u + (uintptr_t)(i) * 0x10u)

/* Runs F backwards over an array of three handles of type T, then checks
 * that exactly three calls happened in order with each element's value, that
 * every element was cleared to CLEARED and that k got to 0 after exactly
 * three calls. A call nested in the argument checks that two expansions of F
 * don't collide. */
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

/* The pool of a free_entry call is an ordinary argument, so it reaches the
 * call once and unchanged. */
static cmempool *const dm_mp = (cmempool *)DM_VAL(9);
static crmempool *const dm_rmp = (crmempool *)DM_VAL(10);
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

/* ccol_iter_destroy calls the _free_fn of the iterator itself. */
static void dm_iter_free(cmap_iterator *it) { dm_record((uintptr_t)it); }

TEST(destroy_macros, ccol_iter_destroy) {
  cmap_iterator dm_its[DM_N + 2];
  memset(dm_its, 0, sizeof(dm_its));
  for (int i = 0; i < DM_N + 2; i++) dm_its[i]._free_fn = dm_iter_free;
  /* As in DM_CASE, the guard iterators below dm_a are live too. */
  cmap_iterator dm_guard_its[DM_N];
  memset(dm_guard_its, 0, sizeof(dm_guard_its));
  cmap_iterator *dm_all[2 * DM_N];
  for (int i = 0; i < DM_N; i++) {
    dm_guard_its[i]._free_fn = dm_iter_free;
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
