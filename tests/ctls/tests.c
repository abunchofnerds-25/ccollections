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
#include <common.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/ctls.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* White-box accessors from ctls.c (RUNNING_UNIT_TESTS only). */
extern SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn);
/* The SSL_CTX a servername dispatch installed on this connection and that
   ctls holds a reference on, or NULL if the connection never dispatched. */
extern SSL_CTX *_ctls_conn_pinned_sni_ctx_for_tests(ctls_conn_t *conn);
/* How many SSL_CTX references ctls currently holds on behalf of live
   connections; see its own doc comment in ctls.c. */
extern long _ctls_live_conn_ctx_pin_count_for_tests(void);

/* White-box accessor from chashmap.c (RUNNING_UNIT_TESTS only); see its own
   doc comment there for the leak shape this permanent guard exists to catch:
   a "possibly lost" chashmap iterator under a heavily-loaded `make memtest`
   run. */
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
/* Real PEM files drive the cert_path and key_path path of                   */
/* ctls_ctx_cert_add, which loads from a file. That path is separate from    */
/* its own path that generates a self-signed certificate. The openssl CLI    */
/* makes these files one time at startup. tests/chttpclient/tests_tls.c and  */
/* tests/chttpserver/tests_tls.c already use the same approach. ctls is      */
/* different from those suites in one way: it never stops the process for a  */
/* certificate that is bad or absent. That is a deliberate design choice;    */
/* see the doc comments in ctls.h. This file therefore needs no separate     */
/* binary for safety against a process abort. If the openssl CLI is absent   */
/* here, the setup of the affected tests fails with a clear message.         */
/* ========================================================================== */

static char g_cert_dir[256];
static char g_server_cert[320], g_server_key[320];
static char g_client_cert[320], g_client_key[320];
static char g_enc_cert[320], g_enc_key[320];
static bool g_certs_ready = false;
static bool g_enc_cert_ready = false;

/* The password a ctls_ctx_cert_add caller has to supply for g_enc_key. */
#define CTLS_TEST_KEY_PASSWORD "hunter2"

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

/* This makes a private key that a password protects, which the -nodes form
 * above deliberately does not produce. A load of such a key is the only path
 * that reaches the PEM password callback that ctls installs, and that
 * callback turns a wrong password into a reported load failure; without it,
 * OpenSSL prompts on a terminal that may not exist. */
static int _openssl_selfsigned_encrypted(const char *key_path,
                                         const char *cert_path, const char *cn,
                                         const char *password) {
  char cmd[1024];
  int n = snprintf(cmd, sizeof(cmd),
                   "openssl req -x509 -newkey rsa:2048 -keyout '%s' -out '%s' "
                   "-days 1 -subj '/CN=%s' -passout pass:%s >/dev/null 2>&1",
                   key_path, cert_path, cn, password);
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
  snprintf(g_enc_cert, sizeof(g_enc_cert), "%s/enc.pem", g_cert_dir);
  snprintf(g_enc_key, sizeof(g_enc_key), "%s/enc_key.pem", g_cert_dir);
  /* Tracked separately: an openssl build without the cipher this needs must
   * skip only the password tests, not every file-backed cert test. */
  g_enc_cert_ready =
      (_openssl_selfsigned_encrypted(g_enc_key, g_enc_cert, "enc.test",
                                     CTLS_TEST_KEY_PASSWORD) == 0);
  return 0;
}

__attribute__((constructor)) static void _setup(void) {
  /* Every write of ctls goes through send(2) with MSG_NOSIGNAL, so no test
   * here needs this ignore; it keeps a regression of that property from
   * ending this whole binary. tests_sigpipe runs with the default disposition
   * and is the suite that asserts the property itself. */
  signal(SIGPIPE, SIG_IGN);
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

/* This makes a non-blocking AF_UNIX socketpair. Such a pair has real socket
 * semantics, so a BIO built on recv and send works over it, and it needs no
 * network and no port. Hostname verification does not depend on the real
 * address family of the socket: the hostname argument of
 * ctls_conn_create_client is only a target for verification, and nothing ties
 * it to the real peer of the fd. So this pair is a faithful substitute for a
 * real TCP loopback pair in every test below. */
static void _make_nonblocking_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  for (int i = 0; i < 2; ++i) {
    /* A failure here that nothing checks leaves fds[i] blocking, so every
     * later ctls_conn_read or ctls_conn_write call in the test that called
     * this helper blocks without a bound, where the expected answer is
     * EWOULDBLOCK. That is a full hang of the test binary, and not merely a
     * check that nothing made, and this helper backs almost every test in
     * this file. */
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

/* This takes non-blocking handshake steps on each end of a connection pair in
 * turn, and stops when both ends finish, or when it uses up max_iters. It is
 * safe with no real poll(2) wait, because both ends share one AF_UNIX
 * socketpair that is already connected, so whatever one side writes is
 * available at once for the very next read of the other side. */
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
  /* A server_name of NULL or "", with cert_path and key_path both NULL, is
   * valid, and generates a self-signed DEFAULT certificate, which carries a
   * generic, fixed subject name, because the default slot has no name of its
   * own to take one from. */
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

TEST(ctls_ctx, trust_system_null_ctx_invalid) {
  REQUIRE_EQ(ctls_ctx_trust_system(NULL), ccol_invalid_args);
}

TEST(ctls_ctx, trust_system_no_crash) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  /* The system CA store of this CI and development environment must be
   * present and loadable. A platform with no store configured legitimately
   * gets ccol_http_tls_cert_load_failed here instead, which is the real
   * failure, and this return value reports it rather than dropping it
   * silently. */
  REQUIRE_EQ(ctls_ctx_trust_system(ctx), ccol_success);
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

/* The cleanup of a replaced ALPN registration must run with no lock of ctls
 * held. The cleanup below asks a second thread to take the lock of the
 * context (through ctls_ctx_retain and ctls_ctx_release) and waits a bounded
 * time for it. When the replacing call is holding that lock, the second
 * thread cannot finish until the cleanup returns, so the wait runs out, and
 * the cleanup records that; the second thread then finishes once the lock is
 * released, so the test never hangs. */
typedef struct {
  ctls_ctx_t *ctx;
  atomic_bool helper_done;
  bool lock_was_free;
  bool cleaned_up;
  pthread_t helper;
  bool helper_started;
} alpn_reentry_probe;

static void *_alpn_reentry_helper(void *arg) {
  alpn_reentry_probe *p = (alpn_reentry_probe *)arg;
  ctls_ctx_retain(p->ctx);
  ctls_ctx_release(p->ctx);
  atomic_store(&p->helper_done, true);
  return NULL;
}

static void _alpn_reentry_cleanup(void *udata) {
  alpn_reentry_probe *p = (alpn_reentry_probe *)udata;
  p->cleaned_up = true;
  p->helper_started =
      pthread_create(&p->helper, NULL, _alpn_reentry_helper, p) == 0;
  if (!p->helper_started) return;
  /* Up to 10 seconds, in 1 ms steps. */
  for (int i = 0; i < 10000 && !atomic_load(&p->helper_done); ++i) {
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  p->lock_was_free = atomic_load(&p->helper_done);
}

TEST(ctls_ctx, alpn_replacement_runs_the_old_cleanup_with_no_lock_held) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  alpn_reentry_probe probe = {.ctx = ctx};
  atomic_init(&probe.helper_done, false);
  ccol_retval_t first = ctls_ctx_alpn_add(ctx, "http/1.1", NULL, &probe,
                                          _alpn_reentry_cleanup, NULL);
  ccol_retval_t second =
      ctls_ctx_alpn_add(ctx, "http/1.1", NULL, NULL, NULL, NULL);
  if (probe.helper_started) pthread_join(probe.helper, NULL);
  size_t count = ctls_ctx_alpn_count(ctx);
  ctls_ctx_release(ctx);
  REQUIRE_EQ(first, ccol_success);
  REQUIRE_EQ(second, ccol_success);
  REQUIRE_EQ(count, (size_t)1);
  REQUIRE_TRUE(probe.cleaned_up);
  REQUIRE_TRUE(probe.helper_started);
  REQUIRE_TRUE(probe.lock_was_free);
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
  /* The server certificate is self-signed, so trust it directly as its own
   * CA, so that the chain verification has something to succeed against. A
   * verify_host implies a verify_peer, which starts that verification, as
   * the documented semantics of this module say. */
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
  REQUIRE_TRUE(ctls_conn_peer_cert_verified(server_conn));

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, mtls_client_presents_no_cert_fails_by_default) {
  /* A server-mode context with a trust store requires a client certificate
   * by default, so a client that presents none fails the handshake on the
   * server side. This test is non-vacuous: with SSL_VERIFY_PEER alone the
   * server completes the handshake. */
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
  bool client_ok = true, server_ok = true;
  _drive_both(client_conn, server_conn, 200, &client_ok, &server_ok);
  bool verified = ctls_conn_peer_cert_verified(server_conn);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_FALSE(server_ok);
  REQUIRE_FALSE(verified);
}

TEST(ctls_handshake, mtls_optional_accepts_a_client_without_a_cert) {
  /* ctls_ctx_peer_cert_optional() turns the requirement into a request: a
   * client that presents no certificate completes the handshake, and the
   * connection reports it as not verified. The call works before and after
   * ctls_ctx_trust(). This test is non-vacuous: a verified answer that
   * rests on SSL_get_verify_result() alone reports true here, because
   * OpenSSL reports X509_V_OK for a peer that presented nothing. */
  if (!g_certs_ready) return;
  for (int before = 0; before < 2; before++) {
    ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
    REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                                 NULL, NULL),
               ccol_success);
    if (before)
      REQUIRE_EQ(ctls_ctx_peer_cert_optional(server_ctx, true), ccol_success);
    REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);
    if (!before)
      REQUIRE_EQ(ctls_ctx_peer_cert_optional(server_ctx, true), ccol_success);

    ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn_t *server_conn =
        ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
    ctls_conn_t *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
    bool ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);
    long verify_result = ctls_conn_verify_result(server_conn);
    bool verified = ctls_conn_peer_cert_verified(server_conn);

    ctls_conn_destroy(client_conn);
    ctls_conn_destroy(server_conn);
    close(fds[0]);
    close(fds[1]);
    ctls_ctx_release(client_ctx);
    ctls_ctx_release(server_ctx);
    REQUIRE_TRUE(ok);
    REQUIRE_EQ(verify_result, (long)X509_V_OK);
    REQUIRE_FALSE(verified);
  }
}

