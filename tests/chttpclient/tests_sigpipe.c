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

/* This binary runs with the default disposition of SIGPIPE, which ends the
 * process. It must stay a binary of its own: tests.c ignores SIGPIPE for the
 * whole process, and an ignored SIGPIPE is discarded when it is raised, so a
 * write that raises it is invisible there.
 *
 * It checks two things: that the client, the async engine included, leaves
 * the disposition of SIGPIPE as the application set it, and that no write of
 * the client raises SIGPIPE when the server has gone. SIGPIPE is blocked
 * around each operation, so a raised one stays pending instead of ending the
 * process, and a failure is one failed test instead of a dead binary. A
 * Tier 1 write runs on the calling thread, which asks sigpending(2), while
 * an async write runs on a thread of the engine; every such thread inherits
 * the blocked mask of the thread that started the engine, and the test reads
 * the pending set of every thread of the process: from /proc on Linux, and
 * from the kern.proc sysctl, one record for each thread, on FreeBSD. macOS
 * raises every SIGPIPE on the process, so there the pending set of the
 * process is the whole answer.
 */

#include <chttpclient.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_signals.h>
#include <test_threads.h>
#pragma GCC diagnostic pop

TAU_MAIN()

__attribute__((constructor)) static void _setup(void) {
  signal(SIGPIPE, SIG_DFL);
}

static bool _sigpipe_has_default_disposition(void) {
  struct sigaction sa;
  if (sigaction(SIGPIPE, NULL, &sa) != 0) return false;
  return sa.sa_handler == SIG_DFL;
}

static void _block_sigpipe(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, old);
}

/* Reports whether a SIGPIPE is pending for the calling thread or for the
 * process, and consumes it if so. The signal must be blocked. */
static bool _take_pending_sigpipe(void) {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  bool raised = sigismember(&pending, SIGPIPE) == 1;
  if (raised) test_consume_pending_sigpipe();
  return raised;
}

/* Reports whether any thread of the process, or the process itself, has a
 * SIGPIPE pending. *threads_seen counts the threads that it read, so that a
 * scan that read nothing cannot pass for a clean one. */
#ifdef __FreeBSD__
static bool _any_thread_has_sigpipe_pending(int *threads_seen) {
  *threads_seen = 0;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD,
                (int)getpid()};
  size_t len = 0;
  if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) return false;
  /* Room for threads that start between the two calls. */
  len += 16 * sizeof(struct kinfo_proc);
  struct kinfo_proc *kp = malloc(len);
  if (!kp) return false;
  bool found = false;
  if (sysctl(mib, 4, kp, &len, NULL, 0) == 0) {
    size_t n = len / sizeof(struct kinfo_proc);
    for (size_t i = 0; i < n; ++i) {
      if (kp[i].ki_structsize != (int)sizeof(struct kinfo_proc)) break;
      ++*threads_seen;
      if (sigismember(&kp[i].ki_siglist, SIGPIPE) == 1) found = true;
    }
  }
  free(kp);
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
    found = true;
  return found;
}
#elif defined(__APPLE__)
/* macOS raises every SIGPIPE on the process and never on one thread, so the
 * pending set that sigpending() gives while the signal is blocked holds it,
 * whichever thread wrote. The thread count stands for the threads read. */
static bool _any_thread_has_sigpipe_pending(int *threads_seen) {
  int n = test_thread_count();
  *threads_seen = n > 0 ? n : 0;
  sigset_t pending;
  sigemptyset(&pending);
  return sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
}
#else
static bool _any_thread_has_sigpipe_pending(int *threads_seen) {
  unsigned long long bit = 1ULL << (SIGPIPE - 1);
  bool found = false;
  *threads_seen = 0;
  DIR *d = opendir("/proc/self/task");
  if (!d) return false;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char path[300];
    snprintf(path, sizeof(path), "/proc/self/task/%s/status", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    char line[256];
    bool read_one = false;
    while (fgets(line, sizeof(line), f)) {
      unsigned long long mask = 0;
      if (sscanf(line, "SigPnd: %llx", &mask) == 1 ||
          sscanf(line, "ShdPnd: %llx", &mask) == 1) {
        read_one = true;
        if (mask & bit) found = true;
      }
    }
    fclose(f);
    if (read_one) ++*threads_seen;
  }
  closedir(d);
  return found;
}
#endif

/* ========================================================================== */
/*                A SERVER THAT REFUSES THE BODY OF A REQUEST                 */
/* ========================================================================== */

typedef struct {
  int listen_fd;
  /* Set once the server has read the request head. */
  atomic_int accepted;
  /* When true, the server holds the request unanswered until release is
     set, and then closes. */
  bool hold;
  atomic_int release;
  char path[64];
  char url[160];
  pthread_t tid;
  bool thread_started;
} refusing_server;

