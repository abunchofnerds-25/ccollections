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

// Ignores SIGPIPE and resets the rest of the inherited signal state before any
// thread starts. The test clients write with plain write(2) and the server can
// close a connection during one; tests_sigpipe.c covers the default
// disposition.
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

// Real TLS handshakes need a valid certificate and key, so this binary makes a
// throwaway self-signed pair with the openssl CLI at startup, starts a real TLS
// listener and drives real HTTPS requests through it. It's a binary of its own
// so that a broken openssl CLI fails only this suite.

// The port and URL of the shared TLS server, which every client test calls.
#define TLS_TEST_PORT 18790
#define BASE_URL "https://127.0.0.1:18790"
// The same server by a name that the certificate (CN=127.0.0.1) doesn't
// cover, for the host name checks. "localhost" resolves to the same loopback
// server.
#define BASE_URL_MISMATCHED_HOST "https://localhost:18790"

// The shared server, its logger and the files of the generated certificates:
// the server's pair, a client identity and a second client identity that no
// server trusts. g_cert_ready is false when the openssl CLI failed, and the
// tests that need certificates then skip.
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

// Answers "Hello, TLS!"; the route that proves a handshake got through.
static void _hello_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, TLS!");
}

// Answers with the length of the body it got, as decimal text, so a test can
// check that a body read through the TLS layer arrived whole.
static void _body_len_tls_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)ctx;
  size_t len = 0;
  (void)chttpsvr_req_body(req, &len);
  chttpsvr_resp_printf(resp, "len=%zu", len);
}

// Generates a self-signed certificate and key for cn with the openssl CLI, into
// key_path and cert_path; the server and client identities of the tests come
// from it. Gives 0 on success and -1 on any failure, which the caller must read
// as "this environment can't check TLS". The certificate carries a
// subjectAltName iPAddress entry for an IP cn, because ctls verifies an IP
// target with X509_VERIFY_PARAM_set1_ip_asc and never falls back to the CN.
static int _openssl_selfsigned(const char *key_path, const char *cert_path,
                               const char *cn, const char *san) {
  char cmd[1024];
  int cn_len;
  if (san) {
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

// Makes the certificate directory and the three pairs the tests use: The
// server's, the client identity for the mutual TLS tests (real files, so
// ctls_ctx_cert_add runs end to end) and a second client identity that no
// server trusts. It fills g_cert_dir and the path globals and gives 0, or -1 on
// a failure.
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
  if (_openssl_selfsigned(g_client_key_path, g_client_cert_path,
                          "chttpclient-test-client", NULL) != 0)
    return -1;
  if (_openssl_selfsigned(g_other_key_path, g_other_cert_path,
                          "chttpclient-untrusted-client", NULL) != 0)
    return -1;
  return 0;
}

// Removes the generated certificate files and their directory, so the binary
// leaves nothing behind.
static void _remove_generated_cert(void) {
  if (g_cert_path[0]) unlink(g_cert_path);
  if (g_key_path[0]) unlink(g_key_path);
  if (g_client_cert_path[0]) unlink(g_client_cert_path);
  if (g_client_key_path[0]) unlink(g_client_key_path);
  if (g_other_cert_path[0]) unlink(g_other_cert_path);
  if (g_other_key_path[0]) unlink(g_other_key_path);
  if (g_cert_dir[0]) rmdir(g_cert_dir);
}

// Destroys g_tls_srv, waits with chttpsvr_engine_wait() so the engine gives
// back its logger, then closes g_test_logger and removes the certificates. It's
// the atexit handler, so nothing of the server is left at exit. On OpenBSD the
// handler runs while the loader holds a lock the join needs, so there it skips
// the wait and leaves the logger open.
static void _teardown(void) {
  if (g_tls_srv) chttpsvr_stop(g_tls_srv);
  if (g_tls_srv) {
    chttpsvr__destroy(g_tls_srv);
    g_tls_srv = CHTTPSVR_NULL;
  }
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_NULL;
  }
#endif
  _remove_generated_cert();
}

// Generates the certificates and starts g_tls_srv on TLS_TEST_PORT with /hello
// and /body-len, the server the handshake tests share. Without the openssl CLI
// it only warns and leaves g_cert_ready false, and those tests skip. Other
// failures end the process. The teardown is registered after the start, so
// g_tls_srv is set when it runs.
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

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT;
  cfg.tls = &tls;

  ccol_retval rv = chttpsvr_start(g_tls_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: TLS chttpsvr_start failed: %d\n", rv);
    exit(1);
  }

  atexit(_teardown);
}

