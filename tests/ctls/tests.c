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

#include <common.h>
#include <ctls.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* White-box accessor from ctls.c (RUNNING_UNIT_TESTS only). */
extern SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn);

/* White-box accessor from chashmap.c (RUNNING_UNIT_TESTS only); see its own
   doc comment there for the finding (a "possibly lost" chashmap iterator
   under a heavily-loaded `make memtest` run) this permanent guard exists to
   catch, should it ever genuinely recur. */
extern long chashmap_iter_outstanding_count_for_tests(void);

/* Checked at true process exit (the exact moment valgrind's own leak check
   runs, later than any TEST()'s own local checks can reach) so a genuine
   future regression in ANY chashmap iterator's own alloc/free balance,
   anywhere in this suite's run, aborts loudly and immediately instead of
   surfacing only as a rare, hard-to-reproduce valgrind report. */
static void __attribute__((destructor)) _check_chmap_iter_balance_at_exit(
    void) {
  long outstanding = chashmap_iter_outstanding_count_for_tests();
  if (outstanding != 0) {
    fprintf(stderr,
            "[chmap-iter-balance] FATAL: %ld chashmap iterator(s) still "
            "outstanding at process exit\n",
            outstanding);
    abort();
  }
}

/* ========================================================================== */
/*                    THROWAWAY CERTIFICATE GENERATION                        */
/*                                                                            */
/* Real PEM files (used to exercise ctls_ctx_cert_add's cert_path/key_path    */
/* file-loading path, distinct from its own self-signed-generation path)     */
/* are produced once at startup via the openssl CLI, mirroring the same      */
/* approach already established in tests/chttpclient/tests_tls.c and         */
/* tests/chttpserver/tests_tls.c. Unlike those suites, ctls never aborts the */
/* process on a bad/missing cert (a confirmed design decision; see          */
/* ctls.h's own doc comments), so there is no need to isolate this into a    */
/* separate binary purely for process-abort safety; a missing openssl CLI   */
/* here simply fails the affected tests' setup with a clear message.        */
/* ========================================================================== */

static char g_cert_dir[256];
static char g_server_cert[320], g_server_key[320];
static char g_client_cert[320], g_client_key[320];
static bool g_certs_ready = false;

static int _openssl_selfsigned(const char *key_path, const char *cert_path,
                               const char *cn, const char *san) {
  char cmd[1024];
  int n;
  if (san) {
    n = snprintf(cmd, sizeof(cmd),
                 "openssl req -x509 -newkey rsa:2048 -nodes -keyout '%s' "
                 "-out '%s' -days 1 -subj '/CN=%s' -addext "
                 "'subjectAltName=%s' >/dev/null 2>&1",
                 key_path, cert_path, cn, san);
  } else {
    n = snprintf(cmd, sizeof(cmd),
                 "openssl req -x509 -newkey rsa:2048 -nodes -keyout '%s' "
                 "-out '%s' -days 1 -subj '/CN=%s' >/dev/null 2>&1",
                 key_path, cert_path, cn);
  }
  if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
  if (system(cmd) != 0) return -1;
  if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) return -1;
  return 0;
}

static int _generate_all_certs(void) {
  snprintf(g_cert_dir, sizeof(g_cert_dir), "/tmp/ctls_test_XXXXXX");
  if (!mkdtemp(g_cert_dir)) return -1;
  snprintf(g_server_cert, sizeof(g_server_cert), "%s/server.pem", g_cert_dir);
  snprintf(g_server_key, sizeof(g_server_key), "%s/server_key.pem", g_cert_dir);
  snprintf(g_client_cert, sizeof(g_client_cert), "%s/client.pem", g_cert_dir);
  snprintf(g_client_key, sizeof(g_client_key), "%s/client_key.pem", g_cert_dir);
  if (_openssl_selfsigned(g_server_key, g_server_cert, "127.0.0.1",
                          "IP:127.0.0.1") != 0)
    return -1;
  if (_openssl_selfsigned(g_client_key, g_client_cert, "test-client", NULL) !=
      0)
    return -1;
  return 0;
}

