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

  /* IPv6 loopback listener, mirroring the fields above; best-effort: not
   * every sandbox/CI environment has an IPv6 stack, so server_fd6 stays -1
   * (and port6 stays 0) when the bind fails, rather than treating that as a
   * hard test-server-setup failure. */
  int server_fd6;
  int port6;
  pthread_t accept_tid6;
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
 * close" is sent; when true, the response relies on HTTP/1.1's implicit
 * keep-alive default instead. All pre-existing routes pass false, preserving
 * their exact original behavior; only the new keep-alive-specific routes
 * (added for real connection-reuse test coverage) pass true.
 */
static void srv_respond(int fd, int status, const char *status_text,
                        const char *content_type, const char *extra_hdrs,
                        const char *body, size_t body_len, bool keep_alive) {
  char header[2048];
  int hlen = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "%s"
      "%s"
      "\r\n",
      status, status_text, content_type ? content_type : "text/plain", body_len,
      keep_alive ? "" : "Connection: close\r\n", extra_hdrs ? extra_hdrs : "");
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
    srv_respond(conn_fd, 204, "No Content", "text/plain", NULL, NULL, 0, false);
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

  if (strcmp(method, "GET") == 0 && strcmp(path, "/echo-auth") == 0) {
    /* Echoes the Authorization header (or "" if none); used to verify
     * userinfo-derived Basic auth injection, that an explicit caller header
     * is never overridden, and the cross-origin credential-stripping
     * behavior on redirect. */
    char auth_val[256] = {0};
    srv_find_header(raw, "authorization", auth_val, sizeof(auth_val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, auth_val,
                strlen(auth_val), false);
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
      srv_respond(conn_fd, 400, "Bad Request", "text/plain", NULL, b, strlen(b),
                  false);
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
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete-echo") == 0) {
    /* Echo the request body for DELETE (body_len is 0 when not sent). */
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/keepalive") == 0) {
    /* No "Connection: close"; relies on HTTP/1.1's implicit keep-alive
     * default so the client's idle-pool reuse logic can be exercised. */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/keepalive-then-close") == 0) {
    /* Responds as keep-alive-eligible (no Connection: close) but the server
     * closes its end immediately after; exercises the client's
     * dead-idle-connection detection (liveness probe on reuse). */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return true;
  }

  if (strcmp(path, "/echo-method-body") == 0) {
    /* Echoes "<METHOD>:<body_len>"; used to verify the redirect-following
     * method/body policy (301/302/303 -> bodyless GET except HEAD; 307/308 ->
     * method and body preserved). */
    char b[64];
    int blen = snprintf(b, sizeof(b), "%s:%zu", method, body_len);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, (size_t)blen, false);
    return true;
  }

  if (strcmp(path, "/redirect-301-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n", g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-307-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n", g_srv.port);
    srv_respond(conn_fd, 307, "Temporary Redirect", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-relative") == 0) {
    /* Root-relative Location (no scheme/host); exercises
     * _resolve_redirect_url's root-relative branch. */
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: /get\r\n", NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/redirect-relative-dotted") == 0) {
    /* Multi-level relative Location ("../../get"); exercises RFC 3986
     * SS5.3 merge + remove_dot_segments end-to-end (not just at the unit
     * level): base path "/nested/dir/redirect-relative-dotted" merges with
     * "../../get" to "/nested/dir/../../get", which must normalise to
     * "/get". */
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: ../../get\r\n",
                NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-abs-path-query-with-slashes") == 0) {
    /* Absolute-path Location whose QUERY string (not the path) contains
     * "/../"; RFC 3986 SS5.2.4 dot-segment removal must never touch query
     * bytes. Regression test for a bug where _resolve_redirect_url's
     * absolute-path branch fed the whole "path?query" string into
     * remove_dot_segments, letting "/../" inside the query corrupt the
     * resolved path (e.g. "/foo/bar?x=1/../2" resolved to "/foo/2" instead
     * of leaving the query untouched). The client must preserve the query
     * byte-for-byte, so the next hop's request line must land on the exact
     * route below, not some dot-segment-mangled path. */
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: /query-preserved-target?x=1/../2\r\n", NULL, 0,
                false);
    return true;
  }

  if (strcmp(path, "/query-preserved-target?x=1/../2") == 0) {
    const char *b = "ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), false);
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-same-origin") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-auth\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-cross-origin") == 0) {
    /* Redirects to the IPv6 loopback listener; used purely as a
     * conveniently-different origin (different host) for the same server
     * process, not to test IPv6 itself. Dependent tests skip themselves if
     * the IPv6 listener never bound (see get_test_port6). */
    char loc_hdr[160];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://[::1]:%d/echo-auth\r\n", g_srv.port6);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-chain-1") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-chain-2\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-chain-2") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-infinite") == 0) {
    /* Always redirects to itself; exercises the CHTTP_MAX_REDIRECTS cap:
     * both Tier 1 and the async engine must stop following after the cap and
     * deliver the last 302 response as-is rather than looping forever. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-infinite\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
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

    bool close_after =
        srv_handle_route(conn_fd, method, path, buf, body, body_len);
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

/* IPv6-loopback counterpart of srv_accept_loop; shares g_srv.running (both
 * loops stop together) and the same connection-thread registry/handler, only
 * the listening fd differs. */
static void *srv_accept_loop6(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = accept(g_srv.server_fd6, NULL, NULL);
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
  g_srv.server_fd6 = -1;

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

  /* Best-effort IPv6 loopback listener; some sandboxes/CI environments
   * have no IPv6 stack at all, in which case this whole block just leaves
   * server_fd6 at -1 and port6 at 0; dependent tests check for that and
   * skip themselves rather than hard-failing. */
  int fd6 = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd6 >= 0) {
    int opt6 = 1;
    setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &opt6, sizeof(opt6));

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(TEST_SERVER_PORT);
    addr6.sin6_addr = in6addr_loopback;

    if (bind(fd6, (struct sockaddr *)&addr6, sizeof(addr6)) == 0 &&
        listen(fd6, 64) == 0) {
      socklen_t len6 = sizeof(addr6);
      getsockname(fd6, (struct sockaddr *)&addr6, &len6);
      g_srv.server_fd6 = fd6;
      g_srv.port6 = ntohs(addr6.sin6_port);
      if (pthread_create(&g_srv.accept_tid6, NULL, srv_accept_loop6, NULL) !=
          0) {
        close(fd6);
        g_srv.server_fd6 = -1;
        g_srv.port6 = 0;
      }
    } else {
      close(fd6);
    }
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
  if (g_srv.server_fd6 > 0) {
    shutdown(g_srv.server_fd6, SHUT_RDWR);
    close(g_srv.server_fd6);
    g_srv.server_fd6 = -1;
    pthread_join(g_srv.accept_tid6, NULL);
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

/* 0 if no IPv6 loopback listener could be bound in this environment;
 * dependent tests must check for that and skip themselves. */
static int get_test_port6(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port6;
}

/* Number of TCP connections accepted so far by the test server; used to
 * assert that keep-alive reuse actually skipped the handshake/TCP setup for
 * a given request, rather than opening a fresh connection. */
static int test_server_accept_count(void) {
  return atomic_load(&g_accept_count);
}

/* Build a URL for the test server: http://127.0.0.1:<port><path>. */
static void make_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://127.0.0.1:%d%s", get_test_port(), path);
}

/* Build an IPv6-loopback URL for the test server: http://[::1]:<port><path>.
 * Only valid to call after checking get_test_port6() != 0. */
static void make_url6(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://[::1]:%d%s", get_test_port6(), path);
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
   * exiled; _pool_release_slot must call curl_easy_cleanup on them and must
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

  /* hdrs must survive the call; chttp_run_query must not take ownership. */
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
   * the flag and returns ccol_not_permitted; no arbitrary sleep required. */
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

/* White-box helpers for _parse_chttp_url/_resolve_redirect_url; see
 * src/chttpclient.c's own RUNNING_UNIT_TESTS block for the exact ownership
 * contract (NULL mp -> every out string is plain-malloc'd; free() it). */
extern ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out);
extern char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                                   const char *location);