TEST(chttpserver_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

// The default trust store must reject our self-signed certificate, which shows
// the server does a real, verifiable handshake.
TEST(chttpserver_tls, handshake_fails_when_ca_is_untrusted) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttp_request *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(chttp_default_client(), req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_FALSE(resp_present);
}

// Connects through "localhost", which doesn't match CN=127.0.0.1, with the CA
// trusted, so a rejection can only come from the host name check.
TEST(chttpserver_tls, hostname_mismatch_rejected_by_default) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req = chttp_request_create(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_FALSE(resp_present);
}

// insecure_skip_hostname_check skips only the host name match; the chain check
// it leaves in place accepts the pinned certificate.
TEST(chttpserver_tls, hostname_mismatch_allowed_when_hostname_check_skipped) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.insecure_skip_hostname_check = true;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req = chttp_request_create(
      CHTTP_GET, BASE_URL_MISMATCHED_HOST "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

// This server neither requires nor verifies a client certificate. The test
// shows that the client's cert_path/key_path load a real pair and don't break a
// normal handshake.
TEST(chttpserver_tls, client_presents_certificate_mtls_smoke) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.cert_path = g_client_cert_path;
  tls.key_path = g_client_key_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, BASE_URL "/hello", NULL, NULL);
  REQUIRE_TRUE(req != NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  bool resp_present = resp != NULL;
  int status_code = resp_present ? resp->status_code : -1;
  bool body_present = resp_present && resp->body != NULL;
  bool body_matches = body_present && strcmp(resp->body, "Hello, TLS!") == 0;
  if (resp_present) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(resp_present);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_TRUE(body_present);
  REQUIRE_TRUE(body_matches);
}

// Mutual TLS: A server whose ca_bundle_path trusts this suite's self-signed
// client certificate (its own CA). Each test starts such a server and drives
// one request with a client that presents the trusted identity, an untrusted
// one, or nothing.

// Answers with what chttpsvr_req_peer_cert_verified reports, so the mutual TLS
// tests can see what the server decided about the client.
static void _peer_cert_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  chttpsvr_resp_printf(resp, "verified=%d",
                       chttpsvr_req_peer_cert_verified(req) ? 1 : 0);
}

// Starts a mutual TLS server on port for one test, trusting the client
// identity; client_cert_optional picks the request-only mode. Gives the server,
// or CHTTPSVR_NULL on any failure.
static chttpsvr _mtls_server(uint16_t port, bool client_cert_optional) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_GET, "/peer", _peer_cert_handler, NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = g_client_cert_path;
  tls.client_cert_optional = client_cert_optional;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  cfg.tls = &tls;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

// Sends GET /peer to 127.0.0.1:port over TLS, trusting the server certificate
// and presenting cert_path and key_path when they aren't NULL; each mutual TLS
// test makes one such request. Gives the result of chttpcli_do and, on
// success, the status and body in *status_out and body_out.
static ccol_retval _mtls_get(uint16_t port, const char *cert_path,
                             const char *key_path, int *status_out,
                             char *body_out, size_t body_cap) {
  *status_out = -1;
  body_out[0] = '\0';
  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL) return ccol_not_enough_memory;
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  tls.cert_path = cert_path;
  tls.key_path = key_path;
  ccol_retval rv = chttpcli_set_tls(cli, &tls);
  if (rv != ccol_success) return rv;
  char url[128];
  snprintf(url, sizeof(url), "https://127.0.0.1:%u/peer", (unsigned)port);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  chttpcli_response *resp = NULL;
  rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  if (resp) {
    *status_out = resp->status_code;
    if (resp->body) snprintf(body_out, body_cap, "%s", resp->body);
    chttpcli_resp_free(resp);
  }
  return rv;
}

// A ca_bundle_path requires a client certificate by default. A server that only
// requested one would answer "verified=0".
TEST(chttpserver_tls, mtls_refuses_a_client_without_a_certificate) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _mtls_server(TLS_TEST_PORT + 20, false);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int status = -1;
  char body[64];
  ccol_retval rv =
      _mtls_get(TLS_TEST_PORT + 20, NULL, NULL, &status, body, sizeof(body));
  REQUIRE_NE(rv, ccol_success);
  REQUIRE_EQ(status, -1);
}

TEST(chttpserver_tls, mtls_accepts_and_reports_a_verified_client) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  for (int optional = 0; optional < 2; optional++) {
    chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
        _mtls_server(TLS_TEST_PORT + 21, optional != 0);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    int status = -1;
    char body[64];
    ccol_retval rv = _mtls_get(TLS_TEST_PORT + 21, g_client_cert_path,
                               g_client_key_path, &status, body, sizeof(body));
    chttpsvr_destroy(srv);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_STREQ(body, "verified=1");
  }
}

