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
#include <internal/test_helpers/chashmap.h>
#include <internal/test_helpers/ctls.h>
#include <limits.h>
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
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_sanitizer.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

/* ========================================================================== */
/* OpenSSL allocation failure                                                 */
/*                                                                            */
/* OpenSSL takes a custom allocator only before its first allocation, so a    */
/* constructor that runs ahead of every other constructor of this binary      */
/* installs a forwarder to malloc. While a test holds the window open the     */
/* forwarder fails one chosen allocation of the arming thread, and apart from */
/* that it behaves exactly like malloc. LibreSSL has no such hook, so the     */
/* tests that need it skip there.                                             */
/* ========================================================================== */

static bool g_ossl_mem_hooked = false;
static _Atomic long g_ossl_alloc_seen = 0;
static _Atomic long g_ossl_alloc_fail_at = 0; /* 0 disarms */
static _Atomic bool g_ossl_alloc_fired = false;
static pthread_t g_ossl_alloc_thread;

#if !defined(LIBRESSL_VERSION_NUMBER)
static bool _ossl_should_fail(void) {
  long at = atomic_load(&g_ossl_alloc_fail_at);
  if (at == 0 || !pthread_equal(pthread_self(), g_ossl_alloc_thread))
    return false;
  if (atomic_fetch_add(&g_ossl_alloc_seen, 1) + 1 != at) return false;
  atomic_store(&g_ossl_alloc_fired, true);
  return true;
}

static void *_ossl_malloc(size_t n, const char *file, int line) {
  (void)file;
  (void)line;
  return _ossl_should_fail() ? NULL : malloc(n);
}
static void *_ossl_realloc(void *p, size_t n, const char *file, int line) {
  (void)file;
  (void)line;
  return _ossl_should_fail() ? NULL : realloc(p, n);
}
static void _ossl_free(void *p, const char *file, int line) {
  (void)file;
  (void)line;
  free(p);
}

__attribute__((constructor(101))) static void _hook_openssl_allocator(void) {
  g_ossl_mem_hooked =
      CRYPTO_set_mem_functions(_ossl_malloc, _ossl_realloc, _ossl_free) == 1;
}
#endif

static void _ossl_arm(long nth) {
  g_ossl_alloc_thread = pthread_self();
  atomic_store(&g_ossl_alloc_seen, 0);
  atomic_store(&g_ossl_alloc_fired, false);
  atomic_store(&g_ossl_alloc_fail_at, nth);
}
static void _ossl_disarm(void) { atomic_store(&g_ossl_alloc_fail_at, 0); }

/* Checked at the real exit of the process, which is the moment valgrind runs
   its own leak check and later than any TEST() can look. A regression in the
   alloc/free balance of ANY chashmap iterator, anywhere in this suite's run,
   then aborts loudly on the spot instead of showing up only as a rare valgrind
   report that is hard to reproduce. */
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
/* Real PEM files exercise the cert_path and key_path branch of               */
/* ctls_ctx_cert_add, which loads from a file and is separate from the branch */
/* that generates a self-signed certificate. The openssl CLI makes these      */
/* files once at startup, the same way tests/chttpclient/tests_tls.c and      */
/* tests/chttpserver/tests_tls.c do. Unlike those suites, ctls never stops    */
/* the process for a bad or missing certificate (that's on purpose, see the   */
/* doc comments in ctls.h), so this file doesn't need a separate binary to    */
/* protect itself from a process abort. If the openssl CLI is missing here,   */
/* the setup of the affected tests fails with a clear message.                */
/* ========================================================================== */

static char g_cert_dir[256];
static char g_server_cert[320], g_server_key[320];
static char g_client_cert[320], g_client_key[320];
static char g_enc_cert[320], g_enc_key[320];
static char g_named_cert[320], g_named_key[320];
static bool g_certs_ready = false;
static bool g_enc_cert_ready = false;

/* The password that a ctls_ctx_cert_add caller has to give for g_enc_key. */
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

/* Makes a password-protected private key, which the -nodes form above
 * deliberately doesn't. Loading such a key is the only way to reach the PEM
 * password callback that ctls installs, and that callback turns a wrong
 * password into a reported load failure. Without it OpenSSL prompts on a
 * terminal that may not exist. */
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
  snprintf(g_named_cert, sizeof(g_named_cert), "%s/named.pem", g_cert_dir);
  snprintf(g_named_key, sizeof(g_named_key), "%s/named_key.pem", g_cert_dir);
  if (_openssl_selfsigned(g_named_key, g_named_cert, "named.test", NULL) != 0)
    return -1;
  snprintf(g_enc_cert, sizeof(g_enc_cert), "%s/enc.pem", g_cert_dir);
  snprintf(g_enc_key, sizeof(g_enc_key), "%s/enc_key.pem", g_cert_dir);
  /* Kept as its own flag so that an openssl build without the cipher this needs
   * skips only the password tests and not every file-backed cert test. */
  g_enc_cert_ready =
      (_openssl_selfsigned_encrypted(g_enc_key, g_enc_cert, "enc.test",
                                     CTLS_TEST_KEY_PASSWORD) == 0);
  return 0;
}

static void _teardown(void);

__attribute__((constructor)) static void _setup(void) {
  /* An exit handler runs on every system, while OpenBSD runs no destructor of a
   * program that links libpthread. */
  atexit(_teardown);
  /* Every write of ctls goes through send(2) with MSG_NOSIGNAL, so no test here
   * needs this ignore. It is here so that a regression of that property doesn't
   * take down the whole binary; tests_sigpipe keeps the default disposition and
   * is the suite that asserts the property itself. */
  signal(SIGPIPE, SIG_IGN);
  g_certs_ready = (_generate_all_certs() == 0);
  if (!g_certs_ready) {
    fprintf(stderr,
            "WARNING: could not generate test certs via the openssl CLI; "
            "file-backed cert tests will be skipped.\n");
  }
}

static void _bundle3_remove(void);

static void _teardown(void) {
  _bundle3_remove();
  unlink(g_enc_cert);
  unlink(g_enc_key);
  unlink(g_server_cert);
  unlink(g_server_key);
  unlink(g_client_cert);
  unlink(g_client_key);
  unlink(g_named_cert);
  unlink(g_named_key);
  rmdir(g_cert_dir);
}

#if defined(LIBRESSL_VERSION_NUMBER)
/* LibreSSL has no SSL_has_pending(), so the socket BIO follows the record
 * framing itself. A client hello in the SSLv2 format doesn't follow that
 * framing, and read as a TLS header its version bytes give a body length of
 * 771. What follows here is 748 bytes of real records that end in the middle of
 * a record, so the TLS layer holds a partial record and the tracker has to
 * report pending input.
 *
 * A tracker that doesn't give up on a header whose first byte isn't a content
 * type fails here: it counts the 748 bytes as the rest of its 771-byte body,
 * ends that body exactly at the end of the input and reports nothing
 * pending. */
TEST(ctls_libressl, framing_tracker_gives_up_on_an_sslv2_hello) {
  /* An SSLv2 CLIENT-HELLO: a two-byte header with the high bit set, then type
   * 1, version 3.3, a 3-byte cipher spec and a 16-byte challenge. */
  static const char v2[28] = {(char)0x80, 0x1c, 0x01, 0x03, 0x03, 0x00, 0x03,
                              0x00,       0x00, 0x00, 0x10, 0x00, 0x00, 0x2f};
  /* 748 bytes: two whole application data records with 300 body bytes each,
   * plus the first 138 bytes of a third one that announces 300. */
  static char tail[748];
  int at = 0;
  for (int r = 0; r < 3; r++) {
    tail[at++] = 0x17;
    tail[at++] = 0x03;
    tail[at++] = 0x03;
    tail[at++] = 0x01;
    tail[at++] = 0x2c; /* 300 */
    int body = r < 2 ? 300 : 748 - at;
    for (int i = 0; i < body; i++) tail[at++] = 'x';
  }
  const char *chunks[2] = {v2, tail};
  int lens[2] = {(int)sizeof(v2), at};
  REQUIRE_EQ(at, 748);
  REQUIRE_TRUE(_ctls_rec_track_pending_after_for_tests(chunks, lens, 2));
}
#endif

/* ========================================================================== */
/*                          HANDSHAKE DRIVER HELPERS                          */
/* ========================================================================== */

/* Makes a non-blocking AF_UNIX socketpair. It has real socket semantics, so a
 * BIO built on recv and send works over it, and it needs neither a network nor
 * a port. Hostname verification doesn't care about the address family of the
 * socket either: the hostname argument of ctls_conn_create_client is only what
 * we verify against, and nothing ties it to the real peer of the fd. So this
 * pair stands in faithfully for a real TCP loopback pair in every test below.
 */