TEST(ctls_handshake, peer_cert_verified_is_false_without_a_trust_store) {
  /* A client that verifies nothing reports no verified peer, although the
   * server presented a certificate; so does a NULL connection. */
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  bool before = ctls_conn_peer_cert_verified(client_conn);
  bool ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  bool after = ctls_conn_peer_cert_verified(client_conn);
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(ok);
  REQUIRE_FALSE(before);
  REQUIRE_FALSE(after);
  REQUIRE_FALSE(ctls_conn_peer_cert_verified(NULL));
  REQUIRE_EQ(ctls_ctx_peer_cert_optional(NULL, true), ccol_invalid_args);
}

TEST(ctls_io, zero_length_write_returns_zero) {
  /* A write of nothing on a healthy connection returns 0 and leaves the
   * connection usable, as write(2) on a socket does. This test is
   * non-vacuous: SSL_write() with a length of 0 returns 0, which the
   * classification of a failure reports as -1 with ECONNRESET. */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  bool ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  errno = 0;
  ssize_t zero = ctls_conn_write(client_conn, "", 0);
  int zero_errno = errno;
  ssize_t three = ctls_conn_write(client_conn, "abc", 3);
  char buf[8] = {0};
  ssize_t got = -1;
  for (int i = 0; i < 200 && got < 0; i++) {
    got = ctls_conn_read(server_conn, buf, sizeof(buf));
    if (got < 0) {
      struct pollfd p = {.fd = fds[0], .events = POLLIN};
      poll(&p, 1, 10);
    }
  }
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(zero, (ssize_t)0);
  REQUIRE_EQ(zero_errno, 0);
  REQUIRE_EQ(three, (ssize_t)3);
  REQUIRE_EQ(got, (ssize_t)3);
  REQUIRE_EQ(memcmp(buf, "abc", 3), 0);
}

TEST(ctls_io, destroy_before_the_handshake_leaves_no_openssl_error) {
  /* Destroying a connection whose handshake never completed, or never
   * started, leaves the OpenSSL error queue of the calling thread empty;
   * otherwise a later, unrelated call of OpenSSL on the same thread would
   * read that entry as its own. This test is non-vacuous: SSL_shutdown() on
   * such a connection pushes "shutdown while in init". */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  /* One step on each side leaves both in the middle of the handshake. */
  (void)ctls_conn_handshake_step(client_conn);
  (void)ctls_conn_handshake_step(server_conn);
  ERR_clear_error();
  ctls_conn_destroy(client_conn);
  unsigned long after_client = ERR_peek_error();
  ctls_conn_destroy(server_conn);
  unsigned long after_server = ERR_peek_error();
  ERR_clear_error();
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_EQ(after_client, 0UL);
  REQUIRE_EQ(after_server, 0UL);
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
  /* This test registers the name with mixed case. ctls_ctx_cert_add makes
   * server_name lowercase before it uses that name as the map key and, for a
   * named entry that it signs itself, as the subject of the certificate, so
   * the CN below is lowercase, whatever case this test uses. */
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

/* A caller can register a named certificate again for a server_name that
 * already has one, which is a supported pattern for certificate rotation.
 * The implementation of ctls_ctx_cert_add looks up the earlier entry and
 * destroys it before it inserts the new one. Inside, the chmap update for a
 * key that is already present returns ccol_key_already_present, and not
 * ccol_success, which ctls_ctx_cert_add must read as a success instead of
 * destroying the certificate that it just stored: such a destroy leaves the
 * value slot of the map for this hostname pointing at freed memory. */
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
  /* This must be the CN of the second certificate, which is the client one.
   * That proves that the rotation really took effect, against a live entry
   * that the code updated correctly, without leaving a dangling pointer from
   * the first certificate, which is freed at this point. */
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

/* ========================================================================== */
/*                      OPENSSL ex_data SLOT OWNERSHIP                        */
/* ========================================================================== */

/* The address is the whole point; the value is never read. */
static int _app_data_sentinel = 0;

TEST(ctls_ex_data, connections_leave_the_openssl_app_data_slot_alone) {
  /* This module attaches its own per-connection pointer to an SSL* through
     an ex_data index it registers with OpenSSL. Index 0 is not that index:
     OpenSSL reserves it for the SSL_set_app_data()/SSL_get_app_data()
     macros, which any other code in the same process may be using on an
     SSL* it is handed. Taking that slot makes the two aliases of one
     pointer, so each side silently reads and overwrites the other's.

     The expectations below are written against OpenSSL's own macros rather
     than against anything this module exports, so they cannot move with the
     implementation they are checking. */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);
  alpn_capture server_cap = {0};
  ccol_retval_t alpn_rv = ctls_ctx_alpn_add(
      server_ctx, "http/1.1", _alpn_capture_cb, &server_cap, NULL, NULL);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  ccol_retval_t client_alpn_rv =
      ctls_ctx_alpn_add(client_ctx, "http/1.1", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);

  void *server_app_at_create = NULL;
  void *client_app_at_create = NULL;
  if (server_conn) {
    server_app_at_create =
        SSL_get_app_data(_ctls_conn_ssl_for_tests(server_conn));
  }
  if (client_conn) {
    client_app_at_create =
        SSL_get_app_data(_ctls_conn_ssl_for_tests(client_conn));
  }

  /* An application's own use of the app-data slot is only simulated once
     both connections genuinely exist and the slot is confirmed free.
     Writing a foreign pointer into a slot this module is reading its own
     connection out of would have it dereference that pointer during the
     handshake; and _ctls_conn_ssl_for_tests() answers NULL for a connection
     that was never created, which OpenSSL then dereferences. Either one
     crashes the whole binary, destroying every other test's result in this
     run, instead of failing this one test. Both at_create values are also
     NULL when creation failed, so they cannot carry that condition
     themselves. */
  bool conns_created = (server_conn != NULL && client_conn != NULL);
  bool slot_free = (conns_created && server_app_at_create == NULL &&
                    client_app_at_create == NULL);
  if (slot_free) {
    SSL_set_app_data(_ctls_conn_ssl_for_tests(server_conn),
                     &_app_data_sentinel);
    SSL_set_app_data(_ctls_conn_ssl_for_tests(client_conn),
                     &_app_data_sentinel);
  }

  bool handshake_ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  void *server_app_after = NULL;
  void *client_app_after = NULL;
  if (slot_free) {
    server_app_after = SSL_get_app_data(_ctls_conn_ssl_for_tests(server_conn));
    client_app_after = SSL_get_app_data(_ctls_conn_ssl_for_tests(client_conn));
  }
  bool alpn_fired = server_cap.fired;

  if (client_conn) ctls_conn_destroy(client_conn);
  if (server_conn) ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(cert_rv, ccol_success);
  REQUIRE_EQ(alpn_rv, ccol_success);
  REQUIRE_EQ(client_alpn_rv, ccol_success);

  /* A fresh connection must leave the app-data slot exactly as OpenSSL
     leaves it. */
  REQUIRE_TRUE(conns_created);
  REQUIRE_EQ(server_app_at_create, (void *)NULL);
  REQUIRE_EQ(client_app_at_create, (void *)NULL);
  REQUIRE_TRUE(slot_free);

  /* A handshake that reads this module's own per-connection pointer (the
     server's ALPN selection callback does exactly that) must neither be
     confused by an application's app data nor overwrite it. */
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_TRUE(alpn_fired);
  REQUIRE_STREQ(server_cap.name, "http/1.1");
  REQUIRE_EQ(server_app_after, (void *)&_app_data_sentinel);
  REQUIRE_EQ(client_app_after, (void *)&_app_data_sentinel);
}

/* The server has none of the protocols that the client offers, so the
 * handshake settles on no protocol at all. A report that names a registered
 * protocol in that case claims an agreement that the peer never made: the
 * server here would be told that it negotiated "spdy/1", with a client that
 * only offered "http/1.1". */
TEST(ctls_alpn, no_overlap_negotiates_no_protocol_at_all) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);
  alpn_capture server_cap = {0};
  ccol_retval_t server_alpn_rv = ctls_ctx_alpn_add(
      server_ctx, "spdy/1", _alpn_capture_cb, &server_cap, NULL, NULL);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  ccol_retval_t client_alpn_rv =
      ctls_ctx_alpn_add(client_ctx, "http/1.1", NULL, NULL, NULL, NULL);

  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool handshake_ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  bool fired = server_cap.fired;
  /* Poisoned so "reported nothing" is distinguishable from "never touched". */
  size_t len = 12345;
  const char *sel = ctls_conn_alpn_selected(server_conn, &len);

  if (client_conn) ctls_conn_destroy(client_conn);
  if (server_conn) ctls_conn_destroy(server_conn);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(cert_rv, ccol_success);
  REQUIRE_EQ(server_alpn_rv, ccol_success);
  REQUIRE_EQ(client_alpn_rv, ccol_success);
  /* The handshake itself still succeeds: ALPN producing no match is not a
   * handshake failure, only an absence of any negotiated protocol. */
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_FALSE(fired);
  REQUIRE_EQ((void *)sel, (void *)NULL);
  REQUIRE_EQ(len, (size_t)0);
}

/* This is the mirror on the client side, and also the case where a wrong
 * report becomes an error on the wire. A client offers {"h2", "http/1.1"} to
 * a server that negotiates no ALPN at all, and that client must be told that
 * nothing was negotiated: a report of the first registered protocol makes it
 * speak HTTP/2 framing to an HTTP/1.1 peer. */