// client_cert_optional relaxes only a missing certificate, so a certificate
// that doesn't verify fails in both modes.
TEST(chttpserver_tls, mtls_refuses_a_certificate_from_another_ca) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  for (int optional = 0; optional < 2; optional++) {
    chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
        _mtls_server(TLS_TEST_PORT + 22, optional != 0);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    int status = -1;
    char body[64];
    ccol_retval rv = _mtls_get(TLS_TEST_PORT + 22, g_other_cert_path,
                               g_other_key_path, &status, body, sizeof(body));
    chttpsvr_destroy(srv);
    REQUIRE_NE(rv, ccol_success);
    REQUIRE_EQ(status, -1);
  }
}

// An accessor that reported the verify result alone would answer "verified=1",
// because OpenSSL reports X509_V_OK for a peer that sent nothing.
TEST(chttpserver_tls, mtls_optional_accepts_an_anonymous_client_unverified) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _mtls_server(TLS_TEST_PORT + 23, true);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int status = -1;
  char body[64];
  ccol_retval rv =
      _mtls_get(TLS_TEST_PORT + 23, NULL, NULL, &status, body, sizeof(body));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=0");
}

TEST(chttpserver_tls, peer_cert_verified_is_false_without_mutual_tls) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _peer_cert_handler, NULL),
             ccol_success);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 24;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1;
  char body[64];
  ccol_retval rv = _mtls_get(TLS_TEST_PORT + 24, g_client_cert_path,
                             g_client_key_path, &status, body, sizeof(body));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=0");
}

// chttpsvr_start must never start plain HTTP when cfg.tls is set but
// incomplete, and must never ignore a CA bundle it can't load, because either
// would return ccol_success with no way for the caller to notice. These tests
// pin the errors. The incomplete configurations are refused before any TLS
// work, so they need no certificate.
TEST(chttpserver_tls, start_rejects_cert_path_without_key_path) {
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/cert.pem";
  tls.key_path = NULL;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 1;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_destroy(srv);
}

TEST(chttpserver_tls, start_rejects_key_path_without_cert_path) {
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = NULL;
  tls.key_path = "/nonexistent/key.pem";
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 2;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_destroy(srv);
}