static void _make_nonblocking_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  for (int i = 0; i < 2; ++i) {
    /* If this fails and nobody checks, fds[i] stays blocking, and every later
     * ctls_conn_read or ctls_conn_write in the calling test blocks forever
     * where EWOULDBLOCK is the expected answer. That hangs the whole test
     * binary rather than just failing a check, and almost every test in this
     * file sits on this helper. */
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

/* Steps the non-blocking handshake on each end of a pair in turn, and stops
 * once both ends are done or max_iters runs out. It's safe without a real
 * poll(2) wait because both ends share one AF_UNIX socketpair that is already
 * connected, so whatever one side writes is there for the very next read of the
 * other side. */
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
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
  /* LibreSSL and OpenSSL 1.1.1 spell the call that takes a reference without
   * the 1. */
  X509 *cert = SSL_get_peer_certificate(ssl);
#else
  X509 *cert = SSL_get1_peer_certificate(ssl);
#endif
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

/* A certificate that matches its key but that the TLS library won't serve (here
 * an RSA-1024 key with a SHA-1 signature) is refused by ctls_ctx_cert_add
 * before anything is stored, so the context keeps working and a later,
 * unrelated change rebuilds it just like before. If the refused pair got
 * stored, the rebuild would fail and every later change of the context would
 * fail along with it. A TLS library that does serve such a pair makes
 * the test skip. */
TEST(ctls_ctx, a_certificate_that_the_library_refuses_is_not_stored) {
  if (!g_certs_ready) return;
  char weak_cert[320], weak_key[320], cmd[1024];
  snprintf(weak_cert, sizeof(weak_cert), "%s/weak.pem", g_cert_dir);
  snprintf(weak_key, sizeof(weak_key), "%s/weak_key.pem", g_cert_dir);
  int n = snprintf(cmd, sizeof(cmd),
                   "openssl req -x509 -newkey rsa:1024 -sha1 -nodes -keyout "
                   "'%s' -out '%s' -days 1 -subj '/CN=weak' >/dev/null 2>&1",
                   weak_key, weak_cert);
  if (n < 0 || (size_t)n >= sizeof(cmd) || system(cmd) != 0) {
    fprintf(stderr, "SKIP: openssl cannot make an RSA-1024/SHA-1 pair\n");
    return;
  }
  char *err = NULL;
  ctls_ctx_t *ctx = ctls_ctx_new(&err);
  REQUIRE_NE((void *)ctx, NULL);
  ccol_retval_t good =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, &err);
  ccol_retval_t weak =
      ctls_ctx_cert_add(ctx, "weak.test", weak_cert, weak_key, NULL, &err);
  ccol_retval_t later = ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, &err);
  ctls_ctx_release(ctx);
  unlink(weak_cert);
  unlink(weak_key);
  REQUIRE_EQ(good, ccol_success);
  if (weak == ccol_success) {
    fprintf(stderr, "SKIP: this TLS library serves an RSA-1024/SHA-1 pair\n");
    return;
  }
  REQUIRE_EQ(weak, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ(later, ccol_success);
}

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

/* An exit handler frees the process-wide key of the self-signed certificates.
 * The program's destructors run after every exit handler, so this one fails the
 * run if the key is still held by then. Without the exit handler the key stays
 * allocated, and the reachable rules of the OpenSSL suppression file would hide
 * that from the memtest. */
__attribute__((destructor)) static void _root_key_freed_at_exit(void) {
  if (_ctls_root_key_held_for_tests()) {
    static const char msg[] = "ctls: the root key is still held at exit\n";
    ssize_t w = write(2, msg, sizeof(msg) - 1);
    (void)w;
    _exit(1);
  }
}

TEST(ctls_ctx, cert_add_no_name_no_files_generates_self_signed_default) {
  /* A server_name of NULL or "" with cert_path and key_path both NULL is valid
   * and generates a self-signed DEFAULT certificate. Its subject is a generic,
   * fixed name, because the default slot has no name of its own to take one
   * from. */
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
  /* The system CA store of this CI and development environment should be there
   * and loadable. A platform with no store configured gets
   * ccol_http_tls_cert_load_failed here instead, which is the real failure, and
   * this return value reports it instead of dropping it silently. */
  REQUIRE_EQ(ctls_ctx_trust_system(ctx), ccol_success);
  ctls_ctx_release(ctx);
}

/* Points SSL_CERT_FILE at path and drops the parsed default CA file, so that
 * the next context reads the file again. Only a child of _ctls_run_in_child
 * calls it, so the parent's environment never changes. */
static void _system_ca_env_set(const char *path) {
  setenv("SSL_CERT_FILE", path, 1);
  _ctls_system_ca_forget_for_tests();
}

/* Runs one handshake between a server serving g_server_cert and a client
 * context, and tells whether the client verified the server. */
static bool _client_verifies_test_server(ctls_ctx_t *client_ctx) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  if (!server_ctx) return false;
  bool ok = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success;
  int fds[2] = {-1, -1};
  ctls_conn_t *server_conn = NULL, *client_conn = NULL;
  if (ok) ok = socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
  for (int i = 0; ok && i < 2; ++i) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    ok = flags >= 0 && fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) == 0;
  }
  if (ok) {
    server_conn = ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
    client_conn =
        ctls_conn_create_client(client_ctx, fds[1], "127.0.0.1", true, NULL);
    ok = server_conn && client_conn &&
         _drive_both(client_conn, server_conn, 200, NULL, NULL) &&
         ctls_conn_verify_result(client_conn) == 0L /* X509_V_OK */;
  }
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(server_ctx);
  return ok;
}

/* Runs body in a forked child and copies the out_size bytes of results it
 * fills back into out, so the parent can assert on them. The two tests below
 * change SSL_CERT_FILE, and doing that in a child keeps the parent's
 * environment as it was: no later test sees the change, and musl, which
 * keeps every setenv() block for the life of the process, has nothing left
 * for the memtest to report. Returns false when the child gave no full
 * result within 30 s, in which case it's killed. */
static bool _ctls_run_in_child(void (*body)(void *out), void *out,
                               size_t out_size) {
  int p[2];
  if (pipe(p) != 0) return false;
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return false;
  }
  if (pid == 0) {
    close(p[0]);
    body(out);
    ssize_t w = write(p[1], out, out_size);
    _exit(w == (ssize_t)out_size ? 0 : 1);
  }
  close(p[1]);
  size_t got = 0;
  while (got < out_size) {
    struct pollfd pf = {.fd = p[0], .events = POLLIN};
    if (poll(&pf, 1, 30000) <= 0) break;
    ssize_t r = read(p[0], (char *)out + got, out_size - got);
    if (r > 0) {
      got += (size_t)r;
    } else if (r == 0 || errno != EINTR) {
      break;
    }
  }
  close(p[0]);
  if (got < out_size) kill(pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return got == out_size;
}

typedef struct {
  ccol_retval_t rv_trusting;
  bool verified;
  ccol_retval_t rv_other;
  bool other_verified;
} _system_ca_names_result_t;

static void _system_ca_names_body(void *out) {
  _system_ca_names_result_t *r = (_system_ca_names_result_t *)out;
  _system_ca_env_set(g_server_cert);
  ctls_ctx_t *trusting = ctls_ctx_new(NULL);
  r->rv_trusting =
      trusting ? ctls_ctx_trust_system(trusting) : ccol_unexpected_failure;
  r->verified =
      r->rv_trusting == ccol_success && _client_verifies_test_server(trusting);
  _system_ca_env_set(g_client_cert);
  ctls_ctx_t *other = ctls_ctx_new(NULL);
  r->rv_other = other ? ctls_ctx_trust_system(other) : ccol_unexpected_failure;
  r->other_verified =
      r->rv_other == ccol_success && _client_verifies_test_server(other);
  ctls_ctx_release(trusting);
  ctls_ctx_release(other);
}

/* The system store is the file that SSL_CERT_FILE names. A client that trusts
 * the system store verifies a server whose self-signed certificate is that
 * file, and stops verifying it once the variable names some other certificate.
 * The test runs in a child; see _ctls_run_in_child. */
TEST(ctls_system_ca, ssl_cert_file_names_the_trusted_bundle) {
  if (!g_certs_ready) return;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_names_result_t r = {ccol_unexpected_failure, false,
                                 ccol_unexpected_failure, true};
  bool ran = _ctls_run_in_child(_system_ca_names_body, &r, sizeof(r));
  REQUIRE_TRUE(ran);
  REQUIRE_EQ(r.rv_trusting, ccol_success);
  REQUIRE_TRUE(r.verified);
  REQUIRE_EQ(r.rv_other, ccol_success);
  REQUIRE_FALSE(r.other_verified);
}

typedef struct {
  bool all_ok;
  long parses;
  bool verified;
} _system_ca_once_result_t;

static void _system_ca_once_body(void *out) {
  _system_ca_once_result_t *r = (_system_ca_once_result_t *)out;
  _system_ca_env_set(g_server_cert);
  long before = _ctls_system_ca_parses_for_tests();
  ctls_ctx_t *ctxs[4] = {NULL, NULL, NULL, NULL};
  r->all_ok = true;
  for (int i = 0; i < 4; ++i) {
    ctxs[i] = ctls_ctx_new(NULL);
    if (!ctxs[i] || ctls_ctx_trust_system(ctxs[i]) != ccol_success ||
        ctls_ctx_alpn_add(ctxs[i], "h2", NULL, NULL, NULL, NULL) !=
            ccol_success)
      r->all_ok = false;
  }
  r->parses = _ctls_system_ca_parses_for_tests() - before;
  r->verified = r->all_ok && _client_verifies_test_server(ctxs[3]);
  for (int i = 0; i < 4; ++i) ctls_ctx_release(ctxs[i]);
}

/* Every context that trusts the system store shares a single parse of the
 * default CA file, across rebuilds too. A store that doesn't keep the parsed
 * list parses the file again for each context and each rebuild of one, and
 * the count goes past one. The test runs in a child; see
 * _ctls_run_in_child. */
