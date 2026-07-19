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
/*     REAL TLS HANDSHAKE COVERAGE (dedicated binary; see Makefile)         */
/*                                                                            */
/* tests/chttpserver/tests.c only covers TLS *argument validation* (see       */
/* serve_tls_zero_port_rejected_before_tls_init there); a real handshake     */
/* needs a valid certificate/key pair. This suite generates a real,          */
/* throwaway self-signed cert/key pair via the `openssl` CLI at startup,     */
/* starts a real TLS chttpsvr listener, and drives an actual HTTPS request   */
/* through it via chttpclient. This file is compiled into its own binary,    */
/* tests_tls, separate from tests.c's tests binary (see the Makefile in      */
/* this same directory) so a broken openssl CLI or a bad cert only fails     */
/* this suite, not the rest of the chttpserver tests; ctls itself never      */
/* aborts the process on bad TLS input, so this split is purely for that     */
/* organizational isolation now, not to contain a process-abort failure      */
/* mode.                                                                     */
/* ========================================================================== */

#define TLS_TEST_PORT 18790
#define BASE_URL "https://127.0.0.1:18790"
/* Same server, addressed by a name the cert's CN=127.0.0.1 does NOT cover;
 * used to exercise hostname verification (both /etc/hosts entries for
 * "localhost" resolve to this same loopback server). */
#define BASE_URL_MISMATCHED_HOST "https://localhost:18790"

static clog g_test_logger = NULL;
static chttpsvr g_tls_srv = NULL;
static char g_cert_dir[256];
static char g_cert_path[320];
static char g_key_path[320];
static char g_client_cert_path[320];
static char g_client_key_path[320];
static bool g_cert_ready = false;

static void _hello_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, TLS!");
}

/* Generates a throwaway self-signed cert/key pair into a fresh mkdtemp()
   directory via the openssl CLI. Returns 0 on success, -1 on any failure
   (missing openssl binary, non-zero exit status, etc.); callers must treat
   -1 as "TLS integration could not be verified in this environment" rather
   than assume a partial/invalid cert file handed to ctls_ctx_cert_add would
   itself be fatal -- ctls never aborts the process on bad TLS input (see
   the ctls module notes), it simply fails the handshake. */
static int _openssl_selfsigned(const char *key_path, const char *cert_path,
                               const char *cn, const char *san) {
  char cmd[1024];
  int cn_len;
  if (san) {
    /* A real IP-address certificate is secured via a subjectAltName
     * iPAddress entry, per RFC 6125; not the legacy CN-matching fallback,
     * which ctls.c's own connect-side verification (X509_VERIFY_PARAM_
     * set1_ip_asc for an IP-literal target, X509_VERIFY_PARAM_set1_host
     * otherwise) does not use for an IP-literal target. Without this, the
     * hostname tests below would only ever be exercising CN string matching
     * by coincidence, not genuine IP-address certificate validation. */
    cn_len = snprintf(cmd, sizeof(cmd),
                      "openssl req -x509 -newkey rsa:2048 -nodes "
                      "-keyout '%s' -out '%s' -days 1 -subj '/CN=%s' "
                      "-addext 'subjectAltName=%s' >/dev/null 2>&1",
                      key_path, cert_path, cn, san);
  } else {
    cn_len = snprintf(cmd, sizeof(cmd),
                      "openssl req -x509 -newkey rsa:2048 -nodes "
                      "-keyout '%s' -out '%s' -days 1 -subj '/CN=%s' "
                      ">/dev/null 2>&1",
                      key_path, cert_path, cn);
  }
  if (cn_len < 0 || (size_t)cn_len >= sizeof(cmd)) return -1;
  if (system(cmd) != 0) return -1;
  if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) return -1;
  return 0;
}