__attribute__((constructor)) static void _setup(void) {
  g_certs_ready = (_generate_all_certs() == 0);
  if (!g_certs_ready) {
    fprintf(stderr,
            "WARNING: could not generate test certs via the openssl CLI; "
            "file-backed cert tests will be skipped.\n");
  }
}

__attribute__((destructor)) static void _teardown(void) {
  unlink(g_server_cert);
  unlink(g_server_key);
  unlink(g_client_cert);
  unlink(g_client_key);
  rmdir(g_cert_dir);
}

/* ========================================================================== */
/*                          HANDSHAKE DRIVER HELPERS                          */
/* ========================================================================== */

/* Non-blocking AF_UNIX socketpair: real socket semantics (recv/send-based
 * BIOs work fine over it), no networking/port binding needed. hostname
 * verification is decoupled from the socket's real address family
 * (ctls_conn_create_client's hostname argument is purely a verification
 * target, not tied to the fd's actual peer) so this is a faithful
 * substitute for a real TCP loopback pair in every test below. */
static void _make_nonblocking_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  for (int i = 0; i < 2; ++i) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
  }
}

/* Alternates non-blocking handshake steps on both ends of a connection pair
 * until both finish or max_iters is exhausted. Safe with no real poll(2)
 * wait: both ends share one already-connected AF_UNIX socketpair, so
 * whatever one side writes is immediately available for the other side's
 * very next read attempt. */
static bool _drive_both(ctls_conn_t *a, ctls_conn_t *b, int max_iters,
                        bool *out_a_ok, bool *out_b_ok) {
  bool a_done = false, b_done = false, a_ok = true, b_ok = true;
  for (int i = 0; i < max_iters && !(a_done && b_done); ++i) {
    if (!a_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) {
        a_done = true;
      } else if (r == CTLS_HANDSHAKE_ERROR) {
        a_done = true;
        a_ok = false;
      }
    }
    if (!b_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(b);
      if (r == CTLS_HANDSHAKE_DONE) {
        b_done = true;
      } else if (r == CTLS_HANDSHAKE_ERROR) {
        b_done = true;
        b_ok = false;
      }
    }
  }
  if (out_a_ok) *out_a_ok = a_done && a_ok;
  if (out_b_ok) *out_b_ok = b_done && b_ok;
  return a_done && b_done && a_ok && b_ok;
}

static const char *_peer_cert_cn(SSL *ssl, char *buf, size_t buflen) {
  X509 *cert = SSL_get1_peer_certificate(ssl);
  if (!cert) return NULL;
  X509_NAME *name = X509_get_subject_name(cert);
  int n = X509_NAME_get_text_by_NID(name, NID_commonName, buf, (int)buflen);
  X509_free(cert);
  return n >= 0 ? buf : NULL;
}

/* ========================================================================== */
/*                          CONTEXT-LEVEL BEHAVIOR                            */
/* ========================================================================== */

TEST(ctls_ctx, new_default_alloc) {
  char *err = NULL;
  ctls_ctx_t *ctx = ctls_ctx_new(&err);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ctls_ctx_release(ctx);
}

static int g_custom_malloc_calls = 0;
static void *_custom_malloc(size_t n) {
  g_custom_malloc_calls++;
  return malloc(n);
}
static void *_custom_calloc(size_t n, size_t sz) {
  g_custom_malloc_calls++;
  return calloc(n, sz);
}
static void *_custom_realloc(void *p, size_t n) { return realloc(p, n); }
static void _custom_free(void *p) { free(p); }

