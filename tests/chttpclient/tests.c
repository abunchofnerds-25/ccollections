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
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                     MINIMAL HTTP/1.1 TEST SERVER                           */
/* ========================================================================== */

#define TEST_SERVER_BUF 65536
#define TEST_SERVER_PORT 0 /* OS assigns a free port */

typedef struct {
  int server_fd;
  int port;
  pthread_t accept_tid;
  atomic_int running;
} test_server_t;

static test_server_t g_srv;
static pthread_once_t g_srv_once = PTHREAD_ONCE_INIT;
static atomic_int g_slow_started = 0;
static atomic_int g_accept_count = 0;

/* Connection-thread registry so stop_test_server can join them all. */
#define MAX_CONN_THREADS 256
static pthread_t g_conn_threads[MAX_CONN_THREADS];
static int g_conn_thread_count = 0;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Send a complete HTTP response. keep_alive controls whether "Connection:
 * close" is sent -- when true, the response relies on HTTP/1.1's implicit
 * keep-alive default instead. All pre-existing routes pass false, preserving
 * their exact original behavior; only the new keep-alive-specific routes
 * (added for real connection-reuse test coverage) pass true.
 */
static void srv_respond(int fd, int status, const char *status_text,
                        const char *content_type, const char *extra_hdrs,
                        const char *body, size_t body_len, bool keep_alive) {
  char header[2048];
  int hlen =
      snprintf(header, sizeof(header),
               "HTTP/1.1 %d %s\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %zu\r\n"
               "%s"
               "%s"
               "\r\n",
               status, status_text, content_type ? content_type : "text/plain",
               body_len, keep_alive ? "" : "Connection: close\r\n",
               extra_hdrs ? extra_hdrs : "");
  if (hlen > 0) {
    size_t to_send =
        ((size_t)hlen < sizeof(header)) ? (size_t)hlen : sizeof(header) - 1;
    send(fd, header, to_send, 0);
  }
  if (body && body_len > 0) send(fd, body, body_len, 0);
}

/* Find a header value in the raw request text (case-insensitive name). */
static int srv_find_header(const char *raw, const char *name, char *out,
                           size_t out_size) {
  const char *p = raw;
  size_t nlen = strlen(name);
  while (*p) {
    /* Move to start of next header line. */
    const char *eol = strstr(p, "\r\n");
    if (!eol) break;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
      const char *val = p + nlen + 1;
      while (*val == ' ' || *val == '\t') val++;
      size_t vlen = (size_t)(eol - val);
      if (vlen >= out_size) vlen = out_size - 1;
      memcpy(out, val, vlen);
      out[vlen] = '\0';
      return 1;
    }
    p = eol + 2;
  }
  return 0;
}

/* Count how many header lines match name (case-insensitive) in the raw request.
 */
static int srv_count_header(const char *raw, const char *name) {
  int count = 0;
  const char *p = raw;
  size_t nlen = strlen(name);
  while (*p) {
    const char *eol = strstr(p, "\r\n");
    if (!eol) break;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') count++;
    p = eol + 2;
  }
  return count;
}

/* Parse the first line of the request into method and path. */
static void srv_parse_request_line(const char *buf, char *method, size_t mlen,
                                   char *path, size_t plen) {
  const char *p = buf;
  size_t i = 0;
  while (*p && *p != ' ' && i < mlen - 1) method[i++] = *p++;
  method[i] = '\0';
  if (*p == ' ') p++;
  i = 0;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < plen - 1)
    path[i++] = *p++;
  path[i] = '\0';
}

/* Read a full HTTP request (headers + body per Content-Length). */
static ssize_t srv_read_request(int fd, char *buf, size_t max) {
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ssize_t total = 0;
  while (total < (ssize_t)(max - 1)) {
    ssize_t n = recv(fd, buf + total, max - 1 - (size_t)total, 0);
    if (n <= 0) break;
    total += n;
    buf[total] = '\0';

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) continue;

    /* Check if we have the full body. */
    char cl_str[32] = {0};
    long content_length = 0;
    if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
      content_length = atol(cl_str);

    size_t hdr_size = (size_t)(hdr_end - buf) + 4;
    size_t body_read = (size_t)total - hdr_size;
    if (body_read >= (size_t)content_length) break;
  }
  return total;
}

/*
 * Route the request and send a response. Returns true if the connection
 * should be closed after this response, false if the caller (srv_conn_thread)
 * should loop and read another request off the same fd (keep-alive).
 */
