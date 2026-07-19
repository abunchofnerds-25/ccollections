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

#include <chttpclient.h>
#include <chttpserver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*     chttpsvr_set_engine_mem_mgmt_procs COVERAGE (dedicated binary)         */
/*                                                                            */
/* chttpsvr_set_engine_mem_mgmt_procs() may only be called before the first  */
/* chttpsvr_start() in the process; a process-wide, set-once-ever            */
/* requirement, matching the shared reactor it configures. The rest of the   */
/* chttpserver test suite (tests.c, sharing this same directory)             */
/* already calls chttpsvr_start() during its own shared _setup(), so testing */
/* the "install procs, then start" happy path there is impossible. This      */
/* file is therefore compiled into its own binary, tests_mem_mgmt, separate  */
/* from tests.c's tests binary (see the Makefile in this same directory;     */
/* same reasoning as tests_tls.c) so it can install counting procs and       */
/* start the shared engine exactly once, before anything else in the        */
/* process has a chance to.                                                  */
/* ========================================================================== */

#define TEST_PORT 18795
#define BASE_URL "http://127.0.0.1:18795"

static clog g_test_logger = NULL;
static chttpsvr g_srv = NULL;

static size_t g_mm_malloc_count = 0;
static size_t g_mm_free_count = 0;
static size_t g_mm_calloc_count = 0;
static size_t g_mm_realloc_count = 0;

/* Counting wrappers around libc: prove that the shared reactor's internal
 * allocations are actually routed through the configured procs, without
 * changing allocator behavior (so the engine keeps working normally while we
 * count). */
static void *_counting_malloc(size_t size) {
  __atomic_fetch_add(&g_mm_malloc_count, 1, __ATOMIC_RELAXED);
  return malloc(size);
}
static void _counting_free(void *ptr) {
  if (ptr) __atomic_fetch_add(&g_mm_free_count, 1, __ATOMIC_RELAXED);
  free(ptr);
}
static void *_counting_calloc(size_t count, size_t size) {
  __atomic_fetch_add(&g_mm_calloc_count, 1, __ATOMIC_RELAXED);
  return calloc(count, size);
}
static void *_counting_realloc(void *ptr, size_t size) {
  __atomic_fetch_add(&g_mm_realloc_count, 1, __ATOMIC_RELAXED);
  return realloc(ptr, size);
}

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, mem-mgmt!");
}

static void _teardown(void) {
  if (g_srv) chttpsvr_stop(g_srv);
  if (g_srv) {
    __chttpsvr_destroy(g_srv);
    g_srv = NULL;
  }
  /* __chttpsvr_destroy releases this server's shared-engine reference but
   * hands teardown of the shared event_loop reactor off to a joinable
   * reaper thread rather than joining it inline; chttpsvr_engine_wait()
   * blocks until that reaper thread has actually joined, which is required
   * here so no reactor thread is still running when this atexit handler
   * returns (otherwise valgrind's leak check can race process exit against
   * the reactor's own teardown). See tests/chttpserver_tls/tests.c for the
   * same reasoning. */
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = NULL;
  }
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  /* Must be called before the first chttpsvr_start() in the process. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  ccol_retval_t rv = chttpsvr_set_engine_mem_mgmt_procs(&procs);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_set_engine_mem_mgmt_procs failed: %d\n",
            rv);
    exit(1);
  }

  g_test_logger = clog_open_fd(2, CLOG_INFO);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  g_srv = create_chttpsvr(g_test_logger, &err);
  if (!g_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n",
            err ? err : "(unknown)");
    exit(1);
  }
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/hello", _hello_handler, NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;

  rv = chttpsvr_start(g_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }

  /* Drive one real request through the engine so its internal allocations
   * (the event_loop registration table, per-connection dispatch state, ...)
   * actually happen before any TEST() body inspects the counters. */
  chttpcli cli = create_chttpclient(NULL);
  if (cli) {
    chttp_request_t *req =
        chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      chttpclient_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) chttpclient_resp_free(resp);
    }
    chttpclient_destroy(cli);
  }

  /* Registered after chttpsvr_start purely as a defensive habit; nothing in
     this codebase registers its own atexit handler that this one would need
     to run before or after, but there is no reason to disturb working
     setup/teardown symmetry with the rest of this codebase's test suites. */
  atexit(_teardown);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_mem_mgmt, procs_wired_into_engine_allocations) {
  /* _setup() installed counting procs before the first chttpsvr_start() and
   * then drove a real request through the running engine. If the shared
   * event_loop reactor's own internal allocations (the registry chmaps, the
   * per-connection dispatch state, and so on) were actually redirected to
   * the configured procs, malloc/calloc/free must all be nonzero by now.
   *
   * realloc is deliberately not asserted on here: traced through
   * cthreadcomm.c directly, event_loop has no _mem_realloc call site at
   * all. Its own fd registry chooses open addressing (both key and value
   * types are integral; see chashmap.c's should_use_open_addressing),
   * and open addressing's own growth path (oa_rehash) allocates a fresh,
   * larger slot array via calloc and frees the old one, rather than
   * reallocating in place. mp->realloc is still a hard requirement (see
   * null_function_pointer_rejected below) for interface completeness and
   * in case a future change introduces a genuine realloc call site, but
   * there is currently no way to *exercise* it through this engine's own
   * allocations, so a dedicated procs_wired_into_engine_reallocations test
   * (which used to exist here, targeting FIOBJ hash growth specifically)
   * was removed rather than built around an artificial trigger. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             (size_t)0);

  /* g_mm_free_count's own source is the server noticing _setup()'s client
   * connection has gone away (chttpclient_destroy closes it) and freeing its
   * own chttpsvr_conn_t in response; an event the server's reactor must
   * still observe and dispatch asynchronously, not something guaranteed to
   * have already happened the instant _setup()'s constructor returns.
   * Bounded retry rather than an immediate single check: this dispatch now
   * goes through the poller-to-ctpool-worker handoff described in
   * cthreadcomm's own history (a real, if small and bounded, added latency
   * versus the single-thread design's near-synchronous inline dispatch),
   * which made a bare immediate assertion here measurably flaky where it
   * previously was not. */
  for (int attempt = 0;
       __atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED) == 0 && attempt < 50;
       attempt++) {
    usleep(20000);
  }
  REQUIRE_GT(__atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED), (size_t)0);
}

