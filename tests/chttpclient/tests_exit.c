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

/* How a process that used the async engine over TLS exits.
 *
 * The Tier 2 and Tier 3 engine stops on a reaper thread once its last reference
 * goes away. A process that returns from main right then must not run OpenSSL's
 * cleanup while an engine thread is still around, because that thread frees
 * OpenSSL's per-thread state as it exits and the cleanup frees the same state.
 * So the client's exit hook stops every engine thread and joins the reaper
 * before the cleanup gets to run.
 *
 * Each case runs its scenario in a fork() child, since the scenario ends with
 * exit(). It is a separate binary because the parent has to fork while it has
 * exactly one thread and has never touched the library, so each child starts
 * the engine from scratch. The TLS server is a separate process that serves
 * every connection in a process of its own, so the children never share a
 * thread with it.
 *
 * Before using the library a child registers a check with atexit(). The atexit
 * list runs in reverse registration order, so the check runs last, after the
 * client's hook and after OpenSSL's cleanup. It asserts that the process has
 * one thread left, that the engine is down and that every reaper the library
 * created has been joined.
 */

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
#include <test_threads.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

/* A child's exit codes. */
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

/* ========================================================================== */
/*                         TLS SERVER (its own process)                       */
/* ========================================================================== */

static char g_dir[128];
static char g_cert[192];
static char g_key[192];
static int g_port = 0;
static pid_t g_server_pid = -1;
static pid_t g_parent_pid = -1;
static bool g_ready = false;
static bool g_setup_failed = false;

/* Answers every request on one connection until the peer closes it. GET /hang
 * never gets an answer, and every other GET gets a small 200 that keeps the
 * connection open. */
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
      /* The client gives up on this request, and the connection ends when the
       * client process exits. */
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

static void _server_main(int lfd) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx ||
      SSL_CTX_use_certificate_file(ctx, g_cert, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, g_key, SSL_FILETYPE_PEM) != 1)
    _exit(1);
  for (;;) {
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) {
      /* A failed accept is passing (a signal, a client that reset in the
         queue, a full descriptor table), and a server that left on one would
         leave its listener open, so every later test of this binary would
         connect and wait for an answer. _teardown ends this process with
         SIGKILL, so only a listener that can't be used ends the loop. */
      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) _exit(0);
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
      _exit(0);
    }
    close(fd);
  }
}

static void _setup(void) {
  if (g_ready || g_setup_failed) return;
  g_setup_failed = true;
  snprintf(g_dir, sizeof(g_dir), "%s", "/tmp/chttpclient_exit_XXXXXX");
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
  /* We use an exit handler because it runs on every system, while OpenBSD runs
   * no destructor of a program that links libpthread and the server would
   * outlive the test. */
  atexit(_teardown);
  g_ready = true;
  g_setup_failed = false;
}

/* A test's child runs this exit handler too when it exits, but only the process
 * that started the server stops it. */
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

/* ========================================================================== */
/*                         THE CHILD                                          */
/* ========================================================================== */

static int _thread_count(void) { return test_thread_count(); }

/* The child gives its verdict as one byte on this pipe. Under valgrind a
 * child's exit status reflects valgrind's leak report and not the child's own
 * code, so we can't take the verdict from the status. No byte at all means the
 * child died before its last exit handler. */
static int g_report_fd = -1;
/* The child's thread count right after fork(), before it touches the library.
 * Natively it's 1, but under qemu-user /proc/self/task also lists the
 * emulator's own host threads, so the count at exit is compared with this
 * baseline and not with the constant 1. */
static int g_thread_baseline = 1;
static int g_scenario_code = CHILD_OK;

/* The child's thread count once every thread the exit handlers joined is gone.
 * pthread_join() returns when the kernel clears the thread's ID and the kernel
 * removes the thread from /proc/self/task a moment later, so a thread joined
 * just before this check may still be listed. Such a thread disappears within
 * microseconds while one that nobody joined stays, so we read the count until
 * it reaches the baseline, for 2 s at most. */
static int _settled_thread_count(void) {
  int threads = _thread_count();
  for (int i = 0; i < 2000 && threads != g_thread_baseline; i++) {
    struct timespec ts = {0, 1000L * 1000L};
    nanosleep(&ts, NULL);
    threads = _thread_count();
  }
  return threads;
}