static bool srv_handle_route(int conn_fd, const char *method, const char *path,
                             const char *raw, const char *body,
                             size_t body_len) {
  if (strcmp(method, "GET") == 0 && strcmp(path, "/get") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if ((strcmp(method, "POST") == 0 && strcmp(path, "/post") == 0) ||
      (strcmp(method, "PATCH") == 0 && strcmp(path, "/patch") == 0)) {
    /* Echo the request body. */
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, body, body_len,
                false);
    return true;
  }

  if (strcmp(method, "PUT") == 0 && strcmp(path, "/put") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "PUT") == 0 && strcmp(path, "/put-echo") == 0) {
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete") == 0) {
    srv_respond(conn_fd, 204, "No Content", "text/plain", NULL, NULL, 0,
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/headers") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json",
                "X-Chttp-Test: hello\r\n", b, strlen(b), false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/echo-header") == 0) {
    char echo_val[256] = {0};
    srv_find_header(raw, "x-echo", echo_val, sizeof(echo_val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, echo_val,
                strlen(echo_val), false);
    return true;
  }

  /* /status/NNN */
  if (strncmp(path, "/status/", 8) == 0) {
    int code = atoi(path + 8);
    if (code >= 100 && code <= 599) {
      const char *b = "status";
      srv_respond(conn_fd, code, "Status", "text/plain", NULL, b, strlen(b),
                  false);
    } else {
      const char *b = "bad code";
      srv_respond(conn_fd, 400, "Bad Request", "text/plain", NULL, b,
                  strlen(b), false);
    }
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/redirect") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(method, "HEAD") == 0 && strcmp(path, "/head") == 0) {
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, NULL, 0, false);
    return true;
  }

  if (strcmp(method, "OPTIONS") == 0 && strcmp(path, "/options") == 0) {
    srv_respond(conn_fd, 200, "OK", "text/plain",
                "Allow: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS\r\n", NULL,
                0, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/slow") == 0) {
    /* Signal that this request has been received before sleeping so that the
     * test can reliably shrink the pool while curl_easy_perform is in flight.
     */
    atomic_fetch_add(&g_slow_started, 1);
    usleep(100000); /* 100 ms */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/large") == 0) {
    /* Body larger than _write_cb's initial 4096-byte cap to exercise growth. */
    static char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, large_body,
                sizeof(large_body), false);
    return true;
  }

  if (strcmp(path, "/echo-content-type") == 0) {
    /* Respond with the content-type the client sent us (any method). */
    char ct[256] = {0};
    srv_find_header(raw, "content-type", ct, sizeof(ct));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, ct, strlen(ct), false);
    return true;
  }

  if (strcmp(path, "/count-content-type") == 0) {
    /* Respond with the number of Content-Type header lines seen (any method).
     */
    int count = srv_count_header(raw, "content-type");
    char body_buf[8];
    int blen = snprintf(body_buf, sizeof(body_buf), "%d", count);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf,
                (size_t)blen, false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete-echo") == 0) {
    /* Echo the request body for DELETE (body_len is 0 when not sent). */
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/keepalive") == 0) {
    /* No "Connection: close" -- relies on HTTP/1.1's implicit keep-alive
     * default so the client's idle-pool reuse logic can be exercised. */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/keepalive-then-close") == 0) {
    /* Responds as keep-alive-eligible (no Connection: close) but the server
     * closes its end immediately after -- exercises the client's
     * dead-idle-connection detection (liveness probe on reuse). */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return true;
  }

  if (strcmp(path, "/echo-method-body") == 0) {
    /* Echoes "<METHOD>:<body_len>" -- used to verify the redirect-following
     * method/body policy (301/302/303 -> bodyless GET except HEAD; 307/308 ->
     * method and body preserved). */
    char b[64];
    int blen = snprintf(b, sizeof(b), "%s:%zu", method, body_len);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(path, "/redirect-301-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n",
             g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-307-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n",
             g_srv.port);
    srv_respond(conn_fd, 307, "Temporary Redirect", "text/plain", loc_hdr,
                NULL, 0, false);
    return true;
  }

  /* 404 for everything else. */
  const char *b = "not found";
  srv_respond(conn_fd, 404, "Not Found", "text/plain", NULL, b, strlen(b),
              false);
  return true;
}

/*
 * Per-connection handler thread. Loops reading additional requests off the
 * same fd as long as srv_handle_route says the connection should stay open
 * (keep-alive routes), bounded so a misbehaving client can never wedge this
 * thread open forever.
 */
#define MAX_KEEPALIVE_REQUESTS_PER_CONN 100

static void *srv_conn_thread(void *arg) {
  int conn_fd = (int)(intptr_t)arg;
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (!buf) {
    close(conn_fd);
    return NULL;
  }

  for (int iter = 0; iter < MAX_KEEPALIVE_REQUESTS_PER_CONN; iter++) {
    ssize_t n = srv_read_request(conn_fd, buf, TEST_SERVER_BUF);
    if (n <= 0) break;

    char method[16], path[512];
    srv_parse_request_line(buf, method, sizeof(method), path, sizeof(path));

    char *body = NULL;
    size_t body_len = 0;
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) {
      char cl_str[32] = {0};
      long cl = 0;
      if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
        cl = atol(cl_str);
      if (cl > 0) {
        body = hdr_end + 4;
        body_len = (size_t)cl;
      }
    }

    bool close_after = srv_handle_route(conn_fd, method, path, buf, body, body_len);
    if (close_after) break;
  }
  free(buf);
  close(conn_fd);
  return NULL;
}

/* Server accept loop (runs in a background thread). */
static void *srv_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = accept(g_srv.server_fd, NULL, NULL);
    if (conn_fd < 0) break;
    atomic_fetch_add(&g_accept_count, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
    } else {
      pthread_mutex_lock(&g_conn_mutex);
      if (g_conn_thread_count < MAX_CONN_THREADS)
        g_conn_threads[g_conn_thread_count++] = tid;
      else
        pthread_detach(tid); /* registry full: fall back to detach */
      pthread_mutex_unlock(&g_conn_mutex);
    }
  }
  return NULL;
}

static void start_test_server(void) {
  g_srv.server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_srv.server_fd < 0) return;

  int opt = 1;
  setsockopt(g_srv.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(TEST_SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(g_srv.server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(g_srv.server_fd, 64) != 0) {
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    return;
  }

  /* Retrieve the assigned port. */
  socklen_t len = sizeof(addr);
  getsockname(g_srv.server_fd, (struct sockaddr *)&addr, &len);
  g_srv.port = ntohs(addr.sin_port);
  atomic_store(&g_srv.running, 1);

  if (pthread_create(&g_srv.accept_tid, NULL, srv_accept_loop, NULL) != 0) {
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    atomic_store(&g_srv.running, 0);
    return;
  }
}

__attribute__((destructor)) static void stop_test_server(void) {
  if (g_srv.server_fd > 0) {
    atomic_store(&g_srv.running, 0);
    /* shutdown() causes a blocked accept() to return with EINVAL on Linux;
     * close() alone does not reliably unblock it. */
    shutdown(g_srv.server_fd, SHUT_RDWR);
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    pthread_join(g_srv.accept_tid, NULL);
  }
  /* Join all connection-handling threads so sanitizers can account for
   * every allocation made on their stacks and no thread is left running. */
  pthread_mutex_lock(&g_conn_mutex);
  int n = g_conn_thread_count;
  pthread_mutex_unlock(&g_conn_mutex);
  for (int i = 0; i < n; i++) pthread_join(g_conn_threads[i], NULL);
}

static int get_test_port(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port;
}

/* Number of TCP connections accepted so far by the test server -- used to
 * assert that keep-alive reuse actually skipped the handshake/TCP setup for
 * a given request, rather than opening a fresh connection. */
static int test_server_accept_count(void) {
  return atomic_load(&g_accept_count);
}

/* Build a URL for the test server: http://127.0.0.1:<port><path>. */
static void make_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://127.0.0.1:%d%s", get_test_port(), path);
}