TEST(url_parsing, ipv6_literal_no_port) {
  bool is_https = false, is_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv =
      _chttp_parse_url_for_tests("https://[::1]/path", &is_https, &is_ipv6,
                                 &host, &port, &pq, &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_https);
  REQUIRE_TRUE(is_ipv6);
  REQUIRE_STREQ(host, "::1");
  REQUIRE_EQ(port, 443);
  REQUIRE_STREQ(pq, "/path");
  REQUIRE_STREQ(origin_key, "https://[::1]:443");
  REQUIRE_EQ((void *)auth, NULL);
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, ipv6_literal_with_port) {
  bool is_https = false, is_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv =
      _chttp_parse_url_for_tests("http://[::1]:8443/", &is_https, &is_ipv6,
                                 &host, &port, &pq, &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_ipv6);
  REQUIRE_STREQ(host, "::1");
  REQUIRE_EQ(port, 8443);
  REQUIRE_STREQ(origin_key, "http://[::1]:8443");
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, ipv6_missing_closing_bracket_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests("http://[::1/path", NULL, NULL,
                                                NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_garbage_after_bracket_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://[::1]x/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_empty_brackets_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests("http://[]/path", NULL, NULL,
                                                NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_live_connect) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] ipv6_live_connect: no IPv6 loopback listener available "
            "in this environment\n");
    return;
  }
  char url[128];
  make_url6(url, sizeof(url), "/get");
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(url_parsing, userinfo_user_and_pass) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://alice:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port,
      &pq, &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)auth, NULL);
  /* base64("alice:s3cr3t") == "YWxpY2U6czNjcjN0" */
  REQUIRE_STREQ(auth, "Basic YWxpY2U6czNjcjN0");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_user_only) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://alice@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  /* base64("alice:") == "YWxpY2U6" */
  REQUIRE_STREQ(auth, "Basic YWxpY2U6");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_pass_only) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  /* base64(":s3cr3t") == "OnMzY3IzdA==" */
  REQUIRE_STREQ(auth, "Basic OnMzY3IzdA==");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_percent_encoded_components) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  /* "%40"->'@', "%3A"->':' inside the raw components; delimiters must be
   * located before decoding, not after. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://user%40x:pa%3Ass@host/path", &dummy_https, &dummy_ipv6, &host,
      &port, &pq, &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  /* base64("user@x:pa:ss") == "dXNlckB4OnBhOnNz" */
  REQUIRE_STREQ(auth, "Basic dXNlckB4OnBhOnNz");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_malformed_percent_escape_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://user%zzpass@host/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, no_userinfo_means_no_auto_authorization) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv =
      _chttp_parse_url_for_tests("http://host/path", &dummy_https, &dummy_ipv6,
                                 &host, &port, &pq, &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ((void *)auth, NULL);
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, fragment_is_stripped_from_wire) {
  /* If the fragment leaked into the request line (the pre-fix bug), the
   * server would see path "/get#section" and fall through to its 404
   * handler instead of matching "/get". */
  char base[128];
  make_url(base, sizeof(base), "/get");
  char url[160];
  snprintf(url, sizeof(url), "%s#section", base);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(url_parsing, fragment_only_defaults_to_root_path) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://host#section", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(pq, "/");
  free(host);
  free(pq);
  free(origin_key);
}