TEST(ctls_ctx, new_custom_alloc_is_used) {
  g_custom_malloc_calls = 0;
  ccol_memmgmt_procs_t procs = {.malloc = _custom_malloc,
                                .free = _custom_free,
                                .calloc = _custom_calloc,
                                .realloc = _custom_realloc};
  char *err = NULL;
  ctls_ctx_t *ctx = ctls_ctx_new_mp(&procs, &err);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  REQUIRE_GT(g_custom_malloc_calls, 0);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_null_ctx_is_invalid) {
  ccol_retval_t rv = ctls_ctx_cert_add(NULL, "x", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
}

TEST(ctls_ctx, cert_add_no_name_no_files_generates_self_signed_default) {
  /* server_name NULL/"" with cert_path/key_path both NULL is valid: it
   * generates a self-signed DEFAULT certificate (a generic, fixed subject
   * name is used since the default slot has no name of its own to borrow
   * one from). */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv = ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_only_cert_path_is_invalid) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv =
      ctls_ctx_cert_add(ctx, NULL, "/nonexistent/cert.pem", NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_missing_file_reports_load_failure) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv = ctls_ctx_cert_add(ctx, NULL, "/nonexistent/cert.pem",
                                       "/nonexistent/key.pem", NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_self_signed_default_succeeds) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv =
      ctls_ctx_cert_add(ctx, "self-signed.test", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_real_files_succeeds) {
  if (!g_certs_ready) return;
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_missing_file_reports_load_failure) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t rv = ctls_ctx_trust(ctx, "/nonexistent/ca.pem", NULL);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_null_args_invalid) {
  REQUIRE_EQ(ctls_ctx_trust(NULL, "x", NULL), ccol_invalid_args);
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust(ctx, NULL, NULL), ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_system_no_crash) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ctls_ctx_trust_system(ctx);
  ctls_ctx_release(ctx);
}

static void _alpn_cleanup_marker(void *udata) { *(bool *)udata = true; }

TEST(ctls_ctx, alpn_add_invalid_args) {
  REQUIRE_EQ(ctls_ctx_alpn_add(NULL, "http/1.1", NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctls_ctx_alpn_add(ctx, "", NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, alpn_add_and_count_and_cleanup_fires_on_release) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  bool cleaned_up = false;
  ccol_retval_t rv = ctls_ctx_alpn_add(ctx, "http/1.1", NULL, &cleaned_up,
                                       _alpn_cleanup_marker, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(ctls_ctx_alpn_count(ctx), (size_t)1);
  ctls_ctx_release(ctx);
  REQUIRE_TRUE(cleaned_up);
}

TEST(ctls_ctx, retain_release_keeps_ctx_alive) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ctls_ctx_retain(ctx);
  ctls_ctx_release(ctx); /* still one reference (the one below) left */
  ccol_retval_t rv =
      ctls_ctx_cert_add(ctx, "still-alive.test", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, release_null_is_noop) { ctls_ctx_release(NULL); }

/* ========================================================================== */
/*                         HANDSHAKE / I/O BEHAVIOR                           */
/* ========================================================================== */

TEST(ctls_handshake, self_signed_no_verify_completes) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  char *err = NULL;
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, &err);
  REQUIRE_NE((void *)server_conn, (void *)NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, &err);
  REQUIRE_NE((void *)client_conn, (void *)NULL);

  bool ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  REQUIRE_TRUE(ok);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, read_write_roundtrip_after_handshake) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  const char *msg = "hello from client";
  ssize_t written = ctls_conn_write(client_conn, msg, strlen(msg));
  REQUIRE_EQ(written, (ssize_t)strlen(msg));

  char buf[128] = {0};
  ssize_t got = -1;
  for (int i = 0; i < 100 && got <= 0; ++i)
    got = ctls_conn_read(server_conn, buf, sizeof(buf) - 1);
  REQUIRE_EQ(got, (ssize_t)strlen(msg));
  REQUIRE_STREQ(buf, msg);

  const char *reply = "hi client";
  written = ctls_conn_write(server_conn, reply, strlen(reply));
  REQUIRE_EQ(written, (ssize_t)strlen(reply));
  char buf2[128] = {0};
  got = -1;
  for (int i = 0; i < 100 && got <= 0; ++i)
    got = ctls_conn_read(client_conn, buf2, sizeof(buf2) - 1);
  REQUIRE_EQ(got, (ssize_t)strlen(reply));
  REQUIRE_STREQ(buf2, reply);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, hostname_verify_success_with_matching_ip_san) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  /* The server cert is self-signed; trust it directly as its own CA so
   * chain verification (triggered by verify_host implying verify_peer, per
   * this module's own documented semantics) has something to succeed
   * against. */
  REQUIRE_EQ(ctls_ctx_trust(client_ctx, g_server_cert, NULL), ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "127.0.0.1", true, NULL);
  REQUIRE_NE((void *)client_conn, (void *)NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));
  REQUIRE_EQ(ctls_conn_verify_result(client_conn), 0L /* X509_V_OK */);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, hostname_verify_failure_wrong_host) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust(client_ctx, g_server_cert, NULL), ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  /* "10.0.0.9" does not match the cert's IP:127.0.0.1 SAN. */
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "10.0.0.9", true, NULL);
  REQUIRE_NE((void *)client_conn, (void *)NULL);

  bool client_ok = true, server_ok = true;
  _drive_both(client_conn, server_conn, 200, &client_ok, &server_ok);
  REQUIRE_FALSE(client_ok);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, mtls_client_presents_trusted_cert_succeeds) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  /* Server trusts the client's own (self-signed) cert as its CA. */
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(client_ctx, NULL, g_client_cert, g_client_key,
                               NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *server_ssl = _ctls_conn_ssl_for_tests(server_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(server_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "test-client");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, mtls_client_presents_no_cert_still_succeeds) {
  /* Verbatim OpenSSL semantics for SSL_VERIFY_PEER with no
   * SSL_VERIFY_FAIL_IF_NO_PEER_CERT: a client presenting no certificate at
   * all is still accepted, since there is nothing to fail verification
   * against. */
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL); /* no client cert configured */

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, mtls_client_presents_untrusted_cert_fails) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  /* Server trusts ONLY the server cert itself as CA (NOT the client
   * cert) so a client presenting its own (self-signed, untrusted) cert
   * must fail verification. */
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_server_cert, NULL), ccol_success);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(client_ctx, NULL, g_client_cert, g_client_key,
                               NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);

  bool client_ok = true, server_ok = true;
  _drive_both(client_conn, server_conn, 200, &client_ok, &server_ok);
  REQUIRE_FALSE(server_ok);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