/* ========================================================================== */
/*                     HELPER UTILITIES                                       */
/* ========================================================================== */

/* ========================================================================== */
/*                     REQUEST CONSTRUCTION TESTS                             */
/* ========================================================================== */

TEST(request, new_copies_url_and_body) {
  const char *json = "{\"x\":1}";
  chttp_request_body_t body = CHTTP_JSON_BODY(json, strlen(json));

  chttp_request_t *req =
      chttp_request_new(CHTTP_POST, "http://example.com/api", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_STREQ(req->url, "http://example.com/api");
  REQUIRE_EQ(req->method, CHTTP_POST);
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, strlen(json));
  REQUIRE_STREQ(req->body.content_type, "application/json");
  /* Verify it's an independent copy. */
  REQUIRE_NE((void *)req->body.data, (void *)json);

  chttp_request_free(req);
}

TEST(request, new_null_body) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  chttp_request_free(req);
}

TEST(request, new_no_body_macro) {
  chttp_request_body_t nb = CHTTP_NO_BODY;
  chttp_request_t *req =
      chttp_request_new(CHTTP_DELETE, "http://example.com/r", &nb, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  chttp_request_free(req);
}

TEST(request, new_null_url_fails) {
  char *err = NULL;
  chttp_request_t *req =
      chttp_request_new_mp(CHTTP_GET, NULL, NULL, NULL, &err);
  REQUIRE_EQ((void *)req, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(request, set_and_get_header) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval_t rv =
      chttp_request_set_header(req, "Authorization", "Bearer token123");
  REQUIRE_EQ(rv, ccol_success);

  const char *val = chttp_request_get_header(req, "Authorization");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "Bearer token123");

  chttp_request_free(req);
}

TEST(request, header_lookup_case_insensitive) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "Content-Type", "application/json");

  /* Lookup with different casings. */
  REQUIRE_NE((void *)chttp_request_get_header(req, "content-type"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "CONTENT-TYPE"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "Content-Type"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "content-type"),
                "application/json");

  chttp_request_free(req);
}

TEST(request, set_header_overwrites) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "X-Custom", "first");
  chttp_request_set_header(req, "X-Custom", "second");

  const char *val = chttp_request_get_header(req, "x-custom");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "second");

  chttp_request_free(req);
}

TEST(request, get_missing_header_returns_null) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X-Missing"), NULL);
  chttp_request_free(req);
}

TEST(request, headers_begin_iterator) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "X-A", "1");
  chttp_request_set_header(req, "X-B", "2");
  chttp_request_set_header(req, "X-C", "3");

  int count = 0;

  ccol_for_each(req->headers, it, {
    REQUIRE_NE(*ccol_iter_key_ptr(it), NULL);
    REQUIRE_NE(*ccol_iter_val_ptr(it), NULL);
    count++;
  });

  REQUIRE_EQ(count, 3);

  chttp_request_free(req);
}

TEST(request, headers_begin_empty_returns_null) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)ccol_begin(req->headers), NULL);
  chttp_request_free(req);
}

TEST(request, free_null_safe) { chttp_request_free(NULL); }

/* ========================================================================== */
/*                     CLIENT CONSTRUCTION TESTS                              */
/* ========================================================================== */

TEST(client_construction, create_and_destroy) {
  char *err = NULL;
  chttpcli cli = create_chttpclient(&err);
  REQUIRE_NE((void *)cli, NULL);
  chttpclient_destroy(cli);
  REQUIRE_EQ((void *)cli, NULL);
}

TEST(client_construction, construct_macro) {
  chttpcli_construct(cli);
  REQUIRE_NE((void *)cli, NULL);
  chttpclient_destroy(cli);
}

TEST(client_construction, construct_scoped_macro) {
  {
    chttpcli_construct_scoped(cli);
    REQUIRE_NE((void *)cli, NULL);
  }
  /* cli auto-destroyed on scope exit; nothing to assert but valgrind checks. */
}

TEST(client_construction, declare_and_init) {
  chttpcli_declare(cli);
  char *err = NULL;
  cli = create_chttpclient(&err);
  REQUIRE_NE((void *)cli, NULL);
  chttpclient_destroy(cli);
}

TEST(client_construction, set_configuration) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 5000), ccol_success);
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 30000), ccol_success);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);
  chttpclient_destroy(cli);
}

TEST(client_construction, null_args) {
  REQUIRE_EQ(chttpclient_set_pool_size(NULL, 4), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_connect_timeout(NULL, 0), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_request_timeout(NULL, 0), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_tls(NULL, NULL), ccol_invalid_args);
}

/* ========================================================================== */
/*                     HTTP TRAFFIC TESTS                                     */
/* ========================================================================== */

TEST(http, get_200) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  chttpclient_resp_free(resp);
}

TEST(http, post_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"hello\":\"world\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(http, post_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

TEST(http, put_200) {
  char url[128];
  make_url(url, sizeof(url), "/put");

  const char *data = "updated";
  chttp_request_body_t body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_put(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, delete_204) {
  char url[128];
  make_url(url, sizeof(url), "/delete");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_delete(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 204);
  chttpclient_resp_free(resp);
}

TEST(http, patch_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  const char *data = "patch-data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_patch(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, data);
  chttpclient_resp_free(resp);
}

TEST(http, patch_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_patch(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

TEST(http, response_headers) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *val = chttpclient_resp_header(resp, "X-Chttp-Test");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "hello");

  chttpclient_resp_free(resp);
}

TEST(http, response_header_lookup_case_insensitive) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);

  /* Server sends X-Chttp-Test; we store it lowercase so any casing resolves. */
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "x-chttp-test"), NULL);
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "X-CHTTP-TEST"), NULL);
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "X-Chttp-Test"), NULL);

  chttpclient_resp_free(resp);
}

