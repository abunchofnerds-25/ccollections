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

/* Every public destroy, free and release macro evaluates its handle argument
 * exactly once, calls the destroy function once with the value that the
 * argument held, and clears the argument. This binary checks the macros
 * alone: it defines the destroy functions that the macros call as recording
 * stubs, so it links none of the library and runs no module code.
 *
 * Each case walks an array backwards with `while (k) F(a[--k])`. A macro that
 * evaluates its argument twice decrements k twice per call, so it destroys
 * the wrong elements and skips others. The array sits at the end of a larger
 * one, so the index that such a macro reaches below 0 still names storage of
 * the test and the failure is an assertion, not undefined behaviour.
 *
 * The binary is built with -Wshadow, and each case also nests one call inside
 * the argument of another, so a temporary of a destroy macro that hides
 * another one fails the build. */

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

/* Recording stubs with the signatures of the library functions. */
void __chmap_destroy(chmap h) { dm_record((uintptr_t)h); }
void __cbmap_destroy(cbmap h) { dm_record((uintptr_t)h); }
void __chttpclient_destroy(chttpcli h) { dm_record((uintptr_t)h); }
void __chttpsvr_destroy(chttpsvr h) { dm_record((uintptr_t)h); }
void __cjson_destroy(cjson h) { dm_record((uintptr_t)h); }
void __cyaml_destroy(cyaml h) { dm_record((uintptr_t)h); }
void __clrucache_destroy(clru_cache h) { dm_record((uintptr_t)h); }
void _ccol_mempool_destroy(ccol_mempool *h) { dm_record((uintptr_t)h); }
void _ccol_r_mempool_destroy(ccol_r_mempool *h) { dm_record((uintptr_t)h); }
void __ccol_circular_queue_destroy(ccol_circular_queue *h) {
  dm_record((uintptr_t)h);
}
void __ccol_dynamic_queue_destroy(ccol_dynamic_queue *h) {
  dm_record((uintptr_t)h);
}
void __ccol_channel_destroy(ccol_channel *h) { dm_record((uintptr_t)h); }
void __ccol_event_loop_destroy(ccol_event_loop h) { dm_record((uintptr_t)h); }
void __ctpool_destroy(ctpool h) { dm_record((uintptr_t)h); }
void __cvector_destroy(cvec h) { dm_record((uintptr_t)h); }
void __cstring_destroy(cstr h) { dm_record((uintptr_t)h); }

/* The pool argument of a free_entry macro is recorded as well, so a case can
 * check that the pool reached the call unchanged. */
static uintptr_t dm_pool_seen;
void _ccol_mempool_free_entry(ccol_mempool *mp, void *entry) {
  dm_pool_seen = (uintptr_t)mp;
  dm_record((uintptr_t)entry);
}
void _ccol_r_mempool_free_entry(ccol_r_mempool *rmp, void *entry) {
  dm_pool_seen = (uintptr_t)rmp;
  dm_record((uintptr_t)entry);
}

/* A fake handle value for element i: never 0, so a macro that skips a NULL
 * or invalid handle still calls the stub. The stubs never dereference it. */
#define DM_VAL(i) ((uintptr_t)0x1000u + (uintptr_t)(i) * 0x10u)

/* Runs F over an array of three handles of type T, backwards, then checks
 * that exactly three calls happened, in order, with the value of each
 * element, that every element is cleared to CLEARED, and that k reached 0
 * after exactly three calls. A nested call inside the argument checks that
 * two expansions of F do not collide. */
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
TEST(destroy_macros, chttpclient_destroy) {
  DM_CASE(chttpclient_destroy, chttpcli, CHTTPCLI_INVALID);
}
TEST(destroy_macros, chttpsvr_destroy) {
  DM_CASE(chttpsvr_destroy, chttpsvr, CHTTPSVR_INVALID);
}
TEST(destroy_macros, clru_destroy) {
  DM_CASE(clru_destroy, clru_cache, CLRU_CACHE_INVALID);
}
TEST(destroy_macros, ctpool_destroy) {
  DM_CASE(ctpool_destroy, ctpool, CTPOOL_INVALID);
}
TEST(destroy_macros, ccol_event_loop_destroy) {
  DM_CASE(ccol_event_loop_destroy, ccol_event_loop, CCOL_EVENT_LOOP_INVALID);
}
TEST(destroy_macros, ccol_mempool_destroy) {
  DM_CASE(ccol_mempool_destroy, ccol_mempool *, NULL);
}
TEST(destroy_macros, ccol_r_mempool_destroy) {
  DM_CASE(ccol_r_mempool_destroy, ccol_r_mempool *, NULL);
}
TEST(destroy_macros, ccol_circular_queue_destroy) {
  DM_CASE(ccol_circular_queue_destroy, ccol_circular_queue *, NULL);
}
TEST(destroy_macros, ccol_dynamic_queue_destroy) {
  DM_CASE(ccol_dynamic_queue_destroy, ccol_dynamic_queue *, NULL);
}
TEST(destroy_macros, ccol_channel_destroy) {
  DM_CASE(ccol_channel_destroy, ccol_channel *, NULL);
}

/* The pool of a free_entry call is an ordinary argument: it reaches the
 * call once, unchanged. */
static ccol_mempool *const dm_mp = (ccol_mempool *)DM_VAL(9);
static ccol_r_mempool *const dm_rmp = (ccol_r_mempool *)DM_VAL(10);
#define DM_FREE_ENTRY(e) ccol_mempool_free_entry(dm_mp, e)
#define DM_R_FREE_ENTRY(e) ccol_r_mempool_free_entry(dm_rmp, e)

TEST(destroy_macros, ccol_mempool_free_entry) {
  DM_CASE(DM_FREE_ENTRY, void *, NULL);
  REQUIRE_EQ(dm_pool_seen, (uintptr_t)dm_mp);
}
TEST(destroy_macros, ccol_r_mempool_free_entry) {
  DM_CASE(DM_R_FREE_ENTRY, void *, NULL);
  REQUIRE_EQ(dm_pool_seen, (uintptr_t)dm_rmp);
}

/* ccol_iter_destroy calls the _free_fn of the iterator itself. */
static void dm_iter_free(cmap_iterator *it) { dm_record((uintptr_t)it); }

TEST(destroy_macros, ccol_iter_destroy) {
  cmap_iterator dm_its[DM_N + 2];
  memset(dm_its, 0, sizeof(dm_its));
  for (int i = 0; i < DM_N + 2; i++) dm_its[i]._free_fn = dm_iter_free;
  /* The guard iterators below dm_a are live too, as in DM_CASE. */
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