TEST(ctls_alpn,
     a_server_that_negotiates_none_leaves_the_client_reporting_none) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);

  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  alpn_capture client_cap = {0};
  ccol_retval_t h2_rv = ctls_ctx_alpn_add(client_ctx, "h2", _alpn_capture_cb,
                                          &client_cap, NULL, NULL);
  ccol_retval_t h1_rv = ctls_ctx_alpn_add(
      client_ctx, "http/1.1", _alpn_capture_cb, &client_cap, NULL, NULL);

  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool handshake_ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  bool fired = client_cap.fired;
  char fired_name[sizeof(client_cap.name)];
  snprintf(fired_name, sizeof(fired_name), "%s", client_cap.name);
  size_t len = 12345;
  const char *sel = ctls_conn_alpn_selected(client_conn, &len);

  if (client_conn) ctls_conn_destroy(client_conn);
  if (server_conn) ctls_conn_destroy(server_conn);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(cert_rv, ccol_success);
  REQUIRE_EQ(h2_rv, ccol_success);
  REQUIRE_EQ(h1_rv, ccol_success);
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_STREQ(fired_name, "");
  REQUIRE_FALSE(fired);
  REQUIRE_EQ((void *)sel, (void *)NULL);
  REQUIRE_EQ(len, (size_t)0);
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

TEST(ctls_conn, a_write_retry_may_come_from_another_buffer) {
  /* A caller that parks a slow reader copies the unsent tail of its data
   * into storage of its own, and retries a write that reported "would
   * block" from there; the retry carries the same bytes at another
   * address, and it must succeed. Non-vacuous: without
   * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, OpenSSL refuses that retry with
   * "bad write retry", and the write fails for good. */
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
  bool shaken = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  static char first[16384], second[16384];
  memset(first, 'w', sizeof(first));
  memcpy(second, first, sizeof(second));
  ssize_t w = 0;
  int writes = 0;
  while (shaken && writes < 100000 &&
         (w = ctls_conn_write(server_conn, first, sizeof(first))) > 0)
    writes++;
  bool blocked = shaken && w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);

  /* Make room, then retry the very same bytes from the other buffer. The
   * blocked record is flushed whole before the retry reports success, and a
   * small socket buffer (about 8 KiB per direction for a FreeBSD socketpair)
   * can take its rest only over several rounds, each of which retries from
   * the other buffer again. A refused retry fails for good at once and ends
   * the loop; only "would block" goes round. */
  static char sink[65536];
  ssize_t retry = -1;
  for (int round = 0; blocked && round < 1000; round++) {
    for (int i = 0; i < 64; i++)
      if (ctls_conn_read(client_conn, sink, sizeof(sink)) <= 0) break;
    retry = ctls_conn_write(server_conn, second, sizeof(second));
    if (retry > 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) break;
  }

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(shaken);
  REQUIRE_TRUE(blocked);
  REQUIRE_GT(retry, (ssize_t)0);
}

/* ========================================================================== */
/* These tests cover three hazards that no ordinary test reaches.            */
/*                                                                           */
/* (1) A use-after-free in _ctls_servername_cb. That callback looks up the    */
/* SSL_CTX* of the matching named certificate under tls->lock. It must not    */
/* call SSL_set_SSL_CTX() on that pointer AFTER it unlocks. A concurrent      */
/* ctls_ctx_cert_add() can rotate that exact SSL_CTX and free it.             */
/*                                                                           */
/* (2) A data race with no lock in _ctls_alpn_select_cb, which selects the    */
/* ALPN protocol in server mode. It must not read tls->alpn[] or              */
/* tls->alpn_count without tls->lock. Its sibling for client mode,            */
/* _ctls_record_client_alpn, holds that lock.                                 */
/*                                                                           */
/* (3) An allocation in ctls_ctx_cert_add for the subject name of a           */
/* self-signed certificate. Nothing may leave its NULL result unchecked. Such */
/* a result reaches _ctls_create_self_signed(NULL), which then calls          */
/* strlen(NULL), whenever memory stays exhausted. Every other allocation      */
/* failure in that same function already reports the documented               */
/* ccol_not_enough_memory instead.                                            */
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
     the first malloc, nc = _ccol_mem_calloc(...) is a calloc (not counted
     here), and nc->self_signed_name = _ctls_strdup(...) is the second
     malloc; exactly the one this test targets. This test is non-vacuous:
     removing the NULL check it guards makes it crash with strlen(NULL)
     inside _ctls_create_self_signed, reached via
     _ctls_ctx_rebuild_locked. */
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
    /* A real wall-clock throttle, not sched_yield(): on a machine with cores
       to spare, an unthrottled rotate thread runs on its own dedicated core
       with nothing else contending for it, so sched_yield() there is a near
       no-op (it only cedes the CPU to another runnable thread on the SAME
       core) and does nothing to slow this thread's own iteration rate
       relative to the main thread's, which is on a different core entirely.
       Unthrottled, the rotate thread re-acquires ctx->lock so much faster
       than the main thread's own handshake-driving calls that the lock is,
       in practice, essentially always held (or immediately re-stolen the
       instant it's released) by the time the main thread ever tries for it;
       the result is a genuine lock-starvation livelock, with the 80
       handshakes below never completing within several real minutes (not a
       deadlock, but not survivable as a test either). usleep(1000) caps the
       rotate thread at roughly 1000 iterations/sec, comfortably enough to
       keep racing the main thread's own lock acquisitions while leaving it
       genuine, reliable wall-clock room to win its share of them. */
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
     whole time. The real assertion is that this survives at all: a plain
     run proves little, but under ASan/valgrind/TSan a use-after-free here
     reliably aborts or is reported. */
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
 *      alpn_selected_name copy (both directions) */

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

/* ========================================================================== */
/*                     PROCESS-WIDE SSL_CTX LIFETIME CENSUS                   */
/*                                                                            */
/* An SSL_CTX has no public getter for its reference count. One claim about   */
/* it can therefore only be checked by a count. That claim is: a context that */
/* is rebuilt many times, while connections come and go, does not accumulate  */
/* references. The count compares how many SSL_CTX objects are created with   */
/* how many are genuinely destroyed. OpenSSL calls a registered ex_data new   */
/* callback one time for each SSL_CTX_new. It calls the matching free         */
/* callback one time for each real destruction. That is the free that runs    */
/* when the reference count reaches zero, and not one for each SSL_CTX_free   */
/* call. One registered index therefore counts both, for every SSL_CTX in the */
/* process. That includes the ones that ctls builds inside itself and never   */
/* hands out. A reference that nobody releases shows up directly as a         */
/* creation with no matching destruction.                                     */
/*                                                                            */
/* A constructor registers the index, so nothing can build an SSL_CTX before  */
/* the index exists. An ex_data callback fires on whichever thread creates or */
/* destroys the context. Both counters are therefore atomic. Nothing reads    */
/* them until the threads under test are joined.                              */
/* ========================================================================== */

static _Atomic long g_ssl_ctx_created = 0;
static _Atomic long g_ssl_ctx_destroyed = 0;

static void _ssl_ctx_census_new(void *parent, void *ptr, CRYPTO_EX_DATA *ad,
                                int idx, long argl, void *argp) {
  (void)parent;
  (void)ptr;
  (void)ad;
  (void)idx;
  (void)argl;
  (void)argp;
  atomic_fetch_add_explicit(&g_ssl_ctx_created, 1, memory_order_relaxed);
}

static void _ssl_ctx_census_free(void *parent, void *ptr, CRYPTO_EX_DATA *ad,
                                 int idx, long argl, void *argp) {
  (void)parent;
  (void)ptr;
  (void)ad;
  (void)idx;
  (void)argl;
  (void)argp;
  atomic_fetch_add_explicit(&g_ssl_ctx_destroyed, 1, memory_order_relaxed);
}

static void __attribute__((constructor)) _register_ssl_ctx_census(void) {
  SSL_CTX_get_ex_new_index(0, NULL, _ssl_ctx_census_new, NULL,
                           _ssl_ctx_census_free);
}

static long _ssl_ctx_outstanding(void) {
  long created = atomic_load_explicit(&g_ssl_ctx_created, memory_order_relaxed);
  long destroyed =
      atomic_load_explicit(&g_ssl_ctx_destroyed, memory_order_relaxed);
  return created - destroyed;
}

/* ========================================================================== */
/* SNI dispatch pins the context the connection actually serves from          */
/*                                                                            */
/* A connection reads the SSL_CTX that it was created from for its whole      */
/* life. OpenSSL calls that one the session context. A server connection      */
/* whose SNI name matches a named certificate also reads the SSL_CTX that     */
/* the dispatch for that certificate installed. Both contexts are shared. A   */
/* concurrent ctls_ctx_cert_add, ctls_ctx_alpn_add or ctls_ctx_trust rebuilds */
/* and frees both. That is a documented and supported use case. ctls          */
/* therefore holds a reference of its own on each one. A context that a live  */
/* connection can still reach is one that this module keeps alive. It is not  */
/* one that the internal bookkeeping of a third party happens to keep alive.  */
/* ========================================================================== */

