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

// How a process that used the async engine over TLS exits. The Tier 2 and
// Tier 3 engine stops on a reaper thread once its last reference goes. A
// process that returns from main right then must not run OpenSSL's cleanup
// while an engine thread is still around, because that thread frees OpenSSL's
// per-thread state as it exits and the cleanup frees it too. So the client's
// exit hook stops every engine thread and joins the reaper before the cleanup
// runs.
//
// Each case runs in a fork() child, since it ends with exit(). This is a binary
// of its own because the parent must fork with one thread and before touching
// the library, so each child starts the engine from scratch. The TLS server is
// a separate process that serves each connection in a process of its own.
//
// A child registers _check_at_exit() with atexit() before it uses the library.
// atexit handlers run in reverse order, so the check runs last, after the
// client's hook and OpenSSL's cleanup.

// Asks for the GNU declarations of the C library before the first include,
// because the file calls memmem(), which glibc declares only then.
#define _GNU_SOURCE
#include <chttpclient.h>
#include <errno.h>
#include <internal/test_helpers/chttpclient.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_accept.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <test_threads.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the inherited signal state before any test runs, because the forked
// children report through their wait status (see
// test_reset_inherited_signals()).
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// A child's exit codes.
enum {
  CHILD_OK = 0,
  CHILD_SCENARIO_FAILED = 10,
  CHILD_THREADS_LEFT = 11,
  CHILD_ENGINE_RUNNING = 12,
  CHILD_REAPER_NOT_JOINED = 13,
  CHILD_NO_UNJOINED_REAPER = 14,
  CHILD_NO_THREAD_TOOK_THE_REAPER_ID = 15,
  CHILD_FORKED_CHILD_HUNG = 16,
  CHILD_FORK_FAILED = 17,
  CHILD_REAPER_NEVER_HELD = 18,
  CHILD_REAPER_RAN_AT_FORK = 19,
};

// The TLS server: Its scratch directory, certificate, key and port, the pids of
// the server and of the process that started it, and the state of _setup().
static char g_dir[128];
static char g_cert[192];
static char g_key[192];
static int g_port = 0;
static pid_t g_server_pid = -1;
static pid_t g_parent_pid = -1;
static bool g_ready = false;
static bool g_setup_failed = false;

// Serves one TLS connection on fd until the peer closes it; the server process
// runs it in a child for each connection. GET /hang never gets an answer (the
// client abandons it and the connection ends with the client process); every
// other GET gets a small keep-alive 200.
static void _serve_connection(SSL_CTX *ctx, int fd) {
  SSL *ssl = SSL_new(ctx);
  if (!ssl) return;
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) != 1) {
    SSL_free(ssl);
    return;
  }
  char buf[8192];
  size_t have = 0;
  for (;;) {
    char *end = have ? memmem(buf, have, "\r\n\r\n", 4) : NULL;
    if (!end) {
      if (have == sizeof(buf)) break;
      int n = SSL_read(ssl, buf + have, (int)(sizeof(buf) - have));
      if (n <= 0) break;
      have += (size_t)n;
      continue;
    }
    size_t used = (size_t)(end + 4 - buf);
    if (have >= 9 && memcmp(buf, "GET /hang", 9) == 0) {
      while (SSL_read(ssl, buf, (int)sizeof(buf)) > 0) {
      }
      break;
    }
    static const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Length: 5\r\n\r\nhello";
    if (SSL_write(ssl, resp, (int)(sizeof(resp) - 1)) <= 0) break;
    memmove(buf, buf + used, have - used);
    have -= used;
  }
  SSL_shutdown(ssl);
  SSL_free(ssl);
}

static void _teardown(void);

// Runs the server process that every case talks to: Accepts connections on lfd
// and serves each one in a child of its own. A failed accept doesn't end the
// loop (a server that left would keep its listener open and later tests would
// hang); only a listener that can't be used does. It never returns;
// _teardown() ends it with SIGKILL.
static void _server_main(int lfd) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx ||
      SSL_CTX_use_certificate_file(ctx, g_cert, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, g_key, SSL_FILETYPE_PEM) != 1)
    test_child_exit(1);
  for (;;) {
    int fd = test_accept(lfd);
    if (fd < 0) {
      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK)
        test_child_exit(0);
      if (errno != EINTR && errno != ECONNABORTED) {
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
      }
      continue;
    }
    pid_t p = fork();
    if (p == 0) {
      close(lfd);
      _serve_connection(ctx, fd);
      close(fd);
      test_child_exit(0);
    }
    close(fd);
  }
}

