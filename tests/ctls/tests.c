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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/ctls.h>
#include <internal/test_helpers/chashmap.h>
#include <internal/test_helpers/ctls.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

#if !defined(LIBRESSL_VERSION_NUMBER)
#include <openssl/rand.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#else
#include <openssl/rand_drbg.h>
#endif
#endif

TAU_MAIN()

// OpenSSL allocation failure
//
// OpenSSL takes a custom allocator only before its first allocation, so a
// priority-101 constructor installs a forwarder to malloc. While a test holds
// the window open, the forwarder fails one chosen allocation of the arming
// thread; otherwise it behaves exactly like malloc. LibreSSL has no such hook,
// so the tests that need it skip there.
//
// Whether the forwarder got installed, how many allocations the armed thread
// made, which one fails (0 disarms), whether it failed, and the armed thread.
static bool g_ossl_mem_hooked = false;
static _Atomic long g_ossl_alloc_seen = 0;
static _Atomic long g_ossl_alloc_fail_at = 0;
static _Atomic bool g_ossl_alloc_fired = false;
static pthread_t g_ossl_alloc_thread;

#if !defined(LIBRESSL_VERSION_NUMBER)
// Tells the forwarders whether this allocation fails: Only on the armed thread
// and only the g_ossl_alloc_fail_at-th one counted in g_ossl_alloc_seen. It
// counts the call and sets g_ossl_alloc_fired when it fails one.
static bool _ossl_should_fail(void) {
  long at = atomic_load(&g_ossl_alloc_fail_at);
  if (at == 0 || !pthread_equal(pthread_self(), g_ossl_alloc_thread))
    return false;
  if (atomic_fetch_add(&g_ossl_alloc_seen, 1) + 1 != at) return false;
  atomic_store(&g_ossl_alloc_fired, true);
  return true;
}

// The OpenSSL allocator forwarders: malloc and realloc that fail when
// _ossl_should_fail() says so, and a plain free.
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

// Installs the forwarders before any other constructor can make OpenSSL
// allocate, and records in g_ossl_mem_hooked whether OpenSSL took them.
__attribute__((constructor(101))) static void _hook_openssl_allocator(void) {
  g_ossl_mem_hooked =
      CRYPTO_set_mem_functions(_ossl_malloc, _ossl_realloc, _ossl_free) == 1;
}
#endif

// Sets when the calling thread's two OpenSSL DRBGs reseed: After secs seconds
// and after requests random draws, where 0 turns a trigger off (EVP_RAND(7),
// RAND_DRBG_reseed(3)). A reseed allocates, so a reseed that falls into a
// sweep run makes it allocate more than the counted run and fails it for no
// fault of ours; under valgrind the suite outlives the 7-minute default.
// _ossl_arm() calls it with (0, 0), which also creates both DRBGs outside the
// count. Only the test thread calls it.
static void _openssl_set_drbg_reseed(unsigned int secs, unsigned int requests) {
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  OSSL_PARAM params[3] = {
      OSSL_PARAM_construct_uint(OSSL_DRBG_PARAM_RESEED_TIME_INTERVAL, &secs),
      OSSL_PARAM_construct_uint(OSSL_DRBG_PARAM_RESEED_REQUESTS, &requests),
      OSSL_PARAM_construct_end()};
  EVP_RAND_CTX_set_params(RAND_get0_public(NULL), params);
  EVP_RAND_CTX_set_params(RAND_get0_private(NULL), params);
#elif !defined(LIBRESSL_VERSION_NUMBER)
  RAND_DRBG *drbgs[2] = {RAND_DRBG_get0_public(), RAND_DRBG_get0_private()};
  for (int i = 0; i < 2; ++i) {
    RAND_DRBG_set_reseed_time_interval(drbgs[i], (time_t)secs);
    RAND_DRBG_set_reseed_interval(drbgs[i], requests);
  }
#else
  (void)secs;
  (void)requests;
#endif
}

// Arms the forwarders to fail allocation number nth of the calling thread
// (0 only counts). It turns off DRBG reseeding first and resets the counters
// and the fired flag.
static void _ossl_arm(long nth) {
  _openssl_set_drbg_reseed(0, 0);
  g_ossl_alloc_thread = pthread_self();
  atomic_store(&g_ossl_alloc_seen, 0);
  atomic_store(&g_ossl_alloc_fired, false);
  atomic_store(&g_ossl_alloc_fail_at, nth);
}
// Disarms the forwarders, so every allocation succeeds again.
static void _ossl_disarm(void) { atomic_store(&g_ossl_alloc_fail_at, 0); }

// Aborts at process exit when a chashmap iterator of this run was never freed,
// so such a leak fails loudly here instead of as a rare valgrind report.
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

// Throwaway certificate generation
//
// The openssl command makes real PEM files once at startup, so the tests reach
// the cert_path and key_path branch of ctls_ctx_cert_add. ctls never stops the
// process for a bad file, so no separate binary is needed; without the openssl
// command the affected tests skip.
//
// The scratch directory and the paths of the generated certificates and keys,
// and whether the generation (and the encrypted key separately) worked.
static char g_cert_dir[256];
static char g_server_cert[320], g_server_key[320];
static char g_client_cert[320], g_client_key[320];
static char g_enc_cert[320], g_enc_key[320];
static char g_named_cert[320], g_named_key[320];
static bool g_certs_ready = false;
static bool g_enc_cert_ready = false;

// The password of g_enc_key, which a ctls_ctx_cert_add caller passes as
// pk_password.
#define CTLS_TEST_KEY_PASSWORD "hunter2"

// Runs the openssl command to make a self-signed RSA certificate at cert_path
// with its key at key_path, subject CN cn and, when san isn't NULL, a
// subjectAltName. Gives 0 when both files are readable afterwards, -1
// otherwise.
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

// Like _openssl_selfsigned() with a key encrypted under password. It's the
// only way to reach the PEM password callback of ctls, which turns a wrong
// password into a load failure instead of a terminal prompt.
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

// Creates g_cert_dir and every certificate the suite uses: The server
// (127.0.0.1 with an IP SAN), the client, named.test and the encrypted key.
// It fills the path globals and g_enc_cert_ready, which is separate so an
// openssl without the needed cipher skips only the password tests. Gives -1
// when a required file couldn't be made.
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
  g_enc_cert_ready =
      (_openssl_selfsigned_encrypted(g_enc_key, g_enc_cert, "enc.test",
                                     CTLS_TEST_KEY_PASSWORD) == 0);
  return 0;
}

static void _teardown(void);

// Prepares the binary: Registers _teardown() with atexit (OpenBSD runs no
// destructor of a program that links libpthread), ignores SIGPIPE so a
// regression can't kill the binary (tests_sigpipe asserts the property),
// resets inherited signals so system() works, and generates the
// certificates into g_certs_ready.
__attribute__((constructor)) static void _setup(void) {
  atexit(_teardown);
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
  g_certs_ready = (_generate_all_certs() == 0);
  if (!g_certs_ready) {
    fprintf(stderr,
            "WARNING: could not generate test certs via the openssl CLI; "
            "file-backed cert tests will be skipped.\n");
  }
}

static void _bundle3_remove(void);

// Removes every file the suite generated and its directory, at exit.
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
// LibreSSL has no SSL_has_pending(), so the socket BIO follows the record
// framing itself. An SSLv2-format hello read as a TLS header claims a 771-byte
// body; 748 bytes of real records follow and end inside a record. A tracker
// that doesn't give up on a header with no valid content type counts them as
// the rest of that body and reports nothing pending.
TEST(ctls_libressl, framing_tracker_gives_up_on_an_sslv2_hello) {
  static const char v2[28] = {(char)0x80, 0x1c, 0x01, 0x03, 0x03, 0x00, 0x03,
                              0x00,       0x00, 0x00, 0x10, 0x00, 0x00, 0x2f};
  static char tail[748];
  int at = 0;
  for (int r = 0; r < 3; r++) {
    tail[at++] = 0x17;
    tail[at++] = 0x03;
    tail[at++] = 0x03;
    tail[at++] = 0x01;
    tail[at++] = 0x2c;
    int body = r < 2 ? 300 : 748 - at;
    for (int i = 0; i < body; i++) tail[at++] = 'x';
  }
  const char *chunks[2] = {v2, tail};
  int lens[2] = {(int)sizeof(v2), at};
  REQUIRE_EQ(at, 748);
  REQUIRE_TRUE(ctls__rec_track_pending_after_for_tests(chunks, lens, 2));
}
#endif

// Handshake driver helpers
//
// Makes a non-blocking AF_UNIX socketpair in fds, the transport of most tests:
// it has real socket semantics and needs no network or port, and hostname
// verification doesn't care about the address family. A test fails (instead
// of later hanging on a blocking read) when the pair can't be made
// non-blocking.
static void _make_nonblocking_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  for (int i = 0; i < 2; ++i) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

// Steps the handshakes of a and b in turn until both are done or max_iters
// runs out. No poll is needed, because the shared socketpair makes each
// side's output readable by the next step of the other. *out_a_ok and
// *out_b_ok (may be NULL) get each side's result; it gives true when both
// succeeded.
static bool _drive_both(ctls_conn *a, ctls_conn *b, int max_iters,
                        bool *out_a_ok, bool *out_b_ok) {
  bool a_done = false, b_done = false, a_ok = true, b_ok = true;
  for (int i = 0; i < max_iters && !(a_done && b_done); ++i) {
    if (!a_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) {
        a_done = true;
      } else if (r == CTLS_HANDSHAKE_ERROR) {
        a_done = true;
        a_ok = false;
      }
    }
    if (!b_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(b);
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

// Copies the CN of ssl's peer certificate into buf and gives buf, or NULL when
// there's no certificate or CN, so a test can see which certificate was
// served.
static const char *_peer_cert_cn(SSL *ssl, char *buf, size_t buflen) {
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
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

TEST(ctls_ctx, new_default_alloc) {
  char *err = NULL;
  ctls_ctx *ctx = ctls_ctx_new(&err);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ctls_ctx_release(ctx);
}

// Context-level behavior
//
// An allocator that counts its malloc and calloc calls in
// g_custom_malloc_calls, so a test can see a context allocate through the
// caller's procs.
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

// An RSA-1024 key with a SHA-1 signature matches but the TLS library won't
// serve it; ctls_ctx_cert_add must refuse it before storing anything, or every
// later rebuild of the context would fail too. Skips where the library serves
// such a pair.
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
  ctls_ctx *ctx = ctls_ctx_new(&err);
  REQUIRE_NE((void *)ctx, NULL);
  ccol_retval good =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, &err);
  ccol_retval weak =
      ctls_ctx_cert_add(ctx, "weak.test", weak_cert, weak_key, NULL, &err);
  ccol_retval later = ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, &err);
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
  ccol_memmgmt_procs procs = {.malloc = _custom_malloc,
                              .free = _custom_free,
                              .calloc = _custom_calloc,
                              .realloc = _custom_realloc};
  char *err = NULL;
  ctls_ctx *ctx = ctls_ctx_new_mp(&procs, &err);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  REQUIRE_GT(g_custom_malloc_calls, 0);
  ctls_ctx_release(ctx);
}

// The request size _refusing_malloc() refuses, and how many it refused.
static size_t g_refused_size;
static long g_refused_allocs;
// malloc() that refuses every request of exactly g_refused_size bytes, so a
// test can fail the buffer that holds one named file and nothing else.
static void *_refusing_malloc(size_t n) {
  if (n == g_refused_size) {
    g_refused_allocs++;
    return NULL;
  }
  return malloc(n);
}

// Gives the size of the file at path, or 0 when stat fails.
static size_t _file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

// A failed allocation of the buffer a certificate, key or CA file is read into
// is ccol_not_enough_memory with a message saying so, never an unreadable
// file, and the same files load once memory is there.
TEST(ctls_ctx, a_file_buffer_that_cannot_be_allocated_is_out_of_memory) {
  if (!g_certs_ready) return;
  ccol_memmgmt_procs procs = {.malloc = _refusing_malloc,
                              .free = _custom_free,
                              .calloc = _custom_calloc,
                              .realloc = _custom_realloc};
  ctls_ctx *ctx = ctls_ctx_new_mp(&procs, NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char *cert_err = NULL, *key_err = NULL, *ca_err = NULL;
  g_refused_allocs = 0;
  g_refused_size = _file_size(g_server_cert);
  ccol_retval cert_rv = ctls_ctx_cert_add(ctx, NULL, g_server_cert,
                                          g_server_key, NULL, &cert_err);
  long cert_refusals = g_refused_allocs;
  g_refused_allocs = 0;
  g_refused_size = _file_size(g_server_key);
  ccol_retval key_rv =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, &key_err);
  long key_refusals = g_refused_allocs;
  g_refused_allocs = 0;
  g_refused_size = _file_size(g_client_cert);
  ccol_retval ca_rv = ctls_ctx_trust(ctx, g_client_cert, &ca_err);
  long ca_refusals = g_refused_allocs;
  g_refused_size = 0;
  ccol_retval cert_later =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
  ccol_retval ca_later = ctls_ctx_trust(ctx, g_client_cert, NULL);
  ctls_ctx_release(ctx);

  REQUIRE_GT(cert_refusals, 0);
  REQUIRE_GT(key_refusals, 0);
  REQUIRE_GT(ca_refusals, 0);
  REQUIRE_EQ(cert_rv, ccol_not_enough_memory);
  REQUIRE_EQ(key_rv, ccol_not_enough_memory);
  REQUIRE_EQ(ca_rv, ccol_not_enough_memory);
  REQUIRE_TRUE(cert_err && strstr(cert_err, "out of memory"));
  REQUIRE_TRUE(key_err && strstr(key_err, "out of memory"));
  REQUIRE_TRUE(ca_err && strstr(ca_err, "out of memory"));
  REQUIRE_EQ(cert_later, ccol_success);
  REQUIRE_EQ(ca_later, ccol_success);
}