/* Runs after every other exit handler of the child. */
static void _check_at_exit(void) {
  unsigned char code = (unsigned char)g_scenario_code;
  int threads = _settled_thread_count();
  if (code == CHILD_OK && (threads < 1 || threads != g_thread_baseline))
    code = CHILD_THREADS_LEFT;
  if (code == CHILD_OK && _chttpclient_engine_running_for_tests())
    code = CHILD_ENGINE_RUNNING;
  if (code == CHILD_OK && _chttpclient_reaper_created_count_for_tests() !=
                              _chttpclient_reaper_joined_count_for_tests())
    code = CHILD_REAPER_NOT_JOINED;
  if (write(g_report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
}

static bool _set_tls(chttpcli cli) {
  chttp_tls_config_t t = {.ca_bundle_path = g_cert};
  return chttpclient_set_tls(cli, &t) == ccol_success;
}

static bool _tier2_get(chttpcli cli, const char *url) {
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  if (!f) return false;
  chttpcli_async_result_t *r = chttpclient_async_result_get(f);
  bool ok =
      r && r->rv == ccol_success && r->resp && r->resp->status_code == 200;
  if (r) {
    chttpclient_resp_free(r->resp);
    chttpclient_async_result_free(r);
  }
  ctpool_future_free(f);
  return ok;
}

static bool _tier3_get(chttpcli cli, const char *url) {
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  bool ok = rv == ccol_success && resp && resp->status_code == 200;
  chttpclient_resp_free(resp);
  return ok;
}

/* Tier 2 and Tier 3 on a client the child destroys, and then Tier 2 on the
 * default client, whose pooled connection is holding the engine when the child
 * exits. */
static void _child_full(void) {
  atexit(_check_at_exit);
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  bool ok = true;
  chttpcli cli = chttpclient_create(NULL);
  if (cli == CHTTPCLI_INVALID || !_set_tls(cli)) {
    g_scenario_code = CHILD_SCENARIO_FAILED;
    exit(1);
  }
  ok = _tier2_get(cli, url) && ok;
  ok = _tier3_get(cli, url) && ok;
  ok = _tier2_get(cli, url) && ok;
  chttpclient_destroy(cli);
  chttpcli dc = chttp_default_client();
  if (dc == CHTTPCLI_INVALID || !_set_tls(dc))
    ok = false;
  else
    ok = _tier2_get(dc, url) && ok;
  g_scenario_code = ok ? CHILD_OK : CHILD_SCENARIO_FAILED;
  exit(0);
}

static size_t _exit_from_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)ctx;
  (void)len;
  exit(CHILD_OK);
}

/* exit() from a streaming request's write_fn, on a reactor thread. */
static void _child_exit_in_callback(void) {
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpclient_create(NULL);
  if (cli == CHTTPCLI_INVALID || !_set_tls(cli)) exit(CHILD_SCENARIO_FAILED);
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) exit(CHILD_SCENARIO_FAILED);
  ctpool_future *f =
      chttpclient_do_async_streaming(cli, req, _exit_from_write_fn, NULL);
  if (!f) exit(CHILD_SCENARIO_FAILED);
  (void)chttpclient_async_result_get(f);
  /* Not reached, the callback ends the process first. */
  exit(CHILD_SCENARIO_FAILED);
}

/* exit() from main with a request in flight. */
static void _child_exit_in_flight(void) {
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/hang", g_port);
  chttpcli cli = chttpclient_create(NULL);
  if (cli == CHTTPCLI_INVALID || !_set_tls(cli)) exit(CHILD_SCENARIO_FAILED);
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) exit(CHILD_SCENARIO_FAILED);
  ctpool_future *f = chttpclient_do_async(cli, req);
  if (!f) exit(CHILD_SCENARIO_FAILED);
  /* Give the request some time to reach the server. */
  struct timespec ts = {0, 200L * 1000L * 1000L};
  nanosleep(&ts, NULL);
  exit(CHILD_OK);
}

/* The prefork pattern: a process uses the async engine, destroys its last
 * client and forks with no handle left. The engine has stopped, and the handle
 * of the reaper that stopped it stays unjoined until the next acquire (in a
 * build with the fork handlers the fork itself joins it first). glibc hands
 * that reaper's descriptor, which the forked child doesn't inherit as a thread,
 * to a thread the child starts. So the child starts threads until one of them
 * carries the reaper's ID, keeps all of them blocked, and only then makes a
 * Tier 2 and a Tier 3 request. Joining the inherited reaper handle would join
 * that blocked thread and never return, and the process waiting for the child
 * reports it as a hang. FreeBSD's thread library rebuilds its thread list in a
 * fork child and never gives an inherited ID to a new thread, so on FreeBSD the
 * case only checks that the child's requests succeed. */
static int g_blocker_fd = -1;

static void *_blocker(void *arg) {
  (void)arg;
  char c;
  while (read(g_blocker_fd, &c, 1) < 0 && errno == EINTR) {
  }
  return NULL;
}

#define PREFORK_MAX_THREADS 128

