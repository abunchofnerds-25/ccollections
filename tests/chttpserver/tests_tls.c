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

#include <arpa/inet.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/test_helpers/chttpserver.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

/* The test clients of this binary write to the server with plain write(2), and
 * the server can close a connection while one of them is writing. The library
 * leaves the SIGPIPE disposition to the application, so this binary ignores it
 * itself; tests_sigpipe.c covers the library under the default disposition.
 *
 * test_reset_inherited_signals() puts back the rest of the signal state that
 * the parent process can leave behind, before any thread starts. */
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

/* ========================================================================== */
/*     REAL TLS HANDSHAKE COVERAGE (dedicated binary; see Makefile)           */
/*                                                                            */
/* tests/chttpserver/tests.c only covers the validation of the TLS arguments  */
/* (see serve_tls_zero_port_rejected_before_tls_init there), and a real       */
/* handshake needs a valid certificate and key pair. So this suite generates  */
/* a real, throwaway self-signed pair at startup with the `openssl` CLI,      */
/* starts a real TLS chttpsvr listener and drives a real HTTPS request        */
/* through it with chttpclient. The build compiles this file into a binary of */
/* its own, tests_tls, apart from the tests binary of tests.c (see the        */
/* Makefile in this directory), so a broken openssl CLI or a bad certificate  */
/* fails only this suite and not the rest of the chttpserver tests. ctls      */
/* itself never aborts the process on bad TLS input, so the split is only     */
/* about how the tests are laid out and isn't there to contain a failure that */
/* aborts the process.                                                        */
/* ========================================================================== */

#define TLS_TEST_PORT 18790
#define BASE_URL "https://127.0.0.1:18790"
/* The same server, addressed by a name that the certificate's CN=127.0.0.1 does
 * NOT cover, which exercises the host name verification. Both /etc/hosts
 * entries for "localhost" resolve to this same loopback server. */
#define BASE_URL_MISMATCHED_HOST "https://localhost:18790"

static clog g_test_logger = CLOG_NULL;
static chttpsvr g_tls_srv = CHTTPSVR_NULL;
static char g_cert_dir[256];
static char g_cert_path[320];
static char g_key_path[320];
static char g_client_cert_path[320];
static char g_client_key_path[320];
static char g_other_cert_path[320];
static char g_other_key_path[320];
static bool g_cert_ready = false;

static void _hello_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, TLS!");
}

/* Answers with the length of the body it got, as decimal text. */
static void _body_len_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)ctx;
  size_t len = 0;
  (void)chttpsvr_req_body(req, &len);
  chttpsvr_resp_printf(resp, "len=%zu", len);
}

/* Generates a throwaway self-signed certificate and key pair with the openssl
   CLI, in a fresh directory made by mkdtemp(). Returns 0 on success and -1 on
   any failure (no openssl binary, a non-zero exit status and so on). A caller
   must read -1 as "this environment couldn't verify the TLS integration", and
   must not assume that a partial or invalid certificate file given to
   ctls_ctx_cert_add would be fatal: ctls never aborts the process on
   bad TLS input (see the notes of the ctls module), it just fails the
   handshake. */
static int _openssl_selfsigned(const char *key_path, const char *cert_path,
                               const char *cn, const char *san) {
  char cmd[1024];
  int cn_len;
  if (san) {
    /* Per RFC 6125, what secures a real certificate for an IP address is a
     * subjectAltName iPAddress entry, not the old fallback that matches on the
     * CN. The verification ctls.c does on the connect side doesn't use that
     * fallback for a target that is an IP literal: it calls
     * X509_VERIFY_PARAM_set1_ip_asc for such a target and
     * X509_VERIFY_PARAM_set1_host otherwise. Without this entry the host name
     * tests below would only match a CN string by luck instead of validating a
     * real certificate for an IP address. */
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
  int odn = snprintf(g_other_cert_path, sizeof(g_other_cert_path),
                     "%s/other_cert.pem", g_cert_dir);
  int okn = snprintf(g_other_key_path, sizeof(g_other_key_path),
                     "%s/other_key.pem", g_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_key_path) || cdn < 0 ||
      (size_t)cdn >= sizeof(g_client_cert_path) || ckn < 0 ||
      (size_t)ckn >= sizeof(g_client_key_path) || odn < 0 ||
      (size_t)odn >= sizeof(g_other_cert_path) || okn < 0 ||
      (size_t)okn >= sizeof(g_other_key_path))
    return -1;

  if (_openssl_selfsigned(g_key_path, g_cert_path, "127.0.0.1",
                          "IP:127.0.0.1") != 0)
    return -1;
  /* The client's identity certificate for the mutual TLS smoke test. It's
   * self-signed and the server of this suite never trusts it, so all it needs
   * is to be a well-formed certificate and key pair. With it, the certificate
   * loading path of ctls_ctx_cert_add runs end to end on real files, while
   * chttpclient's set_tls_deep_copies_strings test uses fake paths that don't
   * exist. */
  if (_openssl_selfsigned(g_client_key_path, g_client_cert_path,
                          "chttpclient-test-client", NULL) != 0)
    return -1;
  /* A second client identity that no mutual TLS server of this suite trusts. */
  if (_openssl_selfsigned(g_other_key_path, g_other_cert_path,
                          "chttpclient-untrusted-client", NULL) != 0)
    return -1;
  return 0;
}

static void _remove_generated_cert(void) {
  if (g_cert_path[0]) unlink(g_cert_path);
  if (g_key_path[0]) unlink(g_key_path);
  if (g_client_cert_path[0]) unlink(g_client_cert_path);
  if (g_client_key_path[0]) unlink(g_client_key_path);
  if (g_other_cert_path[0]) unlink(g_other_cert_path);
  if (g_other_key_path[0]) unlink(g_other_key_path);
  if (g_cert_dir[0]) rmdir(g_cert_dir);
}

static void _teardown(void) {
  if (g_tls_srv) chttpsvr_stop(g_tls_srv);
  if (g_tls_srv) {
    __chttpsvr_destroy(g_tls_srv);
    g_tls_srv = CHTTPSVR_NULL;
  }
  /* __chttpsvr_destroy releases this server's shared-engine reference, but it
   * doesn't wait synchronously for chttpserver's shared ccol_event_loop reactor
   * to stop. chttpsvr_engine_wait() blocks until that reactor stops, which this
   * file needs so that the library gets back the logger the engine holds
   * (g_engine_logger in chttpserver.c, installed by chttpsvr_set_engine_logger)
   * before this atexit handler returns. The call does nothing when generating
   * the TLS certificate above failed and no server was ever started. */
  /* On OpenBSD this exit handler runs while the dynamic loader holds a lock
   * that joining the engine's reaper needs, so the wait would never return
   * there (see _teardown in tests.c). The reactor may then still be running
   * when the process ends, so the logger stays open too. */
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_NULL;
  }