/* ========================================================================== */
/*                              SNI DISPATCH                                  */
/* ========================================================================== */

TEST(ctls_sni, no_sni_uses_default_cert) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  /* No hostname given at all -> no SNI extension sent -> default cert. */
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "ctls-default");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_sni, exact_name_match_selects_named_cert) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "alpha.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "alpha.test");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_sni, one_level_wildcard_matches_subdomain) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "default.test", NULL, NULL, NULL, NULL),
      ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "*.wild.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "foo.wild.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "*.wild.test");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_sni, mixed_case_name_matches_case_insensitively) {
  /* Registered with mixed case; ctls_ctx_cert_add lowercases server_name
   * before using it as both the map key and (for a self-signed named entry)
   * the certificate's own subject, so the CN below is expected lowercase
   * regardless of the case used here. */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "Alpha.Test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  /* Client sends the SNI extension value in a completely different case than
   * how the cert was registered above. */
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "ALPHA.TEST", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  /* Falling back to "ctls-default" here (instead of "alpha.test") would mean
   * the case mismatch caused the named entry to be missed entirely. */
  REQUIRE_STREQ(cn, "alpha.test");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

/* Regression: re-registering a named certificate for a server_name that
 * already has one is a supported certificate-rotation pattern (ctls_ctx_
 * cert_add's own implementation explicitly looks up and destroys the
 * previous entry before inserting the new one). Internally, the chmap
 * update for an already-present key returns ccol_key_already_present, not
 * ccol_success; ctls_ctx_cert_add must treat that as success rather than
 * destroying the certificate it just stored, which would leave the map's
 * value slot for this hostname pointing at freed memory. */
TEST(ctls_sni, cert_add_replaces_existing_named_cert_without_dangling_pointer) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "rotate.test", g_server_cert,
                               g_server_key, NULL, NULL),
             ccol_success);
  /* Rotate: same name, different cert/key pair. */
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "rotate.test", g_client_cert,
                               g_client_key, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "rotate.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  /* Must be the second (client) cert's own CN, proving the rotation
   * actually took effect against a live, correctly-updated entry rather
   * than a dangling pointer left over from the first, freed cert. */
  REQUIRE_STREQ(cn, "test-client");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_sni, unmatched_name_falls_back_to_default) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn = ctls_conn_create_client(
      client_ctx, fds[1], "totally-unrelated.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "ctls-default");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

/* ========================================================================== */
/*                            ALPN NEGOTIATION                                */
/* ========================================================================== */

typedef struct alpn_capture {
  bool fired;
  char name[64];
  size_t len;
} alpn_capture;

static void _alpn_capture_cb(ctls_conn_t *conn, const char *name, size_t len,
                             void *udata) {
  (void)conn;
  alpn_capture *cap = (alpn_capture *)udata;
  cap->fired = true;
  cap->len = len < sizeof(cap->name) - 1 ? len : sizeof(cap->name) - 1;
  memcpy(cap->name, name, cap->len);
  cap->name[cap->len] = 0;
}

TEST(ctls_alpn, matching_protocol_is_selected_both_sides) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  alpn_capture server_cap = {0}, client_cap = {0};
  REQUIRE_EQ(ctls_ctx_alpn_add(server_ctx, "http/1.1", _alpn_capture_cb,
                               &server_cap, NULL, NULL),
             ccol_success);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(client_ctx, "http/1.1", _alpn_capture_cb,
                               &client_cap, NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  REQUIRE_TRUE(server_cap.fired);
  REQUIRE_STREQ(server_cap.name, "http/1.1");
  REQUIRE_TRUE(client_cap.fired);
  REQUIRE_STREQ(client_cap.name, "http/1.1");

  size_t len = 0;
  const char *sel = ctls_conn_alpn_selected(server_conn, &len);
  REQUIRE_NE((void *)sel, (void *)NULL);
  REQUIRE_EQ(len, (size_t)strlen("http/1.1"));
  REQUIRE_EQ(memcmp(sel, "http/1.1", len), 0);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_alpn, no_overlap_falls_back_to_default_protocol) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  alpn_capture server_cap = {0};
  /* "spdy/1" is the server's only (hence default/fallback) protocol. */
  REQUIRE_EQ(ctls_ctx_alpn_add(server_ctx, "spdy/1", _alpn_capture_cb,
                               &server_cap, NULL, NULL),
             ccol_success);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(client_ctx, "http/1.1", NULL, NULL, NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  /* Fallback to the default entry's callback still fires even though the
   * wire negotiation itself produced no overlap. */
  REQUIRE_TRUE(server_cap.fired);
  REQUIRE_STREQ(server_cap.name, "spdy/1");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

/* ========================================================================== */
/*                              ERROR PATHS                                   */
/* ========================================================================== */

TEST(ctls_conn, create_client_null_ctx_fails) {
  char *err = NULL;
  ctls_conn_t *conn = ctls_conn_create_client(NULL, 3, "x", false, &err);
  REQUIRE_EQ((void *)conn, (void *)NULL);
}

TEST(ctls_conn, create_client_negative_fd_fails) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ctls_conn_t *conn = ctls_conn_create_client(ctx, -1, "x", false, NULL);
  REQUIRE_EQ((void *)conn, (void *)NULL);
  ctls_ctx_release(ctx);
}

TEST(ctls_conn, create_server_null_ctx_fails) {
  ctls_conn_t *conn = ctls_conn_create_server(NULL, 3, NULL, NULL);
  REQUIRE_EQ((void *)conn, (void *)NULL);
}

TEST(ctls_conn, handshake_step_null_conn_is_error) {
  REQUIRE_EQ(ctls_conn_handshake_step(NULL), CTLS_HANDSHAKE_ERROR);
}

TEST(ctls_conn, read_write_null_conn_fails) {
  char buf[8];
  REQUIRE_EQ(ctls_conn_read(NULL, buf, sizeof(buf)), (ssize_t)-1);
  REQUIRE_EQ(ctls_conn_write(NULL, buf, sizeof(buf)), (ssize_t)-1);
}

TEST(ctls_conn, verify_result_null_conn_is_negative) {
  REQUIRE_LT(ctls_conn_verify_result(NULL), 0L);
}

TEST(ctls_conn, alpn_selected_null_conn_returns_null) {
  size_t len = 123;
  const char *r = ctls_conn_alpn_selected(NULL, &len);
  REQUIRE_EQ((void *)r, (void *)NULL);
  REQUIRE_EQ(len, (size_t)0);
}

TEST(ctls_conn, udata_null_conn_returns_null) {
  REQUIRE_EQ(ctls_conn_udata(NULL), (void *)NULL);
}

TEST(ctls_conn, destroy_null_is_noop) { ctls_conn_destroy(NULL); }

TEST(ctls_conn, udata_roundtrip) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  int fds[2];
  _make_nonblocking_pair(fds);
  int marker = 42;
  ctls_conn_t *server_conn =
      ctls_conn_create_server(ctx, fds[0], &marker, NULL);
  REQUIRE_EQ(ctls_conn_udata(server_conn), (void *)&marker);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(ctx);
}