TEST(ctls_ctx, cert_add_null_ctx_is_invalid) {
  ccol_retval rv = ctls_ctx_cert_add(NULL, "x", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
}

// Fails the run when the process-wide root key of the self-signed
// certificates is still held at the destructors, which run after every exit
// handler. Without the exit handler of ctls the key would stay allocated, and
// the OpenSSL suppressions would hide it from the memtest.
__attribute__((destructor)) static void _root_key_freed_at_exit(void) {
  if (ctls__root_key_held_for_tests()) {
    static const char msg[] = "ctls: the root key is still held at exit\n";
    ssize_t w = write(2, msg, sizeof(msg) - 1);
    (void)w;
    test_child_exit(1);
  }
}

TEST(ctls_ctx, cert_add_no_name_no_files_generates_self_signed_default) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv = ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_only_cert_path_is_invalid) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv =
      ctls_ctx_cert_add(ctx, NULL, "/nonexistent/cert.pem", NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_missing_file_reports_load_failure) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv = ctls_ctx_cert_add(ctx, NULL, "/nonexistent/cert.pem",
                                     "/nonexistent/key.pem", NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_self_signed_default_succeeds) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv =
      ctls_ctx_cert_add(ctx, "self-signed.test", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, cert_add_real_files_succeeds) {
  if (!g_certs_ready) return;
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_missing_file_reports_load_failure) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval rv = ctls_ctx_trust(ctx, "/nonexistent/ca.pem", NULL);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_null_args_invalid) {
  REQUIRE_EQ(ctls_ctx_trust(NULL, "x", NULL), ccol_invalid_args);
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust(ctx, NULL, NULL), ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, trust_system_null_ctx_invalid) {
  REQUIRE_EQ(ctls_ctx_trust_system(NULL), ccol_invalid_args);
}

TEST(ctls_ctx, trust_system_no_crash) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust_system(ctx), ccol_success);
  ctls_ctx_release(ctx);
}

// Points SSL_CERT_FILE at path and drops the parsed default CA file, so the
// next system-trust context reads the new file. Only children of
// _ctls_run_in_child() call it, so the parent's environment never changes.
static void _system_ca_env_set(const char *path) {
  setenv("SSL_CERT_FILE", path, 1);
  ctls__system_ca_forget_for_tests();
}

// Runs one handshake between a server serving g_server_cert and a client on
// client_ctx, so a test can ask what that client trusts. Gives true when the
// handshake finished and the client verified the server (X509_V_OK).
static bool _client_verifies_test_server(ctls_ctx *client_ctx) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  if (!server_ctx) return false;
  bool ok = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success;
  int fds[2] = {-1, -1};
  ctls_conn *server_conn = NULL, *client_conn = NULL;
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
         ctls_conn_verify_result(client_conn) == 0L;
  }
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  ctls_ctx_release(server_ctx);
  return ok;
}