/* ========================================================================== */
/*                     RELATIVE REDIRECT RESOLUTION (RFC 3986)                */
/* ========================================================================== */

TEST(relative_redirects, rfc3986_5_4_reference_table) {
  /* Representative subset of the well-known RFC 3986 SS5.4 normal/abnormal
   * example table, base "http://a/b/c/d;p?q". Covers plain relative, "./",
   * root-relative, protocol-relative, query-only, query+path, ".", "./",
   * "..", "../", multi-level "..", root-exhaustion, "/./", "/../", and
   * dot-segments in the middle of a path ("g/./h", "g/../h") as well as
   * non-special trailing/leading dots ("g.", ".g") that must NOT be treated
   * as dot-segments. Also covers a query string containing "/../"-shaped
   * bytes on both the absolute-path and relative-path branches: dot-segment
   * removal (RFC 3986 SS5.2.4) operates on the path component only, and a
   * query value must never be reinterpreted as path navigation, even when
   * it happens to contain slashes and dots that would otherwise look like
   * dot segments (regression coverage for a bug where the absolute-path
   * branch fed the whole "path?query" string into remove_dot_segments,
   * corrupting both the path and the query whenever the query contained a
   * "/../"-aligned sequence). */
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"g", "http://a/b/c/g"},
      {"./g", "http://a/b/c/g"},
      {"g/", "http://a/b/c/g/"},
      {"/g", "http://a/g"},
      {"//g", "http://g"},
      {"?y", "http://a/b/c/d;p?y"},
      {"g?y", "http://a/b/c/g?y"},
      {"g;x", "http://a/b/c/g;x"},
      {".", "http://a/b/c/"},
      {"./", "http://a/b/c/"},
      {"..", "http://a/b/"},
      {"../", "http://a/b/"},
      {"../g", "http://a/b/g"},
      {"../..", "http://a/"},
      {"../../", "http://a/"},
      {"../../g", "http://a/g"},
      {"../../../g", "http://a/g"},
      {"../../../../g", "http://a/g"},
      {"/./g", "http://a/g"},
      {"/../g", "http://a/g"},
      {"g.", "http://a/b/c/g."},
      {".g", "http://a/b/c/.g"},
      {"g..", "http://a/b/c/g.."},
      {"..g", "http://a/b/c/..g"},
      {"./../g", "http://a/b/g"},
      {"./g/.", "http://a/b/c/g/"},
      {"g/./h", "http://a/b/c/g/h"},
      {"g/../h", "http://a/b/c/h"},
      {"/foo/bar?x=1/../2", "http://a/foo/bar?x=1/../2"},
      {"/a/../b?x=1/../2", "http://a/b?x=1/../2"},
      {"g?x=1/../2", "http://a/b/c/g?x=1/../2"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = _chttp_resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
                                                         cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

TEST(relative_redirects, live_multi_level_dot_segments) {
  char url[160];
  make_url(url, sizeof(url), "/nested/dir/redirect-relative-dotted");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(relative_redirects, live_absolute_path_query_with_slashes_not_corrupted) {
  /* End-to-end regression test (not just at the _resolve_redirect_url unit
   * level): an absolute-path Location whose query string contains "/../"
   * must be forwarded to the server byte-for-byte. Before the fix, the
   * client would mangle the request target into a different path entirely
   * (see /redirect-abs-path-query-with-slashes' own comment in the mock
   * server), which would 404 instead of hitting /query-preserved-target. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-abs-path-query-with-slashes");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     USERINFO-DERIVED BASIC AUTH (LIVE)                     */
/* ========================================================================== */

TEST(credentials, embedded_userinfo_sets_authorization_header) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  /* Insert "alice:s3cr3t@" right after "http://". */
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpclient_resp_free(resp);
}

TEST(credentials, explicit_authorization_header_wins) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");
  chttpclient_resp_free(resp);
}

TEST(credentials, same_origin_redirect_carries_authorization) {
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpclient_resp_free(resp);
}

TEST(credentials, cross_origin_redirect_drops_authorization) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] cross_origin_redirect_drops_authorization: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-cross-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The redirect target never saw the userinfo; the auto-injected
   * Authorization from the original (different) origin must not follow.
   * A zero-length body is represented as resp->body == NULL, not ""; see
   * every other zero-body assertion in this file. */
  REQUIRE_EQ(resp->body_len, (size_t)0);
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
  /* Same scenario as above but with a lowercase key; verifies the common
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

/* ========================================================================== */
/*                     ASYNC ENGINE LIFECYCLE (WHITE-BOX)                     */
/* ========================================================================== */