/* ========================================================================== */
/* Regression coverage for three real bugs found via an independent deep-scan */
/* code review, none previously caught: (1) a use-after-free in */
/* _ctls_servername_cb, which used to look up the matching named cert's own */
/* SSL_CTX* under tls->lock but call SSL_set_SSL_CTX() on it AFTER releasing */
/* the lock, racing a concurrent ctls_ctx_cert_add() rotating (and freeing) */
/* that exact SSL_CTX; (2) an entirely unlocked data race in */
/* _ctls_alpn_select_cb (server-mode ALPN selection), which read */
/* tls->alpn[]/tls->alpn_count with no locking at all, unlike its own */
/* client-mode sibling _ctls_record_client_alpn, which already locked */
/* correctly; and (3) a missing NULL check on the self-signed-certificate */
/* subject-name allocation in ctls_ctx_cert_add, which could reach */
/* _ctls_create_self_signed(NULL) -> strlen(NULL) under sustained memory */
/* pressure instead of the documented, graceful ccol_not_enough_memory every */
/* other allocation failure in that same function already reports.            */
/* ========================================================================== */

/* (3): self-signed named-cert subject-name OOM must not crash */

static _Atomic int g_fail_malloc_at_call = 0; /* 0 = never fail */
static _Atomic int g_malloc_call_count = 0;