TEST(http, request_headers_forwarded) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Echo", "test-value-42");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "test-value-42");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, not_found_404) {
  char url[128];
  make_url(url, sizeof(url), "/no-such-route");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(http, status_route_custom_code) {
  char url[128];
  make_url(url, sizeof(url), "/status/418");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 418);
  chttpclient_resp_free(resp);
}

TEST(http, follows_redirect) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  /* libcurl follows the 301 to /get which returns 200. */
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, redirect_does_not_leak_intermediate_headers) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  /* The 301 response carries a Location header.  After the redirect the final
   * response is a 200 from /get which has no Location header.  Before the fix
   * the headers map was never reset between responses, so "location" would
   * bleed through from the intermediate 301 into the final response. */
  REQUIRE_EQ((void *)chttpclient_resp_header(resp, "location"), NULL);

  /* The final response must still expose its own headers. */
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "content-type"), NULL);

  chttpclient_resp_free(resp);
}

TEST(http, default_client_convenience) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli def = chttp_default_client();
  REQUIRE_NE((void *)def, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(def, NULL, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  rv = chttpclient_do(def, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, custom_client_with_pool_size) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     STREAMING RESPONSE TEST                                */
/* ========================================================================== */

typedef struct {
  char buf[4096];
  size_t len;
} stream_sink_t;

static size_t stream_sink_write(const void *data, size_t len, void *ctx) {
  stream_sink_t *s = (stream_sink_t *)ctx;
  size_t copy = len;
  if (s->len + copy >= sizeof(s->buf) - 1) copy = sizeof(s->buf) - 1 - s->len;
  memcpy(s->buf + s->len, data, copy);
  s->len += copy;
  s->buf[s->len] = '\0';
  return len;
}

TEST(http, streaming_response) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  int status = 0;
  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

TEST(http, streaming_null_args) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  REQUIRE_EQ(
      chttpclient_do_streaming(NULL, req, stream_sink_write, &sink, &status),
      ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_streaming(chttp_default_client(), NULL,
                                      stream_sink_write, &sink, &status),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_streaming(chttp_default_client(), req, NULL, &sink,
                                      &status),
             ccol_invalid_args);
  chttp_request_free(req);
}

/* ========================================================================== */
/*                     RESPONSE HEADER ITERATION TEST                         */
/* ========================================================================== */

TEST(http, response_header_iteration) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);

  int count = 0;
  bool found_test_header = false;

  ccol_for_each(resp->headers, it, {
    REQUIRE_NE(*ccol_iter_key_ptr(it), NULL);
    REQUIRE_NE(*ccol_iter_val_ptr(it), NULL);
    if (strcmp(*ccol_iter_key_ptr(it), "x-chttp-test") == 0 &&
        strcmp(*ccol_iter_val_ptr(it), "hello") == 0)
      found_test_header = true;
    count++;
  });

  REQUIRE_TRUE(found_test_header);
  REQUIRE_GT(count, 0);

  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     CONCURRENT REQUESTS TEST                               */
/* ========================================================================== */

typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval_t result_rv;
} concurrent_req_arg_t;

static void *concurrent_req_thread(void *arg) {
  concurrent_req_arg_t *a = (concurrent_req_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }

  chttpcli_response *resp = NULL;
  a->result_rv = chttpclient_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

/*
 * probe_arg_t / probe_thread: used by the "not_permitted_when_destroying" test
 * to synchronise a caller that races __chttpclient_destroy.  ready=1 is set
 * immediately before chttpclient_do is called (not after entering it), so the
 * window between the signal and the actual call is minimal but non-zero.
 * After ready=1 the probe will either (a) block in _pool_acquire's
 * cond_var_wait because both pool slots are in-use and get woken by the
 * destroy broadcast, or (b) see destroying=true at the fast-path guard if the
 * destroy thread wins the race.  Both paths return ccol_not_permitted, so the
 * test is correct in either case.
 */
typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval_t result_rv;
  atomic_int ready;
} probe_arg_t;