/*
 * White-box tests for chttpclient's lazy, ref-counted, process-wide async
 * engine (chttpclient.c's g_client_async_*; this module's own DNS/connect
 * pool + deadline sweep, layered on top of the shared facio reactor
 * reference it acquires from src/cfio_engine.c, which backs
 * chttpclient_do_async/pooled-sync). These helpers are compiled only under
 * RUNNING_UNIT_TESTS, matching the same white-box pattern tests/cvector uses
 * for cvector_get_capacity.
 *
 * These tests deliberately run the engine through full start/stop cycles
 * (not just a single acquire/release pair) to prove the lazy-restart path
 * works, since nothing else in this suite exercises it yet.
 */
extern int _chttpclient_engine_ref_count_for_tests(void);
extern bool _chttpclient_engine_running_for_tests(void);
extern ccol_retval_t _chttpclient_engine_acquire_for_tests(void);
extern void _chttpclient_engine_release_for_tests(void);
/* _client_engine_release() hands the actual teardown (releasing the shared
 * cfio_engine reference, stopping the deadline sweep, destroying the DNS
 * pool) off to a detached reaper thread rather than blocking the caller;
 * necessary since release is routinely called from inside a facio callback
 * (on_close), where blocking would be unsafe (see chttpclient.c). That makes
 * g_client_async_running go false immediately but the actual teardown
 * asynchronous; every test below that triggers a stop calls this afterward
 * so the engine is guaranteed fully quiescent before the test returns;
 * otherwise a reaper thread could still be running when the process exits,
 * racing fio_lib_destroy's atexit-time teardown of fio_data itself (a crash
 * caught by valgrind during development of this suite). */
extern void _chttpclient_engine_wait_for_quiescence_for_tests(void);

TEST(async_engine, starts_on_first_acquire_and_stops_at_zero_refcount) {
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);

  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests());
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 1);

  _chttpclient_engine_release_for_tests();
  /* g_client_engine_running flips false synchronously inside release, so
   * this is deterministic without waiting; but the actual teardown (reaper
   * thread) is still asynchronous; wait for it before the test returns. */
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