/* Reads until the end of the request head, or for at most ten seconds. */
static void _read_request_head(int c) {
  char buf[4096];
  size_t total = 0;
  while (total < sizeof(buf) - 1) {
    struct pollfd p = {.fd = c, .events = POLLIN};
    if (poll(&p, 1, 10000) != 1) return;
    ssize_t n = read(c, buf + total, sizeof(buf) - 1 - total);
    if (n <= 0) return;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n")) return;
  }
}

/* Reads and discards until the peer closes, or for at most ten seconds. */
static void _drain_until_eof(int c) {
  char buf[4096];
  for (;;) {
    struct pollfd p = {.fd = c, .events = POLLIN};
    if (poll(&p, 1, 10000) != 1) return;
    if (read(c, buf, sizeof(buf)) <= 0) return;
  }
}

/* Accepts one connection and refuses the body of its request, so that the
   client meets a peer that no longer reads while it still has body bytes to
   send.

   It reads the whole request head of a request that expects 100-continue,
   answers 100 Continue, and shuts its read side down. On a Unix domain
   socket that makes every later write of the client fail with EPIPE (the
   error that raises SIGPIPE) without first reporting an error or a hang-up
   to a poll of the client. Because the client sends no body byte before the
   answer, the first write of the body always meets the shut side. The
   server then waits for the client to close.

   With hold set it answers nothing: it keeps the request in flight until
   release is set, and then closes. */
static void *_refusing_server_main(void *arg) {
  refusing_server *s = arg;
  struct pollfd p = {.fd = s->listen_fd, .events = POLLIN};
  if (poll(&p, 1, 10000) == 1) {
    int c = accept(s->listen_fd, NULL, NULL);
    if (c >= 0) {
      static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
      _read_request_head(c);
      atomic_store(&s->accepted, 1);
      if (s->hold) {
        for (int i = 0; i < 10000 && !atomic_load(&s->release); ++i) {
          struct timespec ms = {0, 1000000L};
          nanosleep(&ms, NULL);
        }
        close(c);
        return NULL;
      }
      (void)send(c, cont, sizeof(cont) - 1, MSG_NOSIGNAL);
      shutdown(c, SHUT_RD);
      _drain_until_eof(c);
      close(c);
    }
  }
  return NULL;
}

static bool _refusing_server_start(refusing_server *s, const char *tag,
                                   bool hold) {
  memset(s, 0, sizeof(*s));
  s->listen_fd = -1;
  s->hold = hold;
  snprintf(s->path, sizeof(s->path), "/tmp/chttpcli_sigpipe_%s_%d.sock", tag,
           (int)getpid());
  unlink(s->path);
  /* The path holds only characters that need no escape, except '/'. */
  size_t o = (size_t)snprintf(s->url, sizeof(s->url), "http+unix://");
  for (const char *c = s->path; *c && o + 4 < sizeof(s->url); ++c) {
    if (*c == '/') {
      memcpy(s->url + o, "%2F", 3);
      o += 3;
    } else {
      s->url[o++] = *c;
    }
  }
  snprintf(s->url + o, sizeof(s->url) - o, "/upload");
  s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s->listen_fd < 0) return false;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, s->path, sizeof(addr.sun_path) - 1);
  if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(s->listen_fd, 4) != 0)
    return false;
  s->thread_started =
      pthread_create(&s->tid, NULL, _refusing_server_main, s) == 0;
  return s->thread_started;
}

static void _refusing_server_stop(refusing_server *s) {
  atomic_store(&s->release, 1);
  if (s->thread_started) pthread_join(s->tid, NULL);
  if (s->listen_fd >= 0) close(s->listen_fd);
  unlink(s->path);
}

extern int _chttpclient_engine_ref_count_for_tests(void);
extern bool _chttpclient_engine_running_for_tests(void);
extern void _chttpclient_engine_wait_for_quiescence_for_tests(void);

/* The engine stops on its own once its last user is gone, on a thread of its
   own. This waits, bounded, for that stop to finish, so that nothing of the
   engine is left at the exit of the process. */
static bool _wait_for_engine_stop(void) {
  for (int i = 0; i < 10000 && _chttpclient_engine_ref_count_for_tests() > 0;
       ++i) {
    struct timespec ms = {0, 1000000L};
    nanosleep(&ms, NULL);
  }
  if (_chttpclient_engine_ref_count_for_tests() > 0) return false;
  _chttpclient_engine_wait_for_quiescence_for_tests();
  return !_chttpclient_engine_running_for_tests();
}

#define SIGPIPE_TEST_BODY_LEN (64u * 1024u)

static chttp_request_t *_large_post(const char *url, char **body_out) {
  char *body = malloc(SIGPIPE_TEST_BODY_LEN);
  *body_out = body;
  if (!body) return NULL;
  memset(body, 'b', SIGPIPE_TEST_BODY_LEN);
  chttp_request_body_t b =
      CHTTP_BODY(body, SIGPIPE_TEST_BODY_LEN, "application/octet-stream");
  return chttp_request_new(CHTTP_POST, url, &b, NULL);
}

