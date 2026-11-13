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

// pthread_tryjoin_np() is a glibc extension that <pthread.h> declares only with
// this define. We use it to reap mock-server connection threads that have
// already stopped.
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chttpclient.h>
#include <common.h>
#include <cthreadpool.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/csock.h>
#include <internal/test_helpers/chttpclient.h>
#include <internal/test_helpers/cpintable.h>
#include <internal/test_helpers/cthreadpool.h>
#include <internal/test_helpers/ctls.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_accept.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_fds.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <test_wait.h>
#include <unistd.h>

TAU_MAIN()

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

// TEST_UNDER_TSAN is defined in a ThreadSanitizer build, with either compiler.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_UNDER_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define TEST_UNDER_TSAN 1
#endif
// Joins t if it has already ended, without blocking. register_conn_thread and
// register_tls_conn_thread use it to reap finished connection threads. Gives 0
// when t was joined, non-zero when it is still running. glibc has
// pthread_tryjoin_np(), and FreeBSD uses pthread_timedjoin_np() with a deadline
// that has passed. macOS never reaps early, and neither does FreeBSD under
// ThreadSanitizer, whose runtime doesn't intercept pthread_timedjoin_np() and
// stops the process when libthr reuses the structure of a thread joined that
// way.
static int test_tryjoin(pthread_t t) {
#if defined(__GLIBC__)
  return pthread_tryjoin_np(t, NULL);
#elif defined(__FreeBSD__) && !defined(TEST_UNDER_TSAN)
  const struct timespec passed = {0, 0};
  return pthread_timedjoin_np(t, NULL, &passed);
#else
  (void)t;
  return EBUSY;
#endif
}

// The descriptor of the destructor probe, or -1; see
// engine_wait.the_module_destructor_initializes_nothing_unused.
static int g_dtor_probe_fd = -1;

// Ignores SIGPIPE for the whole process and resets the inherited signal state,
// before any test runs. The mock server's send() calls don't pass MSG_NOSIGNAL,
// so a client that closes a connection while a server thread is inside send()
// would otherwise kill the process; the library never touches the SIGPIPE
// disposition (tests_sigpipe.c covers that). The forked children report through
// their wait status, which an inherited signal state can break.
__attribute__((constructor)) static void _ignore_sigpipe_for_mock_server(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

// Minimal HTTP/1.1 test server
//
// The request buffer of a connection thread, and port 0 so the OS picks a free
// port.
#define TEST_SERVER_BUF 65536
#define TEST_SERVER_PORT 0

// The listeners of the test server: TCP on 127.0.0.1, TCP on [::1] and a Unix
// domain socket. The fds are atomic because stop_test_server writes them while
// the accept loops read them.
typedef struct {
  atomic_int server_fd;
  int port;
  pthread_t accept_tid;
  atomic_int running;

  // The IPv6 listener is best effort: When the bind fails server_fd6 stays -1
  // and port6 stays 0.
  atomic_int server_fd6;
  int port6;
  pthread_t accept_tid6;

  // The Unix listener; unix_path is built from getpid(), so it's unique per
  // test process.
  atomic_int server_fd_unix;
  char unix_path[128];
  pthread_t accept_tid_unix;
} test_server_t;

// The one test server of the process, started once by the first get_test_port()
// or get_test_unix_socket_path(). g_slow_started counts the slow routes that
// have begun to answer, so a test can wait until its slow request is in
// flight.
static test_server_t g_srv;
static pthread_once_t g_srv_once = PTHREAD_ONCE_INIT;
static atomic_int g_slow_started = 0;
// The /held route only answers after a test releases it. Every held request
// reads this epoch when it starts and waits until it changes, so a release ends
// every hold that started before it and none that starts after it.
static atomic_uint g_held_epoch = 0;

// Ends every hold of /held that started before this call, by moving
// g_held_epoch on. A test that uses /held calls it on every way out, before any
// REQUIRE_* that can return early, so no server thread stays held.
static void held_release(void) { atomic_fetch_add(&g_held_epoch, 1u); }
// The TCP connections the test server has accepted.
static atomic_int g_accept_count = 0;

// The registry of connection threads, so stop_test_server can join all of
// them. g_conn_mutex guards it.
#define MAX_CONN_THREADS 256
static pthread_t g_conn_threads[MAX_CONN_THREADS];
static int g_conn_thread_count = 0;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

// Adds a connection thread to g_conn_threads under g_conn_mutex, so
// stop_test_server can join it. It first reaps the entries that have already
// stopped: The server runs for the whole process, and a stopped thread that
// nobody joined keeps its stack mapping, which can use up the address space of
// a 32-bit build. A thread that doesn't fit is detached.
static void register_conn_thread(pthread_t tid) {
  pthread_mutex_lock(&g_conn_mutex);
  int kept = 0;
  for (int i = 0; i < g_conn_thread_count; i++) {
    if (test_tryjoin(g_conn_threads[i]) != 0) {
      g_conn_threads[kept++] = g_conn_threads[i];
    }
  }
  g_conn_thread_count = kept;

  if (g_conn_thread_count < MAX_CONN_THREADS) {
    g_conn_threads[g_conn_thread_count++] = tid;
  } else {
    pthread_detach(tid);
  }
  pthread_mutex_unlock(&g_conn_mutex);
}

// Sends a complete response on fd; every simple route of the mock server
// answers through it. When keep_alive is false the response carries
// "Connection: close"; only the keep-alive routes pass true.
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

// Finds the value of header name (case-insensitive) in the raw request text,
// so a route can answer from it. Copies the value into out and gives non-zero
// when the header is there.
static int srv_find_header(const char *raw, const char *name, char *out,
                           size_t out_size) {
  const char *p = raw;
  size_t nlen = strlen(name);
  while (*p) {
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

// Counts the header lines named name (case-insensitive) in the raw request.
// /count-header reports it, so a test can see how often the client sent a
// header.
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

// Splits the request line in buf into the method and the path, which the
// connection thread routes on.
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

// Reads the request headers from fd into buf and stops at the first CRLFCRLF,
// keeping only the body bytes that arrived in the same reads. Stopping there
// lets a route answer Expect: 100-continue before it reads any body byte. Gives
// the number of bytes read (0 or less on EOF or error) and the length of the
// header block in *hdr_len_out.
static ssize_t srv_read_headers(int fd, char *buf, size_t max,
                                size_t *hdr_len_out) {
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ssize_t total = 0;
  while (total < (ssize_t)(max - 1)) {
    ssize_t n = recv(fd, buf + total, max - 1 - (size_t)total, 0);
    if (n <= 0) {
      *hdr_len_out = 0;
      return total > 0 ? total : n;
    }
    total += n;
    buf[total] = '\0';

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) {
      *hdr_len_out = (size_t)(hdr_end - buf) + 4;
      return total;
    }
  }
  *hdr_len_out = 0;
  return total;
}

// Serves /expect-continue-echo and /expect-continue-reject, which test both
// answers a server can give to Expect: 100-continue. buf holds `total` bytes,
// the headers (hdr_len bytes) and maybe some body. With accept_body the route
// sends "100 Continue", reads the body and echoes it; without it the route
// answers 417 at once and never reads a body byte, so that answer is the final
// response. Returns true when the connection must close, like
// srv_handle_route.
static bool srv_handle_expect_continue_route(int conn_fd, char *buf, size_t max,
                                             size_t total, size_t hdr_len,
                                             bool accept_body) {
  if (!accept_body) {
    const char *b = "expectation failed";
    srv_respond(conn_fd, 417, "Expectation Failed", "text/plain", NULL, b,
                strlen(b), false);
    return true;
  }

  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  const char *body = buf + hdr_len;
  size_t body_len = (cl > 0) ? (size_t)cl : 0;
  srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
              body_len, false);
  return true;
}

// Writes half of a "100 Continue" status line, sleeps past
// CHTTP_100_CONTINUE_WAIT_US and closes without a response. It tests a reused
// connection whose window ends in the middle of a message, which the client
// must not retry. Always closes.
static bool srv_handle_expect_continue_timeout_then_die_route(int conn_fd) {
  const char *partial = "HTTP/1.1 100 Con";
  send(conn_fd, partial, strlen(partial), 0);
  usleep(1300000);
  return true;
}

// Sends "103 Early Hints" before "100 Continue", then reads and echoes the
// body. The client waits for status 100 only, so this tests that it drops the
// 103 and keeps waiting in the same window.
static bool srv_handle_expect_continue_with_hints_route(int conn_fd, char *buf,
                                                        size_t max,
                                                        size_t total,
                                                        size_t hdr_len) {
  const char *hints =
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
  send(conn_fd, hints, strlen(hints), 0);
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  const char *body = buf + hdr_len;
  size_t body_len = (cl > 0) ? (size_t)cl : 0;
  srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
              body_len, false);
  return true;
}

// Answers directly without "100 Continue", never reads the body, and leaves the
// connection open. The client still owes this connection a body, so the test
// that uses it checks that the client never pools it.
static bool srv_handle_expect_continue_reject_keepalive_route(int conn_fd) {
  const char *b = "expectation failed";
  srv_respond(conn_fd, 417, "Expectation Failed", "text/plain", NULL, b,
              strlen(b), true);
  return false;
}

// Writes "100 Continue" and, in the same send(), the head of a final response
// with a truncated body, then closes. The leftover bytes land in the client's
// parser, so its test checks that the client counts them as bytes read and
// never retries on a fresh connection with that polluted state.
static bool srv_handle_expect_continue_fake_final_then_die_route(int conn_fd) {
  const char *wire =
      "HTTP/1.1 100 Continue\r\n\r\n"
      "HTTP/1.1 200 OK\r\n"
      "x-stale: yes\r\n"
      "content-length: 100\r\n"
      "\r\n"
      "STALE-BODY-PREFIX-THAT-MUST-NEVER-REACH-THE-CALLER";
  send(conn_fd, wire, strlen(wire), 0);
  return true;
}

// The number of full bodies the die-after-100-clean route read, so its test can
// assert that the body reached the server exactly once.
static _Atomic int g_die_after_100_clean_body_recv_count = 0;

// Sends "100 Continue" on its own, reads the whole body (counting it in
// g_die_after_100_clean_body_recv_count) and closes with a clean EOF. The peer
// showed it was alive, so its test checks that the client doesn't resend the
// body on a second connection.
static bool srv_handle_expect_continue_die_after_100_clean_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len) {
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }
  if (cl > 0 && total - hdr_len >= (size_t)cl)
    atomic_fetch_add(&g_die_after_100_clean_body_recv_count, 1);

  return true;
}

// Writes "100 Continue" and a complete final response in one send(), then
// drains the body the client sends. It tests that the async engine keeps a
// final response that arrived before its body write and replays it afterwards.
static bool srv_handle_expect_continue_bundled_final_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len) {
  const char *resp_body = "bundled-response";
  char wire[512];
  int wn = snprintf(wire, sizeof(wire),
                    "HTTP/1.1 100 Continue\r\n\r\n"
                    "HTTP/1.1 200 OK\r\n"
                    "content-type: text/plain\r\n"
                    "content-length: %zu\r\n"
                    "connection: close\r\n"
                    "\r\n%s",
                    strlen(resp_body), resp_body);
  send(conn_fd, wire, (size_t)wn, 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  return true;
}

// Sends n_before "103 Early Hints", then "100 Continue", reads the body, sends
// n_after more 103s and then the answer; the counts come from the path. It
// tests that the client's CHTTP_MAX_INTERIM_RESPONSES cap counts consecutive
// discards, so each side of the "100 Continue" gets its own budget.
static bool srv_handle_expect_continue_hints_both_sides_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len,
    const char *path) {
  int n_before = 0, n_after = 0;
  sscanf(path, "/expect-continue-hints-both-sides/%d/%d", &n_before, &n_after);
  const char *hints =
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
  for (int i = 0; i < n_before; i++) {
    if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
  }
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  for (int i = 0; i < n_after; i++) {
    if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
  }

  const char *b = "{\"status\":\"ok\"}";
  srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
              false);
  return true;
}

static void percent_encode_unix_path(char *buf, size_t buf_size,
                                     const char *raw);

// The /sweep-race route only answers once this is true.
static atomic_bool g_sweep_race_respond = false;

// Routes one request of the mock server and sends its response; every route
// the tests use outside the Expect: 100-continue family lives here. buf holds
// the request read so far. Returns true when the connection must close, and
// false when srv_conn_thread should read another request from the same fd
// (keep-alive). Some routes read or bump globals of this file (g_held_epoch,
// g_slow_started, g_sweep_race_respond).
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
    char auth_val[256] = {0};
    srv_find_header(raw, "authorization", auth_val, sizeof(auth_val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, auth_val,
                strlen(auth_val), false);
    return true;
  }

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

  if (strcmp(method, "GET") == 0 && strcmp(path, "/redirect-with-body") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    const char *b = "this-intermediate-body-must-never-reach-the-caller";
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, b,
                strlen(b), false);
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
    atomic_fetch_add(&g_slow_started, 1);
    usleep(100000);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/very-slow") == 0) {
    atomic_fetch_add(&g_slow_started, 1);
    usleep(500000);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 &&
      (strcmp(path, "/drip") == 0 || strcmp(path, "/drip-stop") == 0)) {
    bool stop = strcmp(path, "/drip-stop") == 0;
    unsigned epoch = atomic_load(&g_held_epoch);
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Length: 10\r\nConnection: close\r\n\r\n";
    if (send(conn_fd, head, sizeof(head) - 1, MSG_NOSIGNAL) < 0) return true;
    for (int i = 0; i < 10; i++) {
      if (stop && i == 5) {
        for (int j = 0; j < 1500 && atomic_load(&g_held_epoch) == epoch; j++)
          usleep(10000);
        return true;
      }
      char b = (char)('0' + i);
      if (send(conn_fd, &b, 1, MSG_NOSIGNAL) != 1) return true;
      if (i < 9) usleep(150000);
    }
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/held") == 0) {
    unsigned epoch = atomic_load(&g_held_epoch);
    atomic_fetch_add(&g_slow_started, 1);
    for (int i = 0; i < 1500 && atomic_load(&g_held_epoch) == epoch; i++)
      usleep(10000);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/large") == 0) {
    char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, large_body,
                sizeof(large_body), false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/huge") == 0) {
    size_t huge_len = 512 * 1024;
    char *huge_body = malloc(huge_len);
    if (!huge_body) {
      srv_respond(conn_fd, 500, "Internal Server Error", "text/plain", NULL, "",
                  0, false);
      return true;
    }
    memset(huge_body, 'h', huge_len);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, huge_body, huge_len,
                false);
    free(huge_body);
    return true;
  }

  if (strcmp(path, "/echo-content-type") == 0) {
    char ct[256] = {0};
    srv_find_header(raw, "content-type", ct, sizeof(ct));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, ct, strlen(ct), false);
    return true;
  }

  if (strcmp(path, "/count-content-type") == 0) {
    int count = srv_count_header(raw, "content-type");
    char body_buf[8];
    int blen = snprintf(body_buf, sizeof(body_buf), "%d", count);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(path, "/count-header") == 0) {
    char name[64] = {0};
    srv_find_header(raw, "x-count-name", name, sizeof(name));
    int count = name[0] ? srv_count_header(raw, name) : -1;
    char body_buf[8];
    int blen = snprintf(body_buf, sizeof(body_buf), "%d", count);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(path, "/echo-header-raw") == 0) {
    char name[64] = {0};
    srv_find_header(raw, "x-echo-name", name, sizeof(name));
    char val[256] = {0};
    if (name[0]) srv_find_header(raw, name, val, sizeof(val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, val, strlen(val),
                false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete-echo") == 0) {
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/keepalive") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/keepalive-then-close") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/early-hints") == 0) {
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    send(conn_fd, hints, strlen(hints), 0);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/endless-early-hints") == 0) {
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    for (int i = 0; i < 100; i++) {
      if (send(conn_fd, hints, strlen(hints), 0) < 0) break;
    }
    return true;
  }

  if (strncmp(path, "/early-hints-count/", 19) == 0) {
    int n = atoi(path + 19);
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    for (int i = 0; i < n; i++) {
      if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
    }
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/early-hints-same-write") == 0) {
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    const char *b = "{\"status\":\"ok\"}";
    char combined[512];
    int clen = snprintf(combined, sizeof(combined),
                        "%sHTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n\r\n%s",
                        hints, strlen(b), b);
    if (clen > 0) send(conn_fd, combined, (size_t)clen, 0);
    return false;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/early-hints-oversized-content-length") == 0) {
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nContent-Length: 999999\r\n\r\n";
    send(conn_fd, hints, strlen(hints), 0);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "HEAD") == 0 &&
      strcmp(path, "/head-oversized-content-length") == 0) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/304-oversized-content-length") == 0) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 304 Not Modified\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/204-oversized-content-length") == 0) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 204 No Content\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/echo-method-body") == 0) {
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

  if (strcmp(path, "/redirect-301-then-307-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-307-to-echo\r\n",
             g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-301-to-count-header") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/count-header\r\n", g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-relative") == 0) {
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: /get\r\n", NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-empty-location") == 0) {
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain",
                "Location: \r\n", NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/sweep-race") == 0) {
    for (int i = 0; i < 10000 && !atomic_load(&g_sweep_race_respond); i++)
      usleep(1000);
    const char *b = "ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), true);
    return false;
  }

  if (strcmp(path, "/repeated-fields") == 0) {
    srv_respond(conn_fd, 200, "OK", "text/plain",
                "Set-Cookie: a=1; Path=/\r\n"
                "X-Multi: one\r\n"
                "Set-Cookie: b=2; Expires=Wed, 21 Oct 2037 07:28:00 GMT\r\n"
                "X-Single: s\r\n"
                "x-multi: two\r\n"
                "X-Empty-Twice: \r\n"
                "X-Multi: three\r\n"
                "X-Empty-Twice: \r\n",
                "ok", 2, false);
    return true;
  }

  if (strncmp(path, "/long-header-line/", 18) == 0 ||
      strncmp(path, "/long-header-hint/", 18) == 0 ||
      strncmp(path, "/long-header-redir/", 19) == 0) {
    bool hint = strncmp(path, "/long-header-hint/", 18) == 0;
    bool redir = strncmp(path, "/long-header-redir/", 19) == 0;
    size_t n = (size_t)strtoul(path + (redir ? 19 : 18), NULL, 10);
    if (n > 200000) n = 200000;
    char *head = malloc(2 * n + 512);
    if (!head) return false;
    int off = 0;
    if (hint) {
      off = snprintf(head, 256, "HTTP/1.1 103 Early Hints\r\nX-Hint: ");
      memset(head + off, 'h', n);
      off += (int)n;
      off += snprintf(head + off, 16, "\r\n\r\n");
    }
    if (redir)
      off += snprintf(head + off, 256,
                      "HTTP/1.1 302 Found\r\nLocation: /long-header-line/"
                      "%zu\r\nContent-Length: 2\r\n"
                      "Connection: close\r\nX-Long: ",
                      n);
    else
      off += snprintf(head + off, 256,
                      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                      "Connection: close\r\nX-Long: ");
    memset(head + off, 'v', n);
    size_t len = (size_t)off + n;
    static const char tail[] = "\r\nX-Long: tail\r\n\r\nok";
    memcpy(head + len, tail, sizeof(tail) - 1);
    len += sizeof(tail) - 1;
    size_t sent = 0;
    while (sent < len) {
      ssize_t w = send(conn_fd, head + sent, len - sent, MSG_NOSIGNAL);
      if (w <= 0) break;
      sent += (size_t)w;
    }
    free(head);
    return true;
  }

  if (strcmp(path, "/many-repeated-fields") == 0) {
    char extra[1536];
    size_t off = 0;
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < 44; i++) {
        int n = pass == 0 ? i : 43 - i;
        off += (size_t)snprintf(extra + off, sizeof(extra) - off,
                                "X-R%02d: %c%d\r\n", n, pass ? 'b' : 'a', n);
      }
    snprintf(extra + off, sizeof(extra) - off, "X-E: \r\nX-E: z\r\n");
    srv_respond(conn_fd, 200, "OK", "text/plain", extra, "ok", 2, false);
    return true;
  }

  if (strcmp(path, "/redirect-two-locations") == 0) {
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: /get\r\nLocation: /no-such-route\r\n", NULL, 0,
                false);
    return true;
  }

  if (strcmp(path, "/redirect-unsupported-scheme-location") == 0) {
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: mailto:test@example.com\r\n", NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-unix-socket") == 0 ||
      strcmp(path, "/redirect-to-unix-socket-mixed-case") == 0) {
    char encoded[256];
    percent_encode_unix_path(encoded, sizeof(encoded), g_srv.unix_path);
    char loc_hdr[512];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: %s%s/get\r\n",
             strcmp(path, "/redirect-to-unix-socket") == 0 ? "http+unix://"
                                                           : "HtTp+UnIx://",
             encoded);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-then-redirect-to-unix-socket") == 0) {
    char loc_hdr[256];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-to-unix-socket\r\n",
             g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-other-unix-socket") == 0) {
    srv_respond(
        conn_fd, 302, "Found", "text/plain",
        "Location: http+unix://%2Ftmp%2Fccol-not-this-socket.sock/get\r\n",
        NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/redirect-relative-dotted") == 0) {
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: ../../get\r\n",
                NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/redirect-relative-plain") == 0) {
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: sibling\r\n",
                NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/sibling") == 0) {
    const char *b = "sibling-ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), false);
    return true;
  }

  if (strcmp(path, "/redirect-abs-path-query-with-slashes") == 0) {
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

  if (strcmp(path, "/redirect-to-echo-auth-upper-localhost") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://LOCALHOST:%d/echo-auth\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/partial-then-reset") == 0) {
    const char *part = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc";
    ssize_t w = send(conn_fd, part, strlen(part), MSG_NOSIGNAL);
    (void)w;
    usleep(200 * 1000);
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(conn_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-cross-origin") == 0) {
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
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-infinite\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/chunked-body") == 0) {
    const char *raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "Hello, \r\n"
        "e\r\n"
        "chunked world!\r\n"
        "0\r\n"
        "\r\n";
    send(conn_fd, raw, strlen(raw), 0);
    return false;
  }

  if (strcmp(path, "/eof-delimited-body") == 0) {
    const char *raw =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "eof-delimited-body-ok";
    send(conn_fd, raw, strlen(raw), 0);
    return true;
  }

  const char *b = "not found";
  srv_respond(conn_fd, 404, "Not Found", "text/plain", NULL, b, strlen(b),
              false);
  return true;
}

// What the slow-final route saw after its answer: The body bytes the client
// sent, which must be 0, and a done counter it bumps after publishing them.
static _Atomic int g_slow_final_body_bytes = 0;
static _Atomic int g_slow_final_done = 0;

// Answers 200 at once without "100 Continue" and holds the end of the body back
// past CHTTP_100_CONTINUE_WAIT_US, chunked or not. The direct_final tests use
// it to check that a response which outlasts the window is read whole. Then it
// counts the body bytes the client sends for up to two seconds into
// g_slow_final_body_bytes, which must stay 0, and bumps g_slow_final_done.
static bool srv_handle_expect_continue_slow_final_route(int conn_fd,
                                                        bool chunked) {
  const char *head =
      chunked ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n"
              : "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  const char *tail = chunked ? "5\r\nworld\r\n0\r\n\r\n" : "world";
  send(conn_fd, head, strlen(head), MSG_NOSIGNAL);
  usleep(1300000);
  send(conn_fd, tail, strlen(tail), MSG_NOSIGNAL);
  int seen = 0;
  for (;;) {
    struct pollfd p = {.fd = conn_fd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 2000) <= 0) break;
    char b[256];
    ssize_t m = recv(conn_fd, b, sizeof(b), 0);
    if (m <= 0) break;
    seen += (int)m;
  }
  atomic_fetch_add(&g_slow_final_body_bytes, seen);
  atomic_fetch_add(&g_slow_final_done, 1);
  return true;
}

// A connection thread serves at most this many requests, so a misbehaving
// client can't keep it for ever.
#define MAX_KEEPALIVE_REQUESTS_PER_CONN 100

// The connections that the plain and the TLS mock servers accepted and haven't
// closed yet. A test that counts open descriptors waits for this to settle
// first (settled_fd_baseline).
static atomic_int g_mock_conns_open = 0;

static void *srv_conn_thread_body(void *arg);

// The thread function of one mock server connection (fd in arg). It serves the
// connection and then drops g_mock_conns_open, so a test can wait until the
// server ends of its connections are closed.
static void *srv_conn_thread(void *arg) {
  void *r = srv_conn_thread_body(arg);
  atomic_fetch_sub(&g_mock_conns_open, 1);
  return r;
}

// Serves the connection whose fd is arg: Reads request after request and hands
// each to its route, for as long as the route keeps the connection open, then
// closes the fd.
static void *srv_conn_thread_body(void *arg) {
  int conn_fd = (int)(intptr_t)arg;
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (!buf) {
    close(conn_fd);
    return NULL;
  }

  for (int iter = 0; iter < MAX_KEEPALIVE_REQUESTS_PER_CONN; iter++) {
    size_t hdr_len = 0;
    ssize_t n = srv_read_headers(conn_fd, buf, TEST_SERVER_BUF, &hdr_len);
    if (n <= 0) break;

    char method[16], path[512];
    srv_parse_request_line(buf, method, sizeof(method), path, sizeof(path));

    bool close_after;
    if (strcmp(path, "/expect-continue-echo") == 0) {
      close_after = srv_handle_expect_continue_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, true);
    } else if (strcmp(path, "/expect-continue-reject") == 0) {
      close_after = srv_handle_expect_continue_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, false);
    } else if (strcmp(path, "/expect-continue-slow-final") == 0) {
      close_after = srv_handle_expect_continue_slow_final_route(conn_fd, false);
    } else if (strcmp(path, "/expect-continue-slow-final-chunked") == 0) {
      close_after = srv_handle_expect_continue_slow_final_route(conn_fd, true);
    } else if (strcmp(path, "/expect-continue-timeout-then-die") == 0) {
      close_after = srv_handle_expect_continue_timeout_then_die_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-with-hints") == 0) {
      close_after = srv_handle_expect_continue_with_hints_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strcmp(path, "/expect-continue-reject-keepalive") == 0) {
      close_after = srv_handle_expect_continue_reject_keepalive_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-fake-final-then-die") == 0) {
      close_after =
          srv_handle_expect_continue_fake_final_then_die_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-die-after-100-clean") == 0) {
      close_after = srv_handle_expect_continue_die_after_100_clean_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strcmp(path, "/expect-continue-bundled-final") == 0) {
      close_after = srv_handle_expect_continue_bundled_final_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strncmp(path, "/expect-continue-hints-both-sides/",
                       strlen("/expect-continue-hints-both-sides/")) == 0) {
      close_after = srv_handle_expect_continue_hints_both_sides_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, path);
    } else {
      size_t total = (size_t)n;
      char cl_str[32] = {0};
      long cl = 0;
      if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
        cl = atol(cl_str);
      while (cl > 0 && total - hdr_len < (size_t)cl &&
             total < TEST_SERVER_BUF - 1) {
        ssize_t m = recv(conn_fd, buf + total, TEST_SERVER_BUF - 1 - total, 0);
        if (m <= 0) break;
        total += (size_t)m;
        buf[total] = '\0';
      }

      char *body = (cl > 0) ? buf + hdr_len : NULL;
      size_t body_len = (cl > 0) ? (size_t)cl : 0;
      close_after =
          srv_handle_route(conn_fd, method, path, buf, body, body_len);
    }
    if (close_after) break;
  }
  free(buf);
  close(conn_fd);
  return NULL;
}

// Counts failed accept(2) calls of the test servers.
static atomic_int g_accept_failures = 0;

// Decides what an accept loop does after accept(2) failed, and counts the
// failure in g_accept_failures. A loop that ended on a passing failure would
// leave the listener open with nobody accepting, and every later request would
// wait for ever. Gives false once g_srv.running is clear, true otherwise; an
// error that can repeat at once waits 1 ms first.
static bool srv_accept_failed_keep_going(const char *which) {
  int e = errno;
  atomic_fetch_add(&g_accept_failures, 1);
  if (!atomic_load(&g_srv.running)) return false;
  fprintf(stderr, "[ACCEPT_DIAG] %s accept failed: errno %d (%s), retrying\n",
          which, e, strerror(e));
  if (e != EINTR && e != ECONNABORTED) {
    struct timespec nap = {0, 1000000};
    nanosleep(&nap, NULL);
  }
  return true;
}

// Accepts connections on the IPv4 listener for the whole process, counts them
// in g_accept_count and starts a registered thread for each, until
// g_srv.running clears.
static void *srv_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = test_accept(g_srv.server_fd);
    if (conn_fd < 0) {
      if (srv_accept_failed_keep_going("tcp4")) continue;
      break;
    }
    if (!atomic_load(&g_srv.running)) {
      close(conn_fd);
      break;
    }
    atomic_fetch_add(&g_accept_count, 1);
    atomic_fetch_add(&g_mock_conns_open, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
      atomic_fetch_sub(&g_mock_conns_open, 1);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

// Accepts connections on the IPv6 listener, for the tests of [::1] URLs. It
// shares g_srv.running, the thread registry and the handler with
// srv_accept_loop.
static void *srv_accept_loop6(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = test_accept(g_srv.server_fd6);
    if (conn_fd < 0) {
      if (srv_accept_failed_keep_going("tcp6")) continue;
      break;
    }
    if (!atomic_load(&g_srv.running)) {
      close(conn_fd);
      break;
    }
    atomic_fetch_add(&g_accept_count, 1);
    atomic_fetch_add(&g_mock_conns_open, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
      atomic_fetch_sub(&g_mock_conns_open, 1);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

// Accepts connections on the Unix domain socket listener, for the http+unix://
// tests, like srv_accept_loop.
static void *srv_accept_loop_unix(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = test_accept(g_srv.server_fd_unix);
    if (conn_fd < 0) {
      if (srv_accept_failed_keep_going("unix")) continue;
      break;
    }
    if (!atomic_load(&g_srv.running)) {
      close(conn_fd);
      break;
    }
    atomic_fetch_add(&g_accept_count, 1);
    atomic_fetch_add(&g_mock_conns_open, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
      atomic_fetch_sub(&g_mock_conns_open, 1);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

// Starts the mock server, once, from the first get_test_port() or
// get_test_unix_socket_path(). Binds the three listeners into g_srv and starts
// their accept loops. The IPv4 listener is required; IPv6 and Unix are skipped
// when they can't bind.
static void start_test_server(void) {
  g_srv.server_fd6 = -1;
  g_srv.server_fd_unix = -1;

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

  snprintf(g_srv.unix_path, sizeof(g_srv.unix_path),
           "/tmp/chttpcli_test_%d.sock", (int)getpid());
  unlink(g_srv.unix_path);

  int fd_unix = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_unix >= 0) {
    struct sockaddr_un addr_un;
    memset(&addr_un, 0, sizeof(addr_un));
    addr_un.sun_family = AF_UNIX;
    size_t path_len = strlen(g_srv.unix_path);
    bool bound = false;
    if (path_len < sizeof(addr_un.sun_path)) {
      memcpy(addr_un.sun_path, g_srv.unix_path, path_len + 1);
      bound =
          (bind(fd_unix, (struct sockaddr *)&addr_un, sizeof(addr_un)) == 0 &&
           listen(fd_unix, 64) == 0);
    }
    if (bound) {
      g_srv.server_fd_unix = fd_unix;
      if (pthread_create(&g_srv.accept_tid_unix, NULL, srv_accept_loop_unix,
                         NULL) != 0) {
        close(fd_unix);
        g_srv.server_fd_unix = -1;
        unlink(g_srv.unix_path);
      }
    } else {
      close(fd_unix);
    }
  }
}

static void test_wake_listener(int lfd);

// Stops the mock server at exit, so no thread of it outlives the tests: Stops
// the accept loops, closes the listeners, removes the socket file and joins
// every connection thread left in g_conn_threads.
__attribute__((destructor)) static void stop_test_server(void) {
  if (g_srv.server_fd > 0) {
    atomic_store(&g_srv.running, 0);
    test_wake_listener(g_srv.server_fd);
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    pthread_join(g_srv.accept_tid, NULL);
  }
  if (g_srv.server_fd6 > 0) {
    test_wake_listener(g_srv.server_fd6);
    close(g_srv.server_fd6);
    g_srv.server_fd6 = -1;
    pthread_join(g_srv.accept_tid6, NULL);
  }
  if (g_srv.server_fd_unix > 0) {
    test_wake_listener(g_srv.server_fd_unix);
    close(g_srv.server_fd_unix);
    g_srv.server_fd_unix = -1;
    pthread_join(g_srv.accept_tid_unix, NULL);
    unlink(g_srv.unix_path);
  }
  pthread_mutex_lock(&g_conn_mutex);
  int n = g_conn_thread_count;
  pthread_mutex_unlock(&g_conn_mutex);
  for (int i = 0; i < n; i++) pthread_join(g_conn_threads[i], NULL);
}

// Gives the port of the IPv4 listener, which every make_url() names; starts
// the server on first use.
static int get_test_port(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port;
}

// Gives the port of the IPv6 listener for the [::1] tests, or 0 when it
// couldn't bind; a test that needs it checks that and skips itself.
static int get_test_port6(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port6;
}

// Gives how many TCP connections the mock server has accepted
// (g_accept_count), so a test can tell a reused connection from a fresh one.
static int test_server_accept_count(void) {
  return atomic_load(&g_accept_count);
}

// Writes http://127.0.0.1:<port><path> into buf, the URL most tests send their
// request to.
static void make_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://127.0.0.1:%d%s", get_test_port(), path);
}

// Writes http://[::1]:<port><path> into buf, for the IPv6 tests. Call it only
// when get_test_port6() isn't 0.
static void make_url6(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://[::1]:%d%s", get_test_port6(), path);
}

// Gives the path of the Unix domain socket the mock server listens on, or ""
// if it couldn't bind, so the http+unix:// tests can build their URLs; starts
// the server on first use.
static const char *get_test_unix_socket_path(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.unix_path;
}

// Percent-encodes the filesystem path raw into buf, for the authority of a
// http+unix:// URL: Unreserved characters stay, every other byte ('/' included)
// becomes %XX. It's the test-side copy of the library's static helper of the
// same name, which the mock server's Unix redirects and make_unix_url need.
static void percent_encode_unix_path(char *buf, size_t buf_size,
                                     const char *raw) {
  size_t w = 0;
  for (const char *p = raw; *p && w + 4 < buf_size; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      buf[w++] = (char)c;
    } else {
      snprintf(buf + w, 4, "%%%02X", c);
      w += 3;
    }
  }
  buf[w] = '\0';
}

// Writes http+unix://<encoded socket path><path> into buf, the URL of the
// http+unix:// tests.
static void make_unix_url(char *buf, size_t buf_size, const char *path) {
  char encoded[256];
  percent_encode_unix_path(encoded, sizeof(encoded),
                           get_test_unix_socket_path());
  snprintf(buf, buf_size, "http+unix://%s%s", encoded, path);
}

// Request construction
TEST(request, new_copies_url_and_body) {
  const char *json = "{\"x\":1}";
  chttp_request_body body = CHTTP_JSON_BODY(json, strlen(json));

  chttp_request *req =
      chttp_request_create(CHTTP_POST, "http://example.com/api", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_STREQ(req->url, "http://example.com/api");
  REQUIRE_EQ(req->method, CHTTP_POST);
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, strlen(json));
  REQUIRE_STREQ(req->body.content_type, "application/json");
  REQUIRE_NE((void *)req->body.data, (void *)json);

  chttp_request_free(req);
}

TEST(request, new_null_body) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  chttp_request_free(req);
}

TEST(request, new_no_body_macro) {
  chttp_request_body nb = CHTTP_NO_BODY;
  chttp_request *req =
      chttp_request_create(CHTTP_DELETE, "http://example.com/r", &nb, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  chttp_request_free(req);
}

TEST(request, new_null_url_fails) {
  char *err = NULL;
  chttp_request *req =
      chttp_request_create_mp(CHTTP_GET, NULL, NULL, NULL, &err);
  REQUIRE_EQ((void *)req, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(request, set_and_get_header) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval rv =
      chttp_request_set_header(req, "Authorization", "Bearer token123");
  REQUIRE_EQ(rv, ccol_success);

  const char *val = chttp_request_get_header(req, "Authorization");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "Bearer token123");

  chttp_request_free(req);
}

TEST(request, header_lookup_case_insensitive) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "Content-Type", "application/json");

  REQUIRE_NE((void *)chttp_request_get_header(req, "content-type"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "CONTENT-TYPE"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "Content-Type"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "content-type"),
                "application/json");

  chttp_request_free(req);
}

TEST(request, set_header_overwrites) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "X-Custom", "first");
  chttp_request_set_header(req, "X-Custom", "second");

  const char *val = chttp_request_get_header(req, "x-custom");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "second");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_crlf_in_name) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "x-evil\r\nx-injected", "1"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-evil\r\nx-injected"),
             NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-injected"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_crlf_in_value) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "x-evil", "1\r\nx-injected: evil"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-evil"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_empty_name) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "", "v"), ccol_invalid_args);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_non_tchar_name) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ(chttp_request_set_header(req, "X Foo", "bar"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "X:Foo", "bar"), ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X Foo"), NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X:Foo"), NULL);

  REQUIRE_EQ(chttp_request_set_header(req, "x-legit!#$%&'*+-.^_`|~", "fine"),
             ccol_success);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit!#$%&'*+-.^_`|~"),
                "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_transfer_encoding) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "Transfer-Encoding", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "transfer-encoding", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "TRANSFER-ENCODING", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "transfer-encoding"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, new_null_body_data_with_nonzero_len_fails) {
  chttp_request_body bad_body = {.data = NULL, .len = 5, .content_type = NULL};
  char *err = NULL;
  chttp_request *req = chttp_request_create_mp(
      CHTTP_POST, "http://example.com/", &bad_body, NULL, &err);
  REQUIRE_EQ((void *)req, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(request, get_missing_header_returns_null) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X-Missing"), NULL);
  chttp_request_free(req);
}

TEST(request, headers_begin_iterator) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
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
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)ccol_begin(req->headers), NULL);
  chttp_request_free(req);
}

TEST(request, free_null_safe) { chttp_request_free(NULL); }

// Client construction
TEST(client_construction, create_and_destroy) {
  char *err = NULL;
  chttpcli cli = chttpcli_create(&err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttpcli_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_NULL);
}

// An allocator for the construction sweeps. It fails the call whose index is
// g_cf_fail_at (-1: None), counts every call in g_cf_calls, records in
// g_cf_failed that it failed one, and keeps in g_cf_live the blocks it handed
// out that nobody gave back, so a sweep can tell a clean failure from a leak.
static atomic_int g_cf_calls;
static atomic_int g_cf_fail_at = -1;
static atomic_bool g_cf_failed;
static atomic_long g_cf_live;

// Counts one call in g_cf_calls and tells whether it's the one at g_cf_fail_at,
// which the cf allocator then fails (and records in g_cf_failed).
static bool cf_should_fail(void) {
  int idx = atomic_fetch_add(&g_cf_calls, 1);
  if (idx != atomic_load(&g_cf_fail_at)) return false;
  atomic_store(&g_cf_failed, true);
  return true;
}
static void *cf_malloc(size_t sz) {
  if (cf_should_fail()) return NULL;
  void *p = malloc(sz);
  if (p) atomic_fetch_add(&g_cf_live, 1);
  return p;
}
static void *cf_calloc(size_t n, size_t sz) {
  if (cf_should_fail()) return NULL;
  void *p = calloc(n, sz);
  if (p) atomic_fetch_add(&g_cf_live, 1);
  return p;
}
static void *cf_realloc(void *old, size_t sz) {
  if (cf_should_fail()) return NULL;
  void *p = realloc(old, sz);
  if (p && !old) atomic_fetch_add(&g_cf_live, 1);
  return p;
}
static void cf_free(void *p) {
  if (p) atomic_fetch_sub(&g_cf_live, 1);
  free(p);
}
static ccol_memmgmt_procs g_cf_mp = {.malloc = cf_malloc,
                                     .free = cf_free,
                                     .calloc = cf_calloc,
                                     .realloc = cf_realloc};

// Arms the cf allocator for one step of a sweep: Resets the call counter and
// the failed flag and makes the call with index fail_at fail.
static void cf_arm(int fail_at) {
  atomic_store(&g_cf_calls, 0);
  atomic_store(&g_cf_failed, false);
  atomic_store(&g_cf_fail_at, fail_at);
}

// Fails each allocation of chttpcli_create_mp in turn. Each failed create must
// return CHTTPCLI_NULL with an err_str and give back every block; the first
// index that no allocation reaches gives a client that must serve a request.
TEST(client_construction, a_create_whose_allocation_fails_frees_everything) {
  enum { SWEEP_MAX = 400 };
  int refused = 0, survived = 0, no_err = 0, leaked = 0;
  bool reached_the_end = false;
  for (int idx = 0; idx < SWEEP_MAX && !reached_the_end; idx++) {
    atomic_store(&g_cf_live, 0);
    cf_arm(idx);
    char *err = NULL;
    chttpcli cli = chttpcli_create_mp(&g_cf_mp, &err);
    bool failed = atomic_load(&g_cf_failed);
    cf_arm(-1);
    if (cli == CHTTPCLI_NULL) {
      refused++;
      if (!err) no_err++;
    } else {
      if (failed)
        survived++;
      else
        reached_the_end = true;
      chttpcli_destroy(cli);
    }
    if (atomic_load(&g_cf_live) != 0) leaked++;
  }

  char url[160];
  make_url(url, sizeof(url), "/get");
  atomic_store(&g_cf_live, 0);
  char *err = NULL;
  chttpcli cli = chttpcli_create_mp(&g_cf_mp, &err);
  int status = -1;
  ccol_retval rv = ccol_unexpected_failure;
  if (cli != CHTTPCLI_NULL) {
    chttp_request *req =
        chttp_request_create_mp(CHTTP_GET, url, NULL, &g_cf_mp, NULL);
    chttpcli_response *resp = NULL;
    if (req) rv = chttpcli_do(cli, req, &resp);
    if (rv == ccol_success) {
      status = resp->status_code;
      chttpcli_resp_free(resp);
    }
    chttp_request_free(req);
    chttpcli_destroy(cli);
  }
  long live_after_use = atomic_load(&g_cf_live);

  REQUIRE_TRUE(reached_the_end);
  REQUIRE_GT(refused, 3);
  REQUIRE_EQ(no_err, 0);
  REQUIRE_EQ(leaked, 0);
  REQUIRE_EQ(survived, 0);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(live_after_use, 0L);
}

// Makes each mutex and condition variable init of chttpcli_create_mp fail in
// turn. The create must fail with the matching err_str, destroy what it already
// initialized and free every block, and the next create must work.
TEST(client_construction,
     a_create_whose_primitive_init_fails_frees_everything) {
  static const struct {
    chttpcli__fault step;
    const char *err;
  } cases[] = {
      {CHTTPCLI__FAULT_LOCK, "failed to initialize mutex"},
      {CHTTPCLI__FAULT_AVAILABLE_CV, "failed to initialize condition variable"},
      {CHTTPCLI__FAULT_IDLE_ASYNC_DRAINED_CV,
       "failed to initialize condition variable"},
      {CHTTPCLI__FAULT_ASYNC_COUNT_LOCK, "failed to initialize mutex"},
      {CHTTPCLI__FAULT_ASYNC_COUNT_DRAINED_CV,
       "failed to initialize condition variable"},
  };
  enum { N = sizeof(cases) / sizeof(cases[0]) };
  bool refused[N], err_ok[N];
  long live[N];
  for (size_t i = 0; i < N; i++) {
    atomic_store(&g_cf_live, 0);
    char *err = NULL;
    chttpcli__arm_create_fault_for_tests(cases[i].step);
    chttpcli cli = chttpcli_create_mp(&g_cf_mp, &err);
    chttpcli__arm_create_fault_for_tests(CHTTPCLI__FAULT_NONE);
    refused[i] = cli == CHTTPCLI_NULL;
    if (!refused[i]) chttpcli_destroy(cli);
    err_ok[i] = err && strstr(err, cases[i].err) != NULL;
    live[i] = atomic_load(&g_cf_live);
  }
  chttpcli cli = chttpcli_create_mp(&g_cf_mp, NULL);
  bool works =
      cli != CHTTPCLI_NULL && chttpcli_set_pool_size(cli, 2) == ccol_success;
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);

  for (size_t i = 0; i < N; i++) {
    REQUIRE_TRUE(refused[i]);
    REQUIRE_TRUE(err_ok[i]);
    REQUIRE_EQ(live[i], 0L);
  }
  REQUIRE_TRUE(works);
}

TEST(client_construction, a_create_with_an_incomplete_procs_table_is_refused) {
  ccol_memmgmt_procs no_free = g_cf_mp;
  no_free.free = NULL;
  cf_arm(-1);
  char *err = NULL;
  chttpcli cli = chttpcli_create_mp(&no_free, &err);
  int calls = atomic_load(&g_cf_calls);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ(calls, 0);
}

// Fails each allocation of chttp_request_create_mp in turn, for a request with
// a body and a content type so every copy is reached.
TEST(client_construction, a_request_whose_allocation_fails_frees_everything) {
  static const char url[] = "http://127.0.0.1:1/x";
  static const char payload[] = "{\"a\":1}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, sizeof(payload) - 1);
  enum { SWEEP_MAX = 32 };
  int refused = 0, no_err = 0, leaked = 0;
  chttp_request *req = NULL;
  for (int idx = 0; idx < SWEEP_MAX && !req; idx++) {
    atomic_store(&g_cf_live, 0);
    cf_arm(idx);
    char *err = NULL;
    chttp_request *r =
        chttp_request_create_mp(CHTTP_POST, url, &body, &g_cf_mp, &err);
    bool failed = atomic_load(&g_cf_failed);
    cf_arm(-1);
    if (!r) {
      refused++;
      if (!err) no_err++;
      if (atomic_load(&g_cf_live) != 0) leaked++;
    } else if (failed) {
      leaked++;
      chttp_request_free(r);
    } else {
      req = r;
    }
  }
  bool copies_ok =
      req && req->url != url && strcmp(req->url, url) == 0 &&
      req->body.data != payload && req->body.len == sizeof(payload) - 1 &&
      memcmp(req->body.data, payload, req->body.len) == 0 &&
      req->body.content_type && req->body.content_type != body.content_type &&
      strcmp(req->body.content_type, body.content_type) == 0;
  chttp_request_free(req);
  long live_after_free = atomic_load(&g_cf_live);

  REQUIRE_EQ(refused, 5);
  REQUIRE_EQ(no_err, 0);
  REQUIRE_EQ(leaked, 0);
  REQUIRE_TRUE(copies_ok);
  REQUIRE_EQ(live_after_free, 0L);
}

TEST(client_construction, construct_macro) {
  chttpcli_construct(cli);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttpcli_destroy(cli);
}

TEST(client_construction, construct_scoped_macro) {
  {
    chttpcli_construct_scoped(cli);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
  }
}

TEST(client_construction, declare_and_init) {
  chttpcli_declare(cli);
  char *err = NULL;
  cli = chttpcli_create(&err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttpcli_destroy(cli);
}

TEST(client_construction, set_configuration) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 4), ccol_success);
  REQUIRE_EQ(chttpcli_set_connect_timeout(cli, 5000000), ccol_success);
  REQUIRE_EQ(chttpcli_set_request_timeout(cli, 30000000), ccol_success);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpcli_set_tls(cli, NULL), ccol_success);
  chttpcli_destroy(cli);
}

TEST(client_construction, null_args) {
  REQUIRE_EQ(chttpcli_set_pool_size(CHTTPCLI_NULL, 4), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_set_connect_timeout(CHTTPCLI_NULL, 0), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_set_request_timeout(CHTTPCLI_NULL, 0), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_set_tls(CHTTPCLI_NULL, NULL), ccol_invalid_args);
}

TEST(client_construction, half_configured_client_cert_pair_rejected) {
  chttpcli_construct(cli);

  chttp_tls_config cert_only = CHTTP_TLS_DEFAULT;
  cert_only.cert_path = "/nonexistent/cert.pem";
  REQUIRE_EQ(chttpcli_set_tls(cli, &cert_only), ccol_invalid_args);

  chttp_tls_config key_only = CHTTP_TLS_DEFAULT;
  key_only.key_path = "/nonexistent/key.pem";
  REQUIRE_EQ(chttpcli_set_tls(cli, &key_only), ccol_invalid_args);

  chttp_tls_config both = CHTTP_TLS_DEFAULT;
  both.cert_path = "/nonexistent/cert.pem";
  both.key_path = "/nonexistent/key.pem";
  REQUIRE_EQ(chttpcli_set_tls(cli, &both), ccol_success);

  chttpcli_destroy(cli);
}

// HTTP traffic
TEST(http, get_200) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  chttpcli_resp_free(resp);
}

TEST(http, eof_delimited_body_without_content_length) {
  char url[128];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "eof-delimited-body-ok");
  chttpcli_resp_free(resp);
}

TEST(http, post_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"hello\":\"world\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);
  chttpcli_resp_free(resp);
}

TEST(http, post_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpcli_resp_free(resp);
}

TEST(http, put_200) {
  char url[128];
  make_url(url, sizeof(url), "/put");

  const char *data = "updated";
  chttp_request_body body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_put(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(http, delete_204) {
  char url[128];
  make_url(url, sizeof(url), "/delete");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_delete(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 204);
  chttpcli_resp_free(resp);
}

TEST(http, patch_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  const char *data = "patch-data";
  chttp_request_body body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_patch(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, data);
  chttpcli_resp_free(resp);
}

TEST(http, patch_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_patch(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpcli_resp_free(resp);
}

TEST(http, response_headers) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *val = chttpcli_resp_header(resp, "X-Chttp-Test");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "hello");

  chttpcli_resp_free(resp);
}

TEST(http, response_header_lookup_case_insensitive) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);

  REQUIRE_NE((void *)chttpcli_resp_header(resp, "x-chttp-test"), NULL);
  REQUIRE_NE((void *)chttpcli_resp_header(resp, "X-CHTTP-TEST"), NULL);
  REQUIRE_NE((void *)chttpcli_resp_header(resp, "X-Chttp-Test"), NULL);

  chttpcli_resp_free(resp);
}

TEST(http, request_headers_forwarded) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Echo", "test-value-42");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "test-value-42");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, not_found_404) {
  char url[128];
  make_url(url, sizeof(url), "/no-such-route");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

TEST(http, status_route_custom_code) {
  char url[128];
  make_url(url, sizeof(url), "/status/418");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 418);
  chttpcli_resp_free(resp);
}

TEST(http, follows_redirect) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(http, redirect_does_not_leak_intermediate_headers) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  REQUIRE_EQ((void *)chttpcli_resp_header(resp, "location"), NULL);

  REQUIRE_NE((void *)chttpcli_resp_header(resp, "content-type"), NULL);

  chttpcli_resp_free(resp);
}

TEST(http, redirect_with_empty_location_header_reported_cleanly) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-empty-location");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 301);
  chttpcli_resp_free(resp);
}

TEST(http, redirect_to_unsupported_scheme_location_reports_invalid_url) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-unsupported-scheme-location");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(http, default_client_convenience) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli def = chttp_default_client();
  REQUIRE_NE(def, CHTTPCLI_NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(def, NULL, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  rv = chttpcli_do(def, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, custom_client_with_pool_size) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 2), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

// Streaming response
//
// A write_fn sink that keeps up to 4 KiB of the body, NUL-terminated.
typedef struct {
  char buf[4096];
  size_t len;
} stream_sink_t;

// The write_fn of the streaming tests: Appends what fits of the chunk to the
// stream_sink_t in ctx and reports the whole chunk as taken.
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

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  int status = 0;
  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

TEST(http, streaming_through_redirect_delivers_final_body_not_intermediate) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  int status = 0;
  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(sink.buf, "{\"status\":\"ok\"}");

  chttp_request_free(req);
}

TEST(http, streaming_null_args) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  REQUIRE_EQ(chttpcli_do_streaming(CHTTPCLI_NULL, req, stream_sink_write, &sink,
                                   &status),
             ccol_invalid_args);
  REQUIRE_EQ(chttpcli_do_streaming(chttp_default_client(), NULL,
                                   stream_sink_write, &sink, &status),
             ccol_invalid_args);
  REQUIRE_EQ(
      chttpcli_do_streaming(chttp_default_client(), req, NULL, &sink, &status),
      ccol_invalid_args);
  chttp_request_free(req);
}

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

  chttpcli_resp_free(resp);
}

// Concurrent requests
//
// The input and the outcome of one request thread.
typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval result_rv;
} concurrent_req_arg_t;

// Makes one GET on a shared client from its own thread, for the tests that run
// requests concurrently or race them against a destroy. Records the status and
// the code in the concurrent_req_arg_t in arg.
static void *concurrent_req_thread(void *arg) {
  concurrent_req_arg_t *a = (concurrent_req_arg_t *)arg;
  chttp_request *req = chttp_request_create(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }

  chttpcli_response *resp = NULL;
  a->result_rv = chttpcli_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

// A caller that races chttpcli__destroy. The probe sets ready just before it
// calls chttpcli_do. Either it blocks waiting for a pool slot and the destroy's
// broadcast wakes it, or it sees the destroy first; both return
// ccol_not_permitted.
typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval result_rv;
  atomic_int ready;
} probe_arg_t;

// The racing caller of pool.do_returns_not_permitted_when_destroying: Signals
// ready, makes one GET and records the outcome in the probe_arg_t in arg.
static void *probe_thread(void *arg) {
  probe_arg_t *a = (probe_arg_t *)arg;
  chttp_request *req = chttp_request_create(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }
  atomic_store(&a->ready, 1);
  chttpcli_response *resp = NULL;
  a->result_rv = chttpcli_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

// Destroys the client that arg points to, so a test can race the destroy
// against requests on other threads.
static void *do_destroy_thread(void *arg) {
  chttpcli h = *(chttpcli *)arg;
  chttpcli__destroy(h);
  return NULL;
}

TEST(http, concurrent_requests) {
  enum { NTHREADS = 8 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, NTHREADS), ccol_success);

  concurrent_req_arg_t args[NTHREADS];
  pthread_t threads[NTHREADS];

  int started = 0;
  for (int i = 0; i < NTHREADS; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }

  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, NTHREADS);
  for (int i = 0; i < NTHREADS; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpcli_destroy(cli);
}

// Pool resize
TEST(pool, grow_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 1), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  REQUIRE_EQ(chttpcli_set_pool_size(cli, 4), ccol_success);

  req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  chttpcli_destroy(cli);
}

TEST(http, head_200) {
  char url[128];
  make_url(url, sizeof(url), "/head");

  chttp_request *req = chttp_request_create(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, options_200) {
  char url[128];
  make_url(url, sizeof(url), "/options");

  chttp_request *req = chttp_request_create(CHTTP_OPTIONS, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *allow = chttpcli_resp_header(resp, "allow");
  REQUIRE_NE((void *)allow, NULL);
  REQUIRE_STREQ(allow, "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, large_body_response) {
  char url[128];
  make_url(url, sizeof(url), "/large");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_EQ((int)resp->body[0], (int)'x');
  REQUIRE_EQ((int)resp->body[4095], (int)'x');
  REQUIRE_EQ((int)resp->body[8191], (int)'x');

  chttpcli_resp_free(resp);
}

// The Tier 1 read loop waits for readability only before its first read and
// after a read that blocked. A loop that waits before every read waits at least
// as often as it reads, and fails here.
TEST(http, a_large_body_is_read_with_fewer_waits_than_reads) {
  char url[128];
  make_url(url, sizeof(url), "/huge");

  unsigned long reads0 = chttp__tier1_reads_for_tests();
  unsigned long waits0 = chttp__tier1_read_waits_for_tests();
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  unsigned long reads = chttp__tier1_reads_for_tests() - reads0;
  unsigned long waits = chttp__tier1_read_waits_for_tests() - waits0;
  size_t body_len = resp ? resp->body_len : 0;
  bool all_h = resp && resp->body && body_len == 512 * 1024 &&
               resp->body[0] == 'h' && resp->body[body_len - 1] == 'h';
  if (resp) chttpcli_resp_free(resp);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(all_h);
  REQUIRE_GT(reads, 1UL);
  REQUIRE_LT(waits, reads);
}

TEST(http, content_type_auto_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "application/json");

  chttpcli_resp_free(resp);
}

TEST(http, content_type_explicit_header_not_duplicated) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "text";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "Content-Type", "text/csv");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/csv");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, streaming_post) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"stream\":true}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(sink.len, strlen(payload));
  REQUIRE_STREQ(sink.buf, payload);

  chttp_request_free(req);
}

TEST(http, streaming_post_content_type_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(sink.buf, "application/json");

  chttp_request_free(req);
}

TEST(http, streaming_null_status_code_out) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

TEST(pool, shrink_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 4), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  REQUIRE_EQ(chttpcli_set_pool_size(cli, 2), ccol_success);

  req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  chttpcli_destroy(cli);
}

TEST(pool, idle_handles_freed_on_shrink) {
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, POOL_LARGE), ccol_success);

  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  int started = 0;
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, POOL_LARGE);
  for (int i = 0; i < POOL_LARGE; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  REQUIRE_EQ(chttpcli_set_pool_size(cli, POOL_SMALL), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  chttpcli_destroy(cli);
}

// Gives the milliseconds since t0 on CLOCK_MONOTONIC, rounded down from the
// full nanosecond difference. The timing tests use it to check how long a
// request took.
static long elapsed_ms_since(const struct timespec *t0) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long ns = (long long)(t1.tv_sec - t0->tv_sec) * 1000000000LL +
                 (long long)(t1.tv_nsec - t0->tv_nsec);
  return (long)(ns / 1000000LL);
}

TEST(pool, shrink_while_in_flight_exiles_slots) {
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/slow");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, POOL_LARGE), ccol_success);

  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  int started = 0;
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }

  struct timespec slow_wait_start;
  clock_gettime(CLOCK_MONOTONIC, &slow_wait_start);
  while (atomic_load(&g_slow_started) < started &&
         elapsed_ms_since(&slow_wait_start) < 10000) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&ts, NULL);
  }

  ccol_retval set_pool_size_rv = chttpcli_set_pool_size(cli, POOL_SMALL);

  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

  REQUIRE_EQ(started, POOL_LARGE);
  REQUIRE_EQ(set_pool_size_rv, ccol_success);
  for (int i = 0; i < POOL_LARGE; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  chttpcli_destroy(cli);
}

// Invalid arguments
TEST(invalid_args, chttpcli_do_null_checks) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(CHTTPCLI_NULL, req, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_do(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_do(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(invalid_args, chttp_convenience_null_checks) {
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_post(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_put(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_get("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_post("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_put("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch("http://x.com/", NULL, NULL), ccol_invalid_args);
}

TEST(invalid_args, resp_header_null_checks) {
  REQUIRE_EQ((void *)chttpcli_resp_header(NULL, "X-Foo"), NULL);
  chttpcli_resp_free(NULL);
}

TEST(invalid_args, request_set_header_null_checks) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://x.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(NULL, "X", "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, NULL, "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "X", NULL), ccol_invalid_args);
  chttp_request_free(req);
}

// Custom allocator
//
// An allocator that counts its allocations in g_alloc_count and its frees in
// g_free_count; a realloc of an existing block counts as one of each. The
// custom_allocator tests use it to check that the client allocates only through
// the caller's procs and frees everything it allocated.
static atomic_int g_alloc_count = 0;
static atomic_int g_free_count = 0;

// The entry points of the counting allocator; each counts its call and passes
// it to the C library.
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
  if (p) atomic_fetch_add(&g_free_count, 1);
  atomic_fetch_add(&g_alloc_count, 1);
  return realloc(p, sz);
}

TEST(custom_allocator, allocations_go_through_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};

  char *err = NULL;
  const char *url = "http://example.com/";
  chttp_request *req = chttp_request_create_mp(CHTTP_GET, url, NULL, &mp, &err);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Custom", "value");
  REQUIRE_NE((void *)chttp_request_get_header(req, "x-custom"), NULL);
  chttp_request_free(req);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));

  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  err = NULL;
  chttpcli cli = chttpcli_create_mp(&mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttpcli_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

// Body buffer out of memory
//
// An allocator whose realloc fails for 4096 bytes or more, so the body buffer
// test can fail exactly the body growth: The buffer grows from 4096 while a
// small GET never needs that much. Every other call goes to the C library.
static void *bodybuf_oom_malloc(size_t sz) { return malloc(sz); }
static void *bodybuf_oom_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void bodybuf_oom_free(void *p) { free(p); }
static void *bodybuf_oom_realloc(void *p, size_t sz) {
  if (sz >= 4096) return NULL;
  return realloc(p, sz);
}
static ccol_memmgmt_procs g_bodybuf_oom_mp = {.malloc = bodybuf_oom_malloc,
                                              .free = bodybuf_oom_free,
                                              .calloc = bodybuf_oom_calloc,
                                              .realloc = bodybuf_oom_realloc};

TEST(body_buffer_oom, realloc_failure_reports_not_enough_memory) {
  char *cerr = NULL;
  chttpcli cli = chttpcli_create_mp(&g_bodybuf_oom_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req =
      chttp_request_create_mp(CHTTP_GET, url, NULL, &g_bodybuf_oom_mp, &cerr);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

// PUT and PATCH with a content type
TEST(http, put_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/plain");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, put_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "raw";
  chttp_request_body body = {.data = payload, .len = 3, .content_type = NULL};

  chttp_request *req = chttp_request_create(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, put_body_round_trip) {
  char url[128];
  make_url(url, sizeof(url), "/put-echo");

  const char *payload = "hello-put";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, put_without_body) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  chttp_request *req = chttp_request_create(CHTTP_PUT, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, patch_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "delta";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "application/json");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, patch_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "patch";
  chttp_request_body body = {.data = payload, .len = 5, .content_type = NULL};

  chttp_request *req = chttp_request_create(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

// TLS configuration
TEST(tls, set_tls_deep_copies_strings) {
  chttpcli_construct(cli);

  {
    char cert[64], key[64], ca[64];
    memcpy(cert, "/tmp/test.crt", sizeof("/tmp/test.crt"));
    memcpy(key, "/tmp/test.key", sizeof("/tmp/test.key"));
    memcpy(ca, "/tmp/ca.pem", sizeof("/tmp/ca.pem"));

    chttp_tls_config tls = {
        .cert_path = cert,
        .key_path = key,
        .ca_bundle_path = ca,
    };
    REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

    memset(cert, 0xff, sizeof(cert));
    memset(key, 0xff, sizeof(key));
    memset(ca, 0xff, sizeof(ca));
  }

  chttp_tls_config tls2 = {
      .cert_path = "/tmp/other.crt",
      .key_path = "/tmp/other.key",
      .ca_bundle_path = NULL,
      .insecure_skip_verify = true,
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls2), ccol_success);
  REQUIRE_EQ(chttpcli_set_tls(cli, NULL), ccol_success);

  chttpcli_destroy(cli);
}

TEST(tls, unreadable_cert_path_reports_cert_load_failed) {
  chttpcli_construct(cli);

  chttp_tls_config tls = {
      .cert_path = "/nonexistent/does-not-exist.crt",
      .key_path = "/nonexistent/does-not-exist.key",
      .ca_bundle_path = NULL,
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(tls, unreadable_ca_bundle_path_reports_cert_load_failed) {
  chttpcli_construct(cli);

  chttp_tls_config tls = {
      .cert_path = NULL,
      .key_path = NULL,
      .ca_bundle_path = "/nonexistent/does-not-exist-ca.pem",
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(pool, set_pool_size_zero_uses_cpu_count) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 0), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(pool, set_pool_size_zero_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 4), ccol_success);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  REQUIRE_EQ(chttpcli_set_pool_size(cli, 0), ccol_success);

  req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttp_request_free(req);

  chttpcli_destroy(cli);
}

TEST(pool, request_timeout_counts_time_spent_waiting_for_a_pool_slot) {
  char slow_url[128], get_url[128];
  make_url(slow_url, sizeof(slow_url), "/held");
  make_url(get_url, sizeof(get_url), "/get");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 1), ccol_success);

  concurrent_req_arg_t occupant = {
      .cli = cli, .result_status = 0, .result_rv = ccol_unexpected_failure};
  memcpy(occupant.url, slow_url, sizeof(slow_url));
  pthread_t occupant_thread;
  REQUIRE_EQ(
      pthread_create(&occupant_thread, NULL, concurrent_req_thread, &occupant),
      0);

  bool occupant_in_flight = false;
  for (int i = 0; i < 10000 && !occupant_in_flight; i++) {
    occupant_in_flight = atomic_load(&g_slow_started) >= 1;
    if (!occupant_in_flight) usleep(1000);
  }

  ccol_retval set_timeout_rv = chttpcli_set_request_timeout(cli, 20000);

  chttp_request *req = chttp_request_create(CHTTP_GET, get_url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = ccol_unexpected_failure;
  long elapsed_ms = -1;
  if (req && occupant_in_flight) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rv = chttpcli_do(cli, req, &resp);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed_ms =
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  }
  if (req) chttp_request_free(req);

  held_release();
  pthread_join(occupant_thread, NULL);

  REQUIRE_TRUE(occupant_in_flight);
  REQUIRE_EQ(set_timeout_rv, ccol_success);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_EQ((void *)resp, NULL);
  REQUIRE_LT(elapsed_ms, 1000L);

  REQUIRE_EQ(occupant.result_rv, ccol_success);
  REQUIRE_EQ(occupant.result_status, 200);

  chttpcli_destroy(cli);
}

// Body macros
TEST(body_macros, chttp_body) {
  const char *data = "raw";
  chttp_request_body b = CHTTP_BODY(data, 3, "text/html");
  REQUIRE_EQ((void *)b.data, (void *)data);
  REQUIRE_EQ(b.len, (size_t)3);
  REQUIRE_STREQ(b.content_type, "text/html");
}

TEST(body_macros, chttp_json_body) {
  const char *data = "{}";
  chttp_request_body b = CHTTP_JSON_BODY(data, 2);
  REQUIRE_STREQ(b.content_type, "application/json");
  REQUIRE_EQ(b.len, (size_t)2);
}

TEST(body_macros, chttp_text_body) {
  const char *data = "hello";
  chttp_request_body b = CHTTP_TEXT_BODY(data, 5);
  REQUIRE_STREQ(b.content_type, "text/plain");
}

TEST(body_macros, chttp_form_body) {
  const char *data = "a=1&b=2";
  chttp_request_body b = CHTTP_FORM_BODY(data, strlen(data));
  REQUIRE_STREQ(b.content_type, "application/x-www-form-urlencoded");
}

TEST(body_macros, chttp_no_body) {
  chttp_request_body b = CHTTP_NO_BODY;
  REQUIRE_EQ((void *)b.data, NULL);
  REQUIRE_EQ(b.len, (size_t)0);
  REQUIRE_EQ((void *)b.content_type, NULL);
}

// Method strings
TEST(method, chttp_method_str) {
  REQUIRE_STREQ(chttp_method_str(CHTTP_GET), "GET");
  REQUIRE_STREQ(chttp_method_str(CHTTP_POST), "POST");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PUT), "PUT");
  REQUIRE_STREQ(chttp_method_str(CHTTP_DELETE), "DELETE");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PATCH), "PATCH");
  REQUIRE_STREQ(chttp_method_str(CHTTP_HEAD), "HEAD");
  REQUIRE_STREQ(chttp_method_str(CHTTP_OPTIONS), "OPTIONS");
}

// Run query
TEST(http, run_query_null_headers) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_post_with_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"run\":\"query\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_headers_forwarded_and_not_consumed) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-echo", "hello-from-run-query");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello-from-run-query");
  chttpcli_resp_free(resp);

  const char *stored = chmap_get(hdrs, "x-echo");
  REQUIRE_NE((void *)stored, NULL);
  REQUIRE_STREQ(stored, "hello-from-run-query");
  chmap_destroy(hdrs);
}

TEST(http, run_query_invalid_args) {
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_perform(CHTTP_GET, NULL, NULL, NULL, &resp),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_perform(CHTTP_GET, "http://x/", NULL, NULL, NULL),
             ccol_invalid_args);
}

// Client destruction
TEST(pool, do_returns_not_permitted_when_destroying) {
  char slow_url[128], url[128];
  make_url(slow_url, sizeof(slow_url), "/held");
  make_url(url, sizeof(url), "/get");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 2), ccol_success);

  concurrent_req_arg_t slow_args[2];
  pthread_t slow_tids[2];
  int slow_started = 0;
  for (int i = 0; i < 2; i++) {
    slow_args[i].cli = cli;
    memcpy(slow_args[i].url, slow_url, sizeof(slow_url));
    slow_args[i].result_status = 0;
    slow_args[i].result_rv = ccol_unexpected_failure;
    if (pthread_create(&slow_tids[i], NULL, concurrent_req_thread,
                       &slow_args[i]) != 0)
      break;
    slow_started++;
  }

  bool all_slow_in_flight = false;
  for (int i = 0; i < 10000 && !all_slow_in_flight; i++) {
    all_slow_in_flight = atomic_load(&g_slow_started) >= slow_started;
    if (!all_slow_in_flight) usleep(1000);
  }

  probe_arg_t probe;
  probe.cli = cli;
  memcpy(probe.url, url, sizeof(url));
  probe.result_status = 0;
  probe.result_rv = ccol_unexpected_failure;
  atomic_store(&probe.ready, 0);
  pthread_t probe_tid;
  bool probe_started =
      (pthread_create(&probe_tid, NULL, probe_thread, &probe) == 0);

  while (probe_started && !atomic_load(&probe.ready)) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
    nanosleep(&ts, NULL);
  }

  pthread_t destroy_tid;
  bool destroy_started =
      (pthread_create(&destroy_tid, NULL, do_destroy_thread, &cli) == 0);

  if (probe_started) pthread_join(probe_tid, NULL);
  held_release();
  for (int i = 0; i < slow_started; i++) pthread_join(slow_tids[i], NULL);
  if (destroy_started) pthread_join(destroy_tid, NULL);
  if (!destroy_started) chttpcli_destroy(cli);

  REQUIRE_EQ(slow_started, 2);
  REQUIRE_TRUE(all_slow_in_flight);
  REQUIRE_TRUE(probe_started);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_EQ(probe.result_rv, ccol_not_permitted);
  REQUIRE_EQ(probe.result_status, 0);
  for (int i = 0; i < 2; i++) {
    REQUIRE_EQ(slow_args[i].result_rv, ccol_success);
    REQUIRE_EQ(slow_args[i].result_status, 200);
  }
}

static void wait_for_async_engine_idle(void);

// Error codes
//
// chttp_request_create refuses CHTTP_ANY and any value outside the enum, and a
// method written into a request afterwards fails Tier 1 and Tier 2 with
// ccol_invalid_args before anything is sent. Without the checks the request
// goes out as "ANY / HTTP/1.1".
TEST(error_codes, a_method_that_names_no_http_method_is_refused) {
  char url[160];
  make_url(url, sizeof(url), "/hello");
  char *err = NULL;
  chttp_request *bad =
      chttp_request_create_mp(CHTTP_ANY, url, NULL, NULL, &err);
  bool any_refused = bad == NULL && err != NULL;
  if (bad) chttp_request_free(bad);
  bad = chttp_request_create_mp((chttp_method)99, url, NULL, NULL, NULL);
  bool out_of_range_refused = bad == NULL;
  if (bad) chttp_request_free(bad);

  chttpcli cli = chttpcli_create(NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv1 = ccol_success, rv2 = ccol_success;
  if (cli != CHTTPCLI_NULL && req) {
    req->method = CHTTP_ANY;
    chttpcli_response *resp = NULL;
    rv1 = chttpcli_do(cli, req, &resp);
    chttpcli_resp_free(resp);
    cthrpool_future *f = chttpcli_do_async(cli, req);
    chttpcli_async_result *r = f ? chttpcli_async_result_get(f) : NULL;
    rv2 = r ? r->rv : ccol_unexpected_failure;
    if (r) chttpcli_async_result_free(r);
    if (f) cthrpool_future_free(f);
  }
  if (req) chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_TRUE(any_refused);
  REQUIRE_TRUE(out_of_range_refused);
  REQUIRE_EQ(rv1, ccol_invalid_args);
  REQUIRE_EQ(rv2, ccol_invalid_args);
}

// A wrapper that maps every NULL from the create to ccol_not_enough_memory
// fails here; that code is kept for a failed allocation.
TEST(error_codes, a_convenience_call_reports_a_refused_request_as_invalid) {
  char url[160];
  make_url(url, sizeof(url), "/hello");
  chttp_request_body no_data = {.data = NULL, .len = 5};
  chttpcli_response *resp = (chttpcli_response *)0x1;
  ccol_retval post = chttp_post(url, &no_data, &resp);
  bool cleared = resp == NULL;
  ccol_retval put = chttp_put(url, &no_data, &resp);
  ccol_retval patch = chttp_patch(url, &no_data, &resp);
  ccol_retval any = chttp_perform(CHTTP_ANY, url, NULL, NULL, &resp);
  ccol_retval out_of_range =
      chttp_perform((chttp_method)99, url, NULL, NULL, &resp);
  ccol_retval body = chttp_perform(CHTTP_POST, url, &no_data, NULL, &resp);
  REQUIRE_EQ(post, ccol_invalid_args);
  REQUIRE_TRUE(cleared);
  REQUIRE_EQ(put, ccol_invalid_args);
  REQUIRE_EQ(patch, ccol_invalid_args);
  REQUIRE_EQ(any, ccol_invalid_args);
  REQUIRE_EQ(out_of_range, ccol_invalid_args);
  REQUIRE_EQ(body, ccol_invalid_args);
}

// The component-wise form (seconds times 1000 plus the nanosecond difference
// divided by 1000000) gives 1 for the first pair and fails.
TEST(error_codes, an_elapsed_time_across_a_second_boundary_rounds_down) {
  long long a = chttp__ms_elapsed_floor_for_tests(10, 999800000L, 11, 300000L);
  long long b = chttp__ms_elapsed_floor_for_tests(10, 999000000L, 11, 500000L);
  long long c = chttp__ms_elapsed_floor_for_tests(10, 100000L, 10, 600000L);
  long long d = chttp__ms_elapsed_floor_for_tests(10, 5L, 11, 5L);
  REQUIRE_EQ(a, 0LL);
  REQUIRE_EQ(b, 1LL);
  REQUIRE_EQ(c, 0LL);
  REQUIRE_EQ(d, 1000LL);
}

TEST(error_codes, unsupported_scheme_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("ccol-not-a-scheme://example.com/", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

// A wrong value, never NULL and never dereferenced. The tests below write it
// into *resp_out before a failing call, so they pass only when the library
// writes NULL there itself.
#define SENTINEL_RESP ((chttpcli_response *)(uintptr_t)0xdeadbeefUL)

TEST(error_codes, resp_out_actively_reset_to_null_chttpcli_do) {
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "ccol-not-a-scheme://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = SENTINEL_RESP;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(error_codes, resp_out_actively_reset_to_null_convenience_wrappers) {
  chttpcli_response *resp;

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_get("ccol-not-a-scheme://example.com/", &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_post("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_put("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_delete("ccol-not-a-scheme://example.com/", &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_patch("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_do_default(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_perform(CHTTP_GET, "ccol-not-a-scheme://example.com/", NULL,
                           NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_get(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_perform(CHTTP_GET, NULL, NULL, NULL, &resp),
             ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(error_codes, resp_out_actively_reset_to_null_chttpcli_do_pooled) {
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "ccol-not-a-scheme://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = SENTINEL_RESP;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(error_codes, resp_out_actively_reset_to_null_for_a_stale_handle) {
  chttpcli dead = chttpcli_create(NULL);
  REQUIRE_NE(dead, CHTTPCLI_NULL);
  chttpcli dead_handle = dead;
  chttpcli__destroy(dead);

  char url[160];
  make_url(url, sizeof(url), "/get");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli handles[] = {dead_handle, CHTTPCLI_NULL,
                        (chttpcli)0x0000000700000009ULL};
  bool all_null = true;
  bool all_invalid = true;
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
    chttpcli_response *resp = SENTINEL_RESP;
    if (chttpcli_do(handles[i], req, &resp) != ccol_invalid_args)
      all_invalid = false;
    if (resp != NULL) all_null = false;
    resp = SENTINEL_RESP;
    if (chttpcli_do_pooled(handles[i], req, &resp) != ccol_invalid_args)
      all_invalid = false;
    if (resp != NULL) all_null = false;
  }
  chttp_request_free(req);
  REQUIRE_TRUE(all_invalid);
  REQUIRE_TRUE(all_null);
}

#undef SENTINEL_RESP

TEST(error_codes, embedded_crlf_in_path_rejected_end_to_end) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(
      "http://127.0.0.1:1/search?q=x\r\nX-Injected: 1\r\n\r\nGET /evil", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(error_codes, host_resolution_failure) {
  chttpcli_construct(cli);
  chttpcli_set_request_timeout(cli, 5000000);

  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://this-host-will-not-resolve.invalid.ccol.test/", NULL,
      NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_host_resolution_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(error_codes, connection_refused) {
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
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_connection_failed);
  REQUIRE_EQ((void *)resp, NULL);
}

// Maximum response body size (Tier 1)
TEST(max_response_body_size, unset_default_is_unlimited) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, declared_content_length_rejected_up_front) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpcli_destroy(cli);
}

TEST(max_response_body_size, eof_delimited_body_rejected_reactively) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpcli_destroy(cli);
}

TEST(max_response_body_size, chunked_body_rejected_reactively) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/chunked-body");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpcli_destroy(cli);
}

TEST(max_response_body_size, redirect_hop_body_exempt_from_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 20), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size,
     interim_1xx_oversized_content_length_exempt_from_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/early-hints-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, head_response_oversized_content_length_exempt) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/head-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, response_304_oversized_content_length_exempt) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/304-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 304);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, response_204_oversized_content_length_exempt) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/204-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 204);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, response_exactly_at_cap_succeeds) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 8192), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(max_response_body_size, streaming_path_is_unaffected_by_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;
  ccol_retval rv =
      chttpcli_do_streaming(cli, req, stream_sink_write, &sink, &status);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);

  chttpcli_destroy(cli);
}

// Chunked responses
//
// /chunked-body sends two data chunks and the final chunk with no
// Content-Length. These tests cover the client side of a chunked response in
// both tiers, and keep-alive after it.
TEST(chunked_response, decoded_correctly) {
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen("Hello, chunked world!"));
  REQUIRE_STREQ(resp->body, "Hello, chunked world!");

  chttpcli_resp_free(resp);
}

TEST(chunked_response, connection_reused_after_chunked_body) {
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 3; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_STREQ(resp->body, "Hello, chunked world!");
    chttpcli_resp_free(resp);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
}

// Keep-alive and the idle pool
TEST(keepalive, sequential_requests_reuse_connection) {
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpcli_resp_free(resp);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
}

TEST(keepalive, dead_connection_detected_and_reconnects) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  chttp_request *req1 = chttp_request_create(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval rv1 = chttpcli_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpcli_resp_free(resp1);

  chttp_request *req2 = chttp_request_create(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  chttpcli_response *resp2 = NULL;
  ccol_retval rv2 = chttpcli_do(cli, req2, &resp2);
  chttp_request_free(req2);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpcli_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpcli_destroy(cli);
}

TEST(keepalive, concurrent_requests_exceeding_idle_cap_no_crash) {
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  chttpcli_set_pool_size(cli, 16);

  const int n = 12;
  pthread_t threads[12];
  concurrent_req_arg_t args[12];
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < n; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    create_rv =
        pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpcli_destroy(cli);
}

// The maximum number of idle origins
TEST(max_idle_origins, tier1_distinct_origins_bounded_and_reclaimed) {
  chttpcli__set_max_idle_origins_for_tests(2);

  char url4[160], url6[160], url_unix[256];
  make_url(url4, sizeof(url4), "/keepalive");
  make_url6(url6, sizeof(url6), "/keepalive");
  make_unix_url(url_unix, sizeof(url_unix), "/keepalive");

  chttpcli_construct(cli);
  struct chttpclient *raw = chttpcli__resolve_for_tests(cli);
  REQUIRE_NE((void *)raw, NULL);

  chttp_request *req4 = chttp_request_create(CHTTP_GET, url4, NULL, NULL);
  REQUIRE_NE((void *)req4, NULL);
  chttpcli_response *resp4 = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req4, &resp4), ccol_success);
  REQUIRE_NE((void *)resp4, NULL);
  chttpcli_resp_free(resp4);
  chttp_request_free(req4);

  chttp_request *req6 = chttp_request_create(CHTTP_GET, url6, NULL, NULL);
  REQUIRE_NE((void *)req6, NULL);
  chttpcli_response *resp6 = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req6, &resp6), ccol_success);
  REQUIRE_NE((void *)resp6, NULL);
  chttpcli_resp_free(resp6);
  chttp_request_free(req6);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_key_count_for_tests(raw), (size_t)2);

  int accepts_before_unix = test_server_accept_count();
  chttp_request *req_u1 = chttp_request_create(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u1, NULL);
  chttpcli_response *resp_u1 = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req_u1, &resp_u1), ccol_success);
  REQUIRE_NE((void *)resp_u1, NULL);
  chttpcli_resp_free(resp_u1);
  chttp_request_free(req_u1);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_key_count_for_tests(raw), (size_t)2);

  chttp_request *req_u2 = chttp_request_create(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u2, NULL);
  chttpcli_response *resp_u2 = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req_u2, &resp_u2), ccol_success);
  REQUIRE_NE((void *)resp_u2, NULL);
  chttpcli_resp_free(resp_u2);
  chttp_request_free(req_u2);

  usleep(20000);
  int accepts_after_unix = test_server_accept_count();
  REQUIRE_EQ(accepts_after_unix - accepts_before_unix, 2);

  char url4_close[160];
  make_url(url4_close, sizeof(url4_close), "/get");
  chttp_request *req4b =
      chttp_request_create(CHTTP_GET, url4_close, NULL, NULL);
  REQUIRE_NE((void *)req4b, NULL);
  chttpcli_response *resp4b = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req4b, &resp4b), ccol_success);
  REQUIRE_NE((void *)resp4b, NULL);
  chttpcli_resp_free(resp4b);
  chttp_request_free(req4b);
  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_key_count_for_tests(raw), (size_t)1);

  chttp_request *req_u3 = chttp_request_create(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u3, NULL);
  chttpcli_response *resp_u3 = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req_u3, &resp_u3), ccol_success);
  REQUIRE_NE((void *)resp_u3, NULL);
  chttpcli_resp_free(resp_u3);
  chttp_request_free(req_u3);
  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_key_count_for_tests(raw), (size_t)2);

  chttpcli__set_max_idle_origins_for_tests(0);
  chttpcli_destroy(cli);
}

// URL parser edge cases
TEST(url_parsing, missing_host_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("http:///get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, non_numeric_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("http://127.0.0.1:abc/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, port_out_of_range_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("http://127.0.0.1:99999/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, zero_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("http://127.0.0.1:0/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, trailing_garbage_after_valid_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get("http://127.0.0.1:80abc/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, explicit_non_default_port_succeeds) {
  char url[128];
  make_url(url, sizeof(url), "/get");
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(url_parsing, ipv6_literal_no_port) {
  bool is_https = false, is_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval rv = chttp__parse_url_for_tests("https://[::1]/path", &is_https,
                                              &is_ipv6, &host, &port, &pq,
                                              &origin_key, &auth, NULL, NULL);
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
  ccol_retval rv = chttp__parse_url_for_tests("http://[::1]:8443/", &is_https,
                                              &is_ipv6, &host, &port, &pq,
                                              &origin_key, &auth, NULL, NULL);
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
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://[::1/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_garbage_after_bracket_is_invalid) {
  ccol_retval rv =
      chttp__parse_url_for_tests("http://[::1]x/path", NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_empty_brackets_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://[]/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, host_with_embedded_crlf_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://evil.com\r\nx-injected:1/path", NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_host_with_embedded_crlf_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://[::1\r\nx-injected:1]/path", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, path_with_embedded_crlf_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://example.com/a\r\nX-Injected: 1", NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, query_with_embedded_crlf_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://example.com/search?q=x\r\nX-Injected:1", NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, bare_lf_in_path_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://example.com/a\nX-Injected:1", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
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
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(url_parsing, userinfo_user_and_pass) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://alice:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port,
      &pq, &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)auth, NULL);
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
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://alice@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
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
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
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
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://user%40x:pa%3Ass@host/path", &dummy_https, &dummy_ipv6, &host,
      &port, &pq, &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(auth, "Basic dXNlckB4OnBhOnNz");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_malformed_percent_escape_is_invalid) {
  ccol_retval rv =
      chttp__parse_url_for_tests("http://user%zzpass@host/path", NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, no_userinfo_means_no_auto_authorization) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval rv = chttp__parse_url_for_tests("http://host/path", &dummy_https,
                                              &dummy_ipv6, &host, &port, &pq,
                                              &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ((void *)auth, NULL);
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, fragment_is_stripped_from_wire) {
  char base[128];
  make_url(base, sizeof(base), "/get");
  char url[160];
  snprintf(url, sizeof(url), "%s#section", base);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(url_parsing, fragment_only_defaults_to_root_path) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://host#section", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(pq, "/");
  free(host);
  free(pq);
  free(origin_key);
}

// Relative redirect resolution (RFC 3986)
TEST(relative_redirects, rfc3986_5_4_reference_table) {
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"g:h", "g:h"},
      {"g", "http://a/b/c/g"},
      {"g#s", "http://a/b/c/g"},
      {"g#/../h", "http://a/b/c/g"},
      {"#s", "http://a/b/c/d;p?q"},
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
    char *result = chttp__resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
                                                         cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

TEST(relative_redirects,
     query_only_reference_against_dotted_base_left_verbatim) {
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"?y", "http://example.com/a/../b?y"},
      {"#frag", "http://example.com/a/../b?x"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = chttp__resolve_redirect_url_for_tests(
        "http://example.com/a/../b?x", cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

// Overflow guards of request serialization
//
// An allocator that counts its calls and always returns NULL. A test uses it to
// show that an overflow guard refuses an append before it reaches the
// allocator; the call count tells the two outcomes apart.
static atomic_int g_ob_overflow_alloc_calls;
static void *ob_overflow_never_malloc(size_t sz) {
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void *ob_overflow_never_calloc(size_t n, size_t sz) {
  (void)n;
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void *ob_overflow_never_realloc(void *p, size_t sz) {
  (void)p;
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void ob_overflow_never_free(void *p) { (void)p; }
static ccol_memmgmt_procs g_ob_overflow_never_mp = {
    .malloc = ob_overflow_never_malloc,
    .free = ob_overflow_never_free,
    .calloc = ob_overflow_never_calloc,
    .realloc = ob_overflow_never_realloc};

TEST(request_serialization,
     ob_append_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool oom = chttp__ob_append_overflow_guard_for_tests(&g_ob_overflow_never_mp,
                                                       SIZE_MAX - 3, 8);
  REQUIRE_TRUE(oom);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, ob_append_ordinary_small_append_still_works) {
  bool oom = chttp__ob_append_overflow_guard_for_tests(NULL, 0, 8);
  REQUIRE_FALSE(oom);
}

TEST(request_serialization,
     percent_encode_unix_path_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = chttp__percent_encode_unix_path_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/x", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization,
     percent_encode_unix_path_ordinary_small_path_still_works) {
  const char *path = "/var/run/app.sock";
  bool rejected = chttp__percent_encode_unix_path_overflow_guard_for_tests(
      NULL, path, strlen(path));
  REQUIRE_FALSE(rejected);
}

TEST(request_serialization,
     sink_buffered_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = chttp__sink_buffered_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, SIZE_MAX - 3);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, sink_buffered_ordinary_small_body_still_works) {
  bool rejected = chttp__sink_buffered_overflow_guard_for_tests(NULL, 0);
  REQUIRE_FALSE(rejected);
}

TEST(request_serialization,
     seen_names_add_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = chttp__seen_names_add_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, SIZE_MAX / 2 + 1);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, seen_names_add_ordinary_small_cap_still_works) {
  bool rejected = chttp__seen_names_add_overflow_guard_for_tests(NULL, 0);
  REQUIRE_FALSE(rejected);
}

TEST(relative_redirects,
     merge_ref_path_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = chttp__merge_ref_path_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/a/b/c", "x", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(relative_redirects, merge_ref_path_ordinary_small_path_still_works) {
  bool rejected =
      chttp__merge_ref_path_overflow_guard_for_tests(NULL, "/a/b/c", "d", 1);
  REQUIRE_FALSE(rejected);
}

TEST(relative_redirects, concat_len_overflow_guard_rejects_without_allocating) {
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = chttp__concat_len_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/a/b", 4, "?q=1", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(relative_redirects, concat_len_ordinary_small_strings_still_works) {
  bool rejected =
      chttp__concat_len_overflow_guard_for_tests(NULL, "/a/b", 4, "?q=1", 4);
  REQUIRE_FALSE(rejected);
}

TEST(relative_redirects, location_with_unrecognized_scheme_resolves_absolute) {
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"mailto:test@example.com", "mailto:test@example.com"},
      {"ftp://other.example/x", "ftp://other.example/x"},
      {"s3://bucket/key", "s3://bucket/key"},
      {"coap+tcp:5683/x", "coap+tcp:5683/x"},
      {"g%3Ah", "http://a/b/c/g%3Ah"},
      {"./this:that", "http://a/b/c/this:that"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = chttp__resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
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
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(relative_redirects, live_absolute_path_query_with_slashes_not_corrupted) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-abs-path-query-with-slashes");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "ok");
  chttpcli_resp_free(resp);
}

// Basic auth from the URL's userinfo
TEST(credentials, embedded_userinfo_sets_authorization_header) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpcli_resp_free(resp);
}

TEST(credentials, explicit_authorization_header_wins) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");
  chttpcli_resp_free(resp);
}

TEST(credentials, same_origin_redirect_carries_authorization) {
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpcli_resp_free(resp);
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
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpcli_resp_free(resp);
}

// Redirect method and body policy
TEST(redirect_policy, post_301_becomes_bodyless_get) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");
  chttpcli_resp_free(resp);
}

TEST(redirect_policy, post_307_preserves_method_and_body) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);
  chttpcli_resp_free(resp);
}

// A Content-Length, Content-Type or Expect that the caller set describes the
// original POST body, so it must not stay on the body-less GET that a 301, 302
// or 303 produced. A stale Content-Length can make the server wait for a body
// that never comes. /count-header reports how many times the header named by
// x-count-name reached the wire.
TEST(redirect_policy, stale_content_length_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "9"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-length"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

TEST(redirect_policy, stale_content_type_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Type", "application/json"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-type"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

TEST(redirect_policy, stale_expect_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Expect", "100-continue"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

// A 307 or 308 keeps the current body, so once an earlier 301 dropped it, the
// chain /redirect-301-then-307-to-echo -> /redirect-307-to-echo ->
// /echo-method-body must arrive as a body-less GET.
TEST(redirect_policy, body_stays_dropped_across_a_later_preserving_hop) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-then-307-to-echo");

  const char *body = "some=data";
  chttp_request *req = chttp_request_create(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");
  chttpcli_resp_free(resp);
}

// A caller-set Authorization header follows curl's rule: It's dropped at the
// first hop whose origin differs from the original request, and sent again on a
// hop that returns to that origin.
TEST(credentials, explicit_authorization_header_carried_same_origin_redirect) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-same-origin");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");
  chttpcli_resp_free(resp);
}

TEST(credentials, explicit_authorization_header_dropped_cross_origin_redirect) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] explicit_authorization_header_dropped_cross_origin_"
            "redirect: no IPv6 loopback listener available in this "
            "environment\n");
    return;
  }
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-cross-origin");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpcli_resp_free(resp);
}

TEST(redirect_policy, exceeding_max_redirects_reports_error) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-infinite");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_http_too_many_redirects);
  chttp_request_free(req);
  REQUIRE_EQ((void *)resp, NULL);
}

static void wait_for_async_engine_idle(void);

// Redirects to http+unix
//
// A redirect must never bring in the AF_UNIX transport or point it somewhere
// else. The mock server serves the same routes on TCP and on its Unix listener,
// so a client that followed such a Location would return ccol_success with
// status 200 instead of ccol_http_invalid_url.
TEST(redirect_transport, tcp_hop_cannot_redirect_to_a_unix_socket) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_mixed_case_unix_scheme_is_refused_too) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket-mixed-case");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_second_hop_cannot_reach_a_unix_socket_either) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-then-redirect-to-unix-socket");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, async_tier_refuses_a_unix_socket_redirect) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  chttp_request_free(req);
  REQUIRE_NE((void *)f, NULL);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval rv = raw->rv;
  chttpcli_response *resp = raw->resp;
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, pooled_sync_tier_refuses_a_unix_socket_redirect) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_unix_hop_still_follows_its_own_relative_redirect) {
  char url[256];
  make_unix_url(url, sizeof(url), "/redirect-relative");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(redirect_transport, a_unix_hop_cannot_redirect_to_a_different_socket) {
  char url[256];
  make_unix_url(url, sizeof(url), "/redirect-to-other-unix-socket");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, resolver_level_decisions) {
  char *resolved = NULL;

  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "http://example.com/a", "http+unix://%2Ftmp%2Fx.sock/secret", false,
      &resolved));
  free(resolved);
  resolved = NULL;
  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "http://example.com/a", "HTTP+UNIX://%2ftmp%2fx.sock/secret", false,
      &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "http://example.com/a/b", "../c", false, &resolved));
  REQUIRE_STREQ(resolved, "http://example.com/c");
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2ftmp%2fx.sock/b", false,
      &resolved));
  free(resolved);
  resolved = NULL;
  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2Ftmp%2Fy.sock/b", false,
      &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a/b", "../c", false, &resolved));
  REQUIRE_STREQ(resolved, "http+unix://%2Ftmp%2Fx.sock/c");
  free(resolved);
}

// A write_fn that takes no byte of the body, which aborts the transfer; the
// streaming abort tests of every tier use it.
static size_t abort_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)len;
  (void)ctx;
  return 0;
}

TEST(http, streaming_write_fn_abort_returns_transfer_aborted) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  int status = 0;
  ccol_retval rv = chttpcli_do_streaming(chttp_default_client(), req,
                                         abort_write_fn, NULL, &status);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
}

// Response lifetime with a custom allocator
TEST(custom_allocator, response_freed_before_client_with_custom_alloc) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  char *err = NULL;
  chttpcli cli = chttpcli_create_mp(&mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttp_request_free(req);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

// Borrowed header maps
//
// A caller's map can hold a header in any letter case, and the client must not
// add its own default beside it.
TEST(http, run_query_borrowed_map_mixed_case_ct_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "text/csv");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_lowercase_ct_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "content-type", "text/csv");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, patch_borrowed_map_mixed_case_ct_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "application/json");

  const char *payload = "patch";
  chttp_request_body body = {.data = payload, .len = 5, .content_type = NULL};

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_PATCH, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

// The same rule for the other headers the client adds a default for (Accept,
// User-Agent, Content-Length, Authorization and Expect); each must reach the
// wire once.
TEST(http, run_query_borrowed_map_mixed_case_accept_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Accept", "application/xml");
  chmap_insert(hdrs, "x-count-name", "accept");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_user_agent_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "User-Agent", "my-agent/1.0");
  chmap_insert(hdrs, "x-count-name", "user-agent");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_content_length_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Length", "4");
  chmap_insert(hdrs, "x-count-name", "content-length");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_authorization_no_duplication) {
  char base[128];
  make_url(base, sizeof(base), "/count-header");
  char url[192];
  const char *scheme_end = strstr(base, "://");
  snprintf(url, sizeof(url), "%.*s://user:pass@%s", (int)(scheme_end - base),
           base, scheme_end + 3);

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Authorization", "Bearer token");
  chmap_insert(hdrs, "x-count-name", "authorization");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_expect_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Expect", "100-continue");
  chmap_insert(hdrs, "x-count-name", "expect");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->headers = hdrs;
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  req->headers = NULL;
  chttp_request_free(req);
  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_case_variant_duplicate_keys_deduplicated) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Host", "should-not-reach-the-wire-twice");
  chmap_insert(hdrs, "host", "also-should-not-reach-the-wire-twice");
  chmap_insert(hdrs, "x-count-name", "host");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_crlf_in_value_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-evil", "1\r\nx-injected: evil");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_crlf_in_name_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-evil\r\nx-injected", "1");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_non_tchar_name_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "X Evil", "1");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_transfer_encoding_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Transfer-Encoding", "chunked");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, post_body_content_type_crlf_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "data";
  chttp_request_body body = {.data = payload,
                             .len = strlen(payload),
                             .content_type = "text/plain\r\nx-injected: evil"};

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(request, set_header_mismatched_content_length_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "0"),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_leading_sign_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), "+%zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_embedded_whitespace_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), " %zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_leading_zeros_still_accepted) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), "000%zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mismatched_content_length_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Length", "999");

  const char *payload = "data";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, set_header_host_reaches_the_wire) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header-raw");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "Host", "custom-host.example");
  chttp_request_set_header(req, "x-echo-name", "host");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "custom-host.example");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

// Zero-length bodies
TEST(request, zero_len_body_content_type_still_reaches_the_wire) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header-raw");

  chttp_request_body body = {
      .data = "ignored", .len = 0, .content_type = "text/plain"};
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "x-echo-name", "content-type");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "text/plain");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(request, body_data_non_null_but_zero_len_treated_as_no_body) {
  const char *data = "ignored";
  chttp_request_body body = {
      .data = data, .len = 0, .content_type = "text/plain"};

  chttp_request *req =
      chttp_request_create(CHTTP_POST, "http://example.com/", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  REQUIRE_NE((void *)req->body.content_type, NULL);
  REQUIRE_STREQ(req->body.content_type, "text/plain");
  chttp_request_free(req);
}

TEST(http, post_zero_len_body_no_data_transmitted) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttp_request_body body = {
      .data = "ignored", .len = 0, .content_type = "text/plain"};

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpcli_resp_free(resp);
}

// DELETE with a body
TEST(http, delete_body_not_transmitted) {
  char url[128];
  make_url(url, sizeof(url), "/delete-echo");

  chttp_request_body body = CHTTP_TEXT_BODY("payload", 7);
  chttp_request *req = chttp_request_create(CHTTP_DELETE, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)7);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(http, delete_with_body_content_type_not_sent) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  chttp_request_body body = CHTTP_JSON_BODY("{\"k\":\"v\"}", 9);
  chttp_request *req = chttp_request_create(CHTTP_DELETE, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_NE((void *)req->body.content_type, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do_default(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

// Does nothing. test_server.an_accept_that_fails_does_not_stop_the_server
// installs it without SA_RESTART, so a signal makes accept(2) fail in the mock
// server.
static void _accept_intr_handler(int sig) { (void)sig; }

TEST(test_server, an_accept_that_fails_does_not_stop_the_server) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _accept_intr_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  bool installed = sigaction(SIGUSR2, &sa, NULL) == 0;
  int before = atomic_load(&g_accept_failures);
  bool failed = false;
  for (int i = 0; installed && i < 50 && !failed; i++) {
    pthread_kill(g_srv.accept_tid, SIGUSR2);
    for (int j = 0; j < 100 && !failed; j++) {
      failed = atomic_load(&g_accept_failures) > before;
      if (!failed) usleep(1000);
    }
  }
  if (installed) signal(SIGUSR2, SIG_IGN);

  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 5000000);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = req ? chttpcli_do(cli, req, &resp) : ccol_not_enough_memory;
  int status = resp ? resp->status_code : -1;
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);

  REQUIRE_TRUE(installed);
  REQUIRE_TRUE(failed);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

// Inactivity timeout
//
// A write_fn sink that keeps the first bytes of the body and sleeps sleep_ms in
// every call.
typedef struct {
  char body[64];
  size_t len;
  int sleep_ms;
} inact_sink_t;

// The write_fn of the inactivity tests: Sleeps sleep_ms, so the time spent in
// the callback can be checked against the inactivity timeout, then appends what
// fits to the inact_sink_t in ctx and reports the whole chunk as taken.
static size_t inact_write_fn(const void *data, size_t len, void *ctx) {
  inact_sink_t *k = (inact_sink_t *)ctx;
  if (k->sleep_ms > 0) usleep((useconds_t)k->sleep_ms * 1000);
  size_t room = sizeof(k->body) - 1 - k->len;
  size_t n = len < room ? len : room;
  memcpy(k->body + k->len, data, n);
  k->len += n;
  k->body[k->len] = '\0';
  return len;
}

// What one inact_run() call ended with.
typedef struct {
  ccol_retval rv;
  int status;
  char body[64];
  long elapsed_ms;
} inact_outcome_t;

// Runs GET path in one tier (1 sync, 2 async, 3 pooled) on a new client with
// the given inactivity and request timeouts; every inactivity test is built on
// it. With sleep_ms >= 0 it streams through inact_write_fn. Gives the code, the
// status, the body and the elapsed time.
static inact_outcome_t inact_run(int tier, const char *path, uint64_t inact_us,
                                 uint64_t request_us, int sleep_ms) {
  inact_outcome_t o = {ccol_unexpected_failure, -1, {0}, -1};
  char url[128];
  make_url(url, sizeof(url), path);
  chttpcli_construct(cli);
  if (cli == CHTTPCLI_NULL ||
      chttpcli_set_inactivity_timeout(cli, inact_us) != ccol_success ||
      chttpcli_set_request_timeout(cli, request_us) != ccol_success) {
    if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
    return o;
  }
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  inact_sink_t sink = {{0}, 0, sleep_ms};
  bool streaming = sleep_ms >= 0;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpcli_response *resp = NULL;
  if (!req) {
    o.rv = ccol_not_enough_memory;
  } else if (tier == 1) {
    o.rv = streaming ? chttpcli_do_streaming(cli, req, inact_write_fn, &sink,
                                             &o.status)
                     : chttpcli_do(cli, req, &resp);
  } else if (tier == 3) {
    o.rv = streaming ? chttpcli_do_pooled_streaming(cli, req, inact_write_fn,
                                                    &sink, &o.status)
                     : chttpcli_do_pooled(cli, req, &resp);
  } else {
    cthrpool_future *f =
        streaming ? chttpcli_do_async_streaming(cli, req, inact_write_fn, &sink)
                  : chttpcli_do_async(cli, req);
    chttpcli_async_result *r = f ? chttpcli_async_result_get(f) : NULL;
    if (r) {
      o.rv = r->rv;
      resp = r->resp;
      r->resp = NULL;
      if (streaming && resp) o.status = resp->status_code;
      chttpcli_async_result_free(r);
    }
    if (f) cthrpool_future_free(f);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  o.elapsed_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                        (t1.tv_nsec - t0.tv_nsec) / 1000000);
  if (resp) {
    o.status = resp->status_code;
    if (resp->body)
      snprintf(o.body, sizeof(o.body), "%.*s", (int)resp->body_len, resp->body);
  }
  if (streaming) memcpy(o.body, sink.body, sizeof(o.body));
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  held_release();
  return o;
}

TEST(inactivity, a_new_client_and_the_default_client_have_the_defaults) {
  chttpcli_construct(cli);
  uint64_t c = 0, r = 1, i = 0;
  bool got = chttpcli__timeouts_for_tests(cli, &c, &r, &i);
  chttpcli_destroy(cli);
  uint64_t dc = 0, dr = 1, di = 0;
  bool dgot =
      chttpcli__timeouts_for_tests(chttp_default_client(), &dc, &dr, &di);
  REQUIRE_TRUE(got);
  REQUIRE_EQ(c, (uint64_t)30000000);
  REQUIRE_EQ(c, CHTTPCLI_DEFAULT_CONNECT_TIMEOUT_US);
  REQUIRE_EQ(i, (uint64_t)60000000);
  REQUIRE_EQ(i, CHTTPCLI_DEFAULT_INACTIVITY_TIMEOUT_US);
  REQUIRE_EQ(r, (uint64_t)0);
  REQUIRE_TRUE(dgot);
  REQUIRE_EQ(dc, (uint64_t)30000000);
  REQUIRE_EQ(di, (uint64_t)60000000);
  REQUIRE_EQ(dr, (uint64_t)0);
}

TEST(inactivity, the_setter_refuses_an_invalid_client) {
  REQUIRE_EQ(chttpcli_set_inactivity_timeout(CHTTPCLI_NULL, 1000),
             ccol_invalid_args);
}

TEST(inactivity, a_server_that_never_answers_ends_the_request_in_every_tier) {
  inact_outcome_t o[3];
  for (int t = 1; t <= 3; t++) o[t - 1] = inact_run(t, "/held", 300000, 0, -1);
  for (int t = 0; t < 3; t++) {
    REQUIRE_EQ(o[t].rv, ccol_timed_out);
    REQUIRE_GE(o[t].elapsed_ms, 250L);
    REQUIRE_LT(o[t].elapsed_ms, 10000L);
  }
}

TEST(inactivity, a_body_that_stops_midway_ends_the_request_in_every_tier) {
  inact_outcome_t o[3];
  for (int t = 1; t <= 3; t++)
    o[t - 1] = inact_run(t, "/drip-stop", 300000, 0, -1);
  for (int t = 0; t < 3; t++) {
    REQUIRE_EQ(o[t].rv, ccol_timed_out);
    REQUIRE_GE(o[t].elapsed_ms, 250L);
    REQUIRE_LT(o[t].elapsed_ms, 10000L);
  }
}

TEST(inactivity, a_slow_body_that_keeps_moving_completes_in_every_tier) {
  inact_outcome_t o[3];
  for (int t = 1; t <= 3; t++) o[t - 1] = inact_run(t, "/drip", 500000, 0, -1);
  for (int t = 0; t < 3; t++) {
    REQUIRE_EQ(o[t].rv, ccol_success);
    REQUIRE_EQ(o[t].status, 200);
    REQUIRE_STREQ(o[t].body, "0123456789");
    REQUIRE_GE(o[t].elapsed_ms, 1200L);
  }
}

TEST(inactivity, time_in_the_write_fn_does_not_count_in_every_tier) {
  inact_outcome_t o[3];
  for (int t = 1; t <= 3; t++) o[t - 1] = inact_run(t, "/drip", 300000, 0, 400);
  for (int t = 0; t < 3; t++) {
    REQUIRE_EQ(o[t].rv, ccol_success);
    REQUIRE_EQ(o[t].status, 200);
    REQUIRE_STREQ(o[t].body, "0123456789");
  }
}

TEST(inactivity, zero_turns_it_off_in_every_tier) {
  inact_outcome_t o[3];
  for (int t = 1; t <= 3; t++) o[t - 1] = inact_run(t, "/held", 0, 800000, -1);
  for (int t = 0; t < 3; t++) {
    REQUIRE_EQ(o[t].rv, ccol_timed_out);
    REQUIRE_GE(o[t].elapsed_ms, 750L);
    REQUIRE_LT(o[t].elapsed_ms, 10000L);
  }
}

// Expect: 100-continue (Tier 1)
TEST(expect_continue, interim_100_then_body_sent) {
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-echo");

  const char *payload = "hold-until-continue";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(expect_continue, server_rejects_without_100) {
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-reject");

  const char *payload = "should-never-be-sent";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 417);
  REQUIRE_STREQ(resp->body, "expectation failed");

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

TEST(expect_continue, direct_rejection_without_100_never_pools_connection) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/expect-continue-reject-keepalive");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  const char *payload = "should-never-be-sent";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req1 = chttp_request_create(CHTTP_POST, url1, &body, NULL);
  REQUIRE_NE((void *)req1, NULL);
  req1->expect_continue = true;
  chttpcli_response *resp1 = NULL;
  ccol_retval rv1 = chttpcli_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 417);
  chttpcli_resp_free(resp1);

  chttp_request *req2 = chttp_request_create(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  chttpcli_response *resp2 = NULL;
  ccol_retval rv2 = chttpcli_do(cli, req2, &resp2);
  chttp_request_free(req2);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpcli_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpcli_destroy(cli);
}

TEST(
    expect_continue,
    dead_connection_after_100_with_fake_leftover_final_not_retried_with_stale_state) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-fake-final-then-die");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  chttp_request *req1 = chttp_request_create(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval rv1 = chttpcli_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpcli_resp_free(resp1);

  const char *payload = "hold-until-continue";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req2 = chttp_request_create(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;

  chttpcli_response *resp2 = NULL;
  ccol_retval rv2 = chttpcli_do(cli, req2, &resp2);
  REQUIRE_NE((int)rv2, (int)ccol_success);
  REQUIRE_EQ((void *)resp2, NULL);

  chttp_request_free(req2);
  chttpcli_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
}

TEST(expect_continue, dead_connection_after_100_clean_eof_not_retried) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-die-after-100-clean");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();
  atomic_store(&g_die_after_100_clean_body_recv_count, 0);

  chttp_request *req1 = chttp_request_create(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval rv1 = chttpcli_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpcli_resp_free(resp1);

  const char *payload = "must-not-be-sent-twice";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req2 = chttp_request_create(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;

  chttpcli_response *resp2 = NULL;
  ccol_retval rv2 = chttpcli_do(cli, req2, &resp2);
  REQUIRE_NE((int)rv2, (int)ccol_success);
  REQUIRE_EQ((void *)resp2, NULL);

  chttp_request_free(req2);
  chttpcli_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);
  REQUIRE_EQ(atomic_load(&g_die_after_100_clean_body_recv_count), 1);

  chttpcli_destroy(cli);
}

TEST(expect_continue, wait_times_out_body_sent_anyway) {
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"sent\":\"after-timeout\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  unsigned long waits_before = chttp__tier1_continue_waits_for_tests();
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GE(chttp__tier1_continue_waits_for_tests(), waits_before + 1);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

// A direct final response that starts inside the 100-continue window and ends
// after it. The window bounds only the wait for the first byte, so the client
// must read the response to the end and never send the body. Each case runs on
// its own client and destroys it before it waits for the route's report.
//
// This is the streaming sink of those cases.
typedef struct {
  char data[64];
  size_t len;
} slow_final_sink_t;

// The write_fn of the streaming slow-final cases: Appends what fits to the
// slow_final_sink_t in ctx and reports the whole chunk as taken.
static size_t slow_final_write(const void *data, size_t len, void *ctx) {
  slow_final_sink_t *sink = (slow_final_sink_t *)ctx;
  size_t room = sizeof(sink->data) - 1 - sink->len;
  size_t n = len < room ? len : room;
  memcpy(sink->data + sink->len, data, n);
  sink->len += n;
  sink->data[sink->len] = '\0';
  return len;
}

// The outcome of one case, with what the route reported.
typedef struct {
  ccol_retval rv;
  int status;
  char body[64];
  int body_bytes_at_server;
  bool route_done;
} slow_final_outcome_t;

// Runs one slow-final case in the given tier, for the direct_final tests, and
// waits for the route to report how many body bytes reached it. Reads
// g_slow_final_done and g_slow_final_body_bytes before and after. Gives the
// outcome of the request and the route's report.
static slow_final_outcome_t run_slow_final_case(int tier, bool streaming,
                                                long request_timeout_ms) {
  slow_final_outcome_t out = {.rv = ccol_unexpected_failure, .status = -1};
  int done_before = atomic_load(&g_slow_final_done);
  int bytes_before = atomic_load(&g_slow_final_body_bytes);
  char url[160];
  make_url(url, sizeof(url),
           streaming ? "/expect-continue-slow-final-chunked"
                     : "/expect-continue-slow-final");
  const char *payload = "must-never-be-sent";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  chttpcli cli = chttpcli_create(NULL);
  if (req && cli != CHTTPCLI_NULL) {
    req->expect_continue = true;
    if (request_timeout_ms > 0)
      (void)chttpcli_set_request_timeout(cli,
                                         (uint64_t)request_timeout_ms * 1000u);
    slow_final_sink_t sink = {.len = 0};
    chttpcli_response *resp = NULL;
    if (tier == 2) {
      cthrpool_future *f = streaming ? chttpcli_do_async_streaming(
                                           cli, req, slow_final_write, &sink)
                                     : chttpcli_do_async(cli, req);
      chttpcli_async_result *r = f ? chttpcli_async_result_get(f) : NULL;
      if (r) {
        out.rv = r->rv;
        resp = r->resp;
        r->resp = NULL;
        chttpcli_async_result_free(r);
      }
      if (f) cthrpool_future_free(f);
    } else if (streaming) {
      out.rv = tier == 1 ? chttpcli_do_streaming(cli, req, slow_final_write,
                                                 &sink, &out.status)
                         : chttpcli_do_pooled_streaming(
                               cli, req, slow_final_write, &sink, &out.status);
    } else {
      out.rv = tier == 1 ? chttpcli_do(cli, req, &resp)
                         : chttpcli_do_pooled(cli, req, &resp);
    }
    if (resp) {
      out.status = resp->status_code;
      if (!streaming && resp->body)
        snprintf(out.body, sizeof(out.body), "%.*s", (int)resp->body_len,
                 (const char *)resp->body);
      chttpcli_resp_free(resp);
    }
    if (streaming) snprintf(out.body, sizeof(out.body), "%s", sink.data);
  }
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  if (req) chttp_request_free(req);
  for (int i = 0; i < 3000 && atomic_load(&g_slow_final_done) == done_before;
       i++)
    usleep(10000);
  out.route_done = atomic_load(&g_slow_final_done) != done_before;
  out.body_bytes_at_server =
      atomic_load(&g_slow_final_body_bytes) - bytes_before;
  return out;
}

// Asserts that the slow-final outcome o read the whole response "helloworld"
// with status 200 and that no body byte reached the server; the direct_final
// tests use it. It's a macro, so a failure names the line of the case.
#define REQUIRE_SLOW_FINAL_READ_WHOLE(o)     \
  do {                                       \
    REQUIRE_EQ((o).rv, ccol_success);        \
    REQUIRE_EQ((o).status, 200);             \
    REQUIRE_STREQ((o).body, "helloworld");   \
    REQUIRE_TRUE((o).route_done);            \
    REQUIRE_EQ((o).body_bytes_at_server, 0); \
  } while (0)

TEST(expect_continue, direct_final_that_outlasts_the_window_tier1) {
  slow_final_outcome_t a = run_slow_final_case(1, false, 0);
  slow_final_outcome_t b = run_slow_final_case(1, true, 10000);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, direct_final_that_outlasts_the_window_tier3) {
  slow_final_outcome_t a = run_slow_final_case(3, false, 10000);
  slow_final_outcome_t b = run_slow_final_case(3, true, 0);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, direct_final_that_outlasts_the_window_tier2) {
  slow_final_outcome_t a = run_slow_final_case(2, false, 0);
  slow_final_outcome_t b = run_slow_final_case(2, true, 10000);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, explicit_expect_header_suppresses_the_wait) {
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"x\":1}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  chttp_request_set_header(req, "Expect", "204-content");

  unsigned long waits_before = chttp__tier1_continue_waits_for_tests();
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  unsigned long waits_after = chttp__tier1_continue_waits_for_tests();
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(waits_after, waits_before);

  chttpcli_resp_free(resp);
}

TEST(expect_continue,
     reused_connection_dies_after_partial_interim_line_is_not_retried) {
  char keepalive_url[160], timeout_url[160];
  make_url(keepalive_url, sizeof(keepalive_url), "/keepalive");
  make_url(timeout_url, sizeof(timeout_url),
           "/expect-continue-timeout-then-die");

  chttpcli_construct(cli);

  chttp_request *warm =
      chttp_request_create(CHTTP_GET, keepalive_url, NULL, NULL);
  REQUIRE_NE((void *)warm, NULL);
  chttpcli_response *warm_resp = NULL;
  ccol_retval warm_rv = chttpcli_do(cli, warm, &warm_resp);
  chttp_request_free(warm);
  REQUIRE_EQ(warm_rv, ccol_success);
  REQUIRE_EQ(warm_resp->status_code, 200);
  chttpcli_resp_free(warm_resp);

  int accepts_before = test_server_accept_count();

  const char *payload = "{\"n\":1}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req =
      chttp_request_create(CHTTP_POST, timeout_url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  if (resp) chttpcli_resp_free(resp);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  chttpcli_destroy(cli);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);
  REQUIRE_EQ(accepts_after - accepts_before, 0);
}

TEST(expect_continue, early_hints_before_100_continue_still_waits) {
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-with-hints");

  const char *payload = "hello";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);
  chttpcli_resp_free(resp);
}

TEST(expect_continue,
     hints_both_sides_of_100_continue_each_phase_gets_its_own_cap) {
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-hints-both-sides/40/40");

  const char *payload = "hello";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(expect_continue, ignored_for_bodyless_request) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpcli_resp_free(resp);
}

// Interim 1xx responses (Tier 1)
TEST(early_hints, discarded_and_real_response_delivered) {
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
  chttpcli_resp_free(resp);
}

TEST(early_hints,
     discarded_when_arriving_in_the_same_read_as_the_final_response) {
  char url[160];
  make_url(url, sizeof(url), "/early-hints-same-write");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_do_default(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
  chttpcli_resp_free(resp);
}

TEST(early_hints, connection_stays_reusable_after_discarding_interim_response) {
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttpcli_construct(cli);

  for (int i = 0; i < 2; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
    chttpcli_resp_free(resp);
  }

  chttpcli_destroy(cli);
}

TEST(early_hints, tier1_gives_up_after_too_many_interim_responses) {
  char url[160];
  make_url(url, sizeof(url), "/endless-early-hints");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_NE(rv, ccol_success);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(early_hints, tier1_discards_exactly_64_before_giving_up) {
  char url64[160];
  make_url(url64, sizeof(url64), "/early-hints-count/64");

  chttpcli_response *resp64 = NULL;
  ccol_retval rv64 = chttp_get(url64, &resp64);
  REQUIRE_EQ(rv64, ccol_success);
  REQUIRE_NE((void *)resp64, NULL);
  REQUIRE_EQ(resp64->status_code, 200);
  chttpcli_resp_free(resp64);

  char url65[160];
  make_url(url65, sizeof(url65), "/early-hints-count/65");

  chttpcli_response *resp65 = NULL;
  ccol_retval rv65 = chttp_get(url65, &resp65);
  REQUIRE_NE(rv65, ccol_success);
  REQUIRE_EQ((void *)resp65, NULL);
}

static void wait_for_async_engine_idle(void);

// Async engine life cycle
//
// The engine is a process-wide reactor that starts on first use and stops when
// its last reference goes. These tests run full start and stop cycles, and a
// test that counts references first waits for the previous test's engine to go
// idle, because the reaper can still be tearing it down.
TEST(async_engine, starts_on_first_acquire_and_stops_at_zero_refcount) {
  wait_for_async_engine_idle();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 0);

  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_TRUE(chttpcli__engine_running_for_tests());
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 1);

  chttpcli__engine_release_for_tests();
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 0);
  chttpcli__engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());
}

TEST(async_engine, refcount_tracks_multiple_acquirers) {
  wait_for_async_engine_idle();
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 3);
  REQUIRE_TRUE(chttpcli__engine_running_for_tests());

  chttpcli__engine_release_for_tests();
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 2);
  REQUIRE_TRUE(chttpcli__engine_running_for_tests());

  chttpcli__engine_release_for_tests();
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 1);
  REQUIRE_TRUE(chttpcli__engine_running_for_tests());

  chttpcli__engine_release_for_tests();
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 0);
  chttpcli__engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());
}

TEST(async_engine, restart_after_full_stop_works) {
  wait_for_async_engine_idle();
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
    REQUIRE_TRUE(chttpcli__engine_running_for_tests());
    chttpcli__engine_release_for_tests();
    chttpcli__engine_wait_for_quiescence_for_tests();
    REQUIRE_FALSE(chttpcli__engine_running_for_tests());
  }
}

// The shared success counter of the acquire-release threads.
typedef struct {
  atomic_int *acquired_ok;
} engine_thread_arg_t;

// Acquires the engine, holds it for 1 ms and releases it, for
// async_engine.concurrent_acquire_release_no_corruption. Counts a successful
// acquire in the counter the engine_thread_arg_t in arg points to.
static void *engine_acquire_release_thread(void *arg) {
  engine_thread_arg_t *a = (engine_thread_arg_t *)arg;
  if (chttpcli__engine_acquire_for_tests() == ccol_success) {
    atomic_fetch_add(a->acquired_ok, 1);
  }
  usleep(1000);
  chttpcli__engine_release_for_tests();
  return NULL;
}

TEST(async_engine, concurrent_acquire_release_no_corruption) {
  wait_for_async_engine_idle();
  enum { N = 16 };
  pthread_t threads[N];
  atomic_int acquired_ok = 0;
  engine_thread_arg_t arg = {.acquired_ok = &acquired_ok};

  int started = 0;
  for (int i = 0; i < N; i++) {
    if (pthread_create(&threads[i], NULL, engine_acquire_release_thread,
                       &arg) != 0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) {
    pthread_join(threads[i], NULL);
  }

  REQUIRE_EQ(started, N);
  REQUIRE_EQ(atomic_load(&acquired_ok), N);
  REQUIRE_EQ(chttpcli__engine_ref_count_for_tests(), 0);
  chttpcli__engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());
}

TEST(async_engine, num_reactor_threads_defaults_to_cpu_count) {
  wait_for_async_engine_idle();
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_success);
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);

  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  size_t expected = (cpus > 0) ? (size_t)cpus : 1;
  REQUIRE_EQ(chttpcli__engine_num_reactor_threads_for_tests(), expected);

  chttpcli__engine_release_for_tests();
  chttpcli__engine_wait_for_quiescence_for_tests();
}

TEST(async_engine, num_reactor_threads_explicit_value_is_wired_in) {
  wait_for_async_engine_idle();
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(3), ccol_success);
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli__engine_num_reactor_threads_for_tests(), (size_t)3);
  chttpcli__engine_release_for_tests();
  chttpcli__engine_wait_for_quiescence_for_tests();

  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_success);
}

TEST(async_engine, num_reactor_threads_rejected_while_running) {
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(2), ccol_not_permitted);
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_not_permitted);
  chttpcli__engine_release_for_tests();
  chttpcli__engine_wait_for_quiescence_for_tests();
}

// Waits until the async engine is fully torn down, so a test leaves no engine
// behind for the next one. A request's future is fulfilled before its teardown
// drops the engine reference, so we poll the ref count down to 0 and then wait
// for the reaper that this drop starts.
static void wait_for_async_engine_idle(void) {
  for (int i = 0; i < 2000 && chttpcli__engine_ref_count_for_tests() > 0; i++) {
    usleep(1000);
  }
  chttpcli__engine_wait_for_quiescence_for_tests();
}

// Polls the engine ref count until it reaches expected or about 2 s pass, and
// gives the last value seen. The ref count tests use it because a future is
// fulfilled before the chain drops its reference, so one read right after a
// future resolves can see a count that is briefly too high.
static int poll_engine_ref_count(int expected) {
  int refs = -1;
  for (int i = 0; i < 2000; i++) {
    refs = chttpcli__engine_ref_count_for_tests();
    if (refs == expected) break;
    usleep(1000);
  }
  return refs;
}

TEST(async_engine, destroy_waits_for_in_flight_async_request) {
  char url[160];
  make_url(url, sizeof(url), "/slow");

  chttpcli_construct(cli);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  chttp_request_free(req);
  REQUIRE_NE((void *)f, NULL);

  chttpcli_destroy(cli);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  REQUIRE_GT(elapsed_ms, 50);

  REQUIRE_TRUE(cthrpool_future_done(f));

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  if (raw) {
    if (raw->resp) chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
}

TEST(async_step_a, get_200) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, eof_delimited_body_without_content_length) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "eof-delimited-body-ok");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, hard_read_error_during_eof_delimited_body_reports_error) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli__force_async_hard_read_error_once_for_tests();

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, post_echoes_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"n\":42}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, expect_continue_field_reaches_the_wire) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/count-header");

  const char *payload = "async-body";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_STREQ(resp->body, "1");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, large_body_response) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_chunked_response, decoded_correctly) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen("Hello, chunked world!"));
  REQUIRE_STREQ(resp->body, "Hello, chunked world!");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// Maximum response body size (Tier 2/3). These tests check that the cap gets
// from the chain into the buffer of each hop.
TEST(async_max_response_body_size, declared_content_length_rejected_up_front) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_max_response_body_size, response_exactly_at_cap_succeeds) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 8192), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_max_response_body_size, redirect_hop_body_exempt_from_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 20), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_max_response_body_size,
     interim_1xx_oversized_content_length_exempt_from_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/early-hints-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_max_response_body_size,
     head_response_oversized_content_length_exempt) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/head-oversized-content-length");
  chttp_request *req = chttp_request_create(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_max_response_body_size, async_streaming_is_unaffected_by_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  cthrpool_future *f =
      chttpcli_do_async_streaming(cli, req, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);

  chttpcli_resp_free(raw->resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(pooled, response_exceeding_cap_reports_msg_too_large) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, status_code_forwarded) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/status/404");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, connection_refused_reports_error) {
  chttpcli_construct(cli);
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, https_connection_refused_reports_error) {
  chttpcli_construct(cli);
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, https_handshake_fails_against_plain_http_server) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  char https_url[168];
  snprintf(https_url, sizeof(https_url), "https%s", url + 4);

  chttp_request *req = chttp_request_create(CHTTP_GET, https_url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval rv = raw->rv;
  REQUIRE_TRUE(rv == ccol_http_tls_handshake_failed ||
               rv == ccol_http_tls_cert_verification_failed ||
               rv == ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_step_a, null_args_returns_null) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpcli_do_async(CHTTPCLI_NULL, req), NULL);
  REQUIRE_EQ((void *)chttpcli_do_async(cli, NULL), NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

// The input and the outcome of one async request thread.
typedef struct {
  chttpcli cli;
  char url[160];
  int expected_status;
  bool ok;
} async_concurrent_arg_t;

// Makes one async GET from its own thread, for
// async_step_a.concurrent_requests_all_succeed, and sets ok in the
// async_concurrent_arg_t in arg when it got expected_status.
static void *async_concurrent_thread(void *arg) {
  async_concurrent_arg_t *a = (async_concurrent_arg_t *)arg;
  chttp_request *req = chttp_request_create(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;

  cthrpool_future *f = chttpcli_do_async(a->cli, req);
  chttp_request_free(req);
  if (!f) return NULL;

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == a->expected_status;
    chttpcli_resp_free(resp);
  }
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  return NULL;
}

TEST(async_step_a, concurrent_requests_all_succeed) {
  enum { N = 12 };
  chttpcli_construct(cli);
  pthread_t threads[N];
  async_concurrent_arg_t args[N];

  int started = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    make_url(args[i].url, sizeof(args[i].url), "/get");
    args[i].expected_status = 200;
    args[i].ok = false;
    if (pthread_create(&threads[i], NULL, async_concurrent_thread, &args[i]) !=
        0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, N);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// Interim 1xx responses (Tier 2)
TEST(async_early_hints, discarded_and_real_response_delivered) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints,
     discarded_when_arriving_in_the_same_read_as_the_final_response) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints-same-write");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints,
     connection_stays_reusable_after_discarding_interim_response) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  for (int i = 0; i < 2; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);

    cthrpool_future *f = chttpcli_do_async(cli, req);
    REQUIRE_NE((void *)f, NULL);
    chttp_request_free(req);

    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);

    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

    chttpcli_resp_free(resp);
    chttpcli_async_result_free(raw);
    cthrpool_future_free(f);
  }

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// Expect: 100-continue (Tier 2/3), on the same mock routes as Tier 1
TEST(async_expect_continue, interim_100_then_body_sent) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-echo");

  const char *payload = "hold-until-continue-async";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue, server_rejects_without_100) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-reject");

  const char *payload = "should-never-be-sent-async";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 417);
  REQUIRE_STREQ(resp->body, "expectation failed");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue,
     direct_rejection_without_100_never_pools_connection) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/expect-continue-reject-keepalive");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  const char *payload = "should-never-be-sent-async";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req1 = chttp_request_create(CHTTP_POST, url1, &body, NULL);
  REQUIRE_NE((void *)req1, NULL);
  req1->expect_continue = true;
  cthrpool_future *f1 = chttpcli_do_async(cli, req1);
  REQUIRE_NE((void *)f1, NULL);
  chttp_request_free(req1);
  chttpcli_async_result *raw1 = chttpcli_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  REQUIRE_NE((void *)raw1->resp, NULL);
  REQUIRE_EQ(raw1->resp->status_code, 417);
  chttpcli_resp_free(raw1->resp);
  chttpcli_async_result_free(raw1);
  cthrpool_future_free(f1);

  chttp_request *req2 = chttp_request_create(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  cthrpool_future *f2 = chttpcli_do_async(cli, req2);
  REQUIRE_NE((void *)f2, NULL);
  chttp_request_free(req2);
  chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  REQUIRE_NE((void *)raw2->resp, NULL);
  REQUIRE_EQ(raw2->resp->status_code, 200);
  chttpcli_resp_free(raw2->resp);
  chttpcli_async_result_free(raw2);
  cthrpool_future_free(f2);

  wait_for_async_engine_idle();
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpcli_destroy(cli);
}

TEST(async_expect_continue, dead_connection_after_100_clean_eof_not_retried) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-die-after-100-clean");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();
  atomic_store(&g_die_after_100_clean_body_recv_count, 0);

  chttp_request *req1 = chttp_request_create(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  cthrpool_future *f1 = chttpcli_do_async(cli, req1);
  REQUIRE_NE((void *)f1, NULL);
  chttp_request_free(req1);
  chttpcli_async_result *raw1 = chttpcli_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  REQUIRE_NE((void *)raw1->resp, NULL);
  REQUIRE_EQ(raw1->resp->status_code, 200);
  chttpcli_resp_free(raw1->resp);
  chttpcli_async_result_free(raw1);
  cthrpool_future_free(f1);

  usleep(20000);

  const char *payload = "must-not-be-sent-twice-async";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req2 = chttp_request_create(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;
  cthrpool_future *f2 = chttpcli_do_async(cli, req2);
  REQUIRE_NE((void *)f2, NULL);
  chttp_request_free(req2);
  chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_NE((int)raw2->rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw2->resp, NULL);
  chttpcli_async_result_free(raw2);
  cthrpool_future_free(f2);

  wait_for_async_engine_idle();
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);
  REQUIRE_EQ(atomic_load(&g_die_after_100_clean_body_recv_count), 1);

  chttpcli_destroy(cli);
}

TEST(async_expect_continue, wait_times_out_body_sent_anyway) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"sent\":\"after-timeout-async\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  unsigned stores_before = chttp__continue_deadline_stores_for_tests();
  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_EQ(chttp__continue_deadline_stores_for_tests(), stores_before + 1);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue, explicit_expect_header_suppresses_the_wait) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"x\":1}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  chttp_request_set_header(req, "Expect", "204-content");

  ccol_retval timeout_rv = chttpcli_set_request_timeout(cli, 30000000);
  if (timeout_rv != ccol_success) {
    chttp_request_free(req);
    chttpcli_destroy(cli);
  }
  REQUIRE_EQ(timeout_rv, ccol_success);
  unsigned stores_before = chttp__continue_deadline_stores_for_tests();
  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  unsigned stores_after = chttp__continue_deadline_stores_for_tests();

  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  REQUIRE_EQ(stores_after, stores_before);

  chttpcli_resp_free(raw->resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue, early_hints_before_100_continue_still_waits) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-with-hints");

  const char *payload = "hello-async";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue,
     hints_both_sides_of_100_continue_each_phase_gets_its_own_cap) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-hints-both-sides/40/40");

  const char *payload = "hello-async";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_expect_continue,
     interim_100_bundled_with_final_response_in_same_read) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-bundled-final");

  const char *payload = "hold-until-continue-bundled";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "bundled-response");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// A counting allocator for the client under test. An object built on the
// caller's procs keeps a copy of them and frees that copy through itself, so a
// call through a copy after that is a use-after-free that no assertion sees.
// carry_mp_free poisons every such copy and holds it instead of freeing it, so
// the use becomes a call into carry_mp_poisoned_free. The test frees the held
// copies itself. g_carry_mp_lock guards the held list.
#define CARRY_MP_MAX_HELD_COPIES 4096
static atomic_int g_carry_mp_live_blocks = 0;
static void *g_carry_mp_held[CARRY_MP_MAX_HELD_COPIES];
static size_t g_carry_mp_held_count = 0;
static atomic_int g_carry_mp_held_overflow = 0;
static ccol_memmgmt_procs g_carry_mp;
static pthread_mutex_t g_carry_mp_lock = PTHREAD_MUTEX_INITIALIZER;

// Reports a call through a poisoned procs copy and aborts, so the
// carry-replay test fails loudly on that use-after-free. The three functions
// below do the same for the other entries of the copy.
static void carry_mp_poisoned_free(void *p) {
  (void)p;
  fprintf(stderr,
          "chttpclient: a copy of the caller's memory management procedures "
          "was used after its owner had already handed it back; that copy is "
          "gone by then and this is a use-after-free\n");
  fflush(stderr);
  abort();
}
static void *carry_mp_poisoned_malloc(size_t sz) {
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}
static void *carry_mp_poisoned_calloc(size_t n, size_t sz) {
  (void)n;
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}
static void *carry_mp_poisoned_realloc(void *p, size_t sz) {
  (void)p;
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}

// Every block carries its size in a header in front of it, so carry_mp_free
// knows the size on every system. The header keeps malloc's alignment.
#define CARRY_MP_HDR sizeof(max_align_t)
// Writes the size header sz at base and gives the address after it, the block
// the carry allocator hands out; NULL stays NULL.
static void *carry_mp_finish(char *base, size_t sz) {
  if (!base) return NULL;
  memcpy(base, &sz, sizeof(sz));
  return base + CARRY_MP_HDR;
}
// Gives the size recorded in front of p, so carry_mp_free can tell whether the
// block can hold a procs copy.
static size_t carry_mp_size(void *p) {
  size_t sz;
  memcpy(&sz, (char *)p - CARRY_MP_HDR, sizeof(sz));
  return sz;
}
// Allocates a block of the carry allocator with a size header and counts it in
// g_carry_mp_live_blocks. carry_mp_calloc and carry_mp_realloc below work the
// same way.
static void *carry_mp_malloc(size_t sz) {
  if (sz > SIZE_MAX - CARRY_MP_HDR) return NULL;
  void *p = carry_mp_finish(malloc(CARRY_MP_HDR + sz), sz);
  if (p) atomic_fetch_add(&g_carry_mp_live_blocks, 1);
  return p;
}
static void *carry_mp_calloc(size_t n, size_t sz) {
  if (sz != 0 && n > (SIZE_MAX - CARRY_MP_HDR) / sz) return NULL;
  void *p = carry_mp_finish(calloc(1, CARRY_MP_HDR + n * sz), n * sz);
  if (p) atomic_fetch_add(&g_carry_mp_live_blocks, 1);
  return p;
}
static void *carry_mp_realloc(void *p, size_t sz) {
  if (!p) return carry_mp_malloc(sz);
  if (sz > SIZE_MAX - CARRY_MP_HDR) return NULL;
  return carry_mp_finish(realloc((char *)p - CARRY_MP_HDR, CARRY_MP_HDR + sz),
                         sz);
}
// Frees a block of the carry allocator and drops g_carry_mp_live_blocks. A
// block that holds a copy of g_carry_mp is poisoned and kept in g_carry_mp_held
// instead, under g_carry_mp_lock, so a later call through it aborts.
static void carry_mp_free(void *p) {
  if (!p) return;
  atomic_fetch_sub(&g_carry_mp_live_blocks, 1);
  if (carry_mp_size(p) >= sizeof(g_carry_mp) &&
      memcmp(p, &g_carry_mp, sizeof(g_carry_mp)) == 0) {
    ccol_memmgmt_procs *copy = (ccol_memmgmt_procs *)p;
    copy->malloc = carry_mp_poisoned_malloc;
    copy->free = carry_mp_poisoned_free;
    copy->calloc = carry_mp_poisoned_calloc;
    copy->realloc = carry_mp_poisoned_realloc;
    pthread_mutex_lock(&g_carry_mp_lock);
    if (g_carry_mp_held_count < CARRY_MP_MAX_HELD_COPIES) {
      g_carry_mp_held[g_carry_mp_held_count++] = p;
      pthread_mutex_unlock(&g_carry_mp_lock);
      return;
    }
    pthread_mutex_unlock(&g_carry_mp_lock);
    atomic_store(&g_carry_mp_held_overflow, 1);
  }
  free((char *)p - CARRY_MP_HDR);
}
static ccol_memmgmt_procs g_carry_mp = {.malloc = carry_mp_malloc,
                                        .free = carry_mp_free,
                                        .calloc = carry_mp_calloc,
                                        .realloc = carry_mp_realloc};

// Frees every poisoned copy in g_carry_mp_held and gives their number. The
// carry-replay test calls it once nothing can reach the client any more.
static size_t carry_mp_release_held(void) {
  pthread_mutex_lock(&g_carry_mp_lock);
  size_t n = g_carry_mp_held_count;
  for (size_t i = 0; i < n; i++)
    free((char *)g_carry_mp_held[i] - CARRY_MP_HDR);
  g_carry_mp_held_count = 0;
  pthread_mutex_unlock(&g_carry_mp_lock);
  return n;
}

// Destroys the client that arg points to, racing the carry replay of
// async_expect_continue.carry_replay_survives_a_racing_destroy.
static void *carry_destroy_thread_fn(void *arg) {
  chttpcli_destroy(*(chttpcli *)arg);
  return NULL;
}

TEST(async_expect_continue, carry_replay_survives_a_racing_destroy) {
  atomic_store(&g_carry_mp_live_blocks, 0);
  char *err = NULL;
  chttpcli cli = chttpcli_create_mp(&g_carry_mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  char url[160];
  make_url(url, sizeof(url), "/expect-continue-bundled-final");
  const char *payload = "hold-until-continue-bundled";
  chttp_request_body body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli__set_continue_carry_replay_delay_ms_for_tests(300);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  chttp_request_free(req);

  pthread_t destroyer;
  bool destroyer_started = false;
  ccol_retval rv = ccol_unexpected_failure;
  int status = 0;
  if (f) {
    destroyer_started =
        (pthread_create(&destroyer, NULL, carry_destroy_thread_fn, &cli) == 0);
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    if (raw) {
      rv = raw->rv;
      if (raw->resp) {
        status = raw->resp->status_code;
        chttpcli_resp_free(raw->resp);
      }
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(f);
    if (destroyer_started) pthread_join(destroyer, NULL);
  }
  chttpcli__set_continue_carry_replay_delay_ms_for_tests(0);
  wait_for_async_engine_idle();

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_TRUE(destroyer_started);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  int live_blocks = atomic_load(&g_carry_mp_live_blocks);
  int held_overflow = atomic_load(&g_carry_mp_held_overflow);
  size_t held = carry_mp_release_held();

  REQUIRE_EQ(live_blocks, 0);
  REQUIRE_EQ(held_overflow, 0);
  REQUIRE_GT(held, (size_t)0);
}

TEST(async_expect_continue, bodyless_request_not_affected) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  chttp_request *req = chttp_request_create(CHTTP_POST, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ccol_retval timeout_rv = chttpcli_set_request_timeout(cli, 30000000);
  if (timeout_rv != ccol_success) {
    chttp_request_free(req);
    chttpcli_destroy(cli);
  }
  REQUIRE_EQ(timeout_rv, ccol_success);
  unsigned stores_before = chttp__continue_deadline_stores_for_tests();
  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  unsigned stores_after = chttp__continue_deadline_stores_for_tests();

  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  REQUIRE_EQ(stores_after, stores_before);

  chttpcli_resp_free(raw->resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// Async redirects: Chains of several hops, the CHTTP_MAX_REDIRECTS cap and
// relative Locations, plus the same method and body policy as Tier 1
TEST(async_redirects, get_301_follows_to_final_resource) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, post_301_becomes_bodyless_get) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, post_307_preserves_method_and_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// The Tier 2/3 side of the stale-header rule: Every hop sends the request
// headers again, so a downgrading hop must strip them too.
TEST(async_redirects, stale_content_length_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "9"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-length"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, stale_content_type_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Type", "application/json"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-type"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, stale_expect_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Expect", "100-continue"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// The Tier 2/3 side: A 307 or 308 after a downgrade must keep the earlier drop
// instead of going back to the original body of hop 0.
TEST(async_redirects, body_stays_dropped_across_a_later_preserving_hop) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-then-307-to-echo");

  const char *body = "some=data";
  chttp_request_body rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, explicit_authorization_header_carried_same_origin) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-same-origin");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, explicit_authorization_header_dropped_cross_origin) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] explicit_authorization_header_dropped_cross_origin: no "
            "IPv6 loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-cross-origin");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, relative_location_resolved_against_current_host) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-relative");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, plain_relative_location_merged_against_current_path) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/nested/dir/redirect-relative-plain");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "sibling-ok");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, relative_location_resolved_against_ipv6_host) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] relative_location_resolved_against_ipv6_host: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char url[160];
  make_url6(url, sizeof(url), "/redirect-relative");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, userinfo_credentials_carried_same_origin) {
  chttpcli_construct(cli);
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
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

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, multi_hop_chain_reaches_final_resource) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-chain-1");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_redirects, exceeding_max_redirects_reports_error) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-infinite");

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_too_many_redirects);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// Async idle pool (Tier 2)
//
// These tests count connections with test_server_accept_count(). A pooled
// connection holds an engine reference of its own until the client is
// destroyed, so they destroy the client before wait_for_async_engine_idle().
//
// Starts an async GET for url on cli, the shared shortcut of the Tier 2 tests.
// Gives its future, or NULL.
static cthrpool_future *async_get(chttpcli cli, const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  cthrpool_future *f = chttpcli_do_async(cli, req);
  chttp_request_free(req);
  return f;
}

TEST(async_early_hints, gives_up_after_too_many_interim_responses) {
  char url[160];
  make_url(url, sizeof(url), "/endless-early-hints");

  chttpcli_construct(cli);
  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_NE(raw->rv, ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints, discards_exactly_64_before_giving_up) {
  char url64[160];
  make_url(url64, sizeof(url64), "/early-hints-count/64");

  chttpcli_construct(cli64);
  cthrpool_future *f64 = async_get(cli64, url64);
  REQUIRE_NE((void *)f64, NULL);
  chttpcli_async_result *raw64 = chttpcli_async_result_get(f64);
  REQUIRE_NE((void *)raw64, NULL);
  REQUIRE_EQ(raw64->rv, ccol_success);
  REQUIRE_NE((void *)raw64->resp, NULL);
  REQUIRE_EQ(raw64->resp->status_code, 200);
  chttpcli_resp_free(raw64->resp);
  chttpcli_async_result_free(raw64);
  cthrpool_future_free(f64);
  chttpcli_destroy(cli64);
  wait_for_async_engine_idle();

  char url65[160];
  make_url(url65, sizeof(url65), "/early-hints-count/65");

  chttpcli_construct(cli65);
  cthrpool_future *f65 = async_get(cli65, url65);
  REQUIRE_NE((void *)f65, NULL);
  chttpcli_async_result *raw65 = chttpcli_async_result_get(f65);
  REQUIRE_NE((void *)raw65, NULL);
  REQUIRE_NE(raw65->rv, ccol_success);
  REQUIRE_EQ((void *)raw65->resp, NULL);
  chttpcli_async_result_free(raw65);
  cthrpool_future_free(f65);
  chttpcli_destroy(cli65);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, sequential_requests_reuse_connection) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    cthrpool_future *f = async_get(cli, url);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpcli_resp_free(resp);
    chttpcli_async_result_free(raw);
    cthrpool_future_free(f);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// Runs one async GET to the end, for the idle pool tests that only need the
// outcome. Gives the result code in *rv and the status in *status.
static void async_get_result(chttpcli cli, const char *url, ccol_retval *rv,
                             int *status) {
  *rv = ccol_unexpected_failure;
  *status = 0;
  cthrpool_future *f = async_get(cli, url);
  if (!f) return;
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  if (raw) {
    *rv = raw->rv;
    if (raw->resp) {
      *status = raw->resp->status_code;
      chttpcli_resp_free(raw->resp);
    }
    chttpcli_async_result_free(raw);
  }
  cthrpool_future_free(f);
}

// A stray write dispatch that arrives while a take is building the new hop
// must wait for the take. Without idle_lock it sends nothing, turns the
// connection to reading, and the request times out.
TEST(async_idle_pool, a_stray_write_during_a_take_waits_for_the_new_hop) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");
  chttpcli_construct(cli);
  ccol_retval trv = chttpcli_set_request_timeout(cli, 3000000);
  int accepts_before = test_server_accept_count();

  ccol_retval rv1, rv2;
  int status1, status2;
  async_get_result(cli, url, &rv1, &status1);
  usleep(20000);
  chttpcli__arm_stray_write_at_take_for_tests();
  async_get_result(cli, url, &rv2, &status2);
  int accepts = test_server_accept_count() - accepts_before;

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_EQ(status1, 200);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_EQ(status2, 200);
  REQUIRE_EQ(accepts, 1);
}

// A stray write dispatch that classified the connection idle just before a take
// must read the state again under the take's lock, or the request times out the
// same way.
TEST(async_idle_pool, a_stray_write_classified_idle_before_a_take_waits) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");
  chttpcli_construct(cli);
  ccol_retval trv = chttpcli_set_request_timeout(cli, 3000000);
  int accepts_before = test_server_accept_count();

  ccol_retval rv1, rv2;
  int status1, status2;
  async_get_result(cli, url, &rv1, &status1);
  usleep(20000);
  chttpcli__arm_stray_write_before_take_for_tests();
  async_get_result(cli, url, &rv2, &status2);
  bool held = chttpcli__stray_write_before_take_held_for_tests();
  int accepts = test_server_accept_count() - accepts_before;

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_EQ(status1, 200);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_EQ(status2, 200);
  REQUIRE_EQ(accepts, 1);
}

TEST(async_idle_pool, dead_connection_detected_and_retried) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  cthrpool_future *f1 = async_get(cli, url1);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result *raw1 = chttpcli_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpcli_response *resp1 = raw1->resp;
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpcli_resp_free(resp1);
  chttpcli_async_result_free(raw1);
  cthrpool_future_free(f1);

  cthrpool_future *f2 = async_get(cli, url2);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  chttpcli_response *resp2 = raw2->resp;
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpcli_resp_free(resp2);
  chttpcli_async_result_free(raw2);
  cthrpool_future_free(f2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, stale_connection_eviction_releases_engine_reference) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  cthrpool_future *f1 = async_get(cli, url);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result *raw1 = chttpcli_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpcli_resp_free(raw1->resp);
  chttpcli_async_result_free(raw1);
  cthrpool_future_free(f1);

  REQUIRE_EQ(poll_engine_ref_count(1), 1);
  chttpcli__force_async_idle_stale_for_tests(chttpcli__resolve_for_tests(cli));

  cthrpool_future *f2 = async_get(cli, url);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  chttpcli_resp_free(raw2->resp);
  chttpcli_async_result_free(raw2);
  cthrpool_future_free(f2);

  REQUIRE_EQ(poll_engine_ref_count(1), 1);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// A thread that makes GETs and marks the pooled connection stale after each.
typedef struct {
  chttpcli cli;
  const char *url;
  int iterations;
} stale_race_arg_t;

// Makes GETs and marks the pooled connection stale after each, racing the IDLE
// dispatch for async_idle_pool.concurrent_stale_eviction_races_dispatch_no_uaf.
// arg is a stale_race_arg_t.
static void *stale_race_thread(void *arg) {
  stale_race_arg_t *a = (stale_race_arg_t *)arg;
  for (int i = 0; i < a->iterations; i++) {
    cthrpool_future *f = async_get(a->cli, a->url);
    if (!f) continue;
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    if (raw) {
      if (raw->resp) chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(f);
    chttpcli__force_async_idle_stale_for_tests(
        chttpcli__resolve_for_tests(a->cli));
  }
  return NULL;
}

TEST(async_idle_pool, concurrent_stale_eviction_races_dispatch_no_uaf) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive-then-close");

  chttpcli_construct(cli);

  enum { NUM_THREADS = 6, ITERATIONS_PER_THREAD = 40 };
  pthread_t tids[NUM_THREADS];
  stale_race_arg_t args[NUM_THREADS];
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < NUM_THREADS; i++) {
    args[i].cli = cli;
    args[i].url = url;
    args[i].iterations = ITERATIONS_PER_THREAD;
    create_rv = pthread_create(&tids[i], NULL, stale_race_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  REQUIRE_EQ(create_rv, 0);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// An allocator that fails the one call with index g_hop_fail_at_call (any of
// malloc, calloc and realloc; -1: Never), to sweep every allocation of a hop on
// a reused connection.
static atomic_int g_hop_fail_call_index;
static int g_hop_fail_at_call = -1;

// The entry points of the hop_fail allocator; each counts its call in
// g_hop_fail_call_index and fails the one at g_hop_fail_at_call.
static void *hop_fail_malloc(size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return malloc(sz);
}
static void *hop_fail_calloc(size_t n, size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return calloc(n, sz);
}
static void *hop_fail_realloc(void *p, size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return realloc(p, sz);
}
static void hop_fail_free(void *p) { free(p); }

// The hop_fail allocator as a procs table, for chttpcli_create_mp.
static ccol_memmgmt_procs g_hop_fail_mp = {.malloc = hop_fail_malloc,
                                           .free = hop_fail_free,
                                           .calloc = hop_fail_calloc,
                                           .realloc = hop_fail_realloc};

// An allocator that fails only a calloc whose n * sz equals
// g_size_fail_target_size (0: Never), so
// async_idle_pool.chain_creation_oom_reports_not_enough_memory_not_generic can
// fail the chain allocation alone. It picks the call by its size, so it doesn't
// depend on how many other allocations run first.
static size_t g_size_fail_target_size = 0;
static void *size_fail_malloc(size_t sz) { return malloc(sz); }
static void *size_fail_calloc(size_t n, size_t sz) {
  if (g_size_fail_target_size != 0 && n * sz == g_size_fail_target_size)
    return NULL;
  return calloc(n, sz);
}
static void *size_fail_realloc(void *p, size_t sz) { return realloc(p, sz); }
static void size_fail_free(void *p) { free(p); }
static ccol_memmgmt_procs g_size_fail_mp = {.malloc = size_fail_malloc,
                                            .free = size_fail_free,
                                            .calloc = size_fail_calloc,
                                            .realloc = size_fail_realloc};

TEST(async_idle_pool, reused_hop_setup_failure_releases_engine_reference) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  char *cerr = NULL;
  chttpcli cli = chttpcli_create_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  enum { SWEEP_UPPER = 40 };
  for (int idx = 0; idx < SWEEP_UPPER; idx++) {
    g_hop_fail_at_call = -1;
    cthrpool_future *fw = async_get(cli, url);
    REQUIRE_NE((void *)fw, NULL);
    chttpcli_async_result *rw = chttpcli_async_result_get(fw);
    REQUIRE_NE((void *)rw, NULL);
    REQUIRE_EQ(rw->rv, ccol_success);
    chttpcli_resp_free(rw->resp);
    chttpcli_async_result_free(rw);
    cthrpool_future_free(fw);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;

    cthrpool_future *f2 = async_get(cli, url);
    if (f2) {
      chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
      if (raw2) {
        if (raw2->resp) chttpcli_resp_free(raw2->resp);
        chttpcli_async_result_free(raw2);
      }
      cthrpool_future_free(f2);
    }
    g_hop_fail_at_call = -1;

    usleep(20000);

    size_t pooled = 0;
    int refs = -1;
    for (int i = 0; i < 2000; i++) {
      pooled = chttpcli__async_idle_total_count_for_tests(
          chttpcli__resolve_for_tests(cli));
      refs = chttpcli__engine_ref_count_for_tests();
      if (refs == (int)pooled) break;
      usleep(1000);
    }
    REQUIRE_EQ(refs, (int)pooled);
  }

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool,
     chain_creation_oom_reports_not_enough_memory_not_generic) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  size_t chain_size = chttp__async_chain_struct_size_for_tests();

  {
    char *cerr = NULL;
    chttpcli cli = chttpcli_create_mp(&g_size_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    g_size_fail_target_size = chain_size;

    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    cthrpool_future *f = chttpcli_do_async(cli, req);
    chttp_request_free(req);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_not_enough_memory);
    REQUIRE_EQ((void *)raw->resp, NULL);
    chttpcli_async_result_free(raw);
    cthrpool_future_free(f);

    g_size_fail_target_size = 0;
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
  }

  {
    char *cerr = NULL;
    chttpcli cli = chttpcli_create_mp(&g_size_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    g_size_fail_target_size = chain_size;

    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_not_enough_memory);
    REQUIRE_EQ((void *)resp, NULL);

    g_size_fail_target_size = 0;
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
  }
}

TEST(client_construction, tier1_hop_allocation_failure_sweep_no_leak) {
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"leak\":\"check\"}";
  chttp_request_body body = CHTTP_JSON_BODY(payload, strlen(payload));

  enum { SWEEP_UPPER = 60 };
  for (int idx = -1; idx < SWEEP_UPPER; idx++) {
    char *cerr = NULL;
    chttpcli cli = chttpcli_create_mp(&g_hop_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_NULL);

    char *rerr = NULL;
    chttp_request *req =
        chttp_request_create_mp(CHTTP_POST, url, &body, &g_hop_fail_mp, &rerr);
    REQUIRE_NE((void *)req, NULL);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;

    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    if (rv == ccol_success) chttpcli_resp_free(resp);

    g_hop_fail_at_call = -1;
    chttp_request_free(req);
    chttpcli_destroy(cli);
  }
}

// The input and the outcome of async_idle_concurrent_thread.
typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} async_idle_concurrent_arg_t;

// Makes one async GET on the shared client from its own thread, for
// concurrent_requests_exceeding_idle_cap_no_crash, and sets ok in the
// async_idle_concurrent_arg_t in arg when it got a 200.
static void *async_idle_concurrent_thread(void *arg) {
  async_idle_concurrent_arg_t *a = (async_idle_concurrent_arg_t *)arg;
  cthrpool_future *f = async_get(a->cli, a->url);
  if (!f) return NULL;
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == 200;
    chttpcli_resp_free(resp);
  }
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  return NULL;
}

TEST(async_idle_pool, concurrent_requests_exceeding_idle_cap_no_crash) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  enum { N = 12 };
  pthread_t threads[N];
  async_idle_concurrent_arg_t args[N];
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    create_rv = pthread_create(&threads[i], NULL, async_idle_concurrent_thread,
                               &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, offer_push_failure_no_double_free) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  chttpcli__force_offer_push_fail_once_for_tests();

  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  chttpcli_resp_free(raw->resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  REQUIRE_EQ(chttpcli__async_idle_total_count_for_tests(
                 chttpcli__resolve_for_tests(cli)),
             (size_t)0);
  REQUIRE_EQ(poll_engine_ref_count(0), 0);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// The sweep can flip a registration to the write direction after another
// thread moved its connection on. A connected socket is almost always writable,
// so such a registration is dispatched over and over and the connection is lost
// unless the write path puts the read direction back. The tests below force
// that order with a hook instead of racing for it.
//
// Gives how many times the library's write path put a stray write-armed
// registration back to the read direction. The async_stray_write_arm tests
// wait for it to move after they force a stray arm.
static unsigned long stray_write_arm_restores(void) {
  return (unsigned long)chttpcli__stray_write_arm_restores_for_tests();
}

// Aborts with a diagnostic naming what when ok is false. The stray-arm tests
// call it when a restore never came, because every later call of the binary
// would block on the wedged reactor.
static void die_if_wedged(bool ok, const char *what) {
  if (ok) return;
  fprintf(stderr,
          "chttpclient: a write-armed registration with nothing to write "
          "never restored its read direction (%s); the shared reactor is "
          "spinning on it and every remaining call would block forever\n",
          what);
  fflush(stderr);
  abort();
}

// Waits up to about 30 s for the restore count to pass before, for the
// stray-arm tests. The bound is only a safety net; the restore lands a dispatch
// or two after the arming. Gives false when it never came.
static bool wait_for_stray_write_arm_restore(unsigned long before) {
  for (int i = 0; i < 3000; i++) {
    if (stray_write_arm_restores() > before) return true;
    usleep(10000);
  }
  return false;
}

// Waits up to about 30 s for f to be fulfilled, so the in-flight stray-arm test
// can report a wedged hop instead of blocking. Gives false on a timeout.
static bool wait_for_future_done(cthrpool_future *f) {
  for (int i = 0; i < 3000; i++) {
    if (cthrpool_future_done(f)) return true;
    usleep(10000);
  }
  return false;
}

TEST(async_stray_write_arm, a_pooled_connection_write_arm_self_heals) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  wait_for_async_engine_idle();
  unsigned long restores_before = stray_write_arm_restores();
  chttpcli__force_stray_idle_write_arm_once_for_tests();

  cthrpool_future *f = async_get(cli, url);
  ccol_retval rv = ccol_unexpected_failure;
  int status = 0;
  if (f) {
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    if (raw) {
      rv = raw->rv;
      if (raw->resp) {
        status = raw->resp->status_code;
        chttpcli_resp_free(raw->resp);
      }
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(f);
  }
  die_if_wedged(wait_for_stray_write_arm_restore(restores_before),
                "idle-pooled connection");

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(async_stray_write_arm, an_in_flight_connection_write_arm_self_heals) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");

  wait_for_async_engine_idle();
  unsigned long restores_before = stray_write_arm_restores();
  chttpcli__force_stray_write_arm_once_for_tests();

  cthrpool_future *f = async_get(cli, url);
  die_if_wedged(f != NULL && wait_for_future_done(f), "in-flight connection");

  ccol_retval rv = ccol_unexpected_failure;
  int status = 0;
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  if (raw) {
    rv = raw->rv;
    if (raw->resp) {
      status = raw->resp->status_code;
      chttpcli_resp_free(raw->resp);
    }
    chttpcli_async_result_free(raw);
  }
  cthrpool_future_free(f);
  unsigned long restores_after = stray_write_arm_restores();

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(restores_after, restores_before);
}

TEST(async_idle_pool, reactivate_failure_retries_without_uaf) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  cthrpool_future *f1 = async_get(cli, url);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result *raw1 = chttpcli_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpcli_resp_free(raw1->resp);
  chttpcli_async_result_free(raw1);
  cthrpool_future_free(f1);
  REQUIRE_EQ(chttpcli__async_idle_total_count_for_tests(
                 chttpcli__resolve_for_tests(cli)),
             (size_t)1);

  chttpcli__force_reactivate_fail_once_for_tests();

  cthrpool_future *f2 = async_get(cli, url);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  REQUIRE_NE((void *)raw2->resp, NULL);
  REQUIRE_EQ(raw2->resp->status_code, 200);
  chttpcli_resp_free(raw2->resp);
  chttpcli_async_result_free(raw2);
  cthrpool_future_free(f2);

  int refs = -1;
  for (int i = 0; i < 2000; i++) {
    refs = chttpcli__engine_ref_count_for_tests();
    if (refs <= 1) break;
    usleep(1000);
  }
  REQUIRE_TRUE(refs <= 1);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(max_idle_origins, tier2_distinct_origins_bounded_and_reclaimed) {
  chttpcli__set_max_idle_origins_for_tests(2);

  char url4[160], url6[160], url_unix[256];
  make_url(url4, sizeof(url4), "/keepalive");
  make_url6(url6, sizeof(url6), "/keepalive");
  make_unix_url(url_unix, sizeof(url_unix), "/keepalive");

  chttpcli_construct(cli);
  struct chttpclient *raw = chttpcli__resolve_for_tests(cli);
  REQUIRE_NE((void *)raw, NULL);

  cthrpool_future *f4 = async_get(cli, url4);
  REQUIRE_NE((void *)f4, NULL);
  chttpcli_async_result *raw4 = chttpcli_async_result_get(f4);
  REQUIRE_NE((void *)raw4, NULL);
  REQUIRE_EQ(raw4->rv, ccol_success);
  chttpcli_resp_free(raw4->resp);
  chttpcli_async_result_free(raw4);
  cthrpool_future_free(f4);

  cthrpool_future *f6 = async_get(cli, url6);
  REQUIRE_NE((void *)f6, NULL);
  chttpcli_async_result *raw6 = chttpcli_async_result_get(f6);
  REQUIRE_NE((void *)raw6, NULL);
  REQUIRE_EQ(raw6->rv, ccol_success);
  chttpcli_resp_free(raw6->resp);
  chttpcli_async_result_free(raw6);
  cthrpool_future_free(f6);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_async_key_count_for_tests(raw), (size_t)2);

  cthrpool_future *fu1 = async_get(cli, url_unix);
  REQUIRE_NE((void *)fu1, NULL);
  chttpcli_async_result *rawu1 = chttpcli_async_result_get(fu1);
  REQUIRE_NE((void *)rawu1, NULL);
  REQUIRE_EQ(rawu1->rv, ccol_success);
  chttpcli_resp_free(rawu1->resp);
  chttpcli_async_result_free(rawu1);
  cthrpool_future_free(fu1);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_async_key_count_for_tests(raw), (size_t)2);

  char url4_close[160];
  make_url(url4_close, sizeof(url4_close), "/get");
  cthrpool_future *f4b = async_get(cli, url4_close);
  REQUIRE_NE((void *)f4b, NULL);
  chttpcli_async_result *raw4b = chttpcli_async_result_get(f4b);
  REQUIRE_NE((void *)raw4b, NULL);
  REQUIRE_EQ(raw4b->rv, ccol_success);
  chttpcli_resp_free(raw4b->resp);
  chttpcli_async_result_free(raw4b);
  cthrpool_future_free(f4b);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_async_key_count_for_tests(raw), (size_t)1);

  cthrpool_future *fu2 = async_get(cli, url_unix);
  REQUIRE_NE((void *)fu2, NULL);
  chttpcli_async_result *rawu2 = chttpcli_async_result_get(fu2);
  REQUIRE_NE((void *)rawu2, NULL);
  REQUIRE_EQ(rawu2->rv, ccol_success);
  chttpcli_resp_free(rawu2->resp);
  chttpcli_async_result_free(rawu2);
  cthrpool_future_free(fu2);

  usleep(20000);
  REQUIRE_EQ(chttpcli__idle_pools_async_key_count_for_tests(raw), (size_t)2);

  chttpcli__set_max_idle_origins_for_tests(0);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// The async deadline sweep, which applies the connect and request timeouts in
// Tier 2 and closes every connection whose deadline has passed
TEST(async_deadline, request_timeout_fires_against_slow_endpoint) {
  char url[160];
  make_url(url, sizeof(url), "/very-slow");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_request_timeout(cli, 20000), ccol_success);

  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_timed_out);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_request_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/slow");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_request_timeout(cli, 5000000), ccol_success);

  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, connect_timeout_fires_against_unroutable_address) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_connect_timeout(cli, 300000), ccol_success);

  cthrpool_future *f = async_get(cli, "http://192.0.2.1:9/");
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  bool connect_did_not_silently_succeed =
      raw->rv == ccol_timed_out || raw->rv == ccol_http_connection_failed ||
      raw->rv == ccol_http_transfer_aborted;
  REQUIRE_TRUE(connect_did_not_silently_succeed);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_connect_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_connect_timeout(cli, 5000000), ccol_success);

  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// The lookup can't be interrupted, so the sweep must answer the request itself;
// a sweep that only marked it would answer after the lookup's 5 s.
TEST(async_deadline, a_stalled_name_lookup_ends_at_the_request_timeout) {
  wait_for_async_engine_idle();
  chttp__set_name_lookup_delay_ms_for_tests(5000);
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 200000);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cthrpool_future *f = async_get(cli, "http://stalled-lookup.invalid:9/");
  chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
  long took_ms = elapsed_ms_since(&t0);
  ccol_retval rv = raw ? raw->rv : ccol_unexpected_failure;
  if (raw) {
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  if (f) cthrpool_future_free(f);
  chttpcli_destroy(cli);
  chttp__set_name_lookup_delay_ms_for_tests(0);
  wait_for_async_engine_idle();
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(took_ms, 3000L);
}

// The DNS pool has two threads, so at most two lookups start before the 200 ms
// deadline. A connect task that ignored the deadline would make four.
TEST(async_deadline, a_request_that_timed_out_in_the_queue_makes_no_lookup) {
  wait_for_async_engine_idle();
  ccol_retval threads_rv = chttpcli_set_engine_num_reactor_threads(2);
  chttp__set_name_lookup_delay_ms_for_tests(1500);
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 200000);
  unsigned long before = chttp__name_lookups_for_tests();
  cthrpool_future *fs[4];
  for (int i = 0; i < 4; i++)
    fs[i] = async_get(cli, "http://stalled-lookup.invalid:9/");
  int timed_out = 0;
  for (int i = 0; i < 4; i++) {
    if (!fs[i]) continue;
    chttpcli_async_result *r = chttpcli_async_result_get(fs[i]);
    if (r) {
      if (r->rv == ccol_timed_out) timed_out++;
      chttpcli_resp_free(r->resp);
      chttpcli_async_result_free(r);
    }
    cthrpool_future_free(fs[i]);
  }
  chttpcli_destroy(cli);
  unsigned long lookups = chttp__name_lookups_for_tests() - before;
  chttp__set_name_lookup_delay_ms_for_tests(0);
  wait_for_async_engine_idle();
  ccol_retval reset_rv = chttpcli_set_engine_num_reactor_threads(0);
  REQUIRE_EQ(threads_rv, ccol_success);
  REQUIRE_EQ(reset_rv, ccol_success);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(timed_out, 4);
  REQUIRE_LE(lookups, 2ul);
}

TEST(async_deadline, stalled_name_lookups_do_not_delay_an_address_literal) {
  wait_for_async_engine_idle();
  ccol_retval threads_rv = chttpcli_set_engine_num_reactor_threads(2);
  chttp__set_name_lookup_delay_ms_for_tests(5000);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttpcli_construct(cli);
  cthrpool_future *stalled[2];
  for (int i = 0; i < 2; i++)
    stalled[i] = async_get(cli, "http://stalled-lookup.invalid:9/");
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cthrpool_future *f = async_get(cli, url);
  chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
  long took_ms = elapsed_ms_since(&t0);
  int status = (raw && raw->rv == ccol_success && raw->resp)
                   ? raw->resp->status_code
                   : -1;
  if (raw) {
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  if (f) cthrpool_future_free(f);
  for (int i = 0; i < 2; i++) {
    if (!stalled[i]) continue;
    chttpcli_async_result *r = chttpcli_async_result_get(stalled[i]);
    if (r) {
      chttpcli_resp_free(r->resp);
      chttpcli_async_result_free(r);
    }
    cthrpool_future_free(stalled[i]);
  }
  chttpcli_destroy(cli);
  chttp__set_name_lookup_delay_ms_for_tests(0);
  wait_for_async_engine_idle();
  ccol_retval reset_rv = chttpcli_set_engine_num_reactor_threads(0);
  REQUIRE_EQ(threads_rv, ccol_success);
  REQUIRE_EQ(reset_rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_LT(took_ms, 3000L);
}

// A TCP listener that accepts one connection and never sends a byte, so a TLS
// client stalls in the middle of its handshake every time.
typedef struct {
  int listen_fd;
  int port;
  pthread_t tid;
} tls_black_hole_srv_t;

// Accepts one connection on the black hole listener in arg and reads until the
// client shuts it down. The deadline sweep's shutdown ends the read, so the
// thread always exits by itself.
static void *tls_black_hole_accept_thread(void *arg) {
  tls_black_hole_srv_t *s = (tls_black_hole_srv_t *)arg;
  int conn_fd = test_accept(s->listen_fd);
  if (conn_fd >= 0) {
    char buf[16];
    while (recv(conn_fd, buf, sizeof(buf), 0) > 0) {
    }
    close(conn_fd);
  }
  return NULL;
}

// Starts the black hole listener of the stuck-handshake test on an ephemeral
// loopback port and fills *s. Gives false when it can't.
static bool tls_black_hole_srv_start(tls_black_hole_srv_t *s) {
  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd < 0) return false;
  int yes = 1;
  setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(s->listen_fd);
    return false;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
    close(s->listen_fd);
    return false;
  }
  s->port = ntohs(addr.sin_port);
  if (listen(s->listen_fd, 1) != 0) {
    close(s->listen_fd);
    return false;
  }
  if (pthread_create(&s->tid, NULL, tls_black_hole_accept_thread, s) != 0) {
    close(s->listen_fd);
    return false;
  }
  return true;
}

// Joins the black hole thread and closes its listener.
static void tls_black_hole_srv_stop(tls_black_hole_srv_t *s) {
  pthread_join(s->tid, NULL);
  close(s->listen_fd);
}

TEST(async_deadline,
     tls_handshake_stuck_peer_reports_timed_out_not_handshake_failed) {
  tls_black_hole_srv_t srv;
  REQUIRE_TRUE(tls_black_hole_srv_start(&srv));

  char url[64];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/", srv.port);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_connect_timeout(cli, 200000), ccol_success);

  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_timed_out);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  tls_black_hole_srv_stop(&srv);
}

// Async streaming (Tier 2), with the sinks of the Tier 1 streaming tests
//
// Starts an async streaming GET for url that delivers the body to write_fn, the
// shortcut of the async streaming tests. Gives its future, or NULL.
static cthrpool_future *async_get_streaming(chttpcli cli, const char *url,
                                            chttpcli_write_fn write_fn,
                                            void *write_ctx) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  cthrpool_future *f =
      chttpcli_do_async_streaming(cli, req, write_fn, write_ctx);
  chttp_request_free(req);
  return f;
}

TEST(async_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  cthrpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_GT(sink.len, (size_t)0);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, large_body_streamed_in_chunks) {
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  cthrpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_EQ(sink.len, sizeof(sink.buf) - 1);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);

  cthrpool_future *f = async_get_streaming(cli, url, abort_write_fn, NULL);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, null_write_fn_returns_null) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpcli_do_async_streaming(cli, req, NULL, NULL), NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, redirect_final_body_delivered_not_intermediate) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  cthrpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(sink.buf, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// The pooled synchronous API (Tier 3)
TEST(pooled, get_200) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled, null_checks) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do_pooled(CHTTPCLI_NULL, req, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_do_pooled(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpcli_do_pooled(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(pooled, bad_url_returns_specific_error_not_generic) {
  chttpcli_construct(cli);
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(pooled, unreadable_cert_path_reports_cert_load_failed) {
  chttpcli_construct(cli);

  chttp_tls_config tls = {
      .cert_path = "/nonexistent/does-not-exist.crt",
      .key_path = "/nonexistent/does-not-exist.key",
      .ca_bundle_path = NULL,
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(pooled, allocation_failure_reports_not_enough_memory) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  char *cerr = NULL;
  chttpcli cli = chttpcli_create_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  enum { SWEEP_UPPER = 40 };
  int unexpected_at = -1;
  int oom_seen = 0;
  bool success_seen = false;
  for (int idx = 0; idx < SWEEP_UPPER && !success_seen; idx++) {
    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
    g_hop_fail_at_call = -1;
    if (rv == ccol_unexpected_failure && unexpected_at < 0) unexpected_at = idx;
    if (rv == ccol_not_enough_memory) oom_seen++;
    if (rv == ccol_success) success_seen = true;
    chttpcli_resp_free(resp);
  }
  g_hop_fail_at_call = -1;
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(unexpected_at, -1);
  REQUIRE_GT(oom_seen, 0);
  REQUIRE_TRUE(success_seen);
}

TEST(pooled, redirect_followed_transparently) {
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

// The input and the outcome of one pooled request thread.
typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} pooled_concurrent_arg_t;

// Makes one pooled GET from its own thread, for
// pooled.concurrent_callers_all_succeed, and sets ok in the
// pooled_concurrent_arg_t in arg when it got a 200.
static void *pooled_concurrent_thread(void *arg) {
  pooled_concurrent_arg_t *a = (pooled_concurrent_arg_t *)arg;
  chttp_request *req = chttp_request_create(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(a->cli, req, &resp);
  a->ok = (rv == ccol_success) && resp && resp->status_code == 200;
  if (resp) chttpcli_resp_free(resp);
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
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    create_rv =
        pthread_create(&threads[i], NULL, pooled_concurrent_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;
  ccol_retval rv =
      chttpcli_do_pooled_streaming(cli, req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, null_write_fn_returns_invalid_args) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ(chttpcli_do_pooled_streaming(cli, req, NULL, NULL, NULL),
             ccol_invalid_args);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

TEST(pooled_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval rv =
      chttpcli_do_pooled_streaming(cli, req, abort_write_fn, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, bad_url_returns_specific_error_not_generic) {
  chttpcli_construct(cli);
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval rv =
      chttpcli_do_pooled_streaming(cli, req, stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);

  chttp_request_free(req);
  chttpcli_destroy(cli);
}

// Unix domain sockets (http+unix://), on all three tiers, against the mock
// server's Unix listener
TEST(unix_socket, get_request_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);
}

TEST(unix_socket, post_with_body_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/echo-method-body");

  const char *body_str = "hello-over-unix-socket";
  chttp_request_body body = CHTTP_JSON_BODY(body_str, strlen(body_str));
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  char expected[64];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body_str));
  REQUIRE_STREQ(resp->body, expected);
  chttpcli_resp_free(resp);
}

TEST(unix_socket, keepalive_reuses_connection) {
  char url[256];
  make_unix_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpcli_resp_free(resp);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
}

TEST(unix_socket, nonexistent_socket_path_fails) {
  char url[512];
  char encoded[256];
  percent_encode_unix_path(encoded, sizeof(encoded),
                           "/tmp/chttpcli_test_nonexistent_xyz.sock");
  snprintf(url, sizeof(url), "http+unix://%s/get", encoded);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_connection_failed);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(unix_socket, path_too_long_returns_invalid_url) {
  char raw_path[200];
  memset(raw_path, 'a', sizeof(raw_path) - 1);
  raw_path[0] = '/';
  raw_path[sizeof(raw_path) - 1] = '\0';

  char encoded[512];
  percent_encode_unix_path(encoded, sizeof(encoded), raw_path);
  char url[600];
  snprintf(url, sizeof(url), "http+unix://%s/get", encoded);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(unix_socket_async, basic_get_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  cthrpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  chttpcli_resp_free(raw->resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(unix_socket_async, sequential_requests_reuse_connection_via_idle_pool) {
  char url[256];
  make_unix_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    cthrpool_future *f = async_get(cli, url);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    REQUIRE_NE((void *)raw->resp, NULL);
    REQUIRE_EQ(raw->resp->status_code, 200);
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
    cthrpool_future_free(f);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(unix_socket_pooled, basic_get_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpcli_resp_free(resp);

  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(url_parsing, http_unix_scheme_basic) {
  bool is_https = false, is_ipv6 = false, is_unix = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  char *unix_path = NULL;
  uint16_t port = 0;
  ccol_retval rv = chttp__parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock/api/users", &is_https, &is_ipv6, &host,
      &port, &pq, &origin_key, &auth, &is_unix, &unix_path);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_unix);
  REQUIRE_FALSE(is_https);
  REQUIRE_STREQ(unix_path, "/tmp/app.sock");
  REQUIRE_STREQ(pq, "/api/users");
  REQUIRE_STREQ(origin_key, "unix:///tmp/app.sock");
  REQUIRE_EQ((void *)host, NULL);
  REQUIRE_EQ((void *)auth, NULL);
  free(unix_path);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, http_unix_scheme_no_path_defaults_to_root) {
  bool is_unix = false;
  char *pq = NULL, *origin_key = NULL, *unix_path = NULL;
  ccol_retval rv = chttp__parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock", NULL, NULL, NULL, NULL, &pq, &origin_key,
      NULL, &is_unix, &unix_path);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_unix);
  REQUIRE_STREQ(unix_path, "/tmp/app.sock");
  REQUIRE_STREQ(pq, "/");
  free(unix_path);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, https_unix_scheme_rejected) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "https+unix://%2Ftmp%2Fapp.sock/api", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, http_unix_scheme_empty_path_rejected) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http+unix:///api", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, http_unix_scheme_path_with_embedded_crlf_is_invalid) {
  ccol_retval rv = chttp__parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock/api\r\nX-Injected:1", NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

// The chttpcli handle life cycle
//
// chttpcli is a generation-tagged value handle that the library's slot table
// resolves before anything touches the client. These tests pin that a second
// destroy is fatal, that a destroy never frees a client a caller has pinned,
// and that a reused slot never looks like a stale handle.
//
// The argument of the destroy watchdog thread.
typedef struct {
  chttpcli h;
  atomic_bool done;
} destroy_watchdog_arg_t;

// The thread of destroy_completes_promptly: Destroys the client in the
// destroy_watchdog_arg_t in arg and sets done.
static void *destroy_watchdog_thread(void *arg) {
  destroy_watchdog_arg_t *a = (destroy_watchdog_arg_t *)arg;
  chttpcli_destroy(a->h);
  atomic_store(&a->done, true);
  return NULL;
}

// Destroys h on a detached thread and waits up to about 3 s, so a pin that
// never goes away fails one test instead of hanging the binary. Gives true when
// the destroy returned in time. On a timeout the argument leaks on purpose,
// because the thread may still touch it.
static bool destroy_completes_promptly(chttpcli h) {
  destroy_watchdog_arg_t *warg =
      (destroy_watchdog_arg_t *)malloc(sizeof(*warg));
  if (!warg) return false;
  warg->h = h;
  atomic_store(&warg->done, false);
  pthread_t tid;
  if (pthread_create(&tid, NULL, destroy_watchdog_thread, warg) != 0) {
    free(warg);
    return false;
  }
  pthread_detach(tid);
  for (int i = 0; i < 300; i++) {
    if (atomic_load(&warg->done)) {
      free(warg);
      return true;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

// The pin index publish at the end of a create can fail. Its rollback must
// clear the client's record of the handle and put the slot back on the free
// list, or the next client to take the slot inherits a handle that names
// somebody else.
TEST(chttpcli_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  enum { CYCLES = 8 };
  size_t before = chttpcli__slot_table_capacity_for_tests();
  size_t free_before = chttpcli__free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  for (int i = 0; i < CYCLES; i++) {
    ccol__pintable_force_next_publish_failure_for_tests();
    chttpcli bad = chttpcli_create(NULL);
    if (bad != CHTTPCLI_NULL) {
      all_failed = false;
      chttpcli__destroy(bad);
      break;
    }
    chttpcli good = chttpcli_create(NULL);
    if (good == CHTTPCLI_NULL) {
      all_created = false;
      break;
    }
    chttpcli__destroy(good);
  }

  ccol__pintable_force_next_publish_failure_for_tests();
  char *err = NULL;
  chttpcli failed = chttpcli_create(&err);
  bool forced_failure_applied = (failed == CHTTPCLI_NULL);
  if (!forced_failure_applied) chttpcli__destroy(failed);

  char url[128];
  make_url(url, sizeof(url), "/get");
  chttpcli_construct(cli);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  bool req_built = (req != NULL);
  ccol_retval rv = ccol_unexpected_failure;
  bool pins_measured = false;
  size_t pins = 0;
  if (req_built) {
    chttpcli_response *resp = NULL;
    rv = chttpcli_do(cli, req, &resp);
    if (resp) chttpcli_resp_free(resp);
    chttp_request_free(req);
    pins = chttpcli__pin_count_for_tests(cli);
    pins_measured = true;
  }
  size_t after = chttpcli__slot_table_capacity_for_tests();
  size_t free_after = chttpcli__free_index_count_for_tests();

  if (pins_measured && pins != 0) {
    REQUIRE_EQ(pins, (size_t)0);
    return;
  }
  chttpcli_destroy(cli);

  REQUIRE_TRUE(forced_failure_applied);
  REQUIRE_TRUE(req_built);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(all_failed);
  REQUIRE_TRUE(all_created);
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

TEST(chttpcli_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) test_child_exit(2);
    chttpcli stale = cli;
    chttpcli_destroy(cli);
    chttpcli__destroy(stale);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The handle that a concurrent destroy thread destroys.
typedef struct {
  chttpcli h;
} concurrent_destroy_arg_t;

// Destroys the handle in arg; concurrent_double_destroy_is_fatal runs two of
// them on the same handle.
static void *concurrent_destroy_thread(void *arg) {
  concurrent_destroy_arg_t *a = (concurrent_destroy_arg_t *)arg;
  chttpcli__destroy(a->h);
  return NULL;
}

TEST(chttpcli_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) test_child_exit(2);
    concurrent_destroy_arg_t a1 = {.h = cli};
    concurrent_destroy_arg_t a2 = {.h = cli};
    pthread_t t1, t2;
    int t1_rv = pthread_create(&t1, NULL, concurrent_destroy_thread, &a1);
    int t2_rv = (t1_rv == 0)
                    ? pthread_create(&t2, NULL, concurrent_destroy_thread, &a2)
                    : -1;
    if (t1_rv != 0 || t2_rv != 0) test_child_exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A destroy thread that records when its destroy returned.
typedef struct {
  chttpcli cli;
  atomic_int returned;
} destroy_observed_arg_t;

// Destroys the client in arg and sets returned, so
// resolve_then_use_race_destroy_waits can see whether the destroy came back
// while a call was still held.
static void *destroy_observed_thread(void *arg) {
  destroy_observed_arg_t *a = (destroy_observed_arg_t *)arg;
  chttpcli_destroy(a->cli);
  atomic_store(&a->returned, 1);
  return NULL;
}

// A blocking chttpcli_do on /held keeps the client pinned, so a concurrent
// destroy must wait until the test releases the request and the call returns.
// A resolve that doesn't pin lets the destroy return while the call is held.
TEST(chttpcli_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  char held_url[128];
  make_url(held_url, sizeof(held_url), "/held");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpcli_set_pool_size(cli, 1), ccol_success);

  atomic_store(&g_slow_started, 0);
  concurrent_req_arg_t occupant = {
      .cli = cli, .result_status = 0, .result_rv = ccol_unexpected_failure};
  memcpy(occupant.url, held_url, sizeof(held_url));
  pthread_t occupant_thread;
  bool occupant_started =
      (pthread_create(&occupant_thread, NULL, concurrent_req_thread,
                      &occupant) == 0);

  bool occupant_held = false;
  for (int i = 0; occupant_started && i < 10000 && !occupant_held; i++) {
    occupant_held = atomic_load(&g_slow_started) >= 1;
    if (!occupant_held) usleep(1000);
  }
  size_t pins_while_held = chttpcli__pin_count_for_tests(cli);

  destroy_observed_arg_t d = {.cli = cli};
  atomic_store(&d.returned, 0);
  pthread_t destroy_tid;
  bool destroy_started = false;
  bool destroy_returned_while_held = false;
  if (occupant_held) {
    destroy_started =
        (pthread_create(&destroy_tid, NULL, destroy_observed_thread, &d) == 0);
  }
  if (destroy_started) {
    for (int i = 0; i < 10000; i++) {
      if (chttpcli_set_pool_size(cli, 1) != ccol_success) break;
      usleep(1000);
    }
    for (int i = 0; i < 200 && !destroy_returned_while_held; i++) {
      destroy_returned_while_held = atomic_load(&d.returned) != 0;
      if (!destroy_returned_while_held) usleep(1000);
    }
  }

  held_release();
  if (destroy_started) pthread_join(destroy_tid, NULL);
  if (occupant_started) pthread_join(occupant_thread, NULL);
  if (!destroy_started) chttpcli_destroy(cli);

  REQUIRE_TRUE(occupant_started);
  REQUIRE_TRUE(occupant_held);
  REQUIRE_EQ(pins_while_held, (size_t)1);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_FALSE(destroy_returned_while_held);
  REQUIRE_EQ(occupant.result_rv, ccol_success);
  REQUIRE_EQ(occupant.result_status, 200);
}

// A skipped unpin fails nothing at once and shows up later as a destroy that
// never returns; asserting the pin count makes it fail here.
TEST(chttpcli_handle_lifecycle, resolve_and_unpin_leave_no_outstanding_pin) {
  char url[128];
  make_url(url, sizeof(url), "/hello");

  chttpcli_construct(cli);

  size_t pins_fresh = chttpcli__pin_count_for_tests(cli);

  ccol_retval set_rv = chttpcli_set_pool_size(cli, 2);
  size_t pins_after_set = chttpcli__pin_count_for_tests(cli);

  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  bool req_built = (req != NULL);
  ccol_retval rv = ccol_unexpected_failure;
  size_t pins_after_req = 0;
  if (req_built) {
    chttpcli_response *resp = NULL;
    rv = chttpcli_do(cli, req, &resp);
    if (resp) chttpcli_resp_free(resp);
    chttp_request_free(req);
    pins_after_req = chttpcli__pin_count_for_tests(cli);
  }

  if (pins_fresh != 0 || pins_after_set != 0 || pins_after_req != 0) {
    REQUIRE_EQ(pins_fresh, (size_t)0);
    REQUIRE_EQ(pins_after_set, (size_t)0);
    REQUIRE_EQ(pins_after_req, (size_t)0);
    return;
  }

  chttpcli_destroy(cli);

  REQUIRE_TRUE(req_built);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
}

// The handle that a pool size setter thread works on.
typedef struct {
  chttpcli h;
} pool_size_setter_arg_t;

// Calls chttpcli_set_pool_size on the handle in arg, a short call that
// resolves, pins and unpins, to race a destroy in resolve_unpin_race_stress.
static void *pool_size_setter_thread(void *arg) {
  pool_size_setter_arg_t *a = (pool_size_setter_arg_t *)arg;
  chttpcli_set_pool_size(a->h, 4);
  return NULL;
}

// A fast call that never blocks races a destroy, so the destroy can find no
// pin and never wait. The window is a few instructions wide, so it repeats on
// fresh clients.
TEST(chttpcli_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpcli cli = chttpcli_create(NULL);
    REQUIRE_NE(cli, CHTTPCLI_NULL);

    pool_size_setter_arg_t setter_arg = {.h = cli};
    concurrent_destroy_arg_t destroy_arg = {.h = cli};
    pthread_t setter_tid, destroy_tid;
    int setter_rv =
        pthread_create(&setter_tid, NULL, pool_size_setter_thread, &setter_arg);
    int destroy_rv = 0;
    if (setter_rv == 0) {
      destroy_rv = pthread_create(&destroy_tid, NULL, concurrent_destroy_thread,
                                  &destroy_arg);
    }
    if (setter_rv == 0) pthread_join(setter_tid, NULL);
    if (setter_rv == 0 && destroy_rv == 0) pthread_join(destroy_tid, NULL);
    REQUIRE_EQ(setter_rv, 0);
    REQUIRE_EQ(destroy_rv, 0);
  }
}

// glibc often hands the address of a freed client to the next one, so only the
// generation can tell a reused slot from a stale handle.
TEST(chttpcli_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpcli a = chttpcli_create(NULL);
  REQUIRE_NE(a, CHTTPCLI_NULL);
  chttpcli stale_a = a;
  chttpcli_destroy(a);

  chttpcli b = chttpcli_create(NULL);
  REQUIRE_NE(b, CHTTPCLI_NULL);

  REQUIRE_EQ(chttpcli_set_pool_size(b, 4), ccol_success);

  REQUIRE_EQ((void *)chttpcli__resolve_for_tests(stale_a), NULL);

  chttpcli_destroy(b);
}

// The capacity is read after the first cycle, because earlier tests may have
// grown the table already.
TEST(chttpcli_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  chttpcli cli0 = chttpcli_create(NULL);
  REQUIRE_NE(cli0, CHTTPCLI_NULL);
  chttpcli_destroy(cli0);
  size_t capacity_after_first = chttpcli__slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpcli cli = chttpcli_create(NULL);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    chttpcli_destroy(cli);
  }

  REQUIRE_EQ(chttpcli__slot_table_capacity_for_tests(), capacity_after_first);
}

TEST(chttpcli_handle_lifecycle, a_failed_free_list_push_loses_no_slot) {
  size_t before = chttpcli__slot_table_capacity_for_tests();
  size_t cycles = chttpcli__free_index_count_for_tests() + 8;
  bool all_ok = true;
  for (size_t i = 0; i < cycles && all_ok; i++) {
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) {
      all_ok = false;
      break;
    }
    chttpcli__force_next_free_index_push_failure_for_tests();
    chttpcli_destroy(cli);
  }
  chttpcli cli = chttpcli_create(NULL);
  bool usable = cli != CHTTPCLI_NULL &&
                chttpcli_set_request_timeout(cli, 1000000) == ccol_success;
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  size_t after = chttpcli__slot_table_capacity_for_tests();
  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(usable);
  REQUIRE_LE(after, before + 1);
}

// chttpcli_set_tls has several exit paths, and a missing unpin on any of them
// only shows as a later destroy that hangs.
TEST(tls, set_tls_all_exit_paths_release_pin) {
  {
    chttpcli cli = chttpcli_create(NULL);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    REQUIRE_EQ(chttpcli_set_tls(cli, NULL), ccol_success);
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }

  {
    chttpcli cli = chttpcli_create(NULL);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    chttp_tls_config tls = CHTTP_TLS_DEFAULT;
    tls.ca_bundle_path = "/nonexistent/ca-bundle.pem";
    REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }

  {
    char *cerr = NULL;
    chttpcli cli = chttpcli_create_mp(&g_hop_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_NULL);
    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = 0;
    chttp_tls_config tls = CHTTP_TLS_DEFAULT;
    tls.cert_path = "/nonexistent/cert.pem";
    tls.key_path = "/nonexistent/key.pem";
    REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_not_enough_memory);
    g_hop_fail_at_call = -1;
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }
}

// Real TLS handshakes
//
// A throwaway self-signed certificate made with the openssl command, and a
// small mock TLS server on raw OpenSSL calls, drive real HTTPS requests in
// Tier 1 and Tier 2.
//
// The body every TLS route answers with.
#define TLS_TEST_BODY "{\"status\":\"ok\"}"
// The size of the one TLS record /one-record-held-open sends: More than either
// tier takes in one read, less than a record can carry. The route keeps its end
// open for ONE_RECORD_HOLD_MS for the client to close.
#define ONE_RECORD_TOTAL 12000
#define ONE_RECORD_HOLD_MS 8000
// g_tls_early_release ends the hold of /early-reject-hold. The other two record
// how /one-record-held-open ended: The client closed it, or the hold ran out.
static atomic_int g_tls_early_release = 0;
static atomic_int g_one_record_closed_by_client = 0;
static atomic_int g_one_record_hold_expired = 0;

// The mock TLS server: Its context, its listener (atomic because the teardown
// writes it while the accept loop reads it), its port and thread, and the
// certificate files. g_tls_cert_ready is false when the openssl command
// couldn't make a certificate, and the TLS tests then skip themselves.
static SSL_CTX *g_tls_ssl_ctx = NULL;
static atomic_int g_tls_srv_fd = -1;
static int g_tls_srv_port = 0;
static pthread_t g_tls_accept_tid;
static atomic_int g_tls_srv_running = 0;
static char g_tls_cert_dir[256];
static char g_tls_cert_path[320];
static char g_tls_key_path[320];
static bool g_tls_cert_ready = false;
// How many connections the mock TLS server has accepted, to tell a reused
// connection from one that ran its own handshake.
static atomic_int g_tls_accept_count = 0;

// Gives how many connections the mock TLS server has accepted, so a TLS test
// can tell a reused connection from a new handshake.
static int tls_server_accept_count(void) {
  return atomic_load(&g_tls_accept_count);
}

// The registry of TLS connection threads. _tls_teardown joins them before it
// frees g_tls_ssl_ctx, which an SSL_accept may still use.
#define MAX_TLS_CONN_THREADS 64
static pthread_t g_tls_conn_threads[MAX_TLS_CONN_THREADS];
static int g_tls_conn_thread_count = 0;
static pthread_mutex_t g_tls_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

// Adds a TLS connection thread to g_tls_conn_threads under g_tls_conn_mutex, so
// _stop_tls_server can join it, after reaping the finished ones like
// register_conn_thread.
static void register_tls_conn_thread(pthread_t tid) {
  pthread_mutex_lock(&g_tls_conn_mutex);
  int kept = 0;
  for (int i = 0; i < g_tls_conn_thread_count; i++) {
    if (test_tryjoin(g_tls_conn_threads[i]) != 0) {
      g_tls_conn_threads[kept++] = g_tls_conn_threads[i];
    }
  }
  g_tls_conn_thread_count = kept;

  if (g_tls_conn_thread_count < MAX_TLS_CONN_THREADS) {
    g_tls_conn_threads[g_tls_conn_thread_count++] = tid;
  } else {
    pthread_detach(tid);
  }
  pthread_mutex_unlock(&g_tls_conn_mutex);
}

// Makes a self-signed certificate and key with the openssl command, for the
// mock TLS server. Returns 0, or -1 when this environment can't check TLS.
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

// Makes the mock TLS server's certificate for 127.0.0.1 in a fresh mkdtemp()
// directory and fills g_tls_cert_dir, g_tls_cert_path and g_tls_key_path.
// Returns 0, or -1.
static int _generate_self_signed_cert(void) {
  snprintf(g_tls_cert_dir, sizeof(g_tls_cert_dir),
           "/tmp/chttpcli_tls_test_XXXXXX");
  if (!mkdtemp(g_tls_cert_dir)) return -1;

  int dn = snprintf(g_tls_cert_path, sizeof(g_tls_cert_path), "%s/cert.pem",
                    g_tls_cert_dir);
  int kn = snprintf(g_tls_key_path, sizeof(g_tls_key_path), "%s/key.pem",
                    g_tls_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_tls_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_tls_key_path))
    return -1;

  return _openssl_selfsigned(g_tls_key_path, g_tls_cert_path, "127.0.0.1",
                             "IP:127.0.0.1");
}

// Removes the certificate files and their directory at teardown.
static void _remove_generated_cert(void) {
  if (g_tls_cert_path[0]) unlink(g_tls_cert_path);
  if (g_tls_key_path[0]) unlink(g_tls_key_path);
  if (g_tls_cert_dir[0]) rmdir(g_tls_cert_dir);
}

// Reads the headers of one request off ssl into buf and gives its path, so the
// mock TLS server can route it. No request in this section sends a body.
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

// Sends a complete JSON response over ssl that closes the connection, the
// answer of the simple TLS routes.
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

// LibreSSL can receive a KeyUpdate but has no call to send one.
#if defined(LIBRESSL_VERSION_NUMBER)
#define TLS_TEST_CAN_POKE 0
#else
#define TLS_TEST_CAN_POKE 1
#endif

// Sends a TLS 1.3 KeyUpdate that asks for no reply. It carries no application
// bytes but makes the socket readable for the peer, which the /ka-poke route
// uses to wake a client that watches an idle connection. Does nothing on TLS
// 1.2.
static void _tls_poke_key_update(SSL *ssl) {
#if TLS_TEST_CAN_POKE
  if (SSL_version(ssl) != TLS1_3_VERSION) return;
  if (SSL_key_update(ssl, SSL_KEY_UPDATE_NOT_REQUESTED) != 1) return;
  SSL_do_handshake(ssl);
#else
  (void)ssl;
#endif
}

// Waits for the peer's next request on a keep-alive connection and pokes it
// again every POKE_INTERVAL_MS, because only a poke that lands after the peer
// started watching is useful. Returns false when the poke budget runs out, and
// the caller closes its end.
static bool _tls_wait_readable_poking(SSL *ssl, int fd) {
  enum { POKE_INTERVAL_MS = 20, MAX_POKES = 250 };
  for (int i = 0; i < MAX_POKES; i++) {
    if (SSL_pending(ssl) > 0) return true;
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&pfd, 1, POKE_INTERVAL_MS);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (rc > 0) return true;
    _tls_poke_key_update(ssl);
  }
  return false;
}

// Serves requests on one connection until the peer stops sending, so a test
// can tell a reused connection from a new handshake. /ka waits under a receive
// timeout; /ka-poke also pokes while it waits.
static void _tls_keepalive_loop(SSL *ssl, int fd, bool poke_while_idle) {
  struct timeval ka_rcvto = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ka_rcvto, sizeof(ka_rcvto));
  for (;;) {
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      strlen(TLS_TEST_BODY));
    if (hl <= 0 || SSL_write(ssl, hdr, hl) <= 0) break;
    if (SSL_write(ssl, TLS_TEST_BODY, (int)strlen(TLS_TEST_BODY)) <= 0) break;
    if (poke_while_idle && !_tls_wait_readable_poking(ssl, fd)) break;
    char nbuf[8192];
    char npath[256];
    _tls_read_request(ssl, nbuf, sizeof(nbuf), npath, sizeof(npath));
    if (nbuf[0] == '\0') break;
  }
}

// Reads and drops what arrives on fd until the peer closes, for 2 s and 1 MiB
// at most. The early-reject routes use it for their lingering close, so a reset
// can only come after a peer that kept sending that much.
static void ers_discard(int fd, char *buf, size_t buf_size) {
  struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  size_t got = 0;
  while (got < 1024u * 1024u) {
    ssize_t r = recv(fd, buf, buf_size, 0);
    if (r == 0 ||
        (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      break;
    if (r > 0) got += (size_t)r;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - t0.tv_sec >= 2) break;
  }
}

static void *_tls_conn_thread_body(void *arg);

// The thread function of one mock TLS connection (fd in arg). It serves the
// connection and then drops g_mock_conns_open.
static void *_tls_conn_thread(void *arg) {
  void *r = _tls_conn_thread_body(arg);
  atomic_fetch_sub(&g_mock_conns_open, 1);
  return r;
}

// Runs the handshake on the fd in arg, counts it in g_tls_accept_count and
// serves the route the request names.
static void *_tls_conn_thread_body(void *arg) {
  int fd = (int)(intptr_t)arg;
  atomic_fetch_add(&g_tls_accept_count, 1);
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
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

  if (strcmp(path, "/ka") == 0 || strcmp(path, "/ka-poke") == 0) {
    _tls_keepalive_loop(ssl, fd, strcmp(path, "/ka-poke") == 0);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/one-record-held-open") == 0) {
    char rec[ONE_RECORD_TOTAL];
    int hl = snprintf(rec, sizeof(rec),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %06d\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      0);
    size_t body_len = sizeof(rec) - (size_t)hl;
    snprintf(rec, sizeof(rec),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %06zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             body_len);
    memset(rec + hl, 'y', body_len);
    SSL_write(ssl, rec, (int)sizeof(rec));
    struct pollfd hp = {.fd = fd, .events = POLLIN, .revents = 0};
    int prc;
    do {
      prc = poll(&hp, 1, ONE_RECORD_HOLD_MS);
    } while (prc < 0 && errno == EINTR);
    if (prc > 0)
      atomic_fetch_add(&g_one_record_closed_by_client, 1);
    else
      atomic_fetch_add(&g_one_record_hold_expired, 1);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/early-reject-close") == 0) {
    static const char r[] =
        "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n"
        "Connection: close\r\n\r\ntoo large";
    SSL_write(ssl, r, (int)sizeof(r) - 1);
    shutdown(fd, SHUT_WR);
    ers_discard(fd, buf, sizeof(buf));
    SSL_free(ssl);
    close(fd);
    return NULL;
  }
  if (strcmp(path, "/early-reject-hold") == 0) {
    static const char r[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\n"
        "unauthorized";
    SSL_write(ssl, r, (int)sizeof(r) - 1);
    for (int i = 0; i < 1500 && !atomic_load(&g_tls_early_release); i++)
      usleep(10000);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/eof-no-notify") == 0 ||
      strcmp(path, "/eof-mid-record") == 0) {
    static const char r[] =
        "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\neof-body-ok";
    SSL_write(ssl, r, (int)sizeof(r) - 1);
    if (strcmp(path, "/eof-mid-record") == 0) {
      static const char part[] =
          "\x17\x03\x03\x00\x40"
          "0123456789";
      ssize_t w = send(fd, part, sizeof(part) - 1, MSG_NOSIGNAL);
      (void)w;
    }
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/echo-headers") == 0) {
    _tls_send_response(ssl, 200, "OK", buf, strlen(buf));
  } else if (strcmp(path, "/large") == 0) {
    char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    _tls_send_response(ssl, 200, "OK", large_body, sizeof(large_body));
  } else {
    _tls_send_response(ssl, 200, "OK", TLS_TEST_BODY, strlen(TLS_TEST_BODY));
  }

  struct timeval rcvto = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
  char drain[64];
  while (SSL_read(ssl, drain, sizeof(drain)) > 0) {
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  close(fd);
  return NULL;
}

// Accepts TLS connections for the whole process, counts them in
// g_mock_conns_open and starts a registered thread for each, until
// g_tls_srv_running clears.
static void *_tls_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_tls_srv_running)) {
    int fd = test_accept(g_tls_srv_fd);
    if (fd < 0) {
      if (!atomic_load(&g_tls_srv_running)) break;
      continue;
    }
    if (!atomic_load(&g_tls_srv_running)) {
      close(fd);
      break;
    }
    atomic_fetch_add(&g_mock_conns_open, 1);
    pthread_t tid;
    if (pthread_create(&tid, NULL, _tls_conn_thread, (void *)(intptr_t)fd) !=
        0) {
      close(fd);
      atomic_fetch_sub(&g_mock_conns_open, 1);
      continue;
    }
    register_tls_conn_thread(tid);
  }
  return NULL;
}

// Binds the mock TLS listener on an ephemeral loopback port, sets g_tls_srv_fd
// and g_tls_srv_port and starts the accept loop. Returns 0, or -1.
static int _start_tls_server(void) {
  g_tls_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_tls_srv_fd < 0) return -1;
  int opt = 1;
  setsockopt(g_tls_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(g_tls_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(g_tls_srv_fd, 64) != 0) {
    close(g_tls_srv_fd);
    g_tls_srv_fd = -1;
    return -1;
  }

  socklen_t len = sizeof(addr);
  getsockname(g_tls_srv_fd, (struct sockaddr *)&addr, &len);
  g_tls_srv_port = ntohs(addr.sin_port);

  atomic_store(&g_tls_srv_running, 1);
  if (pthread_create(&g_tls_accept_tid, NULL, _tls_accept_loop, NULL) != 0) {
    close(g_tls_srv_fd);
    g_tls_srv_fd = -1;
    atomic_store(&g_tls_srv_running, 0);
    return -1;
  }
  return 0;
}

// Stops the mock TLS server: Stops the accept loop and joins every thread in
// g_tls_conn_threads, so none still uses g_tls_ssl_ctx.
static void _stop_tls_server(void) {
  if (g_tls_srv_fd < 0) return;
  atomic_store(&g_tls_srv_running, 0);
  test_wake_listener(g_tls_srv_fd);
  close(g_tls_srv_fd);
  g_tls_srv_fd = -1;
  pthread_join(g_tls_accept_tid, NULL);

  pthread_mutex_lock(&g_tls_conn_mutex);
  for (int i = 0; i < g_tls_conn_thread_count; i++) {
    pthread_join(g_tls_conn_threads[i], NULL);
  }
  g_tls_conn_thread_count = 0;
  pthread_mutex_unlock(&g_tls_conn_mutex);
}

// The atexit handler of the TLS section: Stops the server, frees
// g_tls_ssl_ctx and removes the certificate.
static void _tls_teardown(void) {
  _stop_tls_server();
  if (g_tls_ssl_ctx) {
    SSL_CTX_free(g_tls_ssl_ctx);
    g_tls_ssl_ctx = NULL;
  }
  _remove_generated_cert();
}

// Makes the certificate and starts the mock TLS server before any test runs,
// and sets g_tls_cert_ready. When the openssl command can't make a certificate
// the TLS tests skip themselves. The destructor probe's re-executed process
// (g_dtor_probe_fd set) skips all of it.
__attribute__((constructor)) static void _tls_setup(void) {
  if (g_dtor_probe_fd >= 0) return;
  if (_generate_self_signed_cert() != 0) {
    fprintf(stderr,
            "WARNING: could not generate a self-signed cert with the openssl "
            "CLI. This environment skips the real TLS handshake tests.\n");
    g_tls_cert_ready = false;
    atexit(_tls_teardown);
    return;
  }

  g_tls_ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!g_tls_ssl_ctx) {
    fprintf(stderr, "FATAL: SSL_CTX_new failed\n");
    exit(1);
  }
  if (SSL_CTX_use_certificate_file(g_tls_ssl_ctx, g_tls_cert_path,
                                   SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_use_PrivateKey_file(g_tls_ssl_ctx, g_tls_key_path,
                                  SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "FATAL: could not load generated cert/key into SSL_CTX\n");
    exit(1);
  }

  if (_start_tls_server() != 0) {
    fprintf(stderr, "FATAL: could not start mock TLS server\n");
    exit(1);
  }
  g_tls_cert_ready = true;
  atexit(_tls_teardown);
}

// Writes https://127.0.0.1:<tls port><path> into buf, the URL of the TLS
// tests.
static void make_tls_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "https://127.0.0.1:%d%s", g_tls_srv_port, path);
}

// Tier 1 has its own connect and handshake code, so it gets its own real
// handshake and verification tests.
TEST(sync_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);

  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, TLS_TEST_BODY);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

// Each failed allocation either reports ccol_not_enough_memory and leaves the
// defaults (which don't trust the test certificate), or succeeds and a request
// is served. A call that hides a failure leaves every HTTPS request failing.
TEST(sync_tls, a_set_tls_that_runs_out_of_memory_reports_it) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;

  enum { SWEEP_MAX = 300 };
  int oom = 0, served = 0, wrong = 0;
  bool reached_the_end = false;
  for (int idx = 0; idx < SWEEP_MAX && !reached_the_end; idx++) {
    cf_arm(-1);
    chttpcli cli = chttpcli_create_mp(&g_cf_mp, NULL);
    if (cli == CHTTPCLI_NULL) {
      wrong++;
      break;
    }
    cf_arm(idx);
    ccol_retval rv = chttpcli_set_tls(cli, &tls);
    bool failed = atomic_load(&g_cf_failed);
    cf_arm(-1);
    if (!failed) reached_the_end = true;

    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval req_rv =
        req ? chttpcli_do(cli, req, &resp) : ccol_not_enough_memory;
    int status = req_rv == ccol_success ? resp->status_code : -1;
    if (req_rv == ccol_success) chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);

    if (rv == ccol_not_enough_memory &&
        req_rv == ccol_http_tls_cert_verification_failed)
      oom++;
    else if (rv == ccol_success && status == 200)
      served++;
    else
      wrong++;
  }

  REQUIRE_TRUE(reached_the_end);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_GT(oom, 0);
  REQUIRE_GT(served, 0);
}

// After a reset that lost an allocation, the next request must rebuild the
// context and fail verification the way the defaults do, never with a
// certificate file error.
TEST(sync_tls, a_reset_that_runs_out_of_memory_recovers_at_the_next_request) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");

  enum { SWEEP_MAX = 300 };
  int oom = 0, ok = 0, wrong = 0;
  bool reached_the_end = false;
  for (int idx = 0; idx < SWEEP_MAX && !reached_the_end; idx++) {
    cf_arm(-1);
    chttpcli cli = chttpcli_create_mp(&g_cf_mp, NULL);
    if (cli == CHTTPCLI_NULL) {
      wrong++;
      break;
    }
    cf_arm(idx);
    ccol_retval rv = chttpcli_set_tls(cli, NULL);
    bool failed = atomic_load(&g_cf_failed);
    cf_arm(-1);
    if (!failed) reached_the_end = true;

    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval req_rv =
        req ? chttpcli_do(cli, req, &resp) : ccol_not_enough_memory;
    if (req_rv == ccol_success) chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);

    if (req_rv != ccol_http_tls_cert_verification_failed)
      wrong++;
    else if (rv == ccol_not_enough_memory)
      oom++;
    else if (rv == ccol_success)
      ok++;
    else
      wrong++;
  }

  REQUIRE_TRUE(reached_the_end);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_GT(oom, 0);
  REQUIRE_GT(ok, 0);
}

TEST(sync_tls, an_async_request_after_a_reset_without_memory_recovers) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");

  enum { SWEEP_MAX = 300 };
  int oom = 0, wrong = 0, rearm_missed = 0;
  bool reached_the_end = false;
  for (int idx = 0; idx < SWEEP_MAX && !reached_the_end; idx++) {
    cf_arm(-1);
    chttpcli cli = chttpcli_create_mp(&g_cf_mp, NULL);
    if (cli == CHTTPCLI_NULL) {
      wrong++;
      break;
    }
    cf_arm(idx);
    ccol_retval rv = chttpcli_set_tls(cli, NULL);
    bool failed = atomic_load(&g_cf_failed);
    cf_arm(-1);
    if (!failed) reached_the_end = true;
    if (rv == ccol_not_enough_memory) oom++;

    ccol_retval async_rv = ccol_unexpected_failure;
    cthrpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result *raw = chttpcli_async_result_get(f);
      if (raw) {
        async_rv = raw->rv;
        if (raw->rv == ccol_success) chttpcli_resp_free(raw->resp);
      }
      chttpcli_async_result_free(raw);
      cthrpool_future_free(f);
    }
    if (rv == ccol_not_enough_memory) {
      cf_arm(idx);
      if (chttpcli_set_tls(cli, NULL) != ccol_not_enough_memory) rearm_missed++;
      cf_arm(-1);
    }
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval pooled_rv =
        req ? chttpcli_do_pooled(cli, req, &resp) : ccol_not_enough_memory;
    if (pooled_rv == ccol_success) chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);

    if (async_rv != ccol_http_tls_cert_verification_failed ||
        pooled_rv != ccol_http_tls_cert_verification_failed)
      wrong++;
  }
  wait_for_async_engine_idle();

  REQUIRE_TRUE(reached_the_end);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_EQ(rearm_missed, 0);
  REQUIRE_GT(oom, 0);
}

TEST(sync_tls, large_body_response_over_tls) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpcli_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);

  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_destroy(cli);
}

TEST(sync_tls, untrusted_cert_fails_verification) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttpcli_destroy(cli);
}

TEST(async_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, TLS_TEST_BODY);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_tls, large_body_response_over_tls) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/large");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpcli_resp_free(resp);
  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_tls, untrusted_cert_fails_verification) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  cthrpool_future *f = chttpcli_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval rv = raw->rv;
  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpcli_async_result_free(raw);
  cthrpool_future_free(f);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(async_tls, concurrent_https_requests_all_succeed) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  enum { N = 6 };
  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");

  cthrpool_future *futures[N];
  for (int i = 0; i < N; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    futures[i] = chttpcli_do_async(cli, req);
    REQUIRE_NE((void *)futures[i], NULL);
    chttp_request_free(req);
  }

  for (int i = 0; i < N; i++) {
    chttpcli_async_result *raw = chttpcli_async_result_get(futures[i]);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpcli_resp_free(resp);
    chttpcli_async_result_free(raw);
    cthrpool_future_free(futures[i]);
  }

  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

// TLS I/O direction, TLS policy of pooled connections, engine reaper
//
// SSL_write can need the socket to become readable and SSL_read writable (a
// renegotiation, a KeyUpdate). The injection hook reproduces that on a live
// connection, and only waiting or registering in the direction it names
// releases it. The tests arm it once a connection is pooled and disarm it
// before any assertion.
#define TLS_DIR_INJECT_OFF 0
#define TLS_DIR_INJECT_WRITE_WANTS_READ 1
#define TLS_DIR_INJECT_READ_WANTS_WRITE 2

// The ceiling of the warm-up hop, a safety net only: It runs a full handshake,
// which can take a second under valgrind.
#define TLS_DIR_WARMUP_TIMEOUT_MS 30000

// Opens one HTTPS keep-alive connection on cli and puts it into the pool, so
// the TLS direction and TLS generation tests start from a pooled connection.
// Returns false when there's no usable certificate or the request failed.
static bool tls_warm_pooled_sync_connection(chttpcli cli, const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  if (rv != ccol_success || !resp) return false;
  bool ok = (resp->status_code == 200);
  chttpcli_resp_free(resp);
  return ok;
}

TEST(tls_io_direction, sync_write_wanting_read_waits_on_the_read_direction) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpcli_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");
  bool warmed = tls_warm_pooled_sync_connection(cli, url);

  ccol_retval rv = ccol_unexpected_failure;
  ccol_retval bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  if (warmed) {
    bound_rv = chttpcli_set_request_timeout(cli, 700000);
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_WRITE_WANTS_READ);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      rv = chttpcli_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) {
        status = resp->status_code;
        chttpcli_resp_free(resp);
      }
    }
    released = chttpcli__tls_dir_released_for_tests();
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  chttpcli_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, sync_read_wanting_write_waits_on_the_write_direction) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpcli_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");
  bool warmed = tls_warm_pooled_sync_connection(cli, url);

  ccol_retval rv = ccol_unexpected_failure;
  ccol_retval bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  if (warmed) {
    bound_rv = chttpcli_set_request_timeout(cli, 700000);
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_READ_WANTS_WRITE);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      rv = chttpcli_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) {
        status = resp->status_code;
        chttpcli_resp_free(resp);
      }
    }
    released = chttpcli__tls_dir_released_for_tests();
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  chttpcli_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, async_read_wanting_write_registers_write_interest) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpcli_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  bool warmed = false;
  cthrpool_future *fw = async_get(cli, url);
  if (fw) {
    chttpcli_async_result *rw = chttpcli_async_result_get(fw);
    if (rw) {
      warmed =
          (rw->rv == ccol_success && rw->resp && rw->resp->status_code == 200);
      if (rw->resp) chttpcli_resp_free(rw->resp);
      chttpcli_async_result_free(rw);
    }
    cthrpool_future_free(fw);
  }

  ccol_retval rv = ccol_unexpected_failure;
  ccol_retval bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  unsigned write_interest = 0;
  if (warmed) {
    bound_rv = chttpcli_set_request_timeout(cli, 1500000);
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_READ_WANTS_WRITE);
    cthrpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result *raw = chttpcli_async_result_get(f);
      if (raw) {
        rv = raw->rv;
        if (raw->resp) {
          status = raw->resp->status_code;
          chttpcli_resp_free(raw->resp);
        }
        chttpcli_async_result_free(raw);
      }
      cthrpool_future_free(f);
    }
    released = chttpcli__tls_dir_released_for_tests();
    write_interest = chttpcli__tls_dir_write_interest_while_reading_for_tests();
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  REQUIRE_GT(write_interest, 0u);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, async_write_wanting_read_registers_read_interest) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  if (!TLS_TEST_CAN_POKE) {
    fprintf(stderr,
            "SKIP: the TLS library of this build cannot send a KeyUpdate\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpcli_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka-poke");

  bool warmed = false;
  cthrpool_future *fw = async_get(cli, url);
  if (fw) {
    chttpcli_async_result *rw = chttpcli_async_result_get(fw);
    if (rw) {
      warmed =
          (rw->rv == ccol_success && rw->resp && rw->resp->status_code == 200);
      if (rw->resp) chttpcli_resp_free(rw->resp);
      chttpcli_async_result_free(rw);
    }
    cthrpool_future_free(fw);
  }

  ccol_retval rv = ccol_unexpected_failure;
  ccol_retval bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  unsigned read_interest = 0;
  unsigned block_attempts = 0;
  int accepts_before = tls_server_accept_count();
  int accepts_after = accepts_before;
  if (warmed) {
    bound_rv = chttpcli_set_request_timeout(cli, 3000000);
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_WRITE_WANTS_READ);
    cthrpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result *raw = chttpcli_async_result_get(f);
      if (raw) {
        rv = raw->rv;
        if (raw->resp) {
          status = raw->resp->status_code;
          chttpcli_resp_free(raw->resp);
        }
        chttpcli_async_result_free(raw);
      }
      cthrpool_future_free(f);
    }
    released = chttpcli__tls_dir_released_for_tests();
    read_interest = chttpcli__tls_dir_read_interest_while_writing_for_tests();
    block_attempts = chttpcli__tls_dir_block_attempts_for_tests();
    accepts_after = tls_server_accept_count();
    chttpcli__tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  REQUIRE_GT(read_interest, 0u);
  REQUIRE_TRUE(released);
  REQUIRE_LE(block_attempts, 8u);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(accepts_after, accepts_before);
}

// The idle pools key on the origin alone, which says nothing about the TLS
// policy a pooled connection did its handshake under, so chttpcli_set_tls must
// retire every pooled HTTPS connection.
TEST(tls_config_generation, set_tls_retires_pooled_sync_connections) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  int accepts_before = tls_server_accept_count();
  bool first = tls_warm_pooled_sync_connection(cli, url);
  bool second = tls_warm_pooled_sync_connection(cli, url);
  int accepts_after_reuse = tls_server_accept_count();

  chttp_tls_config tighter = CHTTP_TLS_DEFAULT;
  tighter.ca_bundle_path = g_tls_cert_path;
  tighter.insecure_skip_hostname_check = true;
  ccol_retval set_rv = chttpcli_set_tls(cli, &tighter);

  bool third = tls_warm_pooled_sync_connection(cli, url);
  int accepts_after_reconfig = tls_server_accept_count();
  chttpcli_destroy(cli);

  REQUIRE_TRUE(first);
  REQUIRE_TRUE(second);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(third);
  REQUIRE_EQ(accepts_after_reuse - accepts_before, 1);
  REQUIRE_EQ(accepts_after_reconfig - accepts_before, 2);
}

TEST(tls_config_generation, set_tls_retires_pooled_async_connections) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpcli_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  int accepts_before = tls_server_accept_count();
  bool ok[3] = {false, false, false};
  for (int i = 0; i < 2; i++) {
    cthrpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result *raw = chttpcli_async_result_get(f);
      if (raw) {
        ok[i] = (raw->rv == ccol_success && raw->resp &&
                 raw->resp->status_code == 200);
        if (raw->resp) chttpcli_resp_free(raw->resp);
        chttpcli_async_result_free(raw);
      }
      cthrpool_future_free(f);
    }
  }
  int accepts_after_reuse = tls_server_accept_count();

  chttp_tls_config tighter = CHTTP_TLS_DEFAULT;
  tighter.ca_bundle_path = g_tls_cert_path;
  tighter.insecure_skip_hostname_check = true;
  ccol_retval set_rv = chttpcli_set_tls(cli, &tighter);

  cthrpool_future *f3 = async_get(cli, url);
  if (f3) {
    chttpcli_async_result *raw = chttpcli_async_result_get(f3);
    if (raw) {
      ok[2] = (raw->rv == ccol_success && raw->resp &&
               raw->resp->status_code == 200);
      if (raw->resp) chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(f3);
  }
  int accepts_after_reconfig = tls_server_accept_count();
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_TRUE(ok[0]);
  REQUIRE_TRUE(ok[1]);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(ok[2]);
  REQUIRE_EQ(accepts_after_reuse - accepts_before, 1);
  REQUIRE_EQ(accepts_after_reconfig - accepts_before, 2);
}

TEST(async_idle_pool, abandoned_reused_ctx_is_shut_down_under_its_idle_lock) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  char *cerr = NULL;
  chttpcli cli = chttpcli_create_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  chttpcli__reset_abandon_shutdown_probe_for_tests();

  int observed = -1;
  enum { SWEEP_UPPER = 40 };
  for (int idx = 0; idx < SWEEP_UPPER && observed < 0; idx++) {
    g_hop_fail_at_call = -1;
    cthrpool_future *fw = async_get(cli, url);
    if (!fw) break;
    chttpcli_async_result *rw = chttpcli_async_result_get(fw);
    if (rw) {
      if (rw->resp) chttpcli_resp_free(rw->resp);
      chttpcli_async_result_free(rw);
    }
    cthrpool_future_free(fw);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;
    cthrpool_future *f2 = async_get(cli, url);
    if (f2) {
      chttpcli_async_result *raw2 = chttpcli_async_result_get(f2);
      if (raw2) {
        if (raw2->resp) chttpcli_resp_free(raw2->resp);
        chttpcli_async_result_free(raw2);
      }
      cthrpool_future_free(f2);
    }
    g_hop_fail_at_call = -1;

    for (int i = 0; i < 100; i++) {
      observed = chttpcli__abandon_shutdown_under_lock_for_tests();
      if (observed >= 0) break;
      usleep(1000);
    }
  }

  g_hop_fail_at_call = -1;
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_NE(observed, -1);
  REQUIRE_EQ(observed, 1);
}

// Drops one engine reference from its own thread, for
// async_engine.a_reaper_handle_is_never_lost_to_a_second_spawn.
static void *_reaper_race_release_thread(void *arg) {
  (void)arg;
  chttpcli__engine_release_for_tests();
  return NULL;
}

// Runs one more acquire and release cycle of the engine and sets the flag arg
// points to, for the same reaper race test.
static void *_reaper_race_second_cycle_thread(void *arg) {
  atomic_bool *done = (atomic_bool *)arg;
  if (chttpcli__engine_acquire_for_tests() == ccol_success)
    chttpcli__engine_release_for_tests();
  atomic_store(done, true);
  return NULL;
}

TEST(async_engine, a_reaper_handle_is_never_lost_to_a_second_spawn) {
  chttpcli__engine_wait_for_quiescence_for_tests();
  unsigned created_before = chttpcli__reaper_created_count_for_tests();
  unsigned joined_before = chttpcli__reaper_joined_count_for_tests();
  REQUIRE_EQ(created_before, joined_before);
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);

  chttpcli__hold_next_reaper_spawn_for_tests();
  pthread_t releaser;
  bool releaser_started =
      pthread_create(&releaser, NULL, _reaper_race_release_thread, NULL) == 0;
  bool held = false;
  for (int i = 0; releaser_started && i < 10000 && !held; ++i) {
    held = chttpcli__reaper_spawn_is_held_for_tests();
    if (!held) usleep(1000);
  }

  atomic_bool second_done;
  atomic_init(&second_done, false);
  pthread_t second;
  bool second_started = false;
  if (held) {
    second_started =
        pthread_create(&second, NULL, _reaper_race_second_cycle_thread,
                       &second_done) == 0;
    for (int i = 0; second_started && i < 1000 && !atomic_load(&second_done);
         ++i)
      usleep(1000);
  }
  chttpcli__release_reaper_spawn_for_tests();
  if (second_started) pthread_join(second, NULL);
  if (releaser_started) {
    pthread_join(releaser, NULL);
  } else {
    chttpcli__engine_release_for_tests();
  }
  wait_for_async_engine_idle();

  unsigned created = chttpcli__reaper_created_count_for_tests();
  unsigned joined = chttpcli__reaper_joined_count_for_tests();
  REQUIRE_TRUE(releaser_started);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(second_started);
  REQUIRE_EQ(created - created_before, 2u);
  REQUIRE_EQ(joined - joined_before, 2u);
}

TEST(async_engine, reaper_spawn_failure_is_retried_by_the_sweep) {
  wait_for_async_engine_idle();
  unsigned abandoned_before =
      chttpcli__engine_reaper_abandoned_count_for_tests();
  REQUIRE_EQ(chttpcli__engine_acquire_for_tests(), ccol_success);

  chttpcli__force_reaper_spawn_fail_for_tests(true);
  chttpcli__engine_release_for_tests();
  unsigned abandoned_after =
      chttpcli__engine_reaper_abandoned_count_for_tests();
  bool still_running = chttpcli__engine_running_for_tests();
  int refs = chttpcli__engine_ref_count_for_tests();
  chttpcli__engine_wait_for_quiescence_for_tests();
  chttpcli__force_reaper_spawn_fail_for_tests(false);

  bool reaped_by_sweep = false;
  for (int i = 0; i < 1000 && !reaped_by_sweep; i++) {
    reaped_by_sweep = !chttpcli__engine_running_for_tests();
    if (!reaped_by_sweep) usleep(10000);
  }
  ccol_retval reacquire_rv = ccol_success;
  if (!reaped_by_sweep) {
    reacquire_rv = chttpcli__engine_acquire_for_tests();
    if (reacquire_rv == ccol_success) chttpcli__engine_release_for_tests();
  }
  wait_for_async_engine_idle();
  bool running_at_end = chttpcli__engine_running_for_tests();

  REQUIRE_EQ(reacquire_rv, ccol_success);
  REQUIRE_GE(abandoned_after - abandoned_before, 1u);
  REQUIRE_TRUE(still_running);
  REQUIRE_EQ(refs, 0);
  REQUIRE_TRUE(reaped_by_sweep);
  REQUIRE_FALSE(running_at_end);
}

// On a reactor thread a teardown would destroy the loop whose callback it runs
// in, which is fatal, so the engine must stay up when the reaper can't start.
// The child arranges that an IDLE dispatch drops the last reference, and
// reports through a pipe because valgrind overrides a forked child's exit
// status.
TEST(async_engine, reaper_spawn_failure_on_a_reactor_thread_does_not_abort) {
  wait_for_async_engine_idle();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    char marker = 'F';
    (void)chttpcli_set_engine_num_reactor_threads(4);
    chttpcli cli = chttpcli_create(NULL);
    if (cli != CHTTPCLI_NULL) {
      char child_url[160];
      make_url(child_url, sizeof(child_url), "/keepalive");
      cthrpool_future *f = async_get(cli, child_url);
      if (f) {
        chttpcli_async_result *raw = chttpcli_async_result_get(f);
        if (raw) {
          if (raw->resp) chttpcli_resp_free(raw->resp);
          chttpcli_async_result_free(raw);
        }
        cthrpool_future_free(f);
      }
      if (poll_engine_ref_count(1) == 1) {
        unsigned before = chttpcli__engine_reaper_abandoned_count_for_tests();
        chttpcli__force_reaper_spawn_fail_for_tests(true);
        chttpcli__shutdown_async_idle_connections_for_tests(
            chttpcli__resolve_for_tests(cli));
        bool abandoned = false;
        for (int i = 0; i < 10000 && !abandoned; i++) {
          abandoned =
              chttpcli__engine_reaper_abandoned_count_for_tests() > before;
          if (!abandoned) usleep(1000);
        }
        if (abandoned && chttpcli__engine_running_for_tests()) marker = 'K';
      }
    }
    ssize_t written = write(pfd[1], &marker, 1);
    (void)written;
    test_child_exit(0);
  }

  close(pfd[1]);
  char marker = 0;
  ssize_t got = 0;
  bool timed_out = false;
  if (pid != -1) {
    struct pollfd pr = {.fd = pfd[0], .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pr, 1, 120000);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0) {
      do {
        got = read(pfd[0], &marker, 1);
      } while (got < 0 && errno == EINTR);
    } else {
      timed_out = true;
      kill(pid, SIGKILL);
    }
  }
  close(pfd[0]);
  int status = 0;
  if (pid != -1) waitpid(pid, &status, 0);

  REQUIRE_NE(pid, -1);
  REQUIRE_FALSE(timed_out);
  REQUIRE_EQ((int)got, 1);
  REQUIRE_EQ((int)marker, (int)'K');
  REQUIRE_TRUE(WIFEXITED(status));
}

// The grandchild side of engine_logger_of_an_inherited_engine_is_closed:
// checks whether the inherited logger still writes, reports on report_fd and
// never returns.
static void _inherited_logger_grandchild(clog inherited, int report_fd) {
  test_child_no_core();
  char marker = 'A';
  if (chttpcli__engine_acquire_for_tests() == ccol_success) {
    clog now = chttpcli__engine_logger_for_tests();
    marker = (now == CLOG_NULL) ? 'Z' : (now == inherited ? 'S' : 'N');
  }
  ssize_t w = write(report_fd, &marker, 1);
  (void)w;
  (void)clog_get_level(inherited);
  marker = 'L';
  w = write(report_fd, &marker, 1);
  (void)w;
  test_child_exit(0);
}

// A fork child inherits an engine with no reference and its fallback logger;
// its own engine opens a new logger and the child must close the inherited one,
// which nothing else would close. The grandchild reports 'S' and then 'L' when
// the inherited handle is still in use.
TEST(async_engine, engine_logger_of_an_inherited_engine_is_closed) {
  TEST_SKIP_FORK_AGAIN_IN_A_THREADED_CHILD();
  wait_for_async_engine_idle();
  REQUIRE_FALSE(chttpcli__engine_running_for_tests());

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    char verdict[2] = {'F', 'F'};
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpcli__clear_engine_user_logger_for_tests();
    (void)chttpcli_set_engine_num_reactor_threads(4);
    chttpcli cli = chttpcli_create(NULL);
    clog inherited = CLOG_NULL;
    bool abandoned = false;
    if (cli != CHTTPCLI_NULL) {
      char child_url[160];
      make_url(child_url, sizeof(child_url), "/keepalive");
      cthrpool_future *f = async_get(cli, child_url);
      if (f) {
        chttpcli_async_result *raw = chttpcli_async_result_get(f);
        if (raw) {
          if (raw->resp) chttpcli_resp_free(raw->resp);
          chttpcli_async_result_free(raw);
        }
        cthrpool_future_free(f);
      }
      if (poll_engine_ref_count(1) == 1) {
        unsigned before = chttpcli__engine_reaper_abandoned_count_for_tests();
        chttpcli__force_reaper_spawn_fail_for_tests(true);
        chttpcli__shutdown_async_idle_connections_for_tests(
            chttpcli__resolve_for_tests(cli));
        for (int i = 0; i < 10000 && !abandoned; i++) {
          abandoned =
              chttpcli__engine_reaper_abandoned_count_for_tests() > before;
          if (!abandoned) usleep(1000);
        }
        inherited = chttpcli__engine_logger_for_tests();
      }
    }
    verdict[0] = !abandoned                              ? 'a'
                 : inherited == CLOG_NULL                ? 'b'
                 : !chttpcli__engine_running_for_tests() ? 'c'
                                                         : 'F';
    if (verdict[0] == 'F') {
      int gfd[2];
      if (pipe(gfd) == 0) {
        pid_t g = fork();
        if (g == 0) {
          close(gfd[0]);
          _inherited_logger_grandchild(inherited, gfd[1]);
        }
        close(gfd[1]);
        if (g > 0) {
          char got[2] = {0, 0};
          ssize_t n = 0;
          for (int i = 0; i < 2; i++) {
            struct pollfd pr = {.fd = gfd[0], .events = POLLIN, .revents = 0};
            if (poll(&pr, 1, 60000) <= 0) break;
            ssize_t r = read(gfd[0], &got[i], 1);
            if (r != 1) break;
            n++;
          }
          int st = 0;
          if (n == 0) kill(g, SIGKILL);
          waitpid(g, &st, 0);
          verdict[0] = got[0] ? got[0] : 'F';
          verdict[1] = (n == 1 && WIFSIGNALED(st)) ? 'C' : (n == 2 ? 'L' : 'F');
        }
        close(gfd[0]);
      }
    }
    ssize_t w = write(pfd[1], verdict, 2);
    (void)w;
    test_child_exit(0);
  }

  close(pfd[1]);
  char verdict[2] = {0, 0};
  ssize_t got = 0;
  bool timed_out = false;
  if (pid != -1) {
    struct pollfd pr = {.fd = pfd[0], .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pr, 1, 120000);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0) {
      do {
        got = read(pfd[0], verdict, 2);
      } while (got < 0 && errno == EINTR);
    } else {
      timed_out = true;
      kill(pid, SIGKILL);
    }
  }
  close(pfd[0]);
  int status = 0;
  if (pid != -1) waitpid(pid, &status, 0);

  REQUIRE_NE(pid, -1);
  REQUIRE_FALSE(timed_out);
  REQUIRE_EQ((int)got, 2);
  REQUIRE_EQ((int)verdict[0], (int)'N');
  REQUIRE_EQ((int)verdict[1], (int)'C');
}

// chttp_tls_config must be strict when zero-initialised, since every usual way
// to build it leaves the fields the caller didn't name at zero
TEST(tls_config_defaults, zero_initialised_config_verifies_everything) {
  chttp_tls_config zeroed;
  memset(&zeroed, 0, sizeof(zeroed));
  REQUIRE_FALSE(zeroed.insecure_skip_verify);
  REQUIRE_FALSE(zeroed.insecure_skip_hostname_check);

  chttp_tls_config braced = {0};
  REQUIRE_FALSE(braced.insecure_skip_verify);
  REQUIRE_FALSE(braced.insecure_skip_hostname_check);

  chttp_tls_config just_a_bundle = {.ca_bundle_path = "/etc/ssl/ca.pem"};
  REQUIRE_FALSE(just_a_bundle.insecure_skip_verify);
  REQUIRE_FALSE(just_a_bundle.insecure_skip_hostname_check);

  chttp_tls_config just_a_cert = {.cert_path = "c.pem", .key_path = "k.pem"};
  REQUIRE_FALSE(just_a_cert.insecure_skip_verify);
  REQUIRE_FALSE(just_a_cert.insecure_skip_hostname_check);
}

// Compared field by field: A compound literal leaves its padding unspecified,
// so a memcmp fails for no reason. A new field must be added here too.
TEST(tls_config_defaults, the_default_macro_matches_a_zeroed_struct) {
  chttp_tls_config from_macro = CHTTP_TLS_DEFAULT;
  chttp_tls_config zeroed;
  memset(&zeroed, 0, sizeof(zeroed));

  REQUIRE_EQ((void *)from_macro.cert_path, (void *)zeroed.cert_path);
  REQUIRE_EQ((void *)from_macro.key_path, (void *)zeroed.key_path);
  REQUIRE_EQ((void *)from_macro.ca_bundle_path, (void *)zeroed.ca_bundle_path);
  REQUIRE_EQ((int)from_macro.insecure_skip_verify,
             (int)zeroed.insecure_skip_verify);
  REQUIRE_EQ((int)from_macro.insecure_skip_hostname_check,
             (int)zeroed.insecure_skip_hostname_check);
  REQUIRE_EQ((int)from_macro.client_cert_optional,
             (int)zeroed.client_cert_optional);
}

TEST(tls_config_defaults, skip_verify_together_with_a_ca_bundle_is_refused) {
  chttpcli cli ccol__destructor(chttpcli__destroy_cleanup) =
      chttpcli_create(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_NULL);

  chttp_tls_config contradictory = {
      .ca_bundle_path = "/etc/ssl/ca.pem",
      .insecure_skip_verify = true,
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &contradictory), ccol_invalid_args);

  chttp_tls_config bundle_only = {.ca_bundle_path = "/etc/ssl/ca.pem"};
  REQUIRE_EQ(chttpcli_set_tls(cli, &bundle_only), ccol_success);

  chttp_tls_config skip_only = {.insecure_skip_verify = true};
  REQUIRE_EQ(chttpcli_set_tls(cli, &skip_only), ccol_success);

  chttp_tls_config pinned_no_hostname = {
      .ca_bundle_path = "/etc/ssl/ca.pem",
      .insecure_skip_hostname_check = true,
  };
  REQUIRE_EQ(chttpcli_set_tls(cli, &pinned_no_hostname), ccol_success);

  REQUIRE_EQ(chttpcli_set_tls(cli, NULL), ccol_success);
}

// Redirect transport rules
//
// The AF_UNIX rule is unconditional. The TLS rule is an opt-in through
// prevent_tls_downgrade_on_redirect, and by default a chain follows https to
// http, as curl and Go do. A build that always refuses the downgrade fails
// here.
TEST(redirect_transport, https_to_http_is_followed_by_default) {
  char *resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example/y", false, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://evil.example/y", false, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example:8080/y", false, &resolved));
  free(resolved);
  resolved = NULL;
}

TEST(redirect_transport, prevent_downgrade_refuses_the_downgrade) {
  char *resolved = NULL;

  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://evil.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example:8080/y", true, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "HTTP://a.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;
}

// With the field set, a reference that inherits https, an upgrade and a chain
// that was never on TLS still follow.
TEST(redirect_transport, prevent_downgrade_still_follows_everything_else) {
  char *resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "//other.example/y", true, &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "/y", true, &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/dir/x", "../y", true, &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "https://a.example/x", "https://b.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "http://a.example/x", "https://a.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
      "http://a.example/x", "http://b.example/y", true, &resolved));
  free(resolved);
  resolved = NULL;
}

TEST(redirect_transport, the_unix_rule_ignores_the_opt_in) {
  for (int i = 0; i < 2; i++) {
    bool prevent_downgrade = (i == 1);
    char *resolved = NULL;

    REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
        "https://a.example/x", "http+unix://%2Ftmp%2Fs/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;

    REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
        "http://a.example/x", "http+unix://%2Ftmp%2Fs/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;

    REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http+unix://%2Ftmp%2Fs/y",
        prevent_downgrade, &resolved));
    free(resolved);
    resolved = NULL;

    REQUIRE_FALSE(chttp__redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http+unix://%2Ftmp%2Fother/y",
        prevent_downgrade, &resolved));
    free(resolved);
    resolved = NULL;

    REQUIRE_TRUE(chttp__redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http://a.example/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;
  }
}

TEST(redirect_transport, the_request_field_defaults_to_off) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "https://a.example/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  bool defaulted_off = !req->prevent_tls_downgrade_on_redirect;
  chttp_request_free(req);
  REQUIRE_TRUE(defaulted_off);
}

// Connect fallback, redirect header scope, request-target
//
// Runs req in the tier `tier` names (1 chttpcli_do, 2 chttpcli_do_async, 3
// chttpcli_do_pooled), so one test body can check every tier. Gives the code
// and the response in *resp_out.
static ccol_retval run_in_tier(int tier, chttpcli cli, const chttp_request *req,
                               chttpcli_response **resp_out) {
  *resp_out = NULL;
  if (tier == 1) return chttpcli_do(cli, req, resp_out);
  if (tier == 3) return chttpcli_do_pooled(cli, req, resp_out);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  if (!f) return ccol_unexpected_failure;
  chttpcli_async_result *r = chttpcli_async_result_get(f);
  ccol_retval rv = ccol_unexpected_failure;
  if (r) {
    rv = r->rv;
    *resp_out = r->resp;
    chttpcli_async_result_free(r);
  }
  cthrpool_future_free(f);
  return rv;
}

// Wakes a thread blocked in accept(2) on lfd, so every mock server can stop
// its accept loop. Linux and FreeBSD wake it on shutdown(2), macOS refuses that
// on a listener and OpenBSD wakes on neither shutdown nor close, so we also
// connect to the listener. Every accept loop checks its running flag after
// accept returns, so it never serves that connection.
static void test_wake_listener(int lfd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getsockname(lfd, (struct sockaddr *)&ss, &len) != 0) return;
  if (ss.ss_family == AF_INET) {
    struct sockaddr_in *a = (struct sockaddr_in *)&ss;
    if (a->sin_addr.s_addr == htonl(INADDR_ANY))
      a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (ss.ss_family == AF_INET6) {
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
    if (IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr)) a->sin6_addr = in6addr_loopback;
  }
  int c = socket(ss.ss_family, SOCK_STREAM, 0);
  if (c >= 0) {
    (void)test_connect(c, (struct sockaddr *)&ss, len);
    close(c);
  }
  (void)shutdown(lfd, SHUT_RDWR);
}

// A scripted HTTP/1.1 listener that serves one connection at a time:
//   /redir?u=<url>   302 with "Location: <url>"
//   /raw-location    302 with g_hs_raw_location as it is
//   /http10-te       an HTTP/1.0 keep-alive response framed by chunked
//   /http11-ka       an HTTP/1.1 keep-alive response
//   /bad-chunk-ext   a chunk extension that holds a control byte
//   anything else    200 with the request header block as the body
// The other routes close after one response. redirs_with_credentials counts the
// /redir requests that carried an Authorization or a Cookie header.
typedef struct {
  int fd;
  int port;
  pthread_t tid;
  atomic_int running;
  atomic_int accepts;
  atomic_int redirs;
  atomic_int redirs_with_credentials;
} hs_srv_t;

// The Location that /raw-location sends, byte for byte.
static char g_hs_raw_location[512];

// The accept loop of the scripted listener in arg: Serves its routes one
// connection at a time and counts accepts, redirs and redirs_with_credentials,
// until running clears.
static void *hs_loop(void *arg) {
  hs_srv_t *s = (hs_srv_t *)arg;
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (!buf) return NULL;
  while (atomic_load(&s->running)) {
    int c = test_accept(s->fd);
    if (c < 0) {
      if (!atomic_load(&s->running)) break;
      continue;
    }
    if (!atomic_load(&s->running)) {
      close(c);
      break;
    }
    atomic_fetch_add(&s->accepts, 1);
    for (;;) {
      size_t hl = 0;
      ssize_t n = srv_read_headers(c, buf, TEST_SERVER_BUF, &hl);
      if (n <= 0 || hl == 0) break;
      char method[16], path[1024];
      srv_parse_request_line(buf, method, sizeof(method), path, sizeof(path));
      if (strcmp(path, "/http10-te") == 0) {
        static const char r[] =
            "HTTP/1.0 200 OK\r\n"
            "Connection: keep-alive\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\nhello\r\n0\r\n\r\n";
        send(c, r, sizeof(r) - 1, 0);
        continue;
      }
      if (strcmp(path, "/http11-ka") == 0) {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        send(c, r, sizeof(r) - 1, 0);
        continue;
      }
      if (strcmp(path, "/bad-chunk-ext") == 0) {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "5;a=\x01\r\nhello\r\n0\r\n\r\n";
        send(c, r, sizeof(r) - 1, 0);
        break;
      }
      if (strncmp(path, "/redir?u=", 9) == 0) {
        char v[128];
        atomic_fetch_add(&s->redirs, 1);
        if (srv_find_header(buf, "authorization", v, sizeof(v)) ||
            srv_find_header(buf, "cookie", v, sizeof(v)))
          atomic_fetch_add(&s->redirs_with_credentials, 1);
        char loc[1100];
        snprintf(loc, sizeof(loc), "Location: %s\r\n", path + 9);
        srv_respond(c, 302, "Found", "text/plain", loc, NULL, 0, false);
      } else if (strcmp(path, "/raw-location") == 0) {
        char loc[600];
        snprintf(loc, sizeof(loc), "Location: %s\r\n", g_hs_raw_location);
        srv_respond(c, 302, "Found", "text/plain", loc, NULL, 0, false);
      } else {
        srv_respond(c, 200, "OK", "text/plain", NULL, buf, hl, false);
      }
      break;
    }
    close(c);
  }
  free(buf);
  return NULL;
}

// Starts a scripted listener on the IPv4 address ip and fills *s. Returns false
// when the environment can't bind it.
static bool hs_start(hs_srv_t *s, const char *ip) {
  memset(s, 0, sizeof(*s));
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->fd < 0) return false;
  int opt = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 ||
      bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(s->fd, 16) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  socklen_t len = sizeof(a);
  getsockname(s->fd, (struct sockaddr *)&a, &len);
  s->port = ntohs(a.sin_port);
  atomic_store(&s->running, 1);
  if (pthread_create(&s->tid, NULL, hs_loop, s) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  return true;
}

// Stops the scripted listener. The fd is closed only after the join, because
// the accept loop reads it.
static void hs_stop(hs_srv_t *s) {
  if (s->fd < 0) return;
  atomic_store(&s->running, 0);
  test_wake_listener(s->fd);
  pthread_join(s->tid, NULL);
  close(s->fd);
  s->fd = -1;
}

// Gives a socket bound to 127.0.0.1 whose port refuses a connect while the
// socket lives, and its address in *out, for the connect fallback tests. Linux
// and FreeBSD refuse a port that is bound but not listening; OpenBSD drops that
// silently, so there the socket listens and shuts down.
static int refused_port_socket(struct sockaddr_in *out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(*out);
  if (bind(fd, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(fd, (struct sockaddr *)out, &len) != 0) {
    close(fd);
    return -1;
  }
#if defined(__OpenBSD__)
  if (listen(fd, 1) != 0 || shutdown(fd, SHUT_RDWR) != 0) {
    close(fd);
    return -1;
  }
#endif
  return fd;
}

// The first address refuses and the second serves; every tier must move on.
TEST(connect_fallback, next_address_is_tried_after_a_refusal_in_every_tier) {
  struct sockaddr_in refused;
  int rfd = refused_port_socket(&refused);
  REQUIRE_GE(rfd, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());

  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&refused,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(refused), sizeof(good)};
  chttp__set_addr_override_for_tests(addrs, lens, 2);

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://fallback.invalid/get", NULL, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  close(rfd);

  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
}

TEST(connect_fallback, every_candidate_refusing_reports_connection_failed) {
  struct sockaddr_in r1, r2;
  int f1 = refused_port_socket(&r1);
  int f2 = refused_port_socket(&r2);
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&r1,
                                     (const struct sockaddr *)&r2};
  socklen_t lens[2] = {sizeof(r1), sizeof(r2)};
  if (f1 >= 0 && f2 >= 0) chttp__set_addr_override_for_tests(addrs, lens, 2);

  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://fallback.invalid/get", NULL, NULL);
  ccol_retval rv[4] = {0};
  for (int tier = 1; tier <= 3 && req && f1 >= 0 && f2 >= 0; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  if (f1 >= 0) close(f1);
  if (f2 >= 0) close(f2);

  REQUIRE_GE(f1, 0);
  REQUIRE_GE(f2, 0);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_connection_failed);
}

// Where the resolver lists 127.0.0.1 first this passes without exercising the
// fallback; the test above holds on every machine.
TEST(connect_fallback, localhost_reaches_an_ipv4_only_listener_in_every_tier) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  bool v6_first = getaddrinfo("localhost", "80", &hints, &res) == 0 && res &&
                  res->ai_family == AF_INET6;
  if (res) freeaddrinfo(res);
  if (!v6_first)
    fprintf(stderr,
            "NOTE: localhost does not resolve to ::1 first here, so "
            "localhost_reaches_an_ipv4_only_listener_in_every_tier does not "
            "exercise the fallback\n");

  char url[160];
  snprintf(url, sizeof(url), "http://localhost:%d/get", get_test_port());
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);

  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
}

// What the final hop of a redirect chain got from the headers the test sets.
typedef struct {
  ccol_retval rv;
  int status;
  bool auth, cookie, cookie2, www_auth, keep;
  char host[128];
} hop_seen_t;

// Builds a GET with the credential headers, a custom Host and X-Keep, the
// request of the redirect header scope tests. Gives NULL on failure.
static chttp_request *new_request_with_sensitive_headers(const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  if (chttp_request_set_header(req, "Authorization", "Bearer t0k3n") !=
          ccol_success ||
      chttp_request_set_header(req, "Cookie", "session=abc") != ccol_success ||
      chttp_request_set_header(req, "Cookie2", "$Version=1") != ccol_success ||
      chttp_request_set_header(req, "WWW-Authenticate", "Basic realm=x") !=
          ccol_success ||
      chttp_request_set_header(req, "Host", "custom.example") != ccol_success ||
      chttp_request_set_header(req, "X-Keep", "kept") != ccol_success) {
    chttp_request_free(req);
    return NULL;
  }
  return req;
}

// Runs the sensitive request for url in every tier and records in seen[tier]
// what the final hop of the chain got.
static void run_sensitive_redirect(chttpcli cli, const char *url,
                                   hop_seen_t seen[4]) {
  memset(seen, 0, 4 * sizeof(hop_seen_t));
  chttp_request *req = new_request_with_sensitive_headers(url);
  for (int tier = 1; tier <= 3; tier++) {
    seen[tier].rv = ccol_unexpected_failure;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    seen[tier].rv = run_in_tier(tier, cli, req, &resp);
    if (resp && resp->body) {
      char v[128];
      seen[tier].status = resp->status_code;
      seen[tier].auth = srv_find_header(resp->body, "authorization", v, 128);
      seen[tier].cookie = srv_find_header(resp->body, "cookie", v, 128);
      seen[tier].cookie2 = srv_find_header(resp->body, "cookie2", v, 128);
      seen[tier].www_auth =
          srv_find_header(resp->body, "www-authenticate", v, 128);
      seen[tier].keep = srv_find_header(resp->body, "x-keep", v, 128);
      srv_find_header(resp->body, "host", seen[tier].host,
                      sizeof(seen[tier].host));
    }
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
}

TEST(redirect_header_scope, cross_host_drops_credentials_and_host) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[256], expect_host[64];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/final", a.port,
           b.port);
  snprintf(expect_host, sizeof(expect_host), "127.0.0.2:%d", b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  hs_stop(&a);
  hs_stop(&b);

  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, expect_host);
  }
}

TEST(redirect_header_scope, cross_port_drops_credentials_keeps_host) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.1:%d/final", a.port,
           b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  if (ok_a && ok_b) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (ok_a) hs_stop(&a);
  if (ok_b) hs_stop(&b);

  REQUIRE_TRUE(ok_a && ok_b);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

TEST(redirect_header_scope, same_origin_keeps_every_header) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.1:%d/final", a.port,
           a.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  if (ok_a) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_TRUE(seen[tier].auth);
    REQUIRE_TRUE(seen[tier].cookie);
    REQUIRE_TRUE(seen[tier].cookie2);
    REQUIRE_TRUE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

// Every hop is judged against the first request's origin (curl's rule), so
// A -> B -> A sends the headers again on the hop back to A.
TEST(redirect_header_scope, return_to_first_origin_sends_headers_again) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[512];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/redir?u="
           "http://127.0.0.1:%d/final",
           a.port, b.port, a.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  int b_redirs = atomic_load(&b.redirs);
  int b_with_credentials = atomic_load(&b.redirs_with_credentials);
  hs_stop(&a);
  hs_stop(&b);

  REQUIRE_EQ(b_redirs, 3);
  REQUIRE_EQ(b_with_credentials, 0);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_TRUE(seen[tier].auth);
    REQUIRE_TRUE(seen[tier].cookie);
    REQUIRE_TRUE(seen[tier].cookie2);
    REQUIRE_TRUE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

// A -> B -> B: The last hop matches the hop before it but not the first
// request, so the headers stay off.
TEST(redirect_header_scope, second_hop_on_foreign_origin_still_drops) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[512], expect_host[64];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/redir?u="
           "http://127.0.0.2:%d/final",
           a.port, b.port, b.port);
  snprintf(expect_host, sizeof(expect_host), "127.0.0.2:%d", b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  hs_stop(&a);
  hs_stop(&b);

  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, expect_host);
  }
}

TEST(redirect_header_scope, cross_scheme_drops_credentials) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=https://127.0.0.1:%d/echo-headers",
           a.port, g_tls_srv_port);
  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval trv = chttpcli_set_tls(cli, &tls);
  hop_seen_t seen[4];
  if (ok_a && trv == ccol_success) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_EQ(trv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

// Parses url and gives its request-target in *pq, so the request_target tests
// can check the encoding without a server.
static ccol_retval parsed_target(const char *url, char **pq) {
  *pq = NULL;
  return chttp__parse_url_for_tests(url, NULL, NULL, NULL, NULL, pq, NULL, NULL,
                                    NULL, NULL);
}

TEST(request_target, bytes_outside_rfc3986_are_percent_encoded) {
  static const struct {
    const char *url;
    const char *target;
  } cases[] = {
      {"http://h/my file", "/my%20file"},
      {"http://h/a\"b<c>d\\e^f`g{h|i}j",
       "/a%22b%3Cc%3Ed%5Ce%5Ef%60g%7Bh%7Ci%7Dj"},
      {"http://h/[x]", "/%5Bx%5D"},
      {"http://h/t\x01u\x7fv", "/t%01u%7Fv"},
      {"http://h/caf\xc3\xa9", "/caf%C3%A9"},
      {"http://h/s?q=a b&x=<1>", "/s?q=a%20b&x=%3C1%3E"},
      {"http://h?x y", "/?x%20y"},
      {"http://h/ok-._~!$&'()*+,;=:@/?/?", "/ok-._~!$&'()*+,;=:@/?/?"},
      {"http+unix://%2Ftmp%2Fs/p q", "/p%20q"},
  };
  size_t n = sizeof(cases) / sizeof(cases[0]);
  size_t matched = 0;
  for (size_t i = 0; i < n; i++) {
    char *pq = NULL;
    ccol_retval rv = parsed_target(cases[i].url, &pq);
    if (rv == ccol_success && pq && strcmp(pq, cases[i].target) == 0)
      matched++;
    else
      fprintf(stderr, "request_target mismatch for case %zu: got '%s'\n", i,
              pq ? pq : "(null)");
    free(pq);
  }
  REQUIRE_EQ(matched, n);
}

TEST(request_target, existing_escapes_are_not_encoded_twice) {
  char *pq = NULL;
  REQUIRE_EQ(parsed_target("http://h/a%20b%2Fc?d=%7e%41", &pq), ccol_success);
  bool same = pq && strcmp(pq, "/a%20b%2Fc?d=%7e%41") == 0;
  free(pq);
  REQUIRE_TRUE(same);
}

TEST(request_target, stray_percent_is_invalid) {
  static const char *bad[] = {"http://h/100%", "http://h/a%zz", "http://h/a%2",
                              "http://h/?q=%"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *pq = NULL;
    ccol_retval rv = parsed_target(bad[i], &pq);
    free(pq);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
  }
}

// Runs url in every tier and records the code and whether the request line
// that reached the server equals expect_line, for the request_target tests.
static void run_target_check(const char *url, const char *expect_line,
                             ccol_retval rv_out[4], bool line_ok[4]) {
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  for (int tier = 1; tier <= 3; tier++) {
    rv_out[tier] = ccol_unexpected_failure;
    line_ok[tier] = false;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    rv_out[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp && resp->body)
      line_ok[tier] =
          strncmp(resp->body, expect_line, strlen(expect_line)) == 0;
    if (!line_ok[tier] && resp && resp->body)
      fprintf(stderr, "request line seen: %.80s\n", resp->body);
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
}

TEST(request_target, caller_url_is_encoded_on_the_wire_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/my file?x=a b", a.port);
  ccol_retval rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /my%20file?x=a%20b HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

TEST(request_target, location_is_encoded_on_the_wire_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  snprintf(g_hs_raw_location, sizeof(g_hs_raw_location), "/my file HTTP/1.0");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/raw-location", a.port);
  ccol_retval rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /my%20file%20HTTP/1.0 HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

TEST(request_target, non_ascii_location_is_encoded_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  snprintf(g_hs_raw_location, sizeof(g_hs_raw_location),
           "/caf\xc3\xa9?q=\xe2\x82\xac%%41");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/raw-location", a.port);
  ccol_retval rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /caf%C3%A9?q=%E2%82%AC%41 HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

// The response is one TLS record and the server keeps the connection open, so
// after the first read part of it waits in the TLS layer while the socket has
// nothing more. A client that waits on the socket only gets the rest when the
// server's hold expires, which the server counts.
TEST(tls_buffered_input, response_left_inside_the_tls_layer_is_read) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  int closed_before = atomic_load(&g_one_record_closed_by_client);
  int expired_before = atomic_load(&g_one_record_hold_expired);
  chttpcli_construct(cli);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval trv = chttpcli_set_tls(cli, &tls);
  chttp__set_async_tls_first_read_cap_for_tests(1024);

  char url[160];
  make_tls_url(url, sizeof(url), "/one-record-held-open");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv[4] = {0};
  size_t body_len[4] = {0};
  for (int tier = 1; tier <= 3 && req && trv == ccol_success; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    body_len[tier] = resp ? resp->body_len : 0;
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  chttp__set_async_tls_first_read_cap_for_tests(0);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  for (int i = 0; i < 2 * ONE_RECORD_HOLD_MS / 10; i++) {
    if (atomic_load(&g_one_record_closed_by_client) +
            atomic_load(&g_one_record_hold_expired) - closed_before -
            expired_before >=
        3)
      break;
    usleep(10000);
  }
  int closed = atomic_load(&g_one_record_closed_by_client) - closed_before;
  int expired = atomic_load(&g_one_record_hold_expired) - expired_before;

  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_GT(body_len[tier], (size_t)8192);
  }
  REQUIRE_EQ(expired, 0);
  REQUIRE_EQ(closed, 3);
}

TEST(tls_client_create, missing_name_with_verification_is_invalid_url) {
  ccol_retval empty = chttp__tls_client_create_code_for_tests("", true);
  ccol_retval null_host = chttp__tls_client_create_code_for_tests(NULL, true);
  ccol_retval skipped = chttp__tls_client_create_code_for_tests("", false);
  ccol_retval named =
      chttp__tls_client_create_code_for_tests("example.com", true);
  ccol_retval ip = chttp__tls_client_create_code_for_tests("127.0.0.1", true);
  REQUIRE_EQ(empty, ccol_http_invalid_url);
  REQUIRE_EQ(null_host, ccol_http_invalid_url);
  REQUIRE_EQ(skipped, ccol_success);
  REQUIRE_EQ(named, ccol_success);
  REQUIRE_EQ(ip, ccol_success);
}

// Runs the same request twice in tier against srv and gives how many
// connections the server accepted for the pair, so a test can see whether the
// second request reused the connection.
static int accepts_for_two_requests(int tier, hs_srv_t *srv, const char *path,
                                    ccol_retval rv[2], size_t body_len[2]) {
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", srv->port, path);
  int before = atomic_load(&srv->accepts);
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  for (int i = 0; i < 2; i++) {
    rv[i] = ccol_unexpected_failure;
    body_len[i] = 0;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    rv[i] = run_in_tier(tier, cli, req, &resp);
    body_len[i] = resp ? resp->body_len : 0;
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  return atomic_load(&srv->accepts) - before;
}

// RFC 9112 section 6.1: An HTTP/1.0 message with Transfer-Encoding has faulty
// framing, so the connection is never reused. The HTTP/1.1 control shows the
// same measurement sees a reuse.
TEST(response_framing, http10_transfer_encoding_is_never_kept_alive) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  int te_accepts[4] = {0}, ka_accepts[4] = {0};
  ccol_retval te_rv[4][2], ka_rv[4][2];
  size_t te_len[4][2], ka_len[4][2];
  for (int tier = 1; tier <= 3 && ok_a; tier++) {
    te_accepts[tier] = accepts_for_two_requests(tier, &a, "/http10-te",
                                                te_rv[tier], te_len[tier]);
    ka_accepts[tier] = accepts_for_two_requests(tier, &a, "/http11-ka",
                                                ka_rv[tier], ka_len[tier]);
  }
  if (ok_a) hs_stop(&a);

  for (int tier = 1; tier <= 3 && ok_a; tier++) {
    for (int i = 0; i < 2; i++) {
      if (te_rv[tier][i] != ccol_success || te_len[tier][i] != 5)
        fprintf(stderr, "DIAG /http10-te tier %d request %d: rv %d len %zu\n",
                tier, i, (int)te_rv[tier][i], te_len[tier][i]);
      if (ka_rv[tier][i] != ccol_success || ka_len[tier][i] != 5)
        fprintf(stderr, "DIAG /http11-ka tier %d request %d: rv %d len %zu\n",
                tier, i, (int)ka_rv[tier][i], ka_len[tier][i]);
    }
    if (te_accepts[tier] != 2 || ka_accepts[tier] != 1)
      fprintf(stderr, "DIAG tier %d accepts: /http10-te %d, /http11-ka %d\n",
              tier, te_accepts[tier], ka_accepts[tier]);
  }

  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    for (int i = 0; i < 2; i++) {
      REQUIRE_EQ(te_rv[tier][i], ccol_success);
      REQUIRE_EQ(te_len[tier][i], (size_t)5);
      REQUIRE_EQ(ka_rv[tier][i], ccol_success);
      REQUIRE_EQ(ka_len[tier][i], (size_t)5);
    }
    REQUIRE_EQ(te_accepts[tier], 2);
    REQUIRE_EQ(ka_accepts[tier], 1);
  }
}

TEST(response_framing, malformed_chunk_extension_is_refused) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bad-chunk-ext", a.port);
  ccol_retval rv[4] = {0};
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  for (int tier = 1; tier <= 3 && ok_a && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_transfer_aborted);
}

// The whole response is in the socket and the deadline has passed when the
// client reads it; every tier must check the deadline before reading.
TEST(read_deadline, every_tier_times_out_on_an_already_buffered_response) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/final", a.port);
  chttpcli_construct(cli);
  ccol_retval torv = chttpcli_set_request_timeout(cli, 60000000);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv[4] = {0};
  bool got_resp[4] = {0};
  chttp__set_expire_at_read_for_tests(true);
  for (int tier = 1; tier <= 3 && ok_a && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    got_resp[tier] = (resp != NULL);
    chttpcli_resp_free(resp);
  }
  chttp__set_expire_at_read_for_tests(false);
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_TRUE(have_req);
  REQUIRE_EQ(torv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], rv[1]);
    REQUIRE_EQ(rv[tier], ccol_timed_out);
    REQUIRE_FALSE(got_resp[tier]);
  }
}

// Responses and results outlive their client
//
// An allocator that keeps every freed block until quarantine_drain and fills it
// with pointers to quarantine_trap_free, so a call through procs that lived in
// a freed block is counted instead of crashing. The header is padded to
// max_align_t so blocks keep malloc's alignment.
typedef union quarantine_hdr {
  struct {
    size_t size;
    union quarantine_hdr *next;
  } h;
  max_align_t align;
} quarantine_hdr;

// The freed blocks, under g_quarantine_lock, and the count of trapped calls.
static pthread_mutex_t g_quarantine_lock = PTHREAD_MUTEX_INITIALIZER;
static quarantine_hdr *g_quarantine_list = NULL;
static atomic_uint g_quarantine_trap_calls;

static void quarantine_free(void *p);

// Counts a call through a freed allocator in g_quarantine_trap_calls and frees
// p normally, so the lifetime tests see such a call as a count.
static void quarantine_trap_free(void *p) {
  atomic_fetch_add(&g_quarantine_trap_calls, 1u);
  quarantine_free(p);
}

// Allocates n bytes behind a header that records the size, which
// quarantine_free needs to fill the block.
static void *quarantine_malloc(size_t n) {
  quarantine_hdr *h = (quarantine_hdr *)malloc(sizeof(*h) + n);
  if (!h) return NULL;
  h->h.size = n;
  h->h.next = NULL;
  return h + 1;
}

// Allocates c * n zeroed bytes through quarantine_malloc, refusing an
// overflow.
static void *quarantine_calloc(size_t c, size_t n) {
  if (n && c > SIZE_MAX / n) return NULL;
  void *p = quarantine_malloc(c * n);
  if (p) memset(p, 0, c * n);
  return p;
}

// Fills the block with trap pointers and keeps it on g_quarantine_list under
// g_quarantine_lock instead of freeing it.
static void quarantine_free(void *p) {
  if (!p) return;
  quarantine_hdr *h = (quarantine_hdr *)p - 1;
  void (*trap)(void *) = quarantine_trap_free;
  for (size_t off = 0; off + sizeof(trap) <= h->h.size; off += sizeof(trap))
    memcpy((char *)p + off, &trap, sizeof(trap));
  pthread_mutex_lock(&g_quarantine_lock);
  h->h.next = g_quarantine_list;
  g_quarantine_list = h;
  pthread_mutex_unlock(&g_quarantine_lock);
}

// Moves the block to a new allocation and quarantines the old one.
static void *quarantine_realloc(void *p, size_t n) {
  if (!p) return quarantine_malloc(n);
  quarantine_hdr *h = (quarantine_hdr *)p - 1;
  void *q = quarantine_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, h->h.size < n ? h->h.size : n);
  quarantine_free(p);
  return q;
}

// Frees every quarantined block, at the end of a lifetime test.
static void quarantine_drain(void) {
  pthread_mutex_lock(&g_quarantine_lock);
  quarantine_hdr *h = g_quarantine_list;
  g_quarantine_list = NULL;
  pthread_mutex_unlock(&g_quarantine_lock);
  while (h) {
    quarantine_hdr *next = h->h.next;
    free(h);
    h = next;
  }
}

// The quarantine allocator as a procs table, for chttpcli_create_mp.
static ccol_memmgmt_procs g_quarantine_mp = {.malloc = quarantine_malloc,
                                             .free = quarantine_free,
                                             .calloc = quarantine_calloc,
                                             .realloc = quarantine_realloc};

TEST(lifetime, a_response_freed_after_its_client_uses_no_freed_allocator) {
  atomic_store(&g_quarantine_trap_calls, 0u);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttpcli cli = chttpcli_create_mp(&g_quarantine_mp, NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = ccol_unexpected_failure;
  if (cli != CHTTPCLI_NULL && req) rv = chttpcli_do(cli, req, &resp);
  chttp_request_free(req);
  int status = resp ? resp->status_code : -1;
  chttpcli_destroy(cli);
  chttpcli_resp_free(resp);
  unsigned traps = atomic_load(&g_quarantine_trap_calls);
  quarantine_drain();
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(traps, 0u);
}

TEST(lifetime, an_async_result_freed_after_its_client_uses_no_freed_allocator) {
  atomic_store(&g_quarantine_trap_calls, 0u);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttpcli cli = chttpcli_create_mp(&g_quarantine_mp, NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  cthrpool_future *f = NULL;
  if (cli != CHTTPCLI_NULL && req) f = chttpcli_do_async(cli, req);
  chttp_request_free(req);
  chttpcli_async_result *res = f ? chttpcli_async_result_get(f) : NULL;
  ccol_retval rv = res ? res->rv : ccol_unexpected_failure;
  int status = (res && res->resp) ? res->resp->status_code : -1;
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  if (res) chttpcli_resp_free(res->resp);
  chttpcli_async_result_free(res);
  cthrpool_future_free(f);
  unsigned traps = atomic_load(&g_quarantine_trap_calls);
  quarantine_drain();
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(traps, 0u);
}

// A socket error in the middle of a response
TEST(mid_transfer_error, every_tier_reports_a_reset_as_an_aborted_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/partial-then-reset");
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv[4] = {ccol_success, ccol_success, ccol_success, ccol_success};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_transfer_aborted);
}

// A 3xx with an empty Location is the final response
TEST(redirect_policy, an_empty_location_is_delivered_in_every_tier) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-empty-location");
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : -1;
    chttpcli_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 301);
  }
}

// An origin compares its host without regard to case
//
// Tells whether the resolver answers for "LOCALHOST", so the host case tests
// skip on a system where it doesn't.
static bool upper_localhost_resolves(void) {
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
  struct addrinfo *res = NULL;
  bool ok = getaddrinfo("LOCALHOST", "80", &hints, &res) == 0 && res;
  if (res) freeaddrinfo(res);
  return ok;
}

TEST(credentials,
     a_redirect_that_changes_only_the_case_of_the_host_keeps_them) {
  if (!upper_localhost_resolves()) {
    fprintf(stderr,
            "[SKIP] a_redirect_that_changes_only_the_case_of_the_host_keeps_"
            "them: LOCALHOST does not resolve here\n");
    return;
  }
  char userinfo_url[192];
  snprintf(userinfo_url, sizeof(userinfo_url),
           "http://alice:s3cr3t@localhost:%d"
           "/redirect-to-echo-auth-upper-localhost",
           get_test_port());
  char plain_url[192];
  snprintf(plain_url, sizeof(plain_url),
           "http://localhost:%d/redirect-to-echo-auth-upper-localhost",
           get_test_port());
  chttpcli_construct(cli);
  chttp_request *ureq =
      chttp_request_create(CHTTP_GET, userinfo_url, NULL, NULL);
  chttp_request *hreq = chttp_request_create(CHTTP_GET, plain_url, NULL, NULL);
  ccol_retval hrv =
      hreq ? chttp_request_set_header(hreq, "Authorization", "Bearer mytoken")
           : ccol_unexpected_failure;
  char ubody[4][64] = {{0}};
  char hbody[4][64] = {{0}};
  ccol_retval urv[4] = {0}, hrvs[4] = {0};
  for (int tier = 1; tier <= 3 && ureq && hreq; tier++) {
    chttpcli_response *resp = NULL;
    urv[tier] = run_in_tier(tier, cli, ureq, &resp);
    if (resp && resp->body)
      snprintf(ubody[tier], sizeof(ubody[tier]), "%s", resp->body);
    chttpcli_resp_free(resp);
    resp = NULL;
    hrvs[tier] = run_in_tier(tier, cli, hreq, &resp);
    if (resp && resp->body)
      snprintf(hbody[tier], sizeof(hbody[tier]), "%s", resp->body);
    chttpcli_resp_free(resp);
  }
  bool have_reqs = ureq && hreq;
  chttp_request_free(ureq);
  chttp_request_free(hreq);
  wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  REQUIRE_TRUE(have_reqs);
  REQUIRE_EQ(hrv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(urv[tier], ccol_success);
    REQUIRE_STREQ(ubody[tier], "Basic YWxpY2U6czNjcjN0");
    REQUIRE_EQ(hrvs[tier], ccol_success);
    REQUIRE_STREQ(hbody[tier], "Bearer mytoken");
  }
}

// A deadline far away
TEST(deadline, a_deadline_whose_milliseconds_overflow_a_long_saturates) {
  int ms = 0;
  bool live = chttp__deadline_remaining_ms_for_tests(
      (long long)(LONG_MAX / 1000) + 1, &ms);
  REQUIRE_TRUE(live);
  REQUIRE_EQ(ms, INT_MAX);

  ms = 0;
  live = chttp__deadline_remaining_ms_for_tests(-5, &ms);
  REQUIRE_FALSE(live);

  ms = 0;
  live = chttp__deadline_remaining_ms_for_tests(10, &ms);
  REQUIRE_TRUE(live);
  REQUIRE_GT(ms, 9000);
  REQUIRE_LE(ms, 10000);
}

// Repeated response fields
//
// Runs one GET for url in Tier 1 (tier 0), Tier 2 (1) or Tier 3 (2) on its own
// client, for the repeated field tests. Gives the code and the response in
// *resp_out.
static ccol_retval repeated_fields_get(int tier, const char *url,
                                       chttpcli_response **resp_out) {
  *resp_out = NULL;
  char *err = NULL;
  chttpcli cli = chttpcli_create(&err);
  if (cli == CHTTPCLI_NULL) return ccol_unexpected_failure;
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpcli_destroy(cli);
    return ccol_not_enough_memory;
  }
  ccol_retval rv = ccol_unexpected_failure;
  if (tier == 0) {
    rv = chttpcli_do(cli, req, resp_out);
  } else if (tier == 1) {
    cthrpool_future *f = chttpcli_do_async(cli, req);
    if (f) {
      chttpcli_async_result *res = chttpcli_async_result_get(f);
      if (res) {
        rv = res->rv;
        *resp_out = res->resp;
        res->resp = NULL;
        chttpcli_async_result_free(res);
      }
      cthrpool_future_free(f);
    }
  } else {
    rv = chttpcli_do_pooled(cli, req, resp_out);
  }
  chttp_request_free(req);
  if (tier != 0) wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  return rv;
}

// Checks the repeated fields of /repeated-fields in one tier and adds the
// failed checks to *failures. It runs after chttpcli_destroy, so every value
// must still be readable once the client is gone.
static void repeated_fields_check(int tier, int *failures) {
  char url[160];
  make_url(url, sizeof(url), "/repeated-fields");
  chttpcli_response *resp = NULL;
  ccol_retval rv = repeated_fields_get(tier, url, &resp);
#define RF_EXPECT(cond)                                            \
  do {                                                             \
    if (!(cond)) {                                                 \
      fprintf(stderr, "tier %d: check failed: %s\n", tier, #cond); \
      (*failures)++;                                               \
    }                                                              \
  } while (0)
#define RF_STREQ(a, b) RF_EXPECT((a) != NULL && strcmp((a), (b)) == 0)
  RF_EXPECT(rv == ccol_success);
  RF_EXPECT(resp != NULL);
  if (rv == ccol_success && resp) {
    RF_STREQ(chttpcli_resp_header(resp, "X-Multi"), "one, two, three");
    RF_STREQ(chttpcli_resp_header(resp, "x-single"), "s");
    RF_STREQ(chttpcli_resp_header(resp, "X-Empty-Twice"), ", ");
    RF_STREQ(chttpcli_resp_header(resp, "Set-Cookie"), "a=1; Path=/");

    RF_EXPECT(chttpcli_resp_header_count(resp, "x-multi") == 3);
    RF_STREQ(chttpcli_resp_header_at(resp, "X-MULTI", 0), "one");
    RF_STREQ(chttpcli_resp_header_at(resp, "X-MULTI", 1), "two");
    RF_STREQ(chttpcli_resp_header_at(resp, "X-MULTI", 2), "three");
    RF_EXPECT(chttpcli_resp_header_at(resp, "X-MULTI", 3) == NULL);

    RF_EXPECT(chttpcli_resp_header_count(resp, "set-cookie") == 2);
    RF_STREQ(chttpcli_resp_header_at(resp, "Set-Cookie", 0), "a=1; Path=/");
    RF_STREQ(chttpcli_resp_header_at(resp, "Set-Cookie", 1),
             "b=2; Expires=Wed, 21 Oct 2037 07:28:00 GMT");
    RF_EXPECT(chttpcli_resp_header_at(resp, "Set-Cookie", 2) == NULL);

    RF_EXPECT(chttpcli_resp_header_count(resp, "X-Single") == 1);
    RF_STREQ(chttpcli_resp_header_at(resp, "X-Single", 0), "s");
    RF_EXPECT(chttpcli_resp_header_at(resp, "X-Single", 1) == NULL);

    RF_EXPECT(chttpcli_resp_header_count(resp, "x-empty-twice") == 2);
    RF_STREQ(chttpcli_resp_header_at(resp, "x-empty-twice", 1), "");

    RF_EXPECT(chttpcli_resp_header_count(resp, "X-Absent") == 0);
    RF_EXPECT(chttpcli_resp_header_at(resp, "X-Absent", 0) == NULL);

    const char *direct = chmap_get(resp->headers, "x-multi");
    RF_STREQ(direct, "one, two, three");
  }
#undef RF_STREQ
#undef RF_EXPECT
  chttpcli_resp_free(resp);
}

TEST(repeated_fields, combined_in_order_except_set_cookie_on_every_tier) {
  int failures = 0;
  for (int tier = 0; tier < 3; tier++) repeated_fields_check(tier, &failures);
  REQUIRE_EQ(failures, 0);
}

TEST(repeated_fields, null_arguments_and_streaming_results) {
  REQUIRE_EQ(chttpcli_resp_header_count(NULL, "x"), (size_t)0);
  REQUIRE_EQ((void *)chttpcli_resp_header_at(NULL, "x", 0), NULL);

  char url[160];
  make_url(url, sizeof(url), "/repeated-fields");
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  size_t n = chttpcli_resp_header_count(resp, NULL);
  const char *v = chttpcli_resp_header_at(resp, NULL, 0);
  char long_name[300];
  memset(long_name, 'x', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';
  size_t n_long = chttpcli_resp_header_count(resp, long_name);
  const char *v_long = chttpcli_resp_header_at(resp, long_name, 0);
  chttpcli_resp_free(resp);
  REQUIRE_EQ(n, (size_t)0);
  REQUIRE_EQ((void *)v, NULL);
  REQUIRE_EQ(n_long, (size_t)0);
  REQUIRE_EQ((void *)v_long, NULL);
}

// Streams url in Tier 1, 2 or 3 on its own client, for the long header line
// test. Gives the status and the number of body bytes.
static ccol_retval long_header_stream(int tier, const char *url,
                                      int *status_out, size_t *len_out) {
  *status_out = 0;
  *len_out = 0;
  chttpcli cli = chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL) return ccol_unexpected_failure;
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpcli_destroy(cli);
    return ccol_not_enough_memory;
  }
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval rv = ccol_unexpected_failure;
  if (tier == 0) {
    rv = chttpcli_do_streaming(cli, req, stream_sink_write, &sink, status_out);
  } else if (tier == 1) {
    cthrpool_future *f =
        chttpcli_do_async_streaming(cli, req, stream_sink_write, &sink);
    if (f) {
      chttpcli_async_result *res = chttpcli_async_result_get(f);
      if (res) {
        rv = res->rv;
        if (res->resp) *status_out = res->resp->status_code;
        chttpcli_resp_free(res->resp);
        chttpcli_async_result_free(res);
      }
      cthrpool_future_free(f);
    }
  } else {
    rv = chttpcli_do_pooled_streaming(cli, req, stream_sink_write, &sink,
                                      status_out);
  }
  chttp_request_free(req);
  if (tier != 0) wait_for_async_engine_idle();
  chttpcli_destroy(cli);
  *len_out = sink.len;
  return rv;
}

TEST(repeated_fields, a_header_line_longer_than_the_fixed_parser_buffer) {
  static const char *const ok_paths[] = {"/long-header-line/40000",
                                         "/long-header-hint/40000",
                                         "/long-header-redir/40000"};
  enum { NPATHS = 3 };
  ccol_retval ok_rv[NPATHS][3], long_rv[3], stream_rv[3];
  size_t ok_len[NPATHS][3];
  bool tail_ok[NPATHS][3];
  int stream_status[3];
  size_t stream_len[3];
  char url[160];
  for (int p = 0; p < NPATHS; p++)
    for (int tier = 0; tier < 3; tier++) {
      make_url(url, sizeof(url), ok_paths[p]);
      chttpcli_response *resp = NULL;
      ok_len[p][tier] = 0;
      tail_ok[p][tier] = false;
      ok_rv[p][tier] = repeated_fields_get(tier, url, &resp);
      if (ok_rv[p][tier] == ccol_success && resp) {
        const char *v0 = chttpcli_resp_header_at(resp, "x-long", 0);
        const char *v1 = chttpcli_resp_header_at(resp, "x-long", 1);
        const char *all = chttpcli_resp_header(resp, "X-Long");
        ok_len[p][tier] = v0 ? strlen(v0) : 0;
        tail_ok[p][tier] =
            v1 && strcmp(v1, "tail") == 0 && all && strlen(all) == 40000 + 6;
      }
      chttpcli_resp_free(resp);
    }
  for (int tier = 0; tier < 3; tier++) {
    make_url(url, sizeof(url), "/long-header-line/70000");
    chttpcli_response *resp = NULL;
    long_rv[tier] = repeated_fields_get(tier, url, &resp);
    chttpcli_resp_free(resp);
    make_url(url, sizeof(url), "/long-header-line/40000");
    stream_rv[tier] =
        long_header_stream(tier, url, &stream_status[tier], &stream_len[tier]);
  }
  for (int tier = 0; tier < 3; tier++) {
    for (int p = 0; p < NPATHS; p++) {
      REQUIRE_EQ(ok_rv[p][tier], ccol_success);
      REQUIRE_EQ(ok_len[p][tier], (size_t)40000);
      REQUIRE_TRUE(tail_ok[p][tier]);
    }
    REQUIRE_NE(long_rv[tier], ccol_success);
    REQUIRE_EQ(stream_rv[tier], ccol_success);
    REQUIRE_EQ(stream_status[tier], 200);
    REQUIRE_EQ(stream_len[tier], (size_t)2);
  }
}

TEST(repeated_fields, work_is_linear_in_the_number_of_occurrences) {
  char url[160];
  make_url(url, sizeof(url), "/many-repeated-fields");
  chttpcli_construct(cli);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  unsigned long before = chttpcli__field_rec_reads_for_tests();
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  int wrong = 0;
  if (rv == ccol_success && resp) {
    for (int i = 0; i < 44; i++) {
      char name[16], want[32], a[8], b[8];
      snprintf(name, sizeof(name), "x-r%02d", i);
      snprintf(a, sizeof(a), "a%d", i);
      snprintf(b, sizeof(b), "b%d", i);
      snprintf(want, sizeof(want), "%s, %s", a, b);
      const char *v = chttpcli_resp_header(resp, name);
      const char *v0 = chttpcli_resp_header_at(resp, name, 0);
      const char *v1 = chttpcli_resp_header_at(resp, name, 1);
      if (!v || strcmp(v, want) != 0) wrong++;
      if (!v0 || strcmp(v0, a) != 0) wrong++;
      if (!v1 || strcmp(v1, b) != 0) wrong++;
      if (chttpcli_resp_header_count(resp, name) != 2) wrong++;
    }
    const char *e = chttpcli_resp_header(resp, "X-E");
    if (!e || strcmp(e, ", z") != 0) wrong++;
    const char *e0 = chttpcli_resp_header_at(resp, "x-e", 0);
    if (!e0 || strcmp(e0, "") != 0) wrong++;
  }
  unsigned long reads = chttpcli__field_rec_reads_for_tests() - before;
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_LT(reads, 90ul * 12ul);
}

TEST(repeated_fields, a_redirect_with_two_locations_follows_the_first) {
  int status[3] = {0, 0, 0};
  ccol_retval rvs[3];
  char url[160];
  make_url(url, sizeof(url), "/redirect-two-locations");
  for (int tier = 0; tier < 3; tier++) {
    chttpcli_response *resp = NULL;
    rvs[tier] = repeated_fields_get(tier, url, &resp);
    status[tier] = resp ? resp->status_code : -1;
    chttpcli_resp_free(resp);
  }
  chttpcli cli = chttpcli_create(NULL);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  int streamed_status = 0;
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval srv = req ? chttpcli_do_streaming(cli, req, stream_sink_write,
                                                &sink, &streamed_status)
                        : ccol_not_enough_memory;
  chttp_request_free(req);
  chttpcli_destroy(cli);
  for (int tier = 0; tier < 3; tier++) {
    REQUIRE_EQ(rvs[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
  REQUIRE_EQ(srv, ccol_success);
  REQUIRE_EQ(streamed_status, 200);
}

// Engine logger across engine restarts
//
// Counts the lines of the file at path that contain needle, or gives -1, so the
// engine logger tests can see what the engine logged.
static int count_lines_containing(const char *path, const char *needle) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[4096];
  int n = 0;
  while (fgets(line, sizeof(line), f))
    if (strstr(line, needle)) n++;
  fclose(f);
  return n;
}

// Writes the name of an engine logger test's log file into buf and gives buf.
// The file sits in the working directory, and the pid keeps two runs of this
// binary in one directory apart.
static const char *engine_log_path(char *buf, size_t size, const char *tag) {
  snprintf(buf, size, "chttpcli_engine_logger_%s.%ld.log", tag, (long)getpid());
  return buf;
}

TEST(engine_logger, the_installed_logger_survives_every_engine_restart) {
  char path_buf[96];
  const char *path = engine_log_path(path_buf, sizeof(path_buf), "restart");
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  REQUIRE_GE(fd, 0);
  wait_for_async_engine_idle();

  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  ccol_retval set_rv = lg ? chttpcli_set_engine_logger(lg) : ccol_invalid_args;
  if (lg) clog_close(lg);

  char url[128];
  make_url(url, sizeof(url), "/get");
  enum { CYCLES = 3 };
  int ok = 0;
  for (int i = 0; i < CYCLES && set_rv == ccol_success; i++) {
    chttpcli cli = chttpcli_create(NULL);
    chttp_request *req =
        cli ? chttp_request_create(CHTTP_GET, url, NULL, NULL) : NULL;
    cthrpool_future *f = req ? chttpcli_do_async(cli, req) : NULL;
    chttpcli_async_result *res = f ? chttpcli_async_result_get(f) : NULL;
    if (res && res->rv == ccol_success && res->resp &&
        res->resp->status_code == 200)
      ok++;
    if (res) {
      chttpcli_resp_free(res->resp);
      chttpcli_async_result_free(res);
    }
    if (f) cthrpool_future_free(f);
    chttp_request_free(req);
    if (cli) chttpcli_destroy(cli);
    wait_for_async_engine_idle();
  }

  clog quiet = clog_create_from_fd(2, CLOG_FATAL, NULL);
  ccol_retval quiet_rv =
      quiet ? chttpcli_set_engine_logger(quiet) : ccol_invalid_args;
  if (quiet) clog_close(quiet);
  close(fd);

  int created = count_lines_containing(
      path, "New http client reactor engine has been created");
  int destroyed = count_lines_containing(
      path, "The http client reactor engine has been destroyed");
  unlink(path);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(quiet_rv, ccol_success);
  REQUIRE_EQ(ok, CYCLES);
  REQUIRE_EQ(created, CYCLES);
  REQUIRE_EQ(destroyed, CYCLES);
}

// A setter that derives from a closed handle without checking it ends the
// process with a fatal error; it must return ccol_invalid_args and keep the
// installed logger.
TEST(engine_logger, a_closed_logger_is_refused) {
  char path_buf[96];
  const char *path = engine_log_path(path_buf, sizeof(path_buf), "closed");
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  REQUIRE_GE(fd, 0);
  wait_for_async_engine_idle();

  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  ccol_retval set_rv = lg ? chttpcli_set_engine_logger(lg) : ccol_invalid_args;
  if (lg) clog_close(lg);

  clog closed = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (closed) clog_close(closed);
  ccol_retval closed_rv =
      closed ? chttpcli_set_engine_logger(closed) : ccol_unexpected_failure;

  char url[128];
  make_url(url, sizeof(url), "/get");
  int status = 0;
  chttpcli cli = chttpcli_create(NULL);
  chttp_request *req =
      cli ? chttp_request_create(CHTTP_GET, url, NULL, NULL) : NULL;
  cthrpool_future *f = req ? chttpcli_do_async(cli, req) : NULL;
  chttpcli_async_result *res = f ? chttpcli_async_result_get(f) : NULL;
  if (res && res->rv == ccol_success && res->resp)
    status = res->resp->status_code;
  if (res) {
    chttpcli_resp_free(res->resp);
    chttpcli_async_result_free(res);
  }
  if (f) cthrpool_future_free(f);
  chttp_request_free(req);
  if (cli) chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  clog quiet = clog_create_from_fd(2, CLOG_FATAL, NULL);
  ccol_retval quiet_rv =
      quiet ? chttpcli_set_engine_logger(quiet) : ccol_invalid_args;
  if (quiet) clog_close(quiet);
  close(fd);
  int created = count_lines_containing(
      path, "New http client reactor engine has been created");
  unlink(path);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(closed_rv, ccol_invalid_args);
  REQUIRE_EQ(quiet_rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(created, 1);
}

// A sweep verdict against a hop that finished
//
// Waits up to max_ms for the parked sweep of the sweep race test to reach
// phase. Gives false on a timeout.
static bool sweep_race_wait_for_phase(int phase, int max_ms) {
  for (int i = 0; i < max_ms; i++) {
    if (chttp__sweep_race_parked_phase_for_tests() == phase) return true;
    usleep(1000);
  }
  return false;
}

// Waits up to max_ms for the counter that read() gives to pass above, for the
// sweep race test. Gives false on a timeout.
static bool sweep_race_wait_for_count(unsigned (*read)(void), unsigned above,
                                      int max_ms) {
  for (int i = 0; i < max_ms; i++) {
    if (read() > above) return true;
    usleep(1000);
  }
  return false;
}

TEST(deadline_sweep, a_verdict_on_a_finished_hop_never_marks_its_pooled_ctx) {
  wait_for_async_engine_idle();
  atomic_store(&g_sweep_race_respond, false);
  char url[128];
  make_url(url, sizeof(url), "/sweep-race");
  chttpcli cli = chttpcli_create(NULL);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  REQUIRE_EQ(chttpcli_set_request_timeout(cli, 30000000), ccol_success);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  unsigned entered0 = chttp__offer_entered_count_for_tests();
  unsigned done0 = chttp__offer_done_count_for_tests();
  chttp__sweep_race_arm_for_tests();
  cthrpool_future *f = chttpcli_do_async(cli, req);
  bool parked1 = f && sweep_race_wait_for_phase(1, 5000);
  atomic_store(&g_sweep_race_respond, true);
  bool entered =
      parked1 && sweep_race_wait_for_count(chttp__offer_entered_count_for_tests,
                                           entered0, 5000);
  (void)sweep_race_wait_for_count(chttp__offer_done_count_for_tests, done0,
                                  300);
  chttp__sweep_race_release_for_tests(1);
  bool parked2 = entered && sweep_race_wait_for_phase(2, 5000);

  chttpcli_async_result *res = f ? chttpcli_async_result_get(f) : NULL;
  ccol_retval rv = res ? res->rv : ccol_unexpected_failure;
  struct chttpclient *raw = chttpcli__resolve_for_tests(cli);
  size_t idle = raw ? chttpcli__async_idle_total_count_for_tests(raw) : 0;
  size_t marked =
      raw ? chttpcli__async_idle_timed_out_count_for_tests(raw) : 99;
  chttp__sweep_race_disarm_for_tests();

  if (res) {
    chttpcli_resp_free(res->resp);
    chttpcli_async_result_free(res);
  }
  if (f) cthrpool_future_free(f);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  atomic_store(&g_sweep_race_respond, false);

  REQUIRE_TRUE(parked1);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(parked2);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idle, (size_t)1);
  REQUIRE_EQ(marked, (size_t)0);
}

// Empty port, host case and trailing dot
//
// Parses url with the library's URL parser, for the url_parsing tests of this
// section. Gives the code, and the port, the IPv6 flag, the host, the path and
// query and the origin key through the out parameters; it frees whatever the
// caller doesn't take.
static ccol_retval parse_url_parts(const char *url, uint16_t *port, bool *ipv6,
                                   char **host, char **pq, char **origin) {
  bool https = false, is_unix = false;
  char *auth = NULL, *sock = NULL;
  ccol_retval rv = chttp__parse_url_for_tests(url, &https, ipv6, host, port, pq,
                                              origin, &auth, &is_unix, &sock);
  free(auth);
  free(sock);
  return rv;
}

TEST(url_parsing, an_empty_port_names_the_default_port_of_the_scheme) {
  struct {
    const char *url;
    uint16_t port;
    bool ipv6;
    const char *host;
    const char *pq;
  } cases[] = {
      {"http://example.com:/x", 80, false, "example.com", "/x"},
      {"https://example.com:/", 443, false, "example.com", "/"},
      {"http://[::1]:/y", 80, true, "::1", "/y"},
      {"https://[::1]:", 443, true, "::1", "/"},
      {"http://example.com:?q=1", 80, false, "example.com", "/?q=1"},
      {"http://example.com:#frag", 80, false, "example.com", "/"},
  };
  int failures = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    uint16_t port = 0;
    bool ipv6 = false;
    char *host = NULL, *pq = NULL, *origin = NULL;
    ccol_retval rv =
        parse_url_parts(cases[i].url, &port, &ipv6, &host, &pq, &origin);
    bool ok = rv == ccol_success && port == cases[i].port &&
              ipv6 == cases[i].ipv6 && host &&
              strcmp(host, cases[i].host) == 0 && pq &&
              strcmp(pq, cases[i].pq) == 0;
    if (!ok) {
      fprintf(stderr, "empty port case %zu (%s) failed: rv=%d port=%u\n", i,
              cases[i].url, (int)rv, (unsigned)port);
      failures++;
    }
    free(host);
    free(pq);
    free(origin);
  }
  char *o1 = NULL, *o2 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  (void)parse_url_parts("http://example.com:/", &p, &v6, NULL, NULL, &o1);
  (void)parse_url_parts("http://example.com/", &p, &v6, NULL, NULL, &o2);
  bool same_origin = o1 && o2 && strcmp(o1, o2) == 0;
  free(o1);
  free(o2);

  char *resolved = chttp__resolve_redirect_url_for_tests(
      "http://example.com:8080/a", "https://other.example:/b");
  uint16_t rport = 0;
  bool rv6 = false;
  ccol_retval rrv =
      resolved ? parse_url_parts(resolved, &rport, &rv6, NULL, NULL, NULL)
               : ccol_unexpected_failure;
  free(resolved);

  ccol_retval zero =
      parse_url_parts("http://example.com:0/", &p, &v6, NULL, NULL, NULL);
  ccol_retval garbage =
      parse_url_parts("http://example.com:x/", &p, &v6, NULL, NULL, NULL);

  REQUIRE_EQ(failures, 0);
  REQUIRE_TRUE(same_origin);
  REQUIRE_EQ(rrv, ccol_success);
  REQUIRE_EQ(rport, 443);
  REQUIRE_EQ(zero, ccol_http_invalid_url);
  REQUIRE_EQ(garbage, ccol_http_invalid_url);
}

TEST(url_parsing, the_idle_pool_key_ignores_the_case_of_the_host) {
  char *o1 = NULL, *o2 = NULL, *o3 = NULL, *h1 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  ccol_retval r1 =
      parse_url_parts("HTTP://Example.COM:8080/a", &p, &v6, &h1, NULL, &o1);
  ccol_retval r2 =
      parse_url_parts("http://example.com:8080/b", &p, &v6, NULL, NULL, &o2);
  ccol_retval r3 =
      parse_url_parts("http://[::ABCD]:8080/", &p, &v6, NULL, NULL, &o3);
  bool same = o1 && o2 && strcmp(o1, o2) == 0;
  bool key_lower = o1 && strcmp(o1, "http://example.com:8080") == 0;
  bool v6_lower = o3 && strcmp(o3, "http://[::abcd]:8080") == 0;
  bool host_kept = h1 && strcmp(h1, "Example.COM") == 0;
  free(o1);
  free(o2);
  free(o3);
  free(h1);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_TRUE(same);
  REQUIRE_TRUE(key_lower);
  REQUIRE_TRUE(v6_lower);
  REQUIRE_TRUE(host_kept);
}

TEST(url_parsing, a_trailing_dot_stays_in_the_host_and_the_origin) {
  char *host = NULL, *o1 = NULL, *o2 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  ccol_retval r1 =
      parse_url_parts("https://www.example.com./", &p, &v6, &host, NULL, &o1);
  ccol_retval r2 =
      parse_url_parts("https://www.example.com/", &p, &v6, NULL, NULL, &o2);
  bool host_ok = host && strcmp(host, "www.example.com.") == 0;
  bool distinct = o1 && o2 && strcmp(o1, o2) != 0;
  free(host);
  free(o1);
  free(o2);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(host_ok);
  REQUIRE_TRUE(distinct);
}

TEST(tls_trailing_dot, a_fully_qualified_name_verifies_and_sends_sni) {
  char dir[] = "chttpcli_tls_dot_XXXXXX";
  REQUIRE_NE((void *)mkdtemp(dir), NULL);
  char named_cert[128], named_key[128], def_cert[128], def_key[128];
  snprintf(named_cert, sizeof(named_cert), "%s/named.pem", dir);
  snprintf(named_key, sizeof(named_key), "%s/named.key", dir);
  snprintf(def_cert, sizeof(def_cert), "%s/default.pem", dir);
  snprintf(def_key, sizeof(def_key), "%s/default.key", dir);
  int g1 = _openssl_selfsigned(named_key, named_cert, "tls-dot.test",
                               "DNS:tls-dot.test");
  int g2 = _openssl_selfsigned(def_key, def_cert, "unrelated.test",
                               "DNS:unrelated.test");
  bool dotted = false, plain = false, wrong = true;
  if (g1 == 0 && g2 == 0) {
    dotted = chttp__tls_client_handshake_for_tests(
        "tls-dot.test.", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
    plain = chttp__tls_client_handshake_for_tests(
        "tls-dot.test", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
    wrong = chttp__tls_client_handshake_for_tests(
        "tls-dot.test..", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
  }
  unlink(named_cert);
  unlink(named_key);
  unlink(def_cert);
  unlink(def_key);
  rmdir(dir);
  REQUIRE_EQ(g1, 0);
  REQUIRE_EQ(g2, 0);
  REQUIRE_TRUE(plain);
  REQUIRE_TRUE(dotted);
  REQUIRE_FALSE(wrong);
}

// A response that arrives while the request is still sent
//
// The modes of the early-response server (ers):
//   ERS_REJECT_CLOSE   413 once the head is in, then a lingering close that
//                      resets the connection under a client still sending.
//   ERS_REJECT_HOLD    401 with keep-alive, then hold the connection without
//                      reading until released. A GET gets 200 "ok" and a close.
//   ERS_HINTS_READ     103, then read the whole body and answer 200 "got <n>".
//   ERS_STREAM_ECHO    200 with the body's length, then echo the body as it
//                      arrives.
//   ERS_EARLY_200      a chunked 200 head at once, then read the whole body,
//                      then one chunk "got <n>" and the end.
//   ERS_LATE_REJECT    read 64 KiB, stop reading for 300 ms so the client's
//                      send blocks, then 413 and hold without reading.
//   ERS_SLOW_CONTINUE  per request: Wait 250 ms, send 100 Continue, read the
//                      body and answer 200 "ok".
//   ERS_STREAM_ECHO_KA ERS_STREAM_ECHO with keep-alive, then 200 "ok" for every
//                      later request.
enum {
  ERS_REJECT_CLOSE,
  ERS_REJECT_HOLD,
  ERS_HINTS_READ,
  ERS_STREAM_ECHO,
  ERS_EARLY_200,
  ERS_LATE_REJECT,
  ERS_SLOW_CONTINUE,
  ERS_STREAM_ECHO_KA
};

// The most connections one ers server serves; ers_stop joins their threads.
#define ERS_MAX_CONNS 16

// One early-response server: Its listener, mode, counters and connection
// threads. release ends the holds; mu guards conn_tids.
typedef struct {
  int fd;
  int port;
  int mode;
  pthread_t tid;
  atomic_int running;
  atomic_int accepts;
  atomic_int release;
  pthread_mutex_t mu;
  pthread_t conn_tids[ERS_MAX_CONNS];
  int conn_count;
} ers_srv_t;

// The argument of one connection thread.
typedef struct {
  ers_srv_t *s;
  int fd;
} ers_conn_t;

// Gives the Content-Length of a request head, or 0, so a mode knows how much
// body to read or echo.
static size_t ers_content_length(const char *hdrs) {
  const char *p = strcasestr(hdrs, "\r\ncontent-length:");
  if (!p) return 0;
  return (size_t)strtoull(p + strlen("\r\ncontent-length:"), NULL, 10);
}

// Sends all n bytes of p on fd, stopping at the first error; the modes answer
// through it.
static void ers_send_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

// Serves one connection of an ers server the way its mode says. arg is an
// ers_conn_t that the thread frees. Holding modes wait for s->release.
static void *ers_conn_thread(void *arg) {
  ers_conn_t *c = (ers_conn_t *)arg;
  ers_srv_t *s = c->s;
  int fd = c->fd;
  free(c);
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  size_t hl = 0;
  ssize_t n = buf ? srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl) : -1;
  if (n <= 0 || hl == 0) goto out;
  if (strncmp(buf, "GET ", 4) == 0) {
    static const char ok[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
    goto out;
  }
  size_t cl = ers_content_length(buf);
  size_t have = (size_t)n - hl;
  while (s->mode == ERS_SLOW_CONTINUE) {
    usleep(250000);
    static const char c100[] = "HTTP/1.1 100 Continue\r\n\r\n";
    ers_send_all(fd, c100, sizeof(c100) - 1);
    while (have < cl) {
      ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
      if (r <= 0) goto out;
      have += (size_t)r;
    }
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
    n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
    if (n <= 0 || hl == 0) goto out;
    cl = ers_content_length(buf);
    have = (size_t)n - hl;
  }
  switch (s->mode) {
    case ERS_REJECT_CLOSE: {
      static const char r[] =
          "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n"
          "Connection: close\r\n\r\ntoo large";
      ers_send_all(fd, r, sizeof(r) - 1);
      shutdown(fd, SHUT_WR);
      ers_discard(fd, buf, TEST_SERVER_BUF);
      break;
    }
    case ERS_REJECT_HOLD: {
      static const char r[] =
          "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\n"
          "unauthorized";
      ers_send_all(fd, r, sizeof(r) - 1);
      for (int i = 0; i < 1500 && !atomic_load(&s->release); i++) usleep(10000);
      break;
    }
    case ERS_HINTS_READ: {
      static const char h[] =
          "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n";
      ers_send_all(fd, h, sizeof(h) - 1);
      while (have < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      char resp[128];
      char body[32];
      int bl = snprintf(body, sizeof(body), "got %zu", have);
      int rl = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s",
                        bl, body);
      ers_send_all(fd, resp, (size_t)rl);
      break;
    }
    case ERS_STREAM_ECHO:
    case ERS_STREAM_ECHO_KA: {
      char head[128];
      int hl2 =
          snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n%s\r\n", cl,
                   s->mode == ERS_STREAM_ECHO ? "Connection: close\r\n" : "");
      ers_send_all(fd, head, (size_t)hl2);
      size_t echoed = 0;
      if (have > 0) {
        ers_send_all(fd, buf + hl, have);
        echoed = have;
      }
      while (echoed < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        ers_send_all(fd, buf, (size_t)r);
        echoed += (size_t)r;
      }
      while (s->mode == ERS_STREAM_ECHO_KA) {
        n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
        if (n <= 0 || hl == 0) break;
        static const char ok[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        ers_send_all(fd, ok, sizeof(ok) - 1);
      }
      break;
    }
    case ERS_EARLY_200: {
      static const char h[] =
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n";
      ers_send_all(fd, h, sizeof(h) - 1);
      while (have < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      char body[32];
      int bl = snprintf(body, sizeof(body), "got %zu", have);
      char tail[96];
      int tl = snprintf(tail, sizeof(tail), "%x\r\n%s\r\n0\r\n\r\n", bl, body);
      ers_send_all(fd, tail, (size_t)tl);
      break;
    }
    case ERS_LATE_REJECT: {
      while (have < 65536) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      usleep(300000);
      static const char r[] =
          "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n\r\n"
          "too large";
      ers_send_all(fd, r, sizeof(r) - 1);
      for (int i = 0; i < 1500 && !atomic_load(&s->release); i++) usleep(10000);
      break;
    }
    default:
      break;
  }
out:
  free(buf);
  close(fd);
  return NULL;
}

// Accepts connections of the ers server in arg, counts them in accepts and
// starts a thread for each, recorded under mu, until running clears.
static void *ers_accept_loop(void *arg) {
  ers_srv_t *s = (ers_srv_t *)arg;
  while (atomic_load(&s->running)) {
    int fd = test_accept(s->fd);
    if (fd < 0) {
      if (!atomic_load(&s->running)) break;
      continue;
    }
    if (!atomic_load(&s->running)) {
      close(fd);
      break;
    }
    atomic_fetch_add(&s->accepts, 1);
    ers_conn_t *c = (ers_conn_t *)malloc(sizeof(*c));
    pthread_mutex_lock(&s->mu);
    bool room = s->conn_count < ERS_MAX_CONNS;
    pthread_mutex_unlock(&s->mu);
    if (!c || !room) {
      free(c);
      close(fd);
      continue;
    }
    c->s = s;
    c->fd = fd;
    pthread_t tid;
    if (pthread_create(&tid, NULL, ers_conn_thread, c) != 0) {
      free(c);
      close(fd);
      continue;
    }
    pthread_mutex_lock(&s->mu);
    s->conn_tids[s->conn_count++] = tid;
    pthread_mutex_unlock(&s->mu);
  }
  return NULL;
}

// Starts an ers server in mode on an ephemeral loopback port and fills *s.
// Gives false when it can't.
static bool ers_start(ers_srv_t *s, int mode) {
  memset(s, 0, sizeof(*s));
  s->mode = mode;
  pthread_mutex_init(&s->mu, NULL);
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->fd < 0) return false;
  int opt = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  int small = 16384;
  setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(s->fd, 16) != 0 ||
      getsockname(s->fd, (struct sockaddr *)&a, &len) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  s->port = ntohs(a.sin_port);
  atomic_store(&s->running, 1);
  if (pthread_create(&s->tid, NULL, ers_accept_loop, s) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  return true;
}

// Stops an ers server: Releases the holds, stops the accept loop and joins
// every connection thread.
static void ers_stop(ers_srv_t *s) {
  if (s->fd < 0) return;
  atomic_store(&s->release, 1);
  atomic_store(&s->running, 0);
  test_wake_listener(s->fd);
  pthread_join(s->tid, NULL);
  close(s->fd);
  s->fd = -1;
  for (int i = 0; i < s->conn_count; i++) pthread_join(s->conn_tids[i], NULL);
  s->conn_count = 0;
  pthread_mutex_destroy(&s->mu);
}

// Gives CLOCK_MONOTONIC in milliseconds, for the timing checks of the
// early-response tests.
static long ers_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// A body bigger than any loopback socket buffer, so the request's send can't
// finish before the server answers.
#define ERS_BIG_BODY (16u * 1024u * 1024u)

// The request timeout of a test that echoes ERS_BIG_BODY back. It leaves room
// for a slow run under valgrind, and a client that deadlocks still reports
// ccol_timed_out.
#define ERS_ECHO_TIMEOUT_US 90000000u

// Builds a POST of a body_len-byte patterned body to the ers server on port.
// Gives the request, and the body in *body_out when it isn't NULL (else the
// body is freed); NULL on failure.
static chttp_request *ers_post(int port, size_t body_len, char **body_out) {
  char *body = (char *)malloc(body_len);
  if (!body) return NULL;
  for (size_t i = 0; i < body_len; i++) body[i] = (char)('a' + (i % 23));
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/upload", port);
  chttp_request_body rb = {.data = body,
                           .len = body_len,
                           .content_type = "application/octet-stream"};
  chttp_request *req = chttp_request_create(CHTTP_POST, url, &rb, NULL);
  if (body_out)
    *body_out = body;
  else
    free(body);
  return req;
}

// The reset discards what the client hasn't read, so the 413 must be read while
// the body goes out. A client that reads only after the whole body reports
// ccol_http_transfer_aborted.
TEST(early_response, reject_then_close_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_REJECT_CLOSE));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 20000000);
  chttp_request *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  char body[4][16];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 413);
    REQUIRE_STREQ(body[tier], "too large");
  }
}

// The server answers 401 and never reads the body, so the client's write
// blocks for good. Every tier must return the answer at once and never pool the
// connection.
TEST(early_response, reject_while_not_reading_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_REJECT_HOLD));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 15000000);
  chttp_request *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  char get_url[128];
  snprintf(get_url, sizeof(get_url), "http://127.0.0.1:%d/after", s.port);
  chttp_request *get = chttp_request_create(CHTTP_GET, get_url, NULL, NULL);
  ccol_retval rv[4] = {0}, grv[4] = {0};
  int status[4] = {0}, gstatus[4] = {0};
  long elapsed[4] = {0};
  int accepts[4] = {0};
  for (int tier = 1; tier <= 3 && req && get && cli != CHTTPCLI_NULL; tier++) {
    int before = atomic_load(&s.accepts);
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
    resp = NULL;
    grv[tier] = run_in_tier(tier, cli, get, &resp);
    gstatus[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
    accepts[tier] = atomic_load(&s.accepts) - before;
  }
  bool have = req && get && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  chttp_request_free(get);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 401);
    REQUIRE_LT(elapsed[tier], 10000L);
    REQUIRE_EQ(grv[tier], ccol_success);
    REQUIRE_EQ(gstatus[tier], 200);
    REQUIRE_EQ(accepts[tier], 2);
  }
}

TEST(early_response, interim_response_lets_the_upload_finish_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_HINTS_READ));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 20000000);
  size_t len = 2u * 1024u * 1024u;
  chttp_request *req = ers_post(s.port, len, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  char body[4][32];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  char expect[32];
  snprintf(expect, sizeof(expect), "got %zu", len);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_STREQ(body[tier], expect);
  }
}

// The server echoes the body while it reads it. A client that only writes
// until the request is complete deadlocks once both socket buffers fill up.
TEST(early_response, streamed_2xx_answer_keeps_the_upload_going_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_STREAM_ECHO));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL)
    chttpcli_set_request_timeout(cli, ERS_ECHO_TIMEOUT_US);
  size_t len = ERS_BIG_BODY;
  char *sent = NULL;
  chttp_request *req = ers_post(s.port, len, &sent);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  bool same[4] = {false};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      same[tier] = resp->body_len == len && resp->body &&
                   memcmp(resp->body, sent, len) == 0;
    }
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  free(sent);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_TRUE(same[tier]);
  }
}

// The server sends a 200 head at once and then reads the whole body without
// writing more, so the client must go on with the upload.
TEST(early_response, early_2xx_head_lets_the_upload_finish_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_EARLY_200));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 20000000);
  size_t len = ERS_BIG_BODY;
  chttp_request *req = ers_post(s.port, len, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  char body[4][32];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  char expect[32];
  snprintf(expect, sizeof(expect), "got %zu", len);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_STREQ(body[tier], expect);
  }
}

// The 413 comes in on the direction the blocked send isn't waiting on; a
// client that only waits for room to write times out.
TEST(early_response, reject_after_the_send_blocked_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_LATE_REJECT));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 15000000);
  chttp_request *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 413);
    REQUIRE_LT(elapsed[tier], 10000L);
  }
}

// Happy eyeballs (RFC 8305)
TEST(happy_eyeballs, addresses_alternate_their_families) {
  const int f1[] = {AF_INET6, AF_INET6, AF_INET6, AF_INET, AF_INET};
  size_t o1[5];
  chttp__addr_list_interleave_for_tests(f1, 5, o1);
  const size_t e1[] = {0, 3, 1, 4, 2};
  for (size_t i = 0; i < 5; i++) REQUIRE_EQ(o1[i], e1[i]);

  const int f2[] = {AF_INET, AF_INET, AF_INET6};
  size_t o2[3];
  chttp__addr_list_interleave_for_tests(f2, 3, o2);
  const size_t e2[] = {0, 2, 1};
  for (size_t i = 0; i < 3; i++) REQUIRE_EQ(o2[i], e2[i]);

  const int f3[] = {AF_INET, AF_INET, AF_INET};
  size_t o3[3];
  chttp__addr_list_interleave_for_tests(f3, 3, o3);
  for (size_t i = 0; i < 3; i++) REQUIRE_EQ(o3[i], i);
}

// Gives a socket whose address (in *out) takes connects that neither complete
// nor fail, an address whose packets get dropped, for the happy eyeballs tests.
// On Linux it's a listener with a full accept queue and *filler_out holds the
// connection that fills it. The BSDs and macOS answer such a SYN, so there it's
// a port held without listening, and the client's test hook leaves every
// attempt on it unconnected.
static int blackhole_listener(struct sockaddr_in *out, int *filler_out) {
  *filler_out = -1;
  int l = socket(AF_INET, SOCK_STREAM, 0);
  if (l < 0) return -1;
#if !defined(__linux__)
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t blen = sizeof(*out);
  if (bind(l, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(l, (struct sockaddr *)out, &blen) != 0) {
    close(l);
    return -1;
  }
  chttp__set_silent_addr_for_tests((const struct sockaddr *)out, sizeof(*out));
  return l;
#endif
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(*out);
  if (bind(l, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(l, (struct sockaddr *)out, &len) != 0 || listen(l, 0) != 0) {
    close(l);
    return -1;
  }
  int f = ccol_socket_nb(AF_INET, SOCK_STREAM, 0);
  if (f < 0) {
    close(l);
    return -1;
  }
  (void)test_connect(f, (struct sockaddr *)out, sizeof(*out));
  struct pollfd p = {.fd = f, .events = POLLOUT, .revents = 0};
  int soerr = 1;
  socklen_t sl = sizeof(soerr);
  if (poll(&p, 1, 2000) != 1 ||
      getsockopt(f, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
    close(f);
    close(l);
    return -1;
  }
  *filler_out = f;
  return l;
}

// Gives the number of open descriptors of the process, for the leak checks of
// the connect tests.
static int count_open_fds(void) { return test_count_open_fds(); }

// Gives the open descriptor count once the mock servers have closed their end
// of every connection (g_mock_conns_open) and the async engine is down, so the
// count at the end of a test can be compared with it. It waits about 10 s at
// most.
static int settled_fd_baseline(void) {
  wait_for_async_engine_idle();
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  while (atomic_load(&g_mock_conns_open) > 0 && elapsed_ms_since(&t0) < 10000L)
    usleep(1000);
  return count_open_fds();
}

// A connect that starts while the sweep judges its hop, and whose deadline
// passes before the verdict, is shut down by that sweep. A sweep that reads the
// fd before it marks the hop sees none and leaves the hop running with no
// deadline.
TEST(async_deadline, a_connect_that_starts_as_the_sweep_judges_it_ends) {
  wait_for_async_engine_idle();
  struct sockaddr_in hole;
  memset(&hole, 0, sizeof(hole));
  hole.sin_family = AF_INET;
  hole.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  hole.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[1] = {(const struct sockaddr *)&hole};
  socklen_t lens[1] = {sizeof(hole)};
  chttp__set_addr_override_for_tests(addrs, lens, 1);
  chttp__set_silent_addr_for_tests((const struct sockaddr *)&hole,
                                   sizeof(hole));
  chttp__set_name_lookup_delay_ms_for_tests(20);
  chttp__sweep_fd_wait_arm_for_tests();
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_connect_timeout(cli, 2000000);
  cthrpool_future *f = async_get(cli, "http://late-connect.invalid/get");
  chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
  ccol_retval rv = raw ? raw->rv : ccol_unexpected_failure;
  size_t abandoned = chttp__async_hops_abandoned_for_tests();
  for (int i = 0; i < 1000 && abandoned != 0; i++) {
    usleep(10000);
    abandoned = chttp__async_hops_abandoned_for_tests();
  }
  char trace[512];
  chttp__sweep_fd_wait_trace_for_tests(trace, sizeof(trace));
  bool held = chttp__sweep_fd_wait_held_for_tests();
  if (!held)
    fprintf(stderr,
            "[SWEEP_HOLD_DIAG] %s; sweep ticks %lu; request rv %d; abandoned "
            "%zu\n",
            trace, chttp__sweep_ticks_for_tests(), (int)rv, abandoned);
  if (raw) {
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  if (f) cthrpool_future_free(f);
  chttp__set_name_lookup_delay_ms_for_tests(0);
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_EQ(abandoned, (size_t)0);
}

// Polls the open descriptor count for about 10 s until it is back at baseline,
// because a server thread may still hold its end of a connection for a moment.
// Gives the last count.
static int settle_open_fds(int baseline) {
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int n = count_open_fds();
  while (n != baseline && elapsed_ms_since(&t0) < 10000L) {
    usleep(10000);
    n = count_open_fds();
  }
  return n;
}

// The first address drops every packet. A client that tries the addresses one
// after the other waits the whole connect timeout on it.
TEST(happy_eyeballs, a_silent_first_address_costs_one_attempt_delay) {
  struct sockaddr_in hole;
  int filler = -1;
  int hl = blackhole_listener(&hole, &filler);
  REQUIRE_GE(hl, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&hole,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(hole), sizeof(good)};

  int fds_before = settled_fd_baseline();
  chttp__set_addr_override_for_tests(addrs, lens, 2);
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_connect_timeout(cli, 10000000);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  int fds_after = settle_open_fds(fds_before);
  close(filler);
  close(hl);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_GE(elapsed[tier], 200L);
    REQUIRE_LT(elapsed[tier], 3000L);
  }
  REQUIRE_EQ(fds_after, fds_before);
}

// Tier 2/3 start the next address from a sweep walk, so the test checks the
// instant the sweep planned that walk for, which load can't move, against the
// instant the start was due.
TEST(happy_eyeballs, the_async_tiers_start_the_next_address_on_time) {
  struct sockaddr_in hole;
  int filler = -1;
  int hl = blackhole_listener(&hole, &filler);
  REQUIRE_GE(hl, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&hole,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(hole), sizeof(good)};

  chttp__set_addr_override_for_tests(addrs, lens, 2);
  chttp__sweep_due_reset_for_tests();
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_connect_timeout(cli, 10000000);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval rv[6];
  long elapsed[6];
  for (int i = 0; i < 6; i++) {
    rv[i] = ccol_unexpected_failure;
    elapsed[i] = 0;
  }
  for (int i = 0; i < 6 && req && cli != CHTTPCLI_NULL; i++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    (void)chttp_request_set_header(req, "Connection", "close");
    rv[i] = run_in_tier(2 + (i % 2), cli, req, &resp);
    elapsed[i] = ers_now_ms() - t0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  int64_t max_late_ns = 0;
  unsigned long starts = chttp__sweep_due_for_tests(1, &max_late_ns);
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  close(filler);
  close(hl);

  REQUIRE_TRUE(have);
  for (int i = 0; i < 6; i++) {
    REQUIRE_EQ(rv[i], ccol_success);
    REQUIRE_GE(elapsed[i], 250L);
    REQUIRE_LT(elapsed[i], 8000L);
  }
  REQUIRE_EQ(starts, 6UL);
  REQUIRE_LE(max_late_ns, (int64_t)0);
}

TEST(happy_eyeballs, the_connect_timeout_bounds_the_whole_race) {
  struct sockaddr_in h1, h2;
  int f1 = -1, f2 = -1;
  int l1 = blackhole_listener(&h1, &f1);
  int l2 = blackhole_listener(&h2, &f2);
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&h1,
                                     (const struct sockaddr *)&h2};
  socklen_t lens[2] = {sizeof(h1), sizeof(h2)};
  bool ready = l1 >= 0 && l2 >= 0;

  int fds_before = settled_fd_baseline();
  if (ready) chttp__set_addr_override_for_tests(addrs, lens, 2);
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_connect_timeout(cli, 700000);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval rv[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && ready && req && cli != CHTTPCLI_NULL;
       tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  int fds_after = settle_open_fds(fds_before);
  if (f1 >= 0) close(f1);
  if (f2 >= 0) close(f2);
  if (l1 >= 0) close(l1);
  if (l2 >= 0) close(l2);

  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_timed_out);
    REQUIRE_GE(elapsed[tier], 600L);
    REQUIRE_LT(elapsed[tier], 5000L);
  }
  REQUIRE_EQ(fds_after, fds_before);
}

// The attempt delay is one minute here, so a hand-over that waits for it fails
// the 30 s bound.
TEST(happy_eyeballs, a_refused_address_hands_over_at_once) {
  struct sockaddr_in refused;
  int rfd = refused_port_socket(&refused);
  REQUIRE_GE(rfd, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&refused,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(refused), sizeof(good)};
  chttp__set_addr_override_for_tests(addrs, lens, 2);
  chttp__set_connect_attempt_delay_ms_for_tests(60000);
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_connect_timeout(cli, 90000000);
  chttp_request *req = chttp_request_create(
      CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval rv[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  chttp__set_connect_attempt_delay_ms_for_tests(0);
  chttp__set_addr_override_for_tests(NULL, NULL, 0);
  close(rfd);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_LT(elapsed[tier], 30000L);
  }
}

// The first address stays in flight, the second completes and the hook reports
// it as ECONNREFUSED; the third must start at once instead of after the
// minute-long delay.
TEST(happy_eyeballs, tier1_an_asynchronous_failure_hands_over_at_once) {
  struct sockaddr_in hole;
  int filler = -1;
  int hl = blackhole_listener(&hole, &filler);
  struct sockaddr_in lst;
  memset(&lst, 0, sizeof(lst));
  lst.sin_family = AF_INET;
  lst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t llen = sizeof(lst);
  int lfd = ccol_socket_nb(AF_INET, SOCK_STREAM, 0);
  bool ready = hl >= 0 && lfd >= 0 &&
               bind(lfd, (struct sockaddr *)&lst, sizeof(lst)) == 0 &&
               listen(lfd, 8) == 0 &&
               getsockname(lfd, (struct sockaddr *)&lst, &llen) == 0;
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[3] = {(const struct sockaddr *)&hole,
                                     (const struct sockaddr *)&lst,
                                     (const struct sockaddr *)&good};
  socklen_t lens[3] = {sizeof(hole), sizeof(lst), sizeof(good)};

  ccol_retval rv = ccol_unexpected_failure;
  int status = 0;
  long elapsed = -1;
  bool have = false;
  if (ready) {
    chttp__set_addr_override_for_tests(addrs, lens, 3);
    chttp__set_connect_attempt_delay_ms_for_tests(60000);
    chttp__set_connect_short_delays_for_tests(1);
    chttp__set_connect_async_failures_for_tests(1);
    chttpcli cli = chttpcli_create(NULL);
    if (cli != CHTTPCLI_NULL) {
      chttpcli_set_connect_timeout(cli, 30000000);
      chttpcli_set_request_timeout(cli, 60000000);
    }
    chttp_request *req = chttp_request_create(
        CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
    have = req && cli != CHTTPCLI_NULL;
    if (have) {
      chttpcli_response *resp = NULL;
      long t0 = ers_now_ms();
      rv = chttpcli_do(cli, req, &resp);
      elapsed = ers_now_ms() - t0;
      status = resp ? resp->status_code : 0;
      chttpcli_resp_free(resp);
    }
    chttp_request_free(req);
    if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
    chttp__set_connect_async_failures_for_tests(0);
    chttp__set_connect_short_delays_for_tests(0);
    chttp__set_connect_attempt_delay_ms_for_tests(0);
    chttp__set_addr_override_for_tests(NULL, NULL, 0);
  }
  int accepted = lfd >= 0 ? accept(lfd, NULL, NULL) : -1;
  if (accepted >= 0) close(accepted);
  if (lfd >= 0) close(lfd);
  if (filler >= 0) close(filler);
  if (hl >= 0) close(hl);

  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(have);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_LT(elapsed, 20000L);
  REQUIRE_GE(accepted, 0);
}

// Close-on-exec
//
// The tally of count_client_sockets.
typedef struct {
  int port;
  const char *unix_path;
  int found;
  int inheritable;
} client_socket_census_t;

// Counts fd in the census in arg when its peer is the census's port or unix
// path, and counts it as inheritable when FD_CLOEXEC is missing.
static void census_one_fd(int fd, void *arg) {
  client_socket_census_t *c = (client_socket_census_t *)arg;
  int port = c->port;
  const char *unix_path = c->unix_path;
  {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    bool match = false;
    if (ss.ss_family == AF_INET && port > 0)
      match = ntohs(((struct sockaddr_in *)&ss)->sin_port) == port;
    else if (ss.ss_family == AF_UNIX && unix_path)
      match = strcmp(((struct sockaddr_un *)&ss)->sun_path, unix_path) == 0;
    if (!match) return;
    c->found++;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) c->inheritable++;
  }
}

// Counts this process's sockets whose peer is TCP port `port` on loopback or
// the unix socket at unix_path, and how many of them lack FD_CLOEXEC, so the
// close-on-exec tests can find the client's pooled connections.
static void count_client_sockets(int port, const char *unix_path, int *found,
                                 int *inheritable) {
  client_socket_census_t c = {
      .port = port, .unix_path = unix_path, .found = 0, .inheritable = 0};
  test_for_each_open_fd(census_one_fd, &c);
  *found = c.found;
  *inheritable = c.inheritable;
}

TEST(cloexec, pooled_client_sockets_are_close_on_exec) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");
  char uurl[512];
  make_unix_url(uurl, sizeof(uurl), "/keepalive");
  chttpcli cli = chttpcli_create(NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttp_request *ureq = chttp_request_create(CHTTP_GET, uurl, NULL, NULL);
  ccol_retval rv[4] = {0}, urv[4] = {0};
  for (int tier = 1; tier <= 2 && req && ureq && cli != CHTTPCLI_NULL; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpcli_resp_free(resp);
    resp = NULL;
    urv[tier] = run_in_tier(tier, cli, ureq, &resp);
    chttpcli_resp_free(resp);
  }
  int found = 0, inheritable = 0, ufound = 0, uinheritable = 0;
  count_client_sockets(get_test_port(), NULL, &found, &inheritable);
  count_client_sockets(0, get_test_unix_socket_path(), &ufound, &uinheritable);
  bool have = req && ureq && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  chttp_request_free(ureq);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 2; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(urv[tier], ccol_success);
  }
  REQUIRE_GE(found, 2);
  REQUIRE_GE(ufound, 2);
  REQUIRE_EQ(inheritable, 0);
  REQUIRE_EQ(uinheritable, 0);
}

// The C library entry points that the open watch below reaches. On glibc a
// unit with 64-bit file offsets calls fopen64 and open64 under the names fopen
// and open, and so do our definitions.
#if defined(__GLIBC__)
#define TEST_REAL_FOPEN "fopen64"
#define TEST_REAL_OPEN "open64"
#else
#define TEST_REAL_FOPEN "fopen"
#define TEST_REAL_OPEN "open"
#endif
// AddressSanitizer and ThreadSanitizer define their own fopen() and open().
// With glibc those are weak and ours replace them; elsewhere they are strong
// and a second definition doesn't link, so such a build leaves the watch out
// (TEST_OPEN_WATCH 0) and skips its test.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TEST_UNDER_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TEST_UNDER_SANITIZER 1
#endif
#if defined(TEST_UNDER_SANITIZER) && !defined(__GLIBC__)
#define TEST_OPEN_WATCH 0
#else
#define TEST_OPEN_WATCH 1
#endif
#if TEST_OPEN_WATCH
// The path the open watch looks for, how many opens of it it saw and whether
// the last one asked for close-on-exec, all under g_open_mu.
static char g_open_watch[512];
static int g_open_watch_seen;
static bool g_open_watch_cloexec;
static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;

// Records an open of path in the watch globals when it is the watched path, so
// tls_files_are_opened_close_on_exec can see how the TLS layer opened it.
static void open_watch_note(const char *path, bool cloexec) {
  pthread_mutex_lock(&g_open_mu);
  if (path && g_open_watch[0] && strcmp(path, g_open_watch) == 0) {
    g_open_watch_seen++;
    g_open_watch_cloexec = cloexec;
  }
  pthread_mutex_unlock(&g_open_mu);
}

// Replaces fopen() for this binary: Notes whether the mode has "e" and passes
// the call to the C library's fopen.
FILE *fopen(const char *path, const char *mode) {
  static void *_Atomic real = NULL;
  void *fn = atomic_load(&real);
  if (!fn) {
    fn = dlsym(RTLD_NEXT, TEST_REAL_FOPEN);
    atomic_store(&real, fn);
  }
  open_watch_note(path, mode && strchr(mode, 'e') != NULL);
  if (!fn) {
    errno = ENOSYS;
    return NULL;
  }
  return ((FILE * (*)(const char *, const char *)) fn)(path, mode);
}

// Replaces open() for this binary: Notes whether flags has O_CLOEXEC and
// passes the call, with its mode when it takes one, to the C library.
int open(const char *path, int flags, ...) {
  static void *_Atomic real = NULL;
  void *fn = atomic_load(&real);
  if (!fn) {
    fn = dlsym(RTLD_NEXT, TEST_REAL_OPEN);
    atomic_store(&real, fn);
  }
  unsigned mode = 0;
  bool takes_mode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
  takes_mode = takes_mode || (flags & O_TMPFILE) == O_TMPFILE;
#endif
  if (takes_mode) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, unsigned);
    va_end(ap);
  }
  open_watch_note(path, (flags & O_CLOEXEC) != 0);
  if (!fn) {
    errno = ENOSYS;
    return -1;
  }
  return ((int (*)(const char *, int, ...))fn)(path, flags, mode);
}

// Notes the open and calls the C library's checking entry point name. With
// _FORTIFY_SOURCE glibc's open() without a mode calls __open_2 or __open64_2,
// so the watch replaces those too.
static int open_watch_checked(const char *name, const char *path, int flags) {
  open_watch_note(path, (flags & O_CLOEXEC) != 0);
  int (*fn)(const char *, int) =
      (int (*)(const char *, int))dlsym(RTLD_NEXT, name);
  if (!fn) {
    errno = ENOSYS;
    return -1;
  }
  return fn(path, flags);
}

// The two checking entry points, each defined under its symbol name through an
// asm label: With 64-bit offsets glibc maps the C name __open_2 to __open64_2,
// so a definition under that C name would define __open64_2 twice.
int test_open_watch_open_2(const char *path, int flags) __asm__("__open_2");
int test_open_watch_open_2(const char *path, int flags) {
  return open_watch_checked("__open_2", path, flags);
}

int test_open_watch_open64_2(const char *path, int flags) __asm__("__open64_2");
int test_open_watch_open64_2(const char *path, int flags) {
  return open_watch_checked("__open64_2", path, flags);
}
#endif

// The TLS layer must open the certificate and key files close-on-exec, so a
// process another thread spawns doesn't inherit them.
TEST(cloexec, tls_files_are_opened_close_on_exec) {
#if !TEST_OPEN_WATCH
  fprintf(stderr, "SKIP: the sanitizer here owns fopen() and open()\n");
#else
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  pthread_mutex_lock(&g_open_mu);
  snprintf(g_open_watch, sizeof(g_open_watch), "%s", g_tls_cert_path);
  g_open_watch_seen = 0;
  g_open_watch_cloexec = false;
  pthread_mutex_unlock(&g_open_mu);

  chttpcli cli = chttpcli_create(NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval trv =
      cli != CHTTPCLI_NULL ? chttpcli_set_tls(cli, &tls) : ccol_invalid_args;
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rv = ccol_invalid_args;
  if (req && trv == ccol_success) {
    chttpcli_response *resp = NULL;
    rv = chttpcli_do(cli, req, &resp);
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);

  pthread_mutex_lock(&g_open_mu);
  int seen = g_open_watch_seen;
  bool cloexec = g_open_watch_cloexec;
  g_open_watch[0] = '\0';
  pthread_mutex_unlock(&g_open_mu);

  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GT(seen, 0);
  REQUIRE_TRUE(cloexec);
#endif
}

// The TLS form of the early_response tests. Without the read after a failed or
// blocked write, every tier reports ccol_http_transfer_aborted or
// ccol_timed_out.
TEST(early_response, tls_answers_during_the_upload_are_returned_in_every_tier) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  atomic_store(&g_tls_early_release, 0);
  chttpcli cli = chttpcli_create(NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval trv =
      cli != CHTTPCLI_NULL ? chttpcli_set_tls(cli, &tls) : ccol_invalid_args;
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 15000000);
  size_t len = ERS_BIG_BODY;
  char *body = (char *)malloc(len);
  if (body) memset(body, 'b', len);
  chttp_request_body rb = {
      .data = body, .len = len, .content_type = "application/octet-stream"};
  const char *paths[2] = {"/early-reject-close", "/early-reject-hold"};
  const int want[2] = {413, 401};
  ccol_retval rv[2][4];
  int status[2][4];
  memset(rv, 0, sizeof(rv));
  memset(status, 0, sizeof(status));
  for (int k = 0; k < 2 && body && trv == ccol_success; k++) {
    char url[160];
    make_tls_url(url, sizeof(url), paths[k]);
    chttp_request *req = chttp_request_create(CHTTP_POST, url, &rb, NULL);
    for (int tier = 1; tier <= 3 && req; tier++) {
      chttpcli_response *resp = NULL;
      rv[k][tier] = run_in_tier(tier, cli, req, &resp);
      status[k][tier] = resp ? resp->status_code : 0;
      chttpcli_resp_free(resp);
    }
    chttp_request_free(req);
  }
  atomic_store(&g_tls_early_release, 1);
  free(body);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(trv, ccol_success);
  for (int k = 0; k < 2; k++)
    for (int tier = 1; tier <= 3; tier++) {
      REQUIRE_EQ(rv[k][tier], ccol_success);
      REQUIRE_EQ(status[k][tier], want[k]);
    }
}

// The GET that follows must reach the server on a connection of its own; a
// reuse makes the server count one connection.
TEST(early_response, a_response_that_started_during_the_send_is_never_pooled) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_STREAM_ECHO_KA));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL)
    chttpcli_set_request_timeout(cli, ERS_ECHO_TIMEOUT_US);
  chttp_request *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  char get_url[128];
  snprintf(get_url, sizeof(get_url), "http://127.0.0.1:%d/after", s.port);
  chttp_request *get = chttp_request_create(CHTTP_GET, get_url, NULL, NULL);
  ccol_retval rv[4] = {0}, grv[4] = {0};
  int status[4] = {0};
  int accepts[4] = {0};
  for (int tier = 1; tier <= 3 && req && get && cli != CHTTPCLI_NULL; tier++) {
    int before = atomic_load(&s.accepts);
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
    resp = NULL;
    grv[tier] = run_in_tier(tier, cli, get, &resp);
    chttpcli_resp_free(resp);
    accepts[tier] = atomic_load(&s.accepts) - before;
  }
  bool have = req && get && cli != CHTTPCLI_NULL;
  chttp_request_free(req);
  chttp_request_free(get);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_EQ(grv[tier], ccol_success);
    REQUIRE_EQ(accepts[tier], 2);
  }
}

// Several 100-continue requests on one pooled connection wait across sweep
// ticks while each rewrites the connection's continue deadline; every one gets
// its answer.
TEST(expect_continue, pooled_continue_windows_over_sweep_ticks) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_SLOW_CONTINUE));
  chttpcli cli = chttpcli_create(NULL);
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 10000000);
  chttp_request *req = ers_post(s.port, 64, NULL);
  if (req) req->expect_continue = true;
  ccol_retval rv[4] = {0};
  int status[4] = {0};
  for (int i = 1; i <= 3 && req && cli != CHTTPCLI_NULL; i++) {
    chttpcli_response *resp = NULL;
    rv[i] = run_in_tier(2, cli, req, &resp);
    status[i] = resp ? resp->status_code : 0;
    chttpcli_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_NULL;
  int accepts = atomic_load(&s.accepts);
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int i = 1; i <= 3; i++) {
    REQUIRE_EQ(rv[i], ccol_success);
    REQUIRE_EQ(status[i], 200);
  }
  REQUIRE_EQ(accepts, 1);
}

// A one-connection server for a_stale_sweep_snapshot_never_ends_a_later_window.
// Request 1 waits for go1, gets "100 Continue" and a keep-alive 200. Request 2
// never gets "100 Continue": The server records in body2_after_ms how long
// after its head the first body byte came, then answers 200.
typedef struct {
  int lfd;
  int port;
  pthread_t tid;
  atomic_int go1;
  atomic_long body2_after_ms;
  atomic_int done;
} cwin_srv_t;

// Serves the one connection of the cwin server in arg and sets done.
static void *cwin_srv_thread(void *arg) {
  cwin_srv_t *s = (cwin_srv_t *)arg;
  int fd = test_accept(s->lfd);
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (fd < 0 || !buf) goto out;
  for (int req = 1; req <= 2; req++) {
    size_t hl = 0;
    ssize_t n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
    if (n <= 0 || hl == 0) goto out;
    long t_h = ers_now_ms();
    size_t cl = ers_content_length(buf);
    size_t have = (size_t)n - hl;
    if (req == 1) {
      for (int i = 0; i < 1500 && !atomic_load(&s->go1); i++) usleep(10000);
      static const char c100[] = "HTTP/1.1 100 Continue\r\n\r\n";
      ers_send_all(fd, c100, sizeof(c100) - 1);
    }
    while (have < cl) {
      ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
      if (r <= 0) goto out;
      if (req == 2 && have == 0)
        atomic_store(&s->body2_after_ms, ers_now_ms() - t_h);
      have += (size_t)r;
    }
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
  }
  {
    size_t hl = 0;
    (void)srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
  }
out:
  free(buf);
  if (fd >= 0) close(fd);
  atomic_store(&s->done, 1);
  return NULL;
}

// The argument of cwin_release_thread.
typedef struct {
  unsigned from;
  atomic_int stored;
} cwin_release_t;

// Waits until the count of continue_deadline stores passes from, then lets the
// parked sweep go. It runs on its own thread, so nothing orders the second
// request's store with the release, which is what the test needs under
// ThreadSanitizer.
static void *cwin_release_thread(void *arg) {
  cwin_release_t *r = (cwin_release_t *)arg;
  bool stored = false;
  for (int i = 0; i < 5000 && !stored; i++) {
    stored = chttp__continue_deadline_stores_for_tests() != r->from;
    if (!stored) usleep(1000);
  }
  atomic_store(&r->stored, stored ? 1 : 0);
  chttp__continue_sweep_release_for_tests();
  return NULL;
}

// The sweep snapshots the connection during request 1's window and is held past
// its end, while request 2 starts its own window on the same connection.
// Request 2 must still wait its whole window; a dispatch that sends on the
// sweep's word alone sends its body within milliseconds.
TEST(expect_continue, a_stale_sweep_snapshot_never_ends_a_later_window) {
  cwin_srv_t s;
  memset(&s, 0, sizeof(s));
  atomic_store(&s.body2_after_ms, -1L);
  s.lfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  bool listening = s.lfd >= 0 &&
                   bind(s.lfd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
                   listen(s.lfd, 4) == 0 &&
                   getsockname(s.lfd, (struct sockaddr *)&a, &alen) == 0;
  s.port = ntohs(a.sin_port);
  bool srv_started =
      listening && pthread_create(&s.tid, NULL, cwin_srv_thread, &s) == 0;

  chttpcli cli = srv_started ? chttpcli_create(NULL) : CHTTPCLI_NULL;
  if (cli != CHTTPCLI_NULL) chttpcli_set_request_timeout(cli, 10000000);
  chttp_request *req = srv_started ? ers_post(s.port, 64, NULL) : NULL;
  if (req) req->expect_continue = true;
  bool have = req && cli != CHTTPCLI_NULL;

  bool parked = false, stored = false;
  ccol_retval rv1 = ccol_unexpected_failure, rv2 = ccol_unexpected_failure;
  int st1 = 0, st2 = 0;
  if (have) {
    chttp__continue_sweep_arm_for_tests();
    cthrpool_future *f1 = chttpcli_do_async(cli, req);
    for (int i = 0; f1 && i < 5000 && !parked; i++) {
      parked = chttp__continue_sweep_parked_for_tests();
      if (!parked) usleep(1000);
    }
    if (parked) usleep(1200000);
    atomic_store(&s.go1, 1);
    if (f1) {
      chttpcli_async_result *r = chttpcli_async_result_get(f1);
      if (r) {
        rv1 = r->rv;
        st1 = r->resp ? r->resp->status_code : 0;
        chttpcli_resp_free(r->resp);
        chttpcli_async_result_free(r);
      }
      cthrpool_future_free(f1);
    }
    cwin_release_t rel = {.from = chttp__continue_deadline_stores_for_tests()};
    atomic_store(&rel.stored, 0);
    pthread_t rel_tid;
    bool rel_started =
        parked && rv1 == ccol_success &&
        pthread_create(&rel_tid, NULL, cwin_release_thread, &rel) == 0;
    cthrpool_future *f2 = rel_started ? chttpcli_do_async(cli, req) : NULL;
    if (rel_started) {
      pthread_join(rel_tid, NULL);
      stored = atomic_load(&rel.stored) != 0;
    }
    chttp__continue_sweep_release_for_tests();
    if (f2) {
      chttpcli_async_result *r = chttpcli_async_result_get(f2);
      if (r) {
        rv2 = r->rv;
        st2 = r->resp ? r->resp->status_code : 0;
        chttpcli_resp_free(r->resp);
        chttpcli_async_result_free(r);
      }
      cthrpool_future_free(f2);
    }
  }
  chttp__continue_sweep_release_for_tests();
  chttp_request_free(req);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  if (srv_started) {
    test_wake_listener(s.lfd);
    pthread_join(s.tid, NULL);
  }
  if (s.lfd >= 0) close(s.lfd);
  long body2_after = atomic_load(&s.body2_after_ms);

  REQUIRE_TRUE(srv_started);
  REQUIRE_TRUE(have);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(stored);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_EQ(st1, 200);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_EQ(st2, 200);
  REQUIRE_GE(body2_after, 900L);
}

// A truncating conversion gives 0 for a deadline less than a millisecond away
// and 1 for one 1.5 ms away; it must round up.
TEST(deadline, remaining_milliseconds_round_up) {
  int ms = -7;
  REQUIRE_TRUE(chttp__deadline_remaining_ms_at_for_tests(1, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(chttp__deadline_remaining_ms_at_for_tests(500000, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(chttp__deadline_remaining_ms_at_for_tests(1000000, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(chttp__deadline_remaining_ms_at_for_tests(1500000, &ms));
  REQUIRE_EQ(ms, 2);
  REQUIRE_TRUE(chttp__deadline_remaining_ms_at_for_tests(2999999999LL, &ms));
  REQUIRE_EQ(ms, 3000);
  REQUIRE_FALSE(chttp__deadline_remaining_ms_at_for_tests(0, &ms));
  REQUIRE_FALSE(chttp__deadline_remaining_ms_at_for_tests(-1, &ms));
}

// Runs one GET of path with the given connect and request timeouts through Tier
// 1 or Tier 2, for the zero and huge timeout tests. Gives the code and the
// status, and the elapsed time in *elapsed_ms.
static ccol_retval huge_timeout_get(const char *path, uint64_t connect_us,
                                    uint64_t request_us, bool async,
                                    int *status, long *elapsed_ms) {
  char url[160];
  make_url(url, sizeof(url), path);
  *status = -1;
  *elapsed_ms = -1;
  chttpcli cli = chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL) return ccol_unexpected_failure;
  ccol_retval rv = chttpcli_set_connect_timeout(cli, connect_us);
  if (rv == ccol_success) rv = chttpcli_set_request_timeout(cli, request_us);
  if (rv == ccol_success) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (async) {
      cthrpool_future *f = async_get(cli, url);
      chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
      rv = raw ? raw->rv : ccol_unexpected_failure;
      if (raw && raw->resp) {
        *status = raw->resp->status_code;
        chttpcli_resp_free(raw->resp);
      }
      if (raw) chttpcli_async_result_free(raw);
      if (f) cthrpool_future_free(f);
    } else {
      chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
      chttpcli_response *resp = NULL;
      rv = req ? chttpcli_do(cli, req, &resp) : ccol_unexpected_failure;
      if (resp) {
        *status = resp->status_code;
        chttpcli_resp_free(resp);
      }
      if (req) chttp_request_free(req);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed_ms =
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  }
  chttpcli_destroy(cli);
  if (async) wait_for_async_engine_idle();
  return rv;
}

// A conversion that narrows a timeout to a signed type, or a deadline that
// wraps into the past, makes every UINT64_MAX request time out at once.
TEST(client_construction, zero_and_huge_timeouts_wait_for_the_answer) {
  const uint64_t timeouts[] = {0, UINT64_MAX, UINT64_MAX - 1};
  enum { N = sizeof(timeouts) / sizeof(timeouts[0]) };
  ccol_retval rv[2][N];
  int status[2][N];
  long elapsed[2][N];
  for (int tier = 0; tier < 2; tier++)
    for (int k = 0; k < N; k++)
      rv[tier][k] =
          huge_timeout_get("/slow", timeouts[k], timeouts[k], tier == 1,
                           &status[tier][k], &elapsed[tier][k]);
  for (int tier = 0; tier < 2; tier++)
    for (int k = 0; k < N; k++) {
      REQUIRE_EQ(rv[tier][k], ccol_success);
      REQUIRE_EQ(status[tier][k], 200);
      REQUIRE_GT(elapsed[tier][k], 50L);
    }
}

// A wait longer than INT_MAX ms runs in pieces. The hook makes the next pieces
// return at once, and the request must check its deadline and keep waiting.
TEST(client_construction, a_capped_wait_that_runs_out_waits_again) {
  const uint64_t thirty_days_us = 30ULL * 86400ULL * 1000000ULL;
  int status = 0;
  long elapsed = 0;
  chttp__short_next_capped_waits_for_tests(8);
  ccol_retval rv = huge_timeout_get("/slow", thirty_days_us, thirty_days_us,
                                    false, &status, &elapsed);
  chttp__short_next_capped_waits_for_tests(0);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

// A client that read 20000 us as milliseconds would outwait /held's hold and
// succeed.
TEST(client_construction, a_small_request_timeout_times_out_in_microseconds) {
  int status = 0;
  long elapsed = 0;
  atomic_store(&g_slow_started, 0);
  ccol_retval rv =
      huge_timeout_get("/held", 0, 20000, false, &status, &elapsed);
  for (int i = 0; i < 5000 && atomic_load(&g_slow_started) < 1; i++)
    usleep(1000);
  held_release();
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(elapsed, 5000L);
}

// Scripted peers: Framing, upgrade and reuse checks
//
// The most connections one scripted peer serves.
#define SCRIPTED_PEER_MAX_CONNS 8

// A listener on an ephemeral loopback port that runs fn for every connection
// on its own thread. Every wait inside fn is bounded and ends on stop, so
// scripted_peer_stop can join every thread. A non-zero rcvbuf makes a peer that
// doesn't read fill up after a few kilobytes.
typedef struct scripted_peer scripted_peer;
typedef void (*scripted_peer_fn)(scripted_peer *sp, int fd, int index);

// The peer's listener, threads and the counters its scripts report through
// (phase is raised by the test, stray is the input of the stray-input
// scripts).
struct scripted_peer {
  int lfd;
  int port;
  scripted_peer_fn fn;
  pthread_t accept_tid;
  bool accept_started;
  pthread_t conn_tids[SCRIPTED_PEER_MAX_CONNS];
  atomic_int nconn;
  atomic_bool stop;
  atomic_int phase;
  atomic_llong body_bytes;
  atomic_int saw_eof;
  atomic_int requests;
  const char *stray;
};

// The argument of one connection thread.
typedef struct {
  scripted_peer *sp;
  int fd;
  int index;
} scripted_peer_conn_arg;

// Runs the peer's script on one connection and closes it; frees arg.
static void *scripted_peer_conn_main(void *arg) {
  scripted_peer_conn_arg a = *(scripted_peer_conn_arg *)arg;
  free(arg);
  a.sp->fn(a.sp, a.fd, a.index);
  close(a.fd);
  return NULL;
}

// Accepts connections of the peer in arg until stop and starts a script
// thread for each, up to SCRIPTED_PEER_MAX_CONNS.
static void *scripted_peer_accept_main(void *arg) {
  scripted_peer *sp = (scripted_peer *)arg;
  while (!atomic_load(&sp->stop)) {
    struct pollfd p = {.fd = sp->lfd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 50) <= 0) continue;
    int fd = accept(sp->lfd, NULL, NULL);
    if (fd < 0) continue;
    int idx = atomic_load(&sp->nconn);
    scripted_peer_conn_arg *a = NULL;
    if (idx < SCRIPTED_PEER_MAX_CONNS)
      a = (scripted_peer_conn_arg *)malloc(sizeof(*a));
    if (!a) {
      close(fd);
      continue;
    }
    a->sp = sp;
    a->fd = fd;
    a->index = idx;
    if (pthread_create(&sp->conn_tids[idx], NULL, scripted_peer_conn_main, a) !=
        0) {
      free(a);
      close(fd);
      continue;
    }
    atomic_store(&sp->nconn, idx + 1);
  }
  return NULL;
}

// Starts sp with script fn, resetting its counters. rcvbuf (0: Default) is set
// on the listener before listen(), so every accepted socket inherits it.
// Returns 0, or -1.
static int scripted_peer_start(scripted_peer *sp, scripted_peer_fn fn,
                               int rcvbuf) {
  sp->fn = fn;
  sp->accept_started = false;
  atomic_store(&sp->nconn, 0);
  atomic_store(&sp->stop, false);
  atomic_store(&sp->phase, 0);
  atomic_store(&sp->body_bytes, 0);
  atomic_store(&sp->saw_eof, 0);
  atomic_store(&sp->requests, 0);
  sp->lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sp->lfd < 0) return -1;
  (void)fcntl(sp->lfd, F_SETFD, FD_CLOEXEC);
  if (rcvbuf > 0)
    setsockopt(sp->lfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(addr);
  if (bind(sp->lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(sp->lfd, 16) != 0 ||
      getsockname(sp->lfd, (struct sockaddr *)&addr, &len) != 0) {
    close(sp->lfd);
    sp->lfd = -1;
    return -1;
  }
  sp->port = ntohs(addr.sin_port);
  if (pthread_create(&sp->accept_tid, NULL, scripted_peer_accept_main, sp) !=
      0) {
    close(sp->lfd);
    sp->lfd = -1;
    return -1;
  }
  sp->accept_started = true;
  return 0;
}

// Sets stop and joins the accept thread and every script thread, then closes
// the listener.
static void scripted_peer_stop(scripted_peer *sp) {
  atomic_store(&sp->stop, true);
  if (sp->accept_started) pthread_join(sp->accept_tid, NULL);
  sp->accept_started = false;
  int n = atomic_load(&sp->nconn);
  for (int i = 0; i < n; i++) pthread_join(sp->conn_tids[i], NULL);
  if (sp->lfd >= 0) close(sp->lfd);
  sp->lfd = -1;
}

// Reads up to the end of a request head into buf, for ms milliseconds at most,
// so a script can answer after the head. Returns the bytes read, or -1.
static ssize_t scripted_peer_read_head(scripted_peer *sp, int fd, char *buf,
                                       size_t max, int ms) {
  size_t total = 0;
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop);) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&p, 1, 50);
    if (rc == 0) {
      waited += 50;
      continue;
    }
    if (rc < 0) return -1;
    ssize_t n = recv(fd, buf + total, max - 1 - total, 0);
    if (n <= 0) return -1;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n")) return (ssize_t)total;
    if (total >= max - 1) return -1;
  }
  return -1;
}

// Reads and counts what arrives into sp->body_bytes until the peer closes
// (sets sp->saw_eof), an error, stop, or ms milliseconds, so a test can see how
// much body the client sent.
static void scripted_peer_drain(scripted_peer *sp, int fd, int ms) {
  char b[16384];
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop);) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&p, 1, 50);
    if (rc == 0) {
      waited += 50;
      continue;
    }
    if (rc < 0) return;
    ssize_t n = recv(fd, b, sizeof(b), 0);
    if (n == 0) {
      atomic_store(&sp->saw_eof, 1);
      return;
    }
    if (n < 0) return;
    atomic_fetch_add(&sp->body_bytes, (long long)n);
  }
}

// Waits until stop or for ms milliseconds without reading, for scripts that
// must not take the client's bytes.
static void scripted_peer_idle(scripted_peer *sp, int ms) {
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop); waited += 20)
    usleep(20000);
}

// Waits up to ms for the test to raise sp->phase to at least phase, so a
// script can step in time with the test. Gives false otherwise.
static bool scripted_peer_wait_phase(scripted_peer *sp, int phase, int ms) {
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop); waited += 5) {
    if (atomic_load(&sp->phase) >= phase) return true;
    usleep(5000);
  }
  return atomic_load(&sp->phase) >= phase;
}

// Sends the string s on fd, stopping at the first error.
static void scripted_peer_send(int fd, const char *s) {
  size_t len = strlen(s), off = 0;
  while (off < len) {
    ssize_t n = send(fd, s + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += (size_t)n;
  }
}

// Waits up to ms for sp->saw_eof, so a test can check that the client closed
// the connection.
static bool scripted_peer_wait_eof(scripted_peer *sp, int ms) {
  for (int waited = 0; waited < ms; waited += 5) {
    if (atomic_load(&sp->saw_eof)) return true;
    usleep(5000);
  }
  return atomic_load(&sp->saw_eof) != 0;
}

// Runs req through Tier 1, 2 (through the future) or 3, as tier says, so the
// scripted tests can run one body in every tier. *resp_out gets the response if
// there is one.
static ccol_retval scripted_do(chttpcli cli, const chttp_request *req, int tier,
                               chttpcli_response **resp_out) {
  *resp_out = NULL;
  if (tier == 1) return chttpcli_do(cli, req, resp_out);
  if (tier == 3) return chttpcli_do_pooled(cli, req, resp_out);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  if (!f) return ccol_unexpected_failure;
  chttpcli_async_result *raw = chttpcli_async_result_get(f);
  ccol_retval rv = ccol_unexpected_failure;
  if (raw) {
    rv = raw->rv;
    *resp_out = raw->resp;
    chttpcli_async_result_free(raw);
  }
  cthrpool_future_free(f);
  return rv;
}

// Gives ssl the new write BIO wbio and keeps its read BIO, so a script can
// control what the TLS layer writes. OpenSSL knows when the old write BIO is
// also the read BIO; LibreSSL frees the old write BIO whatever it is, so there
// the read BIO first gets a reference of its own.
static void tls_replace_wbio(SSL *ssl, BIO *wbio) {
  BIO *rbio = SSL_get_rbio(ssl);
#if defined(LIBRESSL_VERSION_NUMBER)
  if (rbio == SSL_get_wbio(ssl)) BIO_up_ref(rbio);
#endif
  SSL_set_bio(ssl, rbio, wbio);
}

// A TLS peer that completes the handshake, reads the start of the request,
// sends the first 8 bytes of one record and never reads again. The client then
// holds the start of a record no read can finish while its send has no room.
static void scripted_tls_partial_record(scripted_peer *sp, int fd, int index) {
  (void)index;
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
  if (!ssl) return;
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) == 1) {
    char b[4096];
    (void)SSL_read(ssl, b, sizeof(b));
    BIO *mem = BIO_new(BIO_s_mem());
    if (mem) {
      BIO_up_ref(mem);
      tls_replace_wbio(ssl, mem);
      const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
      (void)SSL_write(ssl, resp, (int)strlen(resp));
      char *rec = NULL;
      long rec_len = BIO_get_mem_data(mem, &rec);
      if (rec_len > 8) (void)send(fd, rec, 8, MSG_NOSIGNAL);
      atomic_store(&sp->phase, 1);
      scripted_peer_idle(sp, 10000);
      BIO_free(mem);
    }
  }
  SSL_free(ssl);
}

// Gives the CPU time of the calling thread, so
// tier1_upload_waits_without_spinning can tell a wait from a spin.
static double thread_cpu_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// A send loop that trusts the TLS layer's pending input after a read that
// blocked spins for the whole 2 s and burns about 2 s of CPU.
TEST(tls_partial_record, tier1_upload_waits_without_spinning) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  REQUIRE_EQ(scripted_peer_start(&sp, scripted_tls_partial_record, 4096), 0);

  chttpcli cli = chttpcli_create(NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval set_rv = chttpcli_set_tls(cli, &tls);
  (void)chttpcli_set_request_timeout(cli, 2000000);

  size_t body_len = 8u << 20;
  char *body = (char *)calloc(1, body_len);
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/upload", sp.port);
  chttp_request *req =
      body ? chttp_request_create(CHTTP_POST, url,
                                  &CHTTP_BODY(body, body_len, "x/y"), NULL)
           : NULL;
  chttpcli_response *resp = NULL;
  ccol_retval rv = ccol_unexpected_failure;
  double cpu = 0;
  if (req) {
    double c0 = thread_cpu_seconds();
    rv = chttpcli_do(cli, req, &resp);
    cpu = thread_cpu_seconds() - c0;
  }
  int phase = atomic_load(&sp.phase);
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  scripted_peer_stop(&sp);
  free(body);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(phase, 1);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(cpu, 1.0);
}

// The body of the partial-record upload test, large enough to need many
// sends.
#define TLS_PARTIAL_RECORD_BODY_LEN (1u << 20)

// A TLS peer that sends the first 8 bytes of its response record while it
// keeps reading, and the rest once the whole body is in, so the upload must go
// on while the client holds a partial record.
static void scripted_tls_partial_record_reading(scripted_peer *sp, int fd,
                                                int index) {
  (void)index;
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
  if (!ssl) return;
  SSL_set_fd(ssl, fd);
  BIO *mem = NULL;
  if (SSL_accept(ssl) == 1 && (mem = BIO_new(BIO_s_mem())) != NULL) {
    BIO_up_ref(mem);
    tls_replace_wbio(ssl, mem);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    (void)SSL_write(ssl, resp, (int)strlen(resp));
    char *rec = NULL;
    long rec_len = BIO_get_mem_data(mem, &rec);
    if (rec_len > 8 && send(fd, rec, 8, MSG_NOSIGNAL) == 8) {
      char b[16384];
      long long body = -1;
      while (!atomic_load(&sp->stop)) {
        int n = SSL_read(ssl, b, sizeof(b));
        if (n <= 0) break;
        if (body < 0) {
          size_t take = (size_t)n < sizeof(b) - 1 ? (size_t)n : sizeof(b) - 1;
          b[take] = '\0';
          char *end = strstr(b, "\r\n\r\n");
          if (end) body = (long long)((size_t)n - (size_t)(end + 4 - b));
        } else {
          body += n;
        }
        if (body >= (long long)TLS_PARTIAL_RECORD_BODY_LEN) break;
      }
      atomic_store(&sp->body_bytes, body);
      if (body >= (long long)TLS_PARTIAL_RECORD_BODY_LEN)
        (void)send(fd, rec + 8, (size_t)(rec_len - 8), MSG_NOSIGNAL);
      scripted_peer_idle(sp, 10000);
    }
  }
  SSL_free(ssl);
  if (mem) BIO_free(mem);
}

// An engine that takes the start of a record as input to read pauses the body
// for a read that can't finish, and the request times out.
TEST(tls_partial_record, upload_goes_on_while_the_peer_reads) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(
        scripted_peer_start(&sp, scripted_tls_partial_record_reading, 4096), 0);
    chttpcli cli = chttpcli_create(NULL);
    chttp_tls_config tls = CHTTP_TLS_DEFAULT;
    tls.ca_bundle_path = g_tls_cert_path;
    ccol_retval set_rv = chttpcli_set_tls(cli, &tls);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    char *body = (char *)calloc(1, TLS_PARTIAL_RECORD_BODY_LEN);
    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/upload", sp.port);
    chttp_request *req =
        body ? chttp_request_create(
                   CHTTP_POST, url,
                   &CHTTP_BODY(body, TLS_PARTIAL_RECORD_BODY_LEN, "x/y"), NULL)
             : NULL;
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    int status = resp ? resp->status_code : -1;
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    scripted_peer_stop(&sp);
    long long got = atomic_load(&sp.body_bytes);
    free(body);

    REQUIRE_EQ(set_rv, ccol_success);
    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_EQ(got, (long long)TLS_PARTIAL_RECORD_BODY_LEN);
  }
}

// A peer that answers the head with "100 Continue" plus a complete final
// response in one send(), then counts every byte until the client closes.
static void scripted_continue_with_final(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  const char *end = strstr(head, "\r\n\r\n");
  if (end) {
    size_t extra = strlen(end + 4);
    atomic_fetch_add(&sp->body_bytes, (long long)extra);
  }
  scripted_peer_send(fd,
                     "HTTP/1.1 100 Continue\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  scripted_peer_drain(sp, fd, 10000);
}

// The final response that came with the 100 answers the request, so the body
// is never sent and the connection isn't pooled.
TEST(expect_continue_hardening, final_response_with_the_100_stops_the_body) {
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_continue_with_final, 0), 0);

    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    size_t body_len = 256u * 1024u;
    char *body = (char *)calloc(1, body_len);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/upload", sp.port);
    chttp_request *req =
        body ? chttp_request_create(CHTTP_POST, url,
                                    &CHTTP_BODY(body, body_len, "x/y"), NULL)
             : NULL;
    if (req) req->expect_continue = true;
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    int status = resp ? resp->status_code : -1;
    bool body_ok = resp && resp->body && strcmp(resp->body, "ok") == 0;
    bool closed = scripted_peer_wait_eof(&sp, 5000);
    long long sent = atomic_load(&sp.body_bytes);
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    scripted_peer_stop(&sp);
    free(body);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_TRUE(body_ok);
    REQUIRE_TRUE(closed);
    REQUIRE_EQ(sent, 0LL);
  }
}

// A peer that answers with an unsolicited 101 followed by bytes that read as an
// HTTP response, which after a 101 belong to another protocol.
static void scripted_switching_protocols(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  scripted_peer_send(fd,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Connection: upgrade\r\nUpgrade: websocket\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  scripted_peer_drain(sp, fd, 10000);
}

// A client that drops a 101 like any interim response reads the bytes after it
// as the final response and reports a 200.
TEST(interim_responses, unsolicited_101_fails_every_tier) {
  for (int variant = 0; variant < 6; variant++) {
    int tier = 1 + variant % 3;
    bool with_continue = variant >= 3;
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_switching_protocols, 0), 0);

    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", sp.port);
    const char *payload = "payload";
    chttp_request *req =
        with_continue ? chttp_request_create(
                            CHTTP_POST, url,
                            &CHTTP_TEXT_BODY(payload, strlen(payload)), NULL)
                      : chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (req) req->expect_continue = with_continue;
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    bool got_resp = resp != NULL;
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    int requests = atomic_load(&sp.requests);
    scripted_peer_stop(&sp);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(requests, 1);
    REQUIRE_EQ(rv, ccol_http_transfer_aborted);
    REQUIRE_FALSE(got_resp);
  }
}

// A peer that answers with a gzip-coded body: index 0 names gzip and chunked on
// one line, index 1 sends gzip alone, framed by the close.
static void scripted_other_transfer_coding(scripted_peer *sp, int fd,
                                           int index) {
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  int variant = atomic_load(&sp->phase);
  (void)index;
  if (variant == 0)
    scripted_peer_send(fd,
                       "HTTP/1.1 200 OK\r\n"
                       "Transfer-Encoding: gzip, chunked\r\n\r\n"
                       "3\r\nabc\r\n0\r\n\r\n");
  else
    scripted_peer_send(fd,
                       "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
                       "Connection: close\r\n\r\nabc");
  if (variant != 0) return;
  scripted_peer_drain(sp, fd, 10000);
}

TEST(transfer_codings, a_coding_other_than_chunked_fails_every_tier) {
  for (int variant = 0; variant < 6; variant++) {
    int tier = 1 + variant % 3;
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    atomic_store(&sp.phase, variant / 3);
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_other_transfer_coding, 0), 0);

    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/z", sp.port);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    bool got_resp = resp != NULL;
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    scripted_peer_stop(&sp);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_http_transfer_aborted);
    REQUIRE_FALSE(got_resp);
  }
}

// An iterator over the request headers that cannot be allocated
//
// An allocator whose calloc fails at index g_iter_oom_fail_at while
// g_iter_oom_armed is set, for the header iterator tests.
static _Atomic int g_iter_oom_armed, g_iter_oom_seen, g_iter_oom_fail_at;
static void *iter_oom_malloc(size_t n) { return malloc(n); }
static void iter_oom_free(void *p) { free(p); }
static void *iter_oom_realloc(void *p, size_t n) { return realloc(p, n); }
static void *iter_oom_calloc(size_t a, size_t b) {
  if (atomic_load(&g_iter_oom_armed) &&
      atomic_fetch_add(&g_iter_oom_seen, 1) + 1 ==
          atomic_load(&g_iter_oom_fail_at))
    return NULL;
  return calloc(a, b);
}

// An allocator that counts every allocation in g_hn_allocs, so the header
// lookup test can check that a short name allocates nothing.
static atomic_int g_hn_allocs = 0;
static void *hn_malloc(size_t n) {
  atomic_fetch_add(&g_hn_allocs, 1);
  return malloc(n);
}
static void *hn_calloc(size_t n, size_t s) {
  atomic_fetch_add(&g_hn_allocs, 1);
  return calloc(n, s);
}
static void *hn_realloc(void *p, size_t n) {
  atomic_fetch_add(&g_hn_allocs, 1);
  return realloc(p, n);
}

TEST(request_headers, a_short_name_is_looked_up_without_an_allocation) {
  ccol_memmgmt_procs mp = {.malloc = hn_malloc,
                           .calloc = hn_calloc,
                           .realloc = hn_realloc,
                           .free = free};
  chttp_request *req =
      chttp_request_create_mp(CHTTP_GET, "http://127.0.0.1/", NULL, &mp, NULL);
  REQUIRE_NE((void *)req, NULL);
  char long_name[300];
  memset(long_name, 'X', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';
  ccol_retval set1 = chttp_request_set_header(req, "X-Trace-Id", "a");
  ccol_retval set_long = chttp_request_set_header(req, long_name, "b");
  int before = atomic_load(&g_hn_allocs);
  const char *v1 = chttp_request_get_header(req, "x-trace-ID");
  bool v1_ok = v1 && strcmp(v1, "a") == 0;
  const char *v2 = chttp_request_get_header(req, "X-TRACE-ID");
  bool v2_ok = v2 && strcmp(v2, "a") == 0;
  const char *v3 = chttp_request_get_header(req, "x-absent");
  ccol_retval set2 = chttp_request_set_header(req, "X-TRACE-ID", "c");
  int lookups = atomic_load(&g_hn_allocs) - before;
  const char *v4 = chttp_request_get_header(req, "x-trace-id");
  char long_lower[300];
  memset(long_lower, 'x', sizeof(long_lower) - 1);
  long_lower[sizeof(long_lower) - 1] = '\0';
  const char *vl = chttp_request_get_header(req, long_lower);
  bool v4_ok = v4 && strcmp(v4, "c") == 0;
  bool vl_ok = vl && strcmp(vl, "b") == 0;
  chttp_request_free(req);
  REQUIRE_EQ(set1, ccol_success);
  REQUIRE_EQ(set_long, ccol_success);
  REQUIRE_EQ(set2, ccol_success);
  REQUIRE_TRUE(v1_ok);
  REQUIRE_TRUE(v2_ok);
  REQUIRE_EQ((void *)v3, NULL);
  REQUIRE_TRUE(v4_ok);
  REQUIRE_TRUE(vl_ok);
  REQUIRE_EQ(lookups, 0);
}

// The serializer walks the headers twice, and each walk allocates an iterator.
// If either fails the request must not go out, because it would be malformed
// (a body without Content-Length, or two of them).
TEST(request_headers, a_failed_header_iterator_fails_the_request) {
  static ccol_memmgmt_procs mp = {iter_oom_malloc, iter_oom_free,
                                  iter_oom_calloc, iter_oom_realloc};
  for (int fail_at = 1; fail_at <= 2; fail_at++) {
    chttpcli_construct(cli);
    char url[160];
    make_url(url, sizeof(url), "/post");
    char *err = NULL;
    chttp_request *req = chttp_request_create_mp(
        CHTTP_POST, url, &CHTTP_TEXT_BODY("hello", 5), &mp, &err);
    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(chttp_request_set_header(req, "authorization", "Bearer x"),
               ccol_success);
    chttpcli_response *resp = NULL;
    atomic_store(&g_iter_oom_seen, 0);
    atomic_store(&g_iter_oom_fail_at, fail_at);
    atomic_store(&g_iter_oom_armed, 1);
    ccol_retval rv = chttpcli_do(cli, req, &resp);
    atomic_store(&g_iter_oom_armed, 0);
    int seen = atomic_load(&g_iter_oom_seen);
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    REQUIRE_GE(seen, fail_at);
    REQUIRE_EQ(rv, ccol_not_enough_memory);
  }
}

// Whichever allocation fails, the request fails or carries the caller's header;
// a copy that read a failed iterator as an empty map would send a 200 with an
// empty echo.
TEST(request_headers, an_async_request_keeps_its_headers_or_fails) {
  static ccol_memmgmt_procs mp = {iter_oom_malloc, iter_oom_free,
                                  iter_oom_calloc, iter_oom_realloc};
  char url[160];
  make_url(url, sizeof(url), "/echo-header");
  int lost = 0, succeeded = 0, failed_cleanly = 0;
  for (int fail_at = 1; fail_at <= 8; fail_at++) {
    char *err = NULL;
    chttpcli cli = chttpcli_create(&err);
    if (cli == CHTTPCLI_NULL) continue;
    chttp_request *req =
        chttp_request_create_mp(CHTTP_GET, url, NULL, &mp, &err);
    if (req) (void)chttp_request_set_header(req, "x-echo", "kept");
    atomic_store(&g_iter_oom_seen, 0);
    atomic_store(&g_iter_oom_fail_at, fail_at);
    atomic_store(&g_iter_oom_armed, 1);
    cthrpool_future *f = req ? chttpcli_do_async(cli, req) : NULL;
    chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
    atomic_store(&g_iter_oom_armed, 0);
    if (raw && raw->rv == ccol_success && raw->resp) {
      succeeded++;
      if (!raw->resp->body || strcmp(raw->resp->body, "kept") != 0) lost++;
    } else {
      failed_cleanly++;
    }
    if (raw) {
      chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    if (f) cthrpool_future_free(f);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
  }
  REQUIRE_GT(succeeded, 0);
  REQUIRE_EQ(lost, 0);
  (void)failed_cleanly;
}

// The walk over the idle pool allocates nothing, so every pooled socket is
// closed even when every allocation fails.
TEST(request_headers, destroy_closes_pooled_connections_without_memory) {
  static ccol_memmgmt_procs mp = {iter_oom_malloc, iter_oom_free,
                                  iter_oom_calloc, iter_oom_realloc};
  char url[160];
  make_url(url, sizeof(url), "/keepalive");
  {
    chttpcli warm = chttpcli_create(NULL);
    chttp_request *wreq = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *wresp = NULL;
    if (warm != CHTTPCLI_NULL && wreq) (void)chttpcli_do(warm, wreq, &wresp);
    chttpcli_resp_free(wresp);
    chttp_request_free(wreq);
    if (warm != CHTTPCLI_NULL) chttpcli_destroy(warm);
  }
  int fds_before = settled_fd_baseline();
  char *err = NULL;
  chttpcli cli = chttpcli_create_mp(&mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv = req ? chttpcli_do(cli, req, &resp) : ccol_unexpected_failure;
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  atomic_store(&g_iter_oom_seen, 0);
  atomic_store(&g_iter_oom_fail_at, 1);
  atomic_store(&g_iter_oom_armed, 1);
  chttpcli_destroy(cli);
  atomic_store(&g_iter_oom_armed, 0);
  wait_for_async_engine_idle();
  int fds_after = settle_open_fds(fds_before);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(fds_after, fds_before);
}

// A body framed by the close ends there over https too when the server closes
// without close_notify (curl and Go accept it), while a close inside a record
// is a truncation.
TEST(tls_close_without_notify, ends_a_body_at_a_record_boundary_only) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  for (int variant = 0; variant < 6; variant++) {
    int tier = 1 + variant % 3;
    bool mid_record = variant >= 3;
    chttpcli cli = chttpcli_create(NULL);
    chttp_tls_config tls = CHTTP_TLS_DEFAULT;
    tls.ca_bundle_path = g_tls_cert_path;
    ccol_retval set_rv = chttpcli_set_tls(cli, &tls);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    char url[160];
    make_tls_url(url, sizeof(url),
                 mid_record ? "/eof-mid-record" : "/eof-no-notify");
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    bool body_ok = resp && resp->status_code == 200 && resp->body &&
                   strcmp(resp->body, "eof-body-ok") == 0;
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    REQUIRE_EQ(set_rv, ccol_success);
    if (mid_record) {
      REQUIRE_EQ(rv, ccol_http_transfer_aborted);
    } else {
      REQUIRE_EQ(rv, ccol_success);
      REQUIRE_TRUE(body_ok);
    }
  }
}

TEST(url_parsing, host_outside_the_uri_host_grammar_is_invalid) {
  static const char *const bad[] = {
      "http://a b/",
      "http://a\tb:80/",
      "http://h\x01/",
      "http://h\x7f/",
      "http://ex\xc3\xa4mple.com/",
      "http://a\\b/",
      "http://a\"b/",
      "http://a<b>/",
      "http://a{b}/",
      "http://a^b/",
      "http://a|b/",
      "http://a`b/",
      "http://a%/",
      "http://a%4/",
      "http://a%zz/",
      "http://[evil/x]/y",
      "http://[evil?x]/",
      "http://[evil#x]/",
      "http://[::1%25lo]/",
      "http://[::1%lo]/",
      "http://[127.0.0.1]/",
      "http://[::1 ]/",
      "http://[::g]/",
      "http://[1:2:3:4:5:6:7:8:9]/",
      "http://[v]/",
      "http://[v1]/",
      "http://[v1.]/",
      "http://[vx.a]/",
      "http://[v1.a/b]/",
      "https://a\r\nb/",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *host = NULL;
    ccol_retval rv = chttp__parse_url_for_tests(bad[i], NULL, NULL, &host, NULL,
                                                NULL, NULL, NULL, NULL, NULL);
    bool refused = (rv == ccol_http_invalid_url) && host == NULL;
    free(host);
    if (!refused) fprintf(stderr, "accepted: \"%s\"\n", bad[i]);
    REQUIRE_TRUE(refused);
  }
}

TEST(url_parsing, every_uri_host_form_is_accepted_as_written) {
  static const struct {
    const char *url;
    const char *host;
    bool ipv6;
  } good[] = {
      {"http://example.com/", "example.com", false},
      {"http://Ex-am_ple.~co/", "Ex-am_ple.~co", false},
      {"http://a!$&'()*+,;=b/", "a!$&'()*+,;=b", false},
      {"http://xn--exmple-cua.com/", "xn--exmple-cua.com", false},
      {"http://a%41b/", "a%41b", false},
      {"http://10.1.2.3:8080/", "10.1.2.3", false},
      {"http://localhost./", "localhost.", false},
      {"http://[::1]/", "::1", true},
      {"http://[::ffff:192.0.2.1]/", "::ffff:192.0.2.1", true},
      {"http://[2001:DB8::a:1]:81/", "2001:DB8::a:1", true},
      {"http://[v1.fe80::a+en1]/", "v1.fe80::a+en1", true},
      {"http://[VF.x!$:]/", "VF.x!$:", true},
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    char *host = NULL, *pq = NULL, *key = NULL;
    bool ipv6 = !good[i].ipv6;
    ccol_retval rv = chttp__parse_url_for_tests(
        good[i].url, NULL, &ipv6, &host, NULL, &pq, &key, NULL, NULL, NULL);
    bool ok = rv == ccol_success && host && strcmp(host, good[i].host) == 0 &&
              ipv6 == good[i].ipv6;
    free(host);
    free(pq);
    free(key);
    if (!ok) fprintf(stderr, "refused or changed: \"%s\"\n", good[i].url);
    REQUIRE_TRUE(ok);
  }
}

TEST(url_parsing, userinfo_outside_the_grammar_is_invalid) {
  static const char *const bad[] = {
      "http://good.example\\@evil.example/",
      "http://u ser:p@h/",
      "http://u:p\tw@h/",
      "http://u:p\x01@h/",
      "http://u:p\xc3\xa4@h/",
      "http://u:p[w]@h/",
      "http://u:p\"w@h/",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    ccol_retval rv = chttp__parse_url_for_tests(bad[i], NULL, NULL, NULL, NULL,
                                                NULL, NULL, NULL, NULL, NULL);
    if (rv != ccol_http_invalid_url)
      fprintf(stderr, "accepted: \"%s\"\n", bad[i]);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
  }
  char *auth = NULL, *host = NULL;
  ccol_retval rv = chttp__parse_url_for_tests(
      "http://a-._~!$&'()*+,;=:p@ss%C3%A4@h/", NULL, NULL, &host, NULL, NULL,
      NULL, &auth, NULL, NULL);
  bool host_ok = host && strcmp(host, "h") == 0;
  free(host);
  free(auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(host_ok);
}

// A peer that redirects /start to a Location whose host is a bracketed IPv4
// address (not an IP-literal) naming the peer itself, and answers every other
// path with 200, for location_host_outside_the_grammar_is_invalid.
static void scripted_bad_location(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  if (strncmp(head, "GET /start ", 11) == 0) {
    char resp[256];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 302 Found\r\nLocation: http://[127.0.0.1]:%d/next\r\n"
             "Content-Length: 0\r\nConnection: close\r\n\r\n",
             sp->port);
    scripted_peer_send(fd, resp);
  } else {
    scripted_peer_send(fd,
                       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                       "Connection: close\r\n\r\nok");
  }
  scripted_peer_drain(sp, fd, 2000);
}

// A parser that takes everything up to the ']' as the host connects to
// 127.0.0.1 and follows the redirect to a 200.
TEST(redirect_hardening, location_host_outside_the_grammar_is_invalid) {
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_bad_location, 0), 0);
    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", sp.port);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    chttpcli_resp_free(resp);
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    int requests = atomic_load(&sp.requests);
    scripted_peer_stop(&sp);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
    REQUIRE_EQ(requests, 1);
  }
}

// A peer for the stray input tests. The first connection answers "first" and
// stays open; once the test raises the phase to 1 it sends sp->stray on that
// idle connection and answers any further request with "reuse". Every later
// connection answers "fresh".
static void scripted_stray_after_response(scripted_peer *sp, int fd,
                                          int index) {
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  if (index != 0) {
    scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh");
    scripted_peer_drain(sp, fd, 10000);
    return;
  }
  scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
  if (!scripted_peer_wait_phase(sp, 1, 10000)) return;
  scripted_peer_send(fd, sp->stray);
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nreuse");
  scripted_peer_drain(sp, fd, 10000);
}

// Runs the order where a take wins the race, with stray as the input that
// shows up on the pooled connection. Gives both codes, whether the hold
// engaged, the second body and the number of connections.
static void run_stray_input_case(const char *stray, ccol_retval *rv1_out,
                                 ccol_retval *rv2_out, bool *held_out,
                                 char *body2, size_t body2_len,
                                 int *conns_out) {
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  sp.stray = stray;
  *rv1_out = *rv2_out = ccol_unexpected_failure;
  *held_out = false;
  body2[0] = '\0';
  *conns_out = -1;
  if (scripted_peer_start(&sp, scripted_stray_after_response, 0) != 0) return;

  chttpcli cli = chttpcli_create(NULL);
  (void)chttpcli_set_request_timeout(cli, 10000000);
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/get", sp.port);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  if (req) *rv1_out = scripted_do(cli, req, 3, &resp);
  chttpcli_resp_free(resp);
  resp = NULL;

  chttpcli__arm_idle_probe_hold_for_tests();
  atomic_store(&sp.phase, 1);
  *held_out = chttpcli__wait_idle_probe_held_for_tests(5000);
  cthrpool_future *f = req ? chttpcli_do_async(cli, req) : NULL;
  chttpcli__release_idle_probe_hold_for_tests();
  if (f) {
    chttpcli_async_result *raw = chttpcli_async_result_get(f);
    if (raw) {
      *rv2_out = raw->rv;
      if (raw->resp && raw->resp->body)
        snprintf(body2, body2_len, "%s", raw->resp->body);
      chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(f);
  }
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  scripted_peer_stop(&sp);
  *conns_out = atomic_load(&sp.nconn);
}

// A hop that takes a pooled connection before the dispatch that would evict it
// must not read the stray input as its response; a hop that writes on that
// connection fails with ccol_http_transfer_aborted.
TEST(async_idle_pool, stray_input_on_a_taken_connection_is_never_read) {
  ccol_retval rv1, rv2;
  bool held;
  char body2[32];
  int conns;
  run_stray_input_case("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstray",
                       &rv1, &rv2, &held, body2, sizeof(body2), &conns);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_STREQ(body2, "fresh");
  REQUIRE_EQ(conns, 2);
}

// The dispatch that found the connection idle read the one stray byte itself;
// without the mark it leaves, the hop finds an empty socket and gets
// "reuse".
TEST(async_idle_pool, a_stray_byte_that_the_idle_dispatch_took_still_evicts) {
  ccol_retval rv1, rv2;
  bool held;
  char body2[32];
  int conns;
  run_stray_input_case("x", &rv1, &rv2, &held, body2, sizeof(body2), &conns);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_STREQ(body2, "fresh");
  REQUIRE_EQ(conns, 2);
}

// Redirect resolution, growth, self-destroy and stalled hops
//
// An allocator that fails exactly the allocation at index g_resolve_fail_at, on
// any thread, and records in g_resolve_failed that it did, for the redirect
// resolution sweep.
static atomic_int g_resolve_alloc_index;
static atomic_int g_resolve_fail_at;
static atomic_bool g_resolve_failed;

// Counts one allocation and tells whether it's the one to fail.
static bool resolve_fail_now(void) {
  if (!chttp__in_redirect_resolve_for_tests()) return false;
  int idx = atomic_fetch_add(&g_resolve_alloc_index, 1);
  if (idx != atomic_load(&g_resolve_fail_at)) return false;
  atomic_store(&g_resolve_failed, true);
  return true;
}
static void *resolve_fail_malloc(size_t sz) {
  return resolve_fail_now() ? NULL : malloc(sz);
}
static void *resolve_fail_calloc(size_t n, size_t sz) {
  return resolve_fail_now() ? NULL : calloc(n, sz);
}
static void *resolve_fail_realloc(void *p, size_t sz) {
  return resolve_fail_now() ? NULL : realloc(p, sz);
}
static void resolve_fail_free(void *p) { free(p); }
static ccol_memmgmt_procs g_resolve_fail_mp = {.malloc = resolve_fail_malloc,
                                               .free = resolve_fail_free,
                                               .calloc = resolve_fail_calloc,
                                               .realloc = resolve_fail_realloc};

// A resolution that reported a failed allocation like an empty Location would
// make every tier give ccol_http_transfer_aborted.
TEST(redirect_resolution, a_failed_allocation_reports_not_enough_memory) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-relative");
  char *cerr = NULL;
  chttpcli cli = chttpcli_create_mp(&g_resolve_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_NULL);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  int failed_runs[3] = {0, 0, 0};
  bool ended_in_success[3] = {false, false, false};
  bool all_failures_oom = true;
  for (int tier = 0; tier < 3; tier++) {
    for (int idx = 0; idx < 40; idx++) {
      atomic_store(&g_resolve_alloc_index, 0);
      atomic_store(&g_resolve_failed, false);
      atomic_store(&g_resolve_fail_at, idx);
      ccol_retval rv = ccol_unexpected_failure;
      chttpcli_response *resp = NULL;
      if (tier == 0) {
        rv = chttpcli_do(cli, req, &resp);
      } else if (tier == 1) {
        cthrpool_future *f = chttpcli_do_async(cli, req);
        chttpcli_async_result *r = f ? chttpcli_async_result_get(f) : NULL;
        if (r) {
          rv = r->rv;
          resp = r->resp;
          chttpcli_async_result_free(r);
        }
        if (f) cthrpool_future_free(f);
      } else {
        rv = chttpcli_do_pooled(cli, req, &resp);
      }
      atomic_store(&g_resolve_fail_at, -1);
      int status = (rv == ccol_success && resp) ? resp->status_code : -1;
      chttpcli_resp_free(resp);
      if (atomic_load(&g_resolve_failed)) {
        failed_runs[tier]++;
        if (rv != ccol_not_enough_memory) all_failures_oom = false;
        continue;
      }
      ended_in_success[tier] = (rv == ccol_success && status == 200);
      break;
    }
  }
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_TRUE(all_failures_oom);
  for (int tier = 0; tier < 3; tier++) {
    REQUIRE_GE(failed_runs[tier], 3);
    REQUIRE_TRUE(ended_in_success[tier]);
  }
}

// An allocator that fails the allocation at index g_tc_fail_at and counts every
// allocation, for the transport check sweep. The check runs only on the test
// thread, so plain ints do.
static int g_tc_alloc_index;
static int g_tc_fail_at;
static bool g_tc_failed;
// Counts one allocation and tells whether it's the one to fail.
static bool tc_fail_now(void) {
  int idx = g_tc_alloc_index++;
  if (idx != g_tc_fail_at) return false;
  g_tc_failed = true;
  return true;
}
static void *tc_fail_malloc(size_t sz) {
  return tc_fail_now() ? NULL : malloc(sz);
}
static void *tc_fail_calloc(size_t n, size_t sz) {
  return tc_fail_now() ? NULL : calloc(n, sz);
}
static void *tc_fail_realloc(void *p, size_t sz) {
  return tc_fail_now() ? NULL : realloc(p, sz);
}
static ccol_memmgmt_procs g_tc_fail_mp = {.malloc = tc_fail_malloc,
                                          .free = free,
                                          .calloc = tc_fail_calloc,
                                          .realloc = tc_fail_realloc};

// A parse that fails for lack of memory must never let a hop through, or a
// prevented downgrade or a unix target would go out.
TEST(redirect_resolution, a_transport_check_without_memory_never_allows) {
  static const struct {
    const char *base;
    const char *next;
    bool prevent;
    ccol_retval verdict;
  } cases[] = {
      {"https://example.com/a", "http://example.com/b", true,
       ccol_http_invalid_url},
      {"https://example.com/a", "https://other.example/b", true, ccol_success},
      {"http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2Ftmp%2Fy.sock/b", false,
       ccol_http_invalid_url},
      {"http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2Ftmp%2Fx.sock/b", false,
       ccol_success},
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    int failed_runs = 0;
    bool all_oom = true;
    ccol_retval final_rv = ccol_unexpected_failure;
    for (int idx = 0; idx < 64; idx++) {
      g_tc_alloc_index = 0;
      g_tc_fail_at = idx;
      g_tc_failed = false;
      ccol_retval rv = chttp__redirect_transport_check_for_tests(
          &g_tc_fail_mp, cases[c].base, cases[c].next, cases[c].prevent);
      if (g_tc_failed) {
        failed_runs++;
        if (rv != ccol_not_enough_memory) all_oom = false;
        continue;
      }
      final_rv = rv;
      break;
    }
    REQUIRE_GE(failed_runs, 1);
    REQUIRE_TRUE(all_oom);
    REQUIRE_EQ((int)final_rv, (int)cases[c].verdict);
  }
}

// Runs the growth helper `which` from a capacity that can't double, in a child,
// so a growth that never ends fails the test instead of hanging it. Gives true
// when the child reported, through a pipe, that the helper failed after exactly
// one realloc.
static bool growth_from_cap_fails_cleanly(int which) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    atomic_store(&g_ob_overflow_alloc_calls, 0);
    size_t cap = SIZE_MAX / 2 + 1;
    bool oom = which == 0 ? chttp__ob_append_cap_overflow_for_tests(
                                &g_ob_overflow_never_mp, cap)
                          : chttp__sink_buffered_cap_overflow_for_tests(
                                &g_ob_overflow_never_mp, cap);
    unsigned char ok =
        (oom && atomic_load(&g_ob_overflow_alloc_calls) == 1) ? 1 : 0;
    ssize_t w = write(fds[1], &ok, 1);
    test_child_exit(w == 1 ? 0 : 1);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return false;
  }
  int status = 0;
  bool ended = test_wait_child_bounded(pid, 10000, &status);
  unsigned char ok = 0;
  ssize_t r = ended ? read(fds[0], &ok, 1) : 0;
  close(fds[0]);
  return ended && r == 1 && ok == 1;
}

// A capacity that doubles without a check wraps to 0 and the growth loop never
// ends; on ILP32 that's a 2 GiB buffer.
TEST(request_serialization, growth_past_half_of_size_max_never_wraps) {
  REQUIRE_TRUE(growth_from_cap_fails_cleanly(0));
  REQUIRE_TRUE(growth_from_cap_fails_cleanly(1));
}

// The client that self_destroy_write_fn destroys.
typedef struct {
  chttpcli cli;
} self_destroy_ctx_t;

// A write_fn that destroys its own client, which must be fatal.
static size_t self_destroy_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  self_destroy_ctx_t *c = (self_destroy_ctx_t *)ctx;
  chttpcli__destroy(c->cli);
  return len;
}

// Runs a streaming request whose write_fn destroys its own client in a child,
// through Tier 1 (tier 0), Tier 2 (1) or Tier 3 (2). The child's stderr goes
// into msg. Gives true when the child died of SIGABRT.
static bool self_destroy_aborts(int tier, char *msg, size_t msg_size) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    char url[160];
    make_url(url, sizeof(url), "/get");
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) test_child_exit(2);
    self_destroy_ctx_t c = {.cli = cli};
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (!req) test_child_exit(3);
    int status_code = 0;
    if (tier == 0) {
      (void)chttpcli_do_streaming(cli, req, self_destroy_write_fn, &c,
                                  &status_code);
    } else if (tier == 1) {
      cthrpool_future *f =
          chttpcli_do_async_streaming(cli, req, self_destroy_write_fn, &c);
      if (f) (void)chttpcli_async_result_get(f);
    } else {
      (void)chttpcli_do_pooled_streaming(cli, req, self_destroy_write_fn, &c,
                                         &status_code);
    }
    test_child_exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return false;
  }
  int status = 0;
  bool ended = test_wait_child_bounded(pid, 30000, &status);
  size_t got = 0;
  int fl = fcntl(fds[0], F_GETFL);
  fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
  ssize_t n;
  while (got + 1 < msg_size &&
         (n = read(fds[0], msg + got, msg_size - 1 - got)) > 0)
    got += (size_t)n;
  msg[got] = '\0';
  close(fds[0]);
  return ended && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// Without the check the destroy waits for the request that runs it, and the
// child never ends.
TEST(chttpcli_handle_lifecycle, destroy_from_own_write_fn_is_fatal) {
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  for (int tier = 0; tier < 3; tier++) {
    char msg[1024];
    bool aborted = self_destroy_aborts(tier, msg, sizeof(msg));
    REQUIRE_TRUE(aborted);
    REQUIRE_NE((void *)strstr(msg, "inside a request of the same client"),
               NULL);
  }
}

// Polls f until it's done or ms have passed and tells whether it's done, so the
// stalled-hop tests can check the sweep answered without blocking.
static bool future_done_within(cthrpool_future *f, int ms) {
  for (int waited = 0; waited < ms; waited += 5) {
    if (cthrpool_future_done(f)) return true;
    usleep(5000);
  }
  return cthrpool_future_done(f);
}

// A hop that never gets a dispatch (the reactor's pool can't allocate) still
// ends at the request timeout, because the sweep answers the future itself.
TEST(async_deadline, a_hop_that_gets_no_dispatch_ends_at_the_request_timeout) {
  wait_for_async_engine_idle();
  char url[160];
  make_url(url, sizeof(url), "/held");
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 200000);
  chttp__stall_next_read_for_tests();
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cthrpool_future *f = async_get(cli, url);
  bool done = f && future_done_within(f, 3000);
  long took_ms = elapsed_ms_since(&t0);
  size_t abandoned_while_stalled = chttp__async_hops_abandoned_for_tests();
  bool resumed = chttp__stalled_read_resume_for_tests();
  held_release();
  chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
  ccol_retval rv = raw ? raw->rv : ccol_unexpected_failure;
  if (raw) {
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  if (f) cthrpool_future_free(f);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  size_t abandoned_after = chttp__async_hops_abandoned_for_tests();
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(resumed);
  REQUIRE_TRUE(done);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(took_ms, 3000L);
  REQUIRE_EQ(abandoned_while_stalled, (size_t)1);
  REQUIRE_EQ(abandoned_after, (size_t)0);
}

// The input and the outcome of pooled_stall_thread.
typedef struct {
  chttpcli cli;
  const char *url;
  atomic_bool done;
  ccol_retval rv;
} pooled_stall_arg_t;

// Makes one pooled GET from its own thread and sets done, so
// a_pooled_call_whose_hop_gets_no_dispatch_times_out can watch it.
static void *pooled_stall_thread(void *arg) {
  pooled_stall_arg_t *a = (pooled_stall_arg_t *)arg;
  chttp_request *req = chttp_request_create(CHTTP_GET, a->url, NULL, NULL);
  chttpcli_response *resp = NULL;
  a->rv =
      req ? chttpcli_do_pooled(a->cli, req, &resp) : ccol_unexpected_failure;
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  atomic_store(&a->done, true);
  return NULL;
}

TEST(async_deadline, a_pooled_call_whose_hop_gets_no_dispatch_times_out) {
  wait_for_async_engine_idle();
  char url[160];
  make_url(url, sizeof(url), "/held");
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 200000);
  chttp__stall_next_read_for_tests();
  pooled_stall_arg_t a = {.cli = cli, .url = url, .rv = ccol_success};
  atomic_init(&a.done, false);
  pthread_t tid;
  int create_rv = pthread_create(&tid, NULL, pooled_stall_thread, &a);
  bool done = false;
  for (int waited = 0; create_rv == 0 && waited < 3000 && !done; waited += 5) {
    usleep(5000);
    done = atomic_load(&a.done);
  }
  bool resumed = chttp__stalled_read_resume_for_tests();
  held_release();
  if (create_rv == 0) pthread_join(tid, NULL);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(create_rv, 0);
  REQUIRE_TRUE(resumed);
  REQUIRE_TRUE(done);
  REQUIRE_EQ(a.rv, ccol_timed_out);
}

// The write_fn counters of a_write_fn_never_runs_after_the_sweep_answers. The
// first call blocks longer than the request timeout plus two sweep ticks, so
// the sweep answers while the dispatch is still running it.
typedef struct {
  atomic_int calls;
  atomic_int calls_after_answer;
  atomic_bool answered;
} late_sink_t;

// Counts a call, and a call after the test saw the answer; the first call
// sleeps 800 ms.
static size_t late_sink_write(const void *data, size_t len, void *ctx) {
  (void)data;
  late_sink_t *s = (late_sink_t *)ctx;
  if (atomic_load(&s->answered)) atomic_fetch_add(&s->calls_after_answer, 1);
  if (atomic_fetch_add(&s->calls, 1) == 0) usleep(800000);
  return len;
}

// The caller may free write_ctx once the future is fulfilled, so the sweep
// waits for a running call and no call starts after the answer.
TEST(async_deadline, a_write_fn_never_runs_after_the_sweep_answers) {
  wait_for_async_engine_idle();
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");
  chttpcli_construct(cli);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 200000);
  late_sink_t *s = calloc(1, sizeof(*s));
  REQUIRE_NE((void *)s, NULL);
  cthrpool_future *f = async_get_streaming(cli, url, late_sink_write, s);
  chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
  atomic_store(&s->answered, true);
  ccol_retval rv = raw ? raw->rv : ccol_unexpected_failure;
  if (raw) {
    chttpcli_resp_free(raw->resp);
    chttpcli_async_result_free(raw);
  }
  if (f) cthrpool_future_free(f);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  int calls = atomic_load(&s->calls);
  int late = atomic_load(&s->calls_after_answer);
  free(s);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_EQ(calls, 1);
  REQUIRE_EQ(late, 0);
}

#include <locale.h>

// Protocol text folds case as ASCII only
//
// Switches the process to a Turkish LC_CTYPE, where tolower('I') isn't 'i', so
// the locale_folding tests can show the client never folds protocol text
// through the locale. The whole process switches because the async tiers run on
// the engine's threads. On glibc LOCPATH points at the tests_locale/ that the
// Makefile builds. Gives the earlier locale to hand to cli_tr_locale_leave, or
// NULL when the locale isn't available and the test skips.
static char *cli_tr_locale_enter(void) {
  const char *old_ctype = setlocale(LC_CTYPE, NULL);
  char *saved_ctype = old_ctype ? strdup(old_ctype) : NULL;
  if (!saved_ctype) return NULL;
#if defined(__GLIBC__)
  const char *old_path = getenv("LOCPATH");
  char *saved_path = old_path ? strdup(old_path) : NULL;
  setenv("LOCPATH", "tests_locale", 1);
#endif
  bool entered = setlocale(LC_CTYPE, "tr_TR.UTF-8") != NULL ||
                 setlocale(LC_CTYPE, "tr_TR.utf8") != NULL;
#if defined(__GLIBC__)
  if (saved_path) {
    setenv("LOCPATH", saved_path, 1);
    free(saved_path);
  } else {
    unsetenv("LOCPATH");
  }
#endif
  if (entered && strcasecmp("I", "i") == 0) {
    setlocale(LC_CTYPE, saved_ctype);
    entered = false;
  }
  if (!entered) {
    free(saved_ctype);
    printf(
        "    [skipped: no tr_TR locale that folds 'I' apart from 'i'; "
        "tests_locale/tr_TR.UTF-8 is missing (localedef)]\n");
    return NULL;
  }
  return saved_ctype;
}

// Puts back the locale cli_tr_locale_enter saved and frees it; tests call it
// before any assertion.
static void cli_tr_locale_leave(char *saved_ctype) {
  setlocale(LC_CTYPE, saved_ctype);
  free(saved_ctype);
}

// Tells whether the request head has a field line named name, compared with
// ASCII folding only, so a peer can check what the client sent.
static bool head_has_field_ascii(const char *head, const char *name) {
  size_t nlen = strlen(name);
  for (const char *line = strstr(head, "\r\n"); line;
       line = strstr(line + 2, "\r\n")) {
    const char *p = line + 2;
    size_t i = 0;
    while (i < nlen && p[i]) {
      int a = (unsigned char)p[i], b = (unsigned char)name[i];
      if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
      if (a != b) break;
      i++;
    }
    if (i == nlen && p[i] == ':') return true;
  }
  return false;
}

// A peer that answers every request with the redirect in sp->stray.
static void scripted_redirect_peer(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  while (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) >= 0) {
    atomic_fetch_add(&sp->requests, 1);
    scripted_peer_send(fd, sp->stray);
  }
}

// A peer that answers 200 and counts in sp->body_bytes every request that
// carried an Authorization field, so the test can see a leaked credential.
static void scripted_auth_sink_peer(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  while (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) >= 0) {
    atomic_fetch_add(&sp->requests, 1);
    if (head_has_field_ascii(head, "authorization"))
      atomic_fetch_add(&sp->body_bytes, 1);
    scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  }
}

// A request map that folds through tolower() stores "authorIzatIon", which the
// redirect's credential rule doesn't recognise.
TEST(locale_folding, an_upper_case_credential_never_crosses_a_redirect) {
  wait_for_async_engine_idle();
  scripted_peer target, origin;
  memset(&target, 0, sizeof(target));
  memset(&origin, 0, sizeof(origin));
  REQUIRE_EQ(scripted_peer_start(&target, scripted_auth_sink_peer, 0), 0);
  char redirect[256];
  snprintf(redirect, sizeof(redirect),
           "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/landing\r\n"
           "Content-Length: 0\r\n\r\n",
           target.port);
  origin.stray = redirect;
  if (scripted_peer_start(&origin, scripted_redirect_peer, 0) != 0) {
    scripted_peer_stop(&target);
    REQUIRE_TRUE(false);
  }
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", origin.port);

  char *saved = cli_tr_locale_enter();
  ccol_retval rvs[3] = {ccol_unexpected_failure, ccol_unexpected_failure,
                        ccol_unexpected_failure};
  int status[3] = {0, 0, 0};
  if (saved) {
    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (req) (void)chttp_request_set_header(req, "AUTHORIZATION", "secret");
    for (int tier = 1; tier <= 3 && req; tier++) {
      chttpcli_response *resp = NULL;
      rvs[tier - 1] = scripted_do(cli, req, tier, &resp);
      if (resp) status[tier - 1] = resp->status_code;
      chttpcli_resp_free(resp);
    }
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    cli_tr_locale_leave(saved);
  }
  scripted_peer_stop(&origin);
  scripted_peer_stop(&target);
  if (!saved) return;
  long long leaked = atomic_load(&target.body_bytes);
  int reached = atomic_load(&target.requests);
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ(rvs[i], ccol_success);
    REQUIRE_EQ(status[i], 200);
  }
  REQUIRE_EQ(reached, 3);
  REQUIRE_EQ(leaked, 0LL);
}

// A peer that answers with an upper-case "TRANSFER-ENCODING: chunked" and two
// upper-case "SET-COOKIE" lines.
static void scripted_upper_case_fields_peer(scripted_peer *sp, int fd,
                                            int index) {
  (void)index;
  char head[8192];
  while (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) >= 0) {
    atomic_fetch_add(&sp->requests, 1);
    scripted_peer_send(fd,
                       "HTTP/1.1 200 OK\r\n"
                       "SET-COOKIE: a=1\r\n"
                       "SET-COOKIE: b=2\r\n"
                       "TRANSFER-ENCODING: chunked\r\n\r\n"
                       "5\r\nhello\r\n0\r\n\r\n");
  }
}

// A response map that folds through tolower() stores "set-cookIe", joins the
// two cookies and can't find "set-cookie".
TEST(locale_folding, upper_case_response_fields_keep_their_meaning) {
  wait_for_async_engine_idle();
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  REQUIRE_EQ(scripted_peer_start(&sp, scripted_upper_case_fields_peer, 0), 0);
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/fields", sp.port);

  char *saved = cli_tr_locale_enter();
  ccol_retval rvs[3] = {ccol_unexpected_failure, ccol_unexpected_failure,
                        ccol_unexpected_failure};
  char bodies[3][16] = {"", "", ""};
  char cookies[3][32] = {"", "", ""};
  size_t counts[3] = {0, 0, 0};
  if (saved) {
    chttpcli cli = chttpcli_create(NULL);
    (void)chttpcli_set_request_timeout(cli, 10000000);
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    for (int tier = 1; tier <= 3 && req; tier++) {
      chttpcli_response *resp = NULL;
      rvs[tier - 1] = scripted_do(cli, req, tier, &resp);
      if (resp) {
        if (resp->body)
          snprintf(bodies[tier - 1], sizeof(bodies[0]), "%s", resp->body);
        const char *c = chttpcli_resp_header(resp, "set-cookie");
        if (c) snprintf(cookies[tier - 1], sizeof(cookies[0]), "%s", c);
        counts[tier - 1] = chttpcli_resp_header_count(resp, "Set-Cookie");
      }
      chttpcli_resp_free(resp);
    }
    chttp_request_free(req);
    chttpcli_destroy(cli);
    wait_for_async_engine_idle();
    cli_tr_locale_leave(saved);
  }
  scripted_peer_stop(&sp);
  if (!saved) return;
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ(rvs[i], ccol_success);
    REQUIRE_STREQ(bodies[i], "hello");
    REQUIRE_STREQ(cookies[i], "a=1");
    REQUIRE_EQ(counts[i], (size_t)2);
  }
}

// With locale folding the certificate stored under "INFO.test" isn't found for
// "info.test", and the client refuses the default certificate.
TEST(locale_folding, an_upper_case_sni_name_finds_its_certificate) {
  char dir[] = "chttpcli_tls_tr_XXXXXX";
  REQUIRE_NE((void *)mkdtemp(dir), NULL);
  char named_cert[128], named_key[128], def_cert[128], def_key[128];
  snprintf(named_cert, sizeof(named_cert), "%s/named.pem", dir);
  snprintf(named_key, sizeof(named_key), "%s/named.key", dir);
  snprintf(def_cert, sizeof(def_cert), "%s/default.pem", dir);
  snprintf(def_key, sizeof(def_key), "%s/default.key", dir);
  int g1 =
      _openssl_selfsigned(named_key, named_cert, "info.test", "DNS:info.test");
  int g2 = _openssl_selfsigned(def_key, def_cert, "unrelated.test",
                               "DNS:unrelated.test");
  bool ok = false;
  char *saved = NULL;
  if (g1 == 0 && g2 == 0) {
    saved = cli_tr_locale_enter();
    if (saved) {
      ok = chttp__tls_client_handshake_for_tests("info.test", named_cert,
                                                 def_cert, def_key, "INFO.test",
                                                 named_cert, named_key);
      cli_tr_locale_leave(saved);
    }
  }
  unlink(named_cert);
  unlink(named_key);
  unlink(def_cert);
  unlink(def_key);
  rmdir(dir);
  REQUIRE_EQ(g1, 0);
  REQUIRE_EQ(g2, 0);
  if (!saved) return;
  REQUIRE_TRUE(ok);
}

// A caller-set "Connection: close" is honoured
//
// A peer that answers every request with a keep-alive 200 whatever the request
// says.
static void scripted_keepalive_peer(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  while (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) >= 0) {
    atomic_fetch_add(&sp->requests, 1);
    scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  }
}

// Runs two requests in one tier against the keep-alive peer; the first carries
// connection_value as its Connection header (none for NULL). Gives the number
// of connections the peer accepted, or -1, so the test can see whether the
// first connection was reused.
static int connections_for_two_requests(int tier,
                                        const char *connection_value) {
  wait_for_async_engine_idle();
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  if (scripted_peer_start(&sp, scripted_keepalive_peer, 0) != 0) return -1;
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/get", sp.port);
  chttpcli cli = chttpcli_create(NULL);
  (void)chttpcli_set_request_timeout(cli, 10000000);
  chttp_request *first = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttp_request *second = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  bool ok = first && second;
  if (ok && connection_value)
    ok = chttp_request_set_header(first, "Connection", connection_value) ==
         ccol_success;
  chttpcli_response *resp = NULL;
  if (ok) ok = scripted_do(cli, first, tier, &resp) == ccol_success;
  chttpcli_resp_free(resp);
  resp = NULL;
  if (ok) ok = scripted_do(cli, second, tier, &resp) == ccol_success;
  chttpcli_resp_free(resp);
  chttp_request_free(first);
  chttp_request_free(second);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  scripted_peer_stop(&sp);
  return ok ? atomic_load(&sp.nconn) : -1;
}

// RFC 9112 section 9.6: A request that names "close" is the last on its
// connection even when the response keeps it alive. The option is found in a
// token list in any ASCII case.
TEST(request_close, a_caller_close_option_ends_the_connection_in_every_tier) {
  for (int tier = 1; tier <= 3; tier++) {
    int plain = connections_for_two_requests(tier, NULL);
    int closed = connections_for_two_requests(tier, "close");
    int listed = connections_for_two_requests(tier, "keep-alive , CLOSE");
    int other = connections_for_two_requests(tier, "keep-alive, closed");
    REQUIRE_EQ(plain, 1);
    REQUIRE_EQ(closed, 2);
    REQUIRE_EQ(listed, 2);
    REQUIRE_EQ(other, 1);
  }
}

// Control bytes in a request field value: Every one except HTAB, and DEL, is
// refused at the setter and again when a caller-built map is serialized.
TEST(request, set_header_rejects_control_bytes_in_value) {
  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  static const char *const bad[] = {"a\x01z", "a\x1fz", "a\x7fz", "\x0b",
                                    "a\x1b[31mz"};
  ccol_retval rvs[5];
  for (size_t i = 0; i < 5; i++)
    rvs[i] = chttp_request_set_header(req, "x-bad", bad[i]);
  ccol_retval tab = chttp_request_set_header(req, "x-tab", "a\tb c~");
  ccol_retval high = chttp_request_set_header(req, "x-high", "caf\xc3\xa9");
  const char *stored = chttp_request_get_header(req, "x-bad");
  chttp_request_free(req);
  for (size_t i = 0; i < 5; i++) REQUIRE_EQ(rvs[i], ccol_invalid_args);
  REQUIRE_EQ(tab, ccol_success);
  REQUIRE_EQ(high, ccol_success);
  REQUIRE_EQ((void *)stored, NULL);
}

TEST(http, run_query_borrowed_map_with_a_control_byte_is_refused) {
  char url[128];
  make_url(url, sizeof(url), "/get");
  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-bad", "a\x01z");
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttp_perform(CHTTP_GET, url, NULL, hdrs, &resp);
  chmap_destroy(hdrs);
  chttpcli_resp_free(resp);
  chttp_request_body body = CHTTP_BODY("x", 1, "text/plain\x7f");
  resp = NULL;
  ccol_retval ct_rv = chttp_perform(CHTTP_POST, url, &body, NULL, &resp);
  chttpcli_resp_free(resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ(ct_rv, ccol_invalid_args);
}

// A peer that reads the start of the ClientHello and resets the connection.
static void scripted_reset_in_handshake(scripted_peer *sp, int fd, int index) {
  (void)index;
  char buf[512];
  struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
  if (poll(&p, 1, 5000) > 0) (void)recv(fd, buf, sizeof(buf), 0);
  atomic_fetch_add(&sp->requests, 1);
  struct linger lg = {.l_onoff = 1, .l_linger = 0};
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

// Without the handshake code Tier 1 reports the socket error of the handshake
// wait as ccol_http_connection_failed.
TEST(tls, a_handshake_that_the_peer_resets_reports_a_handshake_failure) {
  wait_for_async_engine_idle();
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  REQUIRE_EQ(scripted_peer_start(&sp, scripted_reset_in_handshake, 0), 0);
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/get", sp.port);
  chttpcli cli = chttpcli_create(NULL);
  chttp_tls_config tc = {.insecure_skip_verify = true};
  ccol_retval set_rv = chttpcli_set_tls(cli, &tc);
  (void)chttpcli_set_request_timeout(cli, 10000000);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  ccol_retval rvs[3] = {ccol_unexpected_failure, ccol_unexpected_failure,
                        ccol_unexpected_failure};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rvs[tier - 1] = scripted_do(cli, req, tier, &resp);
    chttpcli_resp_free(resp);
  }
  chttp_request_free(req);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  scripted_peer_stop(&sp);
  REQUIRE_EQ(set_rv, ccol_success);
  for (int i = 0; i < 3; i++)
    REQUIRE_EQ(rvs[i], ccol_http_tls_handshake_failed);
}

TEST(relative_redirects,
     absolute_and_network_path_references_lose_dot_segments) {
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"http://h/a/./b/../c", "http://h/a/c"},
      {"//h/q/../r", "http://h/r"},
      {"https://h:8443/a/../../b?x=/../y#f/../g",
       "https://h:8443/b?x=/../y#f/../g"},
      {"//h/./", "http://h/"},
      {"http://h", "http://h"},
      {"http://h?q=../x", "http://h?q=../x"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = chttp__resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
                                                         cases[i].location);
    bool same = result && strcmp(result, cases[i].expected) == 0;
    if (!same)
      printf("    location %s gave %s\n", cases[i].location,
             result ? result : "(null)");
    free(result);
    REQUIRE_TRUE(same);
  }
}

// A reused connection that dies after a whole request
//
// Reads one request head into head and, when it declares a Content-Length, its
// body. Gives false on EOF, an error or a timeout.
static bool scripted_read_request(scripted_peer *sp, int fd, char *head,
                                  size_t max) {
  ssize_t n = scripted_peer_read_head(sp, fd, head, max, 5000);
  if (n < 0) return false;
  const char *end = strstr(head, "\r\n\r\n");
  size_t have = (size_t)n - (size_t)(end + 4 - head);
  size_t want = 0;
  const char *cl = strcasestr(head, "\r\ncontent-length:");
  if (cl) want = (size_t)strtoul(cl + 17, NULL, 10);
  char b[4096];
  while (have < want) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 5000) <= 0) return false;
    ssize_t r = recv(fd, b, sizeof(b), 0);
    if (r <= 0) return false;
    have += (size_t)r;
  }
  return true;
}

// A peer for the replay tests. The first connection answers its first request
// with keep-alive, then reads the whole second request and closes without
// answering; with sp->phase 1 it resets while the body is still arriving. Every
// later connection answers "retried". sp->requests counts the requests.
static void scripted_die_after_second_request(scripted_peer *sp, int fd,
                                              int index) {
  char head[8192];
  if (index != 0) {
    while (scripted_read_request(sp, fd, head, sizeof(head))) {
      atomic_fetch_add(&sp->requests, 1);
      scripted_peer_send(fd,
                         "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nretried");
    }
    return;
  }
  if (!scripted_read_request(sp, fd, head, sizeof(head))) return;
  atomic_fetch_add(&sp->requests, 1);
  scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
  if (atomic_load(&sp->phase) == 1) {
    if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
    atomic_fetch_add(&sp->requests, 1);
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    return;
  }
  if (!scripted_read_request(sp, fd, head, sizeof(head))) return;
  atomic_fetch_add(&sp->requests, 1);
}

// What run_replay_case ended with.
typedef struct {
  ccol_retval rv;
  char body[16];
  int requests;
  int conns;
} replay_outcome_t;

// Sends a GET the peer answers and then method with body_len bytes of body
// (and a header named idem_header unless it's NULL) on the reused connection,
// through tier, so the replay tests can see whether the request was sent again.
// reset_mid_body picks the reset in the middle of the body over the silent
// close after it.
static replay_outcome_t run_replay_case(int tier, chttp_method method,
                                        size_t body_len,
                                        const char *idem_header,
                                        bool reset_mid_body) {
  replay_outcome_t o = {
      .rv = ccol_unexpected_failure, .body = "", .requests = -1, .conns = -1};
  wait_for_async_engine_idle();
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  if (scripted_peer_start(&sp, scripted_die_after_second_request,
                          reset_mid_body ? 4096 : 0) != 0)
    return o;
  atomic_store(&sp.phase, reset_mid_body ? 1 : 0);
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/r", sp.port);
  chttpcli cli = chttpcli_create(NULL);
  (void)chttpcli_set_request_timeout(cli, 10000000);
  chttp_request *warm = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  char *data = body_len ? (char *)malloc(body_len) : NULL;
  if (data) memset(data, 'b', body_len);
  chttp_request_body b = CHTTP_BODY(data, body_len, "text/plain");
  chttp_request *req =
      chttp_request_create(method, url, body_len ? &b : NULL, NULL);
  if (req && idem_header)
    (void)chttp_request_set_header(req, idem_header, "k1");
  chttpcli_response *resp = NULL;
  ccol_retval warm_rv =
      warm ? scripted_do(cli, warm, tier, &resp) : ccol_not_enough_memory;
  chttpcli_resp_free(resp);
  resp = NULL;
  if (warm_rv == ccol_success && req) {
    o.rv = scripted_do(cli, req, tier, &resp);
    if (resp && resp->body) snprintf(o.body, sizeof(o.body), "%s", resp->body);
    chttpcli_resp_free(resp);
  }
  chttp_request_free(warm);
  chttp_request_free(req);
  free(data);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  scripted_peer_stop(&sp);
  o.requests = atomic_load(&sp.requests);
  o.conns = atomic_load(&sp.nconn);
  return o;
}

// A reused connection takes a whole request and closes without answering.
// POST and PATCH fail with ccol_http_transfer_aborted and reach the server
// once; GET, PUT, DELETE and a POST with an Idempotency-Key or
// X-Idempotency-Key are sent again.
TEST(replay, a_whole_request_that_is_not_idempotent_is_never_sent_twice) {
  for (int tier = 1; tier <= 3; tier++) {
    replay_outcome_t post = run_replay_case(tier, CHTTP_POST, 5, NULL, false);
    replay_outcome_t patch = run_replay_case(tier, CHTTP_PATCH, 5, NULL, false);
    replay_outcome_t get = run_replay_case(tier, CHTTP_GET, 0, NULL, false);
    replay_outcome_t put = run_replay_case(tier, CHTTP_PUT, 5, NULL, false);
    replay_outcome_t del = run_replay_case(tier, CHTTP_DELETE, 0, NULL, false);
    replay_outcome_t keyed =
        run_replay_case(tier, CHTTP_POST, 5, "Idempotency-Key", false);
    replay_outcome_t x_keyed =
        run_replay_case(tier, CHTTP_POST, 5, "X-Idempotency-Key", false);
    REQUIRE_EQ(post.rv, ccol_http_transfer_aborted);
    REQUIRE_EQ(post.requests, 2);
    REQUIRE_EQ(post.conns, 1);
    REQUIRE_EQ(patch.rv, ccol_http_transfer_aborted);
    REQUIRE_EQ(patch.requests, 2);
    REQUIRE_EQ(patch.conns, 1);
    REQUIRE_EQ(get.rv, ccol_success);
    REQUIRE_STREQ(get.body, "retried");
    REQUIRE_EQ(get.requests, 3);
    REQUIRE_EQ(put.rv, ccol_success);
    REQUIRE_STREQ(put.body, "retried");
    REQUIRE_EQ(del.rv, ccol_success);
    REQUIRE_STREQ(del.body, "retried");
    REQUIRE_EQ(keyed.rv, ccol_success);
    REQUIRE_STREQ(keyed.body, "retried");
    REQUIRE_EQ(x_keyed.rv, ccol_success);
    REQUIRE_STREQ(x_keyed.body, "retried");
  }
}

TEST(replay, a_post_that_did_not_go_out_in_full_is_retried) {
  for (int tier = 1; tier <= 3; tier++) {
    replay_outcome_t post =
        run_replay_case(tier, CHTTP_POST, 4u << 20, NULL, true);
    REQUIRE_EQ(post.rv, ccol_success);
    REQUIRE_STREQ(post.body, "retried");
    REQUIRE_EQ(post.conns, 2);
  }
}

// Engine wait, engine allocator and engine logger
//
// An allocator that counts every call in g_ew_calls, and in
// g_ew_calls_after_death the calls after the test set g_ew_dead. It always
// serves the call, so a late call shows as a count and not a crash.
static atomic_bool g_ew_dead = false;
static atomic_int g_ew_calls_after_death = 0;
static atomic_int g_ew_calls = 0;
// Counts one call of the ew allocator.
static void ew_note(void) {
  atomic_fetch_add(&g_ew_calls, 1);
  if (atomic_load(&g_ew_dead)) atomic_fetch_add(&g_ew_calls_after_death, 1);
}
static void *ew_malloc(size_t n) {
  ew_note();
  return malloc(n);
}
static void *ew_calloc(size_t n, size_t s) {
  ew_note();
  return calloc(n, s);
}
static void *ew_realloc(void *p, size_t n) {
  ew_note();
  return realloc(p, n);
}
static void ew_free(void *p) {
  if (p) ew_note();
  free(p);
}
static ccol_memmgmt_procs g_ew_mp = {.malloc = ew_malloc,
                                     .calloc = ew_calloc,
                                     .realloc = ew_realloc,
                                     .free = ew_free};

// Runs one Tier 2 GET to the end and gives its code, so the engine wait tests
// can start and use the engine.
static ccol_retval one_async_get(chttpcli cli, const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval rv =
      req ? scripted_do(cli, req, 2, &resp) : ccol_not_enough_memory;
  chttpcli_resp_free(resp);
  chttp_request_free(req);
  return rv;
}

// The reaper is held in its last section, which closes the fallback logger
// through the engine allocator; a wait that doesn't join the reaper returns
// during the hold.
TEST(engine_wait, the_engine_allocator_is_unused_after_engine_wait) {
  wait_for_async_engine_idle();
  ccol_retval release_rv = chttpcli_set_engine_logger(CLOG_NULL);
  atomic_store(&g_ew_dead, false);
  atomic_store(&g_ew_calls_after_death, 0);
  atomic_store(&g_ew_calls, 0);
  ccol_retval set_rv = chttpcli_set_engine_mem_mgmt_procs(&g_ew_mp);
  char url[128];
  make_url(url, sizeof(url), "/keepalive");
  chttpcli cli = chttpcli_create(NULL);
  ccol_retval rv = one_async_get(cli, url);
  chttpcli__hold_next_reaper_final_section_for_tests(300);
  chttpcli_destroy(cli);
  chttpcli_engine_wait();
  bool held = chttpcli__reaper_final_section_was_entered_for_tests();
  atomic_store(&g_ew_dead, true);
  usleep(600000);
  int late = atomic_load(&g_ew_calls_after_death);
  int used = atomic_load(&g_ew_calls);
  wait_for_async_engine_idle();
  ccol_retval reset_rv = chttpcli_set_engine_mem_mgmt_procs(NULL);
  REQUIRE_EQ(release_rv, ccol_success);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_GT(used, 0);
  REQUIRE_EQ(late, 0);
  REQUIRE_EQ(reset_rv, ccol_success);
}

// An allocator that remembers every live block in g_ep_live under g_ep_lock, so
// the_dns_pool_uses_the_engine_allocator can ask whether an object came from
// it.
enum { EP_TRACK_MAX = 1 << 16 };
static void *g_ep_live[EP_TRACK_MAX];
static size_t g_ep_count = 0;
static pthread_mutex_t g_ep_lock = PTHREAD_MUTEX_INITIALIZER;
// Records the block p as live.
static void ep_remember(void *p) {
  if (!p) return;
  pthread_mutex_lock(&g_ep_lock);
  if (g_ep_count < EP_TRACK_MAX) g_ep_live[g_ep_count++] = p;
  pthread_mutex_unlock(&g_ep_lock);
}
// Drops the block p from the live list.
static void ep_forget(void *p) {
  if (!p) return;
  pthread_mutex_lock(&g_ep_lock);
  for (size_t i = g_ep_count; i-- > 0;) {
    if (g_ep_live[i] == p) {
      g_ep_live[i] = g_ep_live[--g_ep_count];
      break;
    }
  }
  pthread_mutex_unlock(&g_ep_lock);
}
// Tells whether p is a live block of the ep allocator.
static bool ep_is_live(const void *p) {
  bool found = false;
  pthread_mutex_lock(&g_ep_lock);
  for (size_t i = 0; i < g_ep_count && !found; i++) found = (g_ep_live[i] == p);
  pthread_mutex_unlock(&g_ep_lock);
  return found;
}
static void *ep_malloc(size_t n) {
  void *p = malloc(n);
  ep_remember(p);
  return p;
}
static void *ep_calloc(size_t n, size_t s) {
  void *p = calloc(n, s);
  ep_remember(p);
  return p;
}
static void *ep_realloc(void *p, size_t n) {
  ep_forget(p);
  void *q = realloc(p, n);
  ep_remember(q ? q : p);
  return q;
}
static void ep_free(void *p) {
  ep_forget(p);
  free(p);
}

TEST(engine_wait, the_dns_pool_uses_the_engine_allocator) {
  wait_for_async_engine_idle();
  ccol_memmgmt_procs mp = {.malloc = ep_malloc,
                           .calloc = ep_calloc,
                           .realloc = ep_realloc,
                           .free = ep_free};
  ccol_retval set_rv = chttpcli_set_engine_mem_mgmt_procs(&mp);
  ccol_retval acq_rv = chttpcli__engine_acquire_for_tests();
  cthrpool dns = chttpcli__engine_dns_pool_for_tests();
  const void *raw = cthrpool__resolve_for_tests(dns);
  bool from_engine = raw != NULL && ep_is_live(raw);
  if (acq_rv == ccol_success) chttpcli__engine_release_for_tests();
  chttpcli_engine_wait();
  ccol_retval reset_rv = chttpcli_set_engine_mem_mgmt_procs(NULL);
  size_t left = g_ep_count;
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(acq_rv, ccol_success);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_TRUE(from_engine);
  REQUIRE_EQ(reset_rv, ccol_success);
  REQUIRE_EQ(left, (size_t)0);
}

// The destructor probe. A copy of this binary started with
// CHTTPCLI_DTOR_PROBE_FD set runs no test and calls no library function, and
// reports on that descriptor. Some systems run destructors only when main()
// returns, so the copy leaves main() normally.
//
// An exit handler that writes 'x', so the parent knows the copy reached
// exit().
static void _dtor_probe_reached_exit(void) {
  ssize_t w = write(g_dtor_probe_fd, "x", 1);
  (void)w;
}

// Reads CHTTPCLI_DTOR_PROBE_FD into g_dtor_probe_fd, which makes the other
// constructors of this file stand down, and registers the exit handler. It
// reads the environment because only some C libraries pass argc and argv to a
// constructor.
__attribute__((constructor(101))) static void _dtor_probe_enter(void) {
  const char *fd_text = getenv("CHTTPCLI_DTOR_PROBE_FD");
  if (!fd_text) return;
  g_dtor_probe_fd = atoi(fd_text);
  if (atexit(_dtor_probe_reached_exit) != 0) test_child_exit(2);
}

// Runs after the library's destructors and writes 'Y' or 'N' for whether the
// engine globals were initialized.
__attribute__((destructor(101))) static void _dtor_probe_report(void) {
  if (g_dtor_probe_fd < 0) return;
  char verdict = chttpcli__engine_globals_entered_for_tests() ? 'Y' : 'N';
  ssize_t w = write(g_dtor_probe_fd, &verdict, 1);
  (void)w;
}

// A process that never used the engine must not initialize its mutex and
// condition variable at exit. Where a program runs no destructors at all (clang
// on OpenBSD) there is nothing to observe and the test says so.
TEST(engine_wait, the_module_destructor_initializes_nothing_unused) {
  char self[PATH_MAX];
  bool have_self = tau_argv0_ != NULL && realpath(tau_argv0_, self) != NULL;
  int p[2];
  int prv = have_self ? pipe(p) : -1;
  REQUIRE_EQ(prv, 0);
  char fd_text[16];
  snprintf(fd_text, sizeof(fd_text), "%d", p[1]);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    setenv("CHTTPCLI_DTOR_PROBE_FD", fd_text, 1);
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execl(self, self, "--filter=no_test_has_this_name", (char *)NULL);
    ssize_t w = write(p[1], "E", 1);
    (void)w;
    test_child_exit(127);
  }
  close(p[1]);
  char got[4] = {0};
  size_t n = 0;
  while (pid > 0 && n < sizeof(got)) {
    ssize_t r = read(p[0], got + n, sizeof(got) - n);
    if (r > 0) {
      n += (size_t)r;
    } else if (r == 0 || errno != EINTR) {
      break;
    }
  }
  close(p[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  bool reached_exit = memchr(got, 'x', n) != NULL;
  bool says_yes = memchr(got, 'Y', n) != NULL;
  bool says_no = memchr(got, 'N', n) != NULL;
  bool exec_failed = memchr(got, 'E', n) != NULL;
  REQUIRE_GT(pid, 0);
  if (exec_failed) {
    fprintf(stderr,
            "SKIP: this binary can't re-execute itself here (qemu-user "
            "without binfmt, for example)\n");
    return;
  }
  REQUIRE_TRUE(reached_exit);
  if (!says_yes && !says_no) {
    fprintf(stderr, "SKIP: this build runs no destructors at exit\n");
    return;
  }
  REQUIRE_FALSE(says_yes);
}

TEST(engine_wait, returns_at_once_when_no_engine_runs) {
  wait_for_async_engine_idle();
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpcli_engine_wait();
  REQUIRE_LT(elapsed_ms_since(&t0), 1000L);
}

// Without the release, the derived logger stays installed and the next engine
// run writes its start and stop messages to the file.
TEST(engine_logger, a_release_closes_the_installed_logger) {
  char path_buf[96];
  const char *path = engine_log_path(path_buf, sizeof(path_buf), "release");
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  REQUIRE_GE(fd, 0);
  wait_for_async_engine_idle();
  atomic_store(&g_ew_dead, false);
  atomic_store(&g_ew_calls_after_death, 0);
  clog lg = clog_create_from_fd_mp(fd, CLOG_INFO, NULL, &g_ew_mp);
  ccol_retval set_rv =
      lg ? chttpcli_set_engine_logger(lg) : ccol_not_enough_memory;
  if (lg) clog_close(lg);
  char url[128];
  make_url(url, sizeof(url), "/keepalive");
  chttpcli cli = chttpcli_create(NULL);
  ccol_retval rv1 = one_async_get(cli, url);
  ccol_retval busy_rv = chttpcli_set_engine_logger(CLOG_NULL);
  chttpcli_destroy(cli);
  chttpcli_engine_wait();
  int lines_before = count_lines_containing(path, "http client reactor engine");
  ccol_retval release_rv = chttpcli_set_engine_logger(CLOG_NULL);
  atomic_store(&g_ew_dead, true);
  cli = chttpcli_create(NULL);
  ccol_retval rv2 = one_async_get(cli, url);
  chttpcli_destroy(cli);
  chttpcli_engine_wait();
  int late = atomic_load(&g_ew_calls_after_death);
  int lines_after = count_lines_containing(path, "http client reactor engine");
  if (release_rv != ccol_success) {
    clog quiet = clog_create_from_fd(2, CLOG_FATAL, NULL);
    if (quiet) {
      (void)chttpcli_set_engine_logger(quiet);
      clog_close(quiet);
    }
  }
  close(fd);
  unlink(path);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_EQ(busy_rv, ccol_not_permitted);
  REQUIRE_EQ(release_rv, ccol_success);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_EQ(lines_before, 2);
  REQUIRE_EQ(lines_after, 2);
  REQUIRE_EQ(late, 0);
}

// A destroy on a thread of the engine is fatal
//
// The client that other_destroy_write_fn destroys.
typedef struct {
  chttpcli other;
} other_destroy_ctx_t;

// A write_fn that destroys another client, once.
static size_t other_destroy_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  other_destroy_ctx_t *c = (other_destroy_ctx_t *)ctx;
  if (c->other) {
    chttpcli victim = c->other;
    c->other = CHTTPCLI_NULL;
    chttpcli__destroy(victim);
  }
  return len;
}

// Runs, in a child with one reactor thread, a request of client A whose
// write_fn destroys client B, which holds an idle Tier 2 connection. On an
// engine thread (tier 1: Tier 2, tier 2: Tier 3) that must be fatal; through
// Tier 1 (tier 0) it's an ordinary destroy and the child says so on stderr. The
// child's stderr goes into msg. Gives the wait status, or -1.
static int other_destroy_status(int tier, char *msg, size_t msg_size) {
  wait_for_async_engine_idle();
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (chttpcli_set_engine_num_reactor_threads(1) != ccol_success)
      test_child_exit(4);
    char url[160];
    make_url(url, sizeof(url), "/get");
    chttpcli a = chttpcli_create(NULL);
    chttpcli b = chttpcli_create(NULL);
    if (a == CHTTPCLI_NULL || b == CHTTPCLI_NULL) test_child_exit(2);
    if (one_async_get(b, url) != ccol_success) test_child_exit(5);
    other_destroy_ctx_t c = {.other = b};
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    if (!req) test_child_exit(3);
    int status_code = 0;
    if (tier == 0) {
      (void)chttpcli_do_streaming(a, req, other_destroy_write_fn, &c,
                                  &status_code);
    } else if (tier == 1) {
      cthrpool_future *f =
          chttpcli_do_async_streaming(a, req, other_destroy_write_fn, &c);
      if (f) (void)chttpcli_async_result_get(f);
    } else {
      (void)chttpcli_do_pooled_streaming(a, req, other_destroy_write_fn, &c,
                                         &status_code);
    }
    if (c.other == CHTTPCLI_NULL) {
      static const char done[] = "other client destroyed\n";
      ssize_t w = write(STDERR_FILENO, done, sizeof(done) - 1);
      (void)w;
    }
    test_child_exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return -1;
  }
  int status = 0;
  bool ended = test_wait_child_bounded(pid, 10000, &status);
  size_t got = 0;
  int fl = fcntl(fds[0], F_GETFL);
  fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
  ssize_t n;
  while (got + 1 < msg_size &&
         (n = read(fds[0], msg + got, msg_size - 1 - got)) > 0)
    got += (size_t)n;
  msg[got] = '\0';
  close(fds[0]);
  return ended ? status : -1;
}

// Without the check the destroy on the only reactor thread waits for a
// teardown that same thread must run, and the child never ends.
TEST(chttpcli_handle_lifecycle, destroy_on_an_engine_thread_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char msg[1024];
  int st0 = other_destroy_status(0, msg, sizeof(msg));
  bool tier1_clean = st0 >= 0 && WIFEXITED(st0) &&
                     strstr(msg, "other client destroyed") != NULL;
  bool aborted[2] = {false, false};
  bool named[2] = {false, false};
  for (int tier = 1; tier <= 2; tier++) {
    int st = other_destroy_status(tier, msg, sizeof(msg));
    aborted[tier - 1] = st >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    named[tier - 1] = strstr(msg, "on a thread of the async engine") != NULL;
  }
  REQUIRE_TRUE(tier1_clean);
  REQUIRE_TRUE(aborted[0]);
  REQUIRE_TRUE(named[0]);
  REQUIRE_TRUE(aborted[1]);
  REQUIRE_TRUE(named[1]);
}

// Close-on-exec of the default CA file
//
// The CA file to look for (dev and ino) and the watcher's flags and counts.
typedef struct {
  dev_t dev;
  ino_t ino;
  atomic_bool stop;
  atomic_bool inherited;
  atomic_long forks;
} ca_fd_watch_t;

// Forks children until stop is set. Each child looks for a descriptor of the
// default CA file without FD_CLOEXEC and reports through a pipe, using only
// async-signal-safe calls, so it works whatever other threads held at the fork.
// Sets inherited when a child found one.
static void *_ca_fd_watch_thread(void *arg) {
  ca_fd_watch_t *w = (ca_fd_watch_t *)arg;
  while (!atomic_load(&w->stop)) {
    int p[2];
    if (pipe(p) != 0) break;
    pid_t pid = fork();
    if (pid == 0) {
      close(p[0]);
      char verdict = 'N';
      for (int fd = 0; fd < 1024 && verdict == 'N'; fd++) {
        struct stat st;
        if (fd == p[1] || fstat(fd, &st) != 0) continue;
        if (st.st_dev != w->dev || st.st_ino != w->ino) continue;
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && !(flags & FD_CLOEXEC)) verdict = 'Y';
      }
      ssize_t wr = write(p[1], &verdict, 1);
      test_child_exit(wr == 1 ? 0 : 2);
    }
    close(p[1]);
    if (pid < 0) {
      close(p[0]);
      break;
    }
    char verdict = 0;
    ssize_t n;
    while ((n = read(p[0], &verdict, 1)) < 0 && errno == EINTR) {
    }
    close(p[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    atomic_fetch_add(&w->forks, 1);
    if (n == 1 && verdict == 'Y') atomic_store(&w->inherited, true);
  }
  return NULL;
}

// SSL_CTX_set_default_verify_paths would fopen(3) the CA file for every client,
// and a child forked in that window would hold a descriptor an exec() keeps.
TEST(chttpcli_tls, client_creation_leaves_no_inheritable_ca_file_descriptor) {
  const char *path = getenv(X509_get_default_cert_file_env());
  if (!path || !*path) path = X509_get_default_cert_file();
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    fprintf(stderr, "WARNING: no default CA file at %s; test skipped.\n", path);
    return;
  }
  ca_fd_watch_t w = {.dev = st.st_dev, .ino = st.st_ino};
  atomic_init(&w.stop, false);
  atomic_init(&w.inherited, false);
  atomic_init(&w.forks, 0);
  ctls__system_ca_forget_for_tests();
  pthread_t tid;
  bool started = pthread_create(&tid, NULL, _ca_fd_watch_thread, &w) == 0;
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int created = 0;
  bool create_failed = false;
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (long)(now.tv_sec - t0.tv_sec) * 1000 +
              (now.tv_nsec - t0.tv_nsec) / 1000000;
    if (ms >= 20000 ||
        (created >= 20 && ms >= 1500 && atomic_load(&w.forks) >= 5))
      break;
    if (atomic_load(&w.inherited)) break;
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) {
      create_failed = true;
      break;
    }
    chttpcli_destroy(cli);
    created++;
    if (created % 8 == 0) sched_yield();
  }
  atomic_store(&w.stop, true);
  if (started) pthread_join(tid, NULL);
  REQUIRE_TRUE(started);
  REQUIRE_FALSE(create_failed);
  REQUIRE_GT(atomic_load(&w.forks), 0L);
  REQUIRE_FALSE(atomic_load(&w.inherited));
}

// Precision of the async deadline sweep
//
// Gives the milliseconds since t0 on CLOCK_MONOTONIC, for the sweep timing
// tests.
static long _elapsed_ms_from(const struct timespec *t0) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long)(now.tv_sec - t0->tv_sec) * 1000 +
         (now.tv_nsec - t0->tv_nsec) / 1000000;
}

// Runs one GET of /very-slow through Tier 2 (pooled false) or Tier 3 (true)
// with cli's request timeout. Gives its code and the time it took in
// *elapsed_ms.
static ccol_retval _timed_slow_get(chttpcli cli, bool pooled,
                                   long *elapsed_ms) {
  char url[160];
  make_url(url, sizeof(url), "/very-slow");
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval rv = ccol_unexpected_failure;
  if (pooled) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    if (req) rv = chttpcli_do_pooled(cli, req, &resp);
    chttpcli_resp_free(resp);
    chttp_request_free(req);
  } else {
    cthrpool_future *f = async_get(cli, url);
    chttpcli_async_result *raw = f ? chttpcli_async_result_get(f) : NULL;
    if (raw) {
      rv = raw->rv;
      chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    if (f) cthrpool_future_free(f);
  }
  *elapsed_ms = _elapsed_ms_from(&t0);
  return rv;
}

// For every deadline a sweep walk found expired, the instant the sweep planned
// that walk for must be no later than the deadline. Load can't move that
// instant, so wall time is only checked against the deadline and a ceiling.
TEST(async_deadline, short_request_timeouts_fire_close_to_their_deadline) {
  wait_for_async_engine_idle();
  const long timeouts_ms[3] = {5, 30, 150};
  ccol_retval rvs[2][3];
  long elapsed[2][3];
  chttpcli_construct(cli);
  chttp__sweep_due_reset_for_tests();
  for (int tier = 0; tier < 2; tier++) {
    for (int i = 0; i < 3; i++) {
      elapsed[tier][i] = 0;
      rvs[tier][i] =
          chttpcli_set_request_timeout(cli, (uint64_t)timeouts_ms[i] * 1000u);
      if (rvs[tier][i] == ccol_success)
        rvs[tier][i] = _timed_slow_get(cli, tier == 1, &elapsed[tier][i]);
    }
  }
  int64_t max_late_ns = 0;
  unsigned long expiries = chttp__sweep_due_for_tests(0, &max_late_ns);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  for (int tier = 0; tier < 2; tier++) {
    for (int i = 0; i < 3; i++) {
      REQUIRE_EQ(rvs[tier][i], ccol_timed_out);
      REQUIRE_GE(elapsed[tier][i], timeouts_ms[i]);
      REQUIRE_LT(elapsed[tier][i], timeouts_ms[i] + 5000L);
    }
  }
  REQUIRE_EQ(expiries, 6UL);
  REQUIRE_LE(max_late_ns, (int64_t)0);
}

// A walk that didn't start from an empty earliest instant would keep the first
// request's passed deadline as the earliest for good and walk without
// sleeping.
TEST(async_deadline, a_long_timeout_does_not_wake_the_sweep_repeatedly) {
  wait_for_async_engine_idle();
  chttpcli_construct(cli);
  long short_ms = 0;
  ccol_retval short_rv = chttpcli_set_request_timeout(cli, 30000);
  if (short_rv == ccol_success)
    short_rv = _timed_slow_get(cli, false, &short_ms);
  ccol_retval set_rv = chttpcli_set_request_timeout(cli, 10000000);
  unsigned long ticks_before = chttp__sweep_ticks_for_tests();
  long elapsed_ms = 0;
  ccol_retval rv = set_rv == ccol_success
                       ? _timed_slow_get(cli, false, &elapsed_ms)
                       : set_rv;
  unsigned long ticks = chttp__sweep_ticks_for_tests() - ticks_before;
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(short_rv, ccol_timed_out);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_LE(ticks, (unsigned long)(elapsed_ms / 100 + 5));
}

// A deadline after the sweep's planned wake doesn't wake it, and a wake only
// replans, so 30 starts in a row add no walks.
TEST(async_deadline, requests_that_start_in_a_row_do_not_add_sweep_walks) {
  wait_for_async_engine_idle();
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/very-slow");
  enum { N_REQS = 30 };
  cthrpool_future *futures[N_REQS];
  ccol_retval set_rv = ccol_success;
  for (int i = 0; i < N_REQS; i++) futures[i] = NULL;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  unsigned long ticks_before = chttp__sweep_ticks_for_tests();
  for (int i = 0; i < N_REQS && set_rv == ccol_success; i++) {
    set_rv =
        chttpcli_set_request_timeout(cli, (uint64_t)(20000 - 50 * i) * 1000u);
    if (set_rv == ccol_success) futures[i] = async_get(cli, url);
    struct timespec gap = {0, 10000000};
    nanosleep(&gap, NULL);
  }
  int ok = 0;
  for (int i = 0; i < N_REQS; i++) {
    if (!futures[i]) continue;
    chttpcli_async_result *raw = chttpcli_async_result_get(futures[i]);
    if (raw) {
      if (raw->rv == ccol_success && raw->resp && raw->resp->status_code == 200)
        ok++;
      chttpcli_resp_free(raw->resp);
      chttpcli_async_result_free(raw);
    }
    cthrpool_future_free(futures[i]);
  }
  unsigned long ticks = chttp__sweep_ticks_for_tests() - ticks_before;
  long elapsed_ms = _elapsed_ms_from(&t0);
  chttpcli_destroy(cli);
  wait_for_async_engine_idle();
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(ok, (int)N_REQS);
  REQUIRE_LE(ticks, (unsigned long)(elapsed_ms / 100 + 5));
}