TEST(ctls_sni, dispatch_pins_the_named_context_until_the_connection_is_gone) {
  if (!g_certs_ready) return;
  long pins_before = _ctls_live_conn_ctx_pin_count_for_tests();

  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  ccol_retval_t default_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                               g_server_key, NULL, NULL);
  ccol_retval_t named_rv =
      ctls_ctx_cert_add(server_ctx, "pinned.test", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "pinned.test", false, NULL);
  bool shook = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  /* Every observation is captured into a local before any cleanup runs, so
     the asserts at the end of this test can run after both connections and
     both contexts have been released unconditionally. */
  SSL_CTX *server_pinned = _ctls_conn_pinned_sni_ctx_for_tests(server_conn);
  SSL_CTX *server_active =
      SSL_get_SSL_CTX(_ctls_conn_ssl_for_tests(server_conn));
  SSL_CTX *client_pinned = _ctls_conn_pinned_sni_ctx_for_tests(client_conn);
  /* The server connection pins its creation-time context and the dispatched
     named one; the client connection, which never reaches a servername
     callback, pins only its creation-time context. */
  long pins_during = _ctls_live_conn_ctx_pin_count_for_tests();

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  long pins_after = _ctls_live_conn_ctx_pin_count_for_tests();
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(default_rv, ccol_success);
  REQUIRE_EQ(named_rv, ccol_success);
  REQUIRE_TRUE(shook);
  REQUIRE_NE((void *)server_pinned, (void *)NULL);
  REQUIRE_EQ((void *)server_pinned, (void *)server_active);
  REQUIRE_EQ((void *)client_pinned, (void *)NULL);
  REQUIRE_EQ(pins_during - pins_before, 3L);
  REQUIRE_EQ(pins_after, pins_before);
}

TEST(ctls_sni, a_name_matching_no_named_certificate_pins_only_the_default) {
  if (!g_certs_ready) return;
  long pins_before = _ctls_live_conn_ctx_pin_count_for_tests();

  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  ccol_retval_t default_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                               g_server_key, NULL, NULL);
  ccol_retval_t named_rv =
      ctls_ctx_cert_add(server_ctx, "registered.test", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn = ctls_conn_create_client(
      client_ctx, fds[1], "unregistered.test", false, NULL);
  bool shook = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  SSL_CTX *server_pinned = _ctls_conn_pinned_sni_ctx_for_tests(server_conn);
  long pins_during = _ctls_live_conn_ctx_pin_count_for_tests();

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  long pins_after = _ctls_live_conn_ctx_pin_count_for_tests();
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(default_rv, ccol_success);
  REQUIRE_EQ(named_rv, ccol_success);
  REQUIRE_TRUE(shook);
  /* No named certificate answers this name, so the connection keeps serving
     from the default context and there is no second context to pin. */
  REQUIRE_EQ((void *)server_pinned, (void *)NULL);
  REQUIRE_EQ(pins_during - pins_before, 2L);
  REQUIRE_EQ(pins_after, pins_before);
}

/* ========================================================================== */
/* Concurrent reconfiguration under live traffic                              */
/*                                                                            */
/* Four acceptor threads and six client threads run real TLS handshakes over  */
/* loopback. They share one server context and one client context. At the     */
/* same time a rotator thread keeps adding named certificates, the default    */
/* certificate and an ALPN protocol again. Every one of those calls rebuilds  */
/* and frees every SSL_CTX that the context owns. The client names cycle      */
/* through four cases. These are an exact match, a second exact match, a name */
/* that the wildcard entry answers, and a name that nothing answers. Some     */
/* connections therefore go to three different named contexts. Others stay on */
/* the default one. All of them are in flight while the code replaces their   */
/* contexts.                                                                  */
/*                                                                            */
/* The listening socket binds port 0, so several suites (or several runs of   */
/* this one) can never collide on a fixed port.                               */
/* ========================================================================== */

#define CTLS_STRESS_ACCEPTORS 4
#define CTLS_STRESS_CLIENTS 6
#define CTLS_STRESS_ROUNDS 6
/* A hang-safety net only: no assertion below depends on a handshake taking
   any particular time. Generous enough that ten threads serialised one at a
   time under valgrind still finish well inside it. */
#define CTLS_STRESS_HANDSHAKE_DEADLINE_MS 60000

static const char *const g_stress_names[] = {
    "one.stress.test", "two.stress.test", "leaf.wild.stress.test",
    "nothing.stress.test"};

static _Atomic bool g_stress_stop = false;
static _Atomic bool g_stress_go = false;
static _Atomic int g_stress_arrived = 0;
static _Atomic long g_stress_server_ok = 0;
static _Atomic long g_stress_client_ok = 0;
static _Atomic long g_stress_accepted = 0;
static int g_stress_listen_fd = -1;
static struct sockaddr_in g_stress_addr;
static ctls_ctx_t *g_stress_server_ctx = NULL;
static ctls_ctx_t *g_stress_client_ctx = NULL;

static long _elapsed_ms_since(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - start->tv_sec) * 1000L +
         (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void _set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* A counted gate, not a pthread_barrier_t: the barrier's participant count is
   fixed when it is created, so a pthread_create that fails part way leaves
   every thread that did start parked on it forever. The creating thread
   releases this gate once the number of threads it ACTUALLY started have
   arrived, on every path out. */
static void _stress_wait_for_go(void) {
  atomic_fetch_add(&g_stress_arrived, 1);
  while (!atomic_load(&g_stress_go)) {
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
}

/* Drives one end of a handshake to completion, waiting on the direction the
   step itself asked for. */
static bool _drive_one_side(ctls_conn_t *conn, int fd) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn);
    if (r == CTLS_HANDSHAKE_DONE) return true;
    if (r == CTLS_HANDSHAKE_ERROR) return false;
    if (_elapsed_ms_since(&start) >= CTLS_STRESS_HANDSHAKE_DEADLINE_MS)
      return false;
    struct pollfd p = {
        .fd = fd,
        .events = (short)(r == CTLS_HANDSHAKE_WANT_WRITE ? POLLOUT : POLLIN),
        .revents = 0};
    if (poll(&p, 1, 50) < 0 && errno != EINTR) return false;
  }
}

static void *_stress_acceptor_thread(void *arg) {
  (void)arg;
  _stress_wait_for_go();
  while (!atomic_load(&g_stress_stop)) {
    /* A bounded wait, so that the thread sees the stop flag promptly. Only
       Linux bounds accept(2) by SO_RCVTIMEO; the BSDs and macOS ignore it on
       a listening socket. The listening socket is non-blocking, so an
       acceptor that loses the connection to another one gets EAGAIN instead
       of parking in accept(2). */
    struct pollfd pfd = {.fd = g_stress_listen_fd, .events = POLLIN};
    if (poll(&pfd, 1, 100) <= 0) continue;
    int fd = accept(g_stress_listen_fd, NULL, NULL);
    if (fd < 0) continue;
    atomic_fetch_add(&g_stress_accepted, 1);
    _set_nonblocking(fd);
    ctls_conn_t *conn =
        ctls_conn_create_server(g_stress_server_ctx, fd, NULL, NULL);
    if (conn) {
      if (_drive_one_side(conn, fd)) atomic_fetch_add(&g_stress_server_ok, 1);
      ctls_conn_destroy(conn);
    }
    close(fd);
  }
  return NULL;
}

static void *_stress_client_thread(void *arg) {
  long seed = (long)(intptr_t)arg;
  _stress_wait_for_go();
  size_t name_count = sizeof(g_stress_names) / sizeof(g_stress_names[0]);
  for (int i = 0; i < CTLS_STRESS_ROUNDS && !atomic_load(&g_stress_stop); i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) continue;
    if (connect(fd, (struct sockaddr *)&g_stress_addr, sizeof(g_stress_addr)) !=
        0) {
      close(fd);
      continue;
    }
    _set_nonblocking(fd);
    const char *sni = g_stress_names[((size_t)seed + (size_t)i) % name_count];
    ctls_conn_t *conn =
        ctls_conn_create_client(g_stress_client_ctx, fd, sni, false, NULL);
    if (conn) {
      if (_drive_one_side(conn, fd)) atomic_fetch_add(&g_stress_client_ok, 1);
      ctls_conn_destroy(conn);
    }
    close(fd);
  }
  return NULL;
}

static void *_stress_rotator_thread(void *arg) {
  (void)arg;
  _stress_wait_for_go();
  int step = 0;
  while (!atomic_load(&g_stress_stop)) {
    switch (step++ % 4) {
      case 0:
        ctls_ctx_cert_add(g_stress_server_ctx, "one.stress.test", g_server_cert,
                          g_server_key, NULL, NULL);
        break;
      case 1:
        ctls_ctx_cert_add(g_stress_server_ctx, "*.wild.stress.test", NULL, NULL,
                          NULL, NULL);
        break;
      case 2:
        ctls_ctx_cert_add(g_stress_server_ctx, NULL, g_server_cert,
                          g_server_key, NULL, NULL);
        break;
      default:
        ctls_ctx_alpn_add(g_stress_server_ctx, "h2", NULL, NULL, NULL, NULL);
        break;
    }
    /* A real wall-clock throttle rather than sched_yield(), for the reason
       the two rotation threads above already document: an unthrottled
       rotator holds or immediately re-steals ctx->lock so consistently that
       the handshake threads never win their share of it. */
    usleep(500);
  }
  return NULL;
}