TEST(ctls_system_ca, the_default_file_is_parsed_once_per_process) {
  if (!g_certs_ready) return;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_once_result_t r = {false, -1, false};
  bool ran = _ctls_run_in_child(_system_ca_once_body, &r, sizeof(r));
  REQUIRE_TRUE(ran);
  REQUIRE_TRUE(r.all_ok);
  REQUIRE_EQ(r.parses, 1L);
  REQUIRE_TRUE(r.verified);
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

/* The cleanup of a replaced ALPN registration must run with no ctls lock
 * held. The cleanup below asks a second thread to take the context's lock
 * (through ctls_ctx_retain and ctls_ctx_release) and waits a bounded time for
 * it. If the replacing call holds that lock, the second thread can't finish
 * before the cleanup returns, the wait runs out and the cleanup records it. The
 * second thread then finishes once the lock is released, so the test never
 * hangs. */
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
  /* At most 10 seconds, in steps of 1 ms. */
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
  /* The server certificate is self-signed, so we trust it directly as its own
   * CA and the chain verification has something to succeed against. As the
   * documented semantics of this module say, verify_host implies verify_peer,
   * and that's what starts the verification. */
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
  /* "10.0.0.9" doesn't match the IP:127.0.0.1 SAN of the cert. */
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
  /* The server trusts the client's own self-signed cert as its CA. */
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
  /* A server-mode context with a trust store requires a client certificate by
   * default, so a client that presents none fails the handshake on the server
   * side. With SSL_VERIFY_PEER alone the server would complete the handshake,
   * so this catches a missing SSL_VERIFY_FAIL_IF_NO_PEER_CERT. */
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
   * client that presents no certificate gets through the handshake, and the
   * connection reports it as not verified. The call works both before and after
   * ctls_ctx_trust(). The verified check matters here: an answer based only
   * on SSL_get_verify_result() says true, because OpenSSL reports X509_V_OK
   * for a peer that presented nothing. */
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
  /* A client that verifies nothing reports no verified peer even though the
   * server did present a certificate, and so does a NULL connection. */
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
  /* Writing nothing on a healthy connection returns 0 and leaves the connection
   * usable, just like write(2) on a socket. Without the early return,
   * SSL_write() with a length of 0 returns 0, and our failure classification
   * would turn that into -1 with ECONNRESET. */
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
  /* Destroying a connection whose handshake never finished, or never started,
   * leaves the OpenSSL error queue of the calling thread empty. Otherwise a
   * later and unrelated OpenSSL call on the same thread would read that entry
   * as its own. The queue isn't empty by accident: SSL_shutdown() on such a
   * connection pushes "shutdown while in init". */
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
  /* One step on each side leaves both of them halfway through the handshake. */
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
  /* The server trusts ONLY the server cert itself as a CA (NOT the client
   * cert), so a client that presents its own self-signed, untrusted cert
   * fails verification. */
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
  /* No hostname at all, so no SNI extension is sent and we get the default
     cert. */
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
  /* The test registers the name in mixed case. ctls_ctx_cert_add lowercases
   * server_name before using it as the map key and, for a named entry that it
   * signs itself, as the subject of the certificate, so the CN below is
   * lowercase whatever case the test uses. */
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
  /* The client sends the SNI value in a completely different case from the one
   * the cert was registered with above. */
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "ALPHA.TEST", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = _ctls_conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  /* Getting "ctls-default" here instead of "alpha.test" would mean the case
   * mismatch made us miss the named entry altogether. */
  REQUIRE_STREQ(cn, "alpha.test");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

/* A caller may register a named certificate again for a server_name that
 * already has one, which is the supported way to rotate a certificate.
 * ctls_ctx_cert_add looks up the earlier entry and destroys it before it
 * inserts the new one. Inside, the chmap update of a key that is already there
 * returns ccol_key_already_present rather than ccol_success, and
 * ctls_ctx_cert_add must read that as success instead of destroying the
 * certificate it just stored. Such a destroy would leave the map's value slot
 * for this hostname pointing at freed memory. */
TEST(ctls_sni, cert_add_replaces_existing_named_cert_without_dangling_pointer) {
  if (!g_certs_ready) return;
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "rotate.test", g_server_cert,
                               g_server_key, NULL, NULL),
             ccol_success);
  /* Rotate: the same name with a different cert/key pair. */
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
  /* Expect the CN of the second certificate, the client one. That shows
   * the rotation really happened, on a live entry that the code updated
   * correctly, and that no dangling pointer to the first certificate (freed by
   * now) was left behind. */
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

/* Only the address matters, nobody ever reads the value. */
static int _app_data_sentinel = 0;

/* Once the ex_data index is registered, a connection reads it without taking a
 * lock. If every lookup took the mutex of the index, each create below would
 * add one locked call and the count would show it. */
TEST(ctls_ex_data, a_registered_index_is_read_without_the_lock) {
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  /* The first create registers the index unless an earlier test already did. */
  ctls_conn_destroy(ctls_conn_create_server(ctx, fds[0], NULL, NULL));
  long before = _ctls_ex_idx_locked_calls_for_tests();
  bool all_created = true;
  for (int i = 0; i < 16; ++i) {
    ctls_conn_t *conn = ctls_conn_create_server(ctx, fds[0], NULL, NULL);
    if (!conn) all_created = false;
    ctls_conn_destroy(conn);
  }
  long after = _ctls_ex_idx_locked_calls_for_tests();
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(ctx);
  REQUIRE_TRUE(all_created);
  REQUIRE_EQ(after, before);
}

TEST(ctls_ex_data, connections_leave_the_openssl_app_data_slot_alone) {
  /* This module hangs its own per-connection pointer on an SSL* through an
     ex_data index that it registers with OpenSSL. That index is not index 0:
     OpenSSL keeps 0 for the SSL_set_app_data()/SSL_get_app_data() macros, and
     any other code in the same process may use those on an SSL* it gets hold
     of. If we took that slot the two would alias one pointer, and each side
     would silently read and overwrite the other's value.

     The expectations below are written against OpenSSL's own macros instead of
     anything this module exports, so they can't drift together with the
     implementation they check. */
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

  /* We only simulate an application's use of the app-data slot once both
     connections really exist and the slot is known to be free. Writing a
     foreign pointer into a slot that this module reads its own connection from
     would make it dereference that pointer during the handshake, and
     _ctls_conn_ssl_for_tests() returns NULL for a connection that was never
     created, which OpenSSL then dereferences. Either way the whole binary
     crashes and takes every other test result of this run with it, instead of
     failing just this one test. Both at_create values are NULL too when the
     creation failed, so they can't carry that condition by themselves. */
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

  /* A fresh connection should leave the app-data slot exactly the way OpenSSL
     leaves it. */
  REQUIRE_TRUE(conns_created);
  REQUIRE_EQ(server_app_at_create, (void *)NULL);
  REQUIRE_EQ(client_app_at_create, (void *)NULL);
  REQUIRE_TRUE(slot_free);

  /* A handshake that reads this module's own per-connection pointer (the
     server's ALPN selection callback does exactly that) must not be confused by
     the application's app data, and must not overwrite it either. */
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_TRUE(alpn_fired);
  REQUIRE_STREQ(server_cap.name, "http/1.1");
  REQUIRE_EQ(server_app_after, (void *)&_app_data_sentinel);
  REQUIRE_EQ(client_app_after, (void *)&_app_data_sentinel);
}

/* The server has none of the protocols that the client offers, so the handshake
 * ends up with no protocol at all. Reporting a registered protocol in that case
 * claims an agreement the peer never made: the server here would be told it
 * negotiated "spdy/1" with a client that only offered "http/1.1". */
/* What the server on_selected callback below saw. */
typedef struct {
  bool fired;
  bool handshake_finished;
} alpn_self_destroy;

static void _alpn_destroy_own_conn_cb(ctls_conn_t *conn, const char *name,
                                      size_t len, void *udata) {
  (void)name;
  (void)len;
  alpn_self_destroy *seen = (alpn_self_destroy *)udata;
  seen->fired = true;
  seen->handshake_finished =
      SSL_is_init_finished(_ctls_conn_ssl_for_tests(conn)) == 1;
  ctls_conn_destroy(conn);
}

/* A server on_selected callback may destroy its own connection, because it
 * fires after the TLS library has finished the handshake step and nothing reads
 * the connection after that. Were the callback to fire from inside OpenSSL's
 * ALPN selection, it would see an unfinished handshake, and OpenSSL would
 * then read the selected name out of the freed connection, which valgrind and
 * AddressSanitizer report. */
TEST(ctls_alpn, a_server_callback_may_destroy_its_own_connection) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  alpn_self_destroy seen = {0};
  bool configured =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL) ==
          ccol_success &&
      ctls_ctx_alpn_add(server_ctx, "h2", _alpn_destroy_own_conn_cb, &seen,
                        NULL, NULL) == ccol_success &&
      ctls_ctx_alpn_add(client_ctx, "h2", NULL, NULL, NULL, NULL) ==
          ccol_success;
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool client_done = false;
  bool server_done = false;
  for (int i = 0; i < 200 && configured && !(client_done && server_done); ++i) {
    if (!client_done &&
        ctls_conn_handshake_step(client_conn) == CTLS_HANDSHAKE_DONE)
      client_done = true;
    if (!server_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(server_conn);
      /* Once the callback has fired, server_conn is gone. */
      if (seen.fired || r == CTLS_HANDSHAKE_DONE || r == CTLS_HANDSHAKE_ERROR)
        server_done = true;
    }
  }
  if (!seen.fired) ctls_conn_destroy(server_conn);
  ctls_conn_destroy(client_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(configured);
  REQUIRE_TRUE(client_done);
  REQUIRE_TRUE(seen.fired);
  REQUIRE_TRUE(seen.handshake_finished);
}

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
  /* Poisoned so that "reported nothing" can be told apart from "never touched".
   */
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
  /* The handshake itself still succeeds. No ALPN match isn't a handshake
   * failure, it only means that no protocol was negotiated. */
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_FALSE(fired);
  REQUIRE_EQ((void *)sel, (void *)NULL);
  REQUIRE_EQ(len, (size_t)0);
}

