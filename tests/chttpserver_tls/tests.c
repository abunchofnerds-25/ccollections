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
/*     REAL TLS HANDSHAKE COVERAGE (dedicated binary -- see Makefile)         */
/*                                                                            */
/* tests/chttpserver/tests.c only covers TLS *argument validation* (see       */
/* serve_tls_zero_port_rejected_before_tls_init there), because a real        */
/* handshake needs a valid certificate/key pair: the vendored facio TLS      */
/* layer calls FIO_LOG_FATAL (aborts the process) when the configured cert   */
/* files are missing or invalid.  This suite generates a real, throwaway     */
/* self-signed cert/key pair via the `openssl` CLI at startup, starts a real */
/* TLS chttpsvr listener, and drives an actual HTTPS request through it via  */
/* chttpclient -- kept isolated in its own binary so a broken openssl CLI or */
/* a bad cert only fails this suite, not the rest of the chttpserver tests.  */
/* ========================================================================== */

#define TLS_TEST_PORT 18790
#define BASE_URL "https://127.0.0.1:18790"

static clog g_test_logger = NULL;
static chttpsvr g_tls_srv = NULL;
static char g_cert_dir[256];
static char g_cert_path[320];
static char g_key_path[320];
static bool g_cert_ready = false;

static void _hello_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, TLS!");
}

/* Generates a throwaway self-signed cert/key pair into a fresh mkdtemp()
   directory via the openssl CLI. Returns 0 on success, -1 on any failure
   (missing openssl binary, non-zero exit status, etc.) -- callers must treat
   -1 as "TLS integration could not be verified in this environment" rather
   than crash, since fio_tls_cert_add's FIO_LOG_FATAL on a missing/invalid
   cert file would abort the whole process. */
static int _generate_self_signed_cert(void) {
  snprintf(g_cert_dir, sizeof(g_cert_dir), "/tmp/chttpserver_tls_test_XXXXXX");
  if (!mkdtemp(g_cert_dir)) return -1;

  int dn =
      snprintf(g_cert_path, sizeof(g_cert_path), "%s/cert.pem", g_cert_dir);
  int kn = snprintf(g_key_path, sizeof(g_key_path), "%s/key.pem", g_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_key_path))
    return -1;

  char cmd[1024];
  int cn = snprintf(cmd, sizeof(cmd),
                    "openssl req -x509 -newkey rsa:2048 -nodes "
                    "-keyout '%s' -out '%s' -days 1 -subj '/CN=127.0.0.1' "
                    ">/dev/null 2>&1",
                    g_key_path, g_cert_path);
  if (cn < 0 || (size_t)cn >= sizeof(cmd)) return -1;

  int rc = system(cmd);
  if (rc != 0) return -1;
  if (access(g_cert_path, R_OK) != 0 || access(g_key_path, R_OK) != 0)
    return -1;
  return 0;
}

static void _remove_generated_cert(void) {
  if (g_cert_path[0]) unlink(g_cert_path);
  if (g_key_path[0]) unlink(g_key_path);
  if (g_cert_dir[0]) rmdir(g_cert_dir);
}

static void _teardown(void) {
  if (g_tls_srv) chttpsvr_stop(g_tls_srv);
  if (g_tls_srv) {
    __chttpsvr_destroy(g_tls_srv);
    g_tls_srv = NULL;
  }
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = NULL;
  }
  _remove_generated_cert();
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  g_test_logger = clog_open_fd(2, CLOG_INFO);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  if (_generate_self_signed_cert() != 0) {
    fprintf(stderr,
            "WARNING: could not generate a self-signed cert via the openssl "
            "CLI -- real TLS handshake tests will be skipped in this "
            "environment.\n");
    g_cert_ready = false;
    /* No chttpsvr_start has run yet in this branch, so fio_lib_destroy has
       not been registered via atexit -- registration order relative to it
       does not matter here. */
    atexit(_teardown);
    return;
  }
  g_cert_ready = true;

  g_tls_srv = create_chttpsvr(g_test_logger, &err);
  if (!g_tls_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n",
            err ? err : "(unknown)");
    exit(1);
  }
  chttpsvr_register_handler(g_tls_srv, CHTTP_GET, "/hello", _hello_tls_handler,
                            NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT;
  cfg.tls = &tls;

  ccol_retval_t rv = chttpsvr_start(g_tls_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: TLS chttpsvr_start failed: %d\n", rv);
    exit(1);
  }

  /* Must be registered AFTER chttpsvr_start, not before: the first
     chttpsvr_start call registers fio_lib_destroy via atexit (inside
     _fio_global_init). atexit handlers run in reverse registration order, so
     registering _teardown here (after) guarantees it runs BEFORE
     fio_lib_destroy at process exit -- stopping and joining the engine
     first. Registering it earlier (before chttpsvr_start) would let
     fio_lib_destroy free fio_data while the engine's own thread pool is
     still running, a use-after-free that segfaults on exit. */
  atexit(_teardown);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_tls, handshake_succeeds_when_ca_is_trusted) {
  /* Real end-to-end coverage: a client that trusts our self-signed cert (via
     ca_bundle_path pointing directly at it) must complete a genuine TLS
     handshake and receive the expected response. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli = create_chttpclient(NULL);
  REQUIRE_TRUE(cli != NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Hello, TLS!");

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(chttpserver_tls, handshake_fails_when_ca_is_untrusted) {
  /* A client using the default trust store (no ca_bundle_path override) must
     reject our self-signed cert -- proving the server actually performs a
     real, verifiable TLS handshake rather than, say, only checking the
     certificate files exist and then skipping verification. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_TRUE((void *)resp == NULL);
}
