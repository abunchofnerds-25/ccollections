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
/* You may call chttpsvr_set_engine_mem_mgmt_procs() only before the first  */
/* chttpsvr_start() in the process. That requirement covers the whole       */
/* process, and you may set it only one time ever. It matches the shared    */
/* reactor that the call configures. The rest of the chttpserver test suite */
/* lives in tests.c, in this same directory. That file already calls        */
/* chttpsvr_start() during its own shared _setup(). A test of the happy     */
/* path, where you install the procs and then start, is therefore           */
/* impossible there. The build compiles this file into a binary of its own, */
/* tests_mem_mgmt, separate from the tests binary of tests.c. See the       */
/* Makefile in this same directory. tests_tls.c has the same reasoning.     */
/* This file can therefore install counting procs and start the shared      */
/* engine exactly one time, before anything else in the process can.        */
/* ========================================================================== */

#define TEST_PORT 18795
#define BASE_URL "http://127.0.0.1:18795"

static clog g_test_logger = CLOG_INVALID;
static chttpsvr g_srv = CHTTPSVR_INVALID;

static size_t g_mm_malloc_count = 0;
static size_t g_mm_free_count = 0;
static size_t g_mm_calloc_count = 0;
static size_t g_mm_realloc_count = 0;

/* These are counting wrappers around the C library. They prove that the
 * library really routes the internal allocations of the shared reactor
 * through the procs that this file configures. They do not change how the
 * allocator behaves, so the engine keeps working normally while the count
 * runs. */
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
  /* __chttpsvr_destroy releases the shared-engine reference of this server.
   * But it gives the teardown of the shared ccol_event_loop reactor to a
   * joinable reaper thread, and it does not join that thread inline.
   * chttpsvr_engine_wait() blocks until something joins that reaper thread.
   * This file needs that, so that no reactor thread still runs when this
   * atexit handler returns. Without it, the leak check of valgrind can race
   * the exit of the process against the teardown of the reactor.
   * tests_tls.c in this same directory relies on the same reasoning. */
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_INVALID;
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

  g_test_logger = clog_open_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  g_srv = ccol_create_chttpsvr(g_test_logger, &err);
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

  /* Drive one real request through the engine. Its internal allocations
   * then really happen before any TEST() body reads the counters. Those
   * allocations cover the registration table of the ccol_event_loop, the
   * dispatch state of each connection, and more. */
  chttpcli cli = ccol_create_chttpclient(NULL);
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

  /* This code registers the handler after chttpsvr_start, purely as a
     defensive habit. Nothing in this codebase registers an atexit handler
     of its own that this one would have to run before or after. But there
     is no reason to break the symmetry of setup and teardown that works
     across the other test suites of this codebase. */
  atexit(_teardown);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_mem_mgmt, procs_wired_into_engine_allocations) {
  /* _setup() installs counting procs before the first chttpsvr_start(). It
   * then drives a real request through the engine while it runs. The
   * library routes the internal allocations of the shared ccol_event_loop
   * reactor through the procs that this file configured. Those allocations
   * cover the chmaps of the registry, the dispatch state of each
   * connection, and more. The counts for malloc, calloc and free must
   * therefore all be above zero by the time that this test body runs.
   *
   * This code deliberately asserts nothing about realloc. ccol_event_loop
   * has no _ccol_mem_realloc call site at all. Its own fd registry chooses
   * open addressing, because both the key type and the value type are
   * integral; see should_use_open_addressing in chashmap.c. The growth path
   * of open addressing is oa_rehash. It allocates a fresh, larger slot
   * array with calloc and frees the old one. It does not reallocate in
   * place. mp->realloc is still a hard requirement; see
   * null_function_pointer_rejected below. It keeps the interface complete,
   * and it covers a future change that adds a real realloc call site. But
   * there is no way to drive it through the allocations of this engine.
   * There is therefore deliberately no
   * procs_wired_into_engine_reallocations test. Such a test could only be
   * built around an artificial trigger. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             (size_t)0);

  /* g_mm_free_count comes from one source. The server notices that the
   * client connection of _setup() went away, because chttpclient_destroy
   * closes it. The server then frees its own chttpsvr_conn_t. The reactor
   * of the server must still observe that event and dispatch it
   * asynchronously. Nothing guarantees that it already happened the instant
   * that the constructor of _setup() returns. This code therefore retries
   * with a bound, and does not make one immediate check. The dispatch goes
   * through the hand-off from the poller of cthreadcomm to a ctpool worker.
   * The latency of that hand-off is small but real, and it makes a bare
   * immediate assertion here measurably flaky. 150 attempts of 20ms give 3s
   * in total. That matches the headroom that the other waits of this
   * codebase use where dispatch latency matters. The bounds of
   * async_expect_continue in chttpclient are one example. It stays reliable
   * under make memtest, and on a loaded CI runner, and not only on a native
   * run. */
  for (int attempt = 0;
       __atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED) == 0 &&
       attempt < 150;
       attempt++) {
    usleep(20000);
  }
  REQUIRE_GT(__atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED), (size_t)0);
}

extern size_t _chttpsvr_engine_num_reactor_threads_for_tests(void);