// A ca_bundle_path alone is how a client is configured (chttpcli_set_tls, like
// curl's --cacert), so a caller who brings that habit to the server makes a
// realistic mistake. Ignoring it would serve plain HTTP on a port the caller
// believes is HTTPS with mutual TLS. The refused start leaves no listener, so a
// plain start on the same port succeeds.
TEST(chttpserver_tls, start_rejects_ca_bundle_path_without_cert_key_pair) {
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = NULL;
  tls.key_path = NULL;
  tls.ca_bundle_path = "/nonexistent/ca-bundle.pem";
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 4;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_config plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 4;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

// CHTTP_TLS_DEFAULT has exactly this shape (no path at all), so a caller who
// forgets to set the paths makes a realistic mistake. No single pairing check
// fires when everything is missing. The refused start leaves no listener.
TEST(chttpserver_tls, start_rejects_tls_config_with_no_cert_or_key) {
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 5;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_invalid_args);

  chttpsvr_config plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 5;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

// An empty trust store would refuse every client, or under
// client_cert_optional accept only anonymous ones; either way not what the
// caller asked for. The failed start leaves no listener.
TEST(chttpserver_tls, start_rejects_a_ca_bundle_holding_no_certificate) {
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  if (srv == CHTTPSVR_NULL) {
    unlink(bundle);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
    return;
  }

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = bundle;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 6;
  cfg.tls = &tls;

  ccol_retval started = chttpsvr_start(srv, &cfg);

  chttpsvr_config plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 6;
  ccol_retval reused = chttpsvr_start(srv, &plain_cfg);

  chttpsvr_destroy(srv);
  unlink(bundle);

  REQUIRE_EQ(started, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(reused, ccol_success);
}

// A missing certificate file and a key that doesn't match both give
// ccol_http_tls_cert_load_failed and leave the server able to start again.
TEST(chttpserver_tls, start_reports_a_certificate_that_cannot_be_loaded) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/server.pem";
  tls.key_path = g_key_path;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 7;
  cfg.tls = &tls;
  ccol_retval missing = chttpsvr_start(srv, &cfg);
  tls.cert_path = g_key_path;
  ccol_retval not_a_cert = chttpsvr_start(srv, &cfg);
  chttpsvr_config plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 7;
  ccol_retval plain = chttpsvr_start(srv, &plain_cfg);
  REQUIRE_EQ(missing, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(not_a_cert, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(plain, ccol_success);
}

// A valid pair but a CA bundle that can't be loaded fails the start instead of
// serving without the mutual TLS the caller asked for. The failed start leaves
// no listener.
TEST(chttpserver_tls, start_rejects_unloadable_ca_bundle_path) {
  if (!g_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this "
            "environment\n");
    return;
  }

  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = "/nonexistent/ca-bundle.pem";
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 3;
  cfg.tls = &tls;

  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_http_tls_cert_load_failed);

  chttpsvr_config plain_cfg = CHTTPSVR_CONFIG_DEFAULT;
  plain_cfg.host = "127.0.0.1";
  plain_cfg.port = TLS_TEST_PORT + 3;
  REQUIRE_EQ(chttpsvr_start(srv, &plain_cfg), ccol_success);

  chttpsvr_destroy(srv);
}

// A raw OpenSSL client on a blocking socket with a receive timeout, so a test
// controls how many bytes go into each TLS record.
typedef struct {
  SSL_CTX *ctx;
  SSL *ssl;
  int fd;
} _raw_tls_client_t;

// Shuts the raw client's connection down and frees everything, leaving c
// empty, at the end of a test.
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

// Connects the raw client to the shared server and finishes the handshake, for
// the tests that write their own records. Tells whether it worked.
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

// Writes all of buf over the raw client, so a test controls the records it
// sends. Each SSL_write of at most 16384 bytes is one TLS record, so a buffer
// of that size or less arrives as one record. Tells whether it all went out.
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

// Reads from the raw client until the peer closes, the receive timeout fires or
// out is full, so a test gets every reply of a pipelined batch. Gives the
// number of bytes read.
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

// Counts the non-overlapping occurrences of needle in hay, so a test can count
// the responses it got.
static size_t _count_occurrences(const char *hay, const char *needle) {
  size_t n = 0;
  for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

// 70 GETs of about 440 bytes go out as two records of 16 KB and 14 KB. The
// server reads 8 KB at a time, so once it reads into the second record the
// socket is empty while the rest of the record sits in the TLS layer, where
// epoll can't see it. Every request must still be answered without another byte
// from the client.
TEST(chttpserver_tls, pipelined_requests_in_one_record_are_all_answered) {
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

// One record holds a POST head, its 9000-byte body and a second request. The
// server reads 8 KB first, so most of the body and the second request are in
// the TLS layer when the POST reaches its worker.
TEST(chttpserver_tls, body_and_next_request_in_the_tls_buffer_are_served) {
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

// Gives CLOCK_MONOTONIC in milliseconds, for the deadlines of these tests.
static long long _tls_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000L;
}

// Polls the parked-body count of server h until it reaches want, for 5 s, so a
// test knows its slow clients have parked. Tells whether it got there.
static bool _tls_wait_parked_bodies(chttpsvr h, size_t want) {
  long long deadline = _tls_now_ms() + 5000;
  while (chttpsvr__parked_count_for_tests(h, 1) < want) {
    if (_tls_now_ms() > deadline) return false;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return true;
}

// The TLS twin of slow_clients.slow_bodies_park_and_hold_no_worker_thread in
// tests.c: Four clients stop in the middle of their bodies, the connections
// park without a thread, and a fresh request is served.
TEST(chttpserver_tls, slow_tls_bodies_park_and_hold_no_worker_thread) {
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
  size_t active = chttpsvr__pool_active_for_tests(g_tls_srv, false);
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

// A TLS client whose records the test writes to the socket itself, so it can
// stop in the middle of one. Reads go through the socket; writes go into a
// memory BIO that _split_flush moves to the socket.
typedef struct {
  SSL_CTX *ctx;
  SSL *ssl;
  BIO *wbio;
  int fd;
} _split_tls_client_t;

// Moves at most max bytes of pending records from the memory BIO to the socket,
// so a test can stop in the middle of a record. Tells whether every write
// worked.
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

// Frees the split client and leaves c empty, at the end of a test.
static void _split_close(_split_tls_client_t *c) {
  if (c->ssl) SSL_free(c->ssl);
  if (c->ctx) SSL_CTX_free(c->ctx);
  if (c->fd >= 0) close(c->fd);
  c->ssl = NULL;
  c->ctx = NULL;
  c->fd = -1;
}

// Connects the split client to the shared server, non-blocking, and finishes
// the handshake within 5 s, flushing the memory BIO after each step. Tells
// whether it worked.
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

// Reads one TLS response over the split client, for at most timeout_ms, and
// tells whether it holds needle; the split-record test checks its reply so.
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

// A parked body sets SO_RCVLOWAT. The plaintext still expected is a safe mark
// only while OpenSSL holds no part of a record. Here the 5000-byte body travels
// in one record and the client holds back its last 10 bytes: Enough to reach
// the mark and wake the reactor, not enough to decrypt. The server reads the
// partial record into OpenSSL and parks again, and the last 10 bytes must still
// wake it. A mark of 5000 taken from the plaintext alone waits for ever.
TEST(chttpserver_tls, a_record_split_on_the_wire_still_wakes_the_reader) {
  if (!g_cert_ready) return;
  _split_tls_client_t c = {.fd = -1};
  bool opened = _split_open(&c);
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /body-len HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: 5000\r\n\r\n");
  size_t parks0 = chttpsvr__park_count_for_tests();
  bool sent_head =
      opened && SSL_write(c.ssl, head, hn) == hn && _split_flush(&c, SIZE_MAX);
  bool first_park = sent_head && _tls_wait_parked_bodies(g_tls_srv, 1);
  static char body[5000];
  memset(body, 'z', sizeof(body));
  bool staged = first_park &&
                SSL_write(c.ssl, body, (int)sizeof(body)) == (int)sizeof(body);
  size_t record = staged ? BIO_ctrl_pending(c.wbio) : 0;
  size_t parks1 = chttpsvr__park_count_for_tests();
  bool partial = staged && record > 5010 && _split_flush(&c, record - 10);
  bool second_park = false;
  long long deadline = _tls_now_ms() + 5000;
  while (partial && !second_park && _tls_now_ms() < deadline) {
    second_park = chttpsvr__park_count_for_tests() > parks1 &&
                  chttpsvr__parked_count_for_tests(g_tls_srv, 1) == 1;
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

// Sends req over c and reads one whole response into buf, for the write-resume
// test that compares two replies. Gives its length, or 0.
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

// The TLS twin of slow_clients.a_write_cut_inside_the_head_or_the_body_
// resumes_intact in tests.c: A write that stops inside the head or the body
// resumes from the exact byte.
TEST(chttpserver_tls, a_tls_write_cut_inside_the_head_or_the_body_resumes) {
  if (!g_cert_ready) return;
  chttpsvr__set_date_override_for_tests(1700000000);
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
    chttpsvr__force_short_response_write_for_tests(cuts[i]);
    size_t n = _tls_exchange(&c, get, got, sizeof(got));
    if (n == ref_len && memcmp(got, ref, n) == 0) intact++;
  }
  _raw_tls_client_close(&c);
  chttpsvr__set_date_override_for_tests(INT64_MIN);
  REQUIRE_TRUE(opened);
  REQUIRE_GT(ref_len, body);
  REQUIRE_EQ(intact, 2);
}

// Waits up to 5 s for the server's lingering-close counter to pass before, and
// tells whether it did. The counter moves on the closing thread, possibly after
// the client saw the end of the stream.
static bool _linger_count_passes(size_t before) {
  for (int i = 0; i < 5000; i++) {
    if (chttpsvr__linger_count_for_tests() > before) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

// The TLS twin of lingering_close.a_refused_upload_gets_its_response_and_
// an_orderly_end in tests.c. The server refuses an upload with 404 and never
// reads its body; the client reads the response, the close_notify and an
// orderly end of the stream. A close with the body unread would send a reset,
// which the read after the close_notify reports as ECONNRESET.
TEST(chttpserver_tls, a_refused_upload_ends_with_close_notify_and_no_reset) {
  if (!g_cert_ready) return;
  size_t lingers = chttpsvr__linger_count_for_tests();
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

// Revocation lists, unreadable paths and the identity of a client. These tests
// drive ctls directly over a socketpair, and chttpserver over real connections.
// A small PKI made by the openssl CLI on first use holds a root CA, an
// intermediate CA, a client certificate from each, a revoked client
// certificate, a revoked server certificate and a CRL of each CA.

#include <internal/ctls.h>
#include <internal/test_helpers/ctls.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

// The PKI directory, and whether it was made (0 untried, 1 ready, -1
// failed).
static char g_pki_dir[400];
static int g_pki_state = 0;

// Every file the PKI script can create, for the cleanup.
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

// Removes the PKI files and directory; it's an atexit handler, so the binary
// leaves nothing behind.
static void _pki_remove(void) {
  if (!g_pki_dir[0]) return;
  char path[512];
  for (size_t i = 0; i < sizeof(g_pki_files) / sizeof(g_pki_files[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", g_pki_dir, g_pki_files[i]);
    unlink(path);
  }
  rmdir(g_pki_dir);
}

// The shell script that builds the PKI: The two CAs, the leaf certificates,
// the revocations, the CRLs and the CA bundles the tests use.
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

// Makes the PKI once, on first use by a test that needs it, by writing and
// running g_pki_script. It sets g_pki_dir and g_pki_state. Gives false when the
// openssl CLI is missing or fails, and the tests that need the PKI then skip.
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

// Gives the path of the PKI file name, so tests can name files short. It
// rotates through 8 static buffers, so up to 8 results can be used together.
static const char *_pki(const char *name) {
  static char paths[8][512];
  static int next = 0;
  char *p = paths[next];
  next = (next + 1) % 8;
  snprintf(p, sizeof(paths[0]), "%s/%s", g_pki_dir, name);
  return p;
}

// A ctls server connection and a ctls client connection over a socketpair.
// done is true when both handshakes finished.
typedef struct {
  int fds[2];
  ctls_conn *server;
  ctls_conn *client;
  bool done;
} _tls_pair_t;

// Opens a _tls_pair_t over a socketpair (client verifying host when it isn't
// NULL) and drives both handshakes to the end, so the ctls tests get a pair
// without a server. p->done tells whether both finished; the connections stay
// with the caller either way.
static void _tls_pair_open(_tls_pair_t *p, ctls_ctx *server_ctx,
                           ctls_ctx *client_ctx, const char *host) {
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
      ctls_handshake_result r = ctls_conn_handshake_step(p->server);
      if (r == CTLS_HANDSHAKE_ERROR) return;
      s_done = r == CTLS_HANDSHAKE_DONE;
    }
    if (!c_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(p->client);
      if (r == CTLS_HANDSHAKE_ERROR) return;
      c_done = r == CTLS_HANDSHAKE_DONE;
    }
  }
  p->done = s_done && c_done;
}

// Destroys both connections of p and closes the socketpair, at the end of a
// test.
static void _tls_pair_close(_tls_pair_t *p) {
  ctls_conn_destroy(p->client);
  ctls_conn_destroy(p->server);
  if (p->fds[0] >= 0) close(p->fds[0]);
  if (p->fds[1] >= 0) close(p->fds[1]);
}

// Runs one handshake of a server that trusts bundle against a client that
// presents cert and key (nothing when cert is NULL); the CRL tests are built on
// it. Tells whether it finished and gives the server's verify result in
// *verify_out.
static bool _crl_handshake(const char *bundle, const char *cert,
                           const char *key, long *verify_out) {
  *verify_out = -1;
  ctls_ctx *s = ctls_ctx_new(NULL);
  ctls_ctx *c = ctls_ctx_new(NULL);
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

// A bundle that holds a CRL enforces it: The revoked client fails with
// X509_V_ERR_CERT_REVOKED and an unrevoked one of the same CA verifies. A CRL
// that's loaded but not enforced would let the revoked client verify.
TEST(tls_crl, a_revoked_client_certificate_is_refused) {
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

// With a CRL in the bundle, every certificate below the anchor needs a CRL
// from its issuer: Only the intermediate's CRL refuses its client with
// X509_V_ERR_UNABLE_TO_GET_CRL, both CRLs accept it, and no CRL checks nothing.
// Checking only the leaf (no X509_V_FLAG_CRL_CHECK_ALL) would accept it.
TEST(tls_crl, every_ca_of_the_chain_needs_a_crl) {
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
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  ctls_ctx *s = ctls_ctx_new(NULL);
  ctls_ctx *c = ctls_ctx_new(NULL);
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
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _peer_cert_handler, NULL),
             ccol_success);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = _pki("b_root_crl.pem");
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 40;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1;
  char body[64];
  ccol_retval revoked_rv =
      _mtls_get(TLS_TEST_PORT + 40, _pki("revoked.crt"), _pki("revoked.key"),
                &status, body, sizeof(body));
  int revoked_status = status;
  ccol_retval good_rv =
      _mtls_get(TLS_TEST_PORT + 40, _pki("good.crt"), _pki("good.key"), &status,
                body, sizeof(body));
  REQUIRE_NE(revoked_rv, ccol_success);
  REQUIRE_EQ(revoked_status, -1);
  REQUIRE_EQ(good_rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, "verified=1");
}

// A CRL names no trust anchor, so such a bundle is refused like an empty one.
TEST(tls_crl, a_bundle_of_nothing_but_crls_is_refused) {
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  ctls_ctx *s = ctls_ctx_new(NULL);
  REQUIRE_TRUE(s != NULL);
  ccol_retval rv = ctls_ctx_trust(s, _pki("b_crl_only.pem"), NULL);
  ctls_ctx_release(s);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
}

// What the FIFO test's thread gets back from ctls, and when it's done.
typedef struct {
  const char *path;
  ccol_retval trust_rv;
  ccol_retval cert_rv;
  char *trust_why;
  atomic_bool done;
} _fifo_ctx_t;

// Calls ctls_ctx_trust and ctls_ctx_cert_add with the FIFO path on a thread of
// its own, so the test can see whether they block. Stores the results in the
// _fifo_ctx_t and sets done.
static void *_fifo_trust_thread(void *arg) {
  _fifo_ctx_t *f = (_fifo_ctx_t *)arg;
  ctls_ctx *c = ctls_ctx_new(NULL);
  f->trust_rv =
      c ? ctls_ctx_trust(c, f->path, &f->trust_why) : ccol_not_enough_memory;
  f->cert_rv = c ? ctls_ctx_cert_add(c, NULL, f->path, g_key_path, NULL, NULL)
                 : ccol_not_enough_memory;
  ctls_ctx_release(c);
  atomic_store(&f->done, true);
  return NULL;
}

// A path naming a FIFO without a writer is refused at once with
// ccol_http_tls_cert_load_failed. An open that waits would still be running
// after 5 s. The test then opens the FIFO for writing itself, so a blocked call
// ends either way.
TEST(tls_files, a_fifo_path_fails_at_once_and_never_blocks) {
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

// Reads the first certificate of the PEM file at pem_path into its DER
// encoding, its SHA-256 digest and its RFC 2253 subject, which the identity
// tests compare with what the server reports. Tells whether it worked.
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

// The subject comes in RFC 2253 form with the comma inside a value escaped,
// and every call gives the same answer.
TEST(tls_identity, a_verified_client_has_its_der_digest_and_subject) {
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
  ctls_ctx *s = ctls_ctx_new(NULL);
  ctls_ctx *c = ctls_ctx_new(NULL);
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

// Tells whether every identity accessor of conn reports nothing; the test of
// unverified peers checks each case with it.
static bool _identity_is_empty(ctls_conn *conn) {
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

// Covers an anonymous client under client_cert_optional, a client certificate
// sent to a server with no trust store, the client side of a context with no
// trust store (it sees the server's certificate but doesn't verify it), and a
// NULL connection. Accessors that ignored verification would report one in the
// third case. LibreSSL and OpenSSL 1.1.1 spell the call that takes a reference
// on the peer certificate without the 1.
TEST(tls_identity, nothing_is_reported_without_a_verified_certificate) {
  if (!g_cert_ready || !_pki_ready()) {
    fprintf(stderr, "SKIP: no PKI available\n");
    return;
  }
  ctls_ctx *s1 = ctls_ctx_new(NULL);
  ctls_ctx *c1 = ctls_ctx_new(NULL);
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
  ctls_ctx *s2 = ctls_ctx_new(NULL);
  ctls_ctx *c2 = ctls_ctx_new(NULL);
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
  ctls_ctx *s4 = ctls_ctx_new(NULL);
  ctls_ctx *c4 = ctls_ctx_new(NULL);
  bool ok4 = s4 && c4 &&
             ctls_ctx_cert_add(s4, NULL, g_cert_path, g_key_path, NULL, NULL) ==
                 ccol_success;
  bool done4 = false, empty4 = false, seen4 = false;
  if (ok4) {
    _tls_pair_t p;
    _tls_pair_open(&p, s4, c4, NULL);
    done4 = p.done;
    if (done4) {
      SSL *cs = ctls__conn_ssl_for_tests(p.client);
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

// Drives a ctls server connection against a raw OpenSSL client that offers
// *sess when it isn't NULL and gets the connection's session back (one byte
// from the server makes a TLS 1.3 client take in its tickets), for the
// resumption test. Gives the DER length and subject the server sees, and
// whether it resumed.
static bool _identity_resume_connect(ctls_ctx *server_ctx, SSL_CTX *client_ctx,
                                     SSL_SESSION **sess, size_t *der_len,
                                     char *subject, size_t subject_cap,
                                     bool *reused) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  for (int i = 0; i < 2; i++)
    fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
  ctls_conn *server = ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  SSL *client = SSL_new(client_ctx);
  bool ok = server && client && SSL_set_fd(client, fds[1]) == 1;
  if (ok && *sess) ok = SSL_set_session(client, *sess) == 1;
  bool s_done = false, c_done = false;
  if (ok) SSL_set_connect_state(client);
  for (int i = 0; ok && i < 2000 && !(s_done && c_done); i++) {
    if (!s_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(server);
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

// A resuming client presents no certificate, but the server reports the
// identity of the original handshake, for TLS 1.2 and 1.3. LibreSSL resumes no
// TLS 1.3 session, so there only TLS 1.2 is checked.
TEST(tls_identity, a_resumed_session_reports_the_same_identity) {
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
    ctls_ctx *s = ctls_ctx_new(NULL);
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

// Answers with what chttpserver's identity accessors report, so a test sees
// the identity of its client through a handler.
static void _identity_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t len = 7;
  const void *der = chttpsvr_req_peer_cert_der(req, &len);
  unsigned char sha[CHTTPSVR_PEER_CERT_SHA256_LEN];
  memset(sha, 0xAB, sizeof(sha));
  ccol_retval rv = chttpsvr_req_peer_cert_sha256(req, sha);
  char hex[2 * CHTTPSVR_PEER_CERT_SHA256_LEN + 1];
  for (int i = 0; i < CHTTPSVR_PEER_CERT_SHA256_LEN; i++)
    snprintf(hex + 2 * i, 3, "%02x", sha[i]);
  const char *subj = chttpsvr_req_peer_cert_subject(req);
  chttpsvr_resp_printf(resp, "der=%d len=%zu rv=%d sha=%s subj=%s", der != NULL,
                       len, (int)rv, hex, subj ? subj : "(none)");
}

// An anonymous client under client_cert_optional gets ccol_key_not_found and
// a digest buffer left all zeroes.
TEST(tls_identity, a_handler_sees_the_identity_of_its_client) {
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

  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/peer",
                                       _identity_handler, NULL),
             ccol_success);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = _pki("b_root_crl.pem");
  tls.client_cert_optional = true;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TLS_TEST_PORT + 41;
  cfg.tls = &tls;
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int status = -1, anon_status = -1;
  char body[512], anon[512];
  ccol_retval rv = _mtls_get(TLS_TEST_PORT + 41, _pki("good.crt"),
                             _pki("good.key"), &status, body, sizeof(body));
  ccol_retval anon_rv = _mtls_get(TLS_TEST_PORT + 41, NULL, NULL, &anon_status,
                                  anon, sizeof(anon));
  REQUIRE_TRUE(have);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(body, want_body);
  REQUIRE_EQ(anon_rv, ccol_success);
  REQUIRE_EQ(anon_status, 200);
  REQUIRE_STREQ(anon, want_anon);
}

TEST(tls_identity, null_arguments_report_nothing) {
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

// An allocator that counts the blocks it handed out and didn't take back yet,
// so the restart test can show that a restart leaks nothing.
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

// Waits up to 10 s for g_tls_live to reach 0, so the restart test can check
// that nothing leaked, and gives what it read last. The last frees of a started
// server run on the reactor thread after chttpsvr_destroy() returns.
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

// A server that stops and starts again builds its TLS context afresh, gives the
// old one back and keeps verifying client certificates. A second TLS server on
// the port the first holds fails its start with ccol_unexpected_failure, gives
// back what that start built and leaves the first one serving.
TEST(chttpserver_tls,
     a_restarted_tls_server_serves_and_a_busy_port_is_refused) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available\n");
    return;
  }
  ccol_memmgmt_procs mp = {_tls_count_malloc, _tls_count_free,
                           _tls_count_calloc, _tls_count_realloc};
  atomic_store(&g_tls_live, 0);
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/peer", _peer_cert_handler, NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = g_cert_path;
  tls.key_path = g_key_path;
  tls.ca_bundle_path = g_client_cert_path;
  tls.client_cert_optional = true;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.tls = &tls;
  uint16_t port = 0;
  int status_first = -1, status_restart = -1, status_after_busy = -1;
  char body_first[64] = "", body_restart[64] = "", body_after_busy[64] = "";
  ccol_retval first_rv = chttpsvr_start(srv, &cfg);
  ccol_retval restart_rv = ccol_unexpected_failure;
  ccol_retval busy_rv = ccol_success;
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