// Starts the TLS server that every case needs: Makes a self-signed certificate
// for 127.0.0.1, binds a listener on an ephemeral port and forks the server
// process. It sets g_port, g_server_pid, g_parent_pid and g_ready, runs once,
// and leaves g_ready false on a failure. The teardown is an atexit handler
// because OpenBSD runs no destructor of a program that links libpthread.
static void _setup(void) {
  if (g_ready || g_setup_failed) return;
  g_setup_failed = true;
  snprintf(g_dir, sizeof(g_dir), "%s", "/tmp/chttpcli_exit_XXXXXX");
  if (!mkdtemp(g_dir)) return;
  snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
  snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
  char cmd[768];
  snprintf(cmd, sizeof(cmd),
           "openssl req -x509 -newkey rsa:2048 -nodes -keyout '%s' -out '%s' "
           "-days 1 -subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1'"
           " >/dev/null 2>&1",
           g_key, g_cert);
  if (system(cmd) != 0) return;

  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return;
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t al = sizeof(a);
  if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(lfd, 64) != 0 ||
      getsockname(lfd, (struct sockaddr *)&a, &al) != 0) {
    close(lfd);
    return;
  }
  g_port = ntohs(a.sin_port);
  pid_t p = fork();
  if (p < 0) {
    close(lfd);
    return;
  }
  if (p == 0) _server_main(lfd);
  close(lfd);
  g_server_pid = p;
  g_parent_pid = getpid();
  atexit(_teardown);
  g_ready = true;
  g_setup_failed = false;
}

// Kills the server process and removes its files at exit, so nothing outlives
// the binary. A test's child runs this handler too, but only the process that
// started the server (g_parent_pid) acts.
static void _teardown(void) {
  if (getpid() != g_parent_pid) return;
  if (g_server_pid > 0) {
    kill(g_server_pid, SIGKILL);
    waitpid(g_server_pid, NULL, 0);
  }
  if (g_cert[0]) unlink(g_cert);
  if (g_key[0]) unlink(g_key);
  if (g_dir[0]) rmdir(g_dir);
}

// Gives the calling process's thread count, for the exit check.
static int _thread_count(void) { return test_thread_count(); }

// The pipe on which the child writes its verdict as one byte. Under valgrind
// the exit status reflects the leak report, so the status can't carry the
// verdict. No byte at all means the child died before its last exit handler.
static int g_report_fd = -1;
// The child's thread count right after fork(). It's 1 natively, but under
// qemu-user the emulator's own threads show up too, so the count at exit is
// compared with this baseline. g_scenario_code is the verdict of the scenario
// itself.
static int g_thread_baseline = 1;
static int g_scenario_code = CHILD_OK;

// Gives the child's thread count once the threads the exit handlers joined are
// gone. A joined thread leaves /proc/self/task a moment after pthread_join()
// returns, so we read until the count reaches the baseline, for about 10 s by
// the clock. A thread that nobody joined stays, so the check still fails on a
// real leak.
static int _settled_thread_count(void) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int threads = _thread_count();
  while (threads != g_thread_baseline) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long waited_ms =
        ((long long)(now.tv_sec - start.tv_sec) * 1000000000LL +
         (now.tv_nsec - start.tv_nsec)) /
        1000000LL;
    if (waited_ms >= 10000) break;
    struct timespec ts = {0, 1000L * 1000L};
    nanosleep(&ts, NULL);
    threads = _thread_count();
  }
  return threads;
}