extern size_t _chttpsvr_engine_num_reactor_threads_for_tests(void);

TEST(chttpserver_mem_mgmt,
     num_reactor_threads_defaults_to_one_when_unconfigured) {
  /* _setup() never called chttpsvr_set_engine_num_reactor_threads before
   * starting g_srv, so the reactor must have used the default of 1 (a
   * single dedicated thread, not an auto-detected CPU count), matching
   * chttpsvr_set_engine_num_reactor_threads's own documented, benchmarked
   * default. */
  REQUIRE_EQ(_chttpsvr_engine_num_reactor_threads_for_tests(), (size_t)1);
}

TEST(chttpserver_mem_mgmt, null_function_pointer_rejected) {
  /* One missing function pointer must be rejected regardless of engine
   * state; validated unconditionally before the "already running" check. */
  ccol_memmgmt_procs_t bad_procs = {
      .malloc = _counting_malloc,
      .free = NULL,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&bad_procs), ccol_invalid_args);
}

TEST(chttpserver_mem_mgmt, rejected_after_engine_already_running) {
  /* _setup() already started the shared engine, so a second, fully valid
   * call must be rejected: swapping allocators after memory has already been
   * allocated with the previous set would produce mismatched malloc/free
   * pairs. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, null_procs_reverts_rejected_while_running) {
  /* NULL (revert-to-default) is also subject to the "not while running"
   * rule; it is still a live allocator swap. */
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(NULL), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, num_reactor_threads_rejected_while_running) {
  /* Same "baked into the reactor at construction time" restriction as
   * chttpsvr_set_engine_mem_mgmt_procs above, including the 0
   * (revert-to-default) sentinel. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(2), ccol_not_permitted);
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(0), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, reinstall_after_full_stop_then_restart_succeeds) {
  /* chttpsvr_set_engine_mem_mgmt_procs's own doc comment states it "may be
   * called again after the engine has fully stopped (chttpsvr_engine_wait()
   * has returned), before the next chttpsvr_start()" - but no test anywhere
   * in this suite exercised that path; the other three tests above only
   * cover install-before-start (implicitly, via _setup()) and
   * reject-while-running. A regression that made the "engine already
   * running" check sticky (e.g. a latch never cleared on stop) would pass
   * every other test in this file untouched.
   *
   * This must be the last test in the file: it fully tears down g_srv (the
   * only thing keeping the shared engine's refcount above zero in this
   * process) and blocks until the shared engine has genuinely stopped, then
   * reinstalls procs and starts a fresh server on the same port; every test
   * declared after this one would otherwise run with no server listening.
   * g_srv is repointed at the new server so _teardown() still cleans up
   * normally at process exit. */
  chttpsvr_stop(g_srv);
  __chttpsvr_destroy(g_srv);
  g_srv = NULL;
  chttpsvr_engine_wait();

  /* Reinstalled procs must genuinely be exercised, not merely accepted, by
   * the next engine start below; snapshot the counters first so the check
   * after the restart can require real forward progress rather than just
   * "still nonzero from before" (which would pass even if the reinstall
   * silently did nothing). */
  size_t malloc_calloc_before =
      __atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
      __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED);

  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_success);

  /* Exercise chttpsvr_set_engine_num_reactor_threads's own "after a full
   * stop, before the next chttpsvr_start" reinstall path in the same
   * restart cycle, since this is the only one this file performs; a
   * dedicated explicit value must be the exact one wired into the freshly
   * (re)created reactor below, not merely accepted and then silently
   * ignored. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(3), ccol_success);

  char *err = NULL;
  chttpsvr new_srv = create_chttpsvr(g_test_logger, &err);
  REQUIRE_TRUE(new_srv != NULL);
  ccol_retval_t rv = chttpsvr_register_handler(new_srv, CHTTP_GET, "/hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  REQUIRE_EQ((int)chttpsvr_start(new_srv, &cfg), (int)ccol_success);
  g_srv = new_srv;

  /* The shared event_loop reactor is a plain static variable, fully
   * destroyed (event_loop_destroy) when the last reference is released above
   * and fully reconstructed from scratch (event_loop_create_with_mprocs) by
   * chttpsvr_start below; there is no pool to recycle allocations from
   * across a restart, so a fresh malloc/calloc call through the
   * just-reinstalled procs is guaranteed, not merely likely. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             malloc_calloc_before);
  REQUIRE_EQ(_chttpsvr_engine_num_reactor_threads_for_tests(), (size_t)3);

  chttpcli cli = create_chttpclient(NULL);
  REQUIRE_TRUE(cli != NULL);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);
  chttpcli_response *resp = NULL;
  rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Hello, mem-mgmt!");
  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
}