static void *probe_thread(void *arg) {
  probe_arg_t *a = (probe_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }
  atomic_store(&a->ready, 1);
  chttpcli_response *resp = NULL;
  a->result_rv = chttpclient_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

static void *do_destroy_thread(void *arg) {
  __chttpclient_destroy((chttpcli)arg);
  return NULL;
}

TEST(http, concurrent_requests) {
  enum { NTHREADS = 8 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, NTHREADS), ccol_success);

  concurrent_req_arg_t args[NTHREADS];
  pthread_t threads[NTHREADS];

  for (int i = 0; i < NTHREADS; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]), 0);
  }

  for (int i = 0; i < NTHREADS; i++) {
    pthread_join(threads[i], NULL);
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     POOL RESIZE TESTS                                      */
/* ========================================================================== */

TEST(pool, grow_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 1), ccol_success);

  /* First request initialises the pool at size 1. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Grow pool to 4. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* Second request should work fine. */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(http, head_200) {
  char url[128];
  make_url(url, sizeof(url), "/head");

  chttp_request_t *req = chttp_request_new(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* HEAD responses carry no body; write callback is never invoked. */
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, options_200) {
  char url[128];
  make_url(url, sizeof(url), "/options");

  chttp_request_t *req = chttp_request_new(CHTTP_OPTIONS, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *allow = chttpclient_resp_header(resp, "allow");
  REQUIRE_NE((void *)allow, NULL);
  REQUIRE_STREQ(allow, "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, large_body_response) {
  char url[128];
  make_url(url, sizeof(url), "/large");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Server sends 8192 bytes; this exercises _write_cb buffer growth past the
   * initial 4096-byte capacity. */
  REQUIRE_EQ(resp->body_len, (size_t)8192);
  REQUIRE_NE((void *)resp->body, NULL);
  /* Spot-check: every byte should be 'x'. */
  REQUIRE_EQ((int)resp->body[0], (int)'x');
  REQUIRE_EQ((int)resp->body[4095], (int)'x');
  REQUIRE_EQ((int)resp->body[8191], (int)'x');

  chttpclient_resp_free(resp);
}

TEST(http, content_type_auto_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  /* Set body.content_type but no explicit Content-Type header. */
  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Server echoes back the content-type it received. */
  REQUIRE_STREQ(resp->body, "application/json");

  chttpclient_resp_free(resp);
}

TEST(http, content_type_explicit_header_not_duplicated) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "text";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  /* Explicit header overrides auto-injection; server should see exactly this.
   */
  chttp_request_set_header(req, "Content-Type", "text/csv");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/csv");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, streaming_post) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"stream\":true}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  /* Server echoes the body back; streaming sink must have captured it. */
  REQUIRE_EQ(sink.len, strlen(payload));
  REQUIRE_STREQ(sink.buf, payload);

  chttp_request_free(req);
}

TEST(http, streaming_post_content_type_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  /* Server echoes back the content-type we sent; verifies injection in the
   * streaming path mirrors the buffered-response path. */
  REQUIRE_STREQ(sink.buf, "application/json");

  chttp_request_free(req);
}

TEST(http, streaming_null_status_code_out) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ccol_retval_t rv = chttpclient_do_streaming(chttp_default_client(), req,
                                              stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

/* ========================================================================== */
/*                     POOL RESIZE TESTS (continued)                          */
/* ========================================================================== */

TEST(pool, shrink_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* First request initialises the pool at size 4. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Shrink pool to 2; in-flight slots complete in the old range. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  /* Subsequent requests must still succeed (using slots 0 and 1). */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(pool, idle_handles_freed_on_shrink) {
  /* Grow the pool so multiple slots acquire live CURL handles, then shrink.
   * The fix must release idle handles immediately; valgrind verifies no leak.
   */
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_LARGE), ccol_success);

  /* Fire POOL_LARGE concurrent requests so every slot initializes a handle. */
  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]), 0);
  }
  for (int i = 0; i < POOL_LARGE; i++) {
    pthread_join(threads[i], NULL);
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  /* After all requests complete, slots [POOL_SMALL, POOL_LARGE) are idle with
   * live CURL handles.  Shrinking must free them immediately. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_SMALL), ccol_success);

  /* Pool must still work at the new size. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(pool, shrink_while_in_flight_exiles_slots) {
  /* Verify the exiled-slot path in _pool_release_slot: start POOL_LARGE
   * concurrent requests, shrink the pool to POOL_SMALL while all requests are
   * in-flight, then wait for completion.  Slots [POOL_SMALL, POOL_LARGE) become
   * exiled -- _pool_release_slot must call curl_easy_cleanup on them and must
   * not corrupt any state.  Valgrind verifies that no handles are leaked. */
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/slow");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_LARGE), ccol_success);

  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]), 0);
  }

  /* Spin until the test server has received all POOL_LARGE requests.  At that
   * point each client thread is inside curl_easy_perform and its pool slot is
   * marked in_use, so the shrink below will exile slots [POOL_SMALL,
   * POOL_LARGE). */
  while (atomic_load(&g_slow_started) < POOL_LARGE) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */
    nanosleep(&ts, NULL);
  }

  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_SMALL), ccol_success);

  for (int i = 0; i < POOL_LARGE; i++) {
    pthread_join(threads[i], NULL);
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  /* Pool must still be usable at the reduced size after the exiled slots
   * have been cleaned up. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     INVALID ARGUMENT TESTS                                 */
/* ========================================================================== */

TEST(invalid_args, chttpclient_do_null_checks) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(NULL, req, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(invalid_args, chttp_convenience_null_checks) {
  chttpcli_response *resp = NULL;
  /* NULL url */
  REQUIRE_EQ(chttp_get(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_post(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_put(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch(NULL, NULL, &resp), ccol_invalid_args);
  /* NULL resp_out */
  REQUIRE_EQ(chttp_get("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_post("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_put("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch("http://x.com/", NULL, NULL), ccol_invalid_args);
}

TEST(invalid_args, resp_header_null_checks) {
  REQUIRE_EQ((void *)chttpclient_resp_header(NULL, "X-Foo"), NULL);
  chttpclient_resp_free(NULL);
}

TEST(invalid_args, request_set_header_null_checks) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://x.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(NULL, "X", "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, NULL, "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "X", NULL), ccol_invalid_args);
  chttp_request_free(req);
}

/* ========================================================================== */
/*                     CUSTOM ALLOCATOR TEST                                  */
/* ========================================================================== */

static atomic_int g_alloc_count = 0;
static atomic_int g_free_count = 0;

static void *tracked_malloc(size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return malloc(sz);
}
static void tracked_free(void *p) {
  if (p) atomic_fetch_add(&g_free_count, 1);
  free(p);
}
static void *tracked_calloc(size_t n, size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return calloc(n, sz);
}
static void *tracked_realloc(void *p, size_t sz) {
  /* Model realloc as free(old) + malloc(new) so the counters stay balanced. */
  if (p) atomic_fetch_add(&g_free_count, 1);
  atomic_fetch_add(&g_alloc_count, 1);
  return realloc(p, sz);
}

TEST(custom_allocator, allocations_go_through_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};

  /* Test request with custom allocator. */
  char *err = NULL;
  const char *url = "http://example.com/";
  chttp_request_t *req = chttp_request_new_mp(CHTTP_GET, url, NULL, &mp, &err);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Custom", "value");
  REQUIRE_NE((void *)chttp_request_get_header(req, "x-custom"), NULL);
  chttp_request_free(req);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));

  /* Test client with custom allocator. */
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  err = NULL;
  chttpcli cli = create_chttpclient_mp(&mp, &err);
  REQUIRE_NE((void *)cli, NULL);
  chttpclient_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* ========================================================================== */
/*                     PUT / PATCH CONTENT-TYPE TESTS                         */
/* ========================================================================== */

TEST(http, put_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/plain");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  /* Build a PUT body with content_type = NULL to verify that the upload
   * interface does not silently inject Content-Type: application/x-www-form-
   * urlencoded (which CURLOPT_POSTFIELDS would have done). */
  const char *payload = "raw";
  chttp_request_body_t body = {.data = payload, .len = 3, .content_type = NULL};

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Server echoes the content-type; with none sent the body is empty. */
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_body_round_trip) {
  char url[128];
  make_url(url, sizeof(url), "/put-echo");

  const char *payload = "hello-put";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_without_body) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, patch_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "delta";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "application/json");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, patch_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "patch";
  chttp_request_body_t body = {.data = payload, .len = 5, .content_type = NULL};

  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     TLS CONFIGURATION TEST                                 */
/* ========================================================================== */

TEST(tls, set_tls_deep_copies_strings) {
  /* Verify that chttpclient_set_tls owns copies of the path strings.
   * Valgrind (memtest) will flag use-after-free if the fix is absent. */
  chttpcli_construct(cli);

  {
    char cert[64], key[64], ca[64];
    memcpy(cert, "/tmp/test.crt", sizeof("/tmp/test.crt"));
    memcpy(key, "/tmp/test.key", sizeof("/tmp/test.key"));
    memcpy(ca, "/tmp/ca.pem", sizeof("/tmp/ca.pem"));

    chttp_tls_config_t tls = {
        .cert_path = cert,
        .key_path = key,
        .ca_bundle_path = ca,
        .verify_peer = true,
        .verify_host = true,
    };
    REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

    /* Poison the caller's stack buffers; owned copies must not be affected. */
    memset(cert, 0xff, sizeof(cert));
    memset(key, 0xff, sizeof(key));
    memset(ca, 0xff, sizeof(ca));
  }

  /* Set again to exercise the free-old-copy path, then restore to defaults. */
  chttp_tls_config_t tls2 = {
      .cert_path = "/tmp/other.crt",
      .key_path = "/tmp/other.key",
      .ca_bundle_path = NULL,
      .verify_peer = false,
      .verify_host = false,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls2), ccol_success);
  REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     POOL SIZE = 0 (CPU COUNT) TEST                         */
/* ========================================================================== */

TEST(pool, set_pool_size_zero_uses_cpu_count) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  /* 0 must resolve to the CPU count (> 0), not actually set the pool to 0. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 0), ccol_success);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(pool, set_pool_size_zero_after_initialization) {
  /* Verify that passing 0 (CPU count) to chttpclient_set_pool_size after the
   * pool has already been initialised correctly resolves and resizes the pool
   * (either grow or shrink depending on the current CPU count vs pool_cap). */
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* First request initialises the pool at size 4. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Resize to CPU count (0) now that pool_initialized == true. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 0), ccol_success);

  /* Pool must still serve requests after the resize. */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     BODY MACRO TESTS                                       */
/* ========================================================================== */

TEST(body_macros, chttp_body) {
  const char *data = "raw";
  chttp_request_body_t b = CHTTP_BODY(data, 3, "text/html");
  REQUIRE_EQ((void *)b.data, (void *)data);
  REQUIRE_EQ(b.len, (size_t)3);
  REQUIRE_STREQ(b.content_type, "text/html");
}

TEST(body_macros, chttp_json_body) {
  const char *data = "{}";
  chttp_request_body_t b = CHTTP_JSON_BODY(data, 2);
  REQUIRE_STREQ(b.content_type, "application/json");
  REQUIRE_EQ(b.len, (size_t)2);
}

TEST(body_macros, chttp_text_body) {
  const char *data = "hello";
  chttp_request_body_t b = CHTTP_TEXT_BODY(data, 5);
  REQUIRE_STREQ(b.content_type, "text/plain");
}

TEST(body_macros, chttp_form_body) {
  const char *data = "a=1&b=2";
  chttp_request_body_t b = CHTTP_FORM_BODY(data, strlen(data));
  REQUIRE_STREQ(b.content_type, "application/x-www-form-urlencoded");
}

TEST(body_macros, chttp_no_body) {
  chttp_request_body_t b = CHTTP_NO_BODY;
  REQUIRE_EQ((void *)b.data, NULL);
  REQUIRE_EQ(b.len, (size_t)0);
  REQUIRE_EQ((void *)b.content_type, NULL);
}

/* ========================================================================== */
/*                     METHOD STRING TEST                                     */
/* ========================================================================== */

TEST(method, chttp_method_str) {
  REQUIRE_STREQ(chttp_method_str(CHTTP_GET), "GET");
  REQUIRE_STREQ(chttp_method_str(CHTTP_POST), "POST");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PUT), "PUT");
  REQUIRE_STREQ(chttp_method_str(CHTTP_DELETE), "DELETE");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PATCH), "PATCH");
  REQUIRE_STREQ(chttp_method_str(CHTTP_HEAD), "HEAD");
  REQUIRE_STREQ(chttp_method_str(CHTTP_OPTIONS), "OPTIONS");
}