// Writes the child's verdict to g_report_fd as its last exit handler, which is
// the point of the binary: g_scenario_code, or a failure code unless one thread
// is left (relative to g_thread_baseline), the engine is down and every reaper
// the library created has been joined.
static void _check_at_exit(void) {
  unsigned char code = (unsigned char)g_scenario_code;
  int threads = _settled_thread_count();
  if (code == CHILD_OK && (threads < 1 || threads != g_thread_baseline))
    code = CHILD_THREADS_LEFT;
  if (code == CHILD_OK && chttpcli__engine_running_for_tests())
    code = CHILD_ENGINE_RUNNING;
  if (code == CHILD_OK && chttpcli__reaper_created_count_for_tests() !=
                              chttpcli__reaper_joined_count_for_tests())
    code = CHILD_REAPER_NOT_JOINED;
  if (write(g_report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
}

// Makes cli trust the test certificate, so its requests reach the TLS server.
// Tells whether it worked.
static bool _set_tls(chttpcli cli) {
  chttp_tls_config t = {.ca_bundle_path = g_cert};
  return chttpcli_set_tls(cli, &t) == ccol_success;
}

// Makes a Tier 2 GET of url, the async path that starts the engine, and tells
// whether it answered 200.
static bool _tier2_get(chttpcli cli, const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  cthrpool_future *f = chttpcli_do_async(cli, req);
  chttp_request_free(req);
  if (!f) return false;
  chttpcli_async_result *r = chttpcli_async_result_get(f);
  bool ok =
      r && r->rv == ccol_success && r->resp && r->resp->status_code == 200;
  if (r) {
    chttpcli_resp_free(r->resp);
    chttpcli_async_result_free(r);
  }
  cthrpool_future_free(f);
  return ok;
}

// Makes a Tier 3 GET of url, the pooled path on the same engine, and tells
// whether it answered 200.
static bool _tier3_get(chttpcli cli, const char *url) {
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  bool ok = rv == ccol_success && resp && resp->status_code == 200;
  chttpcli_resp_free(resp);
  return ok;
}

// The ordinary exit case: Runs Tier 2 and Tier 3 on a client the child
// destroys, then Tier 2 on the default client, whose pooled connection holds
// the engine when the child exits. Sets g_scenario_code and calls exit().
static void _child_full(void) {
  atexit(_check_at_exit);
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  bool ok = true;
  chttpcli cli = chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL || !_set_tls(cli)) {
    g_scenario_code = CHILD_SCENARIO_FAILED;
    exit(1);
  }
  ok = _tier2_get(cli, url) && ok;
  ok = _tier3_get(cli, url) && ok;
  ok = _tier2_get(cli, url) && ok;
  chttpcli_destroy(cli);
  chttpcli dc = chttp_default_client();
  if (dc == CHTTPCLI_NULL || !_set_tls(dc))
    ok = false;
  else
    ok = _tier2_get(dc, url) && ok;
  g_scenario_code = ok ? CHILD_OK : CHILD_SCENARIO_FAILED;
  exit(0);
}

// Ends the process; it's the write_fn of _child_exit_in_callback, which checks
// an exit() from a reactor thread.
static size_t _exit_from_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)ctx;
  (void)len;
  exit(CHILD_OK);
}

// Calls exit() from a streaming request's write_fn, on a reactor thread, to
// check that the exit hook doesn't wait for its own thread. The final exit() is
// never reached.
static void _child_exit_in_callback(void) {
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL || !_set_tls(cli)) exit(CHILD_SCENARIO_FAILED);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) exit(CHILD_SCENARIO_FAILED);
  cthrpool_future *f =
      chttpcli_do_async_streaming(cli, req, _exit_from_write_fn, NULL);
  if (!f) exit(CHILD_SCENARIO_FAILED);
  (void)chttpcli_async_result_get(f);
  exit(CHILD_SCENARIO_FAILED);
}

// Calls exit() from main with a request to /hang in flight, after giving the
// request time to reach the server, to check that the exit hook doesn't wait
// for it.
static void _child_exit_in_flight(void) {
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/hang", g_port);
  chttpcli cli = chttpcli_create(NULL);
  if (cli == CHTTPCLI_NULL || !_set_tls(cli)) exit(CHILD_SCENARIO_FAILED);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) exit(CHILD_SCENARIO_FAILED);
  cthrpool_future *f = chttpcli_do_async(cli, req);
  if (!f) exit(CHILD_SCENARIO_FAILED);
  struct timespec ts = {0, 200L * 1000L * 1000L};
  nanosleep(&ts, NULL);
  exit(CHILD_OK);
}