// Runs body in a forked child and copies the out_size result bytes it fills
// back into out. Tests that change SSL_CERT_FILE use it so the parent's
// environment stays as it was (and musl, which keeps every setenv() block,
// leaves nothing for the memtest). Gives false, after killing the child, when
// no full result arrived within bound_ms.
static bool _ctls_run_in_child(void (*body)(void *out), void *out,
                               size_t out_size, int bound_ms) {
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
    test_child_exit(w == (ssize_t)out_size ? 0 : 1);
  }
  close(p[1]);
  size_t got = 0;
  while (got < out_size) {
    struct pollfd pf = {.fd = p[0], .events = POLLIN};
    if (poll(&pf, 1, bound_ms) <= 0) break;
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

// What the SSL_CERT_FILE child reports: The trust result and whether the
// client verified, with the variable at the server certificate and then at
// another one.
typedef struct {
  ccol_retval rv_trusting;
  bool verified;
  ccol_retval rv_other;
  bool other_verified;
} _system_ca_names_result_t;

// The child of ssl_cert_file_names_the_trusted_bundle: Trusts the system store
// with SSL_CERT_FILE at the server certificate and then at another one, and
// records in the _system_ca_names_result_t at out whether each context
// verified the server.
static void _system_ca_names_body(void *out) {
  _system_ca_names_result_t *r = (_system_ca_names_result_t *)out;
  _system_ca_env_set(g_server_cert);
  ctls_ctx *trusting = ctls_ctx_new(NULL);
  r->rv_trusting =
      trusting ? ctls_ctx_trust_system(trusting) : ccol_unexpected_failure;
  r->verified =
      r->rv_trusting == ccol_success && _client_verifies_test_server(trusting);
  _system_ca_env_set(g_client_cert);
  ctls_ctx *other = ctls_ctx_new(NULL);
  r->rv_other = other ? ctls_ctx_trust_system(other) : ccol_unexpected_failure;
  r->other_verified =
      r->rv_other == ccol_success && _client_verifies_test_server(other);
  ctls_ctx_release(trusting);
  ctls_ctx_release(other);
}

// The system store is the file SSL_CERT_FILE names: A client trusting it
// verifies a server whose self-signed certificate is that file, and stops once
// the variable names another certificate. Runs in a child.
TEST(ctls_system_ca, ssl_cert_file_names_the_trusted_bundle) {
  if (!g_certs_ready) return;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_names_result_t r = {ccol_unexpected_failure, false,
                                 ccol_unexpected_failure, true};
  bool ran = _ctls_run_in_child(_system_ca_names_body, &r, sizeof(r), 30000);
  REQUIRE_TRUE(ran);
  REQUIRE_EQ(r.rv_trusting, ccol_success);
  REQUIRE_TRUE(r.verified);
  REQUIRE_EQ(r.rv_other, ccol_success);
  REQUIRE_FALSE(r.other_verified);
}

// What the parse-once child reports: Whether every context was built, how
// many parses ran, and whether the last context verifies.
typedef struct {
  bool all_ok;
  long parses;
  bool verified;
} _system_ca_once_result_t;

// The child of the_default_file_is_parsed_once_per_process: Builds four
// system-trust contexts (each rebuilt once more by an ALPN add) and records
// how many parses that took and whether the last context verifies.
static void _system_ca_once_body(void *out) {
  _system_ca_once_result_t *r = (_system_ca_once_result_t *)out;
  _system_ca_env_set(g_server_cert);
  long before = ctls__system_ca_parses_for_tests();
  ctls_ctx *ctxs[4] = {NULL, NULL, NULL, NULL};
  r->all_ok = true;
  for (int i = 0; i < 4; ++i) {
    ctxs[i] = ctls_ctx_new(NULL);
    if (!ctxs[i] || ctls_ctx_trust_system(ctxs[i]) != ccol_success ||
        ctls_ctx_alpn_add(ctxs[i], "h2", NULL, NULL, NULL, NULL) !=
            ccol_success)
      r->all_ok = false;
  }
  r->parses = ctls__system_ca_parses_for_tests() - before;
  r->verified = r->all_ok && _client_verifies_test_server(ctxs[3]);
  for (int i = 0; i < 4; ++i) ctls_ctx_release(ctxs[i]);
}

// Every system-trust context, across rebuilds too, shares one parse of the
// default CA file. Runs in a child.
TEST(ctls_system_ca, the_default_file_is_parsed_once_per_process) {
  if (!g_certs_ready) return;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_once_result_t r = {false, -1, false};
  bool ran = _ctls_run_in_child(_system_ca_once_body, &r, sizeof(r), 30000);
  REQUIRE_TRUE(ran);
  REQUIRE_TRUE(r.all_ok);
  REQUIRE_EQ(r.parses, 1L);
  REQUIRE_TRUE(r.verified);
}

// An ALPN cleanup that sets the bool at udata, so a test sees it ran.
static void _alpn_cleanup_marker(void *udata) { *(bool *)udata = true; }

TEST(ctls_ctx, alpn_add_invalid_args) {
  REQUIRE_EQ(ctls_ctx_alpn_add(NULL, "http/1.1", NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctls_ctx_alpn_add(ctx, "", NULL, NULL, NULL, NULL),
             ccol_invalid_args);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, alpn_add_and_count_and_cleanup_fires_on_release) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  bool cleaned_up = false;
  ccol_retval rv = ctls_ctx_alpn_add(ctx, "http/1.1", NULL, &cleaned_up,
                                     _alpn_cleanup_marker, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(ctls_ctx_alpn_count(ctx), (size_t)1);
  ctls_ctx_release(ctx);
  REQUIRE_TRUE(cleaned_up);
}

// The state the reentry cleanup shares with its helper thread: The context,
// whether the helper finished, whether the lock was free, whether the cleanup
// ran, and the helper thread.
typedef struct {
  ctls_ctx *ctx;
  atomic_bool helper_done;
  bool lock_was_free;
  bool cleaned_up;
  pthread_t helper;
  bool helper_started;
} alpn_reentry_probe;

// Retains and releases p->ctx, which takes the context lock, then sets
// helper_done. It proves the lock is free while a cleanup runs.
static void *_alpn_reentry_helper(void *arg) {
  alpn_reentry_probe *p = (alpn_reentry_probe *)arg;
  ctls_ctx_retain(p->ctx);
  ctls_ctx_release(p->ctx);
  atomic_store(&p->helper_done, true);
  return NULL;
}

// The ALPN cleanup of alpn_replacement_runs_the_old_cleanup_with_no_lock_held:
// starts _alpn_reentry_helper() and waits up to 10 s for it. If the replacing
// call held the context lock, the helper can't finish in time and
// lock_was_free stays false; the helper finishes once the lock goes, so
// nothing hangs.
static void _alpn_reentry_cleanup(void *udata) {
  alpn_reentry_probe *p = (alpn_reentry_probe *)udata;
  p->cleaned_up = true;
  p->helper_started =
      pthread_create(&p->helper, NULL, _alpn_reentry_helper, p) == 0;
  if (!p->helper_started) return;
  for (int i = 0; i < 10000 && !atomic_load(&p->helper_done); ++i) {
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  p->lock_was_free = atomic_load(&p->helper_done);
}

TEST(ctls_ctx, alpn_replacement_runs_the_old_cleanup_with_no_lock_held) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  alpn_reentry_probe probe = {.ctx = ctx};
  atomic_init(&probe.helper_done, false);
  ccol_retval first = ctls_ctx_alpn_add(ctx, "http/1.1", NULL, &probe,
                                        _alpn_reentry_cleanup, NULL);
  ccol_retval second =
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
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ctls_ctx_retain(ctx);
  ctls_ctx_release(ctx);
  ccol_retval rv =
      ctls_ctx_cert_add(ctx, "still-alive.test", NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  ctls_ctx_release(ctx);
}

TEST(ctls_ctx, release_null_is_noop) { ctls_ctx_release(NULL); }

TEST(ctls_handshake, self_signed_no_verify_completes) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  char *err = NULL;
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, &err);
  REQUIRE_NE((void *)server_conn, (void *)NULL);
  ctls_conn *client_conn =
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

// The header of a poisoning allocation: The size, padded to max_align_t.
typedef struct {
  size_t size;
  max_align_t align;
} poison_hdr;

// Handshake and I/O behavior
//
// An allocator that keeps the size in a header and fills a block with 0xA5
// when it frees it, so a read through freed memory sees garbage in an ordinary
// build too.
static void *_poison_malloc(size_t n) {
  poison_hdr *h = (poison_hdr *)malloc(sizeof(poison_hdr) + n);
  if (!h) return NULL;
  h->size = n;
  return h + 1;
}
static void *_poison_calloc(size_t n, size_t sz) {
  if (sz && n > SIZE_MAX / sz) return NULL;
  void *p = _poison_malloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}
static void _poison_free(void *p) {
  if (!p) return;
  poison_hdr *h = (poison_hdr *)p - 1;
  memset(h, 0xA5, sizeof(poison_hdr) + h->size);
  free(h);
}
static void *_poison_realloc(void *p, size_t n) {
  if (!p) return _poison_malloc(n);
  void *q = _poison_malloc(n);
  if (!q) return NULL;
  size_t old = ((poison_hdr *)p - 1)->size;
  memcpy(q, p, old < n ? old : n);
  _poison_free(p);
  return q;
}

// A connection that holds the last reference to its context frees itself with
// the context's allocator after the release freed the context and its procs
// copy. With the poisoning allocator a read of that copy after the release
// calls through 0xA5A5... and the process dies.
TEST(ctls_conn, a_connection_that_outlives_its_context_frees_itself_cleanly) {
  ccol_memmgmt_procs procs = {.malloc = _poison_malloc,
                              .free = _poison_free,
                              .calloc = _poison_calloc,
                              .realloc = _poison_realloc};
  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  REQUIRE_TRUE(fds[0] >= 0 && fds[1] >= 0);
  ctls_ctx *ctx = ctls_ctx_new_mp(&procs, NULL);
  bool have_ctx = ctx != NULL;
  if (!have_ctx) {
    close(fds[0]);
    close(fds[1]);
  }
  REQUIRE_TRUE(have_ctx);
  ctls_conn *conn =
      ctls_conn_create_client(ctx, fds[1], "srv.test", false, NULL);
  ctls_ctx_release(ctx);
  bool created = conn != NULL;
  ctls_conn_destroy(conn);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_TRUE(created);
}

TEST(ctls_handshake, read_write_roundtrip_after_handshake) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust(client_ctx, g_server_cert, NULL), ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "127.0.0.1", true, NULL);
  REQUIRE_NE((void *)client_conn, (void *)NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));
  REQUIRE_EQ(ctls_conn_verify_result(client_conn), 0L);

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_handshake, hostname_verify_failure_wrong_host) {
  if (!g_certs_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_trust(client_ctx, g_server_cert, NULL), ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(client_ctx, NULL, g_client_cert, g_client_key,
                               NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *server_ssl = ctls__conn_ssl_for_tests(server_conn);
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

// With SSL_VERIFY_PEER alone the server would accept a client without a
// certificate, so this catches a missing SSL_VERIFY_FAIL_IF_NO_PEER_CERT.
TEST(ctls_handshake, mtls_client_presents_no_cert_fails_by_default) {
  if (!g_certs_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// peer_cert_optional works before and after ctls_ctx_trust(). The verified
// check matters: SSL_get_verify_result() alone says X509_V_OK for a peer that
// presented nothing.
TEST(ctls_handshake, mtls_optional_accepts_a_client_without_a_cert) {
  if (!g_certs_ready) return;
  for (int before = 0; before < 2; before++) {
    ctls_ctx *server_ctx = ctls_ctx_new(NULL);
    REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                                 NULL, NULL),
               ccol_success);
    if (before)
      REQUIRE_EQ(ctls_ctx_peer_cert_optional(server_ctx, true), ccol_success);
    REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_client_cert, NULL), ccol_success);
    if (!before)
      REQUIRE_EQ(ctls_ctx_peer_cert_optional(server_ctx, true), ccol_success);

    ctls_ctx *client_ctx = ctls_ctx_new(NULL);

    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn *server_conn =
        ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
    ctls_conn *client_conn =
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
  if (!g_certs_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// Without the early return SSL_write() of 0 bytes gives 0, which the failure
// classification turns into -1 with ECONNRESET.
TEST(ctls_io, zero_length_write_returns_zero) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// SSL_shutdown() on an unfinished handshake pushes "shutdown while in init",
// which a later unrelated OpenSSL call on the thread would read as its own.
TEST(ctls_io, destroy_before_the_handshake_leaves_no_openssl_error) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                               NULL, NULL),
             ccol_success);
  REQUIRE_EQ(ctls_ctx_trust(server_ctx, g_server_cert, NULL), ccol_success);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(client_ctx, NULL, g_client_cert, g_client_key,
                               NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// SNI dispatch
TEST(ctls_sni, no_sni_uses_default_cert) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "alpha.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "default.test", NULL, NULL, NULL, NULL),
      ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "*.wild.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "foo.wild.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
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

// The name is registered in mixed case and requested in another case; the
// served CN is the lowercased name, and "ctls-default" would mean the named
// entry was missed.
TEST(ctls_sni, mixed_case_name_matches_case_insensitively) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "Alpha.Test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "ALPHA.TEST", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
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

// Rotating a named certificate updates a key the map already holds, which
// gives ccol_key_already_present. ctls_ctx_cert_add must read that as success;
// destroying the new entry would leave the map slot pointing at freed memory.
// The second certificate's CN must be served.
TEST(ctls_sni, cert_add_replaces_existing_named_cert_without_dangling_pointer) {
  if (!g_certs_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "rotate.test", g_server_cert,
                               g_server_key, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "rotate.test", g_client_cert,
                               g_client_key, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "rotate.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
  char cn_buf[128];
  const char *cn = _peer_cert_cn(client_ssl, cn_buf, sizeof(cn_buf));
  REQUIRE_NE((void *)cn, (void *)NULL);
  REQUIRE_STREQ(cn, "test-client");

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(ctls_sni, unmatched_name_falls_back_to_default) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_cert_add(server_ctx, "alpha.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn = ctls_conn_create_client(
      client_ctx, fds[1], "totally-unrelated.test", false, NULL);
  REQUIRE_TRUE(_drive_both(client_conn, server_conn, 200, NULL, NULL));

  SSL *client_ssl = ctls__conn_ssl_for_tests(client_conn);
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

// What _alpn_capture_cb() saw: Whether it fired and the protocol name.
typedef struct alpn_capture {
  bool fired;
  char name[64];
  size_t len;
} alpn_capture;

// ALPN negotiation
//
// An on_selected callback that records in the alpn_capture at udata that it
// fired and which protocol, so a test can check the agreed name.
static void _alpn_capture_cb(ctls_conn *conn, const char *name, size_t len,
                             void *udata) {
  (void)conn;
  alpn_capture *cap = (alpn_capture *)udata;
  cap->fired = true;
  cap->len = len < sizeof(cap->name) - 1 ? len : sizeof(cap->name) - 1;
  memcpy(cap->name, name, cap->len);
  cap->name[cap->len] = 0;
}

TEST(ctls_alpn, matching_protocol_is_selected_both_sides) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  alpn_capture server_cap = {0}, client_cap = {0};
  REQUIRE_EQ(ctls_ctx_alpn_add(server_ctx, "http/1.1", _alpn_capture_cb,
                               &server_cap, NULL, NULL),
             ccol_success);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(client_ctx, "http/1.1", _alpn_capture_cb,
                               &client_cap, NULL, NULL),
             ccol_success);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// OpenSSL ex_data slot ownership
//
// A pointer a test puts in the OpenSSL app-data slot; only its address
// matters.
static int _app_data_sentinel = 0;

// If every lookup took the index mutex, each create would add one locked
// call to the count.
TEST(ctls_ex_data, a_registered_index_is_read_without_the_lock) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn_destroy(ctls_conn_create_server(ctx, fds[0], NULL, NULL));
  long before = ctls__ex_idx_locked_calls_for_tests();
  bool all_created = true;
  for (int i = 0; i < 16; ++i) {
    ctls_conn *conn = ctls_conn_create_server(ctx, fds[0], NULL, NULL);
    if (!conn) all_created = false;
    ctls_conn_destroy(conn);
  }
  long after = ctls__ex_idx_locked_calls_for_tests();
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(ctx);
  REQUIRE_TRUE(all_created);
  REQUIRE_EQ(after, before);
}

// OpenSSL keeps ex_data index 0 for SSL_set_app_data(), which other code may
// use on an SSL it holds; if ctls used it, the two would overwrite each other.
// The checks use OpenSSL's own macros so they can't drift with the code.
TEST(ctls_ex_data, connections_leave_the_openssl_app_data_slot_alone) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);
  alpn_capture server_cap = {0};
  ccol_retval alpn_rv = ctls_ctx_alpn_add(
      server_ctx, "http/1.1", _alpn_capture_cb, &server_cap, NULL, NULL);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  ccol_retval client_alpn_rv =
      ctls_ctx_alpn_add(client_ctx, "http/1.1", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);

  void *server_app_at_create = NULL;
  void *client_app_at_create = NULL;
  if (server_conn) {
    server_app_at_create =
        SSL_get_app_data(ctls__conn_ssl_for_tests(server_conn));
  }
  if (client_conn) {
    client_app_at_create =
        SSL_get_app_data(ctls__conn_ssl_for_tests(client_conn));
  }

  bool conns_created = (server_conn != NULL && client_conn != NULL);
  bool slot_free = (conns_created && server_app_at_create == NULL &&
                    client_app_at_create == NULL);
  if (slot_free) {
    SSL_set_app_data(ctls__conn_ssl_for_tests(server_conn),
                     &_app_data_sentinel);
    SSL_set_app_data(ctls__conn_ssl_for_tests(client_conn),
                     &_app_data_sentinel);
  }

  bool handshake_ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  void *server_app_after = NULL;
  void *client_app_after = NULL;
  if (slot_free) {
    server_app_after = SSL_get_app_data(ctls__conn_ssl_for_tests(server_conn));
    client_app_after = SSL_get_app_data(ctls__conn_ssl_for_tests(client_conn));
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

  REQUIRE_TRUE(conns_created);
  REQUIRE_EQ(server_app_at_create, (void *)NULL);
  REQUIRE_EQ(client_app_at_create, (void *)NULL);
  REQUIRE_TRUE(slot_free);

  REQUIRE_TRUE(handshake_ok);
  REQUIRE_TRUE(alpn_fired);
  REQUIRE_STREQ(server_cap.name, "http/1.1");
  REQUIRE_EQ(server_app_after, (void *)&_app_data_sentinel);
  REQUIRE_EQ(client_app_after, (void *)&_app_data_sentinel);
}

// What _alpn_destroy_own_conn_cb() saw: Whether it fired and whether the
// handshake had finished by then.
typedef struct {
  bool fired;
  bool handshake_finished;
} alpn_self_destroy;

// A server on_selected callback that records in the alpn_self_destroy at udata
// that it fired and whether the handshake had finished, then destroys its own
// connection.
static void _alpn_destroy_own_conn_cb(ctls_conn *conn, const char *name,
                                      size_t len, void *udata) {
  (void)name;
  (void)len;
  alpn_self_destroy *seen = (alpn_self_destroy *)udata;
  seen->fired = true;
  seen->handshake_finished =
      SSL_is_init_finished(ctls__conn_ssl_for_tests(conn)) == 1;
  ctls_conn_destroy(conn);
}

// The callback fires after the handshake step, so destroying the connection is
// safe. Fired from inside OpenSSL's ALPN selection, it would see an unfinished
// handshake and OpenSSL would read the selected name from freed memory.
TEST(ctls_alpn, a_server_callback_may_destroy_its_own_connection) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
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
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool client_done = false;
  bool server_done = false;
  for (int i = 0; i < 200 && configured && !(client_done && server_done); ++i) {
    if (!client_done &&
        ctls_conn_handshake_step(client_conn) == CTLS_HANDSHAKE_DONE)
      client_done = true;
    if (!server_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(server_conn);
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

// The server holds none of the offered protocols, so nothing is agreed;
// reporting a registered protocol would claim an agreement the peer never
// made. The handshake itself still succeeds.
TEST(ctls_alpn, no_overlap_negotiates_no_protocol_at_all) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);
  alpn_capture server_cap = {0};
  ccol_retval server_alpn_rv = ctls_ctx_alpn_add(
      server_ctx, "spdy/1", _alpn_capture_cb, &server_cap, NULL, NULL);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  ccol_retval client_alpn_rv =
      ctls_ctx_alpn_add(client_ctx, "http/1.1", NULL, NULL, NULL, NULL);

  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool handshake_ok = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  bool fired = server_cap.fired;
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
  REQUIRE_TRUE(handshake_ok);
  REQUIRE_FALSE(fired);
  REQUIRE_EQ((void *)sel, (void *)NULL);
  REQUIRE_EQ(len, (size_t)0);
}

// The client side: A server with no ALPN must leave the client with nothing
// agreed, or it would speak HTTP/2 framing to an HTTP/1.1 peer.
TEST(ctls_alpn,
     a_server_that_negotiates_none_leaves_the_client_reporting_none) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval cert_rv =
      ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL);

  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  alpn_capture client_cap = {0};
  ccol_retval h2_rv = ctls_ctx_alpn_add(client_ctx, "h2", _alpn_capture_cb,
                                        &client_cap, NULL, NULL);
  ccol_retval h1_rv = ctls_ctx_alpn_add(
      client_ctx, "http/1.1", _alpn_capture_cb, &client_cap, NULL, NULL);

  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// Error paths
TEST(ctls_conn, create_client_null_ctx_fails) {
  char *err = NULL;
  ctls_conn *conn = ctls_conn_create_client(NULL, 3, "x", false, &err);
  REQUIRE_EQ((void *)conn, (void *)NULL);
}

TEST(ctls_conn, create_client_negative_fd_fails) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ctls_conn *conn = ctls_conn_create_client(ctx, -1, "x", false, NULL);
  REQUIRE_EQ((void *)conn, (void *)NULL);
  ctls_ctx_release(ctx);
}

TEST(ctls_conn, create_server_null_ctx_fails) {
  ctls_conn *conn = ctls_conn_create_server(NULL, 3, NULL, NULL);
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
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  int fds[2];
  _make_nonblocking_pair(fds);
  int marker = 42;
  ctls_conn *server_conn = ctls_conn_create_server(ctx, fds[0], &marker, NULL);
  REQUIRE_EQ(ctls_conn_udata(server_conn), (void *)&marker);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(ctx);
}

// A server that parks a slow reader retries a blocked write from its own copy
// of the bytes. Without SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER OpenSSL refuses
// that retry with "bad write retry".
TEST(ctls_conn, a_write_retry_may_come_from_another_buffer) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// Self-signed name allocation, SNI and ALPN races
//
// The malloc call that _fail_nth_malloc() fails (0 for none) and the running
// count of its calls.
static _Atomic int g_fail_malloc_at_call = 0;
static _Atomic int g_malloc_call_count = 0;

// An allocator whose malloc fails call number g_fail_malloc_at_call, so a test
// can fail one exact allocation of ctls_ctx_cert_add; calloc, realloc and free
// pass through.
static void *_fail_nth_malloc(size_t n) {
  int call = atomic_fetch_add(&g_malloc_call_count, 1) + 1;
  int fail_at = atomic_load(&g_fail_malloc_at_call);
  if (fail_at && call == fail_at) return NULL;
  return malloc(n);
}
static void _fail_nth_free(void *p) { free(p); }
static void *_fail_nth_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void *_fail_nth_realloc(void *p, size_t sz) { return realloc(p, sz); }

// For a named self-signed certificate the second malloc copies the subject
// name. Without its NULL check the rebuild calls strlen(NULL) inside
// ctls_create_self_signed(). The context must stay usable afterwards.
TEST(ctls_ctx, cert_add_self_signed_name_alloc_failure_reports_oom_not_crash) {
  ccol_memmgmt_procs mp = {_fail_nth_malloc, _fail_nth_free, _fail_nth_calloc,
                           _fail_nth_realloc};
  ctls_ctx *ctx = ctls_ctx_new_mp(&mp, NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  atomic_store(&g_malloc_call_count, 0);
  atomic_store(&g_fail_malloc_at_call, 2);
  char *err = NULL;
  ccol_retval rv = ctls_ctx_cert_add(ctx, "oom.test", NULL, NULL, NULL, &err);
  atomic_store(&g_fail_malloc_at_call, 0);
  REQUIRE_EQ((int)rv, (int)ccol_not_enough_memory);

  REQUIRE_EQ(ctls_ctx_cert_add(ctx, "after-oom.test", NULL, NULL, NULL, NULL),
             ccol_success);

  ctls_ctx_release(ctx);
}

// The stop flag of the SNI rotation thread, and the context it rotates.
static _Atomic bool g_sni_race_stop = false;
static ctls_ctx *g_sni_race_ctx = NULL;

// Re-adds the named certificate "race.test" of g_sni_race_ctx about 1000
// times a second until g_sni_race_stop is set. Each add frees the previous
// named SSL_CTX, the one a concurrent servername callback must never use after
// its lookup. The usleep() throttle matters: An unthrottled thread on its own
// core keeps the context lock so much that the handshakes starve, and
// sched_yield() doesn't help across cores.
static void *_sni_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_sni_race_stop)) {
    ctls_ctx_cert_add(g_sni_race_ctx, "race.test", NULL, NULL, NULL, NULL);
    usleep(1000);
  }
  return NULL;
}

