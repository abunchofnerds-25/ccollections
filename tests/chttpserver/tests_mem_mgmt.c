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
#include <internal/test_helpers/chttpserver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <unistd.h>

TAU_MAIN()

/* ========================================================================== */
/*     chttpsvr_set_engine_mem_mgmt_procs COVERAGE (dedicated binary)         */
/*                                                                            */
/* You may call chttpsvr_set_engine_mem_mgmt_procs() only before the first    */
/* chttpsvr_start() in the process. The requirement is process-wide and you   */
/* may set it only once, which matches the shared reactor that the call       */
/* configures. The rest of the chttpserver test suite lives in tests.c in     */
/* this directory, and that file already calls chttpsvr_start() in its shared */
/* _setup(), so the happy path (install the procs, then start) can't be       */
/* tested there. So the build compiles this file into a binary of its own,    */
/* tests_mem_mgmt, apart from the tests binary of tests.c (see the Makefile   */
/* in this directory), the same way as tests_tls.c. This file can then        */
/* install counting procs and start the shared engine exactly once, before    */
/* anything else in the process can.                                          */
/* ========================================================================== */

#define TEST_PORT 18795
#define BASE_URL "http://127.0.0.1:18795"

static clog g_test_logger = CLOG_INVALID;
static chttpsvr g_srv = CHTTPSVR_INVALID;

static size_t g_mm_malloc_count = 0;
static size_t g_mm_free_count = 0;
static size_t g_mm_calloc_count = 0;
static size_t g_mm_realloc_count = 0;

/* Counting wrappers around the C library. They show that the library really
 * sends the shared reactor's internal allocations through the procs this file
 * configures, and since they don't change how the allocator behaves, the engine
 * keeps working normally while we count. */
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
    g_srv = CHTTPSVR_INVALID;
  }
  /* __chttpsvr_destroy releases this server's shared-engine reference, but it
   * hands the teardown of the shared ccol_event_loop reactor to a joinable
   * reaper thread instead of joining that thread inline. chttpsvr_engine_wait()
   * blocks until something joins that reaper thread, and this file needs that
   * so no reactor thread is still running when this atexit handler returns.
   * Without it, valgrind's leak check can race the exit of the process against
   * the reactor teardown. tests_tls.c in this directory relies on the same
   * reasoning. */
  /* On OpenBSD this exit handler runs while the dynamic loader holds a lock
   * that joining the engine's reaper needs, so the wait would never return
   * there (see _teardown in tests.c). The reactor may then still be running
   * when the process ends, so the logger stays open too. */
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_INVALID;
  }
#endif
}

/* The reactor thread count of the engine that _setup() started, before any test
 * runs; see num_reactor_threads_defaults_to_one_when_unconfigured. */
static size_t g_initial_reactor_threads;

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  /* Must be called before the first chttpsvr_start() of the process. */
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

  g_test_logger = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  g_srv = chttpsvr_create(g_test_logger, &err);
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

  /* Push one real request through the engine so that its internal allocations
   * (the ccol_event_loop registration table, the dispatch state of each
   * connection and so on) have really happened before any TEST() body reads the
   * counters. */
  chttpcli cli = chttpclient_create(NULL);
  if (cli) {
    chttp_request_t *req =
        chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      chttpclient_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) chttpclient_resp_free(resp);
    }
    chttpclient_destroy(cli);
  }

  /* We register the handler after chttpsvr_start purely out of habit. Nothing
     in this codebase registers an atexit handler of its own that this one would
     have to run before or after, but there's no reason to break the setup and
     teardown symmetry that the other test suites of this codebase follow. */
  atexit(_teardown);
  g_initial_reactor_threads = _chttpsvr_engine_num_reactor_threads_for_tests();
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_mem_mgmt, procs_wired_into_engine_allocations) {
  /* _setup() installs counting procs before the first chttpsvr_start() and then
   * pushes a real request through the running engine. The library sends the
   * internal allocations of the shared ccol_event_loop reactor (the registry's
   * chmaps, the dispatch state of each connection and so on) through the procs
   * this file configured, so the malloc, calloc and free counts are all
   * above zero by the time this test body runs.
   *
   * We deliberately assert nothing about realloc, because ccol_event_loop has
   * no _ccol_mem_realloc call site at all. Its fd registry picks open
   * addressing, since the key type and the value type are both integral (see
   * should_use_open_addressing in chashmap.c), and the growth path of open
   * addressing is oa_rebuild, which allocates a fresh, larger slot array with
   * calloc and frees the old one instead of reallocating in place. mp->realloc
   * is still a hard requirement (see null_function_pointer_rejected below),
   * because it keeps the interface complete and covers a future change that
   * adds a real realloc call site. But there's no way to drive it through this
   * engine's allocations, so on purpose there's no
   * procs_wired_into_engine_reallocations test, which could only be built
   * around an artificial trigger. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             (size_t)0);

  /* g_mm_free_count has a single source: the server notices that _setup()'s
   * client connection went away (chttpclient_destroy closes it) and frees its
   * own chttpsvr_conn_t. The server's reactor must observe that event and
   * dispatch it asynchronously first, and nothing guarantees that this has
   * happened the instant _setup()'s constructor returns. So we retry with a
   * bound instead of checking once. The dispatch goes through the
   * hand-off from the cthreadcomm poller to a ctpool worker, whose latency is
   * small but real, enough to make a bare immediate assertion here measurably
   * flaky. 150 attempts of 20ms make 3s in total, the same headroom other waits
   * of this codebase use where dispatch latency matters (the bounds of
   * async_expect_continue in chttpclient are one example). It stays reliable
   * under make memtest and on a loaded CI runner, not only on a native run. */
  for (int attempt = 0;
       __atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED) == 0 &&
       attempt < 150;
       attempt++) {
    usleep(20000);
  }
  REQUIRE_GT(__atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED), (size_t)0);
}

