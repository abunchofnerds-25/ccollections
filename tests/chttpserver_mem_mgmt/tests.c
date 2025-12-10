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
#include <fio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*     chttpsvr_set_engine_mem_mgmt_procs COVERAGE (dedicated binary)         */
/*                                                                            */
/* chttpsvr_set_engine_mem_mgmt_procs() may only be called before the first  */
/* chttpsvr_start() in the process; a process-wide, set-once-ever          */
/* requirement, exactly like the shared facio engine it configures. The rest */
/* of the chttpserver test suite (tests/chttpserver) already calls           */
/* chttpsvr_start() during its own shared _setup(), so testing the "install  */
/* procs, then start" happy path there is impossible. This suite is kept     */
/* isolated (same reasoning as tests/chttpserver_tls) so it can install      */
/* counting procs and start the shared engine exactly once, before anything  */
/* else in the process has a chance to.                                     */
/* ========================================================================== */

#define TEST_PORT 18795
#define BASE_URL "http://127.0.0.1:18795"

static clog g_test_logger = NULL;
static chttpsvr g_srv = NULL;

static size_t g_mm_malloc_count = 0;
static size_t g_mm_free_count = 0;
static size_t g_mm_calloc_count = 0;
static size_t g_mm_realloc_count = 0;

/* Counting wrappers around libc: prove that facio's internal allocations are
 * actually routed through the configured procs, without changing allocator
 * behavior (so the engine keeps working normally while we count). */
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
  /* __chttpsvr_destroy releases this server's shared-engine reference but no
   * longer synchronously waits for the shared facio reactor (shared with
   * chttpclient's async engine; see cfio_engine.h) to actually stop;
   * chttpsvr_engine_wait() blocks until it has, which is required here so
   * the engine-installed default logger is guaranteed reclaimed before this
   * atexit handler returns. See tests/chttpserver_tls/tests.c for the same
   * reasoning. */
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
   * (connection/protocol structs, FIOBJ headers, response buffers, ...)
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

  /* Must be registered AFTER chttpsvr_start, not before: the first
     chttpsvr_start call registers fio_lib_destroy via atexit. atexit handlers
     run in reverse registration order, so registering _teardown here (after)
     guarantees it runs BEFORE fio_lib_destroy at process exit; stopping and
     joining the engine first. See tests/chttpserver_tls/tests.c for the same
     reasoning. */
  atexit(_teardown);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_mem_mgmt, procs_wired_into_engine_allocations) {
  /* _setup() installed counting procs before the first chttpsvr_start() and
   * then drove a real request through the running engine. If facio's
   * internal allocations (the arena's front door plus the scattered
   * allocation sites swept to fio_malloc/fio_calloc/fio_realloc/fio_free)
   * were actually redirected to the configured procs, all four counters must
   * be nonzero by now. */
  REQUIRE_GT(g_mm_malloc_count + g_mm_calloc_count, (size_t)0);
  REQUIRE_GT(g_mm_free_count, (size_t)0);
}

TEST(chttpserver_mem_mgmt, procs_wired_into_engine_reallocations) {
  /* procs_wired_into_engine_allocations only proves malloc/calloc/free were
   * routed through the configured procs; fio_realloc2 (used by, e.g.,
   * fiobj_hash's table growth - see fiobj_hash.c's FIO_SET_REALLOC - when a
   * request's header FIOBJ hash outgrows its initial capacity) is a
   * distinct code path that a stray plain-libc-realloc call site could
   * regress without procs_wired_into_engine_allocations ever noticing, since
   * g_mm_realloc_count was tracked but never asserted on. Sending a request
   * with many distinct headers reliably forces that hash to grow at least
   * once. Capped well under facio's own HTTP_MAX_HEADER_COUNT (128, a
   * "header flood" security ceiling in third_party/facio/http.h enforced
   * over the whole header set including Host/Accept/User-Agent/etc, not
   * just these custom ones) so the request itself still succeeds normally
   * instead of being rejected with 413. */
  size_t realloc_before =
      __atomic_load_n(&g_mm_realloc_count, __ATOMIC_RELAXED);

  chttpcli cli = create_chttpclient(NULL);
  REQUIRE_TRUE(cli != NULL);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);
  char hdr_name[32];
  for (int i = 0; i < 64; i++) {
    snprintf(hdr_name, sizeof(hdr_name), "x-mm-header-%d", i);
    chttp_request_set_header(req, hdr_name, "v");
  }
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);

  REQUIRE_GT(__atomic_load_n(&g_mm_realloc_count, __ATOMIC_RELAXED),
            realloc_before);
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

  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };
  REQUIRE_EQ(chttpsvr_set_engine_mem_mgmt_procs(&procs), ccol_success);

  char *err = NULL;
  chttpsvr new_srv = create_chttpsvr(g_test_logger, &err);
  REQUIRE_TRUE(new_srv != NULL);
  ccol_retval_t rv =
      chttpsvr_register_handler(new_srv, CHTTP_GET, "/hello", _hello_handler,
                                NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  REQUIRE_EQ((int)chttpsvr_start(new_srv, &cfg), (int)ccol_success);
  g_srv = new_srv;

  /* Note on what is (and isn't) asserted below: a naive "malloc_count must
   * increase after this request" check does NOT hold here and would make
   * this test flaky/wrong, not stronger. fio_data's connection-state table
   * and, empirically (confirmed while writing this test, via a temporary
   * debug instrumentation pass), facio's per-connection protocol structs are
   * allocated once during this process's first-ever engine start and then
   * recycled internally across any number of later stop/restart cycles in
   * the same process; a second server's request in the same process can
   * therefore genuinely trigger zero *new* fio_malloc/fio_calloc calls even
   * though every allocation that already exists was originally obtained
   * through the counting procs. fio_has_mem_mgmt_procs() reflects the
   * routing flag itself (a real, direct observable of whether the reinstall
   * call took effect) without depending on whether facio's internal reuse
   * happens to need a fresh allocation on this particular call. */
  REQUIRE_TRUE(fio_has_mem_mgmt_procs());

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

  REQUIRE_TRUE(fio_has_mem_mgmt_procs());
}