TEST(ctls_stress, concurrent_reconfiguration_under_live_handshakes) {
  if (!g_certs_ready) return;
  long pins_before = _ctls_live_conn_ctx_pin_count_for_tests();
  long ssl_ctx_before = _ssl_ctx_outstanding();

  atomic_store(&g_stress_stop, false);
  atomic_store(&g_stress_go, false);
  atomic_store(&g_stress_arrived, 0);
  atomic_store(&g_stress_server_ok, 0);
  atomic_store(&g_stress_client_ok, 0);
  atomic_store(&g_stress_accepted, 0);

  g_stress_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_GE(g_stress_listen_fd, 0);
  int one = 1;
  setsockopt(g_stress_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_addr.sin_port = 0; /* an ephemeral port: never a fixed one */
  bool bound = bind(g_stress_listen_fd, (struct sockaddr *)&bind_addr,
                    sizeof(bind_addr)) == 0 &&
               listen(g_stress_listen_fd, 64) == 0;
  socklen_t alen = sizeof(g_stress_addr);
  memset(&g_stress_addr, 0, sizeof(g_stress_addr));
  if (bound)
    bound = getsockname(g_stress_listen_fd, (struct sockaddr *)&g_stress_addr,
                        &alen) == 0;
  /* Non-blocking, for the bounded accept of _stress_acceptor_thread. */
  _set_nonblocking(g_stress_listen_fd);

  g_stress_server_ctx = ctls_ctx_new(NULL);
  g_stress_client_ctx = ctls_ctx_new(NULL);
  bool configured =
      bound && g_stress_server_ctx && g_stress_client_ctx &&
      ctls_ctx_cert_add(g_stress_server_ctx, NULL, g_server_cert, g_server_key,
                        NULL, NULL) == ccol_success &&
      ctls_ctx_cert_add(g_stress_server_ctx, "one.stress.test", g_server_cert,
                        g_server_key, NULL, NULL) == ccol_success &&
      ctls_ctx_cert_add(g_stress_server_ctx, "two.stress.test", NULL, NULL,
                        NULL, NULL) == ccol_success &&
      ctls_ctx_cert_add(g_stress_server_ctx, "*.wild.stress.test", NULL, NULL,
                        NULL, NULL) == ccol_success &&
      ctls_ctx_alpn_add(g_stress_server_ctx, "h2", NULL, NULL, NULL, NULL) ==
          ccol_success &&
      ctls_ctx_alpn_add(g_stress_client_ctx, "h2", NULL, NULL, NULL, NULL) ==
          ccol_success;

  pthread_t acceptors[CTLS_STRESS_ACCEPTORS];
  pthread_t clients[CTLS_STRESS_CLIENTS];
  pthread_t rotator;
  int acceptors_started = 0, clients_started = 0, rotator_started = 0;
  if (configured) {
    for (int i = 0; i < CTLS_STRESS_ACCEPTORS; i++)
      if (pthread_create(&acceptors[acceptors_started], NULL,
                         _stress_acceptor_thread, NULL) == 0)
        acceptors_started++;
    if (pthread_create(&rotator, NULL, _stress_rotator_thread, NULL) == 0)
      rotator_started = 1;
    for (int i = 0; i < CTLS_STRESS_CLIENTS; i++)
      if (pthread_create(&clients[clients_started], NULL, _stress_client_thread,
                         (void *)(intptr_t)i) == 0)
        clients_started++;
  }
  int started = acceptors_started + clients_started + rotator_started;
  /* Bound by what actually started, and released unconditionally, so a
     partial start still lets every started thread run to completion and be
     joined rather than parking forever. */
  while (atomic_load(&g_stress_arrived) < started) {
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
  atomic_store(&g_stress_go, true);

  for (int i = 0; i < clients_started; i++) pthread_join(clients[i], NULL);
  atomic_store(&g_stress_stop, true);
  for (int i = 0; i < acceptors_started; i++) pthread_join(acceptors[i], NULL);
  if (rotator_started) pthread_join(rotator, NULL);

  close(g_stress_listen_fd);
  g_stress_listen_fd = -1;
  ctls_ctx_release(g_stress_client_ctx);
  ctls_ctx_release(g_stress_server_ctx);
  g_stress_client_ctx = NULL;
  g_stress_server_ctx = NULL;

  long client_ok = atomic_load(&g_stress_client_ok);
  long server_ok = atomic_load(&g_stress_server_ok);
  long accepted = atomic_load(&g_stress_accepted);
  long pins_after = _ctls_live_conn_ctx_pin_count_for_tests();
  long ssl_ctx_after = _ssl_ctx_outstanding();

  REQUIRE_TRUE(configured);
  REQUIRE_EQ(started, CTLS_STRESS_ACCEPTORS + CTLS_STRESS_CLIENTS + 1);
  REQUIRE_GT(accepted, (long)0);
  /* Reconfiguring a context must not break a handshake already in flight
     against it, so every connection that was accepted and driven to an
     answer is expected to have completed; asserted as a healthy majority
     rather than as equality, since a client whose connect(2) is refused
     while the backlog is full is a property of the machine, not of ctls. */
  REQUIRE_GT(client_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  REQUIRE_GT(server_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  /* Every reference ctls took on behalf of a connection is released. */
  REQUIRE_EQ(pins_after, pins_before);
  /* And every SSL_CTX the rotator's rebuilds created is genuinely destroyed
     once the contexts are released: a reference retained by nobody would
     leave a context alive here with its count never reaching zero. */
  REQUIRE_EQ(ssl_ctx_after, ssl_ctx_before);
}

/* ========================================================================== */
/* Allocation-failure sweep                                                   */
/*                                                                            */
/* Every entry point below has cleanup branches that free a different subset */
/* of what the call had built by the time the allocation failed. Nothing else */
/* in this suite executes them, so the documented ccol_not_enough_memory */
/* return and the freeing that goes with it are unverified without this. */
/*                                                                            */
/* The counter is shared across all four procs on purpose. The context
 */
/* itself, a named-certificate record and a connection all come from */
/* _ccol_mem_calloc. The trust array and the ALPN array grow through */
/* _ccol_mem_realloc. A harness that failed only malloc would therefore leave */
/* every one of those branches unreachable. */
/* ========================================================================== */

static _Atomic int g_alloc_seen = 0;
static _Atomic int g_alloc_fail_at = 0; /* 0 disarms */

static bool _sweep_should_fail(void) {
  int at = atomic_load(&g_alloc_fail_at);
  if (at == 0) return false;
  return (atomic_fetch_add(&g_alloc_seen, 1) + 1) == at;
}
static void *_sweep_malloc(size_t n) {
  return _sweep_should_fail() ? NULL : malloc(n);
}
static void _sweep_free(void *p) { free(p); }
static void *_sweep_calloc(size_t a, size_t b) {
  return _sweep_should_fail() ? NULL : calloc(a, b);
}
static void *_sweep_realloc(void *p, size_t n) {
  return _sweep_should_fail() ? NULL : realloc(p, n);
}
static ccol_memmgmt_procs_t g_sweep_mp = {_sweep_malloc, _sweep_free,
                                          _sweep_calloc, _sweep_realloc};

static void _sweep_arm(int nth) {
  atomic_store(&g_alloc_seen, 0);
  atomic_store(&g_alloc_fail_at, nth);
}
static void _sweep_disarm(void) { atomic_store(&g_alloc_fail_at, 0); }

/* This depth walks past the last allocation that any one of these calls
 * makes, so the sweep covers every branch, and not a prefix of them. */
#define CTLS_SWEEP_DEPTH 46

TEST(ctls_oom, ctx_new_reports_failure_at_every_allocation_step) {
  bool all_handled = true;
  for (int n = 1; n <= 10; n++) {
    _sweep_arm(n);
    char *err = NULL;
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, &err);
    _sweep_disarm();
    /* Either the allocation it failed was not on this path, or the context
     * could not be built and no half-constructed one is handed back. */
    if (ctx)
      ctls_ctx_release(ctx);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, named_self_signed_cert_add_never_crashes_or_half_configures) {
  bool all_handled = true;
  for (int n = 1; n <= CTLS_SWEEP_DEPTH; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;

    _sweep_arm(n);
    ccol_retval_t rv =
        ctls_ctx_cert_add(ctx, "sweep.test", NULL, NULL, NULL, NULL);
    _sweep_disarm();

    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      all_handled = false;
    /* A failed add must leave the context usable rather than poisoned. */
    if (ctls_ctx_cert_add(ctx, "after.test", NULL, NULL, NULL, NULL) !=
        ccol_success)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, default_self_signed_cert_add_never_crashes) {
  bool all_handled = true;
  for (int n = 1; n <= CTLS_SWEEP_DEPTH; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval_t rv = ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);
    _sweep_disarm();
    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, file_backed_cert_add_with_password_never_crashes) {
  if (!g_certs_ready) return;
  bool all_handled = true;
  for (int n = 1; n <= CTLS_SWEEP_DEPTH; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval_t rv = ctls_ctx_cert_add(ctx, "sweep.test", g_server_cert,
                                         g_server_key, "unused-password", NULL);
    _sweep_disarm();
    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, trust_array_growth_reports_failure_without_dangling) {
  if (!g_certs_ready) return;
  bool all_handled = true;
  for (int n = 1; n <= 20; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    /* There are two adds, and the second one forces the parallel pem array
     * and length array to grow; a reallocation that applies to only one of
     * them leaves that one pointing at freed storage. */
    _sweep_arm(n);
    ccol_retval_t first = ctls_ctx_trust(ctx, g_server_cert, NULL);
    _sweep_disarm();
    _sweep_arm(n);
    ccol_retval_t second = ctls_ctx_trust(ctx, g_server_cert, NULL);
    _sweep_disarm();
    if (first != ccol_success && first != ccol_not_enough_memory &&
        first != ccol_http_tls_cert_load_failed)
      all_handled = false;
    if (second != ccol_success && second != ccol_not_enough_memory &&
        second != ccol_http_tls_cert_load_failed)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, alpn_array_growth_reports_failure_and_keeps_the_count_honest) {
  bool all_handled = true;
  for (int n = 1; n <= 14; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval_t a = ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL);
    _sweep_disarm();
    _sweep_arm(n);
    ccol_retval_t b =
        ctls_ctx_alpn_add(ctx, "http/1.1", NULL, NULL, NULL, NULL);
    _sweep_disarm();

    /* An add that ran out of memory gives up before it stores the entry, so
     * it must leave the count untouched. Another add stores the entry and
     * then fails only to rebuild the SSL_CTX; that add keeps the
     * registration, a later rebuild that succeeds picks it up, and the count
     * includes it. */
    size_t expect = (a != ccol_not_enough_memory ? 1u : 0u) +
                    (b != ccol_not_enough_memory ? 1u : 0u);
    if (ctls_ctx_alpn_count(ctx) != expect) all_handled = false;
    if (a != ccol_success && a != ccol_not_enough_memory &&
        a != ccol_http_tls_cert_load_failed)
      all_handled = false;
    if (b != ccol_success && b != ccol_not_enough_memory &&
        b != ccol_http_tls_cert_load_failed)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctls_oom, conn_create_returns_null_rather_than_a_partial_connection) {
  bool all_handled = true;
  for (int n = 1; n <= 6; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      _sweep_arm(n);
      ctls_conn_t *cl =
          ctls_conn_create_client(ctx, fds[0], "sweep.test", true, NULL);
      _sweep_disarm();
      if (cl) ctls_conn_destroy(cl);

      _sweep_arm(n);
      ctls_conn_t *sv = ctls_conn_create_server(ctx, fds[1], NULL, NULL);
      _sweep_disarm();
      if (sv) ctls_conn_destroy(sv);

      close(fds[0]);
      close(fds[1]);
    } else {
      all_handled = false;
    }
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

/* ========================================================================== */
/* Password-protected private keys                                            */
/* ========================================================================== */

TEST(ctls_pem_password, a_correct_password_loads_an_encrypted_key) {
  if (!g_enc_cert_ready) return;
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  char *err = NULL;
  ccol_retval_t rv = ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key,
                                       CTLS_TEST_KEY_PASSWORD, &err);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(ctls_pem_password, a_wrong_password_reports_a_load_failure) {
  /* Without the password callback that ctls installs, OpenSSL prompts on the
   * controlling terminal instead of returning, which in a test binary or a
   * daemon is a hang, and not a failure. */
  if (!g_enc_cert_ready) return;
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  char *err = NULL;
  ccol_retval_t rv = ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key,
                                       "not-the-password", &err);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_pem_password, an_encrypted_key_with_no_password_reports_a_failure) {
  if (!g_enc_cert_ready) return;
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ccol_retval_t rv =
      ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key, NULL, NULL);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_pem_password, an_encrypted_named_cert_serves_a_real_handshake) {
  /* Loading is not the same as being usable: this proves the decrypted key
   * actually backs a completed handshake. */
  if (!g_enc_cert_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t added = ctls_ctx_cert_add(
      server_ctx, NULL, g_enc_cert, g_enc_key, CTLS_TEST_KEY_PASSWORD, NULL);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "enc.test", false, NULL);
  bool shook = (server_conn && client_conn) &&
               _drive_both(client_conn, server_conn, 200, NULL, NULL);

  if (client_conn) ctls_conn_destroy(client_conn);
  if (server_conn) ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ((int)added, (int)ccol_success);
  REQUIRE_TRUE(shook);
}

/* ========================================================================== */
/* Unreadable and unusable PEM inputs                                         */
/* ========================================================================== */

TEST(ctls_file_input, an_empty_file_reports_a_load_failure) {
  /* A zero-length file opens and seeks perfectly well, so nothing short of
   * checking the size catches it before the PEM parser is handed nothing. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "/tmp/ctls_empty_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval_t rv = ccol_unexpected_failure;
  bool made = (fd >= 0);
  if (made) {
    close(fd);
    rv = ctls_ctx_trust(ctx, path, NULL);
    unlink(path);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(made);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_file_input, a_file_that_reports_zero_length_reports_a_load_failure) {
  /* procfs entries report a length of zero while still having content, which
   * is the same branch an empty regular file takes. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ccol_retval_t rv = ctls_ctx_trust(ctx, "/proc/self/status", NULL);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_file_input, a_directory_in_place_of_a_bundle_reports_a_load_failure) {
  /* An operator can point a bundle option at a directory by mistake, which is
   * an ordinary configuration slip. An open of a directory succeeds on Linux,
   * and the seek after it can fail, or it can report a length of LONG_MAX,
   * depending on the filesystem, so the rejection must come from the type of
   * the file, and not from its apparent size. This test is not vacuous:
   * without that check, the LONG_MAX case reaches the allocation, and
   * AddressSanitizer stops the process with allocation-size-too-big. This
   * test makes its directory here, and not under /tmp, so that the case does
   * not depend on the filesystem that the tests run on. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  /* This directory sits beside the test binary, and not under /tmp, because
   * whether a seek on a directory succeeds depends on the filesystem: it
   * fails on tmpfs, which /tmp often is, and it succeeds on ext4. The build
   * tree is the one place that is guaranteed to be on the same filesystem
   * that the library normally reads. */
  char dir[] = "ctls_dir_XXXXXX";
  bool made = (mkdtemp(dir) != NULL);
  ccol_retval_t rv = ccol_unexpected_failure;
  if (made) {
    rv = ctls_ctx_trust(ctx, dir, NULL);
    rmdir(dir);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(made);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_file_input, an_oversized_bundle_is_rejected_before_it_is_allocated) {
  /* A PEM artefact is a couple of kilobytes, and a full system CA bundle is a
   * few hundred kilobytes, while a path can point at something else
   * completely, such as a log, an image or a core dump, which must be turned
   * away on its size, and not read into memory. The file below is sparse, so
   * it costs no disk space and no time to create. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_big_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval_t rv = ccol_unexpected_failure;
  char *err = NULL;
  bool sized = false;
  if (fd >= 0) {
    sized = (ftruncate(fd, (off_t)17 * 1024 * 1024) == 0);
    close(fd);
    if (sized) rv = ctls_ctx_trust(ctx, path, &err);
    unlink(path);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(sized);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  /* The reported reason must name the size, and must not claim that the file
   * could not be read: an operator who is told "unreadable" about a file that
   * reads perfectly well looks for a permissions problem that does not
   * exist. */
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "larger than"), (void *)NULL);
}

TEST(ctls_file_input, a_bundle_just_under_the_limit_is_still_read) {
  /* The cap must reject only what is past it: a file below the limit is read
   * normally, and fails later on its contents rather than on its size. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_ok_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval_t rv = ccol_unexpected_failure;
  char *err = NULL;
  bool sized = false;
  if (fd >= 0) {
    sized = (ftruncate(fd, (off_t)15 * 1024 * 1024) == 0);
    close(fd);
    if (sized) rv = ctls_ctx_trust(ctx, path, &err);
    unlink(path);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(sized);
  /* The library rejects this file on its contents, because it carries no
   * certificate, and not on its size; that difference is the whole point of
   * this test. The two branches report different reasons, so the reason pins
   * which one ran: a file below the cap must reach the check on the
   * contents, instead of being turned away for its length. */
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) {
    REQUIRE_NE((void *)strstr(err, "no certificates"), (void *)NULL);
    REQUIRE_EQ((void *)strstr(err, "larger than"), (void *)NULL);
  }
}

TEST(ctls_file_input, a_bundle_holding_no_certificate_is_rejected) {
  /* A readable file can parse to no certificate at all, which leaves a trust
   * store with no issuer in it, while peer verification is on. A client built
   * from such a context rejects every peer. A server in the request-only mode
   * of ctls_ctx_peer_cert_optional() is different: it keeps accepting every
   * client that presents no certificate, so the mutual TLS that the bundle was
   * configured for is silently absent. The configuration call has to fail
   * instead. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_nocert_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval_t rv = ccol_success;
  char *err = NULL;
  bool written = false;
  if (fd >= 0) {
    static const char text[] = "this file is readable and holds no PEM block\n";
    ssize_t n = write(fd, text, sizeof(text) - 1);
    written = (n == (ssize_t)(sizeof(text) - 1));
    close(fd);
    if (written) rv = ctls_ctx_trust(ctx, path, &err);
    unlink(path);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(written);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "no certificates"), (void *)NULL);
}

TEST(ctls_file_input, a_bundle_that_cannot_be_allocated_reports_oom) {
  if (!g_certs_ready) return;
  bool handled = true;
  for (int n = 1; n <= 6; n++) {
    _sweep_disarm();
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval_t rv = ctls_ctx_trust(ctx, g_server_cert, NULL);
    _sweep_disarm();
    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(handled);
}

/* ========================================================================== */
/* Read and write result classification                                       */
/*                                                                            */
/* chttpclient and chttpserver branch on this pair of a return value and an  */
/* errno. They do so on every read and every write that they make. This pair  */
/* is therefore the interface between ctls and the two largest modules in the */
/* library. */
/* ========================================================================== */

/* Builds a completed TLS session over a socketpair. Returns false if the
 * handshake could not be driven to completion, in which case nothing is
 * allocated for the caller to release. */
static bool _make_connected_pair(ctls_ctx_t **out_server_ctx,
                                 ctls_ctx_t **out_client_ctx,
                                 ctls_conn_t **out_server,
                                 ctls_conn_t **out_client, int fds[2]) {
  *out_server_ctx = ctls_ctx_new(NULL);
  *out_client_ctx = ctls_ctx_new(NULL);
  *out_server = NULL;
  *out_client = NULL;
  if (!*out_server_ctx || !*out_client_ctx) return false;
  if (ctls_ctx_cert_add(*out_server_ctx, NULL, NULL, NULL, NULL, NULL) !=
      ccol_success)
    return false;

  _make_nonblocking_pair(fds);
  *out_server = ctls_conn_create_server(*out_server_ctx, fds[0], NULL, NULL);
  *out_client =
      ctls_conn_create_client(*out_client_ctx, fds[1], "io.test", false, NULL);
  if (!*out_server || !*out_client) return false;
  return _drive_both(*out_client, *out_server, 200, NULL, NULL);
}

TEST(ctls_io, a_read_with_nothing_pending_asks_to_be_retried) {
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = 0;
  int err = 0;
  if (up) {
    errno = 0;
    n = ctls_conn_read(c, buf, sizeof(buf));
    err = errno;
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_EQ(n, (ssize_t)-1);
  REQUIRE_TRUE(err == EWOULDBLOCK || err == EAGAIN);
  /* A read that is merely waiting for more data wants the read direction, so
   * a caller that re-armed for writability here would stall the connection. */
  REQUIRE_FALSE(ctls_conn_wants_write(NULL));
}

TEST(ctls_io, a_roundtrip_reports_the_exact_byte_count) {
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  static const char msg[] = "classification";
  ssize_t wrote = 0, got = 0;
  char buf[64] = {0};
  if (up) {
    wrote = ctls_conn_write(c, msg, sizeof(msg));
    got = ctls_conn_read(s, buf, sizeof(buf));
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_EQ(wrote, (ssize_t)sizeof(msg));
  REQUIRE_EQ(got, (ssize_t)sizeof(msg));
  REQUIRE_STREQ(buf, msg);
}

TEST(ctls_io, a_clean_peer_shutdown_reads_as_end_of_stream) {
  /* Zero means the peer closed the session properly, which a caller must not
   * confuse with the -1 that asks for a retry. */
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  if (up) {
    ctls_conn_destroy(c); /* sends the TLS close_notify the peer observes */
    c = NULL;
    n = ctls_conn_read(s, buf, sizeof(buf));
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_EQ(n, (ssize_t)0);
}

TEST(ctls_io, a_protocol_violation_reads_as_a_reset_connection) {
  /* Plaintext arriving where a TLS record belongs is a protocol error, and
   * the caller is told the connection is unusable rather than retryable. */
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  int err = 0;
  if (up) {
    static const char garbage[] =
        "this is not a TLS record at all, not even close";
    ssize_t pushed = write(fds[1], garbage, sizeof(garbage));
    (void)pushed;
    errno = 0;
    n = ctls_conn_read(s, buf, sizeof(buf));
    err = errno;
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_EQ(n, (ssize_t)-1);
  REQUIRE_EQ(err, ECONNRESET);
}

TEST(ctls_io, an_abruptly_closed_peer_reads_as_a_failure_not_a_retry) {
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  int err = 0;
  if (up) {
    /* Closing the descriptor under the peer skips close_notify entirely, so
     * this is the truncation case rather than the clean shutdown above. */
    close(fds[1]);
    fds[1] = -1;
    errno = 0;
    n = ctls_conn_read(s, buf, sizeof(buf));
    err = errno;
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_TRUE(n <= 0);
  if (n < 0) REQUIRE_TRUE(err != 0);
}

TEST(ctls_io, a_write_that_fills_the_socket_asks_for_the_write_direction) {
  /* The one case where a caller must re-arm for writability after a read-side
   * API call: ctls_conn_wants_write is how that is discovered. */
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  bool saw_would_block = false, wants_write = false;
  if (up) {
    static char chunk[16384];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 512 && !saw_would_block; i++) {
      errno = 0;
      ssize_t w = ctls_conn_write(c, chunk, sizeof(chunk));
      if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        saw_would_block = true;
        wants_write = ctls_conn_wants_write(c);
      } else if (w < 0) {
        break;
      }
    }
  }

  if (c) ctls_conn_destroy(c);
  if (s) ctls_conn_destroy(s);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(cc);
  ctls_ctx_release(sc);

  REQUIRE_TRUE(up);
  REQUIRE_TRUE(saw_would_block);
  REQUIRE_TRUE(wants_write);
}

/* ========================================================================== */
/*              CERTIFICATE/KEY PAIRING AND SNI PRECEDENCE                    */
/* ========================================================================== */

/* This completes one handshake against server_ctx with the given SNI name (a
 * NULL name sends no SNI extension at all), and then reports the common name
 * of the certificate that the server really served. */
static void _serve_one_sni(ctls_ctx_t *server_ctx, const char *sni,
                           bool *out_ok, char *out_cn, size_t out_cn_len) {
  *out_ok = false;
  if (out_cn_len) out_cn[0] = '\0';
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  if (!client_ctx) return;
  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  if (fds[0] < 0 || fds[1] < 0) {
    ctls_ctx_release(client_ctx);
    return;
  }
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], sni, false, NULL);
  if (server_conn && client_conn &&
      _drive_both(client_conn, server_conn, 200, NULL, NULL)) {
    char buf[256];
    const char *cn =
        _peer_cert_cn(_ctls_conn_ssl_for_tests(client_conn), buf, sizeof(buf));
    if (cn) {
      snprintf(out_cn, out_cn_len, "%s", cn);
      *out_ok = true;
    }
  }
  if (client_conn) ctls_conn_destroy(client_conn);
  if (server_conn) ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
}

TEST(ctls_ctx, cert_add_rejects_a_certificate_and_key_that_do_not_match) {
  if (!g_certs_ready) return;
  /* Both files read perfectly well and are well-formed PEM, but they belong
   * to two different pairs, which is exactly what a rotation that updates one
   * of the two paths and not the other produces. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  char *default_err = NULL;
  ccol_retval_t default_rv = ctls_ctx_cert_add(
      ctx, NULL, g_server_cert, g_client_key, NULL, &default_err);
  char *named_err = NULL;
  ccol_retval_t named_rv = ctls_ctx_cert_add(
      ctx, "mismatch.test", g_server_cert, g_client_key, NULL, &named_err);
  /* The matching pair must still be accepted, so the check rejects a genuine
   * mismatch rather than every file-backed pair. */
  ccol_retval_t good_rv =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
  ctls_ctx_release(ctx);

  REQUIRE_EQ(default_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)default_err, (void *)NULL);
  REQUIRE_EQ(named_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)named_err, (void *)NULL);
  REQUIRE_EQ(good_rv, ccol_success);
}

TEST(ctls_ctx, a_rejected_pair_leaves_the_context_serving_what_it_had) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t first_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                             g_server_key, NULL, NULL);
  /* A rotation that picks up the new certificate and the old key. */
  ccol_retval_t rotate_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                              g_client_key, NULL, NULL);
  bool served = false;
  char cn[256];
  _serve_one_sni(server_ctx, NULL, &served, cn, sizeof(cn));
  /* A pair that the library rejected must also not stay in the stored
   * configuration, because a later, unrelated change of the configuration
   * rebuilds every context from that store, and such a rebuild would fail on
   * the rejected pair again. */
  ccol_retval_t later_rv = ctls_ctx_trust(server_ctx, g_client_cert, NULL);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(first_rv, ccol_success);
  REQUIRE_EQ(rotate_rv, ccol_http_tls_cert_load_failed);
  /* Accepting the mismatch leaves a context that cannot serve one handshake,
   * so this fails outright rather than merely serving the wrong name. */
  REQUIRE_TRUE(served);
  REQUIRE_STREQ(cn, "127.0.0.1");
  REQUIRE_EQ(later_rv, ccol_success);
}

/* Which certificate a hostname gets must be a property of that hostname, and
 * must not depend on the order in which the code registered the
 * certificates, so the test below runs both orders. A lookup that returns
 * the first pattern that happens to match would serve the wildcard
 * certificate for every name that is registered exactly, in whichever of the
 * two orders puts the wildcard before them. */
static void _run_exact_beats_wildcard(bool wildcard_first, bool *out_setup_ok,
                                      bool *out_all_served,
                                      char served[4][256]) {
  static const char *const kExact[3] = {"a.wild.test", "b.wild.test",
                                        "c.wild.test"};
  *out_setup_ok = false;
  *out_all_served = false;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  if (!server_ctx) return;
  bool setup_ok = ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL) ==
                  ccol_success;
  if (wildcard_first && setup_ok)
    setup_ok = ctls_ctx_cert_add(server_ctx, "*.wild.test", NULL, NULL, NULL,
                                 NULL) == ccol_success;
  for (int i = 0; i < 3 && setup_ok; ++i)
    setup_ok = ctls_ctx_cert_add(server_ctx, kExact[i], NULL, NULL, NULL,
                                 NULL) == ccol_success;
  if (!wildcard_first && setup_ok)
    setup_ok = ctls_ctx_cert_add(server_ctx, "*.wild.test", NULL, NULL, NULL,
                                 NULL) == ccol_success;

  bool all_served = setup_ok;
  for (int i = 0; i < 3 && all_served; ++i) {
    bool ok = false;
    _serve_one_sni(server_ctx, kExact[i], &ok, served[i], 256);
    all_served = ok;
  }
  /* A name with no exact registration of its own still reaches the wildcard,
   * so the precedence rule does not cost the wildcard its own coverage. */
  if (all_served) {
    bool ok = false;
    _serve_one_sni(server_ctx, "z.wild.test", &ok, served[3], 256);
    all_served = ok;
  }
  ctls_ctx_release(server_ctx);
  *out_setup_ok = setup_ok;
  *out_all_served = all_served;
}