/* The same thing from the client side, and the case where a wrong report turns
 * into an error on the wire. A client offers {"h2", "http/1.1"} to a server
 * that negotiates no ALPN at all, and the client has to be told that nothing
 * was negotiated. Reporting the first registered protocol would make it speak
 * HTTP/2 framing to an HTTP/1.1 peer. */
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
  /* A caller that parks a slow reader copies the unsent tail of its data into
   * storage of its own and retries a write that said "would block" from there.
   * The retry carries the same bytes at another address and it has to succeed.
   * Without SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER OpenSSL refuses that retry
   * with "bad write retry", and the write fails for good. */
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
   * small socket buffer (about 8 KiB per direction on a FreeBSD socketpair) may
   * take the rest only over several rounds, each retrying from the other buffer
   * again. A refused retry fails for good at once and ends the loop, only
   * "would block" goes round again. */
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
/* These tests cover three hazards that no ordinary test gets to.             */
/*                                                                            */
/* (1) A use-after-free in _ctls_servername_cb. The callback looks up the     */
/* SSL_CTX* of the matching named certificate under tls->lock, and it must    */
/* not call SSL_set_SSL_CTX() on that pointer AFTER unlocking, because a      */
/* concurrent ctls_ctx_cert_add() can rotate that exact SSL_CTX and free it.  */
/*                                                                            */
/* (2) An unlocked data race in _ctls_alpn_select_cb, which picks the ALPN    */
/* protocol in server mode. It must not read tls->alpn[] or tls->alpn_count   */
/* without tls->lock; its client-mode sibling, _ctls_record_client_alpn,      */
/* holds that lock.                                                           */
/*                                                                            */
/* (3) An allocation in ctls_ctx_cert_add for the subject name of a           */
/* self-signed certificate, whose NULL result nothing may leave unchecked.    */
/* Such a result reaches _ctls_create_self_signed(NULL), which then calls     */
/* strlen(NULL) whenever memory stays exhausted. Every other allocation       */
/* failure in the same function reports the documented ccol_not_enough_memory */
/* instead.                                                                   */
/* ========================================================================== */

/* (3): an OOM on the subject name of a self-signed named cert must not crash */

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
  /* The call sequence of ctls_ctx_cert_add for a NAMED, self-signed certificate
     with no password: lower_name = _ctls_strdup_lower(...) is the first malloc,
     nc = _ccol_mem_calloc(...) is a calloc (not counted here), and
     nc->self_signed_name = _ctls_strdup(...) is the second malloc, which is the
     one this test goes after. Take away the NULL check it guards and the
     test crashes with strlen(NULL) inside _ctls_create_self_signed, reached
     through _ctls_ctx_rebuild_locked. */
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

  /* The context should still be really usable afterwards, so the failed add
     must not have left it half-configured or corrupted. */
  REQUIRE_EQ(ctls_ctx_cert_add(ctx, "after-oom.test", NULL, NULL, NULL, NULL),
             ccol_success);

  ctls_ctx_release(ctx);
}

/* (1): use-after-free in the SNI servername callback under concurrent cert
   rotation */

static _Atomic bool g_sni_race_stop = false;
static ctls_ctx_t *g_sni_race_ctx = NULL;

static void *_sni_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_sni_race_stop)) {
    /* Adding the SAME name over and over makes the commit step of
       _ctls_ctx_rebuild_locked SSL_CTX_free() the previous built_ctx on every
       iteration. That is exactly the free that a concurrent, in-flight
       _ctls_servername_cb call (on the handshake thread below) must never see
       happen to the SSL_CTX* it has already looked up. */
    ctls_ctx_cert_add(g_sni_race_ctx, "race.test", NULL, NULL, NULL, NULL);
    /* A real wall-clock throttle and not sched_yield(). On a machine with cores
       to spare, an unthrottled rotate thread gets a core of its own with
       nothing else competing for it, so sched_yield() there is close to a no-op
       (it only gives the CPU to another runnable thread on the SAME core) and
       does nothing to slow this thread down compared with the main thread,
       which runs on a different core altogether. Unthrottled, the rotate thread
       takes ctx->lock back so much faster than the main thread's handshake
       calls that in practice the lock is always held, or stolen again the
       instant it's released, by the time the main thread asks for it. That's a
       real lock-starvation livelock: the 80 handshakes below don't finish
       within several real minutes (it isn't a deadlock, but a test can't
       survive it either). usleep(1000) caps the rotate thread at about 1000
       iterations per second, which is plenty to keep racing the main thread's
       lock acquisitions while leaving it real wall-clock room to win its share
       of them. */
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

  /* A bounded number of real, full handshakes, each one on a fresh connection
     pair, racing the rotation thread's constant churn the whole time. The real
     assertion is that this survives at all. A plain run proves little, but
     under ASan, valgrind or TSan a use-after-free here reliably aborts or gets
     reported. */
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
  /* A checkpoint in the middle of the suite, next to the stronger check that
     _check_chmap_iter_balance_at_exit makes at the real exit of the process.
     The rotation thread has just stopped, so nothing else in the process can be
     creating or destroying a chashmap iterator right now, and the count must be
     zero. Neither ctls_ctx_cert_add nor _ctls_ctx_rebuild_locked creates one
     (the rebuild walks the named certificates with ccol_chmap_walk_next(),
     which allocates nothing), so this is a standing guard that no chashmap
     iterator of the process is left outstanding. */
  REQUIRE_EQ(chashmap_iter_outstanding_count_for_tests(), (long)0);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(g_sni_race_ctx);
  g_sni_race_ctx = NULL;
}

/* (2): race on the server-mode ALPN select callback, plus the
 * alpn_selected_name copy that the connection owns (both directions) */

static _Atomic bool g_alpn_race_stop = false;
static ctls_ctx_t *g_alpn_race_server_ctx = NULL;