static void *_fail_nth_malloc(size_t n) {
  int call = atomic_fetch_add(&g_malloc_call_count, 1) + 1;
  int fail_at = atomic_load(&g_fail_malloc_at_call);
  if (fail_at && call == fail_at) return NULL;
  return malloc(n);
}
static void _fail_nth_free(void *p) { free(p); }
static void *_fail_nth_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void *_fail_nth_realloc(void *p, size_t sz) { return realloc(p, sz); }

TEST(ctls_ctx, cert_add_self_signed_name_alloc_failure_reports_oom_not_crash) {
  /* ctls_ctx_cert_add's own call sequence for a NAMED, self-signed
     certificate with no password: lower_name = _ctls_strdup_lower(...) is
     the first malloc, nc = _mem_calloc(...) is a calloc (not counted
     here), and nc->self_signed_name = _ctls_strdup(...) is the second
     malloc; exactly the one this test targets. Confirmed to crash
     (strlen(NULL) inside _ctls_create_self_signed, reached via
     _ctls_ctx_rebuild_locked) against a scratch build with the NULL check
     this test guards removed, before the fix was reapplied. */
  ccol_memmgmt_procs_t mp = {_fail_nth_malloc, _fail_nth_free, _fail_nth_calloc,
                             _fail_nth_realloc};
  ctls_ctx_t *ctx = ctls_ctx_new_mp(&mp, NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  atomic_store(&g_malloc_call_count, 0);
  atomic_store(&g_fail_malloc_at_call, 2);
  char *err = NULL;
  ccol_retval_t rv = ctls_ctx_cert_add(ctx, "oom.test", NULL, NULL, NULL, &err);
  atomic_store(&g_fail_malloc_at_call, 0); /* disarm before any further alloc */
  REQUIRE_EQ((int)rv, (int)ccol_not_enough_memory);

  /* The context must still be genuinely usable afterward: the failed add
     must not have left it half-configured or corrupted. */
  REQUIRE_EQ(ctls_ctx_cert_add(ctx, "after-oom.test", NULL, NULL, NULL, NULL),
             ccol_success);

  ctls_ctx_release(ctx);
}

/* (1): SNI servername-callback UAF under concurrent cert rotation */

static _Atomic bool g_sni_race_stop = false;
static ctls_ctx_t *g_sni_race_ctx = NULL;

static void *_sni_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_sni_race_stop)) {
    /* Repeatedly re-adding the SAME name forces _ctls_ctx_rebuild_locked's
       own commit step to SSL_CTX_free() the previous built_ctx on every
       iteration; exactly the free a concurrent, in-flight
       _ctls_servername_cb call (on the handshake thread below) must never
       observe happening to the SSL_CTX* it already looked up. */
    ctls_ctx_cert_add(g_sni_race_ctx, "race.test", NULL, NULL, NULL, NULL);
    /* A real wall-clock throttle, not sched_yield(): with 22 real cores
       available, an unthrottled rotate thread runs on its own dedicated
       core with nothing else contending for it, so sched_yield() there is a
       near no-op (it only cedes the CPU to another runnable thread on the
       SAME core) and does nothing to slow this thread's own iteration rate
       relative to the main thread's, which is on a different core entirely.
       The rotate thread re-acquires ctx->lock so much faster than the main
       thread's own handshake-driving calls that the lock is, in practice,
       essentially always held (or immediately re-stolen the instant it's
       released) by the time the main thread ever tries for it; a genuine
       lock-starvation livelock (300 handshakes never completing within
       several real minutes, not a deadlock but not survivable as a test
       either), found empirically before this throttle was added: first a
       180s run, then a bare sched_yield()-throttled 120s run, both never
       got past the very first iteration of this test. usleep(1000) caps
       the rotate thread at roughly 1000 iterations/sec, comfortably enough
       to keep racing the main thread's own lock acquisitions while leaving
       it genuine, reliable wall-clock room to win its share of them. */
    usleep(1000);
  }
  return NULL;
}