// Full handshakes race a thread that rotates the named certificate. A plain
// run proves little; under ASan, valgrind or TSan a use-after-free in the
// servername callback aborts or is reported. It also checks that no chashmap
// iterator is outstanding once the rotation has stopped.
TEST(ctls_sni, cert_add_race_during_live_handshake_does_not_crash) {
  if (!g_certs_ready) return;
  g_sni_race_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(
      ctls_ctx_cert_add(g_sni_race_ctx, "race.test", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  atomic_store(&g_sni_race_stop, false);
  pthread_t rotate_th;
  REQUIRE_EQ(pthread_create(&rotate_th, NULL, _sni_race_rotate_thread, NULL),
             0);

  for (int i = 0; i < 80; i++) {
    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn *server_conn =
        ctls_conn_create_server(g_sni_race_ctx, fds[0], NULL, NULL);
    ctls_conn *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], "race.test", false, NULL);
    _drive_both(client_conn, server_conn, 200, NULL, NULL);
    ctls_conn_destroy(client_conn);
    ctls_conn_destroy(server_conn);
    close(fds[0]);
    close(fds[1]);
  }

  atomic_store(&g_sni_race_stop, true);
  pthread_join(rotate_th, NULL);
  REQUIRE_EQ(chashmap_iter_outstanding_count_for_tests(), (long)0);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(g_sni_race_ctx);
  g_sni_race_ctx = NULL;
}

// The stop flag of the ALPN rotation thread, and the server context it
// rotates.
static _Atomic bool g_alpn_race_stop = false;
static ctls_ctx *g_alpn_race_server_ctx = NULL;

// Re-registers "h2" on g_alpn_race_server_ctx until g_alpn_race_stop is set.
// Each replace frees the previous entry's name buffer, which a concurrent
// server ALPN callback must never read; throttled as
// _sni_race_rotate_thread().
static void *_alpn_race_rotate_thread(void *arg) {
  (void)arg;
  while (!atomic_load(&g_alpn_race_stop)) {
    ctls_ctx_alpn_add(g_alpn_race_server_ctx, "h2", NULL, NULL, NULL, NULL);
    usleep(1000);
  }
  return NULL;
}

// Handshakes race a thread that replaces the ALPN registration, and a
// selected protocol name must stay readable after the handshake through the
// connection's own copy.
TEST(ctls_alpn, alpn_add_race_during_live_handshake_does_not_crash) {
  if (!g_certs_ready) return;
  g_alpn_race_server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(g_alpn_race_server_ctx, NULL, g_server_cert,
                               g_server_key, NULL, NULL),
             ccol_success);
  REQUIRE_EQ(
      ctls_ctx_alpn_add(g_alpn_race_server_ctx, "h2", NULL, NULL, NULL, NULL),
      ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_alpn_add(client_ctx, "h2", NULL, NULL, NULL, NULL),
             ccol_success);

  atomic_store(&g_alpn_race_stop, false);
  pthread_t rotate_th;
  REQUIRE_EQ(pthread_create(&rotate_th, NULL, _alpn_race_rotate_thread, NULL),
             0);

  bool alpn_len_mismatch = false;
  for (int i = 0; i < 80; i++) {
    int fds[2];
    _make_nonblocking_pair(fds);
    ctls_conn *server_conn =
        ctls_conn_create_server(g_alpn_race_server_ctx, fds[0], NULL, NULL);
    ctls_conn *client_conn =
        ctls_conn_create_client(client_ctx, fds[1], NULL, false, NULL);
    if (_drive_both(client_conn, server_conn, 200, NULL, NULL)) {
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

// Process-wide SSL_CTX census
//
// An SSL_CTX has no public reference count getter, so the suite counts
// creations and real destructions through an ex_data index, whose callbacks
// fire once per SSL_CTX_new and once per destruction at refcount zero, for
// every SSL_CTX of the process. A leaked reference shows as a creation with no
// destruction. The callbacks run on any thread, so the counters are atomic.
static _Atomic long g_ssl_ctx_created = 0;
static _Atomic long g_ssl_ctx_destroyed = 0;

// The return type and the return statement of the ex_data new callback:
// void in OpenSSL, int (1 for success) in LibreSSL.
#if defined(LIBRESSL_VERSION_NUMBER)
#define CENSUS_NEW_RET int
#define CENSUS_NEW_DONE return 1
#else
#define CENSUS_NEW_RET void
#define CENSUS_NEW_DONE return
#endif

// The ex_data new callback of the census: Counts one SSL_CTX creation in
// g_ssl_ctx_created.
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

// The ex_data free callback of the census: Counts one SSL_CTX destruction in
// g_ssl_ctx_destroyed.
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

// Registers the census callbacks before anything can build an SSL_CTX.
static void __attribute__((constructor)) _register_ssl_ctx_census(void) {
  SSL_CTX_get_ex_new_index(0, NULL, _ssl_ctx_census_new, NULL,
                           _ssl_ctx_census_free);
}

// Gives the SSL_CTX objects created and not yet destroyed, so a test can
// check that its contexts all went away.
static long _ssl_ctx_outstanding(void) {
  long created = atomic_load_explicit(&g_ssl_ctx_created, memory_order_relaxed);
  long destroyed =
      atomic_load_explicit(&g_ssl_ctx_destroyed, memory_order_relaxed);
  return created - destroyed;
}

// SNI dispatch pins the context the connection serves from
//
// A server connection whose SNI name matches pins its creation-time context
// and the named one, because a concurrent rebuild frees both; a client
// connection pins only its creation-time context.
TEST(ctls_sni, dispatch_pins_the_named_context_until_the_connection_is_gone) {
  if (!g_certs_ready) return;
  long pins_before = ctls__live_conn_ctx_pin_count_for_tests();

  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  ccol_retval default_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                             g_server_key, NULL, NULL);
  ccol_retval named_rv =
      ctls_ctx_cert_add(server_ctx, "pinned.test", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "pinned.test", false, NULL);
  bool shook = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  SSL_CTX *server_pinned = ctls__conn_pinned_sni_ctx_for_tests(server_conn);
  SSL_CTX *server_active =
      SSL_get_SSL_CTX(ctls__conn_ssl_for_tests(server_conn));
  SSL_CTX *client_pinned = ctls__conn_pinned_sni_ctx_for_tests(client_conn);
  long pins_during = ctls__live_conn_ctx_pin_count_for_tests();

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  long pins_after = ctls__live_conn_ctx_pin_count_for_tests();
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
  long pins_before = ctls__live_conn_ctx_pin_count_for_tests();

  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  ccol_retval default_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                             g_server_key, NULL, NULL);
  ccol_retval named_rv =
      ctls_ctx_cert_add(server_ctx, "registered.test", NULL, NULL, NULL, NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn = ctls_conn_create_client(
      client_ctx, fds[1], "unregistered.test", false, NULL);
  bool shook = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  SSL_CTX *server_pinned = ctls__conn_pinned_sni_ctx_for_tests(server_conn);
  long pins_during = ctls__live_conn_ctx_pin_count_for_tests();

  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  long pins_after = ctls__live_conn_ctx_pin_count_for_tests();
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(default_rv, ccol_success);
  REQUIRE_EQ(named_rv, ccol_success);
  REQUIRE_TRUE(shook);
  REQUIRE_EQ((void *)server_pinned, (void *)NULL);
  REQUIRE_EQ(pins_during - pins_before, 2L);
  REQUIRE_EQ(pins_after, pins_before);
}

// Concurrent reconfiguration under live traffic
//
// Acceptor and client threads run real TLS handshakes over loopback with one
// server and one client context, while a rotator keeps re-adding named
// certificates, the default certificate and an ALPN protocol, each of which
// rebuilds and frees every SSL_CTX. The client names cycle through two exact
// matches, a wildcard match and a name nothing answers. The listener binds
// port 0, so runs never collide.
//
// The thread counts and the rounds each client makes.
#define CTLS_STRESS_ACCEPTORS 4
#define CTLS_STRESS_CLIENTS 6
#define CTLS_STRESS_ROUNDS 6
// A safety net against a hang, generous enough for ten threads under
// valgrind; no assertion depends on how long a handshake takes.
#define CTLS_STRESS_HANDSHAKE_DEADLINE_MS 60000

// The SNI names the clients cycle through.
static const char *const g_stress_names[] = {
    "one.stress.test", "two.stress.test", "leaf.wild.stress.test",
    "nothing.stress.test"};

// The state the stress threads share: The stop flag, the start gate and its
// arrival count, the handshake and accept counters, the listening socket and
// its address, and the two contexts.
static _Atomic bool g_stress_stop = false;
static _Atomic bool g_stress_go = false;
static _Atomic int g_stress_arrived = 0;
static _Atomic long g_stress_server_ok = 0;
static _Atomic long g_stress_client_ok = 0;
static _Atomic long g_stress_accepted = 0;
static int g_stress_listen_fd = -1;
static struct sockaddr_in g_stress_addr;
static ctls_ctx *g_stress_server_ctx = NULL;
static ctls_ctx *g_stress_client_ctx = NULL;

// Gives the milliseconds since start on CLOCK_MONOTONIC, for the handshake
// deadline.
static long _elapsed_ms_since(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - start->tv_sec) * 1000L +
         (now.tv_nsec - start->tv_nsec) / 1000000L;
}

// Puts fd in non-blocking mode, ignoring a failure; the stress threads use it
// on every socket.
static void _set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Counts the calling thread into g_stress_arrived and waits until g_stress_go
// opens. It's a counted gate instead of a pthread_barrier_t, whose count is
// fixed at creation, so a partial start can't park threads forever.
static void _stress_wait_for_go(void) {
  atomic_fetch_add(&g_stress_arrived, 1);
  while (!atomic_load(&g_stress_go)) {
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
}

// Drives the handshake of conn on fd to the end, polling for whichever
// direction each step asks for, so a stress thread can handshake alone. Gives
// false on an error or after CTLS_STRESS_HANDSHAKE_DEADLINE_MS.
static bool _drive_one_side(ctls_conn *conn, int fd) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    ctls_handshake_result r = ctls_conn_handshake_step(conn);
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

// Accepts connections on g_stress_listen_fd and handshakes each with
// g_stress_server_ctx until g_stress_stop, counting accepts and successful
// handshakes. It polls with a timeout, since only Linux bounds accept(2) with
// SO_RCVTIMEO, and the non-blocking listener makes a lost race give EAGAIN.
static void *_stress_acceptor_thread(void *arg) {
  (void)arg;
  _stress_wait_for_go();
  while (!atomic_load(&g_stress_stop)) {
    struct pollfd pfd = {.fd = g_stress_listen_fd, .events = POLLIN};
    if (poll(&pfd, 1, 100) <= 0) continue;
    int fd = accept(g_stress_listen_fd, NULL, NULL);
    if (fd < 0) continue;
    atomic_fetch_add(&g_stress_accepted, 1);
    _set_nonblocking(fd);
    ctls_conn *conn =
        ctls_conn_create_server(g_stress_server_ctx, fd, NULL, NULL);
    if (conn) {
      if (_drive_one_side(conn, fd)) atomic_fetch_add(&g_stress_server_ok, 1);
      ctls_conn_destroy(conn);
    }
    close(fd);
  }
  return NULL;
}

// Connects CTLS_STRESS_ROUNDS times to g_stress_addr and handshakes with
// g_stress_client_ctx, picking the SNI name from g_stress_names by its seed in
// arg, and counts successful handshakes.
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
    ctls_conn *conn =
        ctls_conn_create_client(g_stress_client_ctx, fd, sni, false, NULL);
    if (conn) {
      if (_drive_one_side(conn, fd)) atomic_fetch_add(&g_stress_client_ok, 1);
      ctls_conn_destroy(conn);
    }
    close(fd);
  }
  return NULL;
}

// Cycles through re-adding a named file certificate, a wildcard certificate,
// the default certificate and an ALPN protocol on g_stress_server_ctx until
// g_stress_stop, throttled so the handshake threads get the lock.
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
    usleep(500);
  }
  return NULL;
}

