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
#include <common.h>
#include <cthreadpool.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
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

/* ========================================================================== */
/*   REAL TLS HANDSHAKE COVERAGE FOR THE ASYNC ENGINE (dedicated binary)      */
/*                                                                            */
/* tests/chttpclient's own async_step_a TLS tests only cover failure paths   */
/* (connection refused, handshake against a non-TLS server) -- a real        */
/* successful handshake needs a valid certificate/key pair and a peer that   */
/* actually speaks TLS. This suite generates a real, throwaway self-signed   */
/* cert/key pair via the `openssl` CLI at startup and runs a minimal,        */
/* hand-rolled raw-OpenSSL mock TLS server (SSL_accept/SSL_read/SSL_write --*/
/* NOT chttpserver.c) to drive chttpclient's async engine through a genuine  */
/* end-to-end HTTPS request. It cannot reuse tests/chttpserver_tls's fixture:*/
/* that suite runs chttpserver's own facio engine, which cannot share a      */
/* process with chttpclient's independent async engine (see chttpclient.c's */
/* g_client_engine_* comments) -- both are process-wide facio reactors, and  */
/* only one can be started per process. Kept in its own binary (mirroring   */
/* tests/chttpserver_tls) so a missing/broken openssl CLI or an invalid cert */
/* file -- either of which makes the vendored facio TLS layer call          */
/* FIO_LOG_FATAL and abort the whole process -- cannot take the rest of the  */
/* chttpclient test suite down with it.                                     */
/* ========================================================================== */

/* White-box entry points into chttpclient's async engine internals (the
 * engine lifecycle itself has no public equivalent). chttpclient_do_async
 * and friends -- used below -- are the real, public Tier 2 API. */
extern int _chttpclient_engine_ref_count_for_tests(void);
extern void _chttpclient_engine_wait_for_quiescence_for_tests(void);

/* See tests/chttpclient/tests.c's identical helper for why this is needed:
 * a request's future is fulfilled before on_close runs (and on_close is what
 * actually releases the engine reference), so ctpool_future_get() returning
 * is not sufficient evidence that the engine has even started tearing down
 * yet, let alone finished. */
static void wait_for_async_engine_idle(void) {
  for (int i = 0; i < 2000 && _chttpclient_engine_ref_count_for_tests() > 0;
       i++) {
    usleep(1000);
  }
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

#define TLS_TEST_BODY "{\"status\":\"ok\"}"

static SSL_CTX *g_ssl_ctx = NULL;
static int g_srv_fd = -1;
static int g_srv_port = 0;
static pthread_t g_accept_tid;
static atomic_int g_srv_running = 0;
static char g_cert_dir[256];
static char g_cert_path[320];
static char g_key_path[320];
static bool g_cert_ready = false;

/* Connection-thread registry so _teardown can join them all before freeing
 * g_ssl_ctx out from under any still-running SSL_accept. */
#define MAX_TLS_CONN_THREADS 64
static pthread_t g_conn_threads[MAX_TLS_CONN_THREADS];
static int g_conn_thread_count = 0;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Generates a throwaway self-signed cert/key pair into a fresh mkdtemp()
 * directory via the openssl CLI (same approach as tests/chttpserver_tls).
 * Returns 0 on success, -1 on any failure -- callers must treat -1 as "TLS
 * integration could not be verified in this environment" rather than crash,
 * since fio_tls_cert_add's FIO_LOG_FATAL on a missing/invalid cert file
 * would abort the whole process. */
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

static int _generate_self_signed_cert(void) {
  snprintf(g_cert_dir, sizeof(g_cert_dir), "/tmp/chttpclient_tls_test_XXXXXX");
  if (!mkdtemp(g_cert_dir)) return -1;

  int dn =
      snprintf(g_cert_path, sizeof(g_cert_path), "%s/cert.pem", g_cert_dir);
  int kn = snprintf(g_key_path, sizeof(g_key_path), "%s/key.pem", g_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_key_path))
    return -1;

  /* subjectAltName=IP:127.0.0.1: a real IP-address certificate, matching
   * fio_tls_openssl.c's connect-side X509_check_ip verification path
   * (reached via X509_VERIFY_PARAM_set1_ip_asc for an IP-literal target),
   * not the legacy CN-matching fallback -- see fio.c's own historical notes
   * on this exact distinction. */
  return _openssl_selfsigned(g_key_path, g_cert_path, "127.0.0.1",
                             "IP:127.0.0.1");
}

static void _remove_generated_cert(void) {
  if (g_cert_path[0]) unlink(g_cert_path);
  if (g_key_path[0]) unlink(g_key_path);
  if (g_cert_dir[0]) rmdir(g_cert_dir);
}

/* Reads one HTTP/1.1 request off ssl (headers only; this suite's requests
 * never send a body) up to the terminating blank line, ignoring the actual
 * content -- every route below responds identically regardless of what was
 * requested, except path-based routing for /large. */
static void _tls_read_request(SSL *ssl, char *buf, size_t max, char *path,
                              size_t path_max) {
  size_t total = 0;
  buf[0] = '\0';
  while (total < max - 1) {
    int n = SSL_read(ssl, buf + total, (int)(max - 1 - total));
    if (n <= 0) break;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n")) break;
  }
  path[0] = '\0';
  const char *sp1 = strchr(buf, ' ');
  if (sp1) {
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 && (size_t)(sp2 - sp1 - 1) < path_max) {
      memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
      path[sp2 - sp1 - 1] = '\0';
    }
  }
}