TEST(ctls_sni, cert_add_race_during_live_handshake_does_not_crash) {
  if (!g_certs_ready) return;
  g_sni_race_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(
      ctls_ctx_cert_add(g_sni_race_ctx, "race.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  atomic_store(&g_sni_race_stop, false);
  pthread_t rotate_th;
  REQUIRE_EQ(pthread_create(&rotate_th, NULL, _sni_race_rotate_thread, NULL),
             0);

  /* A bounded number of real, full handshakes, each against a fresh
     connection pair, racing the rotation thread's own continuous churn the
     whole time. The real assertion is that this survives at all (under a
     plain run this proves nothing new; under ASan/valgrind/TSan, a
     use-after-free here reliably aborts or reports before the fix). */
  for (int i = 0; i < 80; i++) {
    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn_t *server_conn =
        ctls_conn_create_server(g_sni_race_ctx, fds[0], NULL, NULL);
    ctls_conn_t *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], "race.test", false, NULL);
    _drive_both(client_conn, server_conn, 200, NULL, NULL);
    ctls_conn_destroy(client_conn);
    ctls_conn_destroy(server_conn);
    close(fds[0]);
    close(fds[1]);
  }

  atomic_store(&g_sni_race_stop, true);
  pthread_join(rotate_th, NULL);
  /* Direct, empirical mid-suite checkpoint (see also
     _check_chmap_iter_balance_at_exit's own, stronger, true-process-exit
     check above): the rotation thread has just stopped, so nothing else in
     the process can be concurrently allocating/destroying a chashmap
     iterator right now; this confirms every chashmap_begin_iter() call this
     test's own rotation thread made (via ctls_ctx_cert_add ->
     _ctls_ctx_rebuild_locked) was genuinely balanced by a matching
     ccol_iter_destroy(), rather than relying on code-reading alone. */
  REQUIRE_EQ(chashmap_iter_outstanding_count_for_tests(), (long)0);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(g_sni_race_ctx);
  g_sni_race_ctx = NULL;
}