// Every accepted connection must complete (a healthy majority is asserted,
// since a refused connect says something about the machine), every
// connection reference is released, and every SSL_CTX the rebuilds made is
// destroyed once the contexts go.
TEST(ctls_stress, concurrent_reconfiguration_under_live_handshakes) {
  if (!g_certs_ready) return;
  long pins_before = ctls__live_conn_ctx_pin_count_for_tests();
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
  bind_addr.sin_port = 0;
  bool bound = bind(g_stress_listen_fd, (struct sockaddr *)&bind_addr,
                    sizeof(bind_addr)) == 0 &&
               listen(g_stress_listen_fd, 64) == 0;
  socklen_t alen = sizeof(g_stress_addr);
  memset(&g_stress_addr, 0, sizeof(g_stress_addr));
  if (bound)
    bound = getsockname(g_stress_listen_fd, (struct sockaddr *)&g_stress_addr,
                        &alen) == 0;
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
  long pins_after = ctls__live_conn_ctx_pin_count_for_tests();
  long ssl_ctx_after = _ssl_ctx_outstanding();

  REQUIRE_TRUE(configured);
  REQUIRE_EQ(started, CTLS_STRESS_ACCEPTORS + CTLS_STRESS_CLIENTS + 1);
  REQUIRE_GT(accepted, (long)0);
  REQUIRE_GT(client_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  REQUIRE_GT(server_ok, (long)(CTLS_STRESS_CLIENTS * CTLS_STRESS_ROUNDS / 2));
  REQUIRE_EQ(pins_after, pins_before);
  REQUIRE_EQ(ssl_ctx_after, ssl_ctx_before);
}

// Allocation-failure sweep
//
// The entry points below have cleanup branches that only a failed allocation
// reaches. The counter is shared by all four procs, since the context, the
// named records and the connection come from calloc and the trust and ALPN
// arrays grow through realloc.
//
// The allocations counted since arming, and the one that fails (0 disarms).
static _Atomic int g_alloc_seen = 0;
static _Atomic int g_alloc_fail_at = 0;

// Counts one allocation of g_sweep_mp and tells whether it's the armed one.
static bool _sweep_should_fail(void) {
  int at = atomic_load(&g_alloc_fail_at);
  if (at == 0) return false;
  return (atomic_fetch_add(&g_alloc_seen, 1) + 1) == at;
}
// The g_sweep_mp functions: malloc, calloc and realloc fail when
// _sweep_should_fail() says so; free passes through.
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
// The procs the sweep tests hand to ctls.
static ccol_memmgmt_procs g_sweep_mp = {_sweep_malloc, _sweep_free,
                                        _sweep_calloc, _sweep_realloc};

// Arms g_sweep_mp to fail allocation number nth and resets the count.
static void _sweep_arm(int nth) {
  atomic_store(&g_alloc_seen, 0);
  atomic_store(&g_alloc_fail_at, nth);
}
// Disarms g_sweep_mp, so every allocation succeeds.
static void _sweep_disarm(void) { atomic_store(&g_alloc_fail_at, 0); }

// How many allocation indexes the sweep tries: Past the last allocation any
// of these calls makes, so every branch is covered.
#define CTLS_SWEEP_DEPTH 46

TEST(ctls_oom, ctx_new_reports_failure_at_every_allocation_step) {
  bool all_handled = true;
  for (int n = 1; n <= 10; n++) {
    _sweep_arm(n);
    char *err = NULL;
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, &err);
    _sweep_disarm();
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;

    _sweep_arm(n);
    ccol_retval rv =
        ctls_ctx_cert_add(ctx, "sweep.test", NULL, NULL, NULL, NULL);
    _sweep_disarm();

    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      all_handled = false;
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval rv = ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval rv = ctls_ctx_cert_add(ctx, "sweep.test", g_server_cert,
                                       g_server_key, "unused-password", NULL);
    _sweep_disarm();
    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      all_handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(all_handled);
}

// The second add grows the parallel PEM and length arrays; a reallocation
// that covers only one of them leaves the other pointing at freed storage.
TEST(ctls_oom, trust_array_growth_reports_failure_without_dangling) {
  if (!g_certs_ready) return;
  bool all_handled = true;
  for (int n = 1; n <= 20; n++) {
    _sweep_disarm();
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval first = ctls_ctx_trust(ctx, g_server_cert, NULL);
    _sweep_disarm();
    _sweep_arm(n);
    ccol_retval second = ctls_ctx_trust(ctx, g_server_cert, NULL);
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval a = ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL);
    _sweep_disarm();
    _sweep_arm(n);
    ccol_retval b = ctls_ctx_alpn_add(ctx, "http/1.1", NULL, NULL, NULL, NULL);
    _sweep_disarm();

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

// A failed change of the configuration changes nothing
//
// Each test fails one allocation at a time, first every OpenSSL allocation and
// then every allocation of this module, while one configuration call runs,
// and stops at the first index the call never reaches. After a failed call
// every name must serve the certificate and verify mode it served before,
// also after a later unrelated rebuild (an ALPN registration that replaces
// itself), which proves the stored configuration matches the serving
// contexts. The fixture uses file certificates only, because OpenSSL 3.5
// frees a pointer it never allocated when an allocation inside X509_sign
// fails.
//
// Copies into buf the CN of the certificate that serves a ClientHello naming
// name and gives buf, "" when that context holds no certificate.
static const char *_served_cn(ctls_ctx *ctx, const char *name, char *buf,
                              size_t buflen) {
  buf[0] = '\0';
  SSL_CTX *served = ctls__ctx_served_ctx_for_tests(ctx, name);
  X509 *cert = served ? SSL_CTX_get0_certificate(served) : NULL;
  if (cert)
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf,
                              (int)buflen);
  return buf;
}

// Gives the verify mode of ctx's default context, or -1 when none is built.
static int _served_verify_mode(ctls_ctx *ctx) {
  SSL_CTX *served = ctls__ctx_served_ctx_for_tests(ctx, NULL);
  return served ? SSL_CTX_get_verify_mode(served) : -1;
}

// The configuration calls the rollback sweeps run.
typedef enum {
  CFG_OP_ROTATE_NAME,
  CFG_OP_ADD_NAME,
  CFG_OP_REPLACE_DEFAULT,
  CFG_OP_TRUST,
  CFG_OP_TRUST_SYSTEM,
  CFG_OP_COUNT
} cfg_op_t;

// The names whose served certificate the rollback tests check, and the CN
// each of them serves before the call under test.
static const char *const g_cfg_names[] = {NULL, "keep.test", "rot.test",
                                          "new.test"};
static const char *const g_cfg_before[] = {"test-client", "named.test",
                                           "named.test", "test-client"};

// Builds the rollback fixture on allocator mp: A default file certificate,
// two named file certificates and an ALPN protocol. Gives NULL when any step
// fails.
static ctls_ctx *_cfg_fixture(ccol_memmgmt_procs *mp) {
  ctls_ctx *ctx = ctls_ctx_new_mp(mp, NULL);
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

// Runs configuration call op on ctx and gives its result; the call's
// arguments are the ones _cfg_applied() checks for.
static ccol_retval _cfg_run_op(ctls_ctx *ctx, cfg_op_t op) {
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

// Tells whether ctx serves exactly what the fixture configured: The
// g_cfg_before CNs for g_cfg_names and no peer verification.
static bool _cfg_unchanged(ctls_ctx *ctx) {
  char buf[128];
  for (size_t i = 0; i < sizeof(g_cfg_names) / sizeof(g_cfg_names[0]); ++i) {
    if (strcmp(_served_cn(ctx, g_cfg_names[i], buf, sizeof(buf)),
               g_cfg_before[i]) != 0)
      return false;
  }
  return _served_verify_mode(ctx) == SSL_VERIFY_NONE;
}

// Tells whether ctx serves what a successful op configures.
static bool _cfg_applied(ctls_ctx *ctx, cfg_op_t op) {
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

// How many failure points a sweep over OpenSSL's allocations visits by
// default, so a run stays short; one call makes thousands of allocations and
// every failure in the rebuild takes the same rollback. CTLS_FULL_SWEEP=1 in
// the environment visits every index.
#define CTLS_CFG_OSSL_POINTS 120

// Runs op on ctx with allocation n failing, through OpenSSL when ossl is set
// and through g_sweep_mp otherwise, and gives the call's result. *fired tells
// whether the call got as far as allocation n.
static ccol_retval _cfg_run_failing(ctls_ctx *ctx, cfg_op_t op, bool ossl,
                                    long n, bool *fired) {
  ccol_retval rv;
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

// Gives the failure point after n in a sweep: 1, 1 + step, ... up to total,
// then total itself, then total + 1, which the call never reaches.
static long _cfg_next_point(long n, long step, long total) {
  if (n >= total) return n + 1;
  return n + step > total ? total : n + step;
}

// Sweeps op through every failure point, first counting the call's
// allocations with nothing failing. Each failed call must give
// ccol_not_enough_memory or ccol_http_tls_cert_load_failed and leave the
// fixture unchanged, also after an unrelated rebuild; a success must apply
// the change, and the point past the last allocation must succeed. Counts the
// ccol_not_enough_memory failures in *oom_failures and gives the number of
// failed calls, or -1 on a breach.
static long _cfg_sweep(cfg_op_t op, bool ossl, long *oom_failures) {
  ctls_ctx *ctx = _cfg_fixture(&g_sweep_mp);
  if (!ctx) return -1;
  bool fired = false;
  ccol_retval rv = _cfg_run_failing(ctx, op, ossl, LONG_MAX / 2, &fired);
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
    if (!_cfg_unchanged(ctx)) break;
    if (ctls_ctx_alpn_add(ctx, "h2", NULL, NULL, NULL, NULL) != ccol_success ||
        !_cfg_unchanged(ctx))
      break;
  }
  if (ctx) ctls_ctx_release(ctx);
  return -1;
}

// A rotation that kept the new entry after a failed rebuild would serve the
// default certificate for the rotated name, and a new name that stayed
// registered would serve the refused certificate at the next rebuild.
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
    REQUIRE_GT(ossl_oom, 0L);
  }
}

// A failed trust call that kept its bundle and verify flag would turn on
// SSL_VERIFY_PEER with the refused bundle at the next unrelated rebuild.
// trust_system makes about 10^5 OpenSSL allocations per call, so only this
// module's allocations are swept for it.
TEST(ctls_cfg_rollback, a_failed_trust_change_keeps_verification_off) {
  if (!g_certs_ready) return;
  for (int op = CFG_OP_TRUST; op < CFG_OP_COUNT; ++op) {
    long oom = 0;
    long lib_failures = _cfg_sweep((cfg_op_t)op, false, &oom);
    REQUIRE_GT(lib_failures, 0L);
    if (!g_ossl_mem_hooked || op == CFG_OP_TRUST_SYSTEM) continue;
    long ossl_oom = 0;
    long ossl_failures = _cfg_sweep((cfg_op_t)op, true, &ossl_oom);
    REQUIRE_GT(ossl_failures, 0L);
    REQUIRE_GT(ossl_oom, 0L);
  }
}

// Checks through a real handshake that a failed rotation leaves the rotated
// host serving its old certificate. The sweep runs from the end, where the
// rebuild's allocations are.
TEST(ctls_cfg_rollback, a_failed_rotation_serves_the_old_certificate) {
  if (!g_certs_ready || !g_ossl_mem_hooked) return;
  ctls_ctx *server_ctx = _cfg_fixture(NULL);
  REQUIRE_NE((void *)server_ctx, (void *)NULL);
  long total = 0;
  {
    ctls_ctx *probe = _cfg_fixture(NULL);
    _ossl_arm(LONG_MAX);
    ccol_retval rv = _cfg_run_op(probe, CFG_OP_ROTATE_NAME);
    _ossl_disarm();
    total = atomic_load(&g_ossl_alloc_seen);
    ctls_ctx_release(probe);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }
  ccol_retval rv = ccol_success;
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
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "rot.test", false, NULL);
  bool done = _drive_both(client_conn, server_conn, 200, NULL, NULL);
  char cn_buf[128];
  const char *cn = done ? _peer_cert_cn(ctls__conn_ssl_for_tests(client_conn),
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

// The three-CA bundle with a CRL and whether it was made (0 untried, 1 ready,
// -1 failed), for a_trust_bundle_is_installed_whole_or_not_at_all.
static char g_bundle3[320];
static int g_bundle3_state = 0;

// Makes g_bundle3 once with the openssl command: server.pem, client.pem,
// named.pem and a CRL issued by named.test. Gives whether it's ready.
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

// Removes g_bundle3 and the files the CRL generation left, at teardown.
static void _bundle3_remove(void) {
  static const char *const files[] = {"crl.cnf",     "crl.idx", "crl.idx.attr",
                                      "crl.idx.old", "crl.pem", "bundle3.pem"};
  char path[512];
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    snprintf(path, sizeof(path), "%s/%s", g_cert_dir, files[i]);
    unlink(path);
  }
}

// Tells whether the context serving name holds exactly the three CAs and the
// CRL of the bundle, with revocation checking and peer verification on.
static bool _bundle3_in_store(ctls_ctx *ctx, const char *name) {
  SSL_CTX *served = ctls__ctx_served_ctx_for_tests(ctx, name);
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

// Tells whether a client hello made from served offers exactly the fixture's
// ALPN list "h2" (extension 16, length 5, list length 3), so a test can catch
// a rebuild that lost its ALPN list.
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

// Runs ctls_ctx_trust(g_bundle3) on a fresh fixture with allocation n
// failing, through OpenSSL when ossl is set. Gives 1 for a success that
// installed the whole bundle everywhere with an empty error queue, 0 for a
// clean failure that changed nothing, 2 when n was never reached, and -1 for
// a breach.
static int _bundle3_try(bool ossl, long n) {
  ctls_ctx *ctx = _cfg_fixture(&g_sweep_mp);
  if (!ctx) return -1;
  ccol_retval rv;
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
        _ctx_offers_h2(ctls__ctx_served_ctx_for_tests(ctx, NULL)) &&
        _ctx_offers_h2(ctls__ctx_served_ctx_for_tests(ctx, "keep.test")) &&
        _ctx_offers_h2(ctls__ctx_served_ctx_for_tests(ctx, "rot.test")))
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

// A rebuild that ignored a parse giving nothing or a failed add would succeed
// with fewer CAs, or without the CRL and so without revocation checks; one
// that ignored a failed SSL_CTX_set_alpn_protos would offer no protocol. A
// second bundle repeating a CA is accepted.
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
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  ccol_retval first = ctls_ctx_trust(ctx, g_server_cert, NULL);
  ccol_retval second = ctls_ctx_trust(ctx, g_bundle3, NULL);
  ctls_ctx_release(ctx);
  _bundle3_remove();
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(breaches, 0L);
  REQUIRE_GT(failures, 0L);
  REQUIRE_EQ((int)first, (int)ccol_success);
  REQUIRE_EQ((int)second, (int)ccol_success);
}

// Without the shortcut SSL_read() of 0 bytes gives 0, which the
// classification reports as -1 with ECONNRESET or EWOULDBLOCK. Waiting input
// must stay for the next read.
TEST(ctls_io, a_read_of_zero_bytes_returns_zero) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    ctls_ctx_cert_add(ctx, NULL, NULL, NULL, NULL, NULL);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      _sweep_arm(n);
      ctls_conn *cl =
          ctls_conn_create_client(ctx, fds[0], "sweep.test", true, NULL);
      _sweep_disarm();
      if (cl) ctls_conn_destroy(cl);

      _sweep_arm(n);
      ctls_conn *sv = ctls_conn_create_server(ctx, fds[1], NULL, NULL);
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

// Password-protected private keys
TEST(ctls_pem_password, a_correct_password_loads_an_encrypted_key) {
  if (!g_enc_cert_ready) return;
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  char *err = NULL;
  ccol_retval rv = ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key,
                                     CTLS_TEST_KEY_PASSWORD, &err);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

// Without the password callback of ctls, OpenSSL would prompt on the terminal
// and hang.
TEST(ctls_pem_password, a_wrong_password_reports_a_load_failure) {
  if (!g_enc_cert_ready) return;
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  char *err = NULL;
  ccol_retval rv = ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key,
                                     "not-the-password", &err);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_pem_password, an_encrypted_key_with_no_password_reports_a_failure) {
  if (!g_enc_cert_ready) return;
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ccol_retval rv =
      ctls_ctx_cert_add(ctx, NULL, g_enc_cert, g_enc_key, NULL, NULL);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

TEST(ctls_pem_password, an_encrypted_named_cert_serves_a_real_handshake) {
  if (!g_enc_cert_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval added = ctls_ctx_cert_add(server_ctx, NULL, g_enc_cert, g_enc_key,
                                        CTLS_TEST_KEY_PASSWORD, NULL);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  _make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
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

// Unreadable and unusable PEM inputs
TEST(ctls_file_input, an_empty_file_reports_a_load_failure) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "/tmp/ctls_empty_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval rv = ccol_unexpected_failure;
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

// procfs entries report a length of 0 although they have content.
TEST(ctls_file_input, a_file_that_reports_zero_length_reports_a_load_failure) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  ccol_retval rv = ctls_ctx_trust(ctx, "/proc/self/status", NULL);
  ctls_ctx_release(ctx);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

// Opening a directory works on Linux, and the seek after it fails or reports
// LONG_MAX depending on the filesystem (tmpfs or ext4), so the type check
// must refuse it; the LONG_MAX case would otherwise reach the allocation. The
// directory is made in the build tree, on the filesystem the library reads
// from.
TEST(ctls_file_input, a_directory_in_place_of_a_bundle_reports_a_load_failure) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char dir[] = "ctls_dir_XXXXXX";
  bool made = (mkdtemp(dir) != NULL);
  ccol_retval rv = ccol_unexpected_failure;
  if (made) {
    rv = ctls_ctx_trust(ctx, dir, NULL);
    rmdir(dir);
  }
  ctls_ctx_release(ctx);

  REQUIRE_TRUE(made);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
}

// A path may name a log or a core dump; a sparse file past the cap is refused
// on its size, and the reason must name the size, not an unreadable file.
TEST(ctls_file_input, an_oversized_bundle_is_rejected_before_it_is_allocated) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_big_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval rv = ccol_unexpected_failure;
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
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "larger than"), (void *)NULL);
}