TEST(async_engine, refcount_tracks_multiple_acquirers) {
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 3);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests());

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 2);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests()); /* still 2 users */

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 1);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests()); /* still 1 user */

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

TEST(async_engine, restart_after_full_stop_works) {
  /* Prove the engine can be stopped and lazily restarted more than once;
   * not just started once for the lifetime of the process. */
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
    REQUIRE_TRUE(_chttpclient_engine_running_for_tests());
    _chttpclient_engine_release_for_tests();
    REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  }
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

typedef struct {
  atomic_int *acquired_ok;
} engine_thread_arg_t;

static void *engine_acquire_release_thread(void *arg) {
  engine_thread_arg_t *a = (engine_thread_arg_t *)arg;
  if (_chttpclient_engine_acquire_for_tests() == ccol_success) {
    atomic_fetch_add(a->acquired_ok, 1);
  }
  /* Hold the reference briefly so overlapping acquires from other threads are
   * likely, then release. */
  usleep(1000);
  _chttpclient_engine_release_for_tests();
  return NULL;
}

TEST(async_engine, concurrent_acquire_release_no_corruption) {
  enum { N = 16 };
  pthread_t threads[N];
  atomic_int acquired_ok = 0;
  engine_thread_arg_t arg = {.acquired_ok = &acquired_ok};

  for (int i = 0; i < N; i++) {
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, engine_acquire_release_thread, &arg),
        0);
  }
  for (int i = 0; i < N; i++) {
    pthread_join(threads[i], NULL);
  }

  REQUIRE_EQ(atomic_load(&acquired_ok), N);
  /* Every acquire was matched by exactly one release. */
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; STEP A (WHITE-BOX, HTTP ONLY)          */
/* ========================================================================== */

/*
 * Functional tests for the Tier 2 async engine (chttpclient_do_async):
 * plain HTTP only (no TLS yet), no redirect-following, no idle-pool reuse;
 * every request opens and then closes a fresh connection. These exercise the
 * real facio reactor end to end against the same mock test server the
 * synchronous (Tier 1) tests use.
 */

/*
 * Waits for the async engine to go fully idle after a request completes.
 * The request's future is fulfilled by _async_fulfill *before* on_data goes
 * on to call fio_close()/fio_force_close(), and on_close (which is what
 * actually calls _client_engine_release()) only runs later, asynchronously
 * ; so ctpool_future_get() returning is not sufficient evidence that
 * release (and the quiescence it can be waited for) has even been triggered
 * yet. Poll for the ref count to reach 0 first, then wait for the reaper
 * that drop triggers to actually finish, so each test leaves the engine
 * fully torn down before returning (see the extern declarations above for
 * why that matters).
 */