TEST(chttpcli_sigpipe, the_binary_runs_with_the_default_disposition) {
  /* Every other test here is vacuous under an ignored SIGPIPE. */
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}

TEST(chttpcli_sigpipe, a_sync_write_to_a_refusing_server_raises_no_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  refusing_server srv;
  bool server_up = _refusing_server_start(&srv, "sync", false);
  chttpcli cli = ccol_create_chttpclient(NULL);
  char *body = NULL;
  chttp_request_t *req = server_up ? _large_post(srv.url, &body) : NULL;
  if (req) req->expect_continue = true;

  sigset_t old;
  _block_sigpipe(&old);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = ccol_unexpected_failure;
  if (req && cli != CHTTPCLI_INVALID) rv = chttpclient_do(cli, req, &resp);
  bool raised = _take_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, &old, NULL);

  chttpclient_resp_free(resp);
  chttp_request_free(req);
  free(body);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  _refusing_server_stop(&srv);

  REQUIRE_TRUE(server_up);
  REQUIRE_TRUE(req != NULL);
  REQUIRE_EQ(atomic_load(&srv.accepted), 1);
  REQUIRE_EQ((int)rv, (int)ccol_http_transfer_aborted);
  REQUIRE_FALSE(raised);
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}

/* The async engine stops once no request is in flight, and a thread that
   exits takes a pending signal with it, so a second request, which a holding
   server keeps unanswered, keeps the engine and its threads alive until the
   scan of the pending signals has run. */
TEST(chttpcli_sigpipe,
     the_async_engine_keeps_the_disposition_and_no_write_raises_it) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  refusing_server srv, holder;
  bool server_up = _refusing_server_start(&srv, "async", false);
  bool holder_up = _refusing_server_start(&holder, "hold", true);
  chttpcli cli = ccol_create_chttpclient(NULL);
  char *body = NULL;
  chttp_request_t *req =
      server_up && holder_up ? _large_post(srv.url, &body) : NULL;
  if (req) req->expect_continue = true;
  chttp_request_t *hold_req =
      req ? chttp_request_new(CHTTP_GET, holder.url, NULL, NULL) : NULL;

  /* The engine starts on the first call, and every thread that it starts
     inherits this mask. */
  sigset_t old;
  _block_sigpipe(&old);
  ctpool_future *hold_f = NULL;
  if (hold_req && cli != CHTTPCLI_INVALID)
    hold_f = chttpclient_do_async(cli, hold_req);
  bool default_after_engine_start = _sigpipe_has_default_disposition();
  for (int i = 0; hold_f && i < 10000 && !atomic_load(&holder.accepted); ++i) {
    struct timespec ms = {0, 1000000L};
    nanosleep(&ms, NULL);
  }
  bool held = atomic_load(&holder.accepted) != 0;

  ctpool_future *f = NULL;
  if (held) f = chttpclient_do_async(cli, req);
  ccol_retval_t rv = ccol_unexpected_failure;
  if (f) {
    chttpcli_async_result_t *r = chttpclient_async_result_get(f);
    if (r) {
      rv = r->rv;
      chttpclient_resp_free(r->resp);
      chttpclient_async_result_free(r);
    }
    ctpool_future_free(f);
  }
  int threads_seen = 0;
  bool raised = _any_thread_has_sigpipe_pending(&threads_seen);

  atomic_store(&holder.release, 1);
  if (hold_f) {
    chttpcli_async_result_t *r = chttpclient_async_result_get(hold_f);
    if (r) {
      chttpclient_resp_free(r->resp);
      chttpclient_async_result_free(r);
    }
    ctpool_future_free(hold_f);
  }
  bool main_raised = _take_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, &old, NULL);

  chttp_request_free(hold_req);
  chttp_request_free(req);
  free(body);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  _refusing_server_stop(&holder);
  _refusing_server_stop(&srv);
  bool engine_stopped = _wait_for_engine_stop();

  REQUIRE_TRUE(server_up);
  REQUIRE_TRUE(holder_up);
  REQUIRE_TRUE(req != NULL);
  REQUIRE_TRUE(hold_req != NULL);
  REQUIRE_TRUE(hold_f != NULL);
  REQUIRE_TRUE(default_after_engine_start);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(f != NULL);
  REQUIRE_EQ(atomic_load(&srv.accepted), 1);
  REQUIRE_EQ((int)rv, (int)ccol_http_transfer_aborted);
  REQUIRE_GT(threads_seen, 1);
  REQUIRE_FALSE(raised);
  REQUIRE_FALSE(main_raised);
  REQUIRE_TRUE(engine_stopped);
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}