static void _tls_send_response(SSL *ssl, int status, const char *status_text,
                               const char *body, size_t body_len) {
  char header[256];
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, status_text, body_len);
  if (hlen > 0) SSL_write(ssl, header, hlen);
  if (body && body_len > 0) SSL_write(ssl, body, (int)body_len);
}

static void *_tls_conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  SSL *ssl = SSL_new(g_ssl_ctx);
  if (!ssl) {
    close(fd);
    return NULL;
  }
  SSL_set_fd(ssl, fd);

  if (SSL_accept(ssl) <= 0) {
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  char buf[8192];
  char path[256];
  _tls_read_request(ssl, buf, sizeof(buf), path, sizeof(path));

  if (strcmp(path, "/large") == 0) {
    static char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    _tls_send_response(ssl, 200, "OK", large_body, sizeof(large_body));
  } else {
    _tls_send_response(ssl, 200, "OK", TLS_TEST_BODY, strlen(TLS_TEST_BODY));
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  close(fd);
  return NULL;
}

static void *_tls_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv_running)) {
    int fd = accept(g_srv_fd, NULL, NULL);
    if (fd < 0) {
      if (!atomic_load(&g_srv_running)) break;
      continue;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, _tls_conn_thread,
                       (void *)(intptr_t)fd) != 0) {
      close(fd);
      continue;
    }
    pthread_mutex_lock(&g_conn_mutex);
    if (g_conn_thread_count < MAX_TLS_CONN_THREADS) {
      g_conn_threads[g_conn_thread_count++] = tid;
    } else {
      pthread_detach(tid);
    }
    pthread_mutex_unlock(&g_conn_mutex);
  }
  return NULL;
}

static int _start_tls_server(void) {
  g_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_srv_fd < 0) return -1;
  int opt = 1;
  setsockopt(g_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(g_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(g_srv_fd, 64) != 0) {
    close(g_srv_fd);
    g_srv_fd = -1;
    return -1;
  }

  socklen_t len = sizeof(addr);
  getsockname(g_srv_fd, (struct sockaddr *)&addr, &len);
  g_srv_port = ntohs(addr.sin_port);

  atomic_store(&g_srv_running, 1);
  if (pthread_create(&g_accept_tid, NULL, _tls_accept_loop, NULL) != 0) {
    close(g_srv_fd);
    g_srv_fd = -1;
    atomic_store(&g_srv_running, 0);
    return -1;
  }
  return 0;
}

static void _stop_tls_server(void) {
  if (g_srv_fd < 0) return;
  atomic_store(&g_srv_running, 0);
  shutdown(g_srv_fd, SHUT_RDWR);
  close(g_srv_fd);
  g_srv_fd = -1;
  pthread_join(g_accept_tid, NULL);

  pthread_mutex_lock(&g_conn_mutex);
  for (int i = 0; i < g_conn_thread_count; i++) {
    pthread_join(g_conn_threads[i], NULL);
  }
  g_conn_thread_count = 0;
  pthread_mutex_unlock(&g_conn_mutex);
}

static void _teardown(void) {
  _stop_tls_server();
  if (g_ssl_ctx) {
    SSL_CTX_free(g_ssl_ctx);
    g_ssl_ctx = NULL;
  }
  _remove_generated_cert();
}

__attribute__((constructor)) static void _setup(void) {
  if (_generate_self_signed_cert() != 0) {
    fprintf(stderr,
            "WARNING: could not generate a self-signed cert via the openssl "
            "CLI -- real TLS handshake tests will be skipped in this "
            "environment.\n");
    g_cert_ready = false;
    atexit(_teardown);
    return;
  }

  g_ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!g_ssl_ctx) {
    fprintf(stderr, "FATAL: SSL_CTX_new failed\n");
    exit(1);
  }
  if (SSL_CTX_use_certificate_file(g_ssl_ctx, g_cert_path, SSL_FILETYPE_PEM) <=
          0 ||
      SSL_CTX_use_PrivateKey_file(g_ssl_ctx, g_key_path, SSL_FILETYPE_PEM) <=
          0) {
    fprintf(stderr, "FATAL: could not load generated cert/key into SSL_CTX\n");
    exit(1);
  }

  if (_start_tls_server() != 0) {
    fprintf(stderr, "FATAL: could not start mock TLS server\n");
    exit(1);
  }
  g_cert_ready = true;
  atexit(_teardown);
}

static void make_tls_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "https://127.0.0.1:%d%s", g_srv_port, path);
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

TEST(async_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, TLS_TEST_BODY);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, large_body_response_over_tls) {
  /* Exercises multiple on_data invocations against a single TLS response
   * (fio_tls_connection_read called repeatedly), not just a one-shot read. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, untrusted_cert_fails_verification) {
  /* No ca_bundle_path configured -- the default system trust store, which
   * does not (and cannot) trust a freshly generated throwaway self-signed
   * cert. A real, negative proof that certificate verification is actually
   * being enforced, not silently skipped. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, concurrent_https_requests_all_succeed) {
  /* Multiple concurrent HTTPS requests through the shared async engine --
   * proves the reactor multiplexes several simultaneous TLS handshakes and
   * encrypted data streams correctly, not just one at a time. */
  if (!g_cert_ready) {
    fprintf(stderr, "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  enum { N = 6 };
  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");

  ctpool_future *futures[N];
  for (int i = 0; i < N; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    futures[i] = chttpclient_do_async(cli, req);
    REQUIRE_NE((void *)futures[i], NULL);
    chttp_request_free(req);
  }

  for (int i = 0; i < N; i++) {
    chttpcli_async_result_t *raw = chttpclient_async_result_get(futures[i]);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(futures[i]);
  }

  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}