static void wait_for_async_engine_idle(void) {
  for (int i = 0; i < 2000 && _chttpclient_engine_ref_count_for_tests() > 0;
       i++) {
    usleep(1000);
  }
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

TEST(async_step_a, get_200) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");

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
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, post_echoes_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"n\":42}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, large_body_response) {
  /* /large returns 8192 bytes; exercises multiple on_data invocations
   * against a single response, not just a one-shot read. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/large");

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

TEST(async_step_a, status_code_forwarded) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/status/404");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, connection_refused_reports_error) {
  chttpcli_construct(cli);
  /* Nothing listens on this port (127.0.0.1 loopback, port 1 is a reserved
   * privileged port essentially never bound in test environments). */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, https_connection_refused_reports_error) {
  /* HTTPS requests are now actually attempted (a future is returned, not
   * NULL); this exercises the is_https flag flowing correctly through ctx
   * creation into the connect stage. Nothing listens on this port, so the
   * failure surfaces before any TLS handshake is even attempted, keeping
   * this test fast; a full end-to-end successful-handshake test lives in
   * the dedicated tests/chttpclient_tls suite (mirroring how
   * tests/chttpserver_tls is kept isolated for its own real cert/handshake
   * needs; see that suite's own top-of-file comment). */
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, https_handshake_fails_against_plain_http_server) {
  /* Connects via https:// to the suite's own plain-HTTP mock server (which
   * never speaks TLS); a real exercise of
   * fio_tls_client_handshake_step's FIO_TLS_HANDSHAKE_ERROR path, without
   * needing a live TLS-capable fixture. Slow (~5s): the mock server's
   * srv_read_request has a fixed 5-second SO_RCVTIMEO and a raw TLS
   * ClientHello never contains the "\r\n\r\n" it's waiting for, so the
   * server sits silent until its own timeout closes the connection;
   * there is no per-request timeout enforcement in the async engine yet
   * (see the "reactor-owned timer/cancellation" roadmap item) to cut this
   * shorter client-side. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  /* make_url builds an http:// URL; splice in an "s" (url + 4 skips past
   * the literal "http", leaving "://127.0.0.1:<port>/get" to append). */
  char https_url[168];
  snprintf(https_url, sizeof(https_url), "https%s", url + 4);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, https_url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_TRUE(rv == ccol_http_tls_handshake_failed ||
               rv == ccol_http_tls_cert_verification_failed ||
               rv == ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, null_args_returns_null) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpclient_do_async(NULL, req), NULL);
  REQUIRE_EQ((void *)chttpclient_do_async(cli, NULL), NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

typedef struct {
  chttpcli cli;
  char url[160];
  int expected_status;
  bool ok;
} async_concurrent_arg_t;

static void *async_concurrent_thread(void *arg) {
  async_concurrent_arg_t *a = (async_concurrent_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;

  ctpool_future *f = chttpclient_do_async(a->cli, req);
  chttp_request_free(req);
  if (!f) return NULL;

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == a->expected_status;
    chttpclient_resp_free(resp);
  }
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  return NULL;
}

TEST(async_step_a, concurrent_requests_all_succeed) {
  /* Fires several concurrent async requests against one shared engine;
   * proving the reactor genuinely multiplexes multiple live connections
   * rather than only ever handling one at a time. */
  enum { N = 12 };
  chttpcli_construct(cli);
  pthread_t threads[N];
  async_concurrent_arg_t args[N];

  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    make_url(args[i].url, sizeof(args[i].url), "/get");
    args[i].expected_status = 200;
    args[i].ok = false;
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, async_concurrent_thread, &args[i]),
        0);
  }
  for (int i = 0; i < N; i++) pthread_join(threads[i], NULL);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; STEP B (REDIRECT FOLLOWING)            */
/* ========================================================================== */

/*
 * Functional tests for redirect-following in the async engine
 * (_chttp_do_async_internal / _async_handle_redirect / _async_submit_hop).
 * Mirrors the synchronous Tier 1 redirect_policy suite's method/body policy
 * assertions, plus async-specific chain-lifecycle coverage (multi-hop
 * chains, the CHTTP_MAX_REDIRECTS cap, and a relative Location) that has no
 * Tier 1 equivalent above since those code paths are shared with Tier 1 via
 * _resolve_redirect_url and the shared llhttp on_headers_complete callback.
 */

TEST(async_redirects, get_301_follows_to_final_resource) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect");

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
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, post_301_becomes_bodyless_get) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, post_307_preserves_method_and_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  /* The original request is freed right after the call returns, before the
   * redirect hop (which needs the body again) ever runs; proving the
   * chain's own deep copy (chttp_async_chain_t.body_data), not a reference
   * into req, is what the second hop actually resends. */
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, relative_location_resolved_against_current_host) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-relative");

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
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, userinfo_credentials_carried_same_origin) {
  /* Tier 2's own carried_auth/carried_auth_origin on chttp_async_chain_t;
   * a separate code path from Tier 1's, mirrored in _async_submit_hop. */
  chttpcli_construct(cli);
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

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
  /* base64("alice:s3cr3t") == "YWxpY2U6czNjcjN0" */
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, userinfo_credentials_dropped_cross_origin) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] userinfo_credentials_dropped_cross_origin: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-cross-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

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
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, multi_hop_chain_reaches_final_resource) {
  /* /redirect-chain-1 -> /redirect-chain-2 -> /get: exercises that each hop
   * gets its own fresh chttp_async_ctx_t (a new connection) while the whole
   * chain still fulfils exactly one future exactly once. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-chain-1");

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
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, exceeding_max_redirects_returns_last_hop_as_is) {
  /* /redirect-infinite always redirects to itself. Once
   * CHTTP_MAX_REDIRECTS hops have been exhausted, both Tier 1 and the async
   * engine stop following and deliver the last 302 response as a normal,
   * successful (non-error) result instead of looping forever; this is the
   * one test in this suite that walks the actual cap, so it also doubles as
   * a stress test of the chain refcount/hop-chaining machinery across 51
   * real connections. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-infinite");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 302);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; IDLE CONNECTION POOL (TIER 2)          */
/* ========================================================================== */

/*
 * Functional tests for the Tier 2 idle pool (_async_idle_pool_take/offer,
 * the CHTTP_ASYNC_IDLE state, and the reused/any_bytes_read dead-connection
 * retry). Mirrors the synchronous Tier 1 `keepalive` suite's verification
 * approach (test_server_accept_count() before/after) against the same
 * /keepalive and /keepalive-then-close mock routes.
 *
 * Unlike the non-pooling async_step_a/async_redirects tests above, a
 * successfully pooled connection holds its OWN engine reference until it is
 * either reused or the client is destroyed (which synchronously drains its
 * pool); so these tests call chttpclient_destroy(cli) BEFORE
 * wait_for_async_engine_idle(), the reverse of the order used elsewhere in
 * this file, since otherwise the still-pooled connection's held reference
 * would keep the engine's ref count above zero indefinitely.
 */

static ctpool_future *async_get(chttpcli cli, const char *url) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  return f;
}