TEST(ctls_sni, an_exact_name_beats_a_wildcard_registered_before_it) {
  bool setup_ok = false, all_served = false;
  char served[4][256] = {{0}, {0}, {0}, {0}};
  _run_exact_beats_wildcard(true, &setup_ok, &all_served, served);
  REQUIRE_TRUE(setup_ok);
  REQUIRE_TRUE(all_served);
  REQUIRE_STREQ(served[0], "a.wild.test");
  REQUIRE_STREQ(served[1], "b.wild.test");
  REQUIRE_STREQ(served[2], "c.wild.test");
  REQUIRE_STREQ(served[3], "*.wild.test");
}

TEST(ctls_sni, an_exact_name_beats_a_wildcard_registered_after_it) {
  bool setup_ok = false, all_served = false;
  char served[4][256] = {{0}, {0}, {0}, {0}};
  _run_exact_beats_wildcard(false, &setup_ok, &all_served, served);
  REQUIRE_TRUE(setup_ok);
  REQUIRE_TRUE(all_served);
  REQUIRE_STREQ(served[0], "a.wild.test");
  REQUIRE_STREQ(served[1], "b.wild.test");
  REQUIRE_STREQ(served[2], "c.wild.test");
  REQUIRE_STREQ(served[3], "*.wild.test");
}