static void *_alpn_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_alpn_race_stop)) {
    /* Adding the SAME protocol name over and over drives the "replace an
       existing registration" branch of ctls_ctx_alpn_add, which frees the name
       buffer of the previous entry. That's exactly the buffer that a
       concurrent, in-flight _ctls_alpn_select_cb call (on the handshake thread
       below) must never read once it's freed. */
    ctls_ctx_alpn_add(g_alpn_race_server_ctx, "h2", NULL, NULL, NULL, NULL);
    /* Same as the usleep() comment in _sni_race_rotate_thread above. Without a
       real wall-clock throttle this tight loop starves the main thread's
       handshake calls so that they practically never win ctx->lock, and
       sched_yield() alone doesn't help. */
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

  /* Kept in a local instead of asserted inside the loop. A failing REQUIRE_EQ
     there would return from this function at once, skipping the
     atomic_store(&g_alpn_race_stop, true)/pthread_join(rotate_th, ...) below,
     which leaves that thread running forever (its loop only ends once it sees
     g_alpn_race_stop set) and leaks both ctls_ctx_t handles that the two
     release calls below would otherwise free. So every check waits until the
     loop, the stop signal, the join and both releases have run, which is how
     this codebase handles every early-REQUIRE-failure hazard of this kind. */
  bool alpn_len_mismatch = false;
  for (int i = 0; i < 80; i++) {
    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn_t *server_conn =
        ctls_conn_create_server(g_alpn_race_server_ctx, fds[0], NULL, NULL);
    ctls_conn_t *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
    if (_drive_both(client_conn, server_conn, 200, NULL, NULL)) {
      /* When a handshake does complete, the selected protocol name has to stay
         readable afterwards, long after tls->lock was released, through the
         copy that the connection owns and not through a pointer owned by the
         ctx that may already have been rotated and freed. */
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
/* An SSL_CTX has no public getter for its reference count, so the claim that */
/* a context rebuilt many times while connections come and go doesn't pile up */
/* references can only be checked by counting. We compare how many SSL_CTX    */
/* objects are created with how many are really destroyed. OpenSSL calls a    */
/* registered ex_data new callback once for each SSL_CTX_new and the matching */
/* free callback once for each real destruction, which is the free that runs  */
/* when the reference count reaches zero and not one for every SSL_CTX_free   */
/* call. One registered index thus counts both for every SSL_CTX of the       */
/* process, including the ones that ctls builds inside itself and never hands */
/* out. A reference that nobody releases shows up directly as a creation with */
/* no matching destruction.                                                   */
/*                                                                            */
/* A constructor registers the index, so nothing can build an SSL_CTX before  */
/* the index exists. An ex_data callback fires on whatever thread creates or  */
/* destroys the context, so both counters are atomic, and nobody reads them   */
/* before the threads under test are joined.                                  */
/* ========================================================================== */

static _Atomic long g_ssl_ctx_created = 0;
static _Atomic long g_ssl_ctx_destroyed = 0;

/* OpenSSL declares the ex data new callback as void and LibreSSL as int, where
 * 1 means success. */
#if defined(LIBRESSL_VERSION_NUMBER)
#define CENSUS_NEW_RET int
#define CENSUS_NEW_DONE return 1
#else
#define CENSUS_NEW_RET void
#define CENSUS_NEW_DONE return
#endif

static CENSUS_NEW_RET _ssl_ctx_census_new(void *parent, void *ptr,
                                          CRYPTO_EX_DATA *ad, int idx,
                                          long argl, void *argp) {
  (void)parent;
  (void)ptr;
  (void)ad;
  (void)idx;
  (void)argl;
  (void)argp;
  atomic_fetch_add_explicit(&g_ssl_ctx_created, 1, memory_order_relaxed);
  CENSUS_NEW_DONE;
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
/* A connection reads the SSL_CTX that it was created from (OpenSSL calls it  */
/* the session context) for its whole life. A server connection whose SNI     */
/* name matches a named certificate also reads the SSL_CTX that the dispatch  */
/* for that certificate installed. Both contexts are shared, and a concurrent */
/* ctls_ctx_cert_add, ctls_ctx_alpn_add or ctls_ctx_trust rebuilds and frees  */
/* both, which is a documented and supported use. So ctls holds a reference   */
/* of its own on each one: a context that a live connection can still reach   */
/* is kept alive by this module, and not by whatever a third party's internal */
/* bookkeeping happens to do.                                                 */
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

  /* Every observation goes into a local before any cleanup runs, so the asserts
     at the end of the test can run after both connections and both contexts
     have been released no matter what. */
  SSL_CTX *server_pinned = _ctls_conn_pinned_sni_ctx_for_tests(server_conn);
  SSL_CTX *server_active =
      SSL_get_SSL_CTX(_ctls_conn_ssl_for_tests(server_conn));
  SSL_CTX *client_pinned = _ctls_conn_pinned_sni_ctx_for_tests(client_conn);
  /* The server connection pins its creation-time context and the dispatched
     named one. The client connection never reaches a servername callback, so it
     pins only its creation-time context. */
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
     from the default context and there's no second context to pin. */
  REQUIRE_EQ((void *)server_pinned, (void *)NULL);
  REQUIRE_EQ(pins_during - pins_before, 2L);
  REQUIRE_EQ(pins_after, pins_before);
}

/* ========================================================================== */
/* Concurrent reconfiguration under live traffic                              */
/*                                                                            */
/* Four acceptor threads and six client threads run real TLS handshakes over  */
/* loopback, sharing one server context and one client context. Meanwhile a   */
/* rotator thread keeps adding named certificates, the default certificate    */
/* and an ALPN protocol again, and each of those calls rebuilds and frees     */
/* every SSL_CTX that the context owns. The client names cycle through four   */
/* cases: an exact match, a second exact match, a name that the wildcard      */
/* entry answers and a name that nothing answers. So some connections go to   */
/* three different named contexts, others stay on the default one, and all of */
/* them are in flight while their contexts get replaced.                      */
/*                                                                            */
/* The listening socket binds port 0, so several suites (or several runs of   */
/* this one) can never collide on a fixed port.                               */
/* ========================================================================== */

#define CTLS_STRESS_ACCEPTORS 4
#define CTLS_STRESS_CLIENTS 6
#define CTLS_STRESS_ROUNDS 6
/* Only a safety net against a hang, since no assertion below depends on how
   long a handshake takes. It's generous enough that ten threads run one at a
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

/* A counted gate instead of a pthread_barrier_t. The barrier's participant
   count is fixed at creation, so a pthread_create that fails halfway leaves
   every thread that did start parked on it forever. The creating thread opens
   this gate once the number of threads it ACTUALLY started have arrived, on
   every path out. */
static void _stress_wait_for_go(void) {
  atomic_fetch_add(&g_stress_arrived, 1);
  while (!atomic_load(&g_stress_go)) {
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
}

/* Drives one end of a handshake to the end, waiting on whichever direction the
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
    /* A bounded wait so the thread notices the stop flag quickly. Only Linux
       bounds accept(2) with SO_RCVTIMEO, the BSDs and macOS ignore it on a
       listening socket. The listening socket is non-blocking, so an acceptor
       that loses the connection to another one gets EAGAIN instead of parking
       in accept(2). */
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
    if (test_connect(fd, (struct sockaddr *)&g_stress_addr,
                     sizeof(g_stress_addr)) != 0) {
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
    /* A real wall-clock throttle instead of sched_yield(), for the reason the
       two rotation threads above explain: an unthrottled rotator holds
       ctx->lock, or takes it back right away, so consistently that the
       handshake threads never win their share of it. */
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
  /* Non-blocking, for the bounded accept in _stress_acceptor_thread. */
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
  /* Bounded by what really started and released no matter what, so a partial
     start still lets every started thread run to the end and be joined instead
     of parking forever. */
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
  /* Reconfiguring a context must not break a handshake that's already in flight
     against it, so every connection that was accepted and driven to an answer
     is expected to complete. We assert a healthy majority and not equality,
     because a client whose test_connect(2) is refused while the backlog is full
     says something about the machine and nothing about ctls. */
  REQUIRE_GT(client_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  REQUIRE_GT(server_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  /* Every reference that ctls took for a connection has been released. */
  REQUIRE_EQ(pins_after, pins_before);
  /* And every SSL_CTX created by the rotator's rebuilds is really destroyed
     once the contexts are released. A reference that nobody holds would leave a
     context alive here whose count never reaches zero. */
  REQUIRE_EQ(ssl_ctx_after, ssl_ctx_before);
}

/* ========================================================================== */
/* Allocation-failure sweep                                                   */
/*                                                                            */
/* Each entry point below has cleanup branches that free a different part of  */
/* what the call had built when the allocation failed. Nothing else in this   */
/* suite runs them, so without this the documented ccol_not_enough_memory     */
/* return and the freeing that comes with it are never checked.               */
/*                                                                            */
/* The counter is shared across all four procs on purpose. The context        */
/* itself, a named-certificate record and a connection all come from          */
/* _ccol_mem_calloc, while the trust array and the ALPN array grow through    */
/* _ccol_mem_realloc, so a harness that failed only malloc would leave every  */
/* one of those branches out of reach.                                        */
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

/* This depth goes past the last allocation that any of these calls makes, so
 * the sweep covers every branch and not just the first few. */
#define CTLS_SWEEP_DEPTH 46

TEST(ctls_oom, ctx_new_reports_failure_at_every_allocation_step) {
  bool all_handled = true;
  for (int n = 1; n <= 10; n++) {
    _sweep_arm(n);
    char *err = NULL;
    ctls_ctx_t *ctx = ctls_ctx_new_mp(&g_sweep_mp, &err);
    _sweep_disarm();
    /* Either the failed allocation wasn't on this path, or the context couldn't
     * be built and we get no half-built one back. */
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
    /* A failed add leaves the context usable and not poisoned. */
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
    /* There are two adds and the second one makes the parallel pem array and
     * length array grow. A reallocation that covers only one of them leaves the
     * other one pointing at freed storage. */
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

    /* A failed add changes nothing, whether it failed before storing the entry
     * or while rebuilding the SSL_CTX, so the count is exactly the number of
     * adds that succeeded. */
    size_t expect =
        (a == ccol_success ? 1u : 0u) + (b == ccol_success ? 1u : 0u);
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

/* ========================================================================== */
/* A failed change of the configuration changes nothing                       */
/*                                                                            */
/* Every certificate of the fixture comes from a file. OpenSSL 3.5 frees a    */
/* pointer it never allocated when an allocation inside X509_sign fails (a    */
/* program of a few lines without this library shows it), so a sweep over     */
/* OpenSSL's allocations must not sign a self-signed certificate.             */
/*                                                                            */
/* Each test below fails one allocation at a time, first every allocation of  */
/* OpenSSL and then every allocation of this module, while one call that      */
/* changes the configuration runs, and it stops at the first index the call   */
/* never reaches. After a failed call every name has to serve the certificate */
/* and the verify mode it served before the call, and it keeps doing so */
/* after a later rebuild for an unrelated reason (an ALPN registration that   */
/* replaces itself). That second part is what proves the stored configuration */
/* matches the contexts that serve.                                           */
/* ========================================================================== */

/* The CN of the certificate that serves a ClientHello naming name, or "" when
 * that context holds no certificate. */
static const char *_served_cn(ctls_ctx_t *ctx, const char *name, char *buf,
                              size_t buflen) {
  buf[0] = '\0';
  SSL_CTX *served = _ctls_ctx_served_ctx_for_tests(ctx, name);
  X509 *cert = served ? SSL_CTX_get0_certificate(served) : NULL;
  if (cert)
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf,
                              (int)buflen);
  return buf;
}

static int _served_verify_mode(ctls_ctx_t *ctx) {
  SSL_CTX *served = _ctls_ctx_served_ctx_for_tests(ctx, NULL);
  return served ? SSL_CTX_get_verify_mode(served) : -1;
}

typedef enum {
  CFG_OP_ROTATE_NAME,
  CFG_OP_ADD_NAME,
  CFG_OP_REPLACE_DEFAULT,
  CFG_OP_TRUST,
  CFG_OP_TRUST_SYSTEM,
  CFG_OP_COUNT
} cfg_op_t;

/* The names whose served certificate the tests check, and the CN each of them
 * serves before the call under test. */
static const char *const g_cfg_names[] = {NULL, "keep.test", "rot.test",
                                          "new.test"};
static const char *const g_cfg_before[] = {"test-client", "named.test",
                                           "named.test", "test-client"};

static ctls_ctx_t *_cfg_fixture(ccol_memmgmt_procs_t *mp) {
  ctls_ctx_t *ctx = ctls_ctx_new_mp(mp, NULL);
  if (!ctx) return NULL;
  if (ctls_ctx_cert_add(ctx, NULL, g_client_cert, g_client_key, NULL, NULL) !=
          ccol_success ||
      ctls_ctx_cert_add(ctx, "keep.test", g_named_cert, g_named_key, NULL,
                        NULL) != ccol_success ||
      ctls_ctx_cert_add(ctx, "rot.test", g_named_cert, g_named_key, NULL,
                        NULL) != ccol_success ||
      ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL) != ccol_success) {
    ctls_ctx_release(ctx);
    return NULL;
  }
  return ctx;
}

static ccol_retval_t _cfg_run_op(ctls_ctx_t *ctx, cfg_op_t op) {
  switch (op) {
    case CFG_OP_ROTATE_NAME:
      return ctls_ctx_cert_add(ctx, "rot.test", g_server_cert, g_server_key,
                               NULL, NULL);
    case CFG_OP_ADD_NAME:
      return ctls_ctx_cert_add(ctx, "new.test", g_server_cert, g_server_key,
                               NULL, NULL);
    case CFG_OP_REPLACE_DEFAULT:
      return ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL,
                               NULL);
    case CFG_OP_TRUST:
      return ctls_ctx_trust(ctx, g_server_cert, NULL);
    case CFG_OP_TRUST_SYSTEM:
      return ctls_ctx_trust_system(ctx);
    case CFG_OP_COUNT:
      break;
  }
  return ccol_invalid_args;
}

/* Tells whether ctx serves exactly what the fixture configured. */
static bool _cfg_unchanged(ctls_ctx_t *ctx) {
  char buf[128];
  for (size_t i = 0; i < sizeof(g_cfg_names) / sizeof(g_cfg_names[0]); ++i) {
    if (strcmp(_served_cn(ctx, g_cfg_names[i], buf, sizeof(buf)),
               g_cfg_before[i]) != 0)
      return false;
  }
  return _served_verify_mode(ctx) == SSL_VERIFY_NONE;
}

/* Tells whether ctx serves what a successful op configures. */
static bool _cfg_applied(ctls_ctx_t *ctx, cfg_op_t op) {
  char buf[128];
  switch (op) {
    case CFG_OP_ROTATE_NAME:
      return strcmp(_served_cn(ctx, "rot.test", buf, sizeof(buf)),
                    "127.0.0.1") == 0;
    case CFG_OP_ADD_NAME:
      return strcmp(_served_cn(ctx, "new.test", buf, sizeof(buf)),
                    "127.0.0.1") == 0;
    case CFG_OP_REPLACE_DEFAULT:
      return strcmp(_served_cn(ctx, NULL, buf, sizeof(buf)), "127.0.0.1") == 0;
    case CFG_OP_TRUST:
    case CFG_OP_TRUST_SYSTEM:
      return (_served_verify_mode(ctx) & SSL_VERIFY_PEER) != 0;
    case CFG_OP_COUNT:
      break;
  }
  return false;
}

/* How many failure points a sweep over OpenSSL's allocations visits by default.
 * One call makes a few thousand of them and every failure inside the rebuild
 * ends up in the same rollback, so evenly spaced points cover it.
 * CTLS_FULL_SWEEP=1 in the environment visits every index.
 */
#define CTLS_CFG_OSSL_POINTS 120

/* Fails allocation n of op on ctx, through OpenSSL when ossl is set and through
 * g_sweep_mp otherwise. Sets *fired if the call got that far. */
static ccol_retval_t _cfg_run_failing(ctls_ctx_t *ctx, cfg_op_t op, bool ossl,
                                      long n, bool *fired) {
  ccol_retval_t rv;
  if (ossl) {
    _ossl_arm(n);
    rv = _cfg_run_op(ctx, op);
    _ossl_disarm();
    *fired = atomic_load(&g_ossl_alloc_fired);
  } else {
    _sweep_arm((int)n);
    rv = _cfg_run_op(ctx, op);
    *fired = atomic_load(&g_alloc_seen) >= n;
    _sweep_disarm();
  }
  return rv;
}

/* The failure points of a sweep: 1, 1 + step, ... up to total, then total
 * itself, then total + 1, which the call never reaches. */
static long _cfg_next_point(long n, long step, long total) {
  if (n >= total) return n + 1;
  return n + step > total ? total : n + step;
}

/* Sweeps one op through the allocations it makes and then once more past the
 * last one, where the call is expected to succeed. Returns the number of
 * failed calls, or -1 on a breach. */
static long _cfg_sweep(cfg_op_t op, bool ossl, long *oom_failures) {
  /* A call that hits no failure point counts the points it goes past. */
  ctls_ctx_t *ctx = _cfg_fixture(&g_sweep_mp);
  if (!ctx) return -1;
  bool fired = false;
  ccol_retval_t rv = _cfg_run_failing(ctx, op, ossl, LONG_MAX / 2, &fired);
  long total =
      ossl ? atomic_load(&g_ossl_alloc_seen) : atomic_load(&g_alloc_seen);
  ctls_ctx_release(ctx);
  if (rv != ccol_success || fired || total <= 0) return -1;

  long step = 1;
  if (ossl && !getenv("CTLS_FULL_SWEEP") && total > CTLS_CFG_OSSL_POINTS)
    step = total / CTLS_CFG_OSSL_POINTS;
  long failures = 0;
  ctx = NULL;
  for (long n = 1; n <= total + 1; n = _cfg_next_point(n, step, total)) {
    if (!ctx) ctx = _cfg_fixture(&g_sweep_mp);
    if (!ctx || !_cfg_unchanged(ctx)) break;
    rv = _cfg_run_failing(ctx, op, ossl, n, &fired);
    if (rv == ccol_success) {
      bool applied = _cfg_applied(ctx, op);
      ctls_ctx_release(ctx);
      ctx = NULL;
      if (!applied) return -1;
      if (n == total + 1) return fired ? -1 : failures;
      continue;
    }
    if (n == total + 1) break;
    if (rv == ccol_not_enough_memory) ++*oom_failures;
    if (rv != ccol_not_enough_memory && rv != ccol_http_tls_cert_load_failed)
      break;
    ++failures;
    /* A failed call changes nothing, neither now nor after a later rebuild. */
    if (!_cfg_unchanged(ctx)) break;
    if (ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL) != ccol_success ||
        !_cfg_unchanged(ctx))
      break;
  }
  if (ctx) ctls_ctx_release(ctx);
  return -1;
}

/* Suppose a rotation stored the new entry before the rebuild and kept it
 * after a failed one. Then the rotated name would serve the default
 * certificate from then on, and if a new name stayed registered, the next
 * rebuild would serve it with the certificate the call refused. */
TEST(ctls_cfg_rollback, a_failed_certificate_change_keeps_every_name) {
  if (!g_certs_ready) return;
  for (int op = CFG_OP_ROTATE_NAME; op <= CFG_OP_REPLACE_DEFAULT; ++op) {
    long oom = 0;
    long lib_failures = _cfg_sweep((cfg_op_t)op, false, &oom);
    REQUIRE_GT(lib_failures, 0L);
    if (!g_ossl_mem_hooked) continue;
    long ossl_oom = 0;
    long ossl_failures = _cfg_sweep((cfg_op_t)op, true, &ossl_oom);
    REQUIRE_GT(ossl_failures, 0L);
    /* A failed OpenSSL allocation inside the rebuild is reported as one. */
    REQUIRE_GT(ossl_oom, 0L);
  }
}

/* A failed trust call that kept its bundle and its verify flag would show up
 * on the next unrelated rebuild, which would turn on SSL_VERIFY_PEER with the
 * bundle the call refused. */
TEST(ctls_cfg_rollback, a_failed_trust_change_keeps_verification_off) {
  if (!g_certs_ready) return;
  for (int op = CFG_OP_TRUST; op < CFG_OP_COUNT; ++op) {
    long oom = 0;
    long lib_failures = _cfg_sweep((cfg_op_t)op, false, &oom);
    REQUIRE_GT(lib_failures, 0L);
    /* trust_system loads the whole system store into every context it builds,
     * about 10^5 OpenSSL allocations per call, so here only this module's
     * allocations fail its rebuild. */
    if (!g_ossl_mem_hooked || op == CFG_OP_TRUST_SYSTEM) continue;
    long ossl_oom = 0;
    long ossl_failures = _cfg_sweep((cfg_op_t)op, true, &ossl_oom);
    REQUIRE_GT(ossl_failures, 0L);
    REQUIRE_GT(ossl_oom, 0L);
  }
}

/* We also check a served name through a real handshake: after a failed
 * rotation, a client naming the rotated host gets the certificate that the host
 * served before the call. */
TEST(ctls_cfg_rollback, a_failed_rotation_serves_the_old_certificate) {
  if (!g_certs_ready || !g_ossl_mem_hooked) return;
  ctls_ctx_t *server_ctx = _cfg_fixture(NULL);
  REQUIRE_NE((void *)server_ctx, (void *)NULL);
  /* The last allocations of a rotation belong to its rebuild, so a sweep from
   * the end hits a failure there first. */
  long total = 0;
  {
    ctls_ctx_t *probe = _cfg_fixture(NULL);
    _ossl_arm(LONG_MAX);
    ccol_retval_t rv = _cfg_run_op(probe, CFG_OP_ROTATE_NAME);
    _ossl_disarm();
    total = atomic_load(&g_ossl_alloc_seen);
    ctls_ctx_release(probe);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }
  ccol_retval_t rv = ccol_success;
  for (long n = total; n > 0 && rv == ccol_success; --n) {
    _ossl_arm(n);
    rv = _cfg_run_op(server_ctx, CFG_OP_ROTATE_NAME);
    _ossl_disarm();
    if (rv == ccol_success) {
      ctls_ctx_release(server_ctx);
      server_ctx = _cfg_fixture(NULL);
      REQUIRE_NE((void *)server_ctx, (void *)NULL);
    }
  }
  bool failed = rv != ccol_success;
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "rot.test", false, NULL);
  bool done = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  char cn_buf[128];
  const char *cn = done ? _peer_cert_cn(_ctls_conn_ssl_for_tests(client_conn),
                                        cn_buf, sizeof(cn_buf))
                        : NULL;
  bool served_old = cn && strcmp(cn, "named.test") == 0;
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(failed);
  REQUIRE_TRUE(done);
  REQUIRE_TRUE(served_old);
}

/* A trust bundle gets into every context whole or not at all. The bundle has
 * three CAs and one CRL. After a call that fails one allocation the context
 * serves what it served before, and after a call that succeeds every context
 * holds all three CAs and the CRL, checks revocation, offers the fixture's ALPN
 * list, and the thread's error queue is empty. A second bundle that repeats a
 * CA of the first one is accepted.
 *
 * Each part of that is checked. If the rebuild ignored a parse that gives
 * nothing, or an add that fails, a call under a failed allocation would
 * succeed with fewer CAs, or without the CRL and so with revocation checking
 * off. If it ignored a failed SSL_CTX_set_alpn_protos, it would succeed with a
 * context that offers no protocol. */

static char g_bundle3[320];
static int g_bundle3_state = 0; /* 0 untried, 1 ready, -1 failed */

/* Writes server.pem, client.pem, named.pem and a CRL issued by named.test into
 * one bundle, using the openssl CLI. */
static bool _bundle3_ready(void) {
  if (g_bundle3_state != 0) return g_bundle3_state > 0;
  g_bundle3_state = -1;
  if (!g_certs_ready) return false;
  snprintf(g_bundle3, sizeof(g_bundle3), "%s/bundle3.pem", g_cert_dir);
  char cmd[2048];
  int n = snprintf(
      cmd, sizeof(cmd),
      "cd '%s' && printf '[ca]\\ndefault_ca=x\\n[x]\\ndatabase=crl.idx\\n"
      "certificate=named.pem\\nprivate_key=named_key.pem\\n"
      "default_md=sha256\\ndefault_crl_days=1\\n' > crl.cnf && : > crl.idx"
      " && openssl ca -batch -config crl.cnf -gencrl -out crl.pem"
      " >/dev/null 2>&1 && cat server.pem client.pem named.pem crl.pem"
      " > bundle3.pem",
      g_cert_dir);
  if (n < 0 || (size_t)n >= sizeof(cmd) || system(cmd) != 0) return false;
  g_bundle3_state = 1;
  return true;
}

static void _bundle3_remove(void) {
  static const char *const files[] = {"crl.cnf",     "crl.idx", "crl.idx.attr",
                                      "crl.idx.old", "crl.pem", "bundle3.pem"};
  char path[512];
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    snprintf(path, sizeof(path), "%s/%s", g_cert_dir, files[i]);
    unlink(path);
  }
}

/* Tells whether the context serving name holds exactly the three CAs and the
 * CRL of the bundle, with revocation checking on. */
static bool _bundle3_in_store(ctls_ctx_t *ctx, const char *name) {
  SSL_CTX *served = _ctls_ctx_served_ctx_for_tests(ctx, name);
  X509_STORE *store = served ? SSL_CTX_get_cert_store(served) : NULL;
  if (!store) return false;
  STACK_OF(X509_OBJECT) *objs = X509_STORE_get0_objects(store);
  int certs = 0;
  int crls = 0;
  for (int i = 0; i < sk_X509_OBJECT_num(objs); ++i) {
    X509_LOOKUP_TYPE t = X509_OBJECT_get_type(sk_X509_OBJECT_value(objs, i));
    if (t == X509_LU_X509) ++certs;
    if (t == X509_LU_CRL) ++crls;
  }
  unsigned long flags =
      X509_VERIFY_PARAM_get_flags(X509_STORE_get0_param(store));
  return certs == 3 && crls == 1 && (flags & X509_V_FLAG_CRL_CHECK) != 0 &&
         (flags & X509_V_FLAG_CRL_CHECK_ALL) != 0 &&
         (SSL_CTX_get_verify_mode(served) & SSL_VERIFY_PEER) != 0;
}

/* Tells whether a client hello of served offers exactly the fixture's ALPN list
 * "h2": extension 16, length 5, list length 3. */
static bool _ctx_offers_h2(SSL_CTX *served) {
  static const unsigned char ext[] = {0x00, 0x10, 0x00, 0x05, 0x00,
                                      0x03, 0x02, 'h',  '2'};
  SSL *ssl = served ? SSL_new(served) : NULL;
  BIO *rbio = BIO_new(BIO_s_mem());
  BIO *wbio = BIO_new(BIO_s_mem());
  bool found = false;
  if (ssl && rbio && wbio) {
    SSL_set_bio(ssl, rbio, wbio);
    rbio = wbio = NULL;
    SSL_set_connect_state(ssl);
    (void)SSL_do_handshake(ssl);
    char *bytes = NULL;
    long len = BIO_get_mem_data(SSL_get_wbio(ssl), &bytes);
    for (long i = 0; bytes && i + (long)sizeof(ext) <= len && !found; ++i)
      found = memcmp(bytes + i, ext, sizeof(ext)) == 0;
  }
  BIO_free(rbio);
  BIO_free(wbio);
  SSL_free(ssl);
  ERR_clear_error();
  return found;
}

/* Runs ctls_ctx_trust(bundle3) on a fresh fixture with allocation n failing,
 * through OpenSSL when ossl is set. Returns 1 for a success that installed the
 * whole bundle, 0 for a clean failure that changed nothing, 2 when n was never
 * reached and -1 for a breach. */
static int _bundle3_try(bool ossl, long n) {
  ctls_ctx_t *ctx = _cfg_fixture(&g_sweep_mp);
  if (!ctx) return -1;
  ccol_retval_t rv;
  bool fired;
  ERR_clear_error();
  if (ossl) {
    _ossl_arm(n);
    rv = ctls_ctx_trust(ctx, g_bundle3, NULL);
    _ossl_disarm();
    fired = atomic_load(&g_ossl_alloc_fired);
  } else {
    _sweep_arm((int)n);
    rv = ctls_ctx_trust(ctx, g_bundle3, NULL);
    fired = atomic_load(&g_alloc_seen) >= n;
    _sweep_disarm();
  }
  unsigned long queued = ERR_peek_error();
  int out = -1;
  if (rv == ccol_success) {
    if (queued == 0 && _bundle3_in_store(ctx, NULL) &&
        _bundle3_in_store(ctx, "keep.test") &&
        _ctx_offers_h2(_ctls_ctx_served_ctx_for_tests(ctx, NULL)) &&
        _ctx_offers_h2(_ctls_ctx_served_ctx_for_tests(ctx, "keep.test")) &&
        _ctx_offers_h2(_ctls_ctx_served_ctx_for_tests(ctx, "rot.test")))
      out = fired ? 1 : 2;
  } else if ((rv == ccol_not_enough_memory ||
              rv == ccol_http_tls_cert_load_failed) &&
             fired && _cfg_unchanged(ctx) &&
             ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL) ==
                 ccol_success &&
             _cfg_unchanged(ctx)) {
    out = 0;
  }
  ERR_clear_error();
  ctls_ctx_release(ctx);
  return out;
}

TEST(ctls_cfg_rollback, a_trust_bundle_is_installed_whole_or_not_at_all) {
  if (!_bundle3_ready()) {
    fprintf(stderr, "SKIP: the openssl CLI cannot make a CRL\n");
    return;
  }
  long breaches = 0;
  long failures = 0;
  bool finished = true;
  for (int pass = 0; pass < 2 && finished; ++pass) {
    bool ossl = pass == 1;
    if (ossl && !g_ossl_mem_hooked) break;
    /* A call that hits no failure point counts the points it goes past. */
    if (_bundle3_try(ossl, LONG_MAX / 2) != 2) {
      ++breaches;
      break;
    }
    long total =
        ossl ? atomic_load(&g_ossl_alloc_seen) : atomic_load(&g_alloc_seen);
    long step = 1;
    if (ossl && !getenv("CTLS_FULL_SWEEP") && total > CTLS_CFG_OSSL_POINTS * 4)
      step = total / (CTLS_CFG_OSSL_POINTS * 4);
    finished = false;
    for (long n = 1; n <= total + 1; n = _cfg_next_point(n, step, total)) {
      int r = _bundle3_try(ossl, n);
      if (r < 0) ++breaches;
      if (r == 0) ++failures;
      if (r == 2) {
        finished = n == total + 1;
        break;
      }
    }
  }
  /* Sharing a CA between two bundles is no reason to fail. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  ccol_retval_t first = ctls_ctx_trust(ctx, g_server_cert, NULL);
  ccol_retval_t second = ctls_ctx_trust(ctx, g_bundle3, NULL);
  ctls_ctx_release(ctx);
  _bundle3_remove();
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(breaches, 0L);
  REQUIRE_GT(failures, 0L);
  REQUIRE_EQ((int)first, (int)ccol_success);
  REQUIRE_EQ((int)second, (int)ccol_success);
}

/* Reading nothing returns 0 at once, like read(2), both on a connection with
 * no input and on one with input waiting, and the input stays there for the
 * next read. Without that shortcut SSL_read with a length of 0 returns 0,
 * and the classification would report that as -1 with ECONNRESET or
 * EWOULDBLOCK.
 */
TEST(ctls_io, a_read_of_zero_bytes_returns_zero) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "x.test", false, NULL);
  bool done = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  char buf[16];
  ssize_t empty = done ? ctls_conn_read(server_conn, buf, 0) : -2;
  ssize_t wrote = done ? ctls_conn_write(client_conn, "ping", 4) : -2;
  ssize_t pending = -2, got = -2;
  for (int i = 0; done && i < 200 && got != 4; ++i) {
    pending = ctls_conn_read(server_conn, buf, 0);
    if (pending != 0) break;
    got = ctls_conn_read(server_conn, buf, sizeof(buf));
    if (got < 0 && errno == EWOULDBLOCK) got = -2;
  }
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
  REQUIRE_TRUE(done);
  REQUIRE_EQ(empty, (ssize_t)0);
  REQUIRE_EQ(wrote, (ssize_t)4);
  REQUIRE_EQ(pending, (ssize_t)0);
  REQUIRE_EQ(got, (ssize_t)4);
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
   * controlling terminal instead of returning, and in a test binary or a daemon
   * that's a hang rather than a failure. */
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
  /* A key that loads isn't necessarily a key that works, so this shows that the
   * decrypted key really backs a completed handshake. */
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
  /* A zero-length file opens and seeks just fine, so only a size check catches
   * it before the PEM parser is handed nothing. */
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
  /* procfs entries report a length of zero although they have content, and that
   * takes the same branch as an empty regular file. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ccol_retval_t rv = ctls_ctx_trust(ctx, "/proc/self/status", NULL);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_file_input, a_directory_in_place_of_a_bundle_reports_a_load_failure) {
  /* Pointing a bundle option at a directory by mistake is an ordinary
   * configuration slip. Opening a directory succeeds on Linux, and depending on
   * the filesystem the seek after it either fails or reports a length of
   * LONG_MAX, so the rejection must come from the type of the file and not
   * from its apparent size. Without that check the LONG_MAX case reaches the
   * allocation and AddressSanitizer stops the process with
   * allocation-size-too-big. The test makes its directory here and not under
   * /tmp, so the case is the same whatever filesystem the tests run on.
   */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  /* The directory sits next to the test binary and not under /tmp because
   * whether a seek on a directory works depends on the filesystem. It fails on
   * tmpfs, which /tmp often is, and succeeds on ext4. The build tree is the one
   * place guaranteed to be on the same filesystem the library normally reads
   * from. */
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
  /* A PEM artefact is a couple of kilobytes and a full system CA bundle a few
   * hundred kilobytes, but a path can point at something completely different,
   * such as a log, an image or a core dump, which we turn away on its
   * size instead of being read into memory. The file below is sparse, so making
   * it costs no disk space and no time. */
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
  /* The reported reason names the size and must not claim that the file
   * couldn't be read. An operator who is told "unreadable" about a file that
   * reads perfectly well goes looking for a permissions problem that doesn't
   * exist. */
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "larger than"), (void *)NULL);
}