TEST(async_idle_pool, sequential_requests_reuse_connection) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    ctpool_future *f = async_get(cli, url);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(f);
  }

  /* Give the reactor a brief moment to finish offering the last response's
   * connection to the idle pool; that happens just before the future is
   * fulfilled (see _async_on_data's HPE_PAUSED handling), but the server's
   * own accept-count increment happens independently, on its own
   * accept()-side thread. */
  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli); /* drains the one still-pooled connection */
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, dead_connection_detected_and_retried) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  ctpool_future *f1 = async_get(cli, url1);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpcli_response *resp1 = raw1->resp;
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);

  /* The server closed its end after that response; the pooled connection's
   * own IDLE-state on_data/on_close should detect this asynchronously and
   * evict it; but even if that hasn't happened yet by the time the next
   * request pops it, the reused/any_bytes_read retry-once mechanism must
   * transparently recover by opening a fresh connection. */
  ctpool_future *f2 = async_get(cli, url2);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_EQ(raw2->rv, ccol_success);
  chttpcli_response *resp2 = raw2->resp;
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpclient_resp_free(resp2);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} async_idle_concurrent_arg_t;

static void *async_idle_concurrent_thread(void *arg) {
  async_idle_concurrent_arg_t *a = (async_idle_concurrent_arg_t *)arg;
  ctpool_future *f = async_get(a->cli, a->url);
  if (!f) return NULL;
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == 200;
    chttpclient_resp_free(resp);
  }
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  return NULL;
}

TEST(async_idle_pool, concurrent_requests_exceeding_idle_cap_no_crash) {
  /* Fires more concurrent keep-alive requests than the idle pool's
   * per-origin cap can hold; excess connections simply are not pooled
   * (the same documented, intentional simplification Tier 1's identical
   * keepalive test exercises) rather than causing any crash, leak, or
   * failure. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  enum { N = 12 };
  pthread_t threads[N];
  async_idle_concurrent_arg_t args[N];
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    REQUIRE_EQ(pthread_create(&threads[i], NULL, async_idle_concurrent_thread,
                              &args[i]),
               0);
  }
  for (int i = 0; i < N; i++) pthread_join(threads[i], NULL);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * ASYNC DEADLINE SWEEP (Tier 2 connect_timeout_ms/request_timeout_ms
 * enforcement); exercises the reactor-owned periodic sweep that force-
 * closes connections whose absolute wall-clock deadline has passed, mirrors
 * Tier 1's own timeout semantics (ccol_timed_out), and verifies a generous
 * timeout never interferes with an otherwise-successful request.
 */

TEST(async_deadline, request_timeout_fires_against_slow_endpoint) {
  char url[160];
  make_url(url, sizeof(url),
           "/slow"); /* server sleeps 100ms before responding */

  chttpcli_construct(cli);
  /* Comfortably shorter than /slow's 100ms sleep, comfortably longer than a
   * loopback connect; isolates the request (not connect) deadline. */
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 20), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_timed_out);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_request_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/slow");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 5000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, connect_timeout_fires_against_unroutable_address) {
  chttpcli_construct(cli);
  /* Short enough to fire quickly; TEST-NET-1 (RFC 5737) is guaranteed
   * non-routable on a normal network, so the connect (never the request)
   * deadline is expected to trip here; but some sandboxed/virtualized
   * network environments respond to it with an immediate rejection
   * (ENETUNREACH or similar) instead of the packet silently vanishing.
   * Confirmed to happen intermittently in this exact CI/sandbox: a raw
   * `bash -c 'exec 3<>/dev/tcp/192.0.2.1/9'` connect attempt, entirely
   * outside this library, sometimes hangs for the full probe duration and
   * sometimes fails immediately with "Network is unreachable"; i.e. this
   * is the underlying network's own inconsistent behavior toward that
   * address, not something chttpclient's connect-timeout logic controls or
   * should be expected to paper over. An immediate connection failure
   * still proves the connect attempt did not silently succeed, so
   * ccol_http_connection_failed and ccol_http_transfer_aborted (a fio_socket
   * failure surfacing through the async engine) are accepted alongside the
   * "real" ccol_timed_out outcome. */
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 300), ccol_success);

  ctpool_future *f = async_get(cli, "http://192.0.2.1:9/");
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  bool connect_did_not_silently_succeed =
      raw->rv == ccol_timed_out || raw->rv == ccol_http_connection_failed ||
      raw->rv == ccol_http_transfer_aborted;
  REQUIRE_TRUE(connect_did_not_silently_succeed);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_connect_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 5000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * ASYNC STREAMING (Tier 2 chttpclient_do_async_streaming); reuses
 * stream_sink_t/stream_sink_write and abort_write_fn (defined earlier in
 * this file for the Tier 1 streaming tests) against the same async engine
 * exercised by the async_step_a/async_redirects/async_idle_pool/
 * async_deadline suites above.
 */