static int _generate_self_signed_cert(void) {
  snprintf(g_cert_dir, sizeof(g_cert_dir), "/tmp/chttpserver_tls_test_XXXXXX");
  if (!mkdtemp(g_cert_dir)) return -1;

  int dn =
      snprintf(g_cert_path, sizeof(g_cert_path), "%s/cert.pem", g_cert_dir);
  int kn = snprintf(g_key_path, sizeof(g_key_path), "%s/key.pem", g_cert_dir);
  int cdn = snprintf(g_client_cert_path, sizeof(g_client_cert_path),
                     "%s/client_cert.pem", g_cert_dir);
  int ckn = snprintf(g_client_key_path, sizeof(g_client_key_path),
                     "%s/client_key.pem", g_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_key_path) || cdn < 0 ||
      (size_t)cdn >= sizeof(g_client_cert_path) || ckn < 0 ||
      (size_t)ckn >= sizeof(g_client_key_path))
    return -1;

  if (_openssl_selfsigned(g_key_path, g_cert_path, "127.0.0.1",
                          "IP:127.0.0.1") != 0)
    return -1;
  /* Client identity cert for the mTLS smoke test; self-signed and never
   * actually trusted by the server in this suite; it only needs to be a
   * well-formed cert/key pair so ctls_ctx_cert_add's cert-loading path (real
   * files, not the fake nonexistent paths used by chttpclient's own
   * set_tls_deep_copies_strings test) is exercised end-to-end. */
  if (_openssl_selfsigned(g_client_key_path, g_client_cert_path,
                          "chttpclient-test-client", NULL) != 0)
    return -1;
  return 0;
}

static void _remove_generated_cert(void) {
  if (g_cert_path[0]) unlink(g_cert_path);
  if (g_key_path[0]) unlink(g_key_path);
  if (g_client_cert_path[0]) unlink(g_client_cert_path);
  if (g_client_key_path[0]) unlink(g_client_key_path);
  if (g_cert_dir[0]) rmdir(g_cert_dir);
}

static void _teardown(void) {
  if (g_tls_srv) chttpsvr_stop(g_tls_srv);
  if (g_tls_srv) {
    __chttpsvr_destroy(g_tls_srv);
    g_tls_srv = NULL;
  }
  /* __chttpsvr_destroy releases this server's shared-engine reference but
   * does not synchronously wait for chttpserver's own shared event_loop
   * reactor to actually stop; chttpsvr_engine_wait() blocks until it has,
   * which is required here so the engine-installed logger (g_engine_logger
   * in chttpserver.c, set via chttpsvr_set_engine_logger) is guaranteed
   * reclaimed before this atexit handler returns. A no-op if TLS cert
   * generation failed above and no server was ever started. */
  chttpsvr_engine_wait();
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
            "CLI; real TLS handshake tests will be skipped in this "
            "environment.\n");
    g_cert_ready = false;
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

  /* Registered here, after chttpsvr_start, purely so g_tls_srv is already
     assigned by the time _teardown() (which stops and destroys it) can
     possibly run; the shared event_loop engine itself has no atexit-based
     teardown of its own to race (see chttpsvr_engine_wait()'s own doc
     comment: teardown runs on an explicitly joined reaper thread, not a
     process-exit hook). */
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
     reject our self-signed cert; proving the server actually performs a
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

TEST(chttpserver_tls, hostname_mismatch_rejected_when_verify_host_enabled) {
  /* Connects to the same server via "localhost"; which resolves to the
     same loopback address but does NOT match the cert's CN=127.0.0.1;
     with verify_host left at its default (true). This exercises ctls.c's
     own client-side hostname-verification wiring (ctls_conn_create_client's
     X509_VERIFY_PARAM_set1_host call); the CA is trusted (ca_bundle_path),
     so any rejection here can only be due to the hostname check, not an
     untrusted-issuer failure. */
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

  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_TRUE((void *)resp == NULL);

  chttpclient_destroy(cli);
}

TEST(chttpserver_tls, hostname_mismatch_allowed_when_verify_host_disabled) {
  /* Same mismatched-hostname connection as above, but with verify_host
     explicitly turned off: the handshake must now succeed, proving
     verify_host actually gates the check rather than always enforcing it. */
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
  tls.verify_host = false;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
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

TEST(chttpserver_tls, client_presents_certificate_mtls_smoke) {
  /* mTLS smoke test: the client presents its own certificate/key pair.
     The server in this suite does not require or verify a client
     certificate, so this does not prove server-side enforcement; it
     proves that chttpclient's cert_path/key_path plumbing through
     ctls_ctx_cert_add (a real cert+key pair, not the fake nonexistent paths
     used by chttpclient's own set_tls_deep_copies_strings test) loads
     correctly and does not break a normal handshake. */
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
  tls.cert_path = g_client_cert_path;
  tls.key_path = g_client_key_path;
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