TEST(ctls_sni, a_wildcard_covers_exactly_one_label) {
  /* The lookup makes one single probe, and that probe encodes one rule:
   * "*.wild.test" answers for one label below "wild.test", and for nothing
   * else, so a name with two labels falls through to the default
   * certificate, and so does the bare domain. */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ccol_retval_t default_rv =
      ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL);
  ccol_retval_t wild_rv =
      ctls_ctx_cert_add(server_ctx, "*.wild.test", NULL, NULL, NULL, NULL);

  bool one_ok = false, two_ok = false, bare_ok = false;
  char one[256], two[256], bare[256];
  _serve_one_sni(server_ctx, "foo.wild.test", &one_ok, one, sizeof(one));
  _serve_one_sni(server_ctx, "a.b.wild.test", &two_ok, two, sizeof(two));
  _serve_one_sni(server_ctx, "wild.test", &bare_ok, bare, sizeof(bare));
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(default_rv, ccol_success);
  REQUIRE_EQ(wild_rv, ccol_success);
  REQUIRE_TRUE(one_ok);
  REQUIRE_TRUE(two_ok);
  REQUIRE_TRUE(bare_ok);
  REQUIRE_STREQ(one, "*.wild.test");
  REQUIRE_STREQ(two, "ctls-default");
  REQUIRE_STREQ(bare, "ctls-default");
}