TEST(ctls_file_input, a_bundle_just_under_the_limit_is_still_read) {
  /* The cap must reject only what goes past it. A file below the limit is read
   * normally and fails later on its contents, not on its size. */
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
  /* The library rejects this file on its contents, because it holds no
   * certificate, and not on its size, and that difference is the whole point of
   * this test. The two branches report different reasons, so the reason tells
   * us which one ran: a file below the cap has to reach the content check
   * instead of being turned away for its length. */
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) {
    REQUIRE_NE((void *)strstr(err, "no certificates"), (void *)NULL);
    REQUIRE_EQ((void *)strstr(err, "larger than"), (void *)NULL);
  }
}

TEST(ctls_file_input, a_bundle_holding_no_certificate_is_rejected) {
  /* A readable file can parse to no certificate at all, which leaves a trust
   * store with no issuer in it while peer verification is on. A client built
   * from such a context rejects every peer. A server in the request-only mode
   * of ctls_ctx_peer_cert_optional() behaves differently: it keeps accepting
   * every client that presents no certificate, so the mutual TLS that the
   * bundle was configured for silently isn't there. The configuration call has
   * to fail instead. */
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
/* chttpclient and chttpserver branch on this pair of a return value and an   */
/* errno on every read and every write they make, so the pair is the          */
/* interface between ctls and the two largest modules of the library.         */
/* ========================================================================== */

/* Builds a TLS session over a socketpair with the handshake done. Returns false
 * if the handshake couldn't be driven to the end, and in that case nothing is
 * left allocated for the caller to release. */
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
  /* A read that is only waiting for more data wants the read direction, and a
   * caller that re-armed for writability here would stall the connection. */
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
  /* Zero means that the peer closed the session properly, and a caller must not
   * mix that up with the -1 that asks for a retry. */
  ctls_ctx_t *sc, *cc;
  ctls_conn_t *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  if (up) {
    ctls_conn_destroy(c); /* sends the TLS close_notify that the peer sees */
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
  /* Plaintext where a TLS record should be is a protocol error, and the caller
   * is told the connection is unusable, not that it can retry. */
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
    /* Closing the descriptor under the peer skips close_notify altogether, so
     * this is the truncation case and not the clean shutdown above. */
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
  /* The one case where a caller has to re-arm for writability after a read-side
   * API call, and ctls_conn_wants_write is how it finds out. */
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

/* Completes one handshake against server_ctx with the given SNI name (a NULL
 * name sends no SNI extension at all) and reports the common name of the
 * certificate the server really served. */
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
  /* Both files read fine and are well-formed PEM, but they come from two
   * different pairs, which is exactly what a rotation produces when it updates
   * one of the two paths and forgets the other. */
  ctls_ctx_t *ctx = ctls_ctx_new(NULL);
  char *default_err = NULL;
  ccol_retval_t default_rv = ctls_ctx_cert_add(
      ctx, NULL, g_server_cert, g_client_key, NULL, &default_err);
  char *named_err = NULL;
  ccol_retval_t named_rv = ctls_ctx_cert_add(
      ctx, "mismatch.test", g_server_cert, g_client_key, NULL, &named_err);
  /* The matching pair is still accepted, so the check rejects a real
   * mismatch and not every file-backed pair. */
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
  /* A rotation that picks up the new certificate together with the old key. */
  ccol_retval_t rotate_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                              g_client_key, NULL, NULL);
  bool served = false;
  char cn[256];
  _serve_one_sni(server_ctx, NULL, &served, cn, sizeof(cn));
  /* A pair that the library rejected must not stay in the stored configuration
   * either. A later, unrelated configuration change rebuilds every context from
   * that store, and the rebuild would fail on the rejected pair again. */
  ccol_retval_t later_rv = ctls_ctx_trust(server_ctx, g_client_cert, NULL);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(first_rv, ccol_success);
  REQUIRE_EQ(rotate_rv, ccol_http_tls_cert_load_failed);
  /* Accepting the mismatch leaves a context that can't serve a single
   * handshake, so this fails outright instead of just serving the wrong name.
   */
  REQUIRE_TRUE(served);
  REQUIRE_STREQ(cn, "127.0.0.1");
  REQUIRE_EQ(later_rv, ccol_success);
}