TEST(chttpserver_mem_mgmt,
     num_reactor_threads_defaults_to_one_when_unconfigured) {
  /* _setup() never called chttpsvr_set_engine_num_reactor_threads before it
   * started g_srv. The reactor must therefore have used the default of 1.
   * That is a single dedicated thread, and not a CPU count that the library
   * detects. It matches the documented default of
   * chttpsvr_set_engine_num_reactor_threads, which a benchmark chose. */
  REQUIRE_EQ(_chttpsvr_engine_num_reactor_threads_for_tests(), (size_t)1);
}

TEST(chttpserver_mem_mgmt, null_function_pointer_rejected) {
  /* The library must reject one missing function pointer, whatever the
   * state of the engine is. It validates the pointers unconditionally,
   * before the "the engine already runs" check. */
  ccol_memmgmt_procs_t bad_procs = {
      .malloc = _counting_malloc,
      .free = NULL,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&bad_procs), ccol_invalid_args);
}

TEST(chttpserver_mem_mgmt, rejected_after_engine_already_running) {
  /* _setup() already started the shared engine. A second call must
   * therefore be rejected, even one that is fully valid. To swap the
   * allocators after the library already allocated memory with the previous
   * set would produce malloc and free pairs that do not match. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, null_procs_reverts_rejected_while_running) {
  /* A NULL value reverts to the default. The "not while the engine runs"
   * rule covers it too, because it is still a live swap of the
   * allocator. */
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(NULL), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, num_reactor_threads_rejected_while_running) {
  /* This has the same restriction as chttpsvr_set_engine_mem_mgmt_procs
   * above. The library bakes the value into the reactor when it builds it.
   * That restriction covers the 0 sentinel too, which reverts to the
   * default. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(2), ccol_not_permitted);
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(0), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, reinstall_after_full_stop_then_restart_succeeds) {
  /* The doc comment of chttpsvr_set_engine_mem_mgmt_procs says that you
   * "may call it again after the engine fully stops, which is after
   * chttpsvr_engine_wait() returns, and before the next chttpsvr_start()".
   * This is the only test in this file that covers that path. Every other
   * one covers only an install before a start, which _setup() does
   * implicitly, and a rejection while the engine runs. This test is not
   * vacuous. Make the "the engine already runs" check sticky, for example
   * with a latch that nothing clears on a stop, and this test fails, and no
   * other test in this file does.
   *
   * This must be the last test in this file. It tears g_srv down in full.
   * g_srv is the only thing that keeps the refcount of the shared engine
   * above zero in this process. The test then blocks until the shared
   * engine truly stops. It installs the procs again and starts a fresh
   * server on the same port. Every test that this file declares after this
   * one would otherwise run with no server that listens. The test repoints
   * g_srv at the new server, so that _teardown() still cleans up normally
   * at the exit of the process. */
  chttpsvr_stop(g_srv);
  __chttpsvr_destroy(g_srv);
  g_srv = CHTTPSVR_INVALID;
  chttpsvr_engine_wait();

  /* The engine start below must really use the procs that this test
   * installs again. It is not enough that it accepts them. This code
   * therefore takes a snapshot of the counters first. The check after the
   * restart can then require real forward progress. Without the snapshot,
   * that check only says "the counters are still above zero from before",
   * and it passes even when the reinstall silently did nothing. */
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

  /* This drives the reinstall path of
   * chttpsvr_set_engine_num_reactor_threads too, in the same restart cycle.
   * That path runs after a full stop and before the next chttpsvr_start.
   * This is the only restart cycle that this file makes. An explicit value
   * must be the exact value that the library wires into the reactor that it
   * creates again below. It is not enough that the library accepts that
   * value and then ignores it silently. */
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(3), ccol_success);

  char *err = NULL;
  /* _ccol_destructor is a safety net for a REQUIRE_* failure between here
     and the transfer of ownership to g_srv below. A REQUIRE_* of Tau
     returns from this function at once on a failure. Without this
     destructor, that return leaks the shared-engine reference of this
     server. It then hangs the chttpsvr_engine_wait() call in the
     _teardown() of this file at the exit of the process. This code sets the
     variable to CHTTPSVR_INVALID right after it hands ownership to g_srv.
     The destructor therefore never destroys the handle that g_srv then owns
     a second time. */
  chttpsvr new_srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, &err);
  REQUIRE_TRUE(new_srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(new_srv, CHTTP_GET, "/hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  REQUIRE_EQ((int)chttpsvr_start(new_srv, &cfg), (int)ccol_success);
  g_srv = new_srv;
  new_srv = CHTTPSVR_INVALID; /* ownership transferred to g_srv; see above */

  /* The shared ccol_event_loop reactor is a plain static variable.
   * ccol_event_loop_destroy destroys it in full when the code above
   * releases the last reference. chttpsvr_start below then builds it again
   * from the start, with ccol_event_loop_create_with_mprocs. There is no
   * pool that recycles an allocation across a restart. A fresh malloc or
   * calloc call through the procs that this test just installed again is
   * therefore guaranteed, and not merely likely. */
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             malloc_calloc_before);
  REQUIRE_EQ(_chttpsvr_engine_num_reactor_threads_for_tests(), (size_t)3);

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      ccol_create_chttpclient(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_INVALID);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);
  chttpcli_response *resp = NULL;
  rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* This code captures each check below on the contents of resp into a
     local first. It then frees resp unconditionally, right after, and
     before any REQUIRE_*. Such a REQUIRE_* could otherwise return early and
     leak resp. resp has no scoped destructor of its own, unlike cli
     above. */
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