TEST(chttpserver_mem_mgmt,
     num_reactor_threads_defaults_to_one_when_unconfigured) {
  /* _setup() never called chttpsvr_set_engine_num_reactor_threads before
   * starting g_srv, so the reactor has to have used the default of 1, a single
   * dedicated thread, and not a CPU count detected by the library. That's the
   * documented default of chttpsvr_set_engine_num_reactor_threads, which a
   * benchmark picked. */
  /* The value comes from _setup(), before any test runs, because a later test
   * of this file sets 3 and the order the tests run in depends on the compiler.
   */
  REQUIRE_EQ(g_initial_reactor_threads, (size_t)1);
}

TEST(chttpserver_mem_mgmt, null_function_pointer_rejected) {
  /* The library needs to reject one missing function pointer whatever state the
   * engine is in, because it validates the pointers unconditionally, before the
   * "the engine already runs" check. */
  ccol_memmgmt_procs_t bad_procs = {
      .malloc = _counting_malloc,
      .free = NULL,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&bad_procs), ccol_invalid_args);
}

TEST(chttpserver_mem_mgmt, rejected_after_engine_already_running) {
  /* _setup() has already started the shared engine, so a second call is
   * rejected even when it's fully valid. Swapping the allocators after the
   * library has already allocated memory with the previous set would give
   * malloc and free pairs that don't match. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, null_procs_reverts_rejected_while_running) {
  /* A NULL value reverts to the default. The "not while the engine runs" rule
   * covers it too, because it is a live swap of the allocator just the same. */
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(NULL), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, num_reactor_threads_rejected_while_running) {
  /* The same restriction as for chttpsvr_set_engine_mem_mgmt_procs above, since
   * the library bakes the value into the reactor when it builds it. It also
   * covers the 0 sentinel, which reverts to the default. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(2), ccol_not_permitted);
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(0), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, reinstall_after_full_stop_then_restart_succeeds) {
  /* The doc comment of chttpsvr_set_engine_mem_mgmt_procs says you "may call it
   * again after the engine fully stops, which is after chttpsvr_engine_wait()
   * returns, and before the next chttpsvr_start()". This is the only test in
   * the file that covers that path, the others cover only an install before a
   * start (which _setup() does implicitly) and a rejection while the engine
   * runs. Make the "the engine already runs" check sticky, for
   * example with a latch that nothing clears on a stop, and this test fails
   * while no other test in the file does.
   *
   * This needs to be the last test in the file. It tears g_srv down completely
   * (g_srv is the only thing keeping the shared engine's refcount above zero in
   * this process), blocks until the shared engine has really stopped, installs
   * the procs again and starts a fresh server on the same port. Without that
   * fresh server every test declared after this one would run with no server
   * listening. The test points g_srv at the new server so that _teardown()
   * cleans up normally when the process exits. */
  chttpsvr_stop(g_srv);
  __chttpsvr_destroy(g_srv);
  g_srv = CHTTPSVR_INVALID;
  chttpsvr_engine_wait();

  /* The engine start below has to really use the procs this test installs
   * again, accepting them isn't enough. So we take a snapshot of the counters
   * first, and the check after the restart can then demand real forward
   * progress. Without the snapshot that check only says "the counters are still
   * above zero from before", and it passes even when the reinstall silently did
   * nothing. */
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

  /* This also drives the reinstall path of
   * chttpsvr_set_engine_num_reactor_threads in the same restart cycle (the only
   * one this file makes), since that path runs after a full stop and before the
   * next chttpsvr_start. An explicit value is exactly the value the
   * library wires into the reactor it creates again below. It isn't enough that
   * the library accepts the value and then silently ignores it. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(3), ccol_success);

  char *err = NULL;
  /* _ccol_destructor is a safety net for a REQUIRE_* failure between here and
     the hand-off of ownership to g_srv below. A Tau REQUIRE_* returns from the
     function at once on a failure, and without this destructor that return
     would leak this server's shared-engine reference, which then hangs the
     chttpsvr_engine_wait() call in this file's _teardown() when the process
     exits. The variable is set to CHTTPSVR_INVALID right after ownership goes
     to g_srv, so the destructor never destroys the handle that g_srv owns from
     then on a second time. */
  chttpsvr new_srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(new_srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(new_srv, CHTTP_GET, "/hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  REQUIRE_EQ((int)chttpsvr_start(new_srv, &cfg), (int)ccol_success);
  g_srv = new_srv;
  new_srv = CHTTPSVR_INVALID; /* ownership went to g_srv, see above */

  /* The shared ccol_event_loop reactor is a plain static variable.
   * ccol_event_loop_destroy destroys it completely when the code above releases
   * the last reference, and chttpsvr_start below builds it again from scratch
   * with ccol_event_loop_create_mp. No pool recycles an allocation across a
   * restart, so a fresh malloc or calloc call through the procs this test just
   * installed again is guaranteed, not just likely. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             malloc_calloc_before);
  REQUIRE_EQ(_chttpsvr_engine_num_reactor_threads_for_tests(), (size_t)3);

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_INVALID);
  chttp_request_t *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);
  chttpcli_response *resp = NULL;
  rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* Each check below on the contents of resp is captured into a local first,
     and resp is then freed no matter what, right after and before any
     REQUIRE_*, which could otherwise return early and leak it. Unlike cli
     above, resp has no scoped destructor of its own. */
  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches =
      body_present && strcmp(resp->body, "Hello, mem-mgmt!") == 0;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}