/* Which certificate a hostname gets must depend on the hostname and not on
 * the order in which the certificates were registered, so the test below runs
 * both orders. A lookup that returns the first pattern that happens to match
 * would serve the wildcard certificate for every exactly registered name in
 * whichever order puts the wildcard first. */
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
  /* A name with no exact registration of its own still reaches the wildcard, so
   * the precedence rule doesn't take away the wildcard's own coverage. */
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
  /* The lookup makes a single probe, and that probe encodes one rule:
   * "*.wild.test" answers for one label below "wild.test" and for nothing else.
   * A name with two labels falls through to the default certificate, and so
   * does the bare domain. */
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

/* The peer here is a plain OpenSSL client and not a ctls client, since that is
 * what every real client that resumes a session looks like: it keeps the
 * session of one connection and offers it on the next one. */

typedef struct {
  int max_version;   /* TLS1_2_VERSION or TLS1_3_VERSION */
  bool present_cert; /* the client presents g_client_cert */
  const char *sni;   /* NULL for no SNI */
} _resume_case_t;

/* Drives a ctls server connection and a raw OpenSSL client to the end of the
 * handshake, then moves one byte from the server to the client. That makes the
 * client read past the handshake, which is where a TLS 1.3 client takes in the
 * server's session tickets. */
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