// The prefork case: A process uses the async engine, destroys its last client
// and forks with no handle left. The handle of the reaper that stopped the
// engine stays unjoined until the next acquire (a build with fork handlers
// joins it at the fork). glibc gives that reaper's descriptor to a thread the
// child starts, so the child starts threads until one carries the reaper's ID,
// keeps them all blocked and then makes a Tier 2 and a Tier 3 request. Joining
// the inherited handle would join a blocked thread and hang. FreeBSD never
// reuses an inherited ID, so there the case only checks that the requests
// succeed.

// The read end that keeps the blocker threads waiting.
static int g_blocker_fd = -1;

// Waits until the write end of g_blocker_fd closes; the grandchild keeps its
// threads parked with it while one of them holds the reaper's ID.
static void *_blocker(void *arg) {
  (void)arg;
  char c;
  while (read(g_blocker_fd, &c, 1) < 0 && errno == EINTR) {
  }
  return NULL;
}

// The most threads the grandchild starts while it looks for the reaper's ID,
// enough for glibc to hand the cached descriptor out.
#define PREFORK_MAX_THREADS 128

// Runs the grandchild of the prefork case: Starts blockers until one takes
// stale_reaper's ID (required on glibc), makes its requests, releases and joins
// the blockers, and reports on report_fd.
static void _forked_child_main(pthread_t stale_reaper, int report_fd) {
  unsigned char code = CHILD_OK;
  int bfd[2];
  if (pipe(bfd) != 0) {
    code = CHILD_SCENARIO_FAILED;
    if (write(report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
    test_child_exit(0);
  }
  g_blocker_fd = bfd[0];
  pthread_t tids[PREFORK_MAX_THREADS];
  int started = 0;
  bool took_id = false;
  while (started < PREFORK_MAX_THREADS && !took_id) {
    if (pthread_create(&tids[started], NULL, _blocker, NULL) != 0) break;
    took_id = pthread_equal(tids[started], stale_reaper) != 0;
    started++;
  }
#ifdef __GLIBC__
  bool id_reuse_expected = true;
#else
  bool id_reuse_expected = false;
#endif
  if (!took_id && id_reuse_expected) {
    code = CHILD_NO_THREAD_TOOK_THE_REAPER_ID;
  } else {
    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
    chttpcli cli = chttpcli_create(NULL);
    bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
    ok = ok && _tier2_get(cli, url);
    ok = ok && _tier3_get(cli, url);
    if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
    if (!ok) code = CHILD_SCENARIO_FAILED;
  }
  close(bfd[1]);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  close(bfd[0]);
  if (write(report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}

static bool _under_valgrind(void);

// Runs the child of the prefork case: Uses the engine, destroys its client,
// forks the grandchild and waits for its report with a bound, so a hang fails
// the case instead of the binary. Reports on g_report_fd.
static void _child_prefork_after_engine_stop(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpcli_create(NULL);
  bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
  ok = ok && _tier3_get(cli, url);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  pthread_t stale;
  if (!ok) {
    code = CHILD_SCENARIO_FAILED;
  } else {
    chttpcli__engine_wait_stopped_unjoined_for_tests();
    if (!chttpcli__engine_unjoined_reaper_for_tests(&stale))
      code = CHILD_NO_UNJOINED_REAPER;
  }
  if (code == CHILD_OK) {
    int rfd[2];
    pid_t p = -1;
    if (pipe(rfd) == 0) {
      p = fork();
      if (p == 0) {
        close(rfd[0]);
        _forked_child_main(stale, rfd[1]);
      }
      close(rfd[1]);
    }
    if (p < 0) {
      code = CHILD_FORK_FAILED;
    } else {
      struct pollfd pr = {.fd = rfd[0], .events = POLLIN, .revents = 0};
      int rc;
      do {
        rc = poll(&pr, 1, _under_valgrind() ? 300000 : 30000);
      } while (rc < 0 && errno == EINTR);
      unsigned char b = 0;
      if (rc > 0 && read(rfd[0], &b, 1) == 1) {
        code = b;
      } else {
        code = CHILD_FORKED_CHILD_HUNG;
        kill(p, SIGKILL);
      }
      waitpid(p, NULL, 0);
      close(rfd[0]);
    }
  }
  if (write(g_report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}

#if CCOL_FORK_SAFETY_REQUIRED
// A fork while the engine's reaper holds the engine mutex in the last critical
// section of its teardown (held there on purpose for a bounded time). A child
// that inherits the mutex held hangs on its first Tier 2 request, so the fork
// must wait for the teardown to finish.

// Runs the forked child of the held-reap case: Makes a Tier 2 and a Tier 3
// request (which hang if the child inherited the engine mutex held) and reports
// on report_fd.
static void _forked_child_requests(int report_fd) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpcli_create(NULL);
  bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
  ok = ok && _tier2_get(cli, url);
  ok = ok && _tier3_get(cli, url);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  if (!ok) code = CHILD_SCENARIO_FAILED;
  if (write(report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}

// How many forks the held-reap case makes; most of them land inside the
// hold.
#define FORK_DURING_REAP_ROUNDS 3

// Runs FORK_DURING_REAP_ROUNDS rounds and reports on g_report_fd. Each round
// destroys the last client (starting a reaper with a held final section), waits
// until the hold has started and forks. The hold flag stays set, so a round
// that wakes after the hold forks after the teardown, which is valid too; the
// hold is long enough that most rounds fork inside it.
static void _child_fork_during_reap(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  int hold_ms = _under_valgrind() ? 3000 : 800;
  for (int round = 0; round < FORK_DURING_REAP_ROUNDS && code == CHILD_OK;
       round++) {
    chttpcli cli = chttpcli_create(NULL);
    bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
    ok = ok && _tier3_get(cli, url);
    chttpcli__hold_next_reaper_final_section_for_tests(hold_ms);
    if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
    if (!ok) {
      code = CHILD_SCENARIO_FAILED;
      break;
    }
    bool held = false;
    for (int i = 0; i < 10000 && !held; i++) {
      held = chttpcli__reaper_final_section_was_entered_for_tests();
      if (!held) usleep(1000);
    }
    if (!held) {
      code = CHILD_REAPER_NEVER_HELD;
      break;
    }
    int rfd[2];
    if (pipe(rfd) != 0) {
      code = CHILD_FORK_FAILED;
      break;
    }
    pid_t p = fork();
    if (p == 0) {
      close(rfd[0]);
      _forked_child_requests(rfd[1]);
    }
    close(rfd[1]);
    if (p < 0) {
      close(rfd[0]);
      code = CHILD_FORK_FAILED;
      break;
    }
    struct pollfd pr = {.fd = rfd[0], .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pr, 1, _under_valgrind() ? 300000 : 30000);
    } while (rc < 0 && errno == EINTR);
    unsigned char b = 0;
    if (rc > 0 && read(rfd[0], &b, 1) == 1) {
      code = b;
    } else {
      code = CHILD_FORKED_CHILD_HUNG;
      kill(p, SIGKILL);
    }
    waitpid(p, NULL, 0);
    close(rfd[0]);
    chttpcli__engine_wait_for_quiescence_for_tests();
  }
  if (write(g_report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}

// A fork while the reaper still runs after its teardown: It has cleared the
// stopping state, closed the fallback logger and waits there with no lock held.
// Closing the logger frees through the engine's allocator, so a child forked
// here can inherit an allocator lock (ours or a sanitizer's) held. The fork
// must wait until the reaper has ended and been joined.

// Runs the forked child of the reaper-tail case: Checks from the inherited
// counters that every reaper was joined, then makes a Tier 2 and a Tier 3
// request and reports on report_fd.
static void _forked_child_after_reaper_tail(int report_fd) {
  unsigned char code = CHILD_OK;
  if (chttpcli__reaper_created_count_for_tests() !=
      chttpcli__reaper_joined_count_for_tests())
    code = CHILD_REAPER_RAN_AT_FORK;
  if (code == CHILD_OK) {
    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
    chttpcli cli = chttpcli_create(NULL);
    bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
    ok = ok && _tier2_get(cli, url);
    ok = ok && _tier3_get(cli, url);
    if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
    if (!ok) code = CHILD_SCENARIO_FAILED;
  }
  if (write(report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}

// Runs the child of the reaper-tail case: Destroys the last client with the
// reaper's tail held, waits until the hold has started, forks and reports on
// g_report_fd. The fork joins the reaper first, so it returns only once the
// hold is over.
static void _child_fork_during_reaper_tail(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpcli_create(NULL);
  bool ok = cli != CHTTPCLI_NULL && _set_tls(cli);
  ok = ok && _tier3_get(cli, url);
  chttpcli__hold_next_reaper_tail_for_tests(_under_valgrind() ? 3000 : 800);
  if (cli != CHTTPCLI_NULL) chttpcli_destroy(cli);
  if (!ok) code = CHILD_SCENARIO_FAILED;
  bool held = false;
  for (int i = 0; code == CHILD_OK && i < 10000 && !held; i++) {
    held = chttpcli__reaper_tail_was_entered_for_tests();
    if (!held) usleep(1000);
  }
  if (code == CHILD_OK && !held) code = CHILD_REAPER_NEVER_HELD;
  int rfd[2] = {-1, -1};
  if (code == CHILD_OK && pipe(rfd) != 0) code = CHILD_FORK_FAILED;
  if (code == CHILD_OK) {
    pid_t p = fork();
    if (p == 0) {
      close(rfd[0]);
      _forked_child_after_reaper_tail(rfd[1]);
    }
    bool tail_left = chttpcli__reaper_tail_was_left_for_tests();
    close(rfd[1]);
    if (p < 0) {
      code = CHILD_FORK_FAILED;
    } else {
      struct pollfd pr = {.fd = rfd[0], .events = POLLIN, .revents = 0};
      int rc;
      do {
        rc = poll(&pr, 1, _under_valgrind() ? 300000 : 30000);
      } while (rc < 0 && errno == EINTR);
      unsigned char b = 0;
      if (rc > 0 && read(rfd[0], &b, 1) == 1) {
        code = b;
      } else {
        code = CHILD_FORKED_CHILD_HUNG;
        kill(p, SIGKILL);
      }
      waitpid(p, NULL, 0);
      if (code == CHILD_OK && !tail_left) code = CHILD_REAPER_RAN_AT_FORK;
    }
    close(rfd[0]);
  }
  if (write(g_report_fd, &code, 1) != 1) test_child_exit(CHILD_SCENARIO_FAILED);
  test_child_exit(0);
}
#endif

// Tells whether we run under valgrind, so the cases can give their children
// more time. valgrind maps its preload object and names it in LD_PRELOAD,
// which is a trace we can find without /proc.
static bool _under_valgrind(void) {
  const char *pre = getenv("LD_PRELOAD");
  if (pre && strstr(pre, "vgpreload")) return true;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return false;
  char line[512];
  bool found = false;
  while (!found && fgets(line, sizeof(line), f))
    if (strstr(line, "vgpreload")) found = true;
  fclose(f);
  return found;
}

// Runs fn in a child and waits for it, for timeout_s seconds at most; every
// case runs its scenario this way. Gives the wait status, or -1 when the child
// had to be killed. *report (when not NULL) gets the byte the child wrote, or
// -1 when it wrote nothing.
static int _run_child(void (*fn)(void), int timeout_s, int *report) {
  int pfd[2];
  if (pipe(pfd) != 0) return -2;
  pid_t p = fork();
  if (p < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -2;
  }
  if (p == 0) {
    close(pfd[0]);
    g_report_fd = pfd[1];
    g_thread_baseline = _thread_count();
    fn();
    test_child_exit(CHILD_SCENARIO_FAILED);
  }
  close(pfd[1]);
  int status = 0;
  for (int i = 0; i < timeout_s * 100; i++) {
    pid_t r = waitpid(p, &status, WNOHANG);
    if (r == p) {
      unsigned char b;
      if (report) *report = read(pfd[0], &b, 1) == 1 ? b : -1;
      close(pfd[0]);
      return status;
    }
    struct timespec ts = {0, 10L * 1000L * 1000L};
    nanosleep(&ts, NULL);
  }
  kill(p, SIGKILL);
  waitpid(p, &status, 0);
  close(pfd[0]);
  if (report) *report = -1;
  return -1;
}

// Without the exit hook nobody joins the reaper of the last release, and the
// child reports CHILD_THREADS_LEFT or CHILD_REAPER_NOT_JOINED. Natively the
// exit status must be clean too, which is where a crash in OpenSSL's cleanup
// shows.
TEST(exit_hook, no_engine_thread_survives_into_the_exit_of_openssl) {
  _setup();
  REQUIRE_TRUE(g_ready);
  bool vg = _under_valgrind();
  int rounds = vg ? 2 : 25;
  int timeout_s = vg ? 300 : 60;
  int bad_report = CHILD_OK;
  int bad_status = 0;
  int bad_round = -1;
  for (int i = 0; i < rounds; i++) {
    int report = -1;
    int st = _run_child(_child_full, timeout_s, &report);
    bool status_ok = vg || (st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (report != CHILD_OK || !status_ok) {
      bad_report = report;
      bad_status =
          st < 0 ? st : (WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st));
      bad_round = i;
      break;
    }
  }
  REQUIRE_EQ(bad_report, CHILD_OK);
  REQUIRE_EQ(bad_status, 0);
  REQUIRE_EQ(bad_round, -1);
}

// The hook does nothing while a request is in flight, and the request whose
// write_fn calls exit() is in flight; waiting there would wait for the thread
// that runs the hook. The child may end by a signal, but it must end.
TEST(exit_hook, exit_from_a_write_fn_does_not_hang) {
  _setup();
  REQUIRE_TRUE(g_ready);
  int st =
      _run_child(_child_exit_in_callback, _under_valgrind() ? 300 : 30, NULL);
  REQUIRE_NE(st, -1);
}

TEST(exit_hook, exit_with_a_request_in_flight_does_not_hang) {
  _setup();
  REQUIRE_TRUE(g_ready);
  int st =
      _run_child(_child_exit_in_flight, _under_valgrind() ? 300 : 30, NULL);
  REQUIRE_NE(st, -1);
}

// A child that joins its inherited reaper handle joins its own blocked thread
// and reports CHILD_FORKED_CHILD_HUNG. On glibc the case also fails with
// CHILD_NO_THREAD_TOOK_THE_REAPER_ID when the precondition isn't met.
TEST(prefork,
     child_after_an_engine_stop_does_not_join_the_reaper_of_its_parent) {
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_prefork_after_engine_stop,
                      _under_valgrind() ? 900 : 90, &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}

#if CCOL_FORK_SAFETY_REQUIRED
// Without fork protection this case is the caller's business. Without the
// client's fork handlers the child inherits the engine mutex held and reports
// CHILD_FORKED_CHILD_HUNG; a reaper that never reached its held section gives
// CHILD_REAPER_NEVER_HELD.
TEST(prefork,
     fork_while_the_reaper_holds_the_engine_lock_gives_a_usable_child) {
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_fork_during_reap, _under_valgrind() ? 1800 : 180,
                      &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}

// Without the join in the client's fork handler the child inherits an unjoined
// reaper and reports CHILD_REAPER_RAN_AT_FORK.
TEST(prefork, fork_while_the_reaper_finishes_waits_for_its_thread) {
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_fork_during_reaper_tail,
                      _under_valgrind() ? 900 : 90, &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}
#endif

// exit() while a request waits for a name lookup that outlived its request
// timeout ends at once, because the exit handler never waits for a lookup. If
// such a request weren't counted as in flight, the handler would wait for its
// chain, which the lookup holds for 30 s, and the parent's 20 s bound would
// find the child still running.
TEST(exit, exit_does_not_wait_for_an_abandoned_lookup) {
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    chttp__set_name_lookup_delay_ms_for_tests(30000);
    chttpcli cli = chttpcli_create(NULL);
    if (cli == CHTTPCLI_NULL) test_child_exit(3);
    (void)chttpcli_set_request_timeout(cli, 100000);
    chttp_request *req = chttp_request_create(
        CHTTP_GET, "http://stalled-lookup.invalid:9/", NULL, NULL);
    cthrpool_future *f = req ? chttpcli_do_async(cli, req) : NULL;
    chttpcli_async_result *r = f ? chttpcli_async_result_get(f) : NULL;
    exit((r && r->rv == ccol_timed_out) ? 0 : 4);
  }
  bool exited = false;
  int status = 0;
  for (int i = 0; i < 20000 && pid > 0; i++) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    struct timespec ts = {0, 1000L * 1000L};
    nanosleep(&ts, NULL);
  }
  if (pid > 0 && !exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  REQUIRE_GT(pid, 0);
  REQUIRE_TRUE(exited);
  REQUIRE_TRUE(WIFEXITED(status));
}