/* ========================================================================== */
/*                     RUN QUERY TESTS                                        */
/* ========================================================================== */

TEST(http, run_query_null_headers) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_post_with_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"run\":\"query\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_headers_forwarded_and_not_consumed) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-echo", "hello-from-run-query");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello-from-run-query");
  chttpclient_resp_free(resp);

  /* hdrs must survive the call -- chttp_run_query must not take ownership. */
  const char *stored = chmap_get(hdrs, "x-echo");
  REQUIRE_NE((void *)stored, NULL);
  REQUIRE_STREQ(stored, "hello-from-run-query");
  chmap_destroy(hdrs);
}

TEST(http, run_query_invalid_args) {
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, NULL, NULL, NULL, &resp),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, "http://x/", NULL, NULL, NULL),
             ccol_invalid_args);
}

/* ========================================================================== */
/*                     CLIENT DESTRUCTION TESTS                               */
/* ========================================================================== */

TEST(pool, do_returns_not_permitted_when_destroying) {
  /* Strategy: fill ALL pool slots with 100 ms in-flight requests so that a
   * subsequent chttpclient_do MUST block in _pool_acquire's cond_var_wait.
   * Once both slots are confirmed in-flight we start a probe thread (which
   * blocks on the full pool) and only then start the destroy thread.  The
   * destroy sets destroying=true and broadcasts, waking the probe which sees
   * the flag and returns ccol_not_permitted -- no arbitrary sleep required. */
  char slow_url[128], url[128];
  make_url(slow_url, sizeof(slow_url), "/slow");
  make_url(url, sizeof(url), "/get");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  /* Fill both slots with 100 ms slow requests. */
  concurrent_req_arg_t slow_args[2];
  pthread_t slow_tids[2];
  for (int i = 0; i < 2; i++) {
    slow_args[i].cli = cli;
    memcpy(slow_args[i].url, slow_url, sizeof(slow_url));
    slow_args[i].result_status = 0;
    slow_args[i].result_rv = ccol_unexpected_failure;
    REQUIRE_EQ(pthread_create(&slow_tids[i], NULL, concurrent_req_thread,
                              &slow_args[i]),
               0);
  }

  /* Spin until both requests are confirmed in-flight by the server side. */
  while (atomic_load(&g_slow_started) < 2) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&ts, NULL);
  }

  /* Launch the probe thread.  Both pool slots are in_use, so chttpclient_do
   * will either block in _pool_acquire or see destroying=true at the fast-path
   * guard.  We spin on probe.ready, which is set immediately before the
   * chttpclient_do call, to minimise the window before destruction begins. */
  probe_arg_t probe;
  probe.cli = cli;
  memcpy(probe.url, url, sizeof(url));
  probe.result_status = 0;
  probe.result_rv = ccol_unexpected_failure;
  atomic_store(&probe.ready, 0);
  pthread_t probe_tid;
  REQUIRE_EQ(pthread_create(&probe_tid, NULL, probe_thread, &probe), 0);

  while (!atomic_load(&probe.ready)) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000}; /* 0.1 ms */
    nanosleep(&ts, NULL);
  }

  /* Now start destroying.  __chttpclient_destroy sets destroying=true,
   * broadcasts (waking the probe), then waits for in_flight_count == 0.
   * The probe sees destroying==true and returns ccol_not_permitted. */
  pthread_t destroy_tid;
  REQUIRE_EQ(pthread_create(&destroy_tid, NULL, do_destroy_thread, (void *)cli),
             0);

  pthread_join(probe_tid, NULL);
  REQUIRE_EQ(probe.result_rv, ccol_not_permitted);
  REQUIRE_EQ(probe.result_status, 0);

  /* Let the slow requests complete so the destroy thread can finish. */
  for (int i = 0; i < 2; i++) {
    pthread_join(slow_tids[i], NULL);
    REQUIRE_EQ(slow_args[i].result_rv, ccol_success);
    REQUIRE_EQ(slow_args[i].result_status, 200);
  }
  pthread_join(destroy_tid, NULL);
  /* cli has been freed by do_destroy_thread; do not call chttpclient_destroy.
   */
}