static ctpool_future *async_get_streaming(chttpcli cli, const char *url,
                                          chttpcli_write_fn write_fn,
                                          void *write_ctx) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  ctpool_future *f =
      chttpclient_do_async_streaming(cli, req, write_fn, write_ctx);
  chttp_request_free(req);
  return f;
}

TEST(async_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Body was delivered via the callback, not buffered; mirrors Tier 1's
   * chttpclient_do_streaming leaving chttpcli_response.body NULL. */
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_GT(sink.len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, large_body_streamed_in_chunks) {
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ((void *)resp->body, NULL);
  /* /large serves 8192 'x' bytes; stream_sink_t's buffer caps at 4095
   * captured bytes, but every chunk must still have been offered to the
   * callback (partial capture is the sink's own choice, not a transfer
   * failure); rv == ccol_success above already proves that. */
  REQUIRE_EQ(sink.len, sizeof(sink.buf) - 1);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);

  ctpool_future *f = async_get_streaming(cli, url, abort_write_fn, NULL);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, null_write_fn_returns_null) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpclient_do_async_streaming(cli, req, NULL, NULL),
             NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, redirect_final_body_delivered_not_intermediate) {
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  /* Final resource's status, not the 301; and the sink must only have
   * captured the FINAL hop's body (redirect hops route through
   * _sink_discard internally, matching Tier 1's identical behaviour). */
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * POOLED-SYNC API (Tier 3, chttpclient_do_pooled/_streaming); thin
 * blocking wrappers over Tier 2. Reuses stream_sink_t/stream_sink_write and
 * abort_write_fn (defined earlier for the Tier 1 streaming tests).
 */

TEST(pooled, get_200) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled, null_checks) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do_pooled(NULL, req, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_pooled(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_pooled(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled, bad_url_returns_specific_error_not_generic) {
  /* The key behavioural claim of this tier: a pre-queue failure Tier 2
   * collapses into a plain NULL (chttpclient_do_async would return NULL
   * here, indistinguishable from OOM or an engine-start failure) must
   * still surface as the exact same specific code chttpclient_do returns
   * for the identical URL; see _chttp_async_preflight_check. */
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled, redirect_followed_transparently) {
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} pooled_concurrent_arg_t;

static void *pooled_concurrent_thread(void *arg) {
  pooled_concurrent_arg_t *a = (pooled_concurrent_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(a->cli, req, &resp);
  a->ok = (rv == ccol_success) && resp && resp->status_code == 200;
  if (resp) chttpclient_resp_free(resp);
  chttp_request_free(req);
  return NULL;
}

TEST(pooled, concurrent_callers_all_succeed) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  enum { N = 12 };
  pthread_t threads[N];
  pooled_concurrent_arg_t args[N];
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    REQUIRE_EQ(
        pthread_create(&threads[i], NULL, pooled_concurrent_thread, &args[i]),
        0);
  }
  for (int i = 0; i < N; i++) pthread_join(threads[i], NULL);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;
  ccol_retval_t rv = chttpclient_do_pooled_streaming(
      cli, req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, null_write_fn_returns_invalid_args) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ(chttpclient_do_pooled_streaming(cli, req, NULL, NULL, NULL),
             ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval_t rv =
      chttpclient_do_pooled_streaming(cli, req, abort_write_fn, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, bad_url_returns_specific_error_not_generic) {
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval_t rv =
      chttpclient_do_pooled_streaming(cli, req, stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}