// A file below the cap fails on its contents (no certificate), and the
// reason tells which check ran.
TEST(ctls_file_input, a_bundle_just_under_the_limit_is_still_read) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_ok_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval rv = ccol_unexpected_failure;
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
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)err, (void *)NULL);
  if (err) {
    REQUIRE_NE((void *)strstr(err, "no certificates"), (void *)NULL);
    REQUIRE_EQ((void *)strstr(err, "larger than"), (void *)NULL);
  }
}

// An empty trust store would make a request-only server accept every client
// without a certificate, so the configuration call must fail.
TEST(ctls_file_input, a_bundle_holding_no_certificate_is_rejected) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);

  char path[] = "ctls_nocert_XXXXXX";
  int fd = mkstemp(path);
  ccol_retval rv = ccol_success;
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
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_sweep_mp, NULL);
    if (!ctx) continue;
    _sweep_arm(n);
    ccol_retval rv = ctls_ctx_trust(ctx, g_server_cert, NULL);
    _sweep_disarm();
    if (rv != ccol_success && rv != ccol_not_enough_memory &&
        rv != ccol_http_tls_cert_load_failed)
      handled = false;
    ctls_ctx_release(ctx);
  }
  REQUIRE_TRUE(handled);
}

// Read and write result classification
//
// chttpclient and chttpserver branch on the return value and errno of every
// read and write, so this pair is the interface between ctls and them.
//
// Builds two contexts, a socketpair in fds and a server and a client
// connection with the handshake done, handing all of them back through the
// out parameters. Gives false, with nothing left allocated, when the
// handshake can't be finished.
static bool _make_connected_pair(ctls_ctx **out_server_ctx,
                                 ctls_ctx **out_client_ctx,
                                 ctls_conn **out_server, ctls_conn **out_client,
                                 int fds[2]) {
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

// The read wants the read direction; a caller that re-armed for writability
// would stall the connection.
TEST(ctls_io, a_read_with_nothing_pending_asks_to_be_retried) {
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
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
  REQUIRE_FALSE(ctls_conn_wants_write(NULL));
}

TEST(ctls_io, a_roundtrip_reports_the_exact_byte_count) {
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
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
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  if (up) {
    ctls_conn_destroy(c);
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
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
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
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
  int fds[2] = {-1, -1};
  bool up = _make_connected_pair(&sc, &cc, &s, &c, fds);

  char buf[64];
  ssize_t n = -99;
  int err = 0;
  if (up) {
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

// The case where a caller must re-arm for writability, which
// ctls_conn_wants_write() tells it.
TEST(ctls_io, a_write_that_fills_the_socket_asks_for_the_write_direction) {
  ctls_ctx *sc, *cc;
  ctls_conn *s, *c;
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

// Certificate and key pairing, SNI precedence
//
// Completes one handshake against server_ctx with SNI name sni (NULL sends no
// SNI), so a test sees which certificate a name gets. *out_ok tells whether
// the handshake finished, and out_cn gets the CN the server served.
static void _serve_one_sni(ctls_ctx *server_ctx, const char *sni, bool *out_ok,
                           char *out_cn, size_t out_cn_len) {
  *out_ok = false;
  if (out_cn_len) out_cn[0] = '\0';
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  if (!client_ctx) return;
  int fds[2] = {-1, -1};
  _make_nonblocking_pair(fds);
  if (fds[0] < 0 || fds[1] < 0) {
    ctls_ctx_release(client_ctx);
    return;
  }
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], sni, false, NULL);
  if (server_conn && client_conn &&
      _drive_both(client_conn, server_conn, 200, NULL, NULL)) {
    char buf[256];
    const char *cn =
        _peer_cert_cn(ctls__conn_ssl_for_tests(client_conn), buf, sizeof(buf));
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

// Two well-formed files from different pairs, as a rotation that updates one
// path and forgets the other produces; the matching pair still loads.
TEST(ctls_ctx, cert_add_rejects_a_certificate_and_key_that_do_not_match) {
  if (!g_certs_ready) return;
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  char *default_err = NULL;
  ccol_retval default_rv = ctls_ctx_cert_add(ctx, NULL, g_server_cert,
                                             g_client_key, NULL, &default_err);
  char *named_err = NULL;
  ccol_retval named_rv = ctls_ctx_cert_add(ctx, "mismatch.test", g_server_cert,
                                           g_client_key, NULL, &named_err);
  ccol_retval good_rv =
      ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
  ctls_ctx_release(ctx);

  REQUIRE_EQ(default_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)default_err, (void *)NULL);
  REQUIRE_EQ(named_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_NE((void *)named_err, (void *)NULL);
  REQUIRE_EQ(good_rv, ccol_success);
}

// A refused pair must not stay stored, or the next unrelated rebuild fails on
// it; accepting the mismatch would leave a context that serves nothing.
TEST(ctls_ctx, a_rejected_pair_leaves_the_context_serving_what_it_had) {
  if (!g_certs_ready) return;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval first_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                           g_server_key, NULL, NULL);
  ccol_retval rotate_rv = ctls_ctx_cert_add(server_ctx, NULL, g_server_cert,
                                            g_client_key, NULL, NULL);
  bool served = false;
  char cn[256];
  _serve_one_sni(server_ctx, NULL, &served, cn, sizeof(cn));
  ccol_retval later_rv = ctls_ctx_trust(server_ctx, g_client_cert, NULL);
  ctls_ctx_release(server_ctx);

  REQUIRE_EQ(first_rv, ccol_success);
  REQUIRE_EQ(rotate_rv, ccol_http_tls_cert_load_failed);
  REQUIRE_TRUE(served);
  REQUIRE_STREQ(cn, "127.0.0.1");
  REQUIRE_EQ(later_rv, ccol_success);
}

// Registers three exact names and "*.wild.test" (the wildcard first when
// wildcard_first is set) and serves each exact name plus a name only the
// wildcard covers, so a test can check that the order of registration
// doesn't decide. *out_setup_ok and *out_all_served give the setup and
// handshake results, and served gets the four CNs.
static void _run_exact_beats_wildcard(bool wildcard_first, bool *out_setup_ok,
                                      bool *out_all_served,
                                      char served[4][256]) {
  static const char *const kExact[3] = {"a.wild.test", "b.wild.test",
                                        "c.wild.test"};
  *out_setup_ok = false;
  *out_all_served = false;
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
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

// "*.wild.test" answers one label below "wild.test" only; two labels and the
// bare domain fall through to the default.
TEST(ctls_sni, a_wildcard_covers_exactly_one_label) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  ccol_retval default_rv =
      ctls_ctx_cert_add(server_ctx, NULL, NULL, NULL, NULL, NULL);
  ccol_retval wild_rv =
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

// Session resumption
//
// The peer is a plain OpenSSL client, which keeps a session and offers it on
// the next connection as every real resuming client does.
//
// One resumption case: The protocol version, whether the client presents
// g_client_cert, and the SNI name (NULL for none).
typedef struct {
  int max_version;
  bool present_cert;
  const char *sni;
} _resume_case_t;

// Drives the handshake of a ctls server connection and a raw OpenSSL client,
// then moves one byte from the server to the client, which makes a TLS 1.3
// client take in the session tickets. Gives true when both finished and the
// byte arrived.
static bool _resume_drive(ctls_conn *server_conn, SSL *client) {
  bool s_done = false, c_done = false;
  for (int i = 0; i < 400 && !(s_done && c_done); i++) {
    if (!s_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(server_conn);
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

// Runs one connection of case rc between server_ctx and a raw client on
// client_ctx. It offers *sess_inout when it isn't NULL and replaces it with
// this connection's session, and *client_reused and *server_reused get what
// each side says about resumption. Gives false when the connection failed.
static bool _resume_connect(ctls_ctx *server_ctx, SSL_CTX *client_ctx,
                            const _resume_case_t *rc, SSL_SESSION **sess_inout,
                            bool *client_reused, bool *server_reused) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  for (int i = 0; i < 2; i++)
    fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
  ctls_conn *server_conn =
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
        SSL_session_reused(ctls__conn_ssl_for_tests(server_conn)) == 1;
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

// Whether TLS 1.3 cases run, and whether the first connection issued a
// session that can be resumed. LibreSSL resumes no TLS 1.3 session and its
// SSL_SESSION_is_resumable() says 0 for everything, so there a session that
// exists is enough.
#if defined(LIBRESSL_VERSION_NUMBER)
#define RESUME_TLS13 0
#define RESUME_SESSION_ISSUED(s) ((s) != NULL)
#else
#define RESUME_TLS13 1
#define RESUME_SESSION_ISSUED(s) \
  ((s) != NULL && SSL_SESSION_is_resumable(s) == 1)
#endif

// Runs case rc twice against a server with a trust store (optional client
// certificate when the client presents none), so a test can check resumption
// for that client shape. Gives true when the second connection resumed on
// both sides and the first didn't.
static bool _resume_case_resumes(const _resume_case_t *rc) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  bool ok = server_ctx &&
            ctls_ctx_cert_add(server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success &&
            ctls_ctx_trust(server_ctx, g_client_cert, NULL) == ccol_success;
  if (ok && rc->sni)
    ok = ctls_ctx_cert_add(server_ctx, rc->sni, NULL, NULL, NULL, NULL) ==
         ccol_success;
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

// Hostname verification needs a hostname
TEST(ctls_conn, verify_host_with_no_hostname_is_refused) {
  ctls_ctx *ctx = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)ctx, (void *)NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  char *err_null = NULL, *err_empty = NULL;
  ctls_conn *a = ctls_conn_create_client(ctx, fds[1], NULL, true, &err_null);
  ctls_conn *b = ctls_conn_create_client(ctx, fds[1], "", true, &err_empty);
  ctls_conn *c = ctls_conn_create_client(ctx, fds[1], NULL, false, NULL);
  ctls_conn *d = ctls_conn_create_client(ctx, fds[1], "", false, NULL);
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

// Input the TLS layer already holds
//
// One 12000-byte write is one record, so a 4096-byte read leaves the rest
// inside the TLS layer while the socket is empty.
TEST(ctls_conn, pending_input_reports_plaintext_that_the_socket_no_longer_has) {
  REQUIRE_FALSE(ctls_conn_has_pending_input(NULL));
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);
  int fds[2];
  _make_nonblocking_pair(fds);
  int room = 65536;
  for (int i = 0; i < 2; i++) {
    setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));
    setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
  }
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  bool hs = _drive_both(client_conn, server_conn, 200, NULL, NULL);

  static char payload[12000];
  memset(payload, 'p', sizeof(payload));
  bool pending_before = true, pending_mid = false, pending_after = true;
  bool socket_empty_mid = false;
  size_t total = 0;
  if (hs) {
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

// Construction and peer-certificate answers under allocation failure
//
// The allocations and frees of non-NULL blocks through g_cnt_mp, so a test can
// tell that a context gave back everything it took.
static _Atomic long g_cnt_allocs = 0;
static _Atomic long g_cnt_frees = 0;

// The g_cnt_mp functions, which count into g_cnt_allocs and g_cnt_frees; a
// realloc counts only when it allocates a new block.
static void *_cnt_malloc(size_t n) {
  void *p = malloc(n);
  if (p) atomic_fetch_add(&g_cnt_allocs, 1);
  return p;
}
static void *_cnt_calloc(size_t a, size_t b) {
  void *p = calloc(a, b);
  if (p) atomic_fetch_add(&g_cnt_allocs, 1);
  return p;
}
static void *_cnt_realloc(void *old, size_t n) {
  void *p = realloc(old, n);
  if (p && !old) atomic_fetch_add(&g_cnt_allocs, 1);
  return p;
}
static void _cnt_free(void *p) {
  if (p) atomic_fetch_add(&g_cnt_frees, 1);
  free(p);
}
// The counting procs.
static ccol_memmgmt_procs g_cnt_mp = {_cnt_malloc, _cnt_free, _cnt_calloc,
                                      _cnt_realloc};

// Zeroes the two counters before a measured run.
static void _cnt_reset(void) {
  atomic_store(&g_cnt_allocs, 0);
  atomic_store(&g_cnt_frees, 0);
}

// An unarmed build runs first so OpenSSL's one-time init is done before the
// sweep; a failure inside a run-once init would stick for the process.
TEST(ctls_oom, ctx_new_under_openssl_allocation_failure_gives_everything_back) {
  if (!g_ossl_mem_hooked) return;
  ctls_ctx *warm = ctls_ctx_new_mp(&g_cnt_mp, NULL);
  bool warmed = warm != NULL;
  if (warm) ctls_ctx_release(warm);
  long failures = 0;
  bool err_missing = false;
  bool unbalanced = false;
  bool finished = false;
  for (long n = 1; warmed && n <= 2000 && !finished; n++) {
    _cnt_reset();
    char *err = NULL;
    _ossl_arm(n);
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_cnt_mp, &err);
    _ossl_disarm();
    bool fired = atomic_load(&g_ossl_alloc_fired);
    if (ctx) {
      ctls_ctx_release(ctx);
      finished = !fired;
    } else {
      failures++;
      if (!err) err_missing = true;
    }
    if (atomic_load(&g_cnt_allocs) != atomic_load(&g_cnt_frees))
      unbalanced = true;
  }
  REQUIRE_TRUE(warmed);
  REQUIRE_TRUE(finished);
  REQUIRE_GT(failures, 0L);
  REQUIRE_FALSE(err_missing);
  REQUIRE_FALSE(unbalanced);
}

// The connection constructors the sweeps cover: A client by name, a client by
// IP literal, and a server.
typedef enum {
  CREATE_CLIENT_NAME,
  CREATE_CLIENT_IP,
  CREATE_SERVER,
  CREATE_KIND_COUNT
} create_kind_t;

// Creates a connection of kind k on fd with ctx, passing err through, so one
// sweep loop covers every constructor. Gives the connection or NULL.
static ctls_conn *_create_of_kind(ctls_ctx *ctx, int fd, create_kind_t k,
                                  char **err) {
  switch (k) {
    case CREATE_CLIENT_NAME:
      return ctls_conn_create_client(ctx, fd, "sweep.test", true, err);
    case CREATE_CLIENT_IP:
      return ctls_conn_create_client(ctx, fd, "127.0.0.1", true, err);
    case CREATE_SERVER:
      return ctls_conn_create_server(ctx, fd, NULL, err);
    case CREATE_KIND_COUNT:
      break;
  }
  return NULL;
}

// A connection that can't be built under an OpenSSL allocation failure gives
// NULL with an err_str naming its create call and hands back the SSL_CTX pin,
// the socket BIO and the context reference; once the context is released the
// allocator has every block back.
TEST(ctls_oom,
     conn_create_under_openssl_allocation_failure_gives_everything_back) {
  if (!g_ossl_mem_hooked || !g_certs_ready) return;
  static const char *const prefix[CREATE_KIND_COUNT] = {
      "ctls_conn_create_client:", "ctls_conn_create_client:",
      "ctls_conn_create_server:"};
  for (int k = 0; k < CREATE_KIND_COUNT; k++) {
    _cnt_reset();
    ctls_ctx *ctx = ctls_ctx_new_mp(&g_cnt_mp, NULL);
    REQUIRE_NE((void *)ctx, NULL);
    ccol_retval rv_cert =
        ctls_ctx_cert_add(ctx, NULL, g_server_cert, g_server_key, NULL, NULL);
    int fds[2] = {-1, -1};
    bool paired = socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
    long pins_before = ctls__live_conn_ctx_pin_count_for_tests();
    size_t bios_before = ctls__live_sock_bio_count_for_tests();
    long failures = 0;
    bool bad_err = false;
    bool leaked = false;
    bool finished = false;
    for (long n = 1; paired && n <= 2000 && !finished; n++) {
      char *err = NULL;
      _ossl_arm(n);
      ctls_conn *conn = _create_of_kind(ctx, fds[0], (create_kind_t)k, &err);
      _ossl_disarm();
      bool fired = atomic_load(&g_ossl_alloc_fired);
      if (conn) {
        ctls_conn_destroy(conn);
        finished = !fired;
      } else {
        failures++;
        if (!err || strncmp(err, prefix[k], strlen(prefix[k])) != 0)
          bad_err = true;
      }
      if (ctls__live_conn_ctx_pin_count_for_tests() != pins_before ||
          ctls__live_sock_bio_count_for_tests() != bios_before)
        leaked = true;
    }
    ctls_conn *after =
        paired ? _create_of_kind(ctx, fds[0], (create_kind_t)k, NULL) : NULL;
    ctls_conn_destroy(after);
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
    ctls_ctx_release(ctx);
    long allocs = atomic_load(&g_cnt_allocs);
    long frees = atomic_load(&g_cnt_frees);

    REQUIRE_EQ(rv_cert, ccol_success);
    REQUIRE_TRUE(paired);
    REQUIRE_TRUE(finished);
    REQUIRE_GT(failures, 0L);
    REQUIRE_FALSE(bad_err);
    REQUIRE_FALSE(leaked);
    REQUIRE_NE((void *)after, NULL);
    REQUIRE_EQ(allocs, frees);
  }
}

// A verified mutual TLS pair: The server trusts the client's self-signed
// certificate, and the server context takes its memory from server_mp.
typedef struct {
  ctls_ctx *server_ctx;
  ctls_ctx *client_ctx;
  ctls_conn *server;
  ctls_conn *client;
  int fds[2];
} _mtls_pair_t;

// Destroys whatever _mtls_open() built and resets *p to empty, so it can be
// called on a partly built pair.
static void _mtls_close(_mtls_pair_t *p) {
  ctls_conn_destroy(p->client);
  ctls_conn_destroy(p->server);
  if (p->fds[0] >= 0) close(p->fds[0]);
  if (p->fds[1] >= 0) close(p->fds[1]);
  ctls_ctx_release(p->client_ctx);
  ctls_ctx_release(p->server_ctx);
  memset(p, 0, sizeof(*p));
  p->fds[0] = p->fds[1] = -1;
}

// Builds a mutual TLS pair in *p with the server context on server_mp; the
// client presents its certificate when client_presents is set, otherwise the
// server only requests one. Gives false, with everything closed, on failure.
static bool _mtls_open(_mtls_pair_t *p, ccol_memmgmt_procs *server_mp,
                       bool client_presents) {
  memset(p, 0, sizeof(*p));
  p->fds[0] = p->fds[1] = -1;
  p->server_ctx = ctls_ctx_new_mp(server_mp, NULL);
  p->client_ctx = ctls_ctx_new(NULL);
  bool ok = p->server_ctx && p->client_ctx &&
            ctls_ctx_cert_add(p->server_ctx, NULL, g_server_cert, g_server_key,
                              NULL, NULL) == ccol_success &&
            ctls_ctx_trust(p->server_ctx, g_client_cert, NULL) == ccol_success;
  if (ok && client_presents)
    ok = ctls_ctx_cert_add(p->client_ctx, NULL, g_client_cert, g_client_key,
                           NULL, NULL) == ccol_success;
  if (ok && !client_presents)
    ok = ctls_ctx_peer_cert_optional(p->server_ctx, true) == ccol_success;
  if (ok) {
    _make_nonblocking_pair(p->fds);
    p->server = ctls_conn_create_server(p->server_ctx, p->fds[0], NULL, NULL);
    p->client =
        ctls_conn_create_client(p->client_ctx, p->fds[1], NULL, false, NULL);
    ok = p->server && p->client &&
         _drive_both(p->client, p->server, 200, NULL, NULL);
  }
  if (!ok) _mtls_close(p);
  return ok;
}

// Reads g_client_cert and gives its DER encoding (free it with OPENSSL_free)
// with the length in *len_out, so a test can compare the accessor's answer.
// NULL on failure.
static unsigned char *_client_cert_der(int *len_out) {
  *len_out = 0;
  FILE *f = fopen(g_client_cert, "r");
  if (!f) return NULL;
  X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
  fclose(f);
  if (!x) return NULL;
  unsigned char *der = NULL;
  int len = i2d_X509(x, &der);
  X509_free(x);
  if (len <= 0) return NULL;
  *len_out = len;
  return der;
}

// A second call answers from the same storage, and a peer with no certificate
// gets NULL, a zero length and an all-zero digest.
TEST(ctls_handshake, peer_cert_accessors_describe_the_verified_client) {
  if (!g_certs_ready) return;
  int want_len = 0;
  unsigned char *want = _client_cert_der(&want_len);
  unsigned char want_md[CTLS_SHA256_LEN] = {0};
  unsigned int md_len = 0;
  bool digested = want && EVP_Digest(want, (size_t)want_len, want_md, &md_len,
                                     EVP_sha256(), NULL) == 1;

  _mtls_pair_t p;
  bool opened = _mtls_open(&p, NULL, true);
  size_t len = 0, len2 = 0;
  const unsigned char *der =
      opened ? ctls_conn_peer_cert_der(p.server, &len) : NULL;
  const unsigned char *der2 =
      opened ? ctls_conn_peer_cert_der(p.server, &len2) : NULL;
  bool der_ok = der && want && len == (size_t)want_len &&
                memcmp(der, want, len) == 0 && der2 == der && len2 == len;
  unsigned char md[CTLS_SHA256_LEN];
  bool md_ok = opened && ctls_conn_peer_cert_sha256(p.server, md) && digested &&
               memcmp(md, want_md, sizeof(md)) == 0;
  const char *subject = opened ? ctls_conn_peer_cert_subject(p.server) : NULL;
  bool subject_ok = subject && strcmp(subject, "CN=test-client") == 0 &&
                    ctls_conn_peer_cert_subject(p.server) == subject;
  size_t client_len = 99;
  bool client_none = opened &&
                     !ctls_conn_peer_cert_der(p.client, &client_len) &&
                     client_len == 0;
  _mtls_close(&p);

  _mtls_pair_t anon;
  bool anon_opened = _mtls_open(&anon, NULL, false);
  size_t anon_len = 99;
  unsigned char anon_md[CTLS_SHA256_LEN];
  memset(anon_md, 0xAB, sizeof(anon_md));
  bool anon_none =
      anon_opened && !ctls_conn_peer_cert_der(anon.server, &anon_len) &&
      anon_len == 0 && !ctls_conn_peer_cert_sha256(anon.server, anon_md) &&
      !ctls_conn_peer_cert_subject(anon.server);
  bool anon_md_zero = true;
  for (size_t i = 0; i < sizeof(anon_md); i++)
    if (anon_md[i] != 0) anon_md_zero = false;
  _mtls_close(&anon);
  OPENSSL_free(want);

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(der_ok);
  REQUIRE_TRUE(md_ok);
  REQUIRE_TRUE(subject_ok);
  REQUIRE_TRUE(client_none);
  REQUIRE_TRUE(anon_opened);
  REQUIRE_TRUE(anon_none);
  REQUIRE_TRUE(anon_md_zero);
}

// An accessor whose answer can't be built (a block of the context allocator,
// or an OpenSSL allocation) answers empty and caches nothing, so the next call
// with memory gives the full answer.
TEST(ctls_oom, peer_cert_accessors_under_allocation_failure_cache_nothing) {
  if (!g_certs_ready) return;
  int want_len = 0;
  unsigned char *want = _client_cert_der(&want_len);
  REQUIRE_NE((void *)want, NULL);
  long failures = 0;
  bool bad_failure = false;
  bool bad_recovery = false;
  bool finished_lib = false, finished_ossl = !g_ossl_mem_hooked;
  for (int ossl = 0; ossl < 2; ossl++) {
    if (ossl && !g_ossl_mem_hooked) break;
    bool *finished = ossl ? &finished_ossl : &finished_lib;
    for (long n = 1; n <= 200 && !*finished; n++) {
      _mtls_pair_t p;
      if (!_mtls_open(&p, &g_sweep_mp, true)) {
        bad_recovery = true;
        break;
      }
      size_t len = 99;
      bool fired;
      const unsigned char *der;
      if (ossl) {
        _ossl_arm(n);
        der = ctls_conn_peer_cert_der(p.server, &len);
        _ossl_disarm();
        fired = atomic_load(&g_ossl_alloc_fired);
      } else {
        _sweep_arm((int)n);
        der = ctls_conn_peer_cert_der(p.server, &len);
        fired = atomic_load(&g_alloc_seen) >= n;
        _sweep_disarm();
      }
      if (!der) {
        failures++;
        if (len != 0) bad_failure = true;
      } else {
        *finished = !fired;
      }
      size_t len2 = 0;
      const unsigned char *der2 = ctls_conn_peer_cert_der(p.server, &len2);
      const char *subject = ctls_conn_peer_cert_subject(p.server);
      if (!der2 || len2 != (size_t)want_len || memcmp(der2, want, len2) != 0 ||
          !subject || strcmp(subject, "CN=test-client") != 0)
        bad_recovery = true;
      _mtls_close(&p);
    }
  }
  long md_failures = 0;
  bool md_bad = false;
  bool md_finished = !g_ossl_mem_hooked;
  _mtls_pair_t p;
  bool opened = _mtls_open(&p, NULL, true);
  unsigned char good[CTLS_SHA256_LEN] = {0};
  bool have_good = opened && ctls_conn_peer_cert_sha256(p.server, good);
  for (long n = 1; have_good && !md_finished && n <= 200; n++) {
    unsigned char md[CTLS_SHA256_LEN];
    memset(md, 0xAB, sizeof(md));
    _ossl_arm(n);
    bool ok = ctls_conn_peer_cert_sha256(p.server, md);
    _ossl_disarm();
    bool fired = atomic_load(&g_ossl_alloc_fired);
    if (ok) {
      md_finished = !fired;
      if (memcmp(md, good, sizeof(md)) != 0) md_bad = true;
    } else {
      md_failures++;
      for (size_t i = 0; i < sizeof(md); i++)
        if (md[i] != 0) md_bad = true;
    }
  }
  if (opened) _mtls_close(&p);
  OPENSSL_free(want);

  REQUIRE_TRUE(finished_lib);
  REQUIRE_TRUE(finished_ossl);
  REQUIRE_GT(failures, 0L);
  REQUIRE_FALSE(bad_failure);
  REQUIRE_FALSE(bad_recovery);
  REQUIRE_TRUE(have_good);
  REQUIRE_TRUE(md_finished);
  if (g_ossl_mem_hooked) REQUIRE_GT(md_failures, 0L);
  REQUIRE_FALSE(md_bad);
}

// What _alpn_log_cleanup() saw: How many cleanups ran and the last udata.
typedef struct {
  int cleanups;
  void *last_udata;
} _alpn_cleanup_log_t;

// The log the cleanup below writes to.
static _alpn_cleanup_log_t g_alpn_log;
// An ALPN cleanup that counts its runs and records udata in g_alpn_log, so a
// test sees which registration was cleaned up.
static void _alpn_log_cleanup(void *udata) {
  g_alpn_log.cleanups++;
  g_alpn_log.last_udata = udata;
}

// The failed replacement runs no cleanup, and the next successful one cleans
// up the earlier registration with its own udata, which shows it was still
// the stored one.
TEST(ctls_oom, a_failed_alpn_replacement_keeps_the_earlier_registration) {
  if (!g_ossl_mem_hooked) return;
  int old_udata = 1, new_udata = 2, last_udata = 3;
  ctls_ctx *probe = ctls_ctx_new(NULL);
  REQUIRE_NE((void *)probe, NULL);
  ccol_retval rv_first =
      ctls_ctx_alpn_add(probe, "h2", NULL, &old_udata, NULL, NULL);
  _ossl_arm(LONG_MAX);
  ccol_retval rv_count =
      ctls_ctx_alpn_add(probe, "h2", NULL, &new_udata, NULL, NULL);
  _ossl_disarm();
  long total = atomic_load(&g_ossl_alloc_seen);
  ctls_ctx_release(probe);
  REQUIRE_EQ(rv_first, ccol_success);
  REQUIRE_EQ(rv_count, ccol_success);
  REQUIRE_GT(total, 0L);

  long step = total > 60 ? total / 60 : 1;
  long failures = 0;
  bool breach = false;
  for (long n = 1; n <= total; n += step) {
    ctls_ctx *ctx = ctls_ctx_new(NULL);
    if (!ctx || ctls_ctx_alpn_add(ctx, "h2", NULL, &old_udata,
                                  _alpn_log_cleanup, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      breach = true;
      break;
    }
    memset(&g_alpn_log, 0, sizeof(g_alpn_log));
    char *err = NULL;
    _ossl_arm(n);
    ccol_retval rv =
        ctls_ctx_alpn_add(ctx, "h2", NULL, &new_udata, _alpn_log_cleanup, &err);
    _ossl_disarm();
    if (rv == ccol_success) {
      if (g_alpn_log.cleanups != 1 || g_alpn_log.last_udata != &old_udata)
        breach = true;
      ctls_ctx_release(ctx);
      continue;
    }
    failures++;
    bool kept = g_alpn_log.cleanups == 0 && ctls_ctx_alpn_count(ctx) == 1 &&
                err && strstr(err, "rebuild failure") != NULL;
    ccol_retval rv_after = ctls_ctx_alpn_add(ctx, "h2", NULL, &last_udata,
                                             _alpn_log_cleanup, NULL);
    if (!kept || rv_after != ccol_success || g_alpn_log.cleanups != 1 ||
        g_alpn_log.last_udata != &old_udata)
      breach = true;
    ctls_ctx_release(ctx);
    if (g_alpn_log.cleanups != 2 || g_alpn_log.last_udata != &last_udata)
      breach = true;
  }
  REQUIRE_GT(failures, 0L);
  REQUIRE_FALSE(breach);
}

// What the hashed-directory child reports: The trust result and whether the
// client verified, with the directory holding the server certificate and
// with an empty one.
typedef struct {
  ccol_retval rv_dir;
  bool verified_dir;
  ccol_retval rv_empty;
  bool verified_empty;
} _system_ca_dir_result_t;

// Links g_server_cert into dir under its subject hash, as OpenSSL's hashed
// lookup expects. Gives false on failure.
static bool _hashed_dir_make(const char *dir) {
  FILE *f = fopen(g_server_cert, "r");
  if (!f) return false;
  X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
  fclose(f);
  if (!x) return false;
  char link_path[320];
  int n = snprintf(link_path, sizeof(link_path), "%s/%08lx.0", dir,
                   X509_subject_name_hash(x));
  X509_free(x);
  return n > 0 && (size_t)n < sizeof(link_path) &&
         symlink(g_server_cert, link_path) == 0;
}

// The child of the hashed-directory test: Makes a hashed directory and an
// empty one, points SSL_CERT_FILE at a key (which contributes no
// certificate) and SSL_CERT_DIR at each directory in turn, and records in the
// _system_ca_dir_result_t at out whether a system-trust client verifies.
// It removes the directories afterwards.
static void _system_ca_dir_body(void *out) {
  _system_ca_dir_result_t *r = (_system_ca_dir_result_t *)out;
  char dir[300], empty[300];
  snprintf(dir, sizeof(dir), "%s/hashed", g_cert_dir);
  snprintf(empty, sizeof(empty), "%s/hashed_empty", g_cert_dir);
  bool made =
      mkdir(dir, 0700) == 0 && mkdir(empty, 0700) == 0 && _hashed_dir_make(dir);
  if (made) {
    setenv("SSL_CERT_DIR", dir, 1);
    _system_ca_env_set(g_server_key);
    ctls_ctx *ctx = ctls_ctx_new(NULL);
    r->rv_dir = ctx ? ctls_ctx_trust_system(ctx) : ccol_unexpected_failure;
    r->verified_dir =
        r->rv_dir == ccol_success && _client_verifies_test_server(ctx);
    ctls_ctx_release(ctx);

    setenv("SSL_CERT_DIR", empty, 1);
    _system_ca_env_set(g_server_key);
    ctx = ctls_ctx_new(NULL);
    r->rv_empty = ctx ? ctls_ctx_trust_system(ctx) : ccol_unexpected_failure;
    r->verified_empty =
        r->rv_empty == ccol_success && _client_verifies_test_server(ctx);
    ctls_ctx_release(ctx);
  }
  char link_path[600];
  DIR *d = opendir(dir);
  for (struct dirent *e; d && (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    int n = snprintf(link_path, sizeof(link_path), "%s/%s", dir, e->d_name);
    if (n > 0 && (size_t)n < sizeof(link_path)) unlink(link_path);
  }
  if (d) closedir(d);
  rmdir(dir);
  rmdir(empty);
}

// With a default CA file that holds no certificate, the system store is the
// directory SSL_CERT_DIR names: A client trusts the server whose certificate
// it holds, and nothing from an empty one. Runs in a child.
TEST(ctls_system_ca,
     a_default_file_without_certificates_falls_back_to_the_directory) {
  if (!g_certs_ready) return;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_dir_result_t r = {ccol_unexpected_failure, false,
                               ccol_unexpected_failure, true};
  bool ran = _ctls_run_in_child(_system_ca_dir_body, &r, sizeof(r), 30000);
  REQUIRE_TRUE(ran);
  REQUIRE_EQ(r.rv_dir, ccol_success);
  REQUIRE_TRUE(r.verified_dir);
  REQUIRE_EQ(r.rv_empty, ccol_success);
  REQUIRE_FALSE(r.verified_empty);
}

// What the allocation sweep child reports: Failed and broken calls, whether
// the sweep got past the last allocation, and the clean call's verification
// and trusted certificate count.
typedef struct {
  long failures;
  long breaches;
  bool reached_the_end;
  bool clean_verified;
  int clean_count;
} _system_ca_oom_result_t;

// Writes six certificates, the test server's among them, to path. OpenSSL's
// parse list starts with room for four, so only a longer bundle makes the
// parse grow it, and a failed growth gives NULL with an empty error queue.
// Gives false when it couldn't write.
static bool _write_system_ca_bundle(const char *path) {
  const char *parts[] = {g_client_cert, g_enc_cert, g_named_cert,
                         g_client_cert, g_enc_cert, g_server_cert};
  FILE *out = fopen(path, "w");
  if (!out) return false;
  bool ok = true;
  for (size_t i = 0; ok && i < sizeof(parts) / sizeof(parts[0]); ++i) {
    FILE *in = fopen(parts[i], "r");
    if (!in) {
      ok = false;
      break;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      if (fwrite(buf, 1, n, out) != n) ok = false;
    fclose(in);
  }
  if (fclose(out) != 0) ok = false;
  return ok;
}

// The child of a_default_file_that_cannot_be_parsed_for_memory_says_so: With
// SSL_CERT_FILE at the six-certificate bundle and SSL_CERT_DIR empty, it
// fails each OpenSSL allocation of ctls_ctx_trust_system() in turn. A success
// must trust as many certificates as the clean call (which must also verify
// the server in a handshake; a handshake per success would take minutes under
// valgrind); a call that ran out of memory reading the file must say so,
// since falling back to the empty directory trusts nothing. The sweep runs to
// twice the clean count (allocation counts differ a lot between TLS
// libraries) and stops at the first index nothing fails. CTLS_SAMPLED_SWEEP
// (set by the memtest target) visits about 300 points, because the cost grows
// with the square of the count. Results go into the _system_ca_oom_result_t
// at out.
static void _system_ca_oom_body(void *out) {
  _system_ca_oom_result_t *r = (_system_ca_oom_result_t *)out;
  char empty[300];
  snprintf(empty, sizeof(empty), "%s/hashed_empty_oom", g_cert_dir);
  char bundle[300];
  snprintf(bundle, sizeof(bundle), "%s/system_bundle_oom.pem", g_cert_dir);
  if (!_write_system_ca_bundle(bundle)) return;
  if (mkdir(empty, 0700) != 0) {
    unlink(bundle);
    return;
  }
  setenv("SSL_CERT_DIR", empty, 1);
  _system_ca_env_set(bundle);
  ctls_ctx *clean = ctls_ctx_new(NULL);
  _ossl_arm(LONG_MAX);
  ccol_retval clean_rv =
      clean ? ctls_ctx_trust_system(clean) : ccol_unexpected_failure;
  _ossl_disarm();
  long clean_allocs = atomic_load(&g_ossl_alloc_seen);
  if (clean_rv == ccol_success) {
    r->clean_count = ctls__trusted_cert_count_for_tests(clean);
    r->clean_verified = _client_verifies_test_server(clean);
  }
  ctls_ctx_release(clean);
  long sweep_max = 2 * clean_allocs + 64;
  long step = 1;
  if (getenv("CTLS_SAMPLED_SWEEP") && clean_allocs > 300)
    step = clean_allocs / 300;
  for (long nth = 1; nth < sweep_max && !r->reached_the_end;
       nth = nth >= clean_allocs         ? nth + 1
             : nth + step > clean_allocs ? clean_allocs
                                         : nth + step) {
    _system_ca_env_set(bundle);
    ctls_ctx *ctx = ctls_ctx_new(NULL);
    if (!ctx) {
      r->breaches++;
      break;
    }
    _ossl_arm(nth);
    ccol_retval rv = ctls_ctx_trust_system(ctx);
    bool fired = atomic_load(&g_ossl_alloc_fired);
    _ossl_disarm();
    if (!fired) r->reached_the_end = true;
    if (rv == ccol_not_enough_memory)
      r->failures++;
    else if (rv != ccol_success ||
             ctls__trusted_cert_count_for_tests(ctx) != r->clean_count)
      r->breaches++;
    ctls_ctx_release(ctx);
  }
  rmdir(empty);
  unlink(bundle);
}

TEST(ctls_system_ca, a_default_file_that_cannot_be_parsed_for_memory_says_so) {
  if (!g_certs_ready) return;
  if (!g_ossl_mem_hooked) {
    fprintf(stderr, "SKIP: this TLS library takes no custom allocator\n");
    return;
  }
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _system_ca_oom_result_t r = {0, 0, false, false, -1};
  bool ran = _ctls_run_in_child(_system_ca_oom_body, &r, sizeof(r), 600000);
  REQUIRE_TRUE(ran);
  REQUIRE_TRUE(r.clean_verified);
  REQUIRE_GE(r.clean_count, 4);
  REQUIRE_TRUE(r.reached_the_end);
  REQUIRE_GT(r.failures, 0L);
  REQUIRE_EQ(r.breaches, 0L);
}