/* ========================================================================== */
/*                     NETWORK ERROR CODE TESTS                               */
/* ========================================================================== */

TEST(error_codes, unsupported_scheme_returns_invalid_url) {
  /* libcurl returns CURLE_UNSUPPORTED_PROTOCOL for unknown URL schemes, which
   * maps to ccol_http_invalid_url. */
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("ccol-not-a-scheme://example.com/", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(error_codes, host_resolution_failure) {
  /* The .invalid TLD is reserved (RFC 6761) and guaranteed not to resolve.
   * Use a short timeout so the test does not stall on a slow resolver. */
  chttpcli_construct(cli);
  chttpclient_set_request_timeout(cli, 5000);

  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, "http://this-host-will-not-resolve.invalid.ccol.test/", NULL,
      NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_host_resolution_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(error_codes, connection_refused) {
  /* Bind to a port, note it, then close immediately so nothing is listening.
   * A connection attempt will be refused (RST) by the kernel. */
  int s = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_GT(s, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(0);
  bind(s, (struct sockaddr *)&a, sizeof(a));
  socklen_t alen = sizeof(a);
  getsockname(s, (struct sockaddr *)&a, &alen);
  int closed_port = ntohs(a.sin_port);
  close(s);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/", closed_port);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_connection_failed);
  REQUIRE_EQ((void *)resp, NULL);
}

/* ========================================================================== */
/*                     KEEP-ALIVE / IDLE POOL TESTS                           */
/*                                                                            */
/* These exercise mechanics that did not exist under the old libcurl-backed  */
/* implementation: the client's own hand-rolled connection reuse.           */
/* ========================================================================== */

TEST(keepalive, sequential_requests_reuse_connection) {
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
  }

  /* Give the server's own accept-count increment a brief moment: it happens
   * on the accept()-side thread, not synchronously with the client's read of
   * the response. */
  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli);
}

TEST(keepalive, dead_connection_detected_and_reconnects) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  chttp_request_t *req1 = chttp_request_new(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval_t rv1 = chttpclient_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);

  /* The server closed its end after that response; a naive pool would try to
   * reuse the now-dead connection here and fail. The client's liveness probe
   * must detect this and transparently reconnect. */
  chttp_request_t *req2 = chttp_request_new(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  chttpcli_response *resp2 = NULL;
  ccol_retval_t rv2 = chttpclient_do(cli, req2, &resp2);
  chttp_request_free(req2);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpclient_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
}

TEST(keepalive, concurrent_requests_exceeding_idle_cap_no_crash) {
  /* Fires more concurrent keep-alive requests than the idle pool's
   * per-origin cap can hold; excess connections simply are not pooled
   * (documented, intentional v1 simplification) rather than causing any
   * crash, leak, or failure. */
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  chttpclient_set_pool_size(cli, 16);

  const int n = 12;
  pthread_t threads[12];
  concurrent_req_arg_t args[12];
  for (int i = 0; i < n; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]);
  }
  for (int i = 0; i < n; i++) pthread_join(threads[i], NULL);
  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     URL PARSER EDGE CASES                                  */
/* ========================================================================== */

TEST(url_parsing, missing_host_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http:///get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, non_numeric_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:abc/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, port_out_of_range_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:99999/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, zero_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:0/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, explicit_non_default_port_succeeds) {
  /* The whole test suite already runs against a non-default (OS-assigned)
   * port, but this makes the "explicit port is parsed and connected to
   * correctly" property an explicit, named assertion. */
  char url[128];
  make_url(url, sizeof(url), "/get");
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     REDIRECT METHOD/BODY POLICY                            */
/* ========================================================================== */

TEST(redirect_policy, post_301_becomes_bodyless_get) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");
  chttpclient_resp_free(resp);
}

TEST(redirect_policy, post_307_preserves_method_and_body) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     STREAMING ABORT TEST                                   */
/* ========================================================================== */

static size_t abort_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)len;
  (void)ctx;
  /* Returning 0 when libcurl expects len > 0 signals an abort,
   * raising CURLE_WRITE_ERROR -> ccol_http_transfer_aborted. */
  return 0;
}