/* Runs one connection. *sess_inout is offered when it isn't NULL and gets the
 * session of this connection, and *reused_out gets what the client and the
 * server each say about resumption. */
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

/* LibreSSL resumes no TLS 1.3 session, and its SSL_SESSION_is_resumable() says
 * 0 for every session, including a TLS 1.2 session that it does resume, so
 * there a session issued by the first connection is enough. */
#if defined(LIBRESSL_VERSION_NUMBER)
#define RESUME_TLS13 0
#define RESUME_SESSION_ISSUED(s) ((s) != NULL)
#else
#define RESUME_TLS13 1
#define RESUME_SESSION_ISSUED(s) \
  ((s) != NULL && SSL_SESSION_is_resumable(s) == 1)
#endif

/* Returns true when both sides report a resumed session on the second of two
 * connections and neither side reports one on the first. */
static bool _resume_case_resumes(const _resume_case_t *rc) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  bool ok = server_ctx &&
            ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success &&
            ctls_ctx_trust(server_ctx, g_client_cert, NULL) == ccol_success;
  if (ok && rc->sni)
    ok = ctls_ctx_cert_add(server_ctx, rc->sni, NULL, NULL, NULL, NULL) ==
         ccol_success;
  /* A client that presents no certificate gets to a server with a trust store
   * only when that server merely requests one, while a client that presents one
   * resumes against the default, which requires it. */
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
  ok = ok && RESUME_SESSION_ISSUED(sess);
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
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (!RESUME_TLS13 && cases[i].max_version == TLS1_3_VERSION) continue;
    if (!_resume_case_resumes(&cases[i])) failed_mask |= 1 << i;
  }
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
  /* The same two calls are accepted when the check is switched off on purpose.
   */
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
  /* Room for the whole record in one write. A FreeBSD socketpair holds about
   * 8 KiB per direction by default, while Linux holds a lot more. */
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

  /* A single write of 12000 bytes is one TLS record, so a 4096-byte read leaves
   * the rest of that record inside the TLS layer while the socket holds nothing
   * more. */
  static char payload[12000];
  memset(payload, 'p', sizeof(payload));
  bool pending_before = true, pending_mid = false, pending_after = true;
  bool socket_empty_mid = false;
  size_t total = 0;
  if (hs) {
    /* Let the server take in whatever the handshake left, such as TLS 1.3
     * tickets going the other way, before we measure. */
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