/* (2): server-mode ALPN-select-callback race, plus the connection-owned
 *      alpn_selected_name copy fix (both directions) */

static _Atomic bool g_alpn_race_stop = false;
static ctls_ctx_t *g_alpn_race_server_ctx = NULL;

static void *_alpn_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_alpn_race_stop)) {
    /* Re-adding the SAME protocol name repeatedly forces
       ctls_ctx_alpn_add's own "replace an existing registration" branch,
       which frees the previous entry's name buffer; exactly the buffer
       a concurrent, in-flight _ctls_alpn_select_cb call (on the handshake
       thread below) must never read from after it has been freed. */
    ctls_ctx_alpn_add(g_alpn_race_server_ctx, "h2", NULL, NULL, NULL, NULL);
    /* See the identical usleep() comment in _sni_race_rotate_thread above:
       without a real wall-clock throttle, this tight loop starves the main
       thread's own handshake-driving calls of ever winning ctx->lock in
       practice, and sched_yield() alone does not fix it. */
    usleep(1000);
  }
  return NULL;
}

TEST(ctls_alpn, alpn_add_race_during_live_handshake_does_not_crash) {
  if (!g_certs_ready) return;
  g_alpn_race_server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(g_alpn_race_server_ctx, NULL, g_server_cert,
                               g_server_key, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_alpn_add(g_alpn_race_server_ctx, "h2", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(client_ctx, "h2", NULL, NULL, NULL, NULL),
             ccol_success);

  atomic_store(&g_alpn_race_stop, false);
  pthread_t rotate_th;
  REQUIRE_EQ(pthread_create(&rotate_th, NULL, _alpn_race_rotate_thread, NULL),
             0);

  /* Captured into a local rather than asserted on directly inside the loop:
     a REQUIRE_EQ failing there would return from this function immediately,
     skipping atomic_store(&g_alpn_race_stop, true)/pthread_join(rotate_th,
     ...) entirely below and leaving that thread permanently running (its
     own loop only terminates once g_alpn_race_stop is observed true), plus
     leaking both ctls_ctx_t handles this function's own two release calls
     below would otherwise reclaim. Every check is deferred until after the
     loop, stop signal, join, and both releases have all run unconditionally,
     matching this codebase's own established discipline for exactly this
     class of early-REQUIRE-failure hazard. */
  bool alpn_len_mismatch = false;
  for (int i = 0; i < 80; i++) {
    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn_t *server_conn =
        ctls_conn_create_server(g_alpn_race_server_ctx, fds[0], NULL, NULL);
    ctls_conn_t *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
    if (_drive_both(client_conn, server_conn, 200, NULL, NULL)) {
      /* When a handshake genuinely completes, the selected protocol name
         must still be readable (through the connection-owned copy, not a
         possibly-already-rotated-and-freed ctx-owned pointer) after the
         fact, well after tls->lock has long since been released. */
      size_t len = 0;
      const char *name = ctls_conn_alpn_selected(server_conn, &len);
      if (name && len != strlen(name)) alpn_len_mismatch = true;
    }
    ctls_conn_destroy(client_conn);
    ctls_conn_destroy(server_conn);
    close(fds[0]);
    close(fds[1]);
  }

  atomic_store(&g_alpn_race_stop, true);
  pthread_join(rotate_th, NULL);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(g_alpn_race_server_ctx);
  g_alpn_race_server_ctx = NULL;

  REQUIRE_FALSE(alpn_len_mismatch);
}