TEST(http, streaming_write_fn_abort_returns_transfer_aborted) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  int status = 0;
  ccol_retval_t rv = chttpclient_do_streaming(chttp_default_client(), req,
                                              abort_write_fn, NULL, &status);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
}

/* ========================================================================== */
/*                     CUSTOM ALLOCATOR RESPONSE LIFETIME TEST                */
/* ========================================================================== */

TEST(custom_allocator, response_freed_before_client_with_custom_alloc) {
  /* Verifies that: (a) the response is allocated via the client's custom
   * allocator, and (b) freeing the response before destroying the client
   * produces a balanced alloc/free count. */
  char url[128];
  make_url(url, sizeof(url), "/get");

  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  chttpcli cli = create_chttpclient_mp(&mp, &err);
  REQUIRE_NE((void *)cli, NULL);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttp_request_free(req);

  /* Free response BEFORE client, as required by the contract when a custom
   * allocator is in use (resp->_m_procs borrows the client's allocator). */
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* ========================================================================== */
/*                     BORROWED HEADER MAP CONTENT-TYPE TESTS                 */
/* ========================================================================== */

TEST(http, run_query_borrowed_map_mixed_case_ct_no_duplication) {
  /* Regression test for Bug 2: _build_req_headers used a case-sensitive map
   * lookup for "content-type", so a borrowed chmap with a key like
   * "Content-Type" (mixed case) was not detected, and auto-injection from
   * body.content_type added a second Content-Type header.
   *
   * The fix uses a case-insensitive linear scan (_map_has_content_type) so
   * that the existing header is found regardless of the key's case. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "text/csv"); /* mixed-case key */

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Exactly one Content-Type header must reach the server; auto-injection must
   * have been suppressed because the borrowed map already had one. */
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_lowercase_ct_no_duplication) {
  /* Same scenario as above but with a lowercase key -- verifies the common
   * (non-borrowed) path still works correctly. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "content-type", "text/csv");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, patch_borrowed_map_mixed_case_ct_suppresses_libcurl_injection) {
  /* PATCH uses CURLOPT_POSTFIELDS, which causes libcurl to auto-inject
   * "Content-Type: application/x-www-form-urlencoded" unless we suppress it
   * via "Content-Type:".  _build_req_headers must detect a mixed-case
   * "Content-Type" in the borrowed map as "has CT" and skip the suppressor,
   * leaving the user's header as the sole Content-Type. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "application/json");

  const char *payload = "patch";
  chttp_request_body_t body = {.data = payload, .len = 5, .content_type = NULL};

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_PATCH, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     ZERO-LENGTH BODY TESTS                                 */
/* ========================================================================== */

TEST(request, body_data_non_null_but_zero_len_treated_as_no_body) {
  /* body.data != NULL with body.len == 0 must not be copied into the request;
   * the condition (body->data && body->len > 0) must gate the copy. */
  const char *data = "ignored";
  chttp_request_body_t body = {
      .data = data, .len = 0, .content_type = "text/plain"};

  chttp_request_t *req =
      chttp_request_new(CHTTP_POST, "http://example.com/", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  /* content_type is only copied when there is an actual body. */
  REQUIRE_EQ((void *)req->body.content_type, NULL);
  chttp_request_free(req);
}

TEST(http, post_zero_len_body_no_data_transmitted) {
  /* A POST whose body struct has data != NULL but len == 0 must be treated as
   * bodyless: no body data is copied into the request and none is transmitted.
   * The server echoes back whatever body it received; it must be empty. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttp_request_body_t body = {
      .data = "ignored", .len = 0, .content_type = "text/plain"};

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Server echoes the received body bytes; must be zero because nothing was
   * sent. */
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     DELETE WITH BODY TEST                                  */
/* ========================================================================== */

TEST(http, delete_body_not_transmitted) {
  /* DELETE uses CURLOPT_CUSTOMREQUEST without configuring a body upload, so
   * body data stored in the request struct is NOT transmitted to the server.
   * This test documents that current behavior and guards against regressions
   * that would accidentally start sending it. */
  char url[128];
  make_url(url, sizeof(url), "/delete-echo");

  chttp_request_body_t body = CHTTP_TEXT_BODY("payload", 7);
  chttp_request_t *req = chttp_request_new(CHTTP_DELETE, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  /* Body is stored in the request struct. */
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)7);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server echoes whatever body it received; it must be empty. */
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}