static void _forked_child_main(pthread_t stale_reaper, int report_fd) {
  unsigned char code = CHILD_OK;
  int bfd[2];
  if (pipe(bfd) != 0) {
    code = CHILD_SCENARIO_FAILED;
    if (write(report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
    _exit(0);
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
    chttpcli cli = chttpclient_create(NULL);
    bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
    ok = ok && _tier2_get(cli, url);
    ok = ok && _tier3_get(cli, url);
    if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
    if (!ok) code = CHILD_SCENARIO_FAILED;
  }
  /* All the blockers are released and joined before the verdict. */
  close(bfd[1]);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  close(bfd[0]);
  if (write(report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}

static bool _under_valgrind(void);

static void _child_prefork_after_engine_stop(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpclient_create(NULL);
  bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
  ok = ok && _tier3_get(cli, url);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  pthread_t stale;
  if (!ok) {
    code = CHILD_SCENARIO_FAILED;
  } else {
    _chttpclient_engine_wait_stopped_unjoined_for_tests();
    if (!_chttpclient_engine_unjoined_reaper_for_tests(&stale))
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
      /* The wait has a bound, so a child that hangs fails this case instead of
       * hanging the binary. */
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
  if (write(g_report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}

#if CCOL_FORK_SAFETY_REQUIRED
/* A fork while the engine's reaper holds the engine mutex, in the last critical
 * section of its teardown. We hold the reaper there on purpose for a bounded
 * time and fork once it's there. A child that inherits the mutex held can never
 * take it, and its first Tier 2 request hangs, so the fork has to wait for the
 * teardown to finish. */
static void _forked_child_requests(int report_fd) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpclient_create(NULL);
  bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
  ok = ok && _tier2_get(cli, url);
  ok = ok && _tier3_get(cli, url);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  if (!ok) code = CHILD_SCENARIO_FAILED;
  if (write(report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}

#define FORK_DURING_REAP_ROUNDS 3

static void _child_fork_during_reap(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  int hold_ms = _under_valgrind() ? 3000 : 800;
  for (int round = 0; round < FORK_DURING_REAP_ROUNDS && code == CHILD_OK;
       round++) {
    chttpcli cli = chttpclient_create(NULL);
    bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
    ok = ok && _tier3_get(cli, url);
    _chttpclient_hold_next_reaper_final_section_for_tests(hold_ms);
    /* The destroy drops the last reference and a reaper starts. */
    if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
    if (!ok) {
      code = CHILD_SCENARIO_FAILED;
      break;
    }
    /* Once the hold has started the flag stays set, so this poll sees the hold
     * even if this thread only wakes after the hold is over. The fork of such a
     * round then comes after the teardown, which is a valid state as well; the
     * hold is long enough that most rounds fork inside it. */
    bool held = false;
    for (int i = 0; i < 10000 && !held; i++) {
      held = _chttpclient_reaper_final_section_was_entered_for_tests();
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
    _chttpclient_engine_wait_for_quiescence_for_tests();
  }
  if (write(g_report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}

/* A fork while the engine's reaper is still running after its teardown: it
 * has cleared the stopping state, closed the fallback logger, and waits
 * there on purpose, with no lock held, for a bounded time. Closing the
 * logger frees memory through the engine's allocator, so a child forked in
 * this window can inherit a lock of that allocator, or of a sanitizer
 * runtime's allocator, in its held state. The fork must therefore wait
 * until the reaper has ended and been joined, so no engine thread exists at
 * the fork. The child checks that from the counters it inherits and then
 * makes a Tier 2 and a Tier 3 request. */
static void _forked_child_after_reaper_tail(int report_fd) {
  unsigned char code = CHILD_OK;
  if (_chttpclient_reaper_created_count_for_tests() !=
      _chttpclient_reaper_joined_count_for_tests())
    code = CHILD_REAPER_RAN_AT_FORK;
  if (code == CHILD_OK) {
    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
    chttpcli cli = chttpclient_create(NULL);
    bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
    ok = ok && _tier2_get(cli, url);
    ok = ok && _tier3_get(cli, url);
    if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
    if (!ok) code = CHILD_SCENARIO_FAILED;
  }
  if (write(report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}

static void _child_fork_during_reaper_tail(void) {
  unsigned char code = CHILD_OK;
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/x", g_port);
  chttpcli cli = chttpclient_create(NULL);
  bool ok = cli != CHTTPCLI_INVALID && _set_tls(cli);
  ok = ok && _tier3_get(cli, url);
  _chttpclient_hold_next_reaper_tail_for_tests(_under_valgrind() ? 3000 : 800);
  /* The destroy drops the last reference and a reaper starts. */
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  if (!ok) code = CHILD_SCENARIO_FAILED;
  /* Once the hold has started the entered flag stays set, so this poll also
   * sees a hold that is already over. */
  bool held = false;
  for (int i = 0; code == CHILD_OK && i < 10000 && !held; i++) {
    held = _chttpclient_reaper_tail_was_entered_for_tests();
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
    /* The fork joins the reaper first, so it only returns once the hold is
     * over. */
    bool tail_left = _chttpclient_reaper_tail_was_left_for_tests();
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
  if (write(g_report_fd, &code, 1) != 1) _exit(CHILD_SCENARIO_FAILED);
  _exit(0);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         THE PARENT                                         */
/* ========================================================================== */

/* valgrind maps its preload object into the process and also names it in
 * LD_PRELOAD, which is the trace we can find on a system without /proc. */
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

/* Runs fn in a child and waits for it, for timeout_s seconds at most. Returns
 * the wait status, or -1 when the child had to be killed. If report isn't NULL
 * it gets the byte the child wrote to its report pipe, or -1 when the child
 * wrote nothing. */
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
    _exit(CHILD_SCENARIO_FAILED);
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

TEST(exit_hook, no_engine_thread_survives_into_the_exit_of_openssl) {
  /* Without the exit hook nobody joins the reaper that the last
   * release started, and the child's check fails with CHILD_THREADS_LEFT or
   * CHILD_REAPER_NOT_JOINED. */
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
    /* Natively we also want a clean exit status, which is where a crash in a
     * later exit handler, OpenSSL's cleanup for example, shows up. */
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

TEST(exit_hook, exit_from_a_write_fn_does_not_hang) {
  /* The hook does nothing while a request is in flight, and the request whose
   * write_fn calls exit() is in flight; waiting for the engine there would mean
   * waiting for the very thread that runs the hook. The child may end by a
   * signal since it abandons a request that OpenSSL is still serving, but it
   * has to end. */
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

TEST(prefork,
     child_after_an_engine_stop_does_not_join_the_reaper_of_its_parent) {
  /* A child that joins the reaper handle it inherited joins its
   * own blocked thread, and the case reports CHILD_FORKED_CHILD_HUNG. On glibc
   * the case also won't pass without the precondition that makes that join
   * hang, and it reports CHILD_NO_THREAD_TOOK_THE_REAPER_ID when none of the
   * child's threads carries the reaper's ID. */
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_prefork_after_engine_stop,
                      _under_valgrind() ? 900 : 90, &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}

/* Without the fork protection this case is the caller's business, who must not
 * fork while a library thread holds one of the library's locks. */
#if CCOL_FORK_SAFETY_REQUIRED
TEST(prefork,
     fork_while_the_reaper_holds_the_engine_lock_gives_a_usable_child) {
  /* Take away the client's fork handlers and the child inherits the
   * engine mutex held and reports CHILD_FORKED_CHILD_HUNG. The case won't pass
   * when the reaper never got to its held section (CHILD_REAPER_NEVER_HELD). */
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_fork_during_reap, _under_valgrind() ? 1800 : 180,
                      &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}

TEST(prefork, fork_while_the_reaper_finishes_waits_for_its_thread) {
  /* Without the join in the client's fork handler the fork lands
   * while the reaper is still waiting after its teardown, and the child reports
   * CHILD_REAPER_RAN_AT_FORK because it inherits a reaper that nobody joined.
   * The case won't pass when the reaper never got to its held section
   * (CHILD_REAPER_NEVER_HELD). */
  _setup();
  REQUIRE_TRUE(g_ready);
  int report = -1;
  int st = _run_child(_child_fork_during_reaper_tail,
                      _under_valgrind() ? 900 : 90, &report);
  REQUIRE_NE(st, -1);
  REQUIRE_EQ(report, CHILD_OK);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/*
 * A process calling exit() while a request, whose name lookup outlived its
 * request timeout, is still waiting for that lookup ends at once: the client's
 * exit handler never waits for a lookup, since nothing can interrupt one. The
 * child leaves the lookup thread running on purpose, so it doesn't use the exit
 * checks of the other cases.
 *
 * If such a request were not counted as in flight, the exit
 * handler would wait for its chain, which the lookup holds for 30 s, and the
 * parent would find the child still running at its 20 s bound (which leaves
 * room for a child that starts slowly under valgrind).
 */
TEST(exit, exit_does_not_wait_for_an_abandoned_lookup) {
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    _chttp_set_name_lookup_delay_ms_for_tests(30000);
    chttpcli cli = chttpclient_create(NULL);
    if (cli == CHTTPCLI_INVALID) _exit(3);
    (void)chttpclient_set_request_timeout(cli, 100000);
    chttp_request_t *req = chttp_request_create(
        CHTTP_GET, "http://stalled-lookup.invalid:9/", NULL, NULL);
    ctpool_future *f = req ? chttpclient_do_async(cli, req) : NULL;
    chttpcli_async_result_t *r = f ? chttpclient_async_result_get(f) : NULL;
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
