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

// chttpsvr_set_engine_mem_mgmt_procs() may be called only before the first
// chttpsvr_start() of the process, and tests.c starts a server in its _setup().
// So this file is a binary of its own: It installs counting procs and starts
// the shared engine exactly once, before anything else in the process can.

// The fixed port of this binary's server and its base URL, so the client in
// _setup() and the tests can reach it.
#define TEST_PORT 18795
#define BASE_URL "http://127.0.0.1:18795"

// The logger and the server that _setup() creates and _teardown() releases.
static clog g_test_logger = CLOG_NULL;
static chttpsvr g_srv = CHTTPSVR_NULL;

// Calls counted by the allocator below. They show that the shared reactor's
// internal allocations go through the configured procs.
static size_t g_mm_malloc_count = 0;
static size_t g_mm_free_count = 0;
static size_t g_mm_calloc_count = 0;
static size_t g_mm_realloc_count = 0;

// Counting wrappers: Each passes the call on to the C library and bumps its
// g_mm_*_count, so the tests can show that the engine allocates through the
// installed procs. They don't change how the allocator behaves, so the engine
// works normally while we count.
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

// Answers "Hello, mem-mgmt!"; the route of the request that _setup() makes.
static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, mem-mgmt!");
}

// Destroys g_srv and waits for the engine reaper with chttpsvr_engine_wait(),
// so no reactor thread runs while valgrind checks for leaks, then closes the
// logger. It's the atexit handler. On OpenBSD the exit handler runs while the
// loader holds a lock that the join needs, so there it skips the wait and
// leaves the logger open.
static void _teardown(void) {
  if (g_srv) chttpsvr_stop(g_srv);
  if (g_srv) {
    chttpsvr__destroy(g_srv);
    g_srv = CHTTPSVR_NULL;
  }
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_NULL;
  }
#endif
}

// The reactor thread count of the engine that _setup() started, read before
// any test runs, because a later test sets 3 and the test order depends on the
// compiler.
static size_t g_initial_reactor_threads;

// Installs the counting procs (before the first chttpsvr_start() of the
// process), starts g_srv on TEST_PORT and pushes one real request through the
// engine, so its internal allocations have happened before any test reads the
// counters. It sets g_test_logger, g_srv and g_initial_reactor_threads, and
// ends the process when a step fails.
__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  ccol_memmgmt_procs procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  ccol_retval rv = chttpsvr_set_engine_mem_mgmt_procs(&procs);
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

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;

  rv = chttpsvr_start(g_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }

  chttpcli cli = chttpcli_create(NULL);
  if (cli) {
    chttp_request *req =
        chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      chttpcli_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) chttpcli_resp_free(resp);
    }
    chttpcli_destroy(cli);
  }

  atexit(_teardown);
  g_initial_reactor_threads = chttpsvr__engine_num_reactor_threads_for_tests();
}

// Nothing is asserted about realloc: ceventloop never calls ccol__mem_realloc
// (its fd registry uses open addressing, which grows by calloc and free). The
// free count comes only from the server freeing the connection of _setup()'s
// client, which the reactor dispatches asynchronously, so it is read with a
// bounded retry.
TEST(chttpserver_mem_mgmt, procs_wired_into_engine_allocations) {
  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             (size_t)0);

  for (int attempt = 0;
       __atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED) == 0 &&
       attempt < 150;
       attempt++) {
    usleep(20000);
  }
  REQUIRE_GT(__atomic_load_n(&g_mm_free_count, __ATOMIC_RELAXED), (size_t)0);
}

// _setup() never set the reactor thread count, so the engine uses the
// documented default of one thread, not a CPU count.
TEST(chttpserver_mem_mgmt,
     num_reactor_threads_defaults_to_one_when_unconfigured) {
  REQUIRE_EQ(g_initial_reactor_threads, (size_t)1);
}

// The pointers are validated before the "engine already runs" check, so one
// missing pointer is refused in any engine state.
TEST(chttpserver_mem_mgmt, null_function_pointer_rejected) {
  ccol_memmgmt_procs bad_procs = {
      .malloc = _counting_malloc,
      .free = NULL,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&bad_procs), ccol_invalid_args);
}

// Swapping allocators after the library allocated with the old ones would
// give malloc and free pairs that don't match.
TEST(chttpserver_mem_mgmt, rejected_after_engine_already_running) {
  ccol_memmgmt_procs procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_not_permitted);
}

TEST(chttpserver_mem_mgmt, null_procs_reverts_rejected_while_running) {
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(NULL), ccol_not_permitted);
}

// The value is baked into the reactor when it's built; the 0 sentinel (the
// default) is refused too.
TEST(chttpserver_mem_mgmt, num_reactor_threads_rejected_while_running) {
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(2), ccol_not_permitted);
  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(0), ccol_not_permitted);
}

// The procs may be installed again after chttpsvr_engine_wait() and before the
// next start. A sticky "engine runs" latch fails this test alone. It must be
// the last test of the file: It stops the engine, installs the procs and a
// reactor thread count again, starts a fresh server on the same port as g_srv
// (so _teardown() cleans up normally) and requires the counters to move past a
// snapshot taken before the restart, because a reinstall that did nothing would
// leave them above zero anyway. The new handle has a scoped destructor until it
// goes to g_srv, so a failed REQUIRE can't leak an engine reference.
TEST(chttpserver_mem_mgmt, reinstall_after_full_stop_then_restart_succeeds) {
  chttpsvr_stop(g_srv);
  chttpsvr__destroy(g_srv);
  g_srv = CHTTPSVR_NULL;
  chttpsvr_engine_wait();

  size_t malloc_calloc_before =
      __atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
      __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED);

  ccol_memmgmt_procs procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_success);

  REQUIRE_EQ(chttpsvr_set_engine_num_reactor_threads(3), ccol_success);

  char *err = NULL;
  chttpsvr new_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(new_srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(new_srv, CHTTP_GET, "/hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  REQUIRE_EQ((int)chttpsvr_start(new_srv, &cfg), (int)ccol_success);
  g_srv = new_srv;
  new_srv = CHTTPSVR_NULL;

  REQUIRE_GT(__atomic_load_n(&g_mm_malloc_count, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_mm_calloc_count, __ATOMIC_RELAXED),
             malloc_calloc_before);
  REQUIRE_EQ(chttpsvr__engine_num_reactor_threads_for_tests(), (size_t)3);

  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);
  chttp_request *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);
  chttpcli_response *resp = NULL;
  rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches =
      body_present && strcmp(resp->body, "Hello, mem-mgmt!") == 0;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}