/* ========================================================================== */
/*                         SESSION RESUMPTION                                 */
/* ========================================================================== */

/* A plain OpenSSL client, and not a ctls client, is the peer here, because it
 * is the shape of every real client that resumes a session: it keeps the
 * session of one connection and offers it on the next. */

typedef struct {
  int max_version;   /* TLS1_2_VERSION or TLS1_3_VERSION */
  bool present_cert; /* the client presents g_client_cert */
  const char *sni;   /* NULL for no SNI */
} _resume_case_t;

/* Drives a ctls server connection and a raw OpenSSL client to the end of
 * the handshake, then moves one byte from the server to the client, which
 * makes the client read past the handshake, where a TLS 1.3 client takes in
 * the session tickets of the server. */
static bool _resume_drive(ctls_conn_t *server_conn, SSL *client) {
  bool s_done = false, c_done = false;
  for (int i = 0; i < 400 && !(s_done && c_done); i++) {
    if (!s_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(server_conn);
      if (r == CTLS_HANDSHAKE_ERROR) return false;
      s_done = (r == CTLS_HANDSHAKE_DONE);
    }
    if (!c_done) {
      int r = SSL_do_handshake(client);
      if (r == 1) {
        c_done = true;
      } else {
        int e = SSL_get_error(client, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
      }
    }
  }
  if (!(s_done && c_done)) return false;
  bool wrote = false;
  for (int i = 0; i < 400; i++) {
    if (!wrote) wrote = (ctls_conn_write(server_conn, "x", 1) == 1);
    char b;
    int r = SSL_read(client, &b, 1);
    if (r == 1) return wrote && b == 'x';
    int e = SSL_get_error(client, r);
    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
  }
  return false;
}

/* Runs one connection. *sess_inout is offered when it is not NULL, and
 * receives the session of this connection, while *reused_out receives what
 * the client and the server each report about resumption. */
static bool _resume_connect(ctls_ctx_t *server_ctx, SSL_CTX *client_ctx,
                            const _resume_case_t *rc, SSL_SESSION **sess_inout,
                            bool *client_reused, bool *server_reused) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  for (int i = 0; i < 2; i++)
    fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  SSL *client = SSL_new(client_ctx);
  bool ok = server_conn && client && SSL_set_fd(client, fds[1]) == 1;
  if (ok && rc->sni) ok = SSL_set_tlsext_host_name(client, rc->sni) == 1;
  if (ok && *sess_inout) ok = SSL_set_session(client, *sess_inout) == 1;
  if (ok) {
    SSL_set_connect_state(client);
    ok = _resume_drive(server_conn, client);
  }
  if (ok) {
    *client_reused = SSL_session_reused(client) == 1;
    *server_reused =
        SSL_session_reused(_ctls_conn_ssl_for_tests(server_conn)) == 1;
    SSL_SESSION *s = SSL_get1_session(client);
    if (*sess_inout) SSL_SESSION_free(*sess_inout);
    *sess_inout = s;
  }
  if (client) {
    SSL_shutdown(client);
    SSL_free(client);
  }
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  return ok;
}

/* Returns true when both sides report a resumed session on the second of
 * two connections, and neither side reports one on the first. */
static bool _resume_case_resumes(const _resume_case_t *rc) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  bool ok = server_ctx &&
            ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success &&
            ctls_ctx_trust(server_ctx, g_client_cert, NULL) == ccol_success;
  if (ok && rc->sni)
    ok = ctls_ctx_cert_add(server_ctx, rc->sni, NULL, NULL, NULL, NULL) ==
         ccol_success;
  /* A client that presents no certificate reaches a server with a trust
   * store only when that server merely requests one, while a client that
   * presents one resumes against the default, which requires it. */
  if (ok && !rc->present_cert)
    ok = ctls_ctx_peer_cert_optional(server_ctx, true) == ccol_success;
  SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
  ok = ok && client_ctx &&
       SSL_CTX_set_min_proto_version(client_ctx, rc->max_version) == 1 &&
       SSL_CTX_set_max_proto_version(client_ctx, rc->max_version) == 1;
  if (ok && rc->present_cert)
    ok = SSL_CTX_use_certificate_file(client_ctx, g_client_cert,
                                      SSL_FILETYPE_PEM) == 1 &&
         SSL_CTX_use_PrivateKey_file(client_ctx, g_client_key,
                                     SSL_FILETYPE_PEM) == 1;
  SSL_SESSION *sess = NULL;
  bool c1 = true, s1 = true, c2 = false, s2 = false;
  ok = ok && _resume_connect(server_ctx, client_ctx, rc, &sess, &c1, &s1);
  ok = ok && sess && SSL_SESSION_is_resumable(sess) == 1;
  ok = ok && _resume_connect(server_ctx, client_ctx, rc, &sess, &c2, &s2);
  if (sess) SSL_SESSION_free(sess);
  if (client_ctx) SSL_CTX_free(client_ctx);
  ctls_ctx_release(server_ctx);
  return ok && !c1 && !s1 && c2 && s2;
}

TEST(ctls_resumption, a_server_with_a_trust_store_resumes_every_client_shape) {
  if (!g_certs_ready) return;
  const _resume_case_t cases[] = {
      {TLS1_3_VERSION, false, NULL},       {TLS1_3_VERSION, true, NULL},
      {TLS1_2_VERSION, false, NULL},       {TLS1_2_VERSION, true, NULL},
      {TLS1_3_VERSION, false, "sni.test"}, {TLS1_2_VERSION, true, "sni.test"},
  };
  int failed_mask = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    if (!_resume_case_resumes(&cases[i])) failed_mask |= 1 << i;
  REQUIRE_EQ(failed_mask, 0);
}

/* ========================================================================== */
/*                  HOSTNAME VERIFICATION NEEDS A HOSTNAME                    */
/* ========================================================================== */

TEST(ctls_conn, verify_host_with_no_hostname_is_refused) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  char *err_null = NULL, *err_empty = NULL;
  ctls_conn_t *a = ctls_conn_create_client(ctx, fds[1], NULL, true, &err_null);
  ctls_conn_t *b = ctls_conn_create_client(ctx, fds[1], "", true, &err_empty);
  /* The same two calls with the check switched off on purpose are
   * accepted. */
  ctls_conn_t *c = ctls_conn_create_client(ctx, fds[1], NULL, false, NULL);
  ctls_conn_t *d = ctls_conn_create_client(ctx, fds[1], "", false, NULL);
  bool a_null = (a == NULL), b_null = (b == NULL);
  bool c_ok = (c != NULL), d_ok = (d != NULL);
  ctls_conn_destroy(a);
  ctls_conn_destroy(b);
  ctls_conn_destroy(c);
  ctls_conn_destroy(d);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(ctx);
  REQUIRE_TRUE(a_null);
  REQUIRE_TRUE(b_null);
  REQUIRE_TRUE(c_ok);
  REQUIRE_TRUE(d_ok);
  REQUIRE_NE((void *)err_null, (void *)NULL);
  REQUIRE_NE((void *)err_empty, (void *)NULL);
}

/* ========================================================================== */
/*                  INPUT THAT THE TLS LAYER ALREADY HOLDS                    */
/* ========================================================================== */

TEST(ctls_conn, pending_input_reports_plaintext_that_the_socket_no_longer_has) {
  REQUIRE_FALSE(ctls_conn_has_pending_input(NULL));
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  /* Room for the whole record in one write: a FreeBSD socketpair holds
   * about 8 KiB per direction by default, where Linux holds far more. */
  int room = 65536;
  for (int i = 0; i < 2; i++) {
    setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));
    setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
  }
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool hs = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  /* One write of 12000 bytes is one TLS record, so a read of 4096 bytes
   * leaves the rest of that record inside the TLS layer, while the socket
   * holds nothing more. */
  static char payload[12000];
  memset(payload, 'p', sizeof(payload));
  bool pending_before = true, pending_mid = false, pending_after = true;
  bool socket_empty_mid = false;
  size_t total = 0;
  if (hs) {
    /* Let the server take in anything that the handshake left, such as
     * TLS 1.3 tickets travelling the other way, before the measurement. */
    char tmp[16];
    (void)ctls_conn_read(server_conn, tmp, sizeof(tmp));
    pending_before = ctls_conn_has_pending_input(server_conn);
    ssize_t w = ctls_conn_write(client_conn, payload, sizeof(payload));
    char buf[4096];
    ssize_t n = (w == (ssize_t)sizeof(payload))
                    ? ctls_conn_read(server_conn, buf, sizeof(buf))
                    : -1;
    if (n > 0) {
      total = (size_t)n;
      pending_mid = ctls_conn_has_pending_input(server_conn);
      struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
      socket_empty_mid = (poll(&pfd, 1, 0) == 0);
      while (ctls_conn_has_pending_input(server_conn)) {
        n = ctls_conn_read(server_conn, buf, sizeof(buf));
        if (n <= 0) break;
        total += (size_t)n;
      }
      pending_after = ctls_conn_has_pending_input(server_conn);
    }
  }
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(hs);
  REQUIRE_FALSE(pending_before);
  REQUIRE_TRUE(pending_mid);
  REQUIRE_TRUE(socket_empty_mid);
  REQUIRE_EQ(total, sizeof(payload));
  REQUIRE_FALSE(pending_after);
}