#endif
  _remove_generated_cert();
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  g_test_logger = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  if (_generate_self_signed_cert() != 0) {
    fprintf(stderr,
            "WARNING: could not generate a self-signed cert with the "
            "openssl CLI. This environment skips the real TLS handshake "
            "tests.\n");
    g_cert_ready = false;
    atexit(_teardown);
    return;
  }
  g_cert_ready = true;

  g_tls_srv = chttpsvr_create(g_test_logger, &err);
  if (!g_tls_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n",
            err ? err : "(unknown)");
    exit(1);
  }
  chttpsvr_register_handler(g_tls_srv, CHTTP_GET, "/hello", _hello_tls_handler,
                            NULL);
  chttpsvr_register_handler(g_tls_srv, CHTTP_POST, "/body-len",
                            _body_len_tls_handler, NULL);

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

  /* Registered here, after chttpsvr_start, only so that g_tls_srv is set by the
     time _teardown() (which stops and destroys it) can run. The shared
     ccol_event_loop engine has no atexit-based teardown of its own to race with
     (see the doc comment of chttpsvr_engine_wait(): the teardown runs on a
     reaper thread that is explicitly joined, not in a process-exit hook). */
  atexit(_teardown);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(chttpserver_tls, handshake_succeeds_when_ca_is_trusted) {
  /* Real end-to-end coverage: a client that trusts this suite's self-signed
     certificate, through a ca_bundle_path pointing straight at that
     certificate, has to finish a real TLS handshake and get the expected
     response. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* Every check on the contents of resp is captured into a local first, and
     resp is freed right after no matter what, before any REQUIRE_* that could
     return early and leak it (unlike cli above, resp has no RAII destructor of
     its own). */
  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

TEST(chttpserver_tls, handshake_fails_when_ca_is_untrusted) {
  /* A client that uses the default trust store (no ca_bundle_path override) has
     to reject our self-signed cert. That shows the server does a real,
     verifiable TLS handshake and doesn't, for example, just check that the
     certificate files exist and then skip verification. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttp_request_t *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  chttp_request_free(req);

  /* resp is freed before any REQUIRE_* that could return early and leak it, no
     matter what, just as in handshake_succeeds_when_ca_is_trusted above. That
     includes the regression this test is here to catch (verification silently
     bypassed), where resp would be non-NULL. */
  bool resp_present = resp != NULL;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_FALSE(resp_present);
}

TEST(chttpserver_tls, hostname_mismatch_rejected_by_default) {
  /* Connects to the same server through "localhost", a name that resolves to
     the same loopback address but does NOT match the certificate's
     CN=127.0.0.1, with the hostname check left at its default, which is on.
     This exercises the client-side host name verification in ctls.c (the
     X509_VERIFY_PARAM_set1_host call of ctls_conn_create_client).
     ca_bundle_path makes the CA trusted, so a rejection here can only come from
     the host name check and not from an issuer nobody trusts. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req = chttp_request_create(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* resp is freed before any REQUIRE_* that could return early and leak it, no
     matter what. That includes the regression this test is here to catch
     (hostname verification silently bypassed), where resp would be non-NULL. */
  bool resp_present = resp != NULL;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_FALSE(resp_present);
}

TEST(chttpserver_tls, hostname_mismatch_allowed_when_hostname_check_skipped) {
  /* The same mismatched-hostname connection as above, but with the hostname
     check explicitly skipped and the chain pinned to g_cert_path. The handshake
     needs to succeed, which shows that insecure_skip_hostname_check gates only
     the hostname match and that the chain verification it leaves in place
     accepts this certificate. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.insecure_skip_hostname_check = true;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req = chttp_request_create(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* Every check on the contents of resp is captured into a local first, and
     resp is freed right after no matter what, before any REQUIRE_* that could
     return early and leak it (unlike cli above, resp has no RAII destructor of
     its own). */
  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

TEST(chttpserver_tls, client_presents_certificate_mtls_smoke) {
  /* mTLS smoke test: the client presents its own certificate/key pair. The
     server of this suite neither requires nor verifies a client certificate, so
     this doesn't prove any server-side enforcement. What it proves is that
     chttpclient's cert_path/key_path plumbing through ctls_ctx_cert_add loads a
     real cert+key pair correctly (unlike the fake, nonexistent paths of
     chttpclient's set_tls_deep_copies_strings test) and doesn't break a normal
     handshake. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.cert_path = g_client_cert_path;
  tls.key_path = g_client_key_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  /* Every check on the contents of resp is captured into a local first, and
     resp is freed right after no matter what, before any REQUIRE_* that could
     return early and leak it (unlike cli above, resp has no RAII destructor of
     its own). */
  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpclient_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

/* ========================================================================== */
/*   Mutual TLS                                                               */
/*                                                                            */
/* A server whose chttp_tls_config_t names a ca_bundle_path trusts this       */
/* suite's self-signed client certificate, which is its own CA. Each test     */
/* starts such a server of its own and drives one request with a client that  */
/* presents the trusted identity, an untrusted one, or nothing. The handler   */
/* answers with what chttpsvr_req_peer_cert_verified reports.                 */
/* ========================================================================== */

static void _peer_cert_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  chttpsvr_resp_printf(resp, "verified=%d",
                       chttpsvr_req_peer_cert_verified(req) ? 1 : 0);
}

/* Starts a mutual TLS server on port, and returns CHTTPSVR_NULL on any
   failure. */
static chttpsvr _mtls_server(uint16_t port, bool client_cert_optional) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_GET, "/peer", _peer_cert_handler, NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = g_client_cert_path;
  tls.client_cert_optional = client_cert_optional;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  cfg.tls = &tls;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

/* Sends GET /peer to 127.0.0.1:port over TLS, trusting this suite's server
   certificate and presenting cert_path and key_path when they aren't NULL.
   Returns the result of chttpclient_do and, on success, stores the status and
   the body in *status_out and body_out. */
static ccol_retval_t _mtls_get(uint16_t port, const char *cert_path,
                               const char *key_path, int *status_out,
                               char *body_out, size_t body_cap) {
  *status_out = -1;
  body_out[0] = '\0';
  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      chttpclient_create(NULL);
  if (cli == CHTTPCLI_NULL) return ccol_not_enough_memory;
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.cert_path = cert_path;
  tls.key_path = key_path;
  ccol_retval_t rv = chttpclient_set_tls(cli, &tls);
  if (rv != ccol_success) return rv;
  char url[128];
  snprintf(url, sizeof(url), "https://127.0.0.1:%u/peer", (unsigned)port);
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  chttpcli_response *resp = NULL;
  rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  if (resp) {
    *status_out = resp->status_code;
    if (resp->body) snprintf(body_out, body_cap, "%s", resp->body);
    chttpclient_resp_free(resp);
  }
  return rv;
}

TEST(chttpserver_tls, mtls_refuses_a_client_without_a_certificate) {
  /* A ca_bundle_path requires a client certificate by default, so a client that
     presents none never gets to the handler. A server that only requested a
     certificate would answer "verified=0". */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _mtls_server(TLS_TEST_PORT + 20, false);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int status = -1;
  char body[64];
  ccol_retval_t rv =
      _mtls_get(TLS_TEST_PORT + 20, NULL, NULL, &status, body, sizeof(body));
  REQUIRE_NE(rv, ccol_success);
  REQUIRE_EQ(status, -1);
}

TEST(chttpserver_tls, mtls_accepts_and_reports_a_verified_client) {
  /* A client presenting a certificate that the bundle trusts is served and the
     handler sees it as verified, in both modes. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  for (int optional = 0; optional < 2; optional++) {
    chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
        _mtls_server(TLS_TEST_PORT + 21, optional != 0);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    int status = -1;
    char body[64];
    ccol_retval_t rv =
        _mtls_get(TLS_TEST_PORT + 21, g_client_cert_path, g_client_key_path,
                  &status, body, sizeof(body));
    chttpsvr_destroy(srv);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_STREQ(body, "verified=1");
  }
}

TEST(chttpserver_tls, mtls_refuses_a_certificate_from_another_ca) {
  /* A client certificate that doesn't verify against the bundle fails the
     handshake in both modes, because client_cert_optional relaxes only the case
     of a missing certificate. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  for (int optional = 0; optional < 2; optional++) {
    chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
        _mtls_server(TLS_TEST_PORT + 22, optional != 0);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    int status = -1;
    char body[64];
    ccol_retval_t rv = _mtls_get(TLS_TEST_PORT + 22, g_other_cert_path,
                                 g_other_key_path, &status, body, sizeof(body));
    chttpsvr_destroy(srv);
    REQUIRE_NE(rv, ccol_success);
    REQUIRE_EQ(status, -1);
  }
}

TEST(chttpserver_tls, mtls_optional_accepts_an_anonymous_client_unverified) {
  /* With client_cert_optional a client that presents nothing is served and the
     handler sees it as not verified. An accessor that reported the verify
     result alone would answer "verified=1" here, because OpenSSL reports
     X509_V_OK for a peer that sent nothing. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _mtls_server(TLS_TEST_PORT + 23, true);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int status = -1;
  char body[64];
  ccol_retval_t rv =
      _mtls_get(TLS_TEST_PORT + 23, NULL, NULL, &status, body, sizeof(body));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=0");
}

TEST(chttpserver_tls, peer_cert_verified_is_false_without_mutual_tls) {
  /* A TLS server with no ca_bundle_path verifies nobody, so a client presenting
     a certificate there is still reported as not verified. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _peer_cert_handler, NULL),
             ccol_success);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 24;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1;
  char body[64];
  ccol_retval_t rv = _mtls_get(TLS_TEST_PORT + 24, g_client_cert_path,
                               g_client_key_path, &status, body, sizeof(body));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=0");
}

/* ========================================================================== */
/*   TLS config failures must never be silently swallowed                     */
/*                                                                            */
/* Ordinary testing reaches neither of the two gaps below. chttpsvr_start     */
/* must not quietly start the server as plain, unencrypted HTTP when cfg.tls  */
/* isn't NULL and only one of cert_path and key_path is set. It also must not */
/* ignore the return value of ctls_ctx_trust for ca_bundle_path, which would  */
/* leave a bad or unreadable CA bundle in place while the server runs TLS, so */
/* the mutual TLS client certificate the caller asked for wouldn't be         */
/* enforced. Either gap would return ccol_success with no way for the caller  */
/* to notice, so chttpsvr_start reports a real error in both cases instead,   */
/* and the tests below pin that directly.                                     */
/* ========================================================================== */

TEST(chttpserver_tls, start_rejects_cert_path_without_key_path) {
  /* A cert_path on its own, with key_path left NULL, is never a valid
     configuration, and the library rejects it before trying any TLS work
     at all. So this test needs no real certificate file and no g_cert_ready
     gate. */
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/cert.pem";
  tls.key_path = NULL;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 1;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_destroy(srv);
}

TEST(chttpserver_tls, start_rejects_key_path_without_cert_path) {
  /* The test above with the two fields swapped. */
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = NULL;
  tls.key_path = "/nonexistent/key.pem";
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 2;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_destroy(srv);
}

TEST(chttpserver_tls, start_rejects_ca_bundle_path_without_cert_key_pair) {
  /* A ca_bundle_path set while cert_path/key_path are both NULL must not be
     silently ignored. The TLS setup block only looks at ca_bundle_path inside
     the branch gated on cert_path AND key_path both being non-NULL, so ignoring
     it would start the server as plain, unencrypted HTTP on a port the caller
     believed was HTTPS with mutual-TLS client verification. It's exactly how
     you configure custom-CA verification on the client side
     (chttpclient_set_tls, like curl's --cacert), so a caller who carries the
     same mental model over to the server makes a realistic mistake and not a
     contrived one. The test needs no real certificate files and no g_cert_ready
     gate, because this must be rejected before any TLS work is tried. */
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = NULL;
  tls.key_path = NULL;
  tls.ca_bundle_path = "/nonexistent/ca-bundle.pem";
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 4;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  /* The rejected start must not leave a listener behind, so a second, plain
     HTTP start on the very same port must succeed cleanly. */
  chttpsvr_config_t plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 4;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

TEST(chttpserver_tls, start_rejects_tls_config_with_no_cert_or_key) {
  /* A cfg->tls set while cert_path/key_path/ca_bundle_path are ALL NULL must
     not be silently ignored (none of the individual pairing checks fires when
     everything is simply missing), because that would start the server as
     plain, unencrypted HTTP on a port the caller believed was HTTPS.
     CHTTP_TLS_DEFAULT (chttp.h) has exactly this shape, with no path at all,
     and is documented as shared, verification-on defaults for both chttpclient
     and chttpserver, so a caller who reaches for it here and forgets to set
     cert_path/key_path afterwards makes a realistic mistake and not a contrived
     one. The test needs no real certificate files and no g_cert_ready gate,
     because this is rejected before any TLS work is tried. */
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 5;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  /* The rejected start must not leave a listener behind, so a second, plain
     HTTP start on the very same port has to succeed cleanly. */
  chttpsvr_config_t plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 5;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

TEST(chttpserver_tls, start_rejects_a_ca_bundle_holding_no_certificate) {
  /* A readable CA bundle that parses to no certificate at all should fail the
     start instead of configuring an empty trust store. Under
     client_cert_optional an empty store rejects a client that presents a
     certificate but accepts one that presents none, and in the default mode it
     rejects every client, so either way the listener would come up with a trust
     store that isn't what the caller asked for. The test needs a real cert/key
     pair to get past ctls_ctx_cert_add and reach the ca_bundle_path handling.
 */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  char bundle[] = "chttpsvr_nocert_bundle_XXXXXX";
  int fd = mkstemp(bundle);
  REQUIRE_GT(fd, -1);
  static const char text[] = "readable, and not a PEM certificate\n";
  ssize_t written = write(fd, text, sizeof(text) - 1);
  close(fd);
  if (written != (ssize_t)(sizeof(text) - 1)) {
    unlink(bundle);
    REQUIRE_EQ((long)written, (long)(sizeof(text) - 1));
    return;
  }

  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  if (srv == CHTTPSVR_NULL) {
    unlink(bundle);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    return;
  }

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = bundle;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 6;
  cfg.tls = &tls;

  ccol_retval_t started = chttpsvr_start(srv, &cfg);

  /* The failed start must not leave a listener behind, so a second, plain HTTP
     start on the very same port needs to succeed cleanly. */
  chttpsvr_config_t plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 6;
  ccol_retval_t reused = chttpsvr_start(srv, &plain_cfg);

  chttpsvr_destroy(srv);
  unlink(bundle);

  REQUIRE_EQ(started, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(reused, ccol_success);
}

TEST(chttpserver_tls, start_reports_a_certificate_that_cannot_be_loaded) {
  /* A certificate path that names no file and a key that doesn't match the
     certificate both make chttpsvr_start report ccol_http_tls_cert_load_failed,
     the TLS layer's code, and leave the server able to start again. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/server.pem";
  tls.key_path = g_key_path;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 7;
  cfg.tls = &tls;
  ccol_retval_t missing = chttpsvr_start(srv, &cfg);
  tls.cert_path = g_key_path;
  ccol_retval_t not_a_cert = chttpsvr_start(srv, &cfg);
  chttpsvr_config_t plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 7;
  ccol_retval_t plain = chttpsvr_start(srv, &plain_cfg);
  REQUIRE_EQ(missing, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(not_a_cert, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(plain, ccol_success);
}

TEST(chttpserver_tls, start_rejects_unloadable_ca_bundle_path) {
  /* A perfectly valid cert/key pair but a ca_bundle_path that can't be loaded:
     chttpsvr_start fails instead of silently starting the server without
     the mutual-TLS enforcement the caller asked for. The test needs a real
     cert/key pair to get past ctls_ctx_cert_add and reach the ca_bundle_path
     handling it is about. */
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = "/nonexistent/ca-bundle.pem";
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 3;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_http_tls_cert_load_failed);

  /* The failed start must not leave a listener behind, so a second, plain HTTP
     start on the very same port must succeed cleanly. */
  chttpsvr_config_t plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 3;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*          PIPELINED INPUT THAT THE TLS LAYER ALREADY HOLDS                  */
/* ========================================================================== */

/* A raw OpenSSL client on a blocking socket with a receive timeout, so the test
 * controls exactly how many bytes go into each TLS record, which no HTTP client
 * API lets you do. */
typedef struct {
  SSL_CTX *ctx;
  SSL *ssl;
  int fd;
} _raw_tls_client_t;

static void _raw_tls_client_close(_raw_tls_client_t *c) {
  if (c->ssl) {
    SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
  }
  if (c->ctx) SSL_CTX_free(c->ctx);
  if (c->fd >= 0) close(c->fd);
  c->ssl = NULL;
  c->ctx = NULL;
  c->fd = -1;
}

static bool _raw_tls_client_open(_raw_tls_client_t *c) {
  c->ctx = NULL;
  c->ssl = NULL;
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) return false;
  struct timeval tv = {5, 0};
  setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TLS_TEST_PORT);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (test_connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    return false;
  c->ctx = SSL_CTX_new(TLS_client_method());
  if (!c->ctx) return false;
  c->ssl = SSL_new(c->ctx);
  if (!c->ssl || SSL_set_fd(c->ssl, c->fd) != 1) return false;
  return SSL_connect(c->ssl) == 1;
}

/* Writes all of buf. Each SSL_write call of at most 16384 bytes is one TLS
 * record, so a buffer of that size or smaller arrives as a single record. */
static bool _raw_tls_write_all(_raw_tls_client_t *c, const char *buf,
                               size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t chunk = len - off > 16384 ? 16384 : len - off;
    int w = SSL_write(c->ssl, buf + off, (int)chunk);
    if (w <= 0) return false;
    off += (size_t)w;
  }
  return true;
}

/* Reads until the peer closes, the receive timeout fires or out is full.
 * Returns the number of bytes read. */
static size_t _raw_tls_read_until_close(_raw_tls_client_t *c, char *out,
                                        size_t cap) {
  size_t total = 0;
  while (total < cap - 1) {
    int r = SSL_read(c->ssl, out + total, (int)(cap - 1 - total));
    if (r <= 0) break;
    total += (size_t)r;
  }
  out[total] = '\0';
  return total;
}

static size_t _count_occurrences(const char *hay, const char *needle) {
  size_t n = 0;
  for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

TEST(chttpserver_tls, pipelined_requests_in_one_record_are_all_answered) {
  /* 70 GET requests of about 440 bytes each make about 30 KB, which the client
   * sends as two TLS records of 16 KB and 14 KB. The server reads 8 KB at a
   * time, so once it has read into the second record the socket is empty while
   * the rest of that record sits inside the TLS layer, where epoll(7) can't see
   * it. Every request still has to be answered promptly without one more byte
   * from the client. The last request asks for a close, so the reply stream
   * ends with an EOF. */
  if (!g_cert_ready) return;
  enum { N_REQ = 70 };
  char pad[380];
  memset(pad, 'a', sizeof(pad) - 1);
  pad[sizeof(pad) - 1] = '\0';
  static char reqs[N_REQ * 512];
  size_t len = 0;
  for (int i = 0; i < N_REQ; i++) {
    int n = snprintf(reqs + len, sizeof(reqs) - len,
                     "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "X-Pad: %s\r\n%s\r\n",
                     pad, i == N_REQ - 1 ? "Connection: close\r\n" : "");
    REQUIRE_GT(n, 0);
    len += (size_t)n;
  }
  REQUIRE_GT(len - 16384, (size_t)(8192 + 4096));

  _raw_tls_client_t c = {.fd = -1};
  bool opened = _raw_tls_client_open(&c);
  bool wrote = opened && _raw_tls_write_all(&c, reqs, len);
  static char out[N_REQ * 512];
  size_t got = wrote ? _raw_tls_read_until_close(&c, out, sizeof(out)) : 0;
  (void)got;
  size_t answered = wrote ? _count_occurrences(out, "Hello, TLS!") : 0;
  _raw_tls_client_close(&c);

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(wrote);
  REQUIRE_EQ(answered, (size_t)N_REQ);
}

TEST(chttpserver_tls, body_and_next_request_in_the_tls_buffer_are_served) {
  /* One record holds the headers of a POST, its 9000-byte body and a second
   * request. The server reads 8 KB of it first, so most of the body and all of
   * the second request are inside the TLS layer by the time the POST reaches
   * its worker. */
  if (!g_cert_ready) return;
  static char msg[12000];
  char body[9000];
  memset(body, 'b', sizeof(body));
  int n = snprintf(msg, sizeof(msg),
                   "POST /body-len HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                   "Content-Length: %zu\r\n\r\n",
                   sizeof(body));
  REQUIRE_GT(n, 0);
  size_t len = (size_t)n;
  memcpy(msg + len, body, sizeof(body));
  len += sizeof(body);
  const char *second =
      "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  memcpy(msg + len, second, strlen(second));
  len += strlen(second);
  REQUIRE_LT(len, (size_t)16384);

  _raw_tls_client_t c = {.fd = -1};
  bool opened = _raw_tls_client_open(&c);
  bool wrote = opened && _raw_tls_write_all(&c, msg, len);
  char out[4096];
  if (wrote)
    _raw_tls_read_until_close(&c, out, sizeof(out));
  else
    out[0] = '\0';
  _raw_tls_client_close(&c);

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(wrote);
  REQUIRE_TRUE(strstr(out, "len=9000") != NULL);
  REQUIRE_TRUE(strstr(out, "Hello, TLS!") != NULL);
}

/* ========================================================================== */
/*                    SLOW CLIENTS OVER TLS                                   */
/* ========================================================================== */

static long long _tls_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000L;
}

/* Polls a server counter until it reaches want, for at most 5 s. */
static bool _tls_wait_parked_bodies(chttpsvr h, size_t want) {
  long long deadline = _tls_now_ms() + 5000;
  while (_chttpsvr_parked_count_for_tests(h, 1) < want) {
    if (_tls_now_ms() > deadline) return false;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return true;
}

TEST(chttpserver_tls, slow_tls_bodies_park_and_hold_no_worker_thread) {
  /* The TLS twin of slow_clients.slow_bodies_park_and_hold_no_worker_thread in
   * tests.c: four TLS clients stop in the middle of their bodies, the
   * connections park without a thread, and a fresh request is served. */
  if (!g_cert_ready) return;
  enum { N = 4 };
  _raw_tls_client_t c[N];
  bool opened = true;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /body-len HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: 1000\r\n\r\n0123456789");
  for (int i = 0; i < N; i++) {
    c[i].fd = -1;
    if (!_raw_tls_client_open(&c[i]) ||
        !_raw_tls_write_all(&c[i], head, (size_t)hn))
      opened = false;
  }
  bool parked = opened && _tls_wait_parked_bodies(g_tls_srv, N);
  size_t active = _chttpsvr_pool_active_for_tests(g_tls_srv, false);
  _raw_tls_client_t h = {.fd = -1};
  char out[4096];
  out[0] = '\0';
  const char *get =
      "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (_raw_tls_client_open(&h) && _raw_tls_write_all(&h, get, strlen(get)))
    _raw_tls_read_until_close(&h, out, sizeof(out));
  bool hello = strstr(out, "Hello, TLS!") != NULL;
  _raw_tls_client_close(&h);
  int completed = 0;
  char rest[990 + 64];
  memset(rest, 'r', 990);
  for (int i = 0; i < N; i++) {
    if (opened && _raw_tls_write_all(&c[i], rest, 990)) {
      /* The head and the body of the response can come in two records. */
      char buf[1024];
      size_t got = 0;
      for (int tries = 0; tries < 8 && got < sizeof(buf) - 1; tries++) {
        int r = SSL_read(c[i].ssl, buf + got, (int)(sizeof(buf) - 1 - got));
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = '\0';
        if (strstr(buf, "len=1000")) {
          completed++;
          break;
        }
      }
    }
    _raw_tls_client_close(&c[i]);
  }
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(active, (size_t)0);
  REQUIRE_TRUE(hello);
  REQUIRE_EQ(completed, N);
}

/* A TLS client whose records the test writes to the socket itself, so it can
 * stop in the middle of one. Reads go through the socket, while writes go into
 * a memory BIO that _split_flush moves to the socket. */
typedef struct {
  SSL_CTX *ctx;
  SSL *ssl;
  BIO *wbio;
  int fd;
} _split_tls_client_t;

static bool _split_flush(_split_tls_client_t *c, size_t max) {
  char buf[65536];
  while (max && BIO_ctrl_pending(c->wbio) > 0) {
    size_t want = max < sizeof(buf) ? max : sizeof(buf);
    int n = BIO_read(c->wbio, buf, (int)want);
    if (n <= 0) break;
    size_t off = 0;
    while (off < (size_t)n) {
      ssize_t w = write(c->fd, buf + off, (size_t)n - off);
      if (w < 0 && errno == EAGAIN) {
        struct pollfd p = {.fd = c->fd, .events = POLLOUT};
        poll(&p, 1, 1000);
        continue;
      }
      if (w <= 0) return false;
      off += (size_t)w;
    }
    max -= (size_t)n;
  }
  return true;
}

static void _split_close(_split_tls_client_t *c) {
  if (c->ssl) SSL_free(c->ssl);
  if (c->ctx) SSL_CTX_free(c->ctx);
  if (c->fd >= 0) close(c->fd);
  c->ssl = NULL;
  c->ctx = NULL;
  c->fd = -1;
}

static bool _split_open(_split_tls_client_t *c) {
  c->ctx = NULL;
  c->ssl = NULL;
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) return false;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TLS_TEST_PORT);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (test_connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    return false;
  fcntl(c->fd, F_SETFL, O_NONBLOCK);
  c->ctx = SSL_CTX_new(TLS_client_method());
  if (!c->ctx) return false;
  c->ssl = SSL_new(c->ctx);
  if (!c->ssl) return false;
  BIO *rbio = BIO_new_socket(c->fd, BIO_NOCLOSE);
  c->wbio = BIO_new(BIO_s_mem());
  if (!rbio || !c->wbio) return false;
  SSL_set_bio(c->ssl, rbio, c->wbio);
  long long deadline = _tls_now_ms() + 5000;
  for (;;) {
    int r = SSL_connect(c->ssl);
    if (!_split_flush(c, SIZE_MAX)) return false;
    if (r == 1) return true;
    int e = SSL_get_error(c->ssl, r);
    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
    if (_tls_now_ms() > deadline) return false;
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    poll(&p, 1, 100);
  }
}

/* Reads one TLS response, for at most timeout_ms, and returns true when it
 * contains needle. */
static bool _split_read_has(_split_tls_client_t *c, const char *needle,
                            int timeout_ms) {
  char buf[4096];
  size_t got = 0;
  long long deadline = _tls_now_ms() + timeout_ms;
  while (got < sizeof(buf) - 1) {
    int r = SSL_read(c->ssl, buf + got, (int)(sizeof(buf) - 1 - got));
    if (r > 0) {
      got += (size_t)r;
      buf[got] = '\0';
      if (strstr(buf, needle)) return true;
      continue;
    }
    int e = SSL_get_error(c->ssl, r);
    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
    _split_flush(c, SIZE_MAX);
    long long left = deadline - _tls_now_ms();
    if (left <= 0) return false;
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    poll(&p, 1, (int)left);
  }
  return false;
}

TEST(chttpserver_tls, a_record_split_on_the_wire_still_wakes_the_reader) {
  /* A parked body asks the kernel to wake the reactor only once enough of it is
   * queued (SO_RCVLOWAT). The plaintext count that the framing still expects is
   * a safe mark while OpenSSL holds no part of a record, because a record never
   * carries fewer bytes on the wire than the plaintext inside it. It is NOT
   * safe once OpenSSL has read only part of a record.
   *
   * Here the 5000-byte body travels in one record, and the client sends all of
   * it except the last 10 bytes. That is enough to reach the mark of 5000 and
   * wake the reactor but not enough to decrypt anything. The server reads the
   * partial record into OpenSSL and parks again with the whole body still
   * expected, and the last 10 bytes still have to wake it. A mark of 5000
   * there, taken from the plaintext alone, waits for 5000 bytes that never
   * come, and the response doesn't arrive within its bound. */
  if (!g_cert_ready) return;
  _split_tls_client_t c = {.fd = -1};
  bool opened = _split_open(&c);
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /body-len HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: 5000\r\n\r\n");
  size_t parks0 = _chttpsvr_park_count_for_tests();
  bool sent_head =
      opened && SSL_write(c.ssl, head, hn) == hn && _split_flush(&c, SIZE_MAX);
  bool first_park = sent_head && _tls_wait_parked_bodies(g_tls_srv, 1);
  static char body[5000];
  memset(body, 'z', sizeof(body));
  bool staged = first_park &&
                SSL_write(c.ssl, body, (int)sizeof(body)) == (int)sizeof(body);
  size_t record = staged ? BIO_ctrl_pending(c.wbio) : 0;
  size_t parks1 = _chttpsvr_park_count_for_tests();
  bool partial = staged && record > 5010 && _split_flush(&c, record - 10);
  /* The reactor wakes on the partial record, a worker reads it into OpenSSL,
   * and the connection parks a second time. */
  bool second_park = false;
  long long deadline = _tls_now_ms() + 5000;
  while (partial && !second_park && _tls_now_ms() < deadline) {
    second_park = _chttpsvr_park_count_for_tests() > parks1 &&
                  _chttpsvr_parked_count_for_tests(g_tls_srv, 1) == 1;
    struct timespec nap = {0, 1000000L};
    if (!second_park) nanosleep(&nap, NULL);
  }
  bool rest = second_park && _split_flush(&c, SIZE_MAX);
  bool answered = rest && _split_read_has(&c, "len=5000", 5000);
  _split_close(&c);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(first_park);
  REQUIRE_GT(parks1, parks0);
  REQUIRE_TRUE(partial);
  REQUIRE_TRUE(second_park);
  REQUIRE_TRUE(answered);
}

/* Sends req and reads one whole response over c. Returns its length, or 0. */
static size_t _tls_exchange(_raw_tls_client_t *c, const char *req, char *buf,
                            size_t cap) {
  if (!_raw_tls_write_all(c, req, strlen(req))) return 0;
  size_t got = 0;
  while (got < cap - 1) {
    int r = SSL_read(c->ssl, buf + got, (int)(cap - 1 - got));
    if (r <= 0) return 0;
    got += (size_t)r;
    buf[got] = '\0';
    const char *end = strstr(buf, "\r\n\r\n");
    const char *cl = strstr(buf, "content-length:");
    if (end && cl) {
      size_t want = (size_t)(end + 4 - buf) + strtoul(cl + 15, NULL, 10);
      if (got >= want) return got == want ? got : 0;
    }
  }
  return 0;
}

TEST(chttpserver_tls, a_tls_write_cut_inside_the_head_or_the_body_resumes) {
  /* The TLS twin of slow_clients.a_write_cut_inside_the_head_or_the_body_
   * resumes_intact in tests.c: a response write that stops inside the head or
   * inside the body resumes from the exact byte, and the client receives the
   * same bytes as from a single write. */
  if (!g_cert_ready) return;
  _chttpsvr_set_date_override_for_tests(1700000000);
  const char *get = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  _raw_tls_client_t c = {.fd = -1};
  static char ref[4096], got[4096];
  bool opened = _raw_tls_client_open(&c);
  size_t ref_len = opened ? _tls_exchange(&c, get, ref, sizeof(ref)) : 0;
  size_t body = strlen("Hello, TLS!");
  size_t head_len = ref_len > body ? ref_len - body : 0;
  size_t cuts[2] = {10, head_len + 4};
  int intact = 0;
  for (int i = 0; i < 2 && head_len; i++) {
    _chttpsvr_force_short_response_write_for_tests(cuts[i]);
    size_t n = _tls_exchange(&c, get, got, sizeof(got));
    if (n == ref_len && memcmp(got, ref, n) == 0) intact++;
  }
  _raw_tls_client_close(&c);
  _chttpsvr_set_date_override_for_tests(INT64_MIN);
  REQUIRE_TRUE(opened);
  REQUIRE_GT(ref_len, body);
  REQUIRE_EQ(intact, 2);
}

/* The counter of lingering closes moves on the closing thread, which can run
   just after the client has already seen the end of the stream, so a test waits
   for it, for at most 5 s. */
static bool _linger_count_passes(size_t before) {
  for (int i = 0; i < 5000; i++) {
    if (_chttpsvr_linger_count_for_tests() > before) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

TEST(chttpserver_tls, a_refused_upload_ends_with_close_notify_and_no_reset) {
  /* The TLS twin of lingering_close.a_refused_upload_gets_its_response_and_
   * an_orderly_end in tests.c. The server refuses an upload with 404 and never
   * reads its body, and the client reads the whole response, then the server's
   * close_notify and then an orderly end of the TCP stream. Closing with the
   * unread body still in the socket would send a reset, which the read after
   * the close_notify reports as ECONNRESET. */
  if (!g_cert_ready) return;
  size_t lingers = _chttpsvr_linger_count_for_tests();
  _raw_tls_client_t c = {.fd = -1};
  bool opened = _raw_tls_client_open(&c);
  static char req[256 + 32768];
  int hn = snprintf(req, 256,
                    "POST /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: 1048576\r\n\r\n");
  memset(req + hn, 'u', 32768);
  bool wrote =
      opened && hn > 0 && _raw_tls_write_all(&c, req, (size_t)hn + 32768);
  static char out[4096];
  size_t got = wrote ? _raw_tls_read_until_close(&c, out, sizeof(out)) : 0;
  int ssl_end = wrote ? SSL_get_error(c.ssl, 0) : -1;
  bool got_404 = got > 0 && strstr(out, "HTTP/1.1 404") != NULL;
  /* The TCP stream after the close_notify. */
  int tcp_end = -1;
  if (got_404) {
    struct pollfd p = {.fd = c.fd, .events = POLLIN};
    if (poll(&p, 1, 5000) > 0) {
      char b;
      ssize_t r = read(c.fd, &b, 1);
      tcp_end = r == 0 ? 0 : (r < 0 ? errno : -2);
    }
  }
  bool lingered = _linger_count_passes(lingers);
  _raw_tls_client_close(&c);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(wrote);
  REQUIRE_TRUE(got_404);
  REQUIRE_EQ(ssl_end, SSL_ERROR_ZERO_RETURN);
  REQUIRE_EQ(tcp_end, 0);
  REQUIRE_TRUE(lingered);
}

/* ========================================================================== */
/*   Revocation lists, unreadable paths and the identity of a client          */
/*                                                                            */
/* These tests drive ctls directly over a socketpair and chttpserver over     */
/* real connections. A small PKI that the openssl CLI makes on first use      */
/* holds a root CA, an intermediate CA, a client certificate issued by each   */
/* of them, a revoked client certificate, a revoked server certificate and a  */
/* CRL of each CA.                                                            */
/* ========================================================================== */

#include <internal/ctls.h>
#include <internal/test_helpers/ctls.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

static char g_pki_dir[400];
static int g_pki_state = 0; /* 0 untried, 1 ready, -1 failed */

static const char *const g_pki_files[] = {
    "gen.sh",         "ext.cnf",        "root.cnf",
    "root.idx",       "root.idx.attr",  "root.idx.attr.old",
    "root.idx.old",   "inter.cnf",      "inter.idx",
    "inter.idx.attr", "inter.idx.old",  "root.key",
    "root.csr",       "root.crt",       "root.crl",
    "inter.key",      "inter.csr",      "inter.crt",
    "inter.crl",      "good.key",       "good.csr",
    "good.crt",       "revoked.key",    "revoked.csr",
    "revoked.crt",    "ileaf.key",      "ileaf.csr",
    "ileaf.crt",      "rsrv.key",       "rsrv.csr",
    "rsrv.crt",       "b_root_crl.pem", "b_inter_crl_only.pem",
    "b_both_crl.pem", "b_no_crl.pem",   "b_crl_only.pem",
    "fifo",           "02.pem",         "03.pem",
    "04.pem",         "05.pem",         "06.pem"};

static void _pki_remove(void) {
  if (!g_pki_dir[0]) return;
  char path[512];
  for (size_t i = 0; i < sizeof(g_pki_files) / sizeof(g_pki_files[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", g_pki_dir, g_pki_files[i]);
    unlink(path);
  }
  rmdir(g_pki_dir);
}

static const char g_pki_script[] =
    "set -e\n"
    "mkca() {\n"
    "  printf '[ca]\\ndefault_ca=x\\n[x]\\ndir=.\\ndatabase=%s.idx\\n"
    "new_certs_dir=.\\ncertificate=%s.crt\\nprivate_key=%s.key\\n"
    "default_md=sha256\\npolicy=p\\ndefault_crl_days=1\\n[p]\\n"
    "commonName=supplied\\n' $1 $1 $1 > $1.cnf\n"
    "  : > $1.idx\n"
    "}\n"
    "printf '[ca_ext]\\nbasicConstraints=critical,CA:TRUE\\n"
    "keyUsage=keyCertSign,cRLSign\\n[cli]\\nextendedKeyUsage=clientAuth\\n"
    "[srv]\\nextendedKeyUsage=serverAuth\\nsubjectAltName=DNS:srv.test\\n'"
    " > ext.cnf\n"
    "openssl req -newkey rsa:2048 -nodes -keyout root.key -out root.csr"
    " -subj /CN=crl-root\n"
    "openssl x509 -req -in root.csr -signkey root.key -days 2"
    " -extfile ext.cnf -extensions ca_ext -out root.crt\n"
    "mkca root\n"
    "leaf() {\n"
    "  openssl req -newkey rsa:2048 -nodes -keyout $1.key -out $1.csr"
    " -subj \"$2\"\n"
    "  openssl x509 -req -in $1.csr -CA $3.crt -CAkey $3.key -set_serial $4"
    " -days 1 -extfile ext.cnf -extensions $5 -out $1.crt\n"
    "}\n"
    "leaf good '/CN=crl-good/O=Example, Inc.' root 2 cli\n"
    "leaf revoked /CN=crl-revoked root 3 cli\n"
    "leaf inter /CN=crl-inter root 4 ca_ext\n"
    "mkca inter\n"
    "leaf ileaf /CN=crl-inter-leaf inter 5 cli\n"
    "leaf rsrv /CN=srv.test root 6 srv\n"
    "openssl ca -batch -config root.cnf -revoke revoked.crt\n"
    "openssl ca -batch -config root.cnf -revoke rsrv.crt\n"
    "openssl ca -batch -config root.cnf -gencrl -out root.crl\n"
    "openssl ca -batch -config inter.cnf -gencrl -out inter.crl\n"
    "cat root.crt root.crl > b_root_crl.pem\n"
    "cat root.crt inter.crt inter.crl > b_inter_crl_only.pem\n"
    "cat root.crt inter.crt inter.crl root.crl > b_both_crl.pem\n"
    "cat root.crt inter.crt > b_no_crl.pem\n"
    "cat root.crl > b_crl_only.pem\n";

/* Makes the PKI once. Returns false when the openssl CLI is missing or fails,
   and the tests that need the PKI then skip. */
static bool _pki_ready(void) {
  if (g_pki_state != 0) return g_pki_state > 0;
  g_pki_state = -1;
  if (!g_cert_dir[0]) return false;
  snprintf(g_pki_dir, sizeof(g_pki_dir), "%s/pki", g_cert_dir);
  if (mkdir(g_pki_dir, 0700) != 0) return false;
  atexit(_pki_remove);
  char path[512];
  snprintf(path, sizeof(path), "%s/gen.sh", g_pki_dir);
  FILE *f = fopen(path, "w");
  if (!f) return false;
  bool wrote = fputs(g_pki_script, f) >= 0;
  if (fclose(f) != 0 || !wrote) return false;
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "cd '%s' && sh gen.sh >/dev/null 2>&1", g_pki_dir);
  if (system(cmd) != 0) return false;
  g_pki_state = 1;
  return true;
}

static const char *_pki(const char *name) {
  static char paths[8][512];
  static int next = 0;
  char *p = paths[next];
  next = (next + 1) % 8;
  snprintf(p, sizeof(paths[0]), "%s/%s", g_pki_dir, name);
  return p;
}

/* Drives a ctls server connection and a ctls client connection over a
   socketpair to the end of their handshakes. Returns true when both finished,
   and leaves both connections to the caller. */
typedef struct {
  int fds[2];
  ctls_conn_t *server;
  ctls_conn_t *client;
  bool done;
} _tls_pair_t;

static void _tls_pair_open(_tls_pair_t *p, ctls_ctx_t *server_ctx,
                           ctls_ctx_t *client_ctx, const char *host) {
  memset(p, 0, sizeof(*p));
  p->fds[0] = p->fds[1] = -1;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, p->fds) != 0) return;
  for (int i = 0; i < 2; i++)
    fcntl(p->fds[i], F_SETFL, fcntl(p->fds[i], F_GETFL, 0) | O_NONBLOCK);
  p->server = ctls_conn_create_server(server_ctx, p->fds[0], NULL, NULL);
  p->client =
      ctls_conn_create_client(client_ctx, p->fds[1], host, host != NULL, NULL);
  if (!p->server || !p->client) return;
  bool s_done = false, c_done = false;
  for (int i = 0; i < 2000 && !(s_done && c_done); i++) {
    if (!s_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(p->server);
      if (r == CTLS_HANDSHAKE_ERROR) return;
      s_done = r == CTLS_HANDSHAKE_DONE;
    }
    if (!c_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(p->client);
      if (r == CTLS_HANDSHAKE_ERROR) return;
      c_done = r == CTLS_HANDSHAKE_DONE;
    }
  }
  p->done = s_done && c_done;
}

static void _tls_pair_close(_tls_pair_t *p) {
  ctls_conn_destroy(p->client);
  ctls_conn_destroy(p->server);
  if (p->fds[0] >= 0) close(p->fds[0]);
  if (p->fds[1] >= 0) close(p->fds[1]);
}

/* Runs one handshake of a server that trusts bundle against a client that
   presents cert and key (nothing when cert is NULL). Reports whether the
   handshake finished, and the server's verify result. */
static bool _crl_handshake(const char *bundle, const char *cert,
                           const char *key, long *verify_out) {
  *verify_out = -1;
  ctls_ctx_t *s = ctls_ctx_new(NULL);
  ctls_ctx_t *c = ctls_ctx_new(NULL);
  bool ok = s && c &&
            ctls_ctx_cert_add(s, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                ccol_success &&
            ctls_ctx_trust(s, bundle, NULL) == ccol_success &&
            ctls_ctx_trust(c, g_cert_path, NULL) == ccol_success;
  if (ok && cert)
    ok = ctls_ctx_cert_add(c, NULL, cert, key, NULL, NULL) == ccol_success;
  bool done = false;
  if (ok) {
    _tls_pair_t p;
    _tls_pair_open(&p, s, c, "127.0.0.1");
    done = p.done;
    if (p.server) *verify_out = ctls_conn_verify_result(p.server);
    _tls_pair_close(&p);
  }
  ctls_ctx_release(c);
  ctls_ctx_release(s);
  return ok && done;
}

TEST(tls_crl, a_revoked_client_certificate_is_refused) {
  /* A CA bundle that holds a CRL enforces it. The handshake of a client whose
     certificate the CRL revokes fails with X509_V_ERR_CERT_REVOKED, and a
     client certificate of the same CA that isn't revoked still verifies.
     With the CRL loaded but not enforced, the revoked client would verify and
     the handshake would succeed. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  long revoked_verify = 0, good_verify = -1;
  bool revoked_done =
      _crl_handshake(_pki("b_root_crl.pem"), _pki("revoked.crt"),
                     _pki("revoked.key"), &revoked_verify);
  bool good_done = _crl_handshake(_pki("b_root_crl.pem"), _pki("good.crt"),
                                  _pki("good.key"), &good_verify);
  REQUIRE_FALSE(revoked_done);
  REQUIRE_EQ(revoked_verify, (long)X509_V_ERR_CERT_REVOKED);
  REQUIRE_TRUE(good_done);
  REQUIRE_EQ(good_verify, (long)X509_V_OK);
}

TEST(tls_crl, every_ca_of_the_chain_needs_a_crl) {
  /* With a CRL in the bundle, every certificate of the chain below the trust
     anchor needs a CRL from its issuer. A bundle with the intermediate CA's CRL
     and none of the root's refuses a client of the intermediate with
     X509_V_ERR_UNABLE_TO_GET_CRL, the same bundle with both CRLs accepts it,
     and a bundle with no CRL at all checks nothing. Checking only the leaf
     (X509_V_FLAG_CRL_CHECK without X509_V_FLAG_CRL_CHECK_ALL) would accept the
     client with only the intermediate's CRL. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  long partial_verify = 0, both_verify = -1, none_verify = -1;
  bool partial_done =
      _crl_handshake(_pki("b_inter_crl_only.pem"), _pki("ileaf.crt"),
                     _pki("ileaf.key"), &partial_verify);
  bool both_done = _crl_handshake(_pki("b_both_crl.pem"), _pki("ileaf.crt"),
                                  _pki("ileaf.key"), &both_verify);
  bool none_done = _crl_handshake(_pki("b_no_crl.pem"), _pki("ileaf.crt"),
                                  _pki("ileaf.key"), &none_verify);
  REQUIRE_FALSE(partial_done);
  REQUIRE_EQ(partial_verify, (long)X509_V_ERR_UNABLE_TO_GET_CRL);
  REQUIRE_TRUE(both_done);
  REQUIRE_EQ(both_verify, (long)X509_V_OK);
  REQUIRE_TRUE(none_done);
  REQUIRE_EQ(none_verify, (long)X509_V_OK);
}

TEST(tls_crl, a_client_refuses_a_revoked_server_certificate) {
  /* A client whose CA bundle holds a CRL refuses a server whose certificate the
     CRL revokes. Without enforcement the handshake would succeed. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  ctls_ctx_t *s = ctls_ctx_new(NULL);
  ctls_ctx_t *c = ctls_ctx_new(NULL);
  bool ok = s && c &&
            ctls_ctx_cert_add(s, NULL, _pki("rsrv.crt"), _pki("rsrv.key"), NULL,
                              NULL) == ccol_success &&
            ctls_ctx_trust(c, _pki("b_root_crl.pem"), NULL) == ccol_success;
  bool done = true;
  long verify = 0;
  if (ok) {
    _tls_pair_t p;
    _tls_pair_open(&p, s, c, "srv.test");
    done = p.done;
    if (p.client) verify = ctls_conn_verify_result(p.client);
    _tls_pair_close(&p);
  }
  ctls_ctx_release(c);
  ctls_ctx_release(s);
  REQUIRE_TRUE(ok);
  REQUIRE_FALSE(done);
  REQUIRE_EQ(verify, (long)X509_V_ERR_CERT_REVOKED);
}

TEST(tls_crl, a_server_refuses_a_revoked_client_through_chttpsvr) {
  /* The same rule through the public configuration: a ca_bundle_path holding a
     CRL refuses a client that it revokes, and serves one that it doesn't. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _peer_cert_handler, NULL),
             ccol_success);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = _pki("b_root_crl.pem");
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 40;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1;
  char body[64];
  ccol_retval_t revoked_rv =
      _mtls_get(TLS_TEST_PORT + 40, _pki("revoked.crt"), _pki("revoked.key"),
                &status, body, sizeof(body));
  int revoked_status = status;
  ccol_retval_t good_rv =
      _mtls_get(TLS_TEST_PORT + 40, _pki("good.crt"), _pki("good.key"), &status,
                body, sizeof(body));
  REQUIRE_NE(revoked_rv, ccol_success);
  REQUIRE_EQ(revoked_status, -1);
  REQUIRE_EQ(good_rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=1");
}

TEST(tls_crl, a_bundle_of_nothing_but_crls_is_refused) {
  /* A CRL names no trust anchor, so a bundle with only CRLs configures no
     issuer and is refused like an empty one. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  ctls_ctx_t *s = ctls_ctx_new(NULL);
  REQUIRE_TRUE(s != NULL);
  ccol_retval_t rv = ctls_ctx_trust(s, _pki("b_crl_only.pem"), NULL);
  ctls_ctx_release(s);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
}

/* ----- a path that is not a regular file never blocks ----- */

typedef struct {
  const char *path;
  ccol_retval_t trust_rv;
  ccol_retval_t cert_rv;
  char *trust_why;
  atomic_bool done;
} _fifo_ctx_t;

static void *_fifo_trust_thread(void *arg) {
  _fifo_ctx_t *f = (_fifo_ctx_t *)arg;
  ctls_ctx_t *c = ctls_ctx_new(NULL);
  f->trust_rv =
      c ? ctls_ctx_trust(c, f->path, &f->trust_why) : ccol_not_enough_memory;
  f->cert_rv = c ? ctls_ctx_cert_add(c, NULL, f->path, g_key_path, NULL, NULL)
                 : ccol_not_enough_memory;
  ctls_ctx_release(c);
  atomic_store(&f->done, true);
  return NULL;
}

TEST(tls_files, a_fifo_path_fails_at_once_and_never_blocks) {
  /* A CA bundle or certificate path naming a FIFO without a writer is refused
     right away with ccol_http_tls_cert_load_failed. An open that waits blocks
     in open(2) until a writer shows up, and the test then finds the call still
     running after 5 s. Afterwards the test opens the FIFO for writing itself,
     so a blocked call ends either way. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  const char *fifo = _pki("fifo");
  unlink(fifo);
  REQUIRE_EQ(mkfifo(fifo, 0600), 0);
  _fifo_ctx_t f = {.path = fifo};
  atomic_init(&f.done, false);
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, _fifo_trust_thread, &f), 0);
  bool finished = false;
  for (int i = 0; i < 5000 && !finished; i++) {
    finished = atomic_load(&f.done);
    if (!finished) {
      struct timespec nap = {0, 1000000};
      nanosleep(&nap, NULL);
    }
  }
  /* Unblocks an open that waits for a writer, and then a read that waits for
     data, until the call ends. */
  while (!atomic_load(&f.done)) {
    int w = open(fifo, O_WRONLY | O_NONBLOCK);
    if (w >= 0) close(w);
    struct timespec nap = {0, 1000000};
    nanosleep(&nap, NULL);
  }
  pthread_join(tid, NULL);
  unlink(fifo);
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(f.trust_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(f.cert_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_TRUE(f.trust_why != NULL);
  REQUIRE_STREQ(f.trust_why, "path does not name a regular file");
}

/* ----- the identity of a verified client ----- */

/* Reads the first certificate of a PEM file into its DER encoding, its SHA-256
   digest and its RFC 2253 subject. */
static bool _expected_identity(const char *pem_path, unsigned char **der_out,
                               int *der_len_out, unsigned char sha[32],
                               char *subject, size_t subject_cap) {
  FILE *f = fopen(pem_path, "r");
  if (!f) return false;
  X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
  fclose(f);
  if (!x) return false;
  *der_out = NULL;
  *der_len_out = i2d_X509(x, der_out);
  unsigned int md_len = 0;
  bool ok = *der_len_out > 0 &&
            EVP_Digest(*der_out, (size_t)*der_len_out, sha, &md_len,
                       EVP_sha256(), NULL) == 1 &&
            md_len == 32;
  BIO *b = BIO_new(BIO_s_mem());
  ok = ok && b &&
       X509_NAME_print_ex(b, X509_get_subject_name(x), 0, XN_FLAG_RFC2253) >= 0;
  if (ok) {
    char *t = NULL;
    long n = BIO_get_mem_data(b, &t);
    ok = n >= 0 && (size_t)n < subject_cap;
    if (ok) {
      memcpy(subject, t, (size_t)n);
      subject[n] = '\0';
    }
  }
  BIO_free(b);
  X509_free(x);
  return ok;
}

TEST(tls_identity, a_verified_client_has_its_der_digest_and_subject) {
  /* The server sees the exact certificate of a verified client: its DER
     encoding, its SHA-256 fingerprint and its subject in RFC 2253 form with the
     comma inside a value escaped. Every call gives the same answer. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  unsigned char *want_der = NULL;
  int want_len = 0;
  unsigned char want_sha[32];
  char want_subject[256];
  bool have = _expected_identity(_pki("good.crt"), &want_der, &want_len,
                                 want_sha, want_subject, sizeof(want_subject));
  ctls_ctx_t *s = ctls_ctx_new(NULL);
  ctls_ctx_t *c = ctls_ctx_new(NULL);
  bool ok = have && s && c &&
            ctls_ctx_cert_add(s, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                ccol_success &&
            ctls_ctx_trust(s, _pki("b_root_crl.pem"), NULL) == ccol_success &&
            ctls_ctx_trust(c, g_cert_path, NULL) == ccol_success &&
            ctls_ctx_cert_add(c, NULL, _pki("good.crt"), _pki("good.key"), NULL,
                              NULL) == ccol_success;
  bool done = false, der_ok = false, sha_ok = false, again_ok = false;
  char subject[256] = {0};
  if (ok) {
    _tls_pair_t p;
    _tls_pair_open(&p, s, c, "127.0.0.1");
    done = p.done;
    size_t len = 0;
    const unsigned char *der =
        done ? ctls_conn_peer_cert_der(p.server, &len) : NULL;
    der_ok = der && len == (size_t)want_len && memcmp(der, want_der, len) == 0;
    unsigned char sha[32];
    sha_ok = done && ctls_conn_peer_cert_sha256(p.server, sha) &&
             memcmp(sha, want_sha, 32) == 0;
    const char *subj = done ? ctls_conn_peer_cert_subject(p.server) : NULL;
    if (subj) snprintf(subject, sizeof(subject), "%s", subj);
    size_t len2 = 0;
    again_ok = done && ctls_conn_peer_cert_der(p.server, &len2) == der &&
               len2 == len && ctls_conn_peer_cert_subject(p.server) == subj;
    _tls_pair_close(&p);
  }
  ctls_ctx_release(c);
  ctls_ctx_release(s);
  OPENSSL_free(want_der);
  REQUIRE_TRUE(have);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(done);
  REQUIRE_TRUE(der_ok);
  REQUIRE_TRUE(sha_ok);
  REQUIRE_STREQ(subject, want_subject);
  REQUIRE_STREQ(subject, "O=Example\\, Inc.,CN=crl-good");
  REQUIRE_TRUE(again_ok);
}

/* Checks that every identity accessor of conn reports nothing. */
static bool _identity_is_empty(ctls_conn_t *conn) {
  size_t len = 7;
  unsigned char sha[32];
  memset(sha, 0xAB, sizeof(sha));
  bool der_none = ctls_conn_peer_cert_der(conn, &len) == NULL && len == 0;
  bool sha_none = !ctls_conn_peer_cert_sha256(conn, sha);
  bool zeroed = true;
  for (int i = 0; i < 32; i++) zeroed = zeroed && sha[i] == 0;
  return der_none && sha_none && zeroed &&
         ctls_conn_peer_cert_subject(conn) == NULL;
}

TEST(tls_identity, nothing_is_reported_without_a_verified_certificate) {
  /* A connection whose peer has no verified certificate reports no identity at
     all. That covers an anonymous client of a server that only asks for a
     certificate, a client presenting a certificate to a server with no trust
     store (which verifies nothing), and the client side of a connection whose
     context has no trust store, which sees the server's certificate and doesn't
     verify it either. A NULL connection reports none. Accessors that reported
     the peer's certificate whether or not it verified would report one in the
     third case. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  /* 1. An anonymous client under client_cert_optional. */
  ctls_ctx_t *s1 = ctls_ctx_new(NULL);
  ctls_ctx_t *c1 = ctls_ctx_new(NULL);
  bool ok1 = s1 && c1 &&
             ctls_ctx_cert_add(s1, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                 ccol_success &&
             ctls_ctx_trust(s1, _pki("b_root_crl.pem"), NULL) == ccol_success &&
             ctls_ctx_peer_cert_optional(s1, true) == ccol_success &&
             ctls_ctx_trust(c1, g_cert_path, NULL) == ccol_success;
  bool done1 = false, empty1 = false;
  if (ok1) {
    _tls_pair_t p;
    _tls_pair_open(&p, s1, c1, "127.0.0.1");
    done1 = p.done;
    empty1 = done1 && _identity_is_empty(p.server);
    _tls_pair_close(&p);
  }
  ctls_ctx_release(c1);
  ctls_ctx_release(s1);
  /* 2. A client certificate presented to a server with no trust store. */
  ctls_ctx_t *s2 = ctls_ctx_new(NULL);
  ctls_ctx_t *c2 = ctls_ctx_new(NULL);
  bool ok2 = s2 && c2 &&
             ctls_ctx_cert_add(s2, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                 ccol_success &&
             ctls_ctx_trust(c2, g_cert_path, NULL) == ccol_success &&
             ctls_ctx_cert_add(c2, NULL, _pki("good.crt"), _pki("good.key"),
                               NULL, NULL) == ccol_success;
  bool done2 = false, empty2 = false;
  if (ok2) {
    _tls_pair_t p;
    _tls_pair_open(&p, s2, c2, "127.0.0.1");
    done2 = p.done;
    empty2 = done2 && _identity_is_empty(p.server);
    _tls_pair_close(&p);
  }
  ctls_ctx_t *s4 = ctls_ctx_new(NULL);
  ctls_ctx_t *c4 = ctls_ctx_new(NULL);
  bool ok4 = s4 && c4 &&
             ctls_ctx_cert_add(s4, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                 ccol_success;
  bool done4 = false, empty4 = false, seen4 = false;
  if (ok4) {
    _tls_pair_t p;
    _tls_pair_open(&p, s4, c4, NULL);
    done4 = p.done;
    if (done4) {
      /* LibreSSL and OpenSSL 1.1.1 spell the call that takes a reference
       * without the 1, and OpenSSL 3 deprecates that name. */
      SSL *cs = _ctls_conn_ssl_for_tests(p.client);
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
      X509 *peer = SSL_get_peer_certificate(cs);
#else
      X509 *peer = SSL_get1_peer_certificate(cs);
#endif
      seen4 = peer != NULL;
      X509_free(peer);
    }
    empty4 = done4 && _identity_is_empty(p.client);
    _tls_pair_close(&p);
  }
  ctls_ctx_release(c4);
  ctls_ctx_release(s4);
  ctls_ctx_release(c2);
  ctls_ctx_release(s2);
  /* 3. A NULL connection. */
  bool empty3 = _identity_is_empty(NULL);
  REQUIRE_TRUE(ok1);
  REQUIRE_TRUE(done1);
  REQUIRE_TRUE(empty1);
  REQUIRE_TRUE(ok2);
  REQUIRE_TRUE(done2);
  REQUIRE_TRUE(empty2);
  REQUIRE_TRUE(empty3);
  REQUIRE_TRUE(ok4);
  REQUIRE_TRUE(done4);
  REQUIRE_TRUE(seen4);
  REQUIRE_TRUE(empty4);
}

/* Drives a ctls server connection and a raw OpenSSL client, which offers *sess
 * when it isn't NULL and gets this connection's session back. Reports the DER
 * length and the subject that the server sees, and whether the server resumed.
 */
static bool _identity_resume_connect(ctls_ctx_t *server_ctx,
                                     SSL_CTX *client_ctx, SSL_SESSION **sess,
                                     size_t *der_len, char *subject,
                                     size_t subject_cap, bool *reused) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  for (int i = 0; i < 2; i++)
    fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
  ctls_conn_t *server = ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  SSL *client = SSL_new(client_ctx);
  bool ok = server && client && SSL_set_fd(client, fds[1]) == 1;
  if (ok && *sess) ok = SSL_set_session(client, *sess) == 1;
  bool s_done = false, c_done = false;
  if (ok) SSL_set_connect_state(client);
  for (int i = 0; ok && i < 2000 && !(s_done && c_done); i++) {
    if (!s_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(server);
      if (r == CTLS_HANDSHAKE_ERROR) ok = false;
      s_done = r == CTLS_HANDSHAKE_DONE;
    }
    if (ok && !c_done) {
      int r = SSL_do_handshake(client);
      if (r == 1) {
        c_done = true;
      } else {
        int e = SSL_get_error(client, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ok = false;
      }
    }
  }
  ok = ok && s_done && c_done;
  /* One byte from the server makes a TLS 1.3 client take in its tickets. */
  bool got = false;
  for (int i = 0; ok && i < 2000 && !got; i++) {
    if (i == 0 && ctls_conn_write(server, "x", 1) != 1) ok = false;
    char b;
    int r = SSL_read(client, &b, 1);
    if (r == 1) {
      got = true;
    } else {
      int e = SSL_get_error(client, r);
      if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ok = false;
    }
  }
  ok = ok && got;
  if (ok) {
    *der_len = 0;
    (void)ctls_conn_peer_cert_der(server, der_len);
    const char *subj = ctls_conn_peer_cert_subject(server);
    snprintf(subject, subject_cap, "%s", subj ? subj : "(none)");
    *reused = SSL_session_reused(client) == 1;
    SSL_SESSION *next = SSL_get1_session(client);
    if (*sess) SSL_SESSION_free(*sess);
    *sess = next;
  }
  if (client) {
    SSL_shutdown(client);
    SSL_free(client);
  }
  ctls_conn_destroy(server);
  close(fds[0]);
  close(fds[1]);
  return ok;
}

TEST(tls_identity, a_resumed_session_reports_the_same_identity) {
  /* A client that resumes its session presents no certificate on the wire, but
     the server still reports the identity that the original handshake verified,
     for TLS 1.2 and TLS 1.3. LibreSSL resumes no TLS 1.3 session, so there we
     check only TLS 1.2. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  const int versions[2] = {TLS1_2_VERSION, TLS1_3_VERSION};
  int failed = 0;
  for (int v = 0; v < 2; v++) {
#if defined(LIBRESSL_VERSION_NUMBER)
    if (versions[v] == TLS1_3_VERSION) continue;
#endif
    ctls_ctx_t *s = ctls_ctx_new(NULL);
    bool ok = s &&
              ctls_ctx_cert_add(s, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                  ccol_success &&
              ctls_ctx_trust(s, _pki("b_root_crl.pem"), NULL) == ccol_success;
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    ok = ok && cctx && SSL_CTX_set_min_proto_version(cctx, versions[v]) == 1 &&
         SSL_CTX_set_max_proto_version(cctx, versions[v]) == 1 &&
         SSL_CTX_use_certificate_file(cctx, _pki("good.crt"),
                                      SSL_FILETYPE_PEM) == 1 &&
         SSL_CTX_use_PrivateKey_file(cctx, _pki("good.key"),
                                     SSL_FILETYPE_PEM) == 1;
    SSL_SESSION *sess = NULL;
    size_t len1 = 0, len2 = 0;
    char subj1[256] = {0}, subj2[256] = {0};
    bool reused1 = true, reused2 = false;
    ok = ok && _identity_resume_connect(s, cctx, &sess, &len1, subj1,
                                        sizeof(subj1), &reused1);
    ok = ok && _identity_resume_connect(s, cctx, &sess, &len2, subj2,
                                        sizeof(subj2), &reused2);
    if (sess) SSL_SESSION_free(sess);
    if (cctx) SSL_CTX_free(cctx);
    ctls_ctx_release(s);
    if (!(ok && !reused1 && reused2 && len1 > 0 && len2 == len1 &&
          strcmp(subj1, "O=Example\\, Inc.,CN=crl-good") == 0 &&
          strcmp(subj2, subj1) == 0))
      failed |= 1 << v;
  }
  REQUIRE_EQ(failed, 0);
}

/* Answers with what chttpserver's identity accessors report. */
static void _identity_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t len = 7;
  const void *der = chttpsvr_req_peer_cert_der(req, &len);
  unsigned char sha[CHTTPSVR_PEER_CERT_SHA256_LEN];
  memset(sha, 0xAB, sizeof(sha));
  ccol_retval_t rv = chttpsvr_req_peer_cert_sha256(req, sha);
  char hex[2 * CHTTPSVR_PEER_CERT_SHA256_LEN + 1];
  for (int i = 0; i < CHTTPSVR_PEER_CERT_SHA256_LEN; i++)
    snprintf(hex + 2 * i, 3, "%02x", sha[i]);
  const char *subj = chttpsvr_req_peer_cert_subject(req);
  chttpsvr_resp_printf(resp, "der=%d len=%zu rv=%d sha=%s subj=%s", der != NULL,
                       len, (int)rv, hex, subj ? subj : "(none)");
}

TEST(tls_identity, a_handler_sees_the_identity_of_its_client) {
  /* Through chttpserver: a verified client is reported with the length of its
     certificate, its fingerprint and its subject, and under
     client_cert_optional an anonymous client is reported with none of them,
     ccol_key_not_found and a digest buffer left all zeroes. */
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  unsigned char *want_der = NULL;
  int want_len = 0;
  unsigned char want_sha[32];
  char want_subject[256];
  bool have = _expected_identity(_pki("good.crt"), &want_der, &want_len,
                                 want_sha, want_subject, sizeof(want_subject));
  OPENSSL_free(want_der);
  char want_hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(want_hex + 2 * i, 3, "%02x", want_sha[i]);
  char want_body[512];
  snprintf(want_body, sizeof(want_body), "der=1 len=%d rv=0 sha=%s subj=%s",
           want_len, want_hex, want_subject);
  char want_anon[256];
  snprintf(want_anon, sizeof(want_anon),
           "der=0 len=0 rv=%d sha=%064d subj=(none)", (int)ccol_key_not_found,
           0);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _identity_handler, NULL),
             ccol_success);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = _pki("b_root_crl.pem");
  tls.client_cert_optional = true;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 41;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1, anon_status = -1;
  char body[512], anon[512];
  ccol_retval_t rv = _mtls_get(TLS_TEST_PORT + 41, _pki("good.crt"),
                               _pki("good.key"), &status, body, sizeof(body));
  ccol_retval_t anon_rv = _mtls_get(TLS_TEST_PORT + 41, NULL, NULL,
                                    &anon_status, anon, sizeof(anon));
  REQUIRE_TRUE(have);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, want_body);
  REQUIRE_EQ(anon_rv, ccol_success);
  REQUIRE_EQ(anon_status, 200);
  REQUIRE_STREQ(anon, want_anon);
}

TEST(tls_identity, null_arguments_report_nothing) {
  /* The accessors refuse NULL arguments without crashing. */
  size_t len = 7;
  unsigned char sha[CHTTPSVR_PEER_CERT_SHA256_LEN];
  memset(sha, 0xAB, sizeof(sha));
  REQUIRE_TRUE(chttpsvr_req_peer_cert_der(NULL, &len) == NULL);
  REQUIRE_EQ(len, (size_t)0);
  REQUIRE_TRUE(chttpsvr_req_peer_cert_der(NULL, NULL) == NULL);
  REQUIRE_EQ(chttpsvr_req_peer_cert_sha256(NULL, sha), ccol_invalid_args);
  bool zeroed = true;
  for (int i = 0; i < CHTTPSVR_PEER_CERT_SHA256_LEN; i++)
    zeroed = zeroed && sha[i] == 0;
  REQUIRE_TRUE(zeroed);
  REQUIRE_EQ(chttpsvr_req_peer_cert_sha256(NULL, NULL), ccol_invalid_args);
  REQUIRE_TRUE(chttpsvr_req_peer_cert_subject(NULL) == NULL);
}

/* An allocator that counts the blocks it has handed out and not yet taken
   back. */
static _Atomic long g_tls_live = 0;
static void *_tls_count_malloc(size_t n) {
  void *p = malloc(n);
  if (p) atomic_fetch_add(&g_tls_live, 1);
  return p;
}
static void *_tls_count_calloc(size_t n, size_t s) {
  void *p = calloc(n, s);
  if (p) atomic_fetch_add(&g_tls_live, 1);
  return p;
}
static void *_tls_count_realloc(void *p, size_t s) {
  void *np = realloc(p, s);
  if (np && !p) atomic_fetch_add(&g_tls_live, 1);
  return np;
}
static void _tls_count_free(void *p) {
  if (p) atomic_fetch_sub(&g_tls_live, 1);
  free(p);
}

/* The last frees of a server that was ever started run on the reactor
   thread after chttpsvr_destroy() has returned, once its registrations are
   reclaimed. Waits up to 10 s for g_tls_live to reach 0 and returns what it
   reads last. */
static long _tls_wait_for_no_live_blocks(void) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  long live = atomic_load(&g_tls_live);
  while (live != 0) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 10) break;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
    live = atomic_load(&g_tls_live);
  }
  return live;
}

/* A TLS server that stops and starts again builds its TLS context afresh,
   gives the old one back and keeps verifying client certificates, and a
   second TLS server on the port that the first one holds fails its start
   with ccol_unexpected_failure, gives back what that start built, and
   leaves the first one serving. */
TEST(chttpserver_tls,
     a_restarted_tls_server_serves_and_a_busy_port_is_refused) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  ccol_memmgmt_procs_t mp = {_tls_count_malloc, _tls_count_free,
                             _tls_count_calloc, _tls_count_realloc};
  atomic_store(&g_tls_live, 0);
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/peer", _peer_cert_handler, NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = g_client_cert_path;
  tls.client_cert_optional = true;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.tls = &tls;
  uint16_t port = 0;
  int status_first = -1, status_restart = -1, status_after_busy = -1;
  char body_first[64] = "", body_restart[64] = "", body_after_busy[64] = "";
  ccol_retval_t first_rv = chttpsvr_start(srv, &cfg);
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  ccol_retval_t busy_rv = ccol_success;
  if (first_rv == ccol_success &&
      chttpsvr_bound_port(srv, &port) == ccol_success) {
    _mtls_get(port, g_client_cert_path, g_client_key_path, &status_first,
              body_first, sizeof(body_first));
    chttpsvr_stop(srv);
    cfg.port = port;
    restart_rv = chttpsvr_start(srv, &cfg);
    if (restart_rv == ccol_success)
      _mtls_get(port, g_client_cert_path, g_client_key_path, &status_restart,
                body_restart, sizeof(body_restart));
    chttpsvr other = chttpsvr_create_mp(&mp, g_test_logger, NULL);
    if (other != CHTTPSVR_NULL) {
      busy_rv = chttpsvr_start(other, &cfg);
      chttpsvr_destroy(other);
    }
    _mtls_get(port, g_client_cert_path, g_client_key_path, &status_after_busy,
              body_after_busy, sizeof(body_after_busy));
  }
  chttpsvr_destroy(srv);
  long live = _tls_wait_for_no_live_blocks();
  REQUIRE_EQ((int)first_rv, (int)ccol_success);
  REQUIRE_EQ(status_first, 200);
  REQUIRE_STREQ(body_first, "verified=1");
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_EQ(status_restart, 200);
  REQUIRE_STREQ(body_restart, "verified=1");
  REQUIRE_EQ((int)busy_rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ(status_after_busy, 200);
  REQUIRE_STREQ(body_after_busy, "verified=1");
  REQUIRE_EQ(live, 0L);
}
