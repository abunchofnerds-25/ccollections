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
#include <assert.h>
#include <cthreadcomm.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/cdeadline.h>
#include <internal/cpoll.h>
#include <internal/csock.h>
#include <internal/test_helpers/cthreadcomm.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_fds.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state the binary inherited before any test runs. The fork
// tests read the wait status of their children, which an inherited SIGCHLD
// disposition or mask would spoil (see test_reset_inherited_signals()).
__attribute__((constructor)) static void tests_default_sigchld(void) {
  test_reset_inherited_signals();
}

// Writes the n bytes of buf to fd, retrying on EINTR. The tests use it for the
// small signal writes into their own result and notify pipes, where EINTR is
// the only failure worth a retry. It consumes the return value because the
// fortified write(2) is warn_unused_result and a (void) cast doesn't silence
// it.
static void test_write_retry_eintr(int fd, const void *buf, size_t n) {
  ssize_t rv;
  do {
    rv = write(fd, buf, n);
  } while (rv < 0 && errno == EINTR);
}

// The microsecond-to-deadline conversion that every timed call uses. The
// expected values are written out by hand, never computed with the arithmetic
// under test.
TEST(ccol_timespec_add_us, carries_whole_seconds_and_nanoseconds) {
  {
    struct timespec t = {.tv_sec = 1, .tv_nsec = 600000000};
    ccol_timespec_add_us(&t, 2400000);
    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 0);
  }
  {
    struct timespec t = {.tv_sec = 1, .tv_nsec = 599999999};
    ccol_timespec_add_us(&t, 2400000);
    REQUIRE_EQ(t.tv_sec, 3);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }
  {
    struct timespec t = {.tv_sec = 7, .tv_nsec = 999999000};
    ccol_timespec_add_us(&t, 1);
    REQUIRE_EQ(t.tv_sec, 8);
    REQUIRE_EQ(t.tv_nsec, 0);
  }
  {
    struct timespec t = {.tv_sec = 7, .tv_nsec = 5};
    ccol_timespec_add_us(&t, 0);
    REQUIRE_EQ(t.tv_sec, 7);
    REQUIRE_EQ(t.tv_nsec, 5);
  }
}

TEST(ccol_us_to_ms_ceil, rounds_up_and_never_turns_a_positive_value_into_0) {
  REQUIRE_EQ(ccol_us_to_ms_ceil(0), (uint64_t)0);
  REQUIRE_EQ(ccol_us_to_ms_ceil(1), (uint64_t)1);
  REQUIRE_EQ(ccol_us_to_ms_ceil(999), (uint64_t)1);
  REQUIRE_EQ(ccol_us_to_ms_ceil(1000), (uint64_t)1);
  REQUIRE_EQ(ccol_us_to_ms_ceil(1001), (uint64_t)2);
  REQUIRE_EQ(ccol_us_to_ms_ceil(UINT64_MAX), (uint64_t)18446744073709552ULL);
  REQUIRE_EQ(ccol_us_to_s_ceil(1), (uint64_t)1);
  REQUIRE_EQ(ccol_us_to_s_ceil(1000000), (uint64_t)1);
  REQUIRE_EQ(ccol_us_to_s_ceil(1000001), (uint64_t)2);
}

// A passed deadline gives 0 (one last look), a deadline a fraction of a
// millisecond ahead gives 1, and a deadline at the end of time gives INT_MAX.
TEST(ccol_deadline_remaining_ms_ceil, rounds_up_and_saturates) {
  struct timespec past;
  clock_gettime(CLOCK_MONOTONIC, &past);
  past.tv_sec -= 1;
  REQUIRE_EQ(ccol_deadline_remaining_ms_ceil(&past), 0);

  struct timespec soon;
  REQUIRE_TRUE(ccol_deadline_after_us(500, &soon));
  int ms = ccol_deadline_remaining_ms_ceil(&soon);
  REQUIRE_LE(ms, 1);

  struct timespec later;
  REQUIRE_TRUE(ccol_deadline_after_us(10000000, &later));
  ms = ccol_deadline_remaining_ms_ceil(&later);
  REQUIRE_GT(ms, 9000);
  REQUIRE_LE(ms, 10000);

  struct timespec never;
  REQUIRE_TRUE(ccol_deadline_after_us(UINT64_MAX, &never));
  REQUIRE_EQ(ccol_deadline_remaining_ms_ceil(&never), INT_MAX);
}

// Circular queue tests
TEST(circular_queues, create_fails) {
  char *err_str = NULL;

  ccircq *cq = ccircq_create_mp(0, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccircq_create_mp(-1, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccircq_create_mp(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

// Both sizes are well inside ccol_max_elem_count, so only the guard against a
// max_size * sizeof(cmessage) that wraps can refuse them. Without it the first
// send corrupts the heap. The second size needs a 64-bit size_t.
TEST(circular_queues,
     create_rejects_max_size_that_would_overflow_the_backing_array_size) {
  char *err_str = NULL;

  size_t smallest_overflowing = SIZE_MAX / sizeof(cmessage) + 1;
  REQUIRE_LT(smallest_overflowing, ccol_max_elem_count);
  ccircq *cq = ccircq_create_mp(smallest_overflowing, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

#if SIZE_MAX > 0xFFFFFFFFu
  size_t known_bad = (size_t)1 << 60;
  REQUIRE_LT(known_bad, ccol_max_elem_count);
  err_str = NULL;
  cq = ccircq_create_mp(known_bad, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
#endif
}

// A large max_size whose allocation doesn't overflow must be accepted, so the
// overflow guard isn't too strict.
TEST(circular_queues, create_succeeds_with_large_non_overflowing_max_size) {
  char *err_str = NULL;
  size_t large_max_size = 1024 * 1024;
  ccircq *cq = ccircq_create_mp(large_max_size, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  ccircq_destroy(cq);
}

TEST(circular_queues, create_and_destroy_no_mem_procs) {
  char *err_str = "";

  ccircq *cq = ccircq_create_mp(1, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccircq_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, create_and_destroy_with_mem_procs) {
  char *err_str = "";

  ccircq *cq = ccircq_create_mp(
      1,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccircq_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, basic_send_and_receive_no_mem_procs) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2;
  REQUIRE_EQ(ccircq_recv(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  ccircq_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_with_mem_procs) {
  ccircq *cq = ccircq_create_mp(
      1,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2;
  REQUIRE_EQ(ccircq_recv(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  ccircq_destroy(cq);
}

TEST(circular_queues, msg_count) {
  ccircq *cq = ccircq_create_mp(3, NULL, NULL);

  cmessage m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(ccircq_msg_count(cq), i);
    ccircq_send(cq, &m1);
    REQUIRE_EQ(ccircq_msg_count(cq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(ccircq_msg_count(cq), i);
    ccircq_recv(cq, &m1);
    REQUIRE_EQ(ccircq_msg_count(cq), i - 1);
  }

  ccircq_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_NULL_msg) {
  ccircq *cq = ccircq_create_mp(3, NULL, NULL);

  cmessage m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  cmessage m2 = {.data = (void *)0xabcdef01, .size = 0x35};
  REQUIRE_EQ(ccircq_recv(cq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);
  REQUIRE_EQ(m2.size, 0);

  ccircq_destroy(cq);
}

TEST(circular_queues, try_send_and_try_receive) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccircq_try_send(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(ccircq_try_send(cq, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(ccircq_try_recv(cq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccircq_destroy(cq);
}

// getMonoTime(A) reads CLOCK_MONOTONIC into the struct timespec A, and
// diffTimeUSec(A, B) evaluates to the microseconds from A to B. The tests time
// their calls with them on the clock the timed calls wait on, so a step of the
// wall clock during a measurement can't move the result.
#define getMonoTime(A) clock_gettime(CLOCK_MONOTONIC, &A);
#define diffTimeUSec(A, B) \
  (B.tv_sec - A.tv_sec) * 1000000 + (B.tv_nsec - A.tv_nsec) / 1000

// Gives the CLOCK_MONOTONIC time in microseconds. The bounded waits of this
// file compute their deadlines from it.
static long long mono_now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

// Sleeps 1 ms between two polls of a hang guard. The guards poll on
// CLOCK_MONOTONIC instead of waiting on a condition variable with an absolute
// deadline, because such a deadline is on CLOCK_REALTIME on macOS and a step of
// the wall clock would cut it short or stretch it.
static void mono_poll_pause(void) {
  struct timespec ts = {0, 1000000L};
  nanosleep(&ts, NULL);
}

// The bound of a wait for something a correct build does within microseconds (a
// dispatch, a callback, a wake). It only stops a hang, so it is sized for a
// loaded VM under valgrind. A wait that checks that something does NOT happen
// keeps its own short window.
#define EVL_WAIT_MS 10000

TEST(circular_queues, timed_send_and_timed_receive) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  uint64_t timeout = 100000;

  struct timespec before;
  struct timespec after;

  getMonoTime(before);
  REQUIRE_EQ(ccircq_timed_send(cq, &m1, timeout), ccol_success);
  getMonoTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getMonoTime(before);
  REQUIRE_EQ(ccircq_timed_send(cq, &m1, timeout), ccol_timed_out);
  getMonoTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  cmessage m2 = {.data = NULL, .size = 0};

  getMonoTime(before);
  REQUIRE_EQ(ccircq_timed_recv(cq, &m2, timeout), ccol_success);
  getMonoTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getMonoTime(before);
  REQUIRE_EQ(ccircq_timed_recv(cq, &m1, timeout), ccol_timed_out);
  getMonoTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccircq_destroy(cq);
}

TEST(circular_queues, enable_disable_sending) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  ccircq_disable_sending(cq);

  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(ccircq_try_send(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(ccircq_timed_send(cq, &m1, 1000000), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(ccircq_timed_send(cq, &m1, 0), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  ccircq_enable_sending(cq);

  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(ccircq_recv(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  ccircq_destroy(cq);
}

TEST(circular_queues, ring_wrap_around) {
  ccircq *cq = ccircq_create_mp(2, NULL, NULL);

  for (int i = 0; i < 5; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = 'A' + i;
    cmessage out = {.data = buf, .size = 1};
    REQUIRE_EQ(ccircq_send(cq, &out), ccol_success);
    REQUIRE_EQ(out.data, NULL);

    cmessage in = {.data = NULL, .size = 0};
    REQUIRE_EQ(ccircq_recv(cq, &in), ccol_success);
    REQUIRE_EQ(*(char *)in.data, 'A' + i);
    free(in.data);
  }

  ccircq_destroy(cq);
}

// A timeout of 0 doesn't wait, so each timed call answers exactly what its try
// variant answers and never ccol_timed_out.
TEST(circular_queues, timed_calls_with_a_zero_timeout_are_the_try_variants) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  struct timespec before, after;
  getMonoTime(before);
  cmessage recv_m = {.data = NULL, .size = 0};
  ccol_retval empty_rv = ccircq_timed_recv(cq, &recv_m, 0);

  cmessage m = {.data = malloc(sizeof(char)), .size = 1};
  ccol_retval first_rv = ccircq_timed_send(cq, &m, 0);
  cmessage m2 = {.data = malloc(sizeof(char)), .size = 1};
  ccol_retval full_rv = ccircq_timed_send(cq, &m2, 0);
  void *kept = m2.data;
  getMonoTime(after);

  cmessage got = {.data = NULL, .size = 0};
  ccol_retval got_rv = ccircq_timed_recv(cq, &got, 0);
  free(got.data);
  free(m2.data);
  ccol_retval null_rv = ccircq_timed_recv(NULL, &got, 0);
  ccircq_destroy(cq);

  REQUIRE_EQ(empty_rv, ccol_container_empty);
  REQUIRE_EQ(first_rv, ccol_success);
  REQUIRE_EQ(full_rv, ccol_container_full);
  REQUIRE_TRUE(kept != NULL);
  REQUIRE_EQ(got_rv, ccol_success);
  REQUIRE_EQ(null_rv, ccol_invalid_args);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
}

// A forced wait error with the queue still full gives ccol_unexpected_failure
// quickly: The loop must neither hide the error nor retry for ever.
TEST(circular_queues, timed_send_reports_unexpected_failure_on_condvar_error) {
  ccircq *cq = ccircq_create(1, NULL);
  cmessage filler = {.data = malloc(1), .size = 1};
  REQUIRE_EQ(ccircq_try_send(cq, &filler), ccol_success);

  ccircq__test_force_next_send_condvar_wait_error();

  cmessage m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getMonoTime(before);
  errno = 0;
  ccol_retval send_rv = ccircq_timed_send(cq, &m, timeout);
  int send_errno = errno;
  getMonoTime(after);
  REQUIRE_EQ(send_rv, ccol_unexpected_failure);
  REQUIRE_EQ(send_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_NE(m.data, NULL);
  free(m.data);

  cmessage drained = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &drained), ccol_success);
  free(drained.data);
  ccircq_destroy(cq);
}

// A consumer can free a slot at the same instant the wait fails, and the wait
// always locks the mutex again before returning. The send must then go through
// instead of reporting ccol_unexpected_failure. The hook makes the race
// deterministic.
TEST(circular_queues, timed_send_condvar_error_racing_freed_slot_still_sends) {
  ccircq *cq = ccircq_create(1, NULL);
  cmessage filler = {.data = malloc(sizeof(int)), .size = sizeof(int)};
  *(int *)filler.data = 42;
  REQUIRE_EQ(ccircq_try_send(cq, &filler), ccol_success);

  ccircq__test_force_next_send_condvar_wait_error_racing_ready();

  cmessage m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccircq_timed_send(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);

  cmessage raced_out = ccircq__test_take_race_freed_msg();
  REQUIRE_NE(raced_out.data, NULL);
  REQUIRE_EQ(*(int *)raced_out.data, 42);
  free(raced_out.data);

  REQUIRE_EQ(ccircq_msg_count(cq), (size_t)1);
  cmessage recv_m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &recv_m), ccol_success);
  free(recv_m.data);
  ccircq_destroy(cq);
}

// The receive-side mirror: A forced error on an empty queue with no race gives
// ccol_unexpected_failure quickly.
TEST(circular_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  ccircq *cq = ccircq_create(1, NULL);

  ccircq__test_force_next_recv_condvar_wait_error();

  cmessage m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getMonoTime(before);
  errno = 0;
  ccol_retval recv_rv = ccircq_timed_recv(cq, &m, timeout);
  int recv_errno = errno;
  getMonoTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  ccircq_destroy(cq);
}

// A producer can complete a send at the same instant the wait fails. The
// receive must return that message; giving up leaves it stuck in the queue for
// ever.
TEST(circular_queues, timed_recv_condvar_error_racing_message_still_receives) {
  ccircq *cq = ccircq_create(4, NULL);

  ccircq__test_force_next_recv_condvar_wait_error_racing_ready();

  cmessage m = {.data = (void *)1, .size = 1};
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccircq_timed_recv(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(ccircq_msg_count(cq), (size_t)0);

  ccircq_destroy(cq);
}

// The operation that makes progress possible must release a blocked waiter, so
// the waiter never sleeps until its own timeout. The queues signal only when
// the waiter count says somebody is parked, so a wait site that doesn't count
// itself is invisible. These tests poll until the waiter has parked, then wake
// it, and allow far less time for the wake than the wait's own timeout. The
// three bounds are the blocked call's timeout, the time to see it parked and
// the time to see it released.
#define CCOL_WAKE_WAIT_TIMEOUT_SEC 10
#define CCOL_WAKE_PARK_BOUND_US 5000000L
#define CCOL_WAKE_WAKE_BOUND_US 3000000L

// Shared by a blocked waiter thread and the test that wakes it.
typedef struct {
  ccircq *cq;
  cdynmq *dq;
  cmessage msg;
  ccol_retval retval;
  atomic_bool finished;
} ccol_wake_args;

// Thread bodies of the wake tests: Each makes one long timed call on a->cq or
// a->dq, so the test has a waiter to park and release. They store the result in
// a->retval and then set a->finished.
static void *_wake_circq_timed_recv(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = ccircq_timed_recv(a->cq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

static void *_wake_circq_timed_send(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = ccircq_timed_send(a->cq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

static void *_wake_dynmq_timed_recv(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = cdynmq_timed_recv(a->dq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

// Sleeps 0.2 ms between two polls of the helpers below. Those helpers poll the
// queue's waiter count until a waiter has parked, or a->finished until it
// returned, so the wake tests know when to act. Each returns false once its
// bound passes, so a regression fails the run instead of hanging it.
static void _wake_poll_pause(void) {
  struct timespec s = {.tv_sec = 0, .tv_nsec = 200000};
  nanosleep(&s, NULL);
}

static bool _wake_await_circq_readers(ccircq *cq) {
  struct timespec start, now;
  getMonoTime(start);
  for (;;) {
    if (ccircq__waiting_readers_for_tests(cq) >= 1) return true;
    getMonoTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_circq_writers(ccircq *cq) {
  struct timespec start, now;
  getMonoTime(start);
  for (;;) {
    if (ccircq__waiting_writers_for_tests(cq) >= 1) return true;
    getMonoTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_dynmq_readers(cdynmq *dq) {
  struct timespec start, now;
  getMonoTime(start);
  for (;;) {
    if (cdynmq__waiting_readers_for_tests(dq) >= 1) return true;
    getMonoTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_finished(ccol_wake_args *a) {
  struct timespec start, now;
  getMonoTime(start);
  for (;;) {
    if (atomic_load(&a->finished)) return true;
    getMonoTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_WAKE_BOUND_US) return false;
    _wake_poll_pause();
  }
}

TEST(circular_queues, blocked_timed_recv_is_released_by_a_send) {
  ccircq *cq = ccircq_create(4, NULL);
  REQUIRE_NE((void *)cq, NULL);

  void *payload = malloc(4);
  ccol_wake_args a = {.cq = cq,
                      .dq = NULL,
                      .msg = {.data = NULL, .size = 0},
                      .retval = ccol_unexpected_failure};
  atomic_init(&a.finished, false);

  pthread_t tid;
  bool started = (pthread_create(&tid, NULL, _wake_circq_timed_recv, &a) == 0);
  bool parked = false, released = false, matched = false;
  ccol_retval sent = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_circq_readers(cq);
    cmessage m = {.data = payload, .size = 4};
    sent = ccircq_send(cq, &m);
    if (sent != ccol_success) free(payload);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    matched = (a.msg.data == payload);
    free(a.msg.data);
  } else {
    free(payload);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = ccircq__waiting_readers_for_tests(cq);
  ccol_retval got = a.retval;
  ccircq_destroy(cq);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(sent, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(matched);
  REQUIRE_EQ(residual, (size_t)0);
}

TEST(circular_queues, blocked_timed_send_is_released_by_a_receive) {
  ccircq *cq = ccircq_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);

  void *first = malloc(4);
  cmessage filler = {.data = first, .size = 4};
  ccol_retval filled = ccircq_send(cq, &filler);
  if (filled != ccol_success) free(first);

  void *payload = malloc(4);
  ccol_wake_args a = {.cq = cq,
                      .dq = NULL,
                      .msg = {.data = payload, .size = 4},
                      .retval = ccol_unexpected_failure};
  atomic_init(&a.finished, false);

  pthread_t tid;
  bool started = (pthread_create(&tid, NULL, _wake_circq_timed_send, &a) == 0);
  bool parked = false, released = false, drained = false, matched = false;
  cmessage out = {.data = NULL, .size = 0};
  ccol_retval recvd = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_circq_writers(cq);
    recvd = ccircq_recv(cq, &out);
    if (recvd == ccol_success) free(out.data);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    cmessage tail = {.data = NULL, .size = 0};
    drained = (ccircq_try_recv(cq, &tail) == ccol_success);
    matched = (tail.data == payload);
    free(tail.data);
  } else {
    free(payload);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = ccircq__waiting_writers_for_tests(cq);
  ccol_retval got = a.retval;
  ccircq_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(recvd, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(drained);
  REQUIRE_TRUE(matched);
  REQUIRE_EQ(residual, (size_t)0);
}

TEST(dynamic_queues, blocked_timed_recv_is_released_by_a_send) {
  cdynmq *dq = cdynmq_create(NULL);
  REQUIRE_NE((void *)dq, NULL);

  void *payload = malloc(4);
  ccol_wake_args a = {.cq = NULL,
                      .dq = dq,
                      .msg = {.data = NULL, .size = 0},
                      .retval = ccol_unexpected_failure};
  atomic_init(&a.finished, false);

  pthread_t tid;
  bool started = (pthread_create(&tid, NULL, _wake_dynmq_timed_recv, &a) == 0);
  bool parked = false, released = false, matched = false;
  ccol_retval sent = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_dynmq_readers(dq);
    cmessage m = {.data = payload, .size = 4};
    sent = cdynmq_send(dq, &m);
    if (sent != ccol_success) free(payload);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    matched = (a.msg.data == payload);
    free(a.msg.data);
  } else {
    free(payload);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (cdynmq_try_recv(dq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = cdynmq__waiting_readers_for_tests(dq);
  ccol_retval got = a.retval;
  cdynmq_destroy(dq);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(sent, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(matched);
  REQUIRE_EQ(residual, (size_t)0);
}

// Turning sends off doesn't stop a receive from draining what is queued, and an
// empty queue then times out instead of answering ccol_not_permitted.
TEST(circular_queues, recv_drains_successfully_after_disable) {
  ccircq *cq = ccircq_create_mp(2, NULL, NULL);

  cmessage m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_send(cq, &m), ccol_success);
  REQUIRE_EQ(ccircq_send(cq, &m), ccol_success);

  ccircq_disable_sending(cq);

  REQUIRE_EQ(ccircq_recv(cq, &m), ccol_success);
  REQUIRE_EQ(ccircq_recv(cq, &m), ccol_success);

  uint64_t timeout = 50000;
  REQUIRE_EQ(ccircq_timed_recv(cq, &m, timeout), ccol_timed_out);

  ccircq_destroy(cq);
}

// A receiver blocked on an empty queue and what its receive returned.
typedef struct {
  ccircq *cq;
  ccol_retval result;
} cq_disable_recv_args;

// Blocks in ccircq_recv() on a->cq and stores the result in a->result, so the
// test can check that turning sends off doesn't release it.
void *cq_blocking_recv_thread(void *raw) {
  cq_disable_recv_args *a = (cq_disable_recv_args *)raw;
  cmessage m = {.data = NULL, .size = 0};
  a->result = ccircq_recv(a->cq, &m);
  return NULL;
}

// Turning sends off doesn't wake a blocked receiver; it returns only once sends
// are back on and a message arrives.
TEST(circular_queues, disable_sending_does_not_unblock_recv) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  cq_disable_recv_args args = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_blocking_recv_thread, &args), 0);

  usleep(20000);
  ccircq_disable_sending(cq);
  usleep(20000);

  ccircq_enable_sending(cq);
  cmessage m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_send(cq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  ccircq_destroy(cq);
}

// Sends off on an empty queue doesn't end a timed receive early; it waits out
// its timeout and reports ccol_timed_out.
TEST(circular_queues, timed_recv_times_out_when_disabled) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  ccircq_disable_sending(cq);

  uint64_t timeout = 100000;
  struct timespec before, after;
  cmessage m = {.data = NULL, .size = 0};

  getMonoTime(before);
  REQUIRE_EQ(ccircq_timed_recv(cq, &m, timeout), ccol_timed_out);
  getMonoTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);

  ccircq_destroy(cq);
}

// Receives the messages "A" and "B" from the queue after a 50 ms delay, so the
// sender of send_and_receive_thread blocks on its second message and is then
// released.
void *cq_helper_thread(void *args) {
  ccircq *cq = (ccircq *)args;
  usleep(50000);

  cmessage m = {.data = NULL, .size = 0};
  assert(ccircq_recv(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  assert(ccircq_recv(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'B');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(circular_queues, send_and_receive_thread) {
  ccircq *cq = ccircq_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_helper_thread, cq), 0);

  cmessage m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(ccircq_send(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);

  m = (cmessage){.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'B';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(ccircq_send(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);

  pthread_join(tid, NULL);

  ccircq_destroy(cq);
}

// Reads the short pseudo-file /proc/<pid>/<name> into buf (buf_cap bytes),
// NUL-terminated, for the stuck-child dump below. It is a best-effort
// diagnostic that runs after a wait timed out, so it returns false on any
// failure and must never hang or crash.
static bool _read_proc_file(pid_t pid, const char *name, char *buf,
                            size_t buf_cap) {
  char path[PATH_MAX];
  int len = snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name);
  if (len < 0 || (size_t)len >= sizeof(path)) return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t n = read(fd, buf, buf_cap - 1);
  close(fd);
  if (n < 0) return false;
  buf[n] = '\0';
  return true;
}

// Prints what /proc says about pid and each of its threads, so a hang in CI
// leaves a diagnosis in the log. It runs after a bounded wait for pid timed out
// and reads status, wchan, syscall and stat one at a time, so a missing file
// doesn't hide the others. The State: line tells a zombie that waitpid() didn't
// see (an emulator problem) from a child stuck in a syscall.
static void _dump_stuck_child_diagnostics(pid_t pid) {
  char buf[4096];
  fprintf(stderr,
          "[STUCK_CHILD_DIAG] parent pid=%d timed out waiting for child "
          "pid=%d; dumping /proc diagnostics\n",
          (int)getpid(), (int)pid);

  static const char *const files[] = {"status", "wchan", "syscall", "stat"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (_read_proc_file(pid, files[i], buf, sizeof(buf))) {
      fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid,
              files[i], buf);
    } else {
      fprintf(stderr,
              "[STUCK_CHILD_DIAG] /proc/%d/%s: unreadable (errno=%d %s)\n",
              (int)pid, files[i], errno, strerror(errno));
    }
  }

  char task_dir[64];
  snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", (int)pid);
  DIR *td = opendir(task_dir);
  if (!td) {
    fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/task: unreadable\n", (int)pid);
  } else {
    struct dirent *ent;
    while ((ent = readdir(td)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      char rel[16 + sizeof(ent->d_name)];
      snprintf(rel, sizeof(rel), "task/%s/status", ent->d_name);
      if (_read_proc_file(pid, rel, buf, sizeof(buf))) {
        fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid, rel,
                buf);
      }
    }
    closedir(td);
  }
  fflush(stderr);
}

// Waits for pid like waitpid(pid, status_out, 0), but for at most timeout_ms.
// Under qemu-user a parent's waitpid() can hang even though the child finished,
// and the child's alarm() can't help the parent, so every parent-side wait of
// this file goes through here. Returns true with *status_out set, or false on a
// timeout, after it dumped the diagnostics and tried to SIGKILL and reap the
// child so it doesn't outlive the test.
static bool _wait_for_forked_child_bounded(pid_t pid, int *status_out,
                                           int timeout_ms) {
  enum { POLL_INTERVAL_MS = 50 };
  int elapsed_ms = 0;
  while (elapsed_ms < timeout_ms) {
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid) return true;
    if (r == -1) return false;
    struct timespec ts = {.tv_sec = POLL_INTERVAL_MS / 1000,
                          .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    elapsed_ms += POLL_INTERVAL_MS;
  }

  _dump_stuck_child_diagnostics(pid);

  kill(pid, SIGKILL);
  for (int i = 0; i < 20; i++) {
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid || r == -1) break;
    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

// The callers of the helper below sit in the CCOL_FORK_SAFETY_REQUIRED block
// further down, so the helper is under the same switch; otherwise a build with
// the switch at 0 fails on -Wunused-function.
#if CCOL_FORK_SAFETY_REQUIRED
// Reads the result byte a forked child writes into fd, waiting at most
// timeout_ms. The child's alarm() closes the pipe on a hang, but under
// qemu-user that alarm can be wedged too. Stores the byte in *out_byte and
// returns 1, or returns 0 for EOF or a timeout, so the caller's
// REQUIRE_EQ((int)n, 1) fails one test instead of hanging the binary.
static ssize_t _read_result_byte_bounded(int fd, char *out_byte,
                                         int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000L +
                        (deadline.tv_nsec - now.tv_nsec) / 1000000L;
    if (remaining_ms <= 0) return 0;

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r = poll(&pfd, 1, (int)remaining_ms);
    if (r == 0) return 0;
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    return read(fd, out_byte, 1);
  }
}
#endif

// Destroying a queue that a cselect() or ceventloop waiter is still linked to
// leaves that waiter pointing into freed memory, so the destroy asserts on the
// waiter lists. It runs in a forked child because the assert ends the process.
TEST(circular_queues, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(10);
    ccircq *cq = ccircq_create(4, NULL);
    if (!cq) test_child_exit(2);
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);
    ceventloop_handlers handlers = {0};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers, NULL, &err);
    if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);
    ccircq_destroy(cq);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A shut-down loop's registration still names the queue until a remove or the
// loop's destroy, so destroying the queue first is the same misuse and must
// abort. A destroy that looks only at the waiter lists, which the shutdown
// emptied, lets the child exit 0.
TEST(ceventloop, queue_destroy_after_shutdown_with_a_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int signals[2] = {0, 0};
  bool reaped_all = true;
  for (int kind = 0; kind < 2; kind++) {
    pid_t pid = fork();
    if (pid == 0) {
      test_child_no_core();
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      alarm(10);
      ccircq *cq = NULL;
      cdynmq *dq = NULL;
      if (kind == 0)
        cq = ccircq_create(4, NULL);
      else
        dq = cdynmq_create(NULL);
      if (!cq && !dq) test_child_exit(2);
      ceventloop loop = ceventloop_create(8, 1, 1, NULL);
      if (loop == CEVENTLOOP_NULL) test_child_exit(2);
      ceventloop_handlers handlers = {0};
      cselectable sel = cq ? ccircq_selectable(cq, cselect_read)
                           : cdynmq_selectable(dq, cselect_read);
      ceventloop_reg reg = ceventloop_add(loop, sel, handlers, NULL, NULL);
      if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);
      if (ceventloop_shutdown(loop) != ccol_success) test_child_exit(2);
      if (cq)
        ccircq_destroy(cq);
      else
        cdynmq_destroy(dq);
      test_child_exit(0);
    }
    if (pid == -1) {
      reaped_all = false;
      continue;
    }
    int status = 0;
    bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
    if (!reaped) {
      reaped_all = false;
      continue;
    }
    signals[kind] = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  }
  REQUIRE_TRUE(reaped_all);
  REQUIRE_EQ(signals[0], SIGABRT);
  REQUIRE_EQ(signals[1], SIGABRT);
}

// The queue that cq_select_waiter_thread() waits on.
typedef struct cq_select_waiter_args {
  ccircq *cq;
} cq_select_waiter_args;

// Waits in cselect_timed() on a->cq with a 5 s timeout, the watcher that makes
// the queue's destroy fatal. The destroy ends the process before it returns.
static void *cq_select_waiter_thread(void *arg) {
  cq_select_waiter_args *a = (cq_select_waiter_args *)arg;
  size_t idx;
  cselect_timed_va(&idx, 5000000, ccircq_selectable(a->cq, cselect_read));
  return NULL;
}

// The child polls until the waiter thread is really linked into the queue's
// read-waiter list before the destroy. Without that a destroy that runs first
// finds nothing, frees the queue, and the late waiter uses freed memory.
TEST(circular_queues, destroy_while_cselect_is_watching_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    alarm(10);
    ccircq *cq = ccircq_create(4, NULL);
    if (!cq) test_child_exit(2);

    cq_select_waiter_args wargs = {.cq = cq};
    pthread_t waiter;
    if (pthread_create(&waiter, NULL, cq_select_waiter_thread, &wargs) != 0)
      test_child_exit(4);

    bool linked = false;
    for (int i = 0; i < 800 && !linked; i++) {
      linked = ccircq__test_has_sel_read_waiter_for_tests(cq);
      if (!linked) {
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
      }
    }
    if (!linked) test_child_exit(3);

    ccircq_destroy(cq);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Dynamic queue tests
TEST(dynamic_queues, create_fails) {
  char *err_str = "";

  cdynmq *dq = cdynmq_create_mp(
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)dq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cdynmq_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_no_mprocs) {
  char *err_str = "";

  cdynmq *dq = cdynmq_create_mp(NULL, &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  cdynmq_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_with_mp) {
  char *err_str = "";

  cdynmq *dq = cdynmq_create_mp(
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  cdynmq_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, basic_send_and_receive_no_mprocs) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  cdynmq_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_with_mp) {
  cdynmq *dq = cdynmq_create_mp(
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  cdynmq_destroy(dq);
}

TEST(dynamic_queues, msg_count) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(cdynmq_msg_count(dq), i);
    cdynmq_send(dq, &m1);
    REQUIRE_EQ(cdynmq_msg_count(dq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(cdynmq_msg_count(dq), i);
    cdynmq_recv(dq, &m1);
    REQUIRE_EQ(cdynmq_msg_count(dq), i - 1);
  }

  cdynmq_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_NULL_msg) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 0;
  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  cdynmq_destroy(dq);
}

TEST(dynamic_queues, fifo_ordering) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  const char labels[] = {'A', 'B', 'C'};
  for (int i = 0; i < 3; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = labels[i];
    cmessage m = {.data = buf, .size = 1};
    REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);
    REQUIRE_EQ(m.data, NULL);
  }
  REQUIRE_EQ(cdynmq_msg_count(dq), 3);

  for (int i = 0; i < 3; ++i) {
    cmessage m = {.data = NULL, .size = 0};
    REQUIRE_EQ(cdynmq_recv(dq, &m), ccol_success);
    REQUIRE_EQ(*(char *)m.data, labels[i]);
    free(m.data);
  }

  cdynmq_destroy(dq);
}

TEST(dynamic_queues, send_and_try_receive) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_try_recv(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(cdynmq_try_recv(dq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  cdynmq_destroy(dq);
}

TEST(dynamic_queues, send_and_timed_receive) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  uint64_t timeout = 100000;

  struct timespec before;
  struct timespec after;

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);

  cmessage m2 = {.data = NULL, .size = 0};

  getMonoTime(before);
  REQUIRE_EQ(cdynmq_timed_recv(dq, &m2, timeout), ccol_success);
  getMonoTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getMonoTime(before);
  REQUIRE_EQ(cdynmq_timed_recv(dq, &m1, timeout), ccol_timed_out);
  getMonoTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  cdynmq_destroy(dq);
}

// A forced wait error on an empty queue with no race gives
// ccol_unexpected_failure quickly.
TEST(dynamic_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  cdynmq *dq = cdynmq_create(NULL);

  cdynmq__test_force_next_recv_condvar_wait_error();

  cmessage m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getMonoTime(before);
  errno = 0;
  ccol_retval recv_rv = cdynmq_timed_recv(dq, &m, timeout);
  int recv_errno = errno;
  getMonoTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  cdynmq_destroy(dq);
}

// A producer can put a message in at the same instant the wait fails. The
// receive must return it instead of reporting ccol_unexpected_failure.
TEST(dynamic_queues, timed_recv_condvar_error_racing_message_still_receives) {
  cdynmq *dq = cdynmq_create(NULL);

  cdynmq__test_force_next_recv_condvar_wait_error_racing_ready();

  cmessage m = {.data = (void *)1, .size = 1};
  uint64_t timeout = 5000000;
  REQUIRE_EQ(cdynmq_timed_recv(dq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(cdynmq_msg_count(dq), (size_t)0);

  cdynmq_destroy(dq);
}

TEST(dynamic_queues, enable_disable_sending) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  cdynmq_disable_sending(dq);

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  cdynmq_enable_sending(dq);

  REQUIRE_EQ(cdynmq_send(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(cdynmq_recv(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  cdynmq_destroy(dq);
}

TEST(dynamic_queues, timed_recv_with_a_zero_timeout_is_the_try_variant) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  struct timespec before, after;
  getMonoTime(before);
  cmessage m = {.data = NULL, .size = 0};
  ccol_retval empty_rv = cdynmq_timed_recv(dq, &m, 0);
  getMonoTime(after);
  cmessage sent = {.data = NULL, .size = 0};
  ccol_retval send_rv = cdynmq_send(dq, &sent);
  ccol_retval got_rv = cdynmq_timed_recv(dq, &m, 0);
  ccol_retval null_rv = cdynmq_timed_recv(NULL, &m, 0);
  cdynmq_destroy(dq);

  REQUIRE_EQ(empty_rv, ccol_container_empty);
  REQUIRE_EQ(send_rv, ccol_success);
  REQUIRE_EQ(got_rv, ccol_success);
  REQUIRE_EQ(null_rv, ccol_invalid_args);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
}

// Turning sends off doesn't stop a receive from draining what is queued, and an
// empty queue then times out instead of answering ccol_not_permitted.
TEST(dynamic_queues, recv_drains_successfully_after_disable) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cmessage m = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);
  REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);

  cdynmq_disable_sending(dq);

  REQUIRE_EQ(cdynmq_recv(dq, &m), ccol_success);
  REQUIRE_EQ(cdynmq_recv(dq, &m), ccol_success);

  uint64_t timeout = 50000;
  REQUIRE_EQ(cdynmq_timed_recv(dq, &m, timeout), ccol_timed_out);

  cdynmq_destroy(dq);
}

// A receiver blocked on an empty queue and what its receive returned.
typedef struct {
  cdynmq *dq;
  ccol_retval result;
} dq_disable_recv_args;

// Blocks in cdynmq_recv() on a->dq and stores the result in a->result, so the
// test can check that turning sends off doesn't release it.
void *dq_blocking_recv_thread(void *raw) {
  dq_disable_recv_args *a = (dq_disable_recv_args *)raw;
  cmessage m = {.data = NULL, .size = 0};
  a->result = cdynmq_recv(a->dq, &m);
  return NULL;
}

// Turning sends off doesn't wake a blocked receiver; it returns only once sends
// are back on and a message arrives.
TEST(dynamic_queues, disable_sending_does_not_unblock_recv) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  dq_disable_recv_args args = {.dq = dq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_blocking_recv_thread, &args), 0);

  usleep(20000);
  cdynmq_disable_sending(dq);
  usleep(20000);

  cdynmq_enable_sending(dq);
  cmessage m = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  cdynmq_destroy(dq);
}

// Sends off on an empty queue doesn't end a timed receive early; it waits out
// its timeout and reports ccol_timed_out.
TEST(dynamic_queues, timed_recv_times_out_when_disabled) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  cdynmq_disable_sending(dq);

  uint64_t timeout = 100000;
  struct timespec before, after;
  cmessage m = {.data = NULL, .size = 0};

  getMonoTime(before);
  REQUIRE_EQ(cdynmq_timed_recv(dq, &m, timeout), ccol_timed_out);
  getMonoTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);

  cdynmq_destroy(dq);
}

// Receives the message "A" from the queue and checks it, the other end of the
// dynamic queue send_and_receive_thread test.
void *dq_helper_thread(void *args) {
  cdynmq *dq = (cdynmq *)args;

  cmessage m = {.data = NULL, .size = 0};
  assert(cdynmq_recv(dq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(dynamic_queues, send_and_receive_thread) {
  cdynmq *dq = cdynmq_create_mp(NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_helper_thread, dq), 0);

  usleep(50000);

  cmessage m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);

  pthread_join(tid, NULL);

  cdynmq_destroy(dq);
}

// The cdynmq version of the circular queue test of the same name.
TEST(dynamic_queues, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cdynmq *dq = cdynmq_create(NULL);
    if (!dq) test_child_exit(2);
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);
    ceventloop_handlers handlers = {0};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cdynmq_selectable(dq, cselect_read), handlers, NULL, &err);
    if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);
    cdynmq_destroy(dq);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Channel tests
TEST(channels, create_fails) {
  char *err_str = NULL;

  cchan *ch = cchan_create_mp(0, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = cchan_create_mp(-1, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = cchan_create_mp(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = NULL, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(channels, create_and_destroy_no_mprocs) {
  char *err_str = "";

  cchan *ch = cchan_create_mp(1, NULL, &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  cchan_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

TEST(channels, create_and_destroy_with_mp) {
  char *err_str = "";

  cchan *ch = cchan_create_mp(
      1,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  cchan_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

// The worker side of the basic channel test: Receives "A" and sends it back to
// the owner. Helper threads check with assert(), since tau's assertions aren't
// thread-safe.
void *thr_for_channels_basic_send_and_receive(void *args) {
  cchan *ch = (cchan *)args;

  cmessage msg = {.data = NULL, .size = 0};

  assert(cchan_recv(ch, &msg) == ccol_success);

  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  assert(cchan_send(ch, &msg) == ccol_success);

  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, basic_send_and_receive_no_mprocs) {
  cchan *ch = cchan_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  cmessage m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(cchan_send(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cchan_recv(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  cchan_destroy(ch);
}

TEST(channels, basic_send_and_receive_with_mp) {
  cchan *ch = cchan_create_mp(
      1,
      &(ccol_memmgmt_procs){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  cmessage m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(cchan_send(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(cchan_recv(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  cchan_destroy(ch);
}

// The worker side of the channel msg_count test: Takes the three queued
// messages and sends three back, checking the count of each direction after
// every step.
void *thr_for_channels_msg_count(void *args) {
  cchan *ch = (cchan *)args;

  cmessage msg = {.data = NULL, .size = 0};

  for (size_t i = 3; i > 0; --i) {
    assert(cchan_msg_count(ch, cchan_owner_to_workers) == i);
    cchan_recv(ch, &msg);
    assert(cchan_msg_count(ch, cchan_owner_to_workers) == i - 1);
  }

  for (size_t i = 0; i < 3; ++i) {
    assert(cchan_msg_count(ch, cchan_workers_to_owner) == i);
    cchan_send(ch, &msg);
    assert(cchan_msg_count(ch, cchan_workers_to_owner) == i + 1);
  }

  return NULL;
}

// The worker never blocks (capacity 3, three messages each way), so the join is
// bounded and the counts after it don't depend on its speed.
TEST(channels, msg_count) {
  cchan *ch = cchan_create_mp(3, NULL, NULL);

  cmessage m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(cchan_msg_count(ch, cchan_owner_to_workers), i);
    cchan_send(ch, &m1);
    REQUIRE_EQ(cchan_msg_count(ch, cchan_owner_to_workers), i + 1);
  }

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_channels_msg_count, ch), 0);

  pthread_join(tid, NULL);

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(cchan_msg_count(ch, cchan_workers_to_owner), i);
    cchan_recv(ch, &m1);
    REQUIRE_EQ(cchan_msg_count(ch, cchan_workers_to_owner), i - 1);
  }

  cchan_destroy(ch);
}

// The two channel tests below take turns through phase, never through a sleep,
// which can end late on a busy runner. The worker records the line of its first
// failed check in failed_line, and the main thread asserts it after the join.
typedef struct {
  cchan *ch;
  atomic_int phase;
  atomic_int failed_line;
} chan_turns_t;

// Checks cond in a channel worker. A failed check stores __LINE__ in
// t->failed_line unless an earlier one is there already, so the main thread can
// report it after the join instead of the worker ending the binary.
#define CHAN_CHECK(t, cond)                                                \
  do {                                                                     \
    if (!(cond)) {                                                         \
      int _zero = 0;                                                       \
      atomic_compare_exchange_strong(&(t)->failed_line, &_zero, __LINE__); \
    }                                                                      \
  } while (0)

// Waits until t->phase reaches want, for at most 10 s, so the channel worker
// and the main thread take turns without a sleep. Returns false on a timeout.
static bool chan_wait_phase(chan_turns_t *t, int want) {
  for (int i = 0; i < 10000; i++) {
    if (atomic_load(&t->phase) >= want) return true;
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

// The worker side of the channel try test: Once t->phase allows it, it checks
// try_recv and try_send against the states the main thread set up, recording a
// failed check through CHAN_CHECK.
void *thr_for_channels_try_send_and_try_receive(void *args) {
  chan_turns_t *t = (chan_turns_t *)args;
  if (!chan_wait_phase(t, 1)) {
    CHAN_CHECK(t, false);
    return NULL;
  }

  cmessage msg = {.data = NULL, .size = 0};
  CHAN_CHECK(t, cchan_try_recv(t->ch, &msg) == ccol_success);
  CHAN_CHECK(t, msg.data != NULL && *((char *)msg.data) == 'A');
  if (msg.data) *((char *)msg.data) = 'B';

  cmessage m2 = {.data = NULL, .size = 0};
  CHAN_CHECK(t, cchan_try_recv(t->ch, &m2) == ccol_container_empty);
  CHAN_CHECK(t, m2.data == NULL);

  if (msg.data) {
    CHAN_CHECK(t, cchan_try_send(t->ch, &msg) == ccol_success);
    CHAN_CHECK(t, msg.data == NULL);
    free(msg.data);
  }

  m2 = (cmessage){.data = malloc(sizeof(char)), .size = 1};
  CHAN_CHECK(t, cchan_try_send(t->ch, &m2) == ccol_container_full);
  CHAN_CHECK(t, m2.data != NULL);
  free(m2.data);

  atomic_store(&t->phase, 2);
  return NULL;
}

TEST(channels, try_send_and_try_receive) {
  chan_turns_t t = {.ch = cchan_create_mp(1, NULL, NULL)};
  REQUIRE_NE((void *)t.ch, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_try_send_and_try_receive, &t),
      0);

  cmessage m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  ccol_retval first_send = cchan_try_send(t.ch, &m1);
  bool first_taken = m1.data == NULL;
  free(m1.data);

  m1 = (cmessage){.data = malloc(sizeof(char)), .size = 1};
  ccol_retval full_send = cchan_try_send(t.ch, &m1);
  bool full_kept = m1.data != NULL;
  free(m1.data);
  m1.data = NULL;

  atomic_store(&t.phase, 1);
  bool worker_done = chan_wait_phase(&t, 2);

  cmessage m2 = {.data = NULL, .size = 0};
  ccol_retval got = cchan_try_recv(t.ch, &m2);
  char got_char = m2.data ? *(char *)m2.data : 0;
  ccol_retval empty = cchan_try_recv(t.ch, &m1);
  bool empty_left_null = m1.data == NULL;

  free(m2.data);
  free(m1.data);
  pthread_join(tid, NULL);
  cchan_destroy(t.ch);

  REQUIRE_EQ(first_send, ccol_success);
  REQUIRE_TRUE(first_taken);
  REQUIRE_EQ(full_send, ccol_container_full);
  REQUIRE_TRUE(full_kept);
  REQUIRE_TRUE(worker_done);
  REQUIRE_EQ(atomic_load(&t.failed_line), 0);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_EQ((int)got_char, (int)'B');
  REQUIRE_EQ(empty, ccol_container_empty);
  REQUIRE_TRUE(empty_left_null);
}

// The timeouts of the timed channel test. A call that should time out gets
// CHAN_SHORT_US and must take at least that long. A call that should succeed at
// once gets CHAN_LONG_US and must return within CHAN_AT_ONCE_US, so waiting out
// the timeout fails but a slow runner doesn't.
#define CHAN_SHORT_US 10000
#define CHAN_LONG_US 1000000
#define CHAN_AT_ONCE_US 500000

// The worker side of the timed channel test: Once t->phase allows it, it checks
// each timed call against the states the main thread set up, timing it with the
// CHAN_*_US bounds.
void *thr_for_channels_timed_send_and_timed_receive(void *args) {
  chan_turns_t *t = (chan_turns_t *)args;
  if (!chan_wait_phase(t, 1)) {
    CHAN_CHECK(t, false);
    return NULL;
  }

  struct timespec before;
  struct timespec after;

  cmessage msg = {.data = NULL, .size = 0};
  getMonoTime(before);
  CHAN_CHECK(t, cchan_timed_recv(t->ch, &msg, CHAN_LONG_US) == ccol_success);
  getMonoTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) < CHAN_AT_ONCE_US);
  CHAN_CHECK(t, msg.data != NULL && *((char *)msg.data) == 'A');
  if (msg.data) *((char *)msg.data) = 'B';

  cmessage m2 = {.data = NULL, .size = 0};
  getMonoTime(before);
  CHAN_CHECK(t, cchan_timed_recv(t->ch, &m2, CHAN_SHORT_US) == ccol_timed_out);
  getMonoTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) >= CHAN_SHORT_US);
  CHAN_CHECK(t, m2.data == NULL);

  if (msg.data) {
    getMonoTime(before);
    CHAN_CHECK(t, cchan_timed_send(t->ch, &msg, CHAN_LONG_US) == ccol_success);
    getMonoTime(after);
    CHAN_CHECK(t, diffTimeUSec(before, after) < CHAN_AT_ONCE_US);
    CHAN_CHECK(t, msg.data == NULL);
    free(msg.data);
  }

  m2 = (cmessage){.data = malloc(sizeof(char)), .size = 1};
  getMonoTime(before);
  CHAN_CHECK(t, cchan_timed_send(t->ch, &m2, CHAN_SHORT_US) == ccol_timed_out);
  getMonoTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) >= CHAN_SHORT_US);
  CHAN_CHECK(t, m2.data != NULL);
  free(m2.data);

  atomic_store(&t->phase, 2);
  return NULL;
}

TEST(channels, timed_send_and_timed_receive) {
  chan_turns_t t = {.ch = cchan_create_mp(1, NULL, NULL)};
  REQUIRE_NE((void *)t.ch, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL,
                            thr_for_channels_timed_send_and_timed_receive, &t),
             0);

  struct timespec before;
  struct timespec after;

  cmessage m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  getMonoTime(before);
  ccol_retval first_send = cchan_timed_send(t.ch, &m1, CHAN_LONG_US);
  getMonoTime(after);
  long first_us = diffTimeUSec(before, after);
  bool first_taken = m1.data == NULL;
  free(m1.data);

  m1 = (cmessage){.data = malloc(sizeof(char)), .size = 1};
  getMonoTime(before);
  ccol_retval full_send = cchan_timed_send(t.ch, &m1, CHAN_SHORT_US);
  getMonoTime(after);
  long full_us = diffTimeUSec(before, after);
  bool full_kept = m1.data != NULL;
  free(m1.data);
  m1.data = NULL;

  atomic_store(&t.phase, 1);
  bool worker_done = chan_wait_phase(&t, 2);

  cmessage m2 = {.data = NULL, .size = 0};
  getMonoTime(before);
  ccol_retval got = cchan_timed_recv(t.ch, &m2, CHAN_LONG_US);
  getMonoTime(after);
  long got_us = diffTimeUSec(before, after);
  char got_char = m2.data ? *(char *)m2.data : 0;

  getMonoTime(before);
  ccol_retval empty = cchan_timed_recv(t.ch, &m1, CHAN_SHORT_US);
  getMonoTime(after);
  long empty_us = diffTimeUSec(before, after);
  bool empty_left_null = m1.data == NULL;

  free(m2.data);
  free(m1.data);
  pthread_join(tid, NULL);
  cchan_destroy(t.ch);

  REQUIRE_EQ(first_send, ccol_success);
  REQUIRE_LT(first_us, (long)CHAN_AT_ONCE_US);
  REQUIRE_TRUE(first_taken);
  REQUIRE_EQ(full_send, ccol_timed_out);
  REQUIRE_GE(full_us, (long)CHAN_SHORT_US);
  REQUIRE_TRUE(full_kept);
  REQUIRE_TRUE(worker_done);
  REQUIRE_EQ(atomic_load(&t.failed_line), 0);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_LT(got_us, (long)CHAN_AT_ONCE_US);
  REQUIRE_EQ((int)got_char, (int)'B');
  REQUIRE_EQ(empty, ccol_timed_out);
  REQUIRE_GE(empty_us, (long)CHAN_SHORT_US);
  REQUIRE_TRUE(empty_left_null);
}

// The worker side of the channel enable/disable test: Receives "A", then checks
// that a send is refused while sends toward the owner are off and goes through
// once they are back on.
void *thr_for_enable_disable_sending(void *args) {
  cchan *ch = (cchan *)args;

  cmessage msg = {.data = NULL, .size = 0};
  assert(cchan_recv(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');
  *((char *)msg.data) = 'B';

  cchan_disable_sending(ch, cchan_workers_to_owner);
  assert(cchan_send(ch, &msg) == ccol_not_permitted);
  assert(msg.data != NULL);

  cchan_enable_sending(ch, cchan_workers_to_owner);
  assert(cchan_send(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, enable_disable_sending) {
  cchan *ch = cchan_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_enable_disable_sending, ch), 0);

  cmessage m1 = {.data = malloc(sizeof(char)), .size = 1};
  m1.size = 1;
  ((char *)(m1.data))[0] = 'A';

  cchan_disable_sending(ch, cchan_owner_to_workers);

  REQUIRE_EQ(cchan_send(ch, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  cchan_enable_sending(ch, cchan_owner_to_workers);

  REQUIRE_EQ(cchan_send(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  cmessage m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(cchan_recv(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  cchan_destroy(ch);
}

// A channel destroys both of its circular queues, so it asserts the same way
// from whichever direction still has a live registration.
TEST(channels, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cchan *ch = cchan_create(4, NULL);
    if (!ch) test_child_exit(2);
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);
    ceventloop_handlers handlers = {0};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cchan_selectable(ch, cselect_read), handlers, NULL, &err);
    if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);
    cchan_destroy(ch);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Cselect tests
//
// The queue, delay and value of thr_send_to_circq().
typedef struct {
  ccircq *cq;
  int delay_us;
  int value;
} sel_circq_args;

// The same for a dynamic queue.
typedef struct {
  cdynmq *dq;
  int delay_us;
  int value;
} sel_dynq_args;

// The two queues of thr_disable_then_reenable_and_send().
typedef struct {
  ccircq *cq0;
  ccircq *cq1;
  int delay_us;
  int value;
} sel_disable_reenable_args;

// The same for a channel.
typedef struct {
  cchan *ch;
  int delay_us;
  int value;
} sel_chan_args;

// Sends one malloc'd int to a->cq after a->delay_us, so a cselect test has a
// message that arrives while it waits.
static void *thr_send_to_circq(void *arg) {
  sel_circq_args *a = (sel_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  cmessage msg = {.data = data, .size = sizeof(int)};
  assert(ccircq_send(a->cq, &msg) == ccol_success);
  return NULL;
}

// Sends one malloc'd int to a->dq after a->delay_us, so a cselect test has a
// message that arrives while it waits.
static void *thr_send_to_dynq(void *arg) {
  sel_dynq_args *a = (sel_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  cmessage msg = {.data = data, .size = sizeof(int)};
  assert(cdynmq_send(a->dq, &msg) == ccol_success);
  return NULL;
}

// Turns sending off on both queues after the delay, waits again, then turns it
// back on for cq0 and sends one message there. The test checks that cselect
// stays blocked through the off phase and wakes only on that message.
static void *thr_disable_then_reenable_and_send(void *arg) {
  sel_disable_reenable_args *a = (sel_disable_reenable_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccircq_disable_sending(a->cq0);
  ccircq_disable_sending(a->cq1);
  usleep((useconds_t)a->delay_us);
  ccircq_enable_sending(a->cq0);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  cmessage msg = {.data = data, .size = sizeof(int)};
  assert(ccircq_send(a->cq0, &msg) == ccol_success);
  return NULL;
}

// Sends one malloc'd int through the channel after a->delay_us, from a worker
// thread, so the owner's cselect has a message that arrives while it waits.
static void *thr_send_via_channel(void *arg) {
  sel_chan_args *a = (sel_chan_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  cmessage msg = {.data = data, .size = sizeof(int)};
  assert(cchan_send(a->ch, &msg) == ccol_success);
  return NULL;
}

TEST(cselect, returns_invalid_args_on_bad_inputs) {
  ccircq *cq = ccircq_create(4, NULL);

  size_t idx = 0;
  cselectable sel = ccircq_selectable(cq, cselect_read);

  REQUIRE_EQ(cselect(NULL, 1, &sel), ccol_invalid_args);
  REQUIRE_EQ(cselect(&idx, 0, &sel), ccol_invalid_args);
  REQUIRE_EQ(cselect(&idx, 1, NULL), ccol_invalid_args);

  cselectable null_sel = ccircq_selectable(NULL, cselect_read);
  REQUIRE_EQ(cselect(&idx, 1, &null_sel), ccol_invalid_args);

  ccircq_destroy(cq);
}

// cselect_timed() allocates one waiter node per selectable with a plain
// malloc(n * size), so n == SIZE_MAX must be refused before that product exists
// and before selectables[i] is read for i > 0. The array has one element on
// purpose, so a late check reads out of bounds.
TEST(cselect, timed_n_too_large_rejected_before_any_allocation_or_oob_read) {
  ccircq *cq = ccircq_create(4, NULL);
  cselectable sel = ccircq_selectable(cq, cselect_read);

  size_t idx = 99;
  REQUIRE_EQ(cselect_timed(&idx, SIZE_MAX, &sel, 0), ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  ccircq_destroy(cq);
}

// The scan keeps the index of a match in an int, so an n above INT_MAX must be
// refused before any selectable past the first is read.
TEST(cselect,
     timed_n_exceeding_int_max_rejected_before_any_allocation_or_oob_read) {
  ccircq *cq = ccircq_create(4, NULL);
  cselectable sel = ccircq_selectable(cq, cselect_read);

  size_t idx = 99;
  REQUIRE_EQ(cselect_timed(&idx, (size_t)INT_MAX + 1, &sel, 0),
             ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  ccircq_destroy(cq);
}

TEST(cselect, receives_from_first_queue_when_message_already_present) {
  ccircq *q0 = ccircq_create(4, NULL);
  ccircq *q1 = ccircq_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 7;
  cmessage send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(q0, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q0, cselect_read),
                        ccircq_selectable(q1, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 7);

  free(recv_msg.data);
  ccircq_destroy(q0);
  ccircq_destroy(q1);
}

TEST(cselect, receives_from_second_queue_when_message_already_present) {
  ccircq *q0 = ccircq_create(4, NULL);
  ccircq *q1 = ccircq_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 42;
  cmessage send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(q1, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q0, cselect_read),
                        ccircq_selectable(q1, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);

  free(recv_msg.data);
  ccircq_destroy(q0);
  ccircq_destroy(q1);
}

TEST(cselect, blocks_until_message_arrives_on_circq) {
  ccircq *q0 = ccircq_create(4, NULL);
  ccircq *q1 = ccircq_create(4, NULL);

  sel_circq_args args = {.cq = q1, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q0, cselect_read),
                        ccircq_selectable(q1, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  ccircq_destroy(q0);
  ccircq_destroy(q1);
}

// Sends off doesn't affect a read wait: The call waits out its timeout instead
// of returning ccol_not_permitted.
TEST(cselect, timed_returns_timed_out_when_all_queues_disabled) {
  ccircq *q0 = ccircq_create(4, NULL);
  ccircq *q1 = ccircq_create(4, NULL);

  ccircq_disable_sending(q0);
  ccircq_disable_sending(q1);

  size_t idx = 99;
  REQUIRE_EQ(cselect_timed_va(&idx, 50000, ccircq_selectable(q0, cselect_read),
                              ccircq_selectable(q1, cselect_read)),
             ccol_timed_out);

  ccircq_destroy(q0);
  ccircq_destroy(q1);
}

TEST(cselect,
     stays_blocked_through_disable_then_wakes_after_reenable_and_send) {
  ccircq *q0 = ccircq_create(4, NULL);
  ccircq *q1 = ccircq_create(4, NULL);

  sel_disable_reenable_args args = {
      .cq0 = q0, .cq1 = q1, .delay_us = 15000, .value = 42};
  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_disable_then_reenable_and_send, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q0, cselect_read),
                        ccircq_selectable(q1, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  ccircq_destroy(q0);
  ccircq_destroy(q1);
}

TEST(cselect, blocks_until_message_arrives_on_dynq) {
  ccircq *q0 = ccircq_create(4, NULL);
  cdynmq *dq = cdynmq_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 55};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q0, cselect_read),
                        cdynmq_selectable(dq, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_try_recv(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 55);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  ccircq_destroy(q0);
  cdynmq_destroy(dq);
}

TEST(cselect, cchan_direction_resolved_correctly_for_owner_thread) {
  cchan *ch = cchan_create_mp(4, NULL, NULL);

  sel_chan_args args = {.ch = ch, .delay_us = 15000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_via_channel, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cchan_selectable(ch, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(cchan_try_recv(ch, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  cchan_destroy(ch);
}

// The queue and delay of thr_recv_from_circq().
typedef struct {
  ccircq *cq;
  int delay_us;
} sel_recv_circq_args;

// Receives and frees one message from a->cq after a->delay_us, which frees the
// slot a write select is waiting for.
static void *thr_recv_from_circq(void *arg) {
  sel_recv_circq_args *a = (sel_recv_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  cmessage msg = {.data = NULL, .size = 0};
  assert(ccircq_recv(a->cq, &msg) == ccol_success);
  free(msg.data);
  return NULL;
}

// The queue and delay of thr_disable_circq_sending().
typedef struct {
  ccircq *cq;
  int delay_us;
} sel_disable_circq_args;

// Turns sending off on a->cq after a->delay_us, which makes a full queue ready
// for a write select that is waiting.
static void *thr_disable_circq_sending(void *arg) {
  sel_disable_circq_args *a = (sel_disable_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccircq_disable_sending(a->cq);
  return NULL;
}

// The message data of sel_fill_one(). It is static, so a test drains the queue
// without a free.
static char sel_full_token;

// Fills a circular queue of capacity 1 with one message the queue doesn't own,
// so the write-select tests have a full queue. Returns what the try_send
// returned.
static ccol_retval sel_fill_one(ccircq *cq) {
  cmessage m = {.data = &sel_full_token, .size = 1};
  return ccircq_try_send(cq, &m);
}

TEST(cselect, write_circq_writable_immediately) {
  ccircq *cq = ccircq_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(cq, cselect_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  ccircq_destroy(cq);
}

TEST(cselect, write_circq_blocks_until_reader_frees_space) {
  ccircq *cq = ccircq_create(2, NULL);

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 10;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 20;
  cmessage m0 = {.data = d0, .size = sizeof(int)};
  cmessage m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &m0), ccol_success);
  REQUIRE_EQ(ccircq_send(cq, &m1), ccol_success);

  sel_recv_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_recv_from_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(cq, cselect_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);

  cmessage drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_recv(cq, &drain), ccol_success);
  free(drain.data);

  ccircq_destroy(cq);
}

// A queue whose sends are off is ready for writing, as POLLOUT on a pipe whose
// reader is gone. A write select on a full queue returns once another thread
// turns sends off, and the send after it reports ccol_not_permitted.
TEST(cselect, write_full_circq_wakes_when_sending_is_disabled) {
  ccircq *cq = ccircq_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval filled = sel_fill_one(cq);

  sel_disable_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  int created = pthread_create(&tid, NULL, thr_disable_circq_sending, &args);

  size_t idx = 99;
  ccol_retval sel_rv = ccol_timed_out;
  ccol_retval send_rv = ccol_success;
  if (created == 0) {
    sel_rv =
        cselect_timed_va(&idx, 10000000, ccircq_selectable(cq, cselect_write));
    cmessage m = {.data = &sel_full_token, .size = 1};
    send_rv = ccircq_try_send(cq, &m);
    pthread_join(tid, NULL);
  }
  cmessage drain;
  while (ccircq_try_recv(cq, &drain) == ccol_success) {
  }
  ccircq_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

TEST(cselect, write_full_circq_is_not_ready_after_sending_is_enabled) {
  ccircq *cq = ccircq_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval filled = sel_fill_one(cq);
  ccircq_disable_sending(cq);
  ccircq_enable_sending(cq);

  size_t idx = 99;
  ccol_retval sel_rv =
      cselect_timed_va(&idx, 0, ccircq_selectable(cq, cselect_write));
  cmessage drain;
  while (ccircq_try_recv(cq, &drain) == ccol_success) {
  }
  ccircq_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(sel_rv, ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
}

TEST(cselect, write_dynq_writable_immediately) {
  cdynmq *dq = cdynmq_create(NULL);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cdynmq_selectable(dq, cselect_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  cdynmq_destroy(dq);
}

TEST(cselect, write_mixed_full_circq_and_readable_circq) {
  ccircq *q_write = ccircq_create(1, NULL);
  ccircq *q_read = ccircq_create(4, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 42;
  cmessage fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(q_write, &fill_msg), ccol_success);

  sel_circq_args args = {.cq = q_read, .delay_us = 15000, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, ccircq_selectable(q_write, cselect_write),
                        ccircq_selectable(q_read, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(q_read, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 88);

  free(recv_msg.data);
  pthread_join(tid, NULL);

  cmessage drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_recv(q_write, &drain), ccol_success);
  free(drain.data);

  ccircq_destroy(q_write);
  ccircq_destroy(q_read);
}

TEST(cselect, write_channel_owner_resolves_send_direction) {
  cchan *ch = cchan_create_mp(4, NULL, NULL);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cchan_selectable(ch, cselect_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  cchan_destroy(ch);
}

// The fd, delay and value of thr_write_to_fd().
typedef struct {
  int write_fd;
  int delay_us;
  int value;
} sel_fd_write_args;

// Writes a->value to a->write_fd after a->delay_us, so an fd cselect test has
// data that arrives while it waits.
static void *thr_write_to_fd(void *arg) {
  sel_fd_write_args *a = (sel_fd_write_args *)arg;
  usleep((useconds_t)a->delay_us);
  int v = a->value;
  assert(write(a->write_fd, &v, sizeof(v)) == sizeof(v));
  return NULL;
}

TEST(cselect, fd_readable_immediately) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 42;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cselectable_fd(pfd[0], cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 42);

  close(pfd[0]);
  close(pfd[1]);
}

// The pipe of sel_capped_writer_thread(), the poll-set count before the call,
// and whether the writer saw the call use up its capped waits.
typedef struct {
  int write_fd;
  uint64_t sets_before;
  bool saw_waits;
} sel_capped_writer_arg;

// Writes one byte to the pipe once the waiting call has built its poll set and
// used up the armed short capped waits, so only a call that waited again sees
// it. saw_waits tells the test whether that point was reached; it stays false
// when the call ended at its first capped wait.
static void *sel_capped_writer_thread(void *arg) {
  sel_capped_writer_arg *a = (sel_capped_writer_arg *)arg;
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    if (cselect__test_poll_sets_created() > a->sets_before &&
        cselect__test_short_capped_waits_left() == 0) {
      a->saw_waits = true;
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 10) break;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  char byte = 'x';
  ssize_t wrote = write(a->write_fd, &byte, 1);
  (void)wrote;
  return NULL;
}

// A 30-day timeout is longer than one kernel wait can take. The hook ends the
// first three capped waits with no event, and the call must wait again instead
// of reporting a timeout 30 days early.
TEST(cselect, fd_wait_capped_below_a_long_timeout_waits_again) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  sel_capped_writer_arg arg = {.write_fd = pfd[1],
                               .sets_before = cselect__test_poll_sets_created(),
                               .saw_waits = false};
  cselect__test_short_next_capped_waits(3);
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, sel_capped_writer_thread, &arg) == 0;

  cselectable sels[1] = {cselectable_fd(pfd[0], cselect_read)};
  size_t idx = 99;
  ccol_retval rv = ccol_unexpected_failure;
  if (created) {
    rv = cselect_timed(&idx, 1, sels, 30ULL * 86400ULL * 1000000ULL);
    pthread_join(tid, NULL);
  }
  cselect__test_short_next_capped_waits(0);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(arg.saw_waits);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
}

TEST(cselect, fd_blocks_until_data_arrives) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  sel_fd_write_args args = {.write_fd = pfd[1], .delay_us = 15000, .value = 77};
  struct timespec before, after;
  getMonoTime(before);
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_write_to_fd, &args), 0);

  size_t idx = 99;
  ccol_retval sel_rv = cselect_va(&idx, cselectable_fd(pfd[0], cselect_read));
  getMonoTime(after);
  pthread_join(tid, NULL);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, 0);
  REQUIRE_GE(diffTimeUSec(before, after), 10000);

  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 77);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(cselect, fd_writable_immediately) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cselectable_fd(pfd[1], cselect_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(cselect, fd_and_queue_fd_wins) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 55;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  ccircq *cq = ccircq_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cselectable_fd(pfd[0], cselect_read),
                        ccircq_selectable(cq, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 55);

  close(pfd[0]);
  close(pfd[1]);
  ccircq_destroy(cq);
}

// The eventfd bridge wakes the kernel wait for a queue event in a set that
// mixes fds and queues.
TEST(cselect, fd_and_queue_queue_wins) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccircq *cq = ccircq_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 15000, .value = 33};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cselectable_fd(pfd[0], cselect_read),
                        ccircq_selectable(cq, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 33);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  ccircq_destroy(cq);
}

TEST(cselect, fd_and_dynq_dynq_wins) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  cdynmq *dq = cdynmq_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(cselect_va(&idx, cselectable_fd(pfd[0], cselect_read),
                        cdynmq_selectable(dq, cselect_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(cdynmq_try_recv(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  cdynmq_destroy(dq);
}

TEST(cselect, fd_invalid_fd_returns_invalid_args) {
  size_t idx = 99;
  cselectable bad = cselectable_fd(-1, cselect_read);
  REQUIRE_EQ(cselect(&idx, 1, &bad), ccol_invalid_args);
}

TEST(cselect, timed_circq_returns_timed_out) {
  ccircq *cq = ccircq_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(cselect_timed_va(&idx, 50000, ccircq_selectable(cq, cselect_read)),
             ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
  ccircq_destroy(cq);
}

TEST(cselect, timed_fd_returns_timed_out) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  size_t idx = 99;
  REQUIRE_EQ(
      cselect_timed_va(&idx, 50000, cselectable_fd(pfd[0], cselect_read)),
      ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(cselect, timed_poll_zero_ms_circq_empty) {
  ccircq *cq = ccircq_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(cselect_timed_va(&idx, 0, ccircq_selectable(cq, cselect_read)),
             ccol_timed_out);
  ccircq_destroy(cq);
}

// The queue and message of helper_thread_main().
typedef struct helper_thread_args {
  ccircq *cq;
  cmessage msg;
} helper_thread_args;

// Sends a->msg to a->cq after 20 ms, the late producer of the
// timed_succeeds_before_deadline test.
void *helper_thread_main(void *arg) {
  helper_thread_args *a = arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
  nanosleep(&ts, NULL);
  ccircq_send(a->cq, &a->msg);
  return NULL;
}

TEST(cselect, timed_succeeds_before_deadline) {
  ccircq *cq = ccircq_create(4, NULL);

  pthread_t tid;
  cmessage send_msg = {.data = malloc(4), .size = 4};
  *(int *)send_msg.data = 1234;

  helper_thread_args args = {cq, send_msg};

  REQUIRE_EQ(pthread_create(&tid, NULL, helper_thread_main, &args), 0);

  size_t idx = 99;
  ccol_retval sel_rv =
      cselect_timed_va(&idx, 5000000, ccircq_selectable(cq, cselect_read));
  pthread_join(tid, NULL);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 1234);
  free(recv_msg.data);

  ccircq_destroy(cq);
}

// A select that timed out must unlink its waiter node before freeing it, or the
// next send uses freed memory (valgrind and ASan report it).
TEST(cselect, timed_out_deregisters_waiter_node) {
  ccircq *cq = ccircq_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(cselect_timed_va(&idx, 30000, ccircq_selectable(cq, cselect_read)),
             ccol_timed_out);

  cmessage msg = {.data = malloc(4), .size = 4};
  *(int *)msg.data = 42;
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);

  cmessage drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_recv(cq, &drain), ccol_success);
  free(drain.data);

  ccircq_destroy(cq);
}

// With no fd selectables the call waits on a condition variable. A forced wait
// error that isn't ETIMEDOUT gives ccol_unexpected_failure quickly instead of a
// retry loop or a wait for the whole deadline.
TEST(cselect, timed_wait_reports_unexpected_failure_on_condvar_error) {
  ccircq *cq = ccircq_create(4, NULL);

  cselect__test_force_next_condvar_wait_error();

  size_t idx = 99;
  struct timespec before, after;
  getMonoTime(before);
  REQUIRE_EQ(
      cselect_timed_va(&idx, 5000000, ccircq_selectable(cq, cselect_read)),
      ccol_unexpected_failure);
  getMonoTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_EQ(idx, (size_t)99);

  ccircq_destroy(cq);
}

// A wake can set *ready just before an unrelated wait error arrives, since the
// wait relocks its mutex on failure too. The call must check *ready again and
// resume instead of reporting ccol_unexpected_failure. The hook forces the
// error and *ready together, and a real background send lets the resumed call
// succeed.
TEST(cselect, timed_wait_ready_racing_condvar_error_still_succeeds) {
  ccircq *cq = ccircq_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 50000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  cselect__test_force_next_condvar_wait_error_racing_ready();

  size_t idx = 99;
  REQUIRE_EQ(
      cselect_timed_va(&idx, 2000000, ccircq_selectable(cq, cselect_read)),
      ccol_success);
  REQUIRE_EQ(idx, (size_t)0);

  cmessage recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccircq_try_recv(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  ccircq_destroy(cq);
}

// A write select on a full queue whose sends are already off is ready at once,
// also with a timeout of 0.
TEST(cselect, write_full_circq_is_ready_at_once_when_sending_disabled) {
  ccircq *cq = ccircq_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval filled = sel_fill_one(cq);
  ccircq_disable_sending(cq);

  size_t idx = 99;
  ccol_retval sel_rv =
      cselect_timed_va(&idx, 0, ccircq_selectable(cq, cselect_write));
  cmessage m = {.data = &sel_full_token, .size = 1};
  ccol_retval send_rv = ccircq_try_send(cq, &m);
  cmessage drain;
  while (ccircq_try_recv(cq, &drain) == ccol_success) {
  }
  ccircq_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

// The same for a dynamic queue, untimed and with a timeout of 0.
TEST(cselect, write_dynq_is_ready_at_once_when_sending_disabled) {
  cdynmq *dq = cdynmq_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  cdynmq_disable_sending(dq);

  size_t idx = 99;
  ccol_retval poll_rv =
      cselect_timed_va(&idx, 0, cdynmq_selectable(dq, cselect_write));
  size_t untimed_idx = 99;
  ccol_retval untimed_rv = ccol_timed_out;
  if (poll_rv == ccol_success)
    untimed_rv = cselect_va(&untimed_idx, cdynmq_selectable(dq, cselect_write));
  int *data = malloc(sizeof(int));
  cmessage m = {.data = data, .size = sizeof(int)};
  ccol_retval send_rv = cdynmq_send(dq, &m);
  if (send_rv != ccol_success) free(data);
  cmessage drain;
  while (cdynmq_try_recv(dq, &drain) == ccol_success) free(drain.data);
  cdynmq_destroy(dq);

  REQUIRE_NE((void *)data, NULL);
  REQUIRE_EQ(poll_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(untimed_rv, ccol_success);
  REQUIRE_EQ(untimed_idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

// The queue a write waiter watches and what its select returned.
typedef struct {
  ccircq *cq;
  ccol_retval result;
} sel_write_wait_result;

// Waits up to 5 s for a->cq to become writable and stores the result in
// a->result; two of these are the concurrent write waiters of the cascade
// test.
static void *thr_circq_write_wait(void *arg) {
  sel_write_wait_result *a = (sel_write_wait_result *)arg;
  size_t idx = 0;
  a->result =
      cselect_timed_va(&idx, 5000000, ccircq_selectable(a->cq, cselect_write));
  return NULL;
}

// One dequeue must cascade through the waiter list and wake both write waiters,
// not only the head.
TEST(cselect, write_circq_two_concurrent_waiters_both_wake_on_slot_free) {
  ccircq *cq = ccircq_create(1, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 0;
  cmessage fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &fill_msg), ccol_success);

  sel_write_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_write_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  bool started_t1 = (pthread_create(&t1, NULL, thr_circq_write_wait, &a1) == 0);
  bool started_t2 = (pthread_create(&t2, NULL, thr_circq_write_wait, &a2) == 0);

  usleep(30000);

  cmessage drain = {.data = NULL, .size = 0};
  ccol_retval drain_rv = ccircq_recv(cq, &drain);
  free(drain.data);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
  REQUIRE_EQ(drain_rv, ccol_success);
  REQUIRE_TRUE(started_t1);
  REQUIRE_TRUE(started_t2);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  ccircq_destroy(cq);
}

// The queue a read waiter watches and what it got.
typedef struct {
  ccircq *cq;
  ccol_retval result;
} sel_read_wait_result;

// Waits up to 5 s for a->cq to become readable and then takes a message; two of
// these are the concurrent read waiters of the cascade test. a->result is the
// select's result, or the receive's after a success.
static void *thr_circq_read_wait(void *arg) {
  sel_read_wait_result *a = (sel_read_wait_result *)arg;
  size_t idx = 0;
  a->result =
      cselect_timed_va(&idx, 5000000, ccircq_selectable(a->cq, cselect_read));
  if (a->result == ccol_success) {
    cmessage msg = {.data = NULL, .size = 0};
    ccol_retval rv = ccircq_try_recv(a->cq, &msg);
    a->result = rv;
    if (rv == ccol_success) free(msg.data);
  }
  return NULL;
}

// cselect() only peeks, so a waiter that finds itself ready must forward the
// wake to the next waiter. Without that, two readers of one empty queue get two
// messages and only the first wakes.
TEST(cselect,
     read_circq_two_concurrent_waiters_both_wake_on_message_available) {
  ccircq *cq = ccircq_create(4, NULL);

  sel_read_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_read_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  bool started_t1 = (pthread_create(&t1, NULL, thr_circq_read_wait, &a1) == 0);
  bool started_t2 = (pthread_create(&t2, NULL, thr_circq_read_wait, &a2) == 0);

  size_t want_waiters = (size_t)started_t1 + (size_t)started_t2;
  for (int i = 0; i < 10000 && ccircq__test_sel_read_waiter_count_for_tests(
                                   cq) < want_waiters;
       i++)
    usleep(1000);
  size_t seen_waiters = ccircq__test_sel_read_waiter_count_for_tests(cq);

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 1;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 2;
  cmessage m0 = {.data = d0, .size = sizeof(int)};
  cmessage m1 = {.data = d1, .size = sizeof(int)};
  ccol_retval send0_rv = ccircq_send(cq, &m0);
  ccol_retval send1_rv = ccircq_send(cq, &m1);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
  REQUIRE_TRUE(started_t1);
  REQUIRE_TRUE(started_t2);
  REQUIRE_EQ(seen_waiters, (size_t)2);
  REQUIRE_EQ(send0_rv, ccol_success);
  REQUIRE_EQ(send1_rv, ccol_success);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  ccircq_destroy(cq);
}

// Two selectables can share one fd, in any mix of directions. The fds are
// grouped (sorted, so O(n log n)) into one kernel registration each, or the
// second add fails with EEXIST. An event maps back to the member whose
// direction it satisfies.
TEST(cselect, fd_same_fd_two_directions_writable_wins) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  size_t idx = 999;
  cselectable sels[2] = {
      cselectable_fd(sv[0], cselect_read),
      cselectable_fd(sv[0], cselect_write),
  };
  ccol_retval rv = cselect_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)1);

  close(sv[0]);
  close(sv[1]);
}

TEST(cselect, fd_same_fd_two_directions_both_ready_resolves_to_a_member) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  char byte = 'x';
  REQUIRE_EQ(write(sv[1], &byte, 1), (ssize_t)1);

  size_t idx = 999;
  cselectable sels[2] = {
      cselectable_fd(sv[0], cselect_read),
      cselectable_fd(sv[0], cselect_write),
  };
  ccol_retval rv = cselect_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  char buf[8];
  REQUIRE_EQ(read(sv[0], buf, sizeof(buf)), (ssize_t)1);
  close(sv[0]);
  close(sv[1]);
}

TEST(cselect, fd_duplicate_selectable_same_direction_still_succeeds) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  cselectable sels[2] = {
      cselectable_fd(pfd[1], cselect_write),
      cselectable_fd(pfd[1], cselect_write),
  };
  ccol_retval rv = cselect_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(cselect, fd_duplicate_selectable_neither_ready_times_out) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  cselectable sels[2] = {
      cselectable_fd(pfd[0], cselect_read),
      cselectable_fd(pfd[0], cselect_read),
  };
  ccol_retval rv = cselect_timed(&idx, 2, sels, 100000);
  REQUIRE_EQ(rv, ccol_timed_out);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(cselect, fd_grouped_registration_coexists_with_ready_queue) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccircq *cq = ccircq_create(4, NULL);
  REQUIRE_NE((void *)cq, NULL);
  int *data = malloc(sizeof(int));
  REQUIRE_NE((void *)data, NULL);
  *data = 42;
  cmessage msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);

  size_t idx = 999;
  cselectable sels[3] = {
      cselectable_fd(pfd[0], cselect_read),
      cselectable_fd(pfd[0], cselect_read),
      ccircq_selectable(cq, cselect_read),
  };
  ccol_retval rv = cselect_timed(&idx, 3, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)2);

  cmessage out;
  REQUIRE_EQ(ccircq_try_recv(cq, &out), ccol_success);
  free(out.data);
  ccircq_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);
}

// All three queues stay ready, since cselect() never consumes anything, and the
// rotation of the scan gives each about a third of the calls. A scan that
// always starts at index 0 hands it every call.
TEST(cselect, three_always_ready_queues_share_the_calls) {
  ccircq *cq_r = ccircq_create(4, NULL);
  cdynmq *dq_r = cdynmq_create(NULL);
  ccircq *cq_w = ccircq_create(4, NULL);
  REQUIRE_NE((void *)cq_r, NULL);
  REQUIRE_NE((void *)dq_r, NULL);
  REQUIRE_NE((void *)cq_w, NULL);
  int *a = malloc(sizeof(int));
  int *b = malloc(sizeof(int));
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_NE((void *)b, NULL);
  *a = 1;
  *b = 2;
  cmessage ma = {.data = a, .size = sizeof(int)};
  cmessage mb = {.data = b, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq_r, &ma), ccol_success);
  REQUIRE_EQ(cdynmq_send(dq_r, &mb), ccol_success);

  cselectable sels[3] = {
      ccircq_selectable(cq_r, cselect_read),
      cdynmq_selectable(dq_r, cselect_read),
      ccircq_selectable(cq_w, cselect_write),
  };
  size_t wins[3] = {0, 0, 0};
  size_t failures = 0;
  for (int i = 0; i < 3000; i++) {
    size_t idx = 99;
    if (cselect(&idx, 3, sels) != ccol_success || idx >= 3)
      failures++;
    else
      wins[idx]++;
  }

  cmessage out;
  REQUIRE_EQ(ccircq_try_recv(cq_r, &out), ccol_success);
  free(out.data);
  REQUIRE_EQ(cdynmq_try_recv(dq_r, &out), ccol_success);
  free(out.data);
  ccircq_destroy(cq_r);
  cdynmq_destroy(dq_r);
  ccircq_destroy(cq_w);

  REQUIRE_EQ(failures, (size_t)0);
  for (int k = 0; k < 3; k++) {
    REQUIRE_GE(wins[k], (size_t)900);
    REQUIRE_LE(wins[k], (size_t)1100);
  }
}

// A worker waiting on {stop fd, busy queue} must see the stop although the
// queue never runs empty. The fd wins within a few calls and about half of them
// overall; a scan that looks at fds only when no queue is ready never picks it.
TEST(cselect, a_readable_fd_beside_an_always_ready_queue_wins) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char stop = 's';
  REQUIRE_EQ(write(pfd[1], &stop, 1), (ssize_t)1);

  cdynmq *dq = cdynmq_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  int *v = malloc(sizeof(int));
  REQUIRE_NE((void *)v, NULL);
  *v = 7;
  cmessage m = {.data = v, .size = sizeof(int)};
  REQUIRE_EQ(cdynmq_send(dq, &m), ccol_success);

  cselectable sels[2] = {
      cselectable_fd(pfd[0], cselect_read),
      cdynmq_selectable(dq, cselect_read),
  };
  int first_fd_win = -1;
  size_t failures = 0;
  for (int i = 0; i < 8 && first_fd_win < 0; i++) {
    size_t idx = 99;
    if (cselect_timed(&idx, 2, sels, 1000000) != ccol_success)
      failures++;
    else if (idx == 0)
      first_fd_win = i;
  }
  size_t fd_wins = 0;
  for (int i = 0; i < 2000; i++) {
    size_t idx = 99;
    if (cselect(&idx, 2, sels) != ccol_success || idx > 1)
      failures++;
    else if (idx == 0)
      fd_wins++;
  }

  cmessage out;
  REQUIRE_EQ(cdynmq_try_recv(dq, &out), ccol_success);
  free(out.data);
  cdynmq_destroy(dq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(failures, (size_t)0);
  REQUIRE_GE(first_fd_win, 0);
  REQUIRE_GE(fd_wins, (size_t)800);
  REQUIRE_LE(fd_wins, (size_t)1200);
}

// With no queue in the set the kernel wait reports both fds, and each should
// win about half of the calls instead of the first one in array order.
TEST(cselect, two_always_readable_fds_share_the_calls) {
  int p0[2];
  int p1[2];
  REQUIRE_EQ(pipe(p0), 0);
  REQUIRE_EQ(pipe(p1), 0);
  char byte = 'x';
  REQUIRE_EQ(write(p0[1], &byte, 1), (ssize_t)1);
  REQUIRE_EQ(write(p1[1], &byte, 1), (ssize_t)1);

  cselectable sels[2] = {
      cselectable_fd(p0[0], cselect_read),
      cselectable_fd(p1[0], cselect_read),
  };
  size_t wins[2] = {0, 0};
  size_t failures = 0;
  for (int i = 0; i < 1000; i++) {
    size_t idx = 99;
    if (cselect(&idx, 2, sels) != ccol_success || idx > 1)
      failures++;
    else
      wins[idx]++;
  }
  close(p0[0]);
  close(p0[1]);
  close(p1[0]);
  close(p1[1]);

  REQUIRE_EQ(failures, (size_t)0);
  REQUIRE_GE(wins[0], (size_t)400);
  REQUIRE_GE(wins[1], (size_t)400);
}

// The queue to wake and the count of poll sets before the call.
typedef struct {
  ccircq *cq;
  uint64_t sets_before;
} sel_set_waker_arg;

// The static message data that sel_set_waker_thread() sends.
static char sel_set_waker_token;

// Sends one message to a->cq once the waiting call has built its poll set (the
// count of created sets passes a->sets_before), so a call that has to wait ends
// on the message and never on its deadline. The wait is bounded at 10 s and
// only stops a hang.
static void *sel_set_waker_thread(void *arg) {
  sel_set_waker_arg *a = (sel_set_waker_arg *)arg;
  long long end = mono_now_us() + 10000000LL;
  while (cselect__test_poll_sets_created() <= a->sets_before &&
         mono_now_us() < end)
    mono_poll_pause();
  cmessage m = {.data = &sel_set_waker_token, .size = 1};
  (void)ccircq_try_send(a->cq, &m);
  return NULL;
}

// Only a call that has to wait builds an epoll set. A call answered at once by
// a ready queue or fd, or ended by a timeout of 0, builds none, which the count
// of created sets shows.
TEST(cselect, a_call_that_returns_at_once_creates_no_poll_set) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  cdynmq *dq = cdynmq_create(NULL);
  ccircq *cq = ccircq_create(1, NULL);
  int *v = malloc(sizeof(int));
  bool setup = dq != NULL && cq != NULL && v != NULL;
  cmessage m = {.data = v, .size = sizeof(int)};
  if (setup) setup = cdynmq_send(dq, &m) == ccol_success;
  if (!setup) free(v);

  uint64_t ready_queue_sets = UINT64_MAX, ready_fd_sets = UINT64_MAX;
  uint64_t zero_timeout_sets = UINT64_MAX, waiting_sets = UINT64_MAX;
  size_t failures = 0;
  ccol_retval rv_zero = ccol_success, rv_wait = ccol_timed_out;
  size_t wait_idx = 99;
  bool waker_started = false;
  if (setup) {
    cselectable mixed[2] = {
        cselectable_fd(pfd[0], cselect_read),
        cdynmq_selectable(dq, cselect_read),
    };
    uint64_t before = cselect__test_poll_sets_created();
    for (int i = 0; i < 100; i++) {
      size_t idx = 99;
      if (cselect(&idx, 2, mixed) != ccol_success || idx != 1) failures++;
    }
    ready_queue_sets = cselect__test_poll_sets_created() - before;

    char byte = 'x';
    if (write(pfd[1], &byte, 1) != 1) failures++;
    cselectable fd_only = cselectable_fd(pfd[0], cselect_read);
    before = cselect__test_poll_sets_created();
    for (int i = 0; i < 100; i++) {
      size_t idx = 99;
      if (cselect(&idx, 1, &fd_only) != ccol_success || idx != 0) failures++;
    }
    ready_fd_sets = cselect__test_poll_sets_created() - before;
    if (read(pfd[0], &byte, 1) != 1) failures++;

    cselectable idle[2] = {
        cselectable_fd(pfd[0], cselect_read),
        ccircq_selectable(cq, cselect_read),
    };
    size_t idx = 99;
    before = cselect__test_poll_sets_created();
    rv_zero = cselect_timed(&idx, 2, idle, 0);
    zero_timeout_sets = cselect__test_poll_sets_created() - before;
    before = cselect__test_poll_sets_created();
    sel_set_waker_arg wa = {.cq = cq, .sets_before = before};
    pthread_t waker;
    waker_started =
        pthread_create(&waker, NULL, sel_set_waker_thread, &wa) == 0;
    if (waker_started) {
      rv_wait = cselect_timed(&wait_idx, 2, idle, 10000000);
      waiting_sets = cselect__test_poll_sets_created() - before;
      pthread_join(waker, NULL);
    }

    cmessage out;
    if (cdynmq_try_recv(dq, &out) == ccol_success) free(out.data);
    (void)ccircq_try_recv(cq, &out);
  }
  if (dq) cdynmq_destroy(dq);
  if (cq) ccircq_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_TRUE(setup);
  REQUIRE_EQ(failures, (size_t)0);
  REQUIRE_EQ(ready_queue_sets, (uint64_t)0);
  REQUIRE_EQ(ready_fd_sets, (uint64_t)0);
  REQUIRE_EQ(rv_zero, ccol_timed_out);
  REQUIRE_EQ(zero_timeout_sets, (uint64_t)0);
  REQUIRE_TRUE(waker_started);
  REQUIRE_EQ(rv_wait, ccol_success);
  REQUIRE_EQ(wait_idx, (size_t)1);
  REQUIRE_EQ(waiting_sets, (uint64_t)1);
}

// The queue and value of sel_late_send_thread().
typedef struct {
  ccircq *cq;
  int value;
} sel_late_send_arg;

// Sends a->value to a->cq after 30 ms, so a queue beside an idle fd fills while
// the call waits.
static void *sel_late_send_thread(void *arg) {
  sel_late_send_arg *a = (sel_late_send_arg *)arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 30000000L};
  nanosleep(&ts, NULL);
  cmessage m = {.data = &a->value, .size = sizeof(int)};
  (void)ccircq_send(a->cq, &m);
  return NULL;
}

TEST(cselect, a_queue_beside_an_idle_fd_wakes_a_waiting_call) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccircq *cq = ccircq_create(1, NULL);
  ccol_retval rv = ccol_unexpected_failure;
  size_t idx = 99;
  bool started = false;
  sel_late_send_arg arg = {.cq = cq, .value = 5};
  pthread_t t;
  if (cq) {
    started = pthread_create(&t, NULL, sel_late_send_thread, &arg) == 0;
    if (started) {
      cselectable sels[2] = {
          cselectable_fd(pfd[0], cselect_read),
          ccircq_selectable(cq, cselect_read),
      };
      rv = cselect_timed(&idx, 2, sels, 5000000);
      pthread_join(t, NULL);
      cmessage out;
      (void)ccircq_try_recv(cq, &out);
    }
    ccircq_destroy(cq);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)1);
}

// Every allocation of cselect_timed() reports ccol_not_enough_memory when it
// fails and leaves no waiter linked. The event buffer is allocated only by a
// call that gets ready to wait, so the failing calls get a timeout no slow
// runner can use up.
TEST(cselect, a_failed_allocation_reports_not_enough_memory) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccircq *cq = ccircq_create(1, NULL);
  ccol_retval rv[4] = {ccol_success, ccol_success, ccol_success, ccol_success};
  ccol_retval rv_queues_only = ccol_success;
  size_t waiters_left = SIZE_MAX;
  bool created = cq != NULL;
  if (cq) {
    cselectable sels[2] = {
        cselectable_fd(pfd[0], cselect_read),
        ccircq_selectable(cq, cselect_read),
    };
    for (uint32_t k = 0; k < 4; k++) {
      size_t idx = 99;
      cselect__test_fail_alloc(k + 1);
      rv[k] = cselect_timed(&idx, 2, sels, k < 3 ? 10000000 : 10000);
      cselect__test_fail_alloc(0);
    }
    waiters_left = ccircq__test_sel_read_waiter_count_for_tests(cq);
    size_t idx = 99;
    cselect__test_fail_alloc(1);
    rv_queues_only = cselect_timed(&idx, 1, &sels[1], 10000);
    cselect__test_fail_alloc(0);
    ccircq_destroy(cq);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(rv[0], ccol_not_enough_memory);
  REQUIRE_EQ(rv[1], ccol_not_enough_memory);
  REQUIRE_EQ(rv[2], ccol_not_enough_memory);
  REQUIRE_EQ(rv[3], ccol_timed_out);
  REQUIRE_EQ(waiters_left, (size_t)0);
  REQUIRE_EQ(rv_queues_only, ccol_not_enough_memory);
}

// A stripe comes from the high bits of the fd's hash. Reducing modulo the
// stripe count puts even fds on 2 of 4 stripes and fds 4 apart on 1.
TEST(ceventloop, fds_that_share_low_bits_spread_over_every_stripe) {
  size_t even[4] = {0, 0, 0, 0};
  size_t step4[4] = {0, 0, 0, 0};
  bool in_range = true;
  for (int fd = 0; fd < 1000; fd += 2) {
    size_t s = ceventloop__test_stripe_index(4, fd);
    if (s >= 4)
      in_range = false;
    else
      even[s]++;
  }
  for (int fd = 0; fd < 1000; fd += 4) {
    size_t s = ceventloop__test_stripe_index(4, fd);
    if (s >= 4)
      in_range = false;
    else
      step4[s]++;
  }
  bool odd_range = true;
  for (int fd = 0; fd < 100; fd++) {
    if (ceventloop__test_stripe_index(7, fd) >= 7) odd_range = false;
    if (ceventloop__test_stripe_index(1, fd) != 0) odd_range = false;
  }
  REQUIRE_TRUE(in_range);
  REQUIRE_TRUE(odd_range);
  for (int k = 0; k < 4; k++) {
    REQUIRE_GE(even[k], (size_t)75);
    REQUIRE_GE(step4[k], (size_t)37);
  }
}

// Ceventloop tests
//
// The state the generic callbacks below update under mtx, with cond broadcast
// after every change. self_reg and self_loop serve the self-removal tests.
typedef struct evl_sync_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int readable_count;
  int writable_count;
  int error_count;
  cmessage last_msg;
  bool last_msg_valid;
  cselect_dir last_dir_seen;
  ceventloop_reg self_reg;
  ceventloop self_loop;
} evl_sync_ctx;

// Initializes c for one test, with every counter at zero and no message. The
// pthread calls are checked, because a failed init leaves every later lock and
// wait on c undefined, maybe an endless hang.
static void evl_sync_ctx_init(evl_sync_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->readable_count = 0;
  c->writable_count = 0;
  c->error_count = 0;
  c->last_msg = (cmessage){.data = NULL, .size = 0};
  c->last_msg_valid = false;
  c->self_reg = CEVENTLOOP_REG_NULL;
  c->self_loop = CEVENTLOOP_NULL;
}

// Frees the last message c holds and destroys its lock and condition, once the
// test has joined every thread that could still dispatch with c.
static void evl_sync_ctx_destroy(evl_sync_ctx *c) {
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

// The generic read callback of the loop tests: Counts the dispatch in
// c->readable_count and records its direction, under c->mtx. The loop never
// receives for the caller, so for a queue it does the try_recv here, as a real
// caller does, and keeps the message in c->last_msg. A failed try_recv (another
// consumer took the message) leaves last_msg_valid false. An fd is left for the
// test to read.
static void evl_on_readable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  cmessage msg = {.data = NULL, .size = 0};
  bool got_msg = false;
  if (sel->type == cselectable_type_ccircq) {
    got_msg = (ccircq_try_recv(sel->cq, &msg) == ccol_success);
  } else if (sel->type == cselectable_type_cdynmq) {
    got_msg = (cdynmq_try_recv(sel->dq, &msg) == ccol_success);
  }
  pthread_mutex_lock(&c->mtx);
  c->readable_count++;
  c->last_dir_seen = sel->dir;
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  if (got_msg) {
    c->last_msg = msg;
    c->last_msg_valid = true;
  } else {
    c->last_msg = (cmessage){.data = NULL, .size = 0};
    c->last_msg_valid = false;
  }
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// The generic write callback of the loop tests: Counts the dispatch in
// c->writable_count and records its direction, under c->mtx.
static void evl_on_writable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->writable_count++;
  c->last_dir_seen = sel->dir;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// The generic error callback of the loop tests: Counts the dispatch in
// c->error_count under c->mtx.
static void evl_on_error(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                         void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->error_count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// Removes its own registration (c->self_loop, c->self_reg) from inside the
// callback, then acts like evl_on_readable(); the remove-from-callback test
// uses it. It checks with assert(): It runs on a reactor thread, and tau's
// assertions aren't thread-safe. Every background helper of this file does the
// same.
static void evl_on_readable_self_remove(ceventloop loop, ceventloop_reg reg,
                                        cselectable *sel, void *arg) {
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  assert(reg == c->self_reg);
  assert(ceventloop_remove(c->self_loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, arg);
}

// Waits until *counter_field, a counter of c read under c->mtx, reaches target,
// for at most timeout_ms. The loop tests wait for their dispatches with it.
// Returns false on a timeout.
static bool evl_wait_for(evl_sync_ctx *c, int *counter_field, int target,
                         int timeout_ms) {
  long long deadline = mono_now_us() + (long long)timeout_ms * 1000LL;
  for (;;) {
    pthread_mutex_lock(&c->mtx);
    bool ok = (*counter_field >= target);
    pthread_mutex_unlock(&c->mtx);
    if (ok) return true;
    if (mono_now_us() >= deadline) return false;
    mono_poll_pause();
  }
}

// Sets O_NONBLOCK on fd. Callbacks that read the fd themselves need it: Under
// concurrent dispatch a callback may find nothing to read, and a blocking
// read() then hangs the reactor thread and the shutdown that joins it.
static void evl_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

TEST(ceventloop, create_destroy) {
  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);
  ceventloop_destroy(loop);
  REQUIRE_EQ(loop, CEVENTLOOP_NULL);
}

TEST(ceventloop, create_destroy_scoped) {
  {
    ceventloop_construct_scoped(loop, 8, 1, 1);
    REQUIRE_NE(loop, CEVENTLOOP_NULL);
  }
}

// evl_on_readable() never drains the fd, so with a level-triggered reactor it
// keeps being dispatched until the registration goes away. The body sits in a
// nested block whose end shuts the loop down and joins its reactor thread, and
// only after that may ctx be destroyed. The tests below use the same pattern.
TEST(ceventloop, fd_on_readable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool last_msg_valid;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);

    int val = 42;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
    pthread_mutex_lock(&ctx.mtx);
    last_msg_valid = ctx.last_msg_valid;
    pthread_mutex_unlock(&ctx.mtx);

    char rbuf[16];
    ssize_t n = read(pfd[0], rbuf, sizeof(rbuf));
    REQUIRE_EQ(n, (ssize_t)sizeof(val));
    REQUIRE_EQ(*(int *)rbuf, 42);

    ceventloop_remove(loop, reg);
  }

  REQUIRE_FALSE(last_msg_valid);

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, fd_on_writable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[1], cselect_write), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// One socket fd with both directions: An add, a combine with MOD, and the MOD
// back down when one direction is removed.
TEST(ceventloop, fd_both_directions_combine_and_recombine) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &write_ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)2);

    REQUIRE_TRUE(
        evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, EVL_WAIT_MS));

    REQUIRE_EQ(ceventloop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(
        evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

// Both callbacks must fire for the same kernel batch, which is why the event
// carries the shared entry and not one registration.
TEST(ceventloop, fd_simultaneous_readable_and_writable) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &write_ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

    REQUIRE_TRUE(
        evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, EVL_WAIT_MS));

    int val = 99;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));

    REQUIRE_TRUE(
        evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, EVL_WAIT_MS));
    REQUIRE_TRUE(
        evl_wait_for(&write_ctx, &write_ctx.writable_count, 2, EVL_WAIT_MS));

    ceventloop_remove(loop, rreg);
    ceventloop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ceventloop, fd_on_error_fires_for_both_directions) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers rh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &write_ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

    close(sv[1]);

    REQUIRE_TRUE(
        evl_wait_for(&read_ctx, &read_ctx.error_count, 1, EVL_WAIT_MS));
    REQUIRE_TRUE(
        evl_wait_for(&write_ctx, &write_ctx.error_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, rreg);
    ceventloop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
}

// A socket whose peer hung up reports writable and an error at once. A
// write-only registration with no on_error (a documented pattern) must still
// get on_writable instead of being starved for ever. This covers the path with
// one reactor thread.
TEST(ceventloop,
     fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

    close(sv[1]);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, EVL_WAIT_MS));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    ceventloop_remove(loop, wreg);
  }

  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

// The same with three reactor threads, which go through the poller's own copy
// of that handling.
TEST(
    ceventloop,
    fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up_multi_thread) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    ceventloop_construct_scoped(loop, 8, 1, 3);

    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

    close(sv[1]);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, EVL_WAIT_MS));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    ceventloop_remove(loop, wreg);
  }

  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

// Registering an fd again after a remove. The tests register an fd, remove it,
// register the same number again, wait until the loop reclaims the old entry,
// and only then make the fd readable. A loop that lets the old entry touch the
// fd's kernel interest (a DEL or MOD by fd number from the reclaim or a late
// re-arm) silences the new registration.
//
// The gate holds evl_readd_on_readable() while closed, and in_callback says the
// callback is inside. Only the in-flight case closes the gate.
static atomic_bool evl_readd_gate_closed;
static atomic_bool evl_readd_in_callback;

// The callback of the re-add tests: Waits (bounded) while
// evl_readd_gate_closed is set, after setting evl_readd_in_callback, then
// drains the fd without blocking and counts a readable dispatch in the
// evl_sync_ctx at arg.
static void evl_readd_on_readable(ceventloop loop, ceventloop_reg reg,
                                  cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  atomic_store(&evl_readd_in_callback, true);
  for (int i = 0; i < 10000 && atomic_load(&evl_readd_gate_closed); i++) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  char buf[64];
  while (read(sel->fd, buf, sizeof(buf)) > 0) {
  }
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->readable_count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// Waits up to timeout_ms until loop has reclaimed more than base event entries,
// so a re-add test makes the fd readable only after the old entry is gone.
// Returns false on a timeout.
static bool evl_readd_wait_reclaimed(ceventloop loop, uint64_t base,
                                     int timeout_ms) {
  for (int i = 0; i < timeout_ms; i++) {
    if (ceventloop__entries_reclaimed_for_tests(loop) > base) return true;
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  return ceventloop__entries_reclaimed_for_tests(loop) > base;
}

// Reads c->readable_count under its lock, for the re-add runs that check it
// outside evl_wait_for().
static int evl_readd_count(evl_sync_ctx *c) {
  pthread_mutex_lock(&c->mtx);
  int n = c->readable_count;
  pthread_mutex_unlock(&c->mtx);
  return n;
}

// How the first registration goes away before the fd is registered again.
typedef enum {
  EVL_READD_PLAIN,
  EVL_READD_PAUSED,
  EVL_READD_NUMBER_REUSE
} evl_readd_mode;

// Runs one re-add case (mode) on a fresh loop with num_threads reactor threads.
// Returns a bit set of failures (0 on success) and releases everything on every
// path. For a reused number the new socket gets the old number through dup2()
// when it isn't the lowest free one. A second event checks that the interest
// stays armed after the first dispatch.
static int evl_readd_run(size_t num_threads, evl_readd_mode mode) {
  int fail = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1 << 0;
  evl_set_nonblocking(sv[0]);

  evl_sync_ctx old_ctx, new_ctx;
  evl_sync_ctx_init(&old_ctx);
  evl_sync_ctx_init(&new_ctx);
  atomic_store(&evl_readd_gate_closed, false);

  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, num_threads, &err);
  if (loop == CEVENTLOOP_NULL) {
    fail |= 1 << 1;
  } else {
    ceventloop_handlers h = {.on_readable = evl_readd_on_readable};
    int fd = sv[0];
    ceventloop_reg r1 = ceventloop_add(loop, cselectable_fd(fd, cselect_read),
                                       h, &old_ctx, &err);
    if (r1 == CEVENTLOOP_REG_NULL) fail |= 1 << 2;
    if (mode == EVL_READD_PAUSED && ceventloop_pause(loop, r1) != ccol_success)
      fail |= 1 << 3;
    uint64_t base = ceventloop__entries_reclaimed_for_tests(loop);
    if (ceventloop_remove(loop, r1) != ccol_success) fail |= 1 << 4;

    if (mode == EVL_READD_NUMBER_REUSE) {
      close(sv[0]);
      close(sv[1]);
      int nv[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, nv) != 0) {
        fail |= 1 << 5;
        sv[0] = sv[1] = -1;
      } else if (nv[0] != fd && dup2(nv[0], fd) != fd) {
        fail |= 1 << 5;
        close(nv[0]);
        sv[0] = -1;
        sv[1] = nv[1];
      } else {
        if (nv[0] != fd) close(nv[0]);
        sv[0] = fd;
        sv[1] = nv[1];
        evl_set_nonblocking(sv[0]);
      }
    }

    ceventloop_reg r2 = CEVENTLOOP_REG_NULL;
    if (sv[0] >= 0) {
      r2 = ceventloop_add(loop, cselectable_fd(sv[0], cselect_read), h,
                          &new_ctx, &err);
    }
    if (r2 == CEVENTLOOP_REG_NULL) fail |= 1 << 6;
    if (!evl_readd_wait_reclaimed(loop, base, 5000)) fail |= 1 << 7;

    if (r2 != CEVENTLOOP_REG_NULL && sv[1] >= 0) {
      test_write_retry_eintr(sv[1], "x", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, 1, 5000))
        fail |= 1 << 8;
      test_write_retry_eintr(sv[1], "y", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, 2, 5000))
        fail |= 1 << 9;
    }
    if (r2 != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r2);
    ceventloop_destroy(loop);
  }
  if (evl_readd_count(&old_ctx) != 0) fail |= 1 << 10;

  evl_sync_ctx_destroy(&old_ctx);
  evl_sync_ctx_destroy(&new_ctx);
  if (sv[0] >= 0) close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  return fail;
}

TEST(ceventloop, fd_readded_after_remove_fires_after_old_entry_reclaim) {
  int fails[6];
  int k = 0;
  const size_t threads[2] = {1, 3};
  for (int t = 0; t < 2; t++) {
    fails[k++] = evl_readd_run(threads[t], EVL_READD_PLAIN);
    fails[k++] = evl_readd_run(threads[t], EVL_READD_PAUSED);
    fails[k++] = evl_readd_run(threads[t], EVL_READD_NUMBER_REUSE);
  }
  for (int i = 0; i < k; i++) REQUIRE_EQ(fails[i], 0);
}

// Runs the in-flight re-add case: Removes a registration while its callback is
// held by the gate and registers the fd again meanwhile. Neither the re-arm
// after that callback nor the reclaim of the old entry may touch the new
// registration's interest. Returns a bit set of failures (0 on success).
static int evl_readd_inflight_run(size_t num_threads) {
  int fail = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1 << 0;
  evl_set_nonblocking(sv[0]);

  evl_sync_ctx old_ctx, new_ctx;
  evl_sync_ctx_init(&old_ctx);
  evl_sync_ctx_init(&new_ctx);
  atomic_store(&evl_readd_in_callback, false);
  atomic_store(&evl_readd_gate_closed, true);

  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, num_threads, &err);
  if (loop == CEVENTLOOP_NULL) {
    fail |= 1 << 1;
  } else {
    ceventloop_handlers h = {.on_readable = evl_readd_on_readable};
    ceventloop_reg r1 = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), h, &old_ctx, &err);
    if (r1 == CEVENTLOOP_REG_NULL) fail |= 1 << 2;
    test_write_retry_eintr(sv[1], "a", 1);
    bool entered = false;
    for (int i = 0; i < 5000 && !entered; i++) {
      entered = atomic_load(&evl_readd_in_callback);
      if (!entered) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
      }
    }
    if (!entered) fail |= 1 << 3;

    uint64_t base = ceventloop__entries_reclaimed_for_tests(loop);
    if (ceventloop_remove(loop, r1) != ccol_success) fail |= 1 << 4;
    ceventloop_reg r2 = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), h, &new_ctx, &err);
    if (r2 == CEVENTLOOP_REG_NULL) fail |= 1 << 5;

    atomic_store(&evl_readd_gate_closed, false);
    if (!evl_wait_for(&old_ctx, &old_ctx.readable_count, 1, 5000))
      fail |= 1 << 6;
    if (!evl_readd_wait_reclaimed(loop, base, 5000)) fail |= 1 << 7;

    if (r2 != CEVENTLOOP_REG_NULL) {
      int before = evl_readd_count(&new_ctx);
      test_write_retry_eintr(sv[1], "b", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, before + 1, 5000))
        fail |= 1 << 8;
      ceventloop_remove(loop, r2);
    }
    ceventloop_destroy(loop);
  }
  atomic_store(&evl_readd_gate_closed, false);

  evl_sync_ctx_destroy(&old_ctx);
  evl_sync_ctx_destroy(&new_ctx);
  close(sv[0]);
  close(sv[1]);
  return fail;
}

TEST(ceventloop, fd_readded_while_removed_dispatch_in_flight_fires) {
  int fail_single = evl_readd_inflight_run(1);
  int fail_pool = evl_readd_inflight_run(3);
  REQUIRE_EQ(fail_single, 0);
  REQUIRE_EQ(fail_pool, 0);
}

TEST(ceventloop, fd_duplicate_direction_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg1 = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg1, CEVENTLOOP_REG_NULL);

  err = NULL;
  ceventloop_reg reg2 = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_EQ(reg2, CEVENTLOOP_REG_NULL);
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);

  ceventloop_remove(loop, reg1);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, fd_modify_rejects_occupied_direction) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg rreg = ceventloop_add(
      loop, cselectable_fd(sv[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
  ceventloop_reg wreg = ceventloop_add(
      loop, cselectable_fd(sv[0], cselect_write), handlers, NULL, &err);
  REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_modify(loop, rreg, cselect_write), ccol_not_permitted);

  ceventloop_remove(loop, rreg);
  ceventloop_remove(loop, wreg);
  close(sv[0]);
  close(sv[1]);
}

// last_dir_seen is read under ctx.mtx, because the undrained fd keeps the
// reactor writing it.
TEST(ceventloop, fd_modify_flips_direction_and_updates_sel) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  cselect_dir last_dir_seen;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                    .on_writable = evl_on_writable,
                                    .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, EVL_WAIT_MS));

    REQUIRE_EQ(ceventloop_modify(loop, reg, cselect_read), ccol_success);

    int val = 5;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

    pthread_mutex_lock(&ctx.mtx);
    last_dir_seen = ctx.last_dir_seen;
    pthread_mutex_unlock(&ctx.mtx);

    ceventloop_remove(loop, reg);
  }

  REQUIRE_EQ((int)last_dir_seen, (int)cselect_read);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ceventloop, fd_modify_after_remove_returns_invalid_args) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_modify(loop, reg, cselect_write), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

// A queue's direction is part of its registration, so modify refuses a queue
// with ccol_invalid_args, also for the direction it already has.
TEST(ceventloop, fd_modify_rejects_queue_selectable) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_modify(loop, reg, cselect_write), ccol_invalid_args);
  REQUIRE_EQ(ceventloop_modify(loop, reg, cselect_read), ccol_invalid_args);

  ceventloop_remove(loop, reg);
  ccircq_destroy(cq);
}

TEST(ceventloop, queue_circq_readable_delivers_message) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int *payload = malloc(sizeof(int));
  *payload = 123;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(ctx.last_msg.size, sizeof(int));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 123);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccircq_destroy(cq);
}

// evl_on_writable() never touches the queue, so the reactor must not either.
// The test's own send and receive make the queue writable again, so a second
// dispatch can still be in flight; the nested block joins it before ctx and cq
// go.
TEST(ceventloop, queue_circq_writable_fires_without_consuming) {
  ccircq *cq = ccircq_create(1, NULL);

  int *filler = malloc(sizeof(int));
  *filler = 1;
  cmessage fmsg = {.data = filler, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &fmsg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_write), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    cmessage recvd;
    REQUIRE_EQ(ccircq_recv(cq, &recvd), ccol_success);
    free(recvd.data);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, EVL_WAIT_MS));

    int *payload = malloc(sizeof(int));
    *payload = 55;
    cmessage msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(ccircq_try_send(cq, &msg), ccol_success);
    REQUIRE_EQ(ccircq_msg_count(cq), (size_t)1);

    cmessage out;
    REQUIRE_EQ(ccircq_recv(cq, &out), ccol_success);
    REQUIRE_EQ(*(int *)out.data, 55);
    free(out.data);

    ceventloop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  ccircq_destroy(cq);
}

TEST(ceventloop, queue_dynq_readable_delivers_message) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  cdynmq *dq = cdynmq_create(NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, cdynmq_selectable(dq, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int *payload = malloc(sizeof(int));
  *payload = 321;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(cdynmq_send(dq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 321);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  cdynmq_destroy(dq);
}

// The channel and value of evl_chan_sender_thread().
typedef struct evl_chan_sender_args {
  cchan *ch;
  int value;
} evl_chan_sender_args;

// Sends a->value through a->ch from a thread that doesn't own the channel, so
// it goes to the owner-side queue that the channel test watches.
static void *evl_chan_sender_thread(void *arg) {
  evl_chan_sender_args *a = (evl_chan_sender_args *)arg;
  int *payload = malloc(sizeof(int));
  *payload = a->value;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  cchan_send(a->ch, &msg);
  return NULL;
}

TEST(ceventloop, queue_channel_selectable) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  cchan *ch = cchan_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, cchan_selectable(ch, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  evl_chan_sender_args sargs = {.ch = ch, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, evl_chan_sender_thread, &sargs), 0);
  pthread_join(tid, NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 88);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  cchan_destroy(ch);
}

TEST(ceventloop, queue_persistent_across_multiple_cycles) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  for (int i = 0; i < 5; i++) {
    int *payload = malloc(sizeof(int));
    *payload = i;
    cmessage msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, i + 1, EVL_WAIT_MS));
    REQUIRE_EQ(*(int *)ctx.last_msg.data, i);
  }

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccircq_destroy(cq);
}

// The bridge eventfd has no earlier notify for a message sent before the add,
// so the add must trigger itself when the queue is already ready.
TEST(ceventloop, queue_already_pending_message_at_registration_time) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 999;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 999);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccircq_destroy(cq);
}

// Counts the calls of one registration.
typedef struct evl_two_readers_ctx {
  _Atomic size_t calls;
} evl_two_readers_ctx;

// The slow head of the starvation test: Takes one message, sleeps 0.5 ms and
// counts the call. It is slower than the feeder on purpose, so a backlog builds
// behind it and the cascade has something to forward.
static void evl_two_readers_slow_on_readable(ceventloop loop,
                                             ceventloop_reg reg,
                                             cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  cmessage msg = {.data = NULL, .size = 0};
  (void)ccircq_try_recv(sel->cq, &msg);
  struct timespec ts = {0, 500000};
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

// Takes one message and counts the call; the fast reader of the two-reader
// tests.
static void evl_two_readers_fast_on_readable(ceventloop loop,
                                             ceventloop_reg reg,
                                             cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  cmessage msg = {.data = NULL, .size = 0};
  (void)ccircq_try_recv(sel->cq, &msg);
  atomic_fetch_add(&c->calls, 1);
}

// The queue a feeder sends to and how many messages it sends.
typedef struct evl_two_readers_feeder_args {
  ccircq *cq;
  int iterations;
} evl_two_readers_feeder_args;

// Floods a->cq with blocking sends to build a backlog for the starvation test;
// the queue's capacity keeps it from running far ahead.
static void *evl_two_readers_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    cmessage msg = {.data = NULL, .size = 0};
    (void)ccircq_send(a->cq, &msg);
  }
  return NULL;
}

// A notify wakes only the head of a queue's waiter list, and a loop
// registration stays linked until removed. Without the cascade forward a second
// registration behind the head gets no call at all. r1 is added first so the
// slow r2 becomes the head.
TEST(ceventloop, queue_second_registration_on_same_queue_is_not_starved) {
  ccircq *cq = ccircq_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg r1 = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers1, &ctx1, &err);
    REQUIRE_NE(r1, CEVENTLOOP_REG_NULL);

    ceventloop_handlers handlers2 = {
        .on_readable = evl_two_readers_slow_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ceventloop_reg r2 = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers2, &ctx2, &err);
    REQUIRE_NE(r2, CEVENTLOOP_REG_NULL);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 300};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_two_readers_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    long long deadline = mono_now_us() + EVL_WAIT_MS * 1000LL;
    for (;;) {
      if (atomic_load(&ctx1.calls) > 0 && atomic_load(&ctx2.calls) > 0) break;
      if (mono_now_us() >= deadline) break;
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    REQUIRE_GT(atomic_load(&ctx1.calls), (size_t)0);
    REQUIRE_GT(atomic_load(&ctx2.calls), (size_t)0);

    ceventloop_remove(loop, r1);
    ceventloop_remove(loop, r2);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &leftover) == ccol_success) {
  }
  ccircq_destroy(cq);
}

// Sends one message every 0.2 ms, so the queue drains between sends and no
// backlog ever forms. With nothing to cascade, the fairness test then checks
// the round-robin rotor of the waiter list.
static void *evl_two_readers_no_backlog_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    cmessage msg = {.data = NULL, .size = 0};
    (void)ccircq_send(a->cq, &msg);
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

// A registration added last stays the head of the waiter list, and a callback
// that keeps up leaves nothing to cascade. Head-only notification gives one
// registration every call, so the rotor must give each a turn per rotation.
// Each side must get at least a quarter of an even split.
TEST(ceventloop, queue_multiple_registrations_share_traffic_fairly) {
  ccircq *cq = ccircq_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg r1 = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers1, &ctx1, &err);
    REQUIRE_NE(r1, CEVENTLOOP_REG_NULL);

    ceventloop_handlers handlers2 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ceventloop_reg r2 = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers2, &ctx2, &err);
    REQUIRE_NE(r2, CEVENTLOOP_REG_NULL);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_two_readers_no_backlog_feeder_thread,
                       &feeder_args),
        0);
    pthread_join(feeder, NULL);

    for (int i = 0;
         i < 10000 &&
         atomic_load(&ctx1.calls) + atomic_load(&ctx2.calls) < (size_t)350;
         i++) {
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }

    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    ceventloop_remove(loop, r1);
    ceventloop_remove(loop, r2);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &leftover) == ccol_success) {
  }
  ccircq_destroy(cq);
}

// The cdynmq version of the fairness test above, for the rotors of a dynamic
// queue.
typedef struct evl_dynq_two_readers_ctx {
  _Atomic size_t calls;
} evl_dynq_two_readers_ctx;

// Takes one message from the dynamic queue and counts the call when it got one;
// the reader of the dynamic queue fairness test.
static void evl_dynq_two_readers_on_readable(ceventloop loop,
                                             ceventloop_reg reg,
                                             cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_dynq_two_readers_ctx *c = (evl_dynq_two_readers_ctx *)arg;
  cmessage msg = {.data = NULL, .size = 0};
  if (cdynmq_try_recv(sel->dq, &msg) == ccol_success) {
    atomic_fetch_add(&c->calls, 1);
  }
}

// The queue a feeder sends to and how many messages it sends.
typedef struct evl_dynq_feeder_args {
  cdynmq *dq;
  int iterations;
} evl_dynq_feeder_args;

// Sends one message to a->dq every 0.2 ms, so no backlog forms in the dynamic
// queue fairness test.
static void *evl_dynq_no_backlog_feeder_thread(void *arg) {
  evl_dynq_feeder_args *a = (evl_dynq_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    cmessage msg = {.data = NULL, .size = 0};
    (void)cdynmq_send(a->dq, &msg);
    struct timespec ts = {0, 200000};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

TEST(ceventloop, dynq_multiple_registrations_share_traffic_fairly) {
  cdynmq *dq = cdynmq_create(NULL);

  evl_dynq_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers1 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg r1 = ceventloop_add(
        loop, cdynmq_selectable(dq, cselect_read), handlers1, &ctx1, &err);
    REQUIRE_NE(r1, CEVENTLOOP_REG_NULL);

    ceventloop_handlers handlers2 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ceventloop_reg r2 = ceventloop_add(
        loop, cdynmq_selectable(dq, cselect_read), handlers2, &ctx2, &err);
    REQUIRE_NE(r2, CEVENTLOOP_REG_NULL);

    evl_dynq_feeder_args feeder_args = {.dq = dq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_dynq_no_backlog_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    for (int i = 0;
         i < 10000 &&
         atomic_load(&ctx1.calls) + atomic_load(&ctx2.calls) < (size_t)350;
         i++) {
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }

    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    ceventloop_remove(loop, r1);
    ceventloop_remove(loop, r2);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (cdynmq_try_recv(dq, &leftover) == ccol_success) {
  }
  cdynmq_destroy(dq);
}

TEST(ceventloop, remove_from_within_callback) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ctx.self_loop = loop;
  ceventloop_handlers handlers = {.on_readable = evl_on_readable_self_remove,
                                  .on_writable = NULL,
                                  .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  ctx.self_reg = reg;

  int *payload = malloc(sizeof(int));
  *payload = 1;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);

  int *payload2 = malloc(sizeof(int));
  *payload2 = 2;
  cmessage msg2 = {.data = payload2, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg2), ccol_success);
  usleep(50000);
  REQUIRE_EQ(ctx.readable_count, 1);

  cmessage drained;
  REQUIRE_EQ(ccircq_recv(cq, &drained), ccol_success);
  free(drained.data);

  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccircq_destroy(cq);
}

// The arguments of evl_remover_thread().
typedef struct evl_remover_args {
  ceventloop loop;
  ceventloop_reg reg;
  int delay_us;
} evl_remover_args;

// Removes a->reg from a->loop after a->delay_us, the concurrent remover of the
// remove-while-dispatching stress test.
static void *evl_remover_thread(void *arg) {
  evl_remover_args *a = (evl_remover_args *)arg;
  usleep((useconds_t)a->delay_us);
  ceventloop_remove(a->loop, a->reg);
  return NULL;
}

// Many cycles of add, notify and a remove from another thread. A remove may
// return while a dispatch is still running, so each cycle's ctx goes on the
// heap and is freed, like cq, only after the whole loop is joined.
TEST(ceventloop, remove_from_different_thread_concurrent_with_dispatch) {
  evl_sync_ctx *ctx_log[50];
  int ctx_log_count = 0;
  ccircq *cq = ccircq_create(4, NULL);

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    for (int iter = 0; iter < 50; iter++) {
      evl_sync_ctx *ctx = malloc(sizeof(*ctx));
      evl_sync_ctx_init(ctx);
      ctx_log[ctx_log_count++] = ctx;
      ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                      .on_writable = NULL,
                                      .on_error = NULL};
      char *err = NULL;
      ceventloop_reg reg = ceventloop_add(
          loop, ccircq_selectable(cq, cselect_read), handlers, ctx, &err);
      REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

      evl_remover_args rargs = {.loop = loop, .reg = reg, .delay_us = 0};
      pthread_t rtid;
      REQUIRE_EQ(pthread_create(&rtid, NULL, evl_remover_thread, &rargs), 0);

      int *payload = malloc(sizeof(int));
      *payload = iter;
      cmessage msg = {.data = payload, .size = sizeof(int)};
      ccircq_send(cq, &msg);

      pthread_join(rtid, NULL);

      cmessage leftover;
      while (ccircq_try_recv(cq, &leftover) == ccol_success) {
        free(leftover.data);
      }
    }

    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);
  }

  ccircq_destroy(cq);
  for (int i = 0; i < ctx_log_count; i++) {
    evl_sync_ctx_destroy(ctx_log[i]);
    free(ctx_log[i]);
  }
}

// A callback whose dispatch the test holds open through release_callback, to
// mimic a callback in flight. on_removed_saw_finished is the value of
// callback_finished that on_removed sees, which proves the order of the two.
typedef struct evl_slow_cb_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool callback_started;
  bool release_callback;
  bool callback_finished;
  bool on_removed_called;
  bool on_removed_saw_finished;
  void *on_removed_arg_seen;
} evl_slow_cb_ctx;

// Initializes c for one on_removed test with every flag false. The pthread
// calls are checked as in evl_sync_ctx_init().
static void evl_slow_cb_ctx_init(evl_slow_cb_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->callback_started = false;
  c->release_callback = false;
  c->callback_finished = false;
  c->on_removed_called = false;
  c->on_removed_saw_finished = false;
  c->on_removed_arg_seen = NULL;
}

// Destroys the lock and condition of c once the test is done with it.
static void evl_slow_cb_ctx_destroy(evl_slow_cb_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

// Waits until *flag, read under mtx, becomes true, for at most timeout_ms; the
// on_removed tests wait on their bool flags with it. Returns false on a
// timeout. It polls, so cond isn't used.
static bool evl_wait_bool(pthread_mutex_t *mtx, pthread_cond_t *cond,
                          bool *flag, int timeout_ms) {
  (void)cond;
  long long deadline = mono_now_us() + (long long)timeout_ms * 1000LL;
  for (;;) {
    pthread_mutex_lock(mtx);
    bool ok = *flag;
    pthread_mutex_unlock(mtx);
    if (ok) return true;
    if (mono_now_us() >= deadline) return false;
    mono_poll_pause();
  }
}

// A callback the test holds in flight: Drains the fd, sets callback_started,
// blocks until the test sets release_callback and sets callback_finished. The
// 20 s bound only stops a hang; a shorter one could release a correct build
// early.
static void evl_on_readable_slow(ceventloop loop, ceventloop_reg reg,
                                 cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_slow_cb_ctx *c = (evl_slow_cb_ctx *)arg;
  char buf[64];
  if (sel->type == cselectable_type_fd) {
    ssize_t n = read(sel->fd, buf, sizeof(buf));
    (void)n;
  }

  pthread_mutex_lock(&c->mtx);
  c->callback_started = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
  (void)evl_wait_bool(&c->mtx, &c->cond, &c->release_callback, 20000);
  pthread_mutex_lock(&c->mtx);
  c->callback_finished = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// The on_removed of the slow-callback tests: Records that it ran, its argument
// and whether the callback had finished by then, so the test can check the
// order.
static void evl_on_removed_slow(void *arg) {
  evl_slow_cb_ctx *c = (evl_slow_cb_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->on_removed_called = true;
  c->on_removed_saw_finished = c->callback_finished;
  c->on_removed_arg_seen = arg;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// The reactor thread is held inside evl_on_readable_slow() while this thread
// removes the registration. The remove returns at once, and on_removed must not
// fire until the callback has finished.
TEST(ceventloop, on_removed_fires_only_after_in_flight_callback_finishes) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = evl_on_readable_slow,
                                  .on_writable = NULL,
                                  .on_error = NULL,
                                  .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  char b = 1;
  REQUIRE_EQ(write(pfd[1], &b, 1), (ssize_t)1);
  REQUIRE_TRUE(
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.callback_started, EVL_WAIT_MS));

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);

  usleep(100000);
  pthread_mutex_lock(&ctx.mtx);
  bool fired_too_early = ctx.on_removed_called;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_FALSE(fired_too_early);

  pthread_mutex_lock(&ctx.mtx);
  ctx.release_callback = true;
  pthread_cond_broadcast(&ctx.cond);
  pthread_mutex_unlock(&ctx.mtx);

  REQUIRE_TRUE(
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, EVL_WAIT_MS));
  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_TRUE(ctx.on_removed_saw_finished);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, on_removed_fires_for_an_ordinary_removal) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = NULL,
                                  .on_writable = NULL,
                                  .on_error = NULL,
                                  .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_TRUE(
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, EVL_WAIT_MS));
  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, on_removed_null_is_harmless) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  close(pfd[0]);
  close(pfd[1]);
}

// Nothing removes the registration; the loop's destroy tears it down while it
// is still registered.
TEST(ceventloop, on_removed_fires_on_destroy_for_a_still_registered_reg) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 4, 1);
    ceventloop_handlers handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  }

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_TRUE(ctx.on_removed_called);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// The arguments and result of one ceventloop_reg_generation() call that the
// test delays where it drops its resolve pin.
typedef struct evl_delayed_reg_generation_ctx {
  ceventloop loop;
  ceventloop_reg reg;
  uint64_t gen;
} evl_delayed_reg_generation_ctx;

// Reads the generation of c->reg into c->gen. The test arms a delay where this
// call drops its resolve pin, so the remove runs while the pin is held.
static void *evl_delayed_reg_generation_thread(void *arg) {
  evl_delayed_reg_generation_ctx *c = (evl_delayed_reg_generation_ctx *)arg;
  c->gen = ceventloop_reg_generation(c->loop, c->reg);
  return NULL;
}

// A resolve pin is still held when the remove marks the registration removed
// and defers its reclaim, and the unpin's wake is a deliberate no-op in that
// order. The loop is otherwise idle, so only the bounded retry of the poller
// can reclaim it and fire on_removed, which this test waits for.
TEST(ceventloop,
     on_removed_fires_via_bounded_retry_when_resolve_unpin_races_remove) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = NULL,
                                  .on_writable = NULL,
                                  .on_error = NULL,
                                  .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  ceventloop__test_delay_next_reg_resolve_unpin_ms(300);

  evl_delayed_reg_generation_ctx gen_ctx = {.loop = loop, .reg = reg, .gen = 0};
  pthread_t th;
  int create_rv =
      pthread_create(&th, NULL, evl_delayed_reg_generation_thread, &gen_ctx);
  if (create_rv != 0) ceventloop__test_delay_next_reg_resolve_unpin_ms(0);
  REQUIRE_EQ(create_rv, 0);

  for (int i = 0;
       i < 10000 && !ceventloop__test_reg_resolve_unpin_delay_taken_for_tests();
       i++)
    usleep(1000);

  ccol_retval remove_rv = ceventloop_remove(loop, reg);

  int join_rv = pthread_join(th, NULL);
  ceventloop__test_delay_next_reg_resolve_unpin_ms(0);

  bool on_removed_fired =
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, EVL_WAIT_MS);
  pthread_mutex_lock(&ctx.mtx);
  void *on_removed_arg_seen = ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(join_rv, 0);
  REQUIRE_NE(gen_ctx.gen, (uint64_t)0);
  REQUIRE_EQ(remove_rv, ccol_success);
  REQUIRE_TRUE(on_removed_fired);
  REQUIRE_EQ(on_removed_arg_seen, (void *)&ctx);
}

TEST(ceventloop, shutdown_with_pending_registrations) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_shutdown(loop), ccol_success);
  REQUIRE_EQ(ceventloop_shutdown(loop), ccol_success);

  ceventloop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

// Counts the messages that evl_shutdown_peer_on_read() received.
static _Atomic long evl_shutdown_peer_got;
// Takes one message from the circular queue, frees it and counts it in
// evl_shutdown_peer_got; the reader of both loops in the shut-down peer
// test.
static void evl_shutdown_peer_on_read(ceventloop l, ceventloop_reg r,
                                      cselectable *s, void *a) {
  (void)l;
  (void)r;
  (void)a;
  cmessage m;
  if (ccircq_try_recv(s->cq, &m) == ccol_success) {
    free(m.data);
    atomic_fetch_add(&evl_shutdown_peer_got, 1);
  }
}

// Two loops watch one queue and one is shut down, its registration left in
// place. Every later message reaches the running loop at once. A shut-down
// registration that stays in the waiter list takes wakes nobody acts on, and
// about every second message waits for the next send.
TEST(ceventloop, a_shut_down_loop_takes_no_wake_from_its_peers) {
  int stuck[2] = {0, 0};
  int setup_failed = 0;
  for (int pass = 0; pass < 2; pass++) {
    atomic_store(&evl_shutdown_peer_got, 0);
    ccircq *q = ccircq_create(16, NULL);
    ceventloop a = ceventloop_create(8, pass == 0 ? 1 : 3, 1, NULL);
    ceventloop b = ceventloop_create(8, 1, 1, NULL);
    ceventloop_handlers h = {.on_readable = evl_shutdown_peer_on_read};
    ceventloop_reg ra =
        a ? ceventloop_add(a, ccircq_selectable(q, cselect_read), h, NULL, NULL)
          : CEVENTLOOP_REG_NULL;
    ceventloop_reg rb =
        b ? ceventloop_add(b, ccircq_selectable(q, cselect_read), h, NULL, NULL)
          : CEVENTLOOP_REG_NULL;
    if (!q || !a || !b || ra == CEVENTLOOP_REG_NULL ||
        rb == CEVENTLOOP_REG_NULL || ceventloop_shutdown(a) != ccol_success) {
      setup_failed++;
    } else {
      for (long i = 0; i < 20; i++) {
        cmessage m = {.data = malloc(1), .size = 1};
        if (!m.data || ccircq_send(q, &m) != ccol_success) {
          free(m.data);
          setup_failed++;
          break;
        }
        int waited_ms = 0;
        while (atomic_load(&evl_shutdown_peer_got) < i + 1 &&
               waited_ms < EVL_WAIT_MS) {
          struct timespec ts = {0, 1000000L};
          nanosleep(&ts, NULL);
          waited_ms++;
        }
        if (waited_ms >= EVL_WAIT_MS) stuck[pass]++;
      }
    }
    if (ra != CEVENTLOOP_REG_NULL) ceventloop_remove(a, ra);
    if (rb != CEVENTLOOP_REG_NULL) ceventloop_remove(b, rb);
    if (a) ceventloop_destroy(a);
    if (b) ceventloop_destroy(b);
    if (q) {
      cmessage m;
      while (ccircq_try_recv(q, &m) == ccol_success) free(m.data);
      ccircq_destroy(q);
    }
  }
  REQUIRE_EQ(setup_failed, 0);
  REQUIRE_EQ(stuck[0], 0);
  REQUIRE_EQ(stuck[1], 0);
}

// A shut-down loop refuses every new registration, with a message that says
// why, since nothing of it dispatches any more.
TEST(ceventloop, add_to_a_shut_down_loop_is_refused) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccircq *q = ccircq_create(4, NULL);
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  ccol_retval shut = ceventloop_shutdown(loop);
  ceventloop_handlers h = {.on_readable = evl_on_readable};
  char *err_q = NULL, *err_fd = NULL;
  ceventloop_reg rq =
      ceventloop_add(loop, ccircq_selectable(q, cselect_read), h, NULL, &err_q);
  ceventloop_reg rf = ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read),
                                     h, NULL, &err_fd);
  bool named_q = err_q && strstr(err_q, "shut down") != NULL;
  bool named_fd = err_fd && strstr(err_fd, "shut down") != NULL;
  if (rq != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, rq);
  if (rf != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, rf);
  ceventloop_destroy(loop);
  ccircq_destroy(q);
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_EQ(shut, ccol_success);
  REQUIRE_EQ(rq, CEVENTLOOP_REG_NULL);
  REQUIRE_EQ(rf, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(named_q);
  REQUIRE_TRUE(named_fd);
}

// The destroy of the loop must unlink the registration from the queue's waiter
// list before it frees it, so the queue stays usable afterwards.
TEST(ceventloop, destroy_while_queue_registration_pending_queue_outlives_loop) {
  ccircq *cq = ccircq_create(4, NULL);

  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  ceventloop_destroy(loop);

  int *payload = malloc(sizeof(int));
  *payload = 42;
  cmessage msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccircq_send(cq, &msg), ccol_success);
  cmessage out;
  REQUIRE_EQ(ccircq_recv(cq, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.data, 42);
  free(out.data);

  ccircq_destroy(cq);
}

TEST(ceventloop, reg_count_tracks_add_remove) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);
  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);

  close(pfd[0]);
  close(pfd[1]);
}

// ceventloop_add() must take the handle slot before it wires the registration
// into a registry. Otherwise a failed slot allocation returns
// CEVENTLOOP_REG_NULL for a registration that can already dispatch with an arg
// the caller then frees. The reg count snapshot taken at the forced failure
// proves the order: It would read 1 if the wiring came first.
TEST(ceventloop, add_slot_acquire_failure_leaves_nothing_wired_or_dispatched) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int one = 1;
  REQUIRE_EQ(write(pfd[1], &one, sizeof(one)), (ssize_t)sizeof(one));

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 3);

    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    ceventloop__test_force_next_reg_slot_acquire_failure();

    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_EQ(reg, CEVENTLOOP_REG_NULL);
    REQUIRE_NE((void *)err, NULL);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)0);

    REQUIRE_EQ(ceventloop__test_last_forced_slot_acquire_failure_reg_count(),
               (size_t)0);

    REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));
    pthread_mutex_lock(&ctx.mtx);
    REQUIRE_EQ(ctx.readable_count, 0);
    pthread_mutex_unlock(&ctx.mtx);

    err = NULL;
    ceventloop_reg reg2 = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg2, CEVENTLOOP_REG_NULL);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, reg2);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// Raises the soft descriptor limit to at least want, so the churn test can open
// its pipes (macOS defaults to 256). Stores the old limit in *saved for the
// caller to put back. Returns false when the hard limit is lower.
static bool evl_raise_nofile(rlim_t want, struct rlimit *saved) {
  if (getrlimit(RLIMIT_NOFILE, saved) != 0) return false;
  if (saved->rlim_cur != RLIM_INFINITY && saved->rlim_cur < want) {
    struct rlimit raised = *saved;
    raised.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &raised) != 0) return false;
  }
  return true;
}

TEST(ceventloop, high_add_remove_churn_stress) {
  const int n = 200;
  struct rlimit saved_nofile;
  if (!evl_raise_nofile((rlim_t)(2 * n + test_count_open_fds() + 64),
                        &saved_nofile)) {
    fprintf(stderr, "SKIP: the hard descriptor limit can't hold %d pipes\n", n);
    return;
  }
  ceventloop_construct_scoped(loop, 32, 1, 1);
  int pfds[200][2];
  ceventloop_reg regs[200];

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  int opened = 0;
  bool added = true;
  for (; opened < n; opened++) {
    if (pipe(pfds[opened]) != 0) break;
    char *err = NULL;
    regs[opened] =
        ceventloop_add(loop, cselectable_fd(pfds[opened][0], cselect_read),
                       handlers, NULL, &err);
    if (regs[opened] == CEVENTLOOP_REG_NULL) {
      added = false;
      close(pfds[opened][0]);
      close(pfds[opened][1]);
      break;
    }
  }
  size_t count_full = ceventloop_reg_count(loop);
  int removed = 0;
  for (int i = 0; i < opened; i++) {
    if (ceventloop_remove(loop, regs[i]) == ccol_success) removed++;
    close(pfds[i][0]);
    close(pfds[i][1]);
  }
  size_t count_empty = ceventloop_reg_count(loop);
  bool restored = setrlimit(RLIMIT_NOFILE, &saved_nofile) == 0;

  REQUIRE_TRUE(restored);
  REQUIRE_EQ(opened, n);
  REQUIRE_TRUE(added);
  REQUIRE_EQ(count_full, (size_t)n);
  REQUIRE_EQ(removed, n);
  REQUIRE_EQ(count_empty, (size_t)0);
}

// Two loops with different stripe counts (1 and 8) share no state. The read of
// ctx_b's count takes its lock, since the kernel event is no synchronization in
// the memory model.
TEST(ceventloop, multiple_independent_instances) {
  int pfd_a[2], pfd_b[2];
  REQUIRE_EQ(pipe(pfd_a), 0);
  REQUIRE_EQ(pipe(pfd_b), 0);

  evl_sync_ctx ctx_a, ctx_b;
  evl_sync_ctx_init(&ctx_a);
  evl_sync_ctx_init(&ctx_b);

  {
    ceventloop_construct_scoped(loop_a, 8, 1, 1);
    ceventloop_construct_scoped(loop_b, 8, 8, 1);

    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg_a = ceventloop_add(
        loop_a, cselectable_fd(pfd_a[0], cselect_read), handlers, &ctx_a, &err);
    REQUIRE_NE(reg_a, CEVENTLOOP_REG_NULL);
    ceventloop_reg reg_b = ceventloop_add(
        loop_b, cselectable_fd(pfd_b[0], cselect_read), handlers, &ctx_b, &err);
    REQUIRE_NE(reg_b, CEVENTLOOP_REG_NULL);

    int val_a = 1, val_b = 2;
    REQUIRE_EQ((ssize_t)sizeof(val_a), write(pfd_a[1], &val_a, sizeof(val_a)));
    REQUIRE_TRUE(evl_wait_for(&ctx_a, &ctx_a.readable_count, 1, EVL_WAIT_MS));
    pthread_mutex_lock(&ctx_b.mtx);
    int b_count_before = ctx_b.readable_count;
    pthread_mutex_unlock(&ctx_b.mtx);
    REQUIRE_EQ(b_count_before, 0);

    REQUIRE_EQ((ssize_t)sizeof(val_b), write(pfd_b[1], &val_b, sizeof(val_b)));
    REQUIRE_TRUE(evl_wait_for(&ctx_b, &ctx_b.readable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop_a, reg_a);
    ceventloop_remove(loop_b, reg_b);
  }

  evl_sync_ctx_destroy(&ctx_a);
  evl_sync_ctx_destroy(&ctx_b);
  close(pfd_a[0]);
  close(pfd_a[1]);
  close(pfd_b[0]);
  close(pfd_b[1]);
}

// Lock-striping tests
TEST(ceventloop, num_lock_stripes_zero_returns_null) {
  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 0, 1, &err);
  REQUIRE_EQ(loop, CEVENTLOOP_NULL);
}

// The poller passes max_events_per_wait to the kernel as an int and sizes its
// buffer from it. A value above INT_MAX, or one whose buffer size overflows
// size_t (the guard that binds on ILP32), must be refused at create, or the
// poller thread exits silently and nothing ever fires.
TEST(ceventloop, create_rejects_max_events_per_wait_exceeding_int_max) {
  char *err = NULL;
  ceventloop loop = ceventloop_create((size_t)INT_MAX + 1, 1, 1, &err);
  REQUIRE_EQ(loop, CEVENTLOOP_NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(
    ceventloop,
    create_rejects_max_events_per_wait_that_would_overflow_events_buffer_size) {
  char *err = NULL;
  size_t smallest_overflowing = SIZE_MAX / sizeof(ccol_poll_event) + 1;
  ceventloop loop = ceventloop_create(smallest_overflowing, 1, 1, &err);
  REQUIRE_EQ(loop, CEVENTLOOP_NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(ceventloop, create_succeeds_with_a_large_but_valid_max_events_per_wait) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ceventloop loop = ceventloop_create(4096, 1, 1, NULL);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int val = 1;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// Both directions of one fd hash to the same stripe, so the combine and the MOD
// back down behave as with one stripe.
TEST(ceventloop, fd_both_directions_combine_and_recombine_multi_stripe) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    ceventloop_construct_scoped(loop, 8, 8, 1);

    ceventloop_handlers rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ceventloop_handlers wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, &write_ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)2);

    REQUIRE_TRUE(
        evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, EVL_WAIT_MS));

    REQUIRE_EQ(ceventloop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(
        evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

// modify finds the stripe without a lock and refuses a removed registration
// cleanly. The test calls it right after the remove and doesn't wait for a
// reclaim, which would make the call a real use-after-free outside the
// contract.
TEST(ceventloop, fd_modify_after_remove_returns_invalid_args_multi_stripe) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 8, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_modify(loop, reg, cselect_write), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

// Pause and resume
//
// The test pauses before anything makes the fd ready: A pause that races a
// delivery can't be ordered against the re-arm of a dispatch thread. A bounded
// wait stands in for "never".
TEST(ceventloop, pause_stops_delivery_then_resume_restores_it) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// A pipe's write end is writable at once, so a dispatch can still beat the
// pause. The count is checked against its value when pause() returned.
TEST(ceventloop, pause_write_direction) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[1], cselect_write), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  int baseline = ctx.writable_count;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, 300));

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(
      evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, pause_is_idempotent) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);

  ceventloop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, resume_never_paused_is_idempotent_and_harmless) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, pause_and_resume_reject_queue_selectable) {
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ccircq *cq = ccircq_create(4, NULL);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                                      handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_invalid_args);
  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_invalid_args);

  ceventloop_remove(loop, reg);
  ccircq_destroy(cq);
}

TEST(ceventloop, pause_and_resume_null_args_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(CEVENTLOOP_NULL, reg), ccol_invalid_args);
  REQUIRE_EQ(ceventloop_pause(loop, CEVENTLOOP_REG_NULL), ccol_invalid_args);
  REQUIRE_EQ(ceventloop_resume(CEVENTLOOP_NULL, reg), ccol_invalid_args);
  REQUIRE_EQ(ceventloop_resume(loop, CEVENTLOOP_REG_NULL), ccol_invalid_args);

  ceventloop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, resume_after_remove_returns_invalid_args) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

// The first remove releases the handle slot before it returns, so a later
// remove doesn't resolve and reports ccol_invalid_args. Only a remove that
// races the first one can see ccol_success.
TEST(ceventloop, sequential_double_remove_returns_invalid_args_not_success) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  ceventloop_handlers handlers = {0};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_remove(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, pause_resume_preserve_reg_count_and_generation) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 1);

  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  size_t count_before = ceventloop_reg_count(loop);
  uint64_t gen_before = ceventloop_reg_generation(loop, reg);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_reg_count(loop), count_before);
  REQUIRE_EQ(ceventloop_reg_generation(loop, reg), gen_before);

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);
  REQUIRE_EQ(ceventloop_reg_count(loop), count_before);
  REQUIRE_EQ(ceventloop_reg_generation(loop, reg), gen_before);

  ceventloop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

// The write direction of a socket hears nothing about the end of its receive
// direction; epoll reports no event for OUT|ERR|HUP when the peer only shuts
// down its sending side. On kqueue a write interest that arms the read filter
// fails both tests: The select asserts and the loop's poller spins.
TEST(fd_half_close, a_write_select_on_a_half_closed_socket_reports_writable) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  REQUIRE_EQ(shutdown(sv[1], SHUT_WR), 0);
  usleep(20000);
  cselectable sel = cselectable_fd(sv[0], cselect_write);
  size_t idx = 99;
  ccol_retval rv = cselect_timed(&idx, 1, &sel, 2000000);
  close(sv[0]);
  close(sv[1]);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
}

// Fills the socket until a send would block and counts the call in the _Atomic
// int at arg, so the half-close test can tell a writer that keeps being
// dispatched.
static void fd_half_close_fill(ceventloop loop, ceventloop_reg reg,
                               cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  static char chunk[16384];
  while (send(sel->fd, chunk, sizeof(chunk), MSG_DONTWAIT) > 0) {
  }
  atomic_fetch_add((_Atomic int *)arg, 1);
}

// macOS ignores MSG_DONTWAIT on send, so the socket gets O_NONBLOCK or the fill
// loops block for ever there.
TEST(fd_half_close, a_writer_on_a_full_half_closed_socket_does_not_spin) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  REQUIRE_EQ(fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK), 0);
  static char chunk[16384];
  while (send(sv[0], chunk, sizeof(chunk), MSG_DONTWAIT) > 0) {
  }
  REQUIRE_EQ(shutdown(sv[1], SHUT_WR), 0);
  _Atomic int fills = 0;
  ceventloop_construct_scoped(loop, 8, 1, 1);
  ceventloop_handlers handlers = {.on_writable = fd_half_close_fill};
  char *err = NULL;
  ceventloop_reg reg =
      ceventloop_add(loop, cselectable_fd(sv[0], cselect_write), handlers,
                     (void *)&fills, &err);
  usleep(50000);
  uint64_t before = ceventloop__poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = ceventloop__poller_iterations_for_tests(loop);
  if (reg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  close(sv[0]);
  close(sv[1]);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_LT(after - before, (uint64_t)20);
}

// The kernel reports ERR and HUP whatever the interest mask, so a paused fd in
// a hangup state would wake the poller on every wait. The loop therefore takes
// an fd out of the kernel set when every direction on it is paused, and adds it
// back (ADD, not MOD) on resume. The test counts completed kernel waits, which
// an unguarded pause drives into the thousands.
TEST(ceventloop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_single_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                  .on_writable = NULL,
                                  .on_error = evl_on_error};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);

  close(pfd[1]);

  usleep(50000);
  uint64_t before = ceventloop__poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = ceventloop__poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

// The same with a dispatch pool, where the spin is a cycle of wait, submit,
// skip and re-arm.
TEST(ceventloop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_multi_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 3);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                  .on_writable = NULL,
                                  .on_error = evl_on_error};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);

  close(pfd[1]);

  usleep(50000);
  uint64_t before = ceventloop__poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = ceventloop__poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

// A resume must add the fd back to the kernel set with ADD, since a MOD of an
// fd that isn't there fails with ENOENT. The pending hangup is then dispatched,
// as an error event for an empty pipe whose writer closed.
TEST(ceventloop, resume_after_hangup_while_paused_delivers_dispatch) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                  .on_writable = NULL,
                                  .on_error = evl_on_error};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  REQUIRE_EQ(ceventloop_pause(loop, reg), ccol_success);
  close(pfd[1]);
  usleep(100000);

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.error_count, 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

// reg is _Atomic because it is published after the add returns, when another
// thread can already dispatch it.
typedef struct {
  evl_sync_ctx *ctx;
  ceventloop loop;
  _Atomic(ceventloop_reg) reg;
} evl_pause_from_callback_args;

// Pauses its own registration from inside the callback, as chttpserver does,
// then acts like evl_on_readable() on a->ctx. a->reg is read atomically since
// it is published after the add returns.
static void evl_on_readable_pause_self(ceventloop loop, ceventloop_reg reg,
                                       cselectable *sel, void *arg) {
  evl_pause_from_callback_args *a = (evl_pause_from_callback_args *)arg;
  assert(reg == atomic_load(&a->reg));
  assert(ceventloop_pause(a->loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, a->ctx);
}

// The pattern chttpserver depends on: A callback pauses its own registration,
// and a thread that isn't one of the loop's resumes it later.
TEST(ceventloop, pause_from_within_callback_then_resume_from_another_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  evl_pause_from_callback_args cb_args = {.ctx = &ctx, .loop = loop};
  ceventloop_handlers handlers = {.on_readable = evl_on_readable_pause_self,
                                  .on_writable = NULL,
                                  .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &cb_args, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  atomic_store(&cb_args.reg, reg);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

  int drain;
  REQUIRE_EQ(read(pfd[0], &drain, sizeof(drain)), (ssize_t)sizeof(drain));
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 2, 300));

  REQUIRE_EQ(ceventloop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 2, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// A callback that runs before ceventloop_add() has returned still gets its own
// handle and can remove the registration with it. The add hook makes the first
// dispatch run while the add is still inside the call.
typedef struct {
  _Atomic ceventloop_reg returned_reg;
  _Atomic ceventloop_reg reg_seen;
  _Atomic ceventloop_reg stored_seen;
  _Atomic int dispatch_count;
  _Atomic int remove_rv;
  _Atomic bool first_dispatch_done;
  _Atomic int removed_count;
} evl_early_reg_ctx;

// The context the add hook waits on; NULL outside that test.
static evl_early_reg_ctx *_Atomic g_evl_early_reg_ctx = NULL;

// On the first dispatch it records its own handle and the one the adder has
// stored so far, then removes itself with its own handle; the early-callback
// test checks that this works before the add returns.
static void evl_early_reg_on_readable(ceventloop loop, ceventloop_reg reg,
                                      cselectable *sel, void *arg) {
  (void)sel;
  evl_early_reg_ctx *c = (evl_early_reg_ctx *)arg;
  if (atomic_fetch_add(&c->dispatch_count, 1) != 0) return;
  atomic_store(&c->stored_seen, atomic_load(&c->returned_reg));
  atomic_store(&c->reg_seen, reg);
  atomic_store(&c->remove_rv, (int)ceventloop_remove(loop, reg));
  atomic_store(&c->first_dispatch_done, true);
}

// Counts the on_removed calls of the early-callback test.
static void evl_early_reg_on_removed(void *arg) {
  evl_early_reg_ctx *c = (evl_early_reg_ctx *)arg;
  atomic_fetch_add(&c->removed_count, 1);
}

// The add hook of the early-callback test: Runs inside ceventloop_add() and
// waits, for at most 10 s, until the first dispatch of the new registration has
// finished. It does nothing while g_evl_early_reg_ctx is NULL.
static void evl_early_reg_add_hook(void) {
  evl_early_reg_ctx *c = atomic_load(&g_evl_early_reg_ctx);
  if (!c) return;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; i < 10000 && !atomic_load(&c->first_dispatch_done); i++)
    nanosleep(&ts, NULL);
}

TEST(ceventloop, callback_before_add_returns_removes_itself_by_reg) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char byte = 'x';
  ssize_t wrote = write(pfd[1], &byte, 1);

  evl_early_reg_ctx ctx;
  atomic_init(&ctx.returned_reg, CEVENTLOOP_REG_NULL);
  atomic_init(&ctx.reg_seen, CEVENTLOOP_REG_NULL);
  atomic_init(&ctx.stored_seen, CEVENTLOOP_REG_NULL);
  atomic_init(&ctx.dispatch_count, 0);
  atomic_init(&ctx.remove_rv, -1);
  atomic_init(&ctx.first_dispatch_done, false);
  atomic_init(&ctx.removed_count, 0);

  ceventloop loop = ceventloop_create(8, 4, 4, NULL);
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  if (loop != CEVENTLOOP_NULL && wrote == 1) {
    atomic_store(&g_evl_early_reg_ctx, &ctx);
    ceventloop__test_set_add_before_return_hook(evl_early_reg_add_hook);
    reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read),
        (ceventloop_handlers){.on_readable = evl_early_reg_on_readable,
                              .on_removed = evl_early_reg_on_removed},
        &ctx, NULL);
    ceventloop__test_set_add_before_return_hook(NULL);
    atomic_store(&g_evl_early_reg_ctx, NULL);
    atomic_store(&ctx.returned_reg, reg);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0; i < 10000 && atomic_load(&ctx.removed_count) == 0; i++)
      nanosleep(&ts, NULL);
  }
  int removed_before_destroy = atomic_load(&ctx.removed_count);
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(wrote, (ssize_t)1);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(atomic_load(&ctx.first_dispatch_done));
  REQUIRE_EQ(atomic_load(&ctx.stored_seen), CEVENTLOOP_REG_NULL);
  REQUIRE_EQ(atomic_load(&ctx.reg_seen), reg);
  REQUIRE_EQ(atomic_load(&ctx.remove_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&ctx.dispatch_count), 1);
  REQUIRE_EQ(removed_before_destroy, 1);
  REQUIRE_EQ(atomic_load(&ctx.removed_count), 1);
}

// One worker of the stripe stress tests. The test opens pfd once per worker
// instead of once per iteration, because a stale dispatch can read it while a
// close runs. ctx_log (and cq_log for the queue worker) keeps every iteration's
// context until the loop and all workers are joined, since a remove may return
// while a callback still uses them.
typedef struct {
  ceventloop loop;
  int thread_id;
  int iterations;
  int pfd[2];
  evl_sync_ctx **ctx_log;
  int ctx_log_count;
  ccircq **cq_log;
} evl_stripe_stress_args;

// One worker of the fd stripe stress test: Registers its pipe, writes to it,
// waits for the dispatch and removes the registration, iterations times. Each
// context goes into a->ctx_log to be freed after the loop is joined.
static void *evl_stripe_stress_fd_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        a->loop, cselectable_fd(a->pfd[0], cselect_read), handlers, ctx, &err);
    if (reg) {
      a->ctx_log[a->ctx_log_count++] = ctx;
      int val = a->thread_id;
      test_write_retry_eintr(a->pfd[1], &val, sizeof(val));
      evl_wait_for(ctx, &ctx->readable_count, 1, EVL_WAIT_MS);
      ceventloop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
    }
  }
  return NULL;
}

// Many threads and many distinct fds over several stripes, so adds, removes and
// dispatches run in parallel. Failures are counted and asserted only after
// every started thread is joined.
TEST(ceventloop, multi_threaded_multi_fd_stress_with_stripes) {
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  size_t residual_regs = 0;
  {
    ceventloop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_fd_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    all_started = (started == n_threads);
    residual_regs = ceventloop_reg_count(loop);
  }

  for (int i = 0; i < n_threads; i++) {
    close(args[i].pfd[0]);
    close(args[i].pfd[1]);
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      evl_sync_ctx_destroy(args[i].ctx_log[j]);
      free(args[i].ctx_log[j]);
    }
    free(args[i].ctx_log);
  }

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(residual_regs, (size_t)0);
}

// One worker of the queue stripe stress test: Registers a new circular queue,
// sends to it, waits for the dispatch and removes the registration, iterations
// times. Each queue goes into a->cq_log beside its context.
static void *evl_stripe_stress_queue_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    ccircq *cq = ccircq_create(4, NULL);
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        a->loop, ccircq_selectable(cq, cselect_read), handlers, ctx, &err);
    if (reg) {
      a->ctx_log[a->ctx_log_count] = ctx;
      a->cq_log[a->ctx_log_count] = cq;
      a->ctx_log_count++;
      int *payload = malloc(sizeof(int));
      *payload = a->thread_id;
      cmessage msg = {.data = payload, .size = sizeof(int)};
      ccircq_send(cq, &msg);
      evl_wait_for(ctx, &ctx->readable_count, 1, EVL_WAIT_MS);
      ceventloop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
      ccircq_destroy(cq);
    }
  }
  return NULL;
}

// A message left in a queue is a dispatch that never came. It is counted and
// drained first, because destroying a queue that holds a message ends the
// process.
TEST(ceventloop, multi_threaded_multi_queue_stress_with_stripes) {
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].cq_log = malloc(sizeof(ccircq *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  size_t residual_regs = 0;
  {
    ceventloop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_queue_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    all_started = (started == n_threads);
    residual_regs = ceventloop_reg_count(loop);
  }

  size_t undelivered = 0;
  for (int i = 0; i < n_threads; i++) {
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      evl_sync_ctx_destroy(args[i].ctx_log[j]);
      free(args[i].ctx_log[j]);
      cmessage left = {.data = NULL, .size = 0};
      while (ccircq_try_recv(args[i].cq_log[j], &left) == ccol_success) {
        free(left.data);
        left = (cmessage){.data = NULL, .size = 0};
        undelivered++;
      }
      ccircq_destroy(args[i].cq_log[j]);
    }
    free(args[i].ctx_log);
    free(args[i].cq_log);
  }

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(residual_regs, (size_t)0);
  REQUIRE_EQ(undelivered, (size_t)0);
}

// The destroy walks every stripe, stripe 0 included, to free registrations
// nobody removed.
TEST(ceventloop, destroy_frees_registrations_across_multiple_stripes) {
  const int n = 20;
  int pfds[20][2];
  ccircq *queues[20];
  ceventloop_handlers handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  {
    ceventloop_construct_scoped(loop, 32, 8, 1);
    char *err = NULL;

    for (int i = 0; i < n; i++) {
      REQUIRE_EQ(pipe(pfds[i]), 0);
      ceventloop_reg reg = ceventloop_add(
          loop, cselectable_fd(pfds[i][0], cselect_read), handlers, NULL, &err);
      REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

      queues[i] = ccircq_create(4, NULL);
      ceventloop_reg qreg =
          ceventloop_add(loop, ccircq_selectable(queues[i], cselect_read),
                         handlers, NULL, &err);
      REQUIRE_NE(qreg, CEVENTLOOP_REG_NULL);
    }
    REQUIRE_EQ(ceventloop_reg_count(loop), (size_t)(2 * n));
  }

  for (int i = 0; i < n; i++) {
    close(pfds[i][0]);
    close(pfds[i][1]);
    ccircq_destroy(queues[i]);
  }
}

// Tests with more than one reactor thread
TEST(ceventloop, multi_thread_basic_smoke) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 4, 6);

    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

    ceventloop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

// Counts the callbacks running at once for one entry under mtx. violation
// records that two ran together.
typedef struct evl_no_double_dispatch_ctx {
  pthread_mutex_t mtx;
  int active;
  bool violation;
  _Atomic int total_calls;
} evl_no_double_dispatch_ctx;

// Counts the callbacks running at once for one entry in c->active and flags a
// second one in c->violation. It holds a 0.2 ms window open, so a second
// reactor thread that also saw the fd ready has room to enter at the same time;
// only a broken dispatch lock lets it in.
static void evl_no_double_dispatch_on_readable(ceventloop loop,
                                               ceventloop_reg reg,
                                               cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;

  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

// The pipe end a feeder thread writes to and how many bytes it writes.
typedef struct evl_feeder_args {
  int write_fd;
  int iterations;
} evl_feeder_args;

// Writes a->iterations bytes to a->write_fd, the hot fd of the double-dispatch
// test. A full pipe that blocks it briefly keeps the read end ready for every
// reactor thread at once.
static void *evl_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n;
  }
  return NULL;
}

// The thundering-herd case: Many reactor threads and one hot fd. The entry's
// dispatch lock must keep active at 1.
TEST(ceventloop, multi_thread_no_double_dispatch_same_fd) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    ceventloop_construct_scoped(loop, 8, 4, 12);

    ceventloop_handlers handlers = {
        .on_readable = evl_no_double_dispatch_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 4000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_feeder_thread, &feeder_args),
               0);
    pthread_join(feeder, NULL);

    for (int spins = 0;
         spins < 400 && atomic_load(&ctx.total_calls) < feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    ceventloop_remove(loop, reg);
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(pfd[0]);
  close(pfd[1]);
}

// Reads the fd and holds a 0.2 ms window, counting the callbacks running at
// once in c->active. The write callback below shares the count, which checks
// that the dispatch lock also keeps the two directions of one fd apart.
static void evl_cross_dir_on_readable(ceventloop loop, ceventloop_reg reg,
                                      cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

// The write-direction twin of evl_cross_dir_on_readable(): Holds the same
// window under the same active count.
static void evl_cross_dir_on_writable(ceventloop loop, ceventloop_reg reg,
                                      cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

// Both directions of sv[0] stay ready for the whole test. The nested block
// joins every reactor thread before ctx goes, since a remove doesn't stop
// another reactor thread's duplicate dispatch of a live entry.
TEST(ceventloop, multi_thread_cross_direction_serialization) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  evl_set_nonblocking(sv[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    ceventloop_construct_scoped(loop, 8, 4, 12);

    ceventloop_handlers read_handlers = {
        .on_readable = evl_cross_dir_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ceventloop_handlers write_handlers = {
        .on_readable = NULL,
        .on_writable = evl_cross_dir_on_writable,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), read_handlers, &ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), write_handlers, &ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);

    evl_feeder_args feeder_args = {.write_fd = sv[1], .iterations = 4000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_feeder_thread, &feeder_args),
               0);
    pthread_join(feeder, NULL);

    for (int spins = 0; spins < 400 && atomic_load(&ctx.total_calls) < 500;
         spins++) {
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    ceventloop_remove(loop, rreg);
    ceventloop_remove(loop, wreg);
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(sv[0]);
  close(sv[1]);
}

// One registration and the generation its dispatches must see.
typedef struct evl_reuse_ctx {
  ceventloop_reg reg;
  uint64_t expected_generation;
  _Atomic int mismatch_count;
  _Atomic int call_count;
} evl_reuse_ctx;

// Drains the fd and counts a dispatch whose registration doesn't carry
// c->expected_generation in c->mismatch_count, which a stale entry on a reused
// fd would cause. It counts every call in c->call_count last.
static void evl_reuse_on_readable(ceventloop loop, ceventloop_reg reg,
                                  cselectable *sel, void *arg) {
  (void)reg;
  evl_reuse_ctx *c = (evl_reuse_ctx *)arg;
  char buf[16];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  if (ceventloop_reg_generation(loop, c->reg) != c->expected_generation) {
    atomic_fetch_add(&c->mismatch_count, 1);
  }
  atomic_fetch_add(&c->call_count, 1);
}

// One driver thread. ctx_log keeps every iteration's context until the loop is
// joined, because another reactor thread can still run a callback after the
// remove returns. pfd stays open for the whole run, since each add of it mints
// a fresh entry anyway and a close would race a stale read.
typedef struct evl_reuse_driver_args {
  ceventloop loop;
  int iterations;
  _Atomic int mismatch_total;
  evl_reuse_ctx **ctx_log;
  int ctx_log_count;
  int pfd[2];
} evl_reuse_driver_args;

// One driver of the generation test: Registers its fd, writes, waits for the
// dispatch and removes it, iterations times, adding the mismatches to
// a->mismatch_total. The remove waits for the dispatch, because a callback that
// reads the generation after the remove gets 0 and counts a false mismatch.
static void *evl_reuse_driver_thread(void *arg) {
  evl_reuse_driver_args *a = (evl_reuse_driver_args *)arg;
  ceventloop_handlers handlers = {.on_readable = evl_reuse_on_readable,
                                  .on_writable = NULL,
                                  .on_error = NULL};

  for (int i = 0; i < a->iterations; i++) {
    evl_reuse_ctx *ctx = malloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        a->loop, cselectable_fd(a->pfd[0], cselect_read), handlers, ctx, &err);
    if (!reg) {
      free(ctx);
      continue;
    }
    ctx->reg = reg;
    ctx->expected_generation = ceventloop_reg_generation(a->loop, reg);
    a->ctx_log[a->ctx_log_count++] = ctx;

    int val = i;
    ssize_t wn = write(a->pfd[1], &val, sizeof(val));
    (void)wn;

    long long deadline = mono_now_us() + EVL_WAIT_MS * 1000LL;
    while (atomic_load(&ctx->call_count) == 0 && mono_now_us() < deadline) {
      struct timespec ts = {0, 500000};
      nanosleep(&ts, NULL);
    }

    ceventloop_remove(a->loop, reg);

    atomic_fetch_add(&a->mismatch_total, atomic_load(&ctx->mismatch_count));
  }
  return NULL;
}

// Several drivers churn their own fd on one loop, and every dispatch must see
// the generation of its own registration.
TEST(ceventloop, multi_thread_fd_reuse_generation_stays_consistent) {
  const int n_drivers = 4;
  const int iterations = 150;
  pthread_t drivers[4];
  evl_reuse_driver_args args[4];

  for (int i = 0; i < n_drivers; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].iterations = iterations;
    atomic_init(&args[i].mismatch_total, 0);
    args[i].ctx_log = malloc(sizeof(evl_reuse_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  {
    ceventloop_construct_scoped(loop, 8, 4, 8);
    int started = 0;
    for (int i = 0; i < n_drivers; i++) {
      args[i].loop = loop;
      if (pthread_create(&drivers[i], NULL, evl_reuse_driver_thread,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(drivers[i], NULL);
    }

    all_started = (started == n_drivers);
  }

  int total_mismatches = 0;
  for (int i = 0; i < n_drivers; i++) {
    close(args[i].pfd[0]);
    close(args[i].pfd[1]);
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      free(args[i].ctx_log[j]);
    }
    free(args[i].ctx_log);
    total_mismatches += atomic_load(&args[i].mismatch_total);
  }

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(total_mismatches, 0);
}

// With nothing registered only the poller waits in the kernel, and the one
// write to the shutdown eventfd must wake and join it within the bound.
TEST(ceventloop, multi_thread_shutdown_joins_poller_promptly) {
  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(ceventloop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  ceventloop_destroy(loop);
}

// The other half of a shutdown: The drain of a large idle dispatch pool must
// wake every worker within the bound.
TEST(ceventloop, multi_thread_shutdown_drains_idle_dispatch_pool_promptly) {
  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(ceventloop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  ceventloop_destroy(loop);
}

// The two flags of evl_shutdown_drain_on_readable(), under mtx.
typedef struct evl_shutdown_drain_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool started;
  bool finished;
} evl_shutdown_drain_ctx;

// Sets started, sleeps 100 ms and sets finished, under c->mtx, so the test can
// shut the loop down while this dispatch is in flight and then check that it
// completed.
static void evl_shutdown_drain_on_readable(ceventloop loop, ceventloop_reg reg,
                                           cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_shutdown_drain_ctx *c = (evl_shutdown_drain_ctx *)arg;
  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;

  pthread_mutex_lock(&c->mtx);
  c->started = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);

  struct timespec ts = {0, 100000000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->finished = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// A shutdown finishes the in-flight dispatch before it returns, so the callback
// has completed when it does.
TEST(ceventloop, multi_thread_shutdown_drains_in_flight_dispatch_job) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_shutdown_drain_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);
  assert(pthread_cond_init(&ctx.cond, NULL) == 0);

  ceventloop loop = ceventloop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  ceventloop_handlers handlers = {.on_readable = evl_shutdown_drain_on_readable,
                                  .on_writable = NULL,
                                  .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  bool started = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.started, EVL_WAIT_MS);
  REQUIRE_TRUE(started);

  REQUIRE_EQ(ceventloop_shutdown(loop), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  bool finished = ctx.finished;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_TRUE(finished);

  ceventloop_destroy(loop);
  pthread_mutex_destroy(&ctx.mtx);
  pthread_cond_destroy(&ctx.cond);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, reg_generation_semantics) {
  REQUIRE_EQ(ceventloop_reg_generation(CEVENTLOOP_NULL, CEVENTLOOP_REG_NULL),
             (uint64_t)0);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int pfd2[2];
  REQUIRE_EQ(pipe(pfd2), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    ceventloop_construct_scoped(loop, 8, 1, 4);
    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;

    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(rreg, CEVENTLOOP_REG_NULL);
    uint64_t rgen = ceventloop_reg_generation(loop, rreg);
    REQUIRE_GT(rgen, (uint64_t)0);

    ceventloop_handlers write_handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    ceventloop_reg wreg =
        ceventloop_add(loop, cselectable_fd(pfd[0], cselect_write),
                       write_handlers, &ctx, &err);
    REQUIRE_NE(wreg, CEVENTLOOP_REG_NULL);
    REQUIRE_EQ(ceventloop_reg_generation(loop, wreg), rgen);

    ceventloop_remove(loop, wreg);
    REQUIRE_EQ(ceventloop_modify(loop, rreg, cselect_write), ccol_success);
    REQUIRE_EQ(ceventloop_reg_generation(loop, rreg), rgen);
    REQUIRE_EQ(ceventloop_modify(loop, rreg, cselect_read), ccol_success);

    ceventloop_reg reg2 = ceventloop_add(
        loop, cselectable_fd(pfd2[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg2, CEVENTLOOP_REG_NULL);
    REQUIRE_NE(ceventloop_reg_generation(loop, reg2), rgen);

    ceventloop_remove(loop, rreg);
    ceventloop_remove(loop, reg2);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
  close(pfd2[0]);
  close(pfd2[1]);
}

// Does nothing. A few always-writable pipe ends with this callback keep the
// poller cycling fast for the race test below.
static void evl_reg_race_hot_writable(ceventloop loop, ceventloop_reg reg,
                                      cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

// The registration that the two race threads below work on.
typedef struct evl_reg_race_args {
  ceventloop loop;
  ceventloop_reg reg;
} evl_reg_race_args;

// Removes a->reg from a->loop, the remover side of the accessor race.
static void *evl_reg_race_remover_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  ceventloop_remove(a->loop, a->reg);
  return NULL;
}

// Calls every accessor of a->reg while another thread removes it, the other
// side of the race. Any return value is fine; none of the calls may touch freed
// memory.
static void *evl_reg_race_accessor_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  ceventloop_modify(a->loop, a->reg, cselect_write);
  ceventloop_pause(a->loop, a->reg);
  ceventloop_resume(a->loop, a->reg);
  (void)ceventloop_reg_generation(a->loop, a->reg);
  ceventloop_remove(a->loop, a->reg);
  return NULL;
}

// The accessors must resolve the registration handle through the loop's table
// before touching it, since the poller can reclaim it right after another
// thread's remove. Run it under memtest, where an unprotected dereference shows
// as a use-after-free.
TEST(ceventloop, reg_handle_survives_concurrent_remove_vs_accessor_race) {
  ceventloop_construct_scoped(loop, 64, 4, 4);

  enum { N_HOT = 4 };
  int hot_pfd[N_HOT][2];
  ceventloop_reg hot_reg[N_HOT];
  ceventloop_handlers hot_handlers = {.on_readable = NULL,
                                      .on_writable = evl_reg_race_hot_writable,
                                      .on_error = NULL};
  for (int i = 0; i < N_HOT; i++) {
    REQUIRE_EQ(pipe(hot_pfd[i]), 0);
    char *err = NULL;
    hot_reg[i] =
        ceventloop_add(loop, cselectable_fd(hot_pfd[i][1], cselect_write),
                       hot_handlers, NULL, &err);
    REQUIRE_NE(hot_reg[i], CEVENTLOOP_REG_NULL);
  }

  const int iterations = 300;
  ceventloop_handlers reg_handlers = {0};
  for (int i = 0; i < iterations; i++) {
    int pfd[2];
    REQUIRE_EQ(pipe(pfd), 0);
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), reg_handlers, NULL, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    evl_reg_race_args shared = {loop, reg};
    pthread_t remover, accessor;
    REQUIRE_EQ(
        pthread_create(&remover, NULL, evl_reg_race_remover_thread, &shared),
        0);
    REQUIRE_EQ(
        pthread_create(&accessor, NULL, evl_reg_race_accessor_thread, &shared),
        0);
    pthread_join(remover, NULL);
    pthread_join(accessor, NULL);

    close(pfd[0]);
    close(pfd[1]);
  }

  for (int i = 0; i < N_HOT; i++) {
    ceventloop_remove(loop, hot_reg[i]);
    close(hot_pfd[i][0]);
    close(hot_pfd[i][1]);
  }
}

// Counts the calls of a slow callback.
typedef struct evl_bounded_ctx {
  _Atomic size_t calls;
} evl_bounded_ctx;

// Drains the fd, sleeps 0.5 ms and counts the call. It is slower than the
// feeder, so the poller comes back to the kernel wait while a job for this fd
// is queued or running, which the bounded-pending test needs.
static void evl_bounded_hot_fd_on_readable(ceventloop loop, ceventloop_reg reg,
                                           cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  char buf[64];
  ssize_t n;
  do {
    n = read(sel->fd, buf, sizeof(buf));
  } while (n > 0);
  struct timespec ts = {0, 500000};
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

// Writes a->iterations bytes to a->write_fd, the fast feeder of the bounded fd
// test.
static void *evl_bounded_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n;
  }
  return NULL;
}

// An fd that is still ready but not yet re-armed must not be collected again,
// or the pending jobs of the dispatch pool grow without bound behind a slow
// callback. The test samples that count while the feeder runs.
TEST(ceventloop, multi_thread_hot_fd_dispatch_pool_pending_stays_bounded) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    ceventloop_construct_scoped(loop, 8, 4, 8);

    ceventloop_handlers handlers = {
        .on_readable = evl_bounded_hot_fd_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_bounded_feeder_thread, &feeder_args),
        0);

    for (int i = 0; i < 200; i++) {
      size_t pending = ceventloop__dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    for (int spins = 0; spins < 100 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    ceventloop_remove(loop, reg);
  }

  REQUIRE_LT(max_pending, (size_t)10);
}

// The queue a feeder sends to and how many messages it sends.
typedef struct evl_bounded_queue_feeder_args {
  ccircq *cq;
  int iterations;
} evl_bounded_queue_feeder_args;

// Sends a->iterations empty messages to a->cq, the fast feeder of the bounded
// queue test. It uses a blocking send, so a full queue throttles it and no send
// is dropped.
static void *evl_bounded_queue_feeder_thread(void *arg) {
  evl_bounded_queue_feeder_args *a = (evl_bounded_queue_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    cmessage msg = {.data = NULL, .size = 0};
    (void)ccircq_send(a->cq, &msg);
  }
  return NULL;
}

// Takes one message, sleeps 0.5 ms and counts the call; the slow callback of
// the bounded queue test. A bridge eventfd has no finite buffer to throttle
// this case, which makes it the more valuable of the two bounded tests.
static void evl_bounded_hot_queue_on_readable(ceventloop loop,
                                              ceventloop_reg reg,
                                              cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  cmessage msg = {.data = NULL, .size = 0};
  (void)ccircq_try_recv(sel->cq, &msg);
  struct timespec ts = {0, 500000};
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

// A single registration can end with a few messages unread because of the
// coalesced wake, so the test drains the queue itself after the remove.
TEST(ceventloop, multi_thread_hot_queue_dispatch_pool_pending_stays_bounded) {
  ccircq *cq = ccircq_create(64, NULL);
  REQUIRE_NE((void *)cq, NULL);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    ceventloop_construct_scoped(loop, 8, 4, 8);

    ceventloop_handlers handlers = {
        .on_readable = evl_bounded_hot_queue_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers, &ctx, &err);
    REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

    evl_bounded_queue_feeder_args feeder_args = {.cq = cq, .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_bounded_queue_feeder_thread,
                              &feeder_args),
               0);

    for (int i = 0; i < 200; i++) {
      size_t pending = ceventloop__dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    for (int spins = 0; spins < 500 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    ceventloop_remove(loop, reg);
  }

  cmessage leftover = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &leftover) == ccol_success) {
  }

  ccircq_destroy(cq);
  REQUIRE_LT(max_pending, (size_t)10);
}

// A callback calls ceventloop_shutdown() on its own loop and stores the result
// in observed_rv. observed_rv is _Atomic because the main thread polls it while
// a loop thread writes it.
typedef struct evl_self_shutdown_ctx {
  ceventloop loop;
  _Atomic int observed_rv;
} evl_self_shutdown_ctx;

// Shuts its own loop down from inside the callback and stores the result in
// c->observed_rv, which the self-shutdown tests expect to be
// ccol_not_permitted.
static void evl_self_shutdown_on_readable(ceventloop loop, ceventloop_reg reg,
                                          cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  evl_self_shutdown_ctx *c = (evl_self_shutdown_ctx *)arg;
  atomic_store(&c->observed_rv, (int)ceventloop_shutdown(loop));
}

// With one reactor thread a shutdown from a callback would join that thread to
// itself. The loop is destroyed before any assertion, because the callback
// writes to ctx on this stack.
TEST(ceventloop,
     shutdown_from_within_callback_returns_not_permitted_single_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);
  ctx.loop = loop;

  ceventloop_handlers handlers = {.on_readable = evl_self_shutdown_on_readable,
                                  .on_writable = NULL,
                                  .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  long long end = mono_now_us() + (long long)EVL_WAIT_MS * 1000;
  while (atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure &&
         mono_now_us() < end)
    mono_poll_pause();
  int observed = atomic_load(&ctx.observed_rv);

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_EQ(observed, (int)ccol_not_permitted);
}

// With a dispatch pool the callback runs on a pool worker, which would join
// itself in the drain.
TEST(ceventloop,
     shutdown_from_within_callback_returns_not_permitted_multi_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  ceventloop loop = ceventloop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);
  ctx.loop = loop;

  ceventloop_handlers handlers = {.on_readable = evl_self_shutdown_on_readable,
                                  .on_writable = NULL,
                                  .on_error = NULL};
  char *err = NULL;
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  long long end = mono_now_us() + (long long)EVL_WAIT_MS * 1000;
  while (atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure &&
         mono_now_us() < end)
    mono_poll_pause();
  int observed = atomic_load(&ctx.observed_rv);

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_EQ(observed, (int)ccol_not_permitted);
}

// Event loop handle lifecycle
//
// A second destroy, through a copy of a handle that is already torn down, is a
// fatal error. These tests run in a forked child because ccol__fatal_err()
// aborts the process.
TEST(ceventloop_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);
    ceventloop stale = loop;
    ceventloop_destroy(loop);
    ceventloop__destroy(stale);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Destroys its own loop from inside the callback, the misuse that the
// self-destroy tests expect to be fatal.
static void evl_self_destroy_on_readable(ceventloop loop, ceventloop_reg reg,
                                         cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  (void)arg;
  ceventloop_destroy(loop);
}

// A destroy from a callback of the same loop is fatal. The shutdown inside the
// destroy refuses the self-join, but the destroy returns void and would free
// the registrations and the loop under the running callback.
TEST(ceventloop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_single_thread) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) test_child_exit(2);
    evl_set_nonblocking(pfd[0]);

    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);

    ceventloop_handlers handlers = {.on_readable = evl_self_destroy_on_readable,
                                    .on_writable = NULL,
                                    .on_error = NULL};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, NULL);
    if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val))
      test_child_exit(2);

    struct timespec ts = {15, 0};
    nanosleep(&ts, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(ceventloop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_multi_thread) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) test_child_exit(2);
    evl_set_nonblocking(pfd[0]);

    ceventloop loop = ceventloop_create(8, 4, 4, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);

    ceventloop_handlers handlers = {.on_readable = evl_self_destroy_on_readable,
                                    .on_writable = NULL,
                                    .on_error = NULL};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, NULL, NULL);
    if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val))
      test_child_exit(2);

    struct timespec ts = {15, 0};
    nanosleep(&ts, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The handle that evl_concurrent_destroy_thread() destroys.
typedef struct {
  ceventloop h;
} evl_concurrent_destroy_arg_t;

// Destroys a->h through the raw destroy, racing the main thread's destroy of
// the same handle.
static void *evl_concurrent_destroy_thread(void *arg) {
  evl_concurrent_destroy_arg_t *a = (evl_concurrent_destroy_arg_t *)arg;
  ceventloop__destroy(a->h);
  return NULL;
}

// Two threads destroy two copies of one live handle at the same time; whichever
// loses the race must hit ccol__fatal_err().
TEST(ceventloop_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) test_child_exit(2);
    evl_concurrent_destroy_arg_t a1 = {.h = loop};
    evl_concurrent_destroy_arg_t a2 = {.h = loop};
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, evl_concurrent_destroy_thread, &a1) != 0)
      test_child_exit(3);
    if (pthread_create(&t2, NULL, evl_concurrent_destroy_thread, &a2) != 0)
      test_child_exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The handle a pin holder pins and the flags it shares with the test.
typedef struct {
  ceventloop h;
  atomic_int pinned;
  atomic_int release;
  bool resolved;
} evl_pin_hold_arg_t;

// Resolves and pins a->h until the test sets a->release, so a concurrent
// destroy must wait. It stores -1 in a->pinned when the handle didn't resolve,
// so the test doesn't wait for it.
static void *evl_pin_hold_thread(void *arg) {
  evl_pin_hold_arg_t *a = (evl_pin_hold_arg_t *)arg;
  a->resolved =
      ceventloop__resolve_pin_until_for_tests(a->h, &a->pinned, &a->release);
  if (!a->resolved) atomic_store(&a->pinned, -1);
  return NULL;
}

// The handle to destroy and a flag set once the destroy returns.
typedef struct {
  ceventloop h;
  atomic_int returned;
} evl_destroy_arg_t;

// Destroys a->h and then sets a->returned, so the test can see whether the
// destroy has returned.
static void *evl_destroy_thread(void *arg) {
  evl_destroy_arg_t *a = (evl_destroy_arg_t *)arg;
  ceventloop_destroy(a->h);
  atomic_store(&a->returned, 1);
  return NULL;
}

// A destroy must wait while another thread holds a pin. While release is clear
// the pin is held, so a correct destroy can't have returned however slow the
// machine is.
TEST(ceventloop_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  evl_pin_hold_arg_t pin_arg = {.h = loop, .resolved = false};
  atomic_init(&pin_arg.pinned, 0);
  atomic_init(&pin_arg.release, 0);
  evl_destroy_arg_t destroy_arg = {.h = loop};
  atomic_init(&destroy_arg.returned, 0);

  pthread_t pin_thread, destroy_thread;
  bool pin_started =
      pthread_create(&pin_thread, NULL, evl_pin_hold_thread, &pin_arg) == 0;
  struct timespec poll = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; pin_started && i < 10000 && atomic_load(&pin_arg.pinned) == 0;
       i++)
    nanosleep(&poll, NULL);
  bool pinned = atomic_load(&pin_arg.pinned) == 1;

  bool destroy_started = false;
  int returned_while_pinned = -1;
  if (pinned) {
    destroy_started = pthread_create(&destroy_thread, NULL, evl_destroy_thread,
                                     &destroy_arg) == 0;
    struct timespec margin = {.tv_sec = 0, .tv_nsec = 200000000};
    nanosleep(&margin, NULL);
    returned_while_pinned = atomic_load(&destroy_arg.returned);
  }
  atomic_store(&pin_arg.release, 1);
  if (pin_started) pthread_join(pin_thread, NULL);
  if (destroy_started) {
    pthread_join(destroy_thread, NULL);
  } else {
    ceventloop_destroy(loop);
  }

  REQUIRE_TRUE(pin_started);
  REQUIRE_TRUE(pin_arg.resolved);
  REQUIRE_TRUE(pinned);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_EQ(returned_while_pinned, 0);
  REQUIRE_EQ(atomic_load(&destroy_arg.returned), 1);
}

// The handle that evl_reg_count_thread() reads.
typedef struct {
  ceventloop h;
} evl_reg_count_arg_t;

// Makes one ceventloop_reg_count() call to race a destroy; either result is
// correct.
static void *evl_reg_count_thread(void *arg) {
  evl_reg_count_arg_t *a = (evl_reg_count_arg_t *)arg;
  ceventloop_reg_count(a->h);
  return NULL;
}

// The fast counterpart of the test above: A resolve, read and unpin with no
// sleep races a destroy, on a fresh loop each round, many times. The window of
// a lock-free pin is a few instructions wide.
TEST(ceventloop_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, CEVENTLOOP_NULL);

    evl_reg_count_arg_t reg_count_arg = {.h = loop};
    evl_concurrent_destroy_arg_t destroy_arg = {.h = loop};
    pthread_t reg_count_tid, destroy_tid;
    bool started_reg_count_tid =
        (pthread_create(&reg_count_tid, NULL, evl_reg_count_thread,
                        &reg_count_arg) == 0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, evl_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    if (started_reg_count_tid) pthread_join(reg_count_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    REQUIRE_TRUE(started_reg_count_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

TEST(ceventloop_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  ceventloop a = ceventloop_create(8, 1, 1, NULL);
  REQUIRE_NE(a, CEVENTLOOP_NULL);
  ceventloop stale_a = a;
  ceventloop_destroy(a);

  ceventloop b = ceventloop_create(8, 1, 1, NULL);
  REQUIRE_NE(b, CEVENTLOOP_NULL);

  REQUIRE_EQ(ceventloop_reg_count(b), (size_t)0);

  REQUIRE_EQ((void *)ceventloop__resolve_for_tests(stale_a), NULL);

  ceventloop_destroy(b);
}

// Creating and destroying one loop at a time reuses the same slot. The capacity
// is taken after the first pair, since earlier tests may have grown the
// table.
TEST(ceventloop_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  ceventloop loop0 = ceventloop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop0, CEVENTLOOP_NULL);
  ceventloop_destroy(loop0);
  size_t capacity_after_first = ceventloop__slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, CEVENTLOOP_NULL);
    ceventloop_destroy(loop);
  }

  REQUIRE_EQ(ceventloop__slot_table_capacity_for_tests(), capacity_after_first);
}

// Fork safety
//
// The rest of these tests exercise the pthread_atfork() machinery of the event
// loop and of the queue mutex registry. A build with CCOL_FORK_SAFETY_REQUIRED
// at 0 drops that machinery, so it drops these tests too.
#if CCOL_FORK_SAFETY_REQUIRED

// Drains the fd without blocking; the callback of the busy loop in the fork
// test.
static void fork_safety_hot_fd_on_readable(ceventloop loop, ceventloop_reg reg,
                                           cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)arg;
  char buf[64];
  while (read(sel->fd, buf, sizeof(buf)) > 0) {
  }
}

// The stop flag of the churn thread.
typedef struct {
  _Atomic int stop;
} fork_safety_churn_arg_t;

// Creates and destroys throwaway loops until a->stop is set, so some thread is
// inside the slot-table mutex as often as possible while the test forks.
static void *fork_safety_churn_thread(void *arg) {
  fork_safety_churn_arg_t *a = (fork_safety_churn_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ceventloop l = ceventloop_create_mp(4, 1, 1, NULL, &err);
    if (l != CEVENTLOOP_NULL) {
      int pfd[2];
      if (pipe(pfd) == 0) {
        ceventloop_handlers h = {0};
        ceventloop_reg r = ceventloop_add(
            l, cselectable_fd(pfd[0], cselect_read), h, NULL, NULL);
        (void)r;
        close(pfd[0]);
        close(pfd[1]);
      }
      ceventloop_destroy(l);
    }
  }
  return NULL;
}

// fork() copies only the calling thread, so without the atfork handlers a child
// can inherit the slot-table mutex or a loop's locks locked. A churn thread and
// a busy single-stripe loop keep those locks taken while the test forks, and
// each child adds one more registration under alarm(30).
TEST(fork_safety, fork_does_not_inherit_a_locked_event_loop_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  alarm(360);
  fork_safety_churn_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  REQUIRE_EQ(pthread_create(&churn_tid, NULL, fork_safety_churn_thread, &churn),
             0);

  char *err = NULL;
  ceventloop loop = ceventloop_create_mp(16, 1, 3, NULL, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  int hotfd[2];
  REQUIRE_EQ(pipe(hotfd), 0);
  evl_set_nonblocking(hotfd[0]);
  ceventloop_handlers h = {.on_readable = fork_safety_hot_fd_on_readable,
                           .on_writable = NULL,
                           .on_error = NULL};
  ceventloop_reg reg = ceventloop_add(
      loop, cselectable_fd(hotfd[0], cselect_read), h, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  enum { TRIALS = 30 };
  int hangs = 0;
  for (int i = 0; i < TRIALS; i++) {
    char byte = 'x';
    REQUIRE_EQ(write(hotfd[1], &byte, 1), 1);

    pid_t pid = fork();
    REQUIRE_NE(pid, -1);
    if (pid == 0) {
      alarm(30);

      int fd2[2];
      if (pipe(fd2) != 0) test_child_exit(2);
      evl_set_nonblocking(fd2[0]);
      ceventloop_handlers h2 = {0};
      ceventloop_reg r2 = ceventloop_add(
          loop, cselectable_fd(fd2[0], cselect_read), h2, NULL, NULL);
      int rc = r2 ? 0 : 1;
      if (r2 != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r2);
      test_child_exit(rc);
    }

    int status = 0;
    bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
    REQUIRE_TRUE(reaped);
    if (!reaped) return;
    if (!WIFEXITED(status)) hangs++;
  }

  REQUIRE_EQ(hangs, 0);

  atomic_store(&churn.stop, 1);
  pthread_join(churn_tid, NULL);

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  close(hotfd[0]);
  close(hotfd[1]);

  alarm(0);
}

// The queue a holder thread keeps locked for hold_ms, and the flags it sets
// once it holds the lock and just before it releases it.
typedef struct {
  ccircq *cq;
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} queue_fork_lock_arg_t;

// Locks a->cq's mutex, sets a->locked, holds it for a->hold_ms, sets
// a->released and unlocks, so the test can fork while another thread holds the
// mutex. It releases on its own schedule: The fork's prepare handler waits for
// this unlock, so a release that waited for the forking thread would deadlock.
// released is stored before the unlock, so fork() can't return before it is
// visible.
static void *queue_fork_lock_thread(void *arg) {
  queue_fork_lock_arg_t *a = (queue_fork_lock_arg_t *)arg;
  ccircq__test_lock_mutex_for_tests(a->cq);
  atomic_store(&a->locked, true);
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&a->released, true);
  ccircq__test_unlock_mutex_for_tests(a->cq);
  return NULL;
}

// A holder thread keeps a queue's mutex locked for HOLD_MS, and the test forks
// once it holds it. The atfork prepare handler must block until the holder
// releases, so released is already true when fork() returns, and the child can
// use the queue. The child reports through a pipe, since valgrind can override
// a child's exit status.
TEST(fork_safety, fork_does_not_inherit_a_locked_circular_queue_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ccircq *cq = ccircq_create(4, &err);
  REQUIRE_NE((void *)cq, NULL);

  enum { HOLD_MS = 300 };
  queue_fork_lock_arg_t arg = {
      .cq = cq, .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, queue_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    sched_yield();
  }

  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);
  pid_t pid = -1;
  bool released_at_fork = false;
  if (pipe_ok) pid = fork();
  if (pipe_ok && pid == 0) {
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    cmessage msg = {.data = NULL, .size = 0};
    ccol_retval rv = ccircq_try_send(cq, &msg);
    char byte = (rv == ccol_success) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    test_child_exit(0);
  }
  if (pipe_ok) {
    released_at_fork = atomic_load(&arg.released);
    close(result_pipe[1]);
    if (pid == -1) close(result_pipe[0]);
  }

  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(released_at_fork);

  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  ccircq_destroy(cq);
}

// The loop whose registration-slot lock a holder thread write-locks for
// hold_ms, and the flags it sets around the hold.
typedef struct {
  ceventloop loop;
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} reg_slot_fork_lock_arg_t;

// Write-locks a->loop's registration-slot lock for a->hold_ms, so the test can
// fork while another thread holds it, then sets a->released and unlocks through
// the pointer the lock call returned (resolving the loop again would deadlock
// against the fork). It releases on its own schedule, as
// queue_fork_lock_thread() does.
static void *reg_slot_fork_lock_thread(void *arg) {
  reg_slot_fork_lock_arg_t *a = (reg_slot_fork_lock_arg_t *)arg;
  void *resolved = ceventloop__test_wrlock_reg_slot_for_tests(a->loop);
  atomic_store(&a->locked, true);
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&a->released, true);
  ceventloop__test_wrunlock_reg_slot_for_tests(resolved);
  return NULL;
}

// glibc tracks a rwlock writer by TID, so a plain unlock in the child does
// nothing for a lock that another thread write-locked, and every resolve in the
// child hangs. The child must reinitialize the lock instead. The structure is
// that of the circular queue test above.
TEST(fork_safety, fork_does_not_inherit_a_write_locked_reg_slot_rwlock) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ceventloop loop = ceventloop_create_mp(4, 1, 1, NULL, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  ceventloop_handlers h = {.on_readable = fork_safety_hot_fd_on_readable,
                           .on_writable = NULL,
                           .on_error = NULL};
  ceventloop_reg reg =
      ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), h, NULL, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  enum { HOLD_MS = 300 };
  reg_slot_fork_lock_arg_t arg = {
      .loop = loop, .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, reg_slot_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    sched_yield();
  }

  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);
  pid_t pid = -1;
  bool released_at_fork = false;
  if (pipe_ok) pid = fork();
  if (pipe_ok && pid == 0) {
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    uint64_t gen = ceventloop_reg_generation(loop, reg);
    char byte = (gen != 0) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    test_child_exit(0);
  }
  if (pipe_ok) {
    released_at_fork = atomic_load(&arg.released);
    close(result_pipe[1]);
    if (pid == -1) close(result_pipe[0]);
  }

  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(released_at_fork);

  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  uint64_t gen_after = ceventloop_reg_generation(loop, reg);
  REQUIRE_NE(gen_after, (uint64_t)0);

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

// The flags and hold time of the slot-table lock holder.
typedef struct {
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} ceventloop_slot_table_fork_lock_arg_t;

// Write-locks the process-wide loop table for a->hold_ms, so the test can fork
// while another thread holds it, then sets a->released and unlocks, on its own
// schedule like queue_fork_lock_thread().
static void *ceventloop_slot_table_fork_lock_thread(void *arg) {
  ceventloop_slot_table_fork_lock_arg_t *a =
      (ceventloop_slot_table_fork_lock_arg_t *)arg;
  ceventloop__test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&a->released, true);
  ceventloop__test_wrunlock_slot_table_for_tests();
  return NULL;
}

// The same TID problem for the process-wide loop table, which every create,
// destroy and resolve takes. Every outcome is kept in a local and asserted only
// after the holder and the child are reaped, so no failure leaks the holder.
TEST(fork_safety, fork_does_not_inherit_a_write_locked_event_loop_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  enum { HOLD_MS = 300 };
  ceventloop_slot_table_fork_lock_arg_t arg = {
      .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL,
                            ceventloop_slot_table_fork_lock_thread, &arg),
             0);

  while (!atomic_load(&arg.locked)) {
    sched_yield();
  }

  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);

  pid_t pid = -1;
  bool released_at_fork = false;
  char byte = 0;
  ssize_t n = -1;
  bool reaped = false;
  int status = 0;

  if (pipe_ok) {
    pid = fork();
    if (pid == 0) {
      close(result_pipe[0]);
      alarm(30);
      char *err = NULL;
      ceventloop l = ceventloop_create(4, 1, 1, &err);
      char b = (l != CEVENTLOOP_NULL) ? 1 : 0;
      if (l != CEVENTLOOP_NULL) ceventloop_destroy(l);
      test_write_retry_eintr(result_pipe[1], &b, 1);
      close(result_pipe[1]);
      test_child_exit(0);
    }

    if (pid > 0) {
      released_at_fork = atomic_load(&arg.released);
      close(result_pipe[1]);

      n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
      close(result_pipe[0]);

      reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
    } else {
      close(result_pipe[0]);
      close(result_pipe[1]);
    }
  }

  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(released_at_fork);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  char *err2 = NULL;
  ceventloop l2 = ceventloop_create(4, 1, 1, &err2);
  REQUIRE_NE(l2, CEVENTLOOP_NULL);
  ceventloop_destroy(l2);
}

// Does nothing; the queue registration of the lock-order test only has to
// exist.
static void fork_safety2_noop_readable(ceventloop loop, ceventloop_reg reg,
                                       cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

// The queue a sender thread sends to and a flag it sets when it starts.
typedef struct {
  ccircq *cq;
  _Atomic bool started;
} fork_safety2_sender_arg_t;

// Sets a->started and sends one empty message to a->cq, the concurrent send
// that takes the queue mutex and the registration's wait mutex while the child
// forks.
static void *fork_safety2_sender_thread(void *arg) {
  fork_safety2_sender_arg_t *a = (fork_safety2_sender_arg_t *)arg;
  atomic_store(&a->started, true);
  cmessage msg = {.data = NULL, .size = 0};
  ccircq_send(a->cq, &msg);
  return NULL;
}

// Atfork prepare handlers run in reverse registration order. With a queue
// created before the first loop, two separate handler sets would lock a
// registration's wait mutex and then the queue mutex, the reverse of the order
// a concurrent send uses, and fork() deadlocks. Both subsystems therefore share
// one atfork registration. The child creates the queue first and forks while a
// sender runs.
TEST(fork_safety,
     fork_does_not_deadlock_with_queue_registered_before_event_loop) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_AGAIN_IN_A_THREADED_CHILD();
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  pid_t outer_pid = fork();
  REQUIRE_NE(outer_pid, -1);
  if (outer_pid == 0) {
    close(result_pipe[0]);
    alarm(180);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }

    char byte = 0;

    char *err = NULL;
    ccircq *cq = ccircq_create(4, &err);
    ceventloop loop = cq ? ceventloop_create(16, 1, 1, &err) : CEVENTLOOP_NULL;
    ceventloop_reg reg = CEVENTLOOP_REG_NULL;
    if (cq && loop) {
      ceventloop_handlers h = {.on_readable = fork_safety2_noop_readable};
      reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read), h, NULL,
                           &err);
    }

    if (cq && loop && reg) {
      cthreadcomm__notify_waiter_test_set_delay_us(300000);

      fork_safety2_sender_arg_t sarg = {.cq = cq, .started = false};
      pthread_t sender;
      if (pthread_create(&sender, NULL, fork_safety2_sender_thread, &sarg) ==
          0) {
        while (!atomic_load(&sarg.started)) {
          sched_yield();
        }
        usleep(50000);

        pid_t inner_pid = fork();
        if (inner_pid == 0) {
          test_child_exit(0);
        } else if (inner_pid > 0) {
          int inner_status = 0;
          (void)_wait_for_forked_child_bounded(inner_pid, &inner_status, 10000);
          byte = 1;
        }

        cthreadcomm__notify_waiter_test_set_delay_us(0);
        pthread_join(sender, NULL);
      }
    }

    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);

    if (reg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, reg);
    if (cq) {
      cmessage drain = {0};
      ccircq_try_recv(cq, &drain);
      ccircq_destroy(cq);
    }
    if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
    test_child_exit(0);
  }

  close(result_pipe[1]);
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 200000);
  close(result_pipe[0]);

  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(outer_pid, &status, 200000);
  REQUIRE_TRUE(reaped);

  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
}

// A child destroys a loop it inherited. Its poller and pool threads don't exist
// in the child and the shutdown eventfd is shared with the parent, so the
// destroy must neither join them nor write that eventfd, and the parent's loop
// must keep working.
TEST(fork_safety, ceventloop_destroy_of_inherited_loop_in_child_is_safe) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  char *err = NULL;
  ceventloop loop = ceventloop_create_mp(8, 2, 3, NULL, &err);
  REQUIRE_NE(loop, CEVENTLOOP_NULL);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers h = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ceventloop_reg reg =
      ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), h, &ctx, &err);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    close(result_pipe[0]);
    close(pfd[0]);
    close(pfd[1]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(5);

    ceventloop_destroy(loop);

    char byte = 1;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    test_child_exit(0);
  }

  close(result_pipe[1]);
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);

  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);

  if (reaped) REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  char val = 'x';
  REQUIRE_EQ(write(pfd[1], &val, 1), 1);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, EVL_WAIT_MS));

  ceventloop_remove(loop, reg);
  ceventloop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

#endif

// Queue registration lifetime against the queue itself
//
// Counts the file descriptors this process has open (see test_fds.h), so the
// idle-loop test can see a leaked bridge eventfd.
static size_t evl_count_open_fds(void) { return (size_t)test_count_open_fds(); }

// Sleeps ms milliseconds. It only paces the bounded polling loops of these
// tests and never decides a result.
static void evl_sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// The cascade after a queue dispatch locks the queue's mutex, and the
// documented retirement is a remove followed at once by the queue's destroy. So
// remove must not return while a cascade is on its way. The cascade publishes
// begun and finished counts, an armed delay widens the window, and remove must
// not return between the two.
TEST(ceventloop, queue_cascade_finishes_before_remove_returns) {
  ceventloop_construct_scoped(loop, 8, 4, 1);
  ccircq *cq = ccircq_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ceventloop_handlers handlers = {.on_readable = evl_on_readable,
                                  .on_writable = NULL,
                                  .on_error = NULL,
                                  .on_removed = NULL};
  char *err = NULL;
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  if (queue_created)
    reg = ceventloop_add(loop, ccircq_selectable(cq, cselect_read), handlers,
                         &ctx, &err);
  bool registered = (reg != CEVENTLOOP_REG_NULL);

  uint64_t begun_before = 0, finished_before = 0;
  ceventloop__test_queue_cascade_counts(&begun_before, &finished_before);

  bool sent = false, dispatched = false, cascade_begun = false;
  if (registered) {
    ceventloop__test_delay_next_queue_cascade_us(300000);
    char *payload = (char *)malloc(4);
    if (payload) {
      memcpy(payload, "abc", 4);
      cmessage msg = {.data = payload, .size = 4};
      sent = (ccircq_send(cq, &msg) == ccol_success);
      if (!sent) free(payload);
    }
    dispatched = sent && evl_wait_for(&ctx, &ctx.readable_count, 1, 5000);

    for (int i = 0; i < 5000 && !cascade_begun; i++) {
      uint64_t begun = 0;
      ceventloop__test_queue_cascade_counts(&begun, NULL);
      if (begun > begun_before) {
        cascade_begun = true;
        break;
      }
      evl_sleep_ms(1);
    }
  }

  ccol_retval remove_rv = ccol_invalid_args;
  uint64_t finished_at_return = finished_before;
  if (registered) {
    remove_rv = ceventloop_remove(loop, reg);
    ceventloop__test_queue_cascade_counts(NULL, &finished_at_return);
  }

  if (queue_created) ccircq_destroy(cq);

  ceventloop__test_delay_next_queue_cascade_us(0);
  evl_sync_ctx_destroy(&ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(dispatched);
  REQUIRE_TRUE(cascade_begun);
  REQUIRE_EQ(remove_rv, ccol_success);
  REQUIRE_GT(finished_at_return, finished_before);
}

// Waits up to timeout_ms until the poller of loop stops completing kernel
// waits, which is how a poller parked with nothing to reclaim looks from
// outside; the idle-loop test takes its baseline then. The count must be
// non-zero, so a poller that hasn't started doesn't count as parked.
static bool evl_wait_until_poller_parked(ceventloop loop, int timeout_ms) {
  uint64_t prev = ceventloop__poller_iterations_for_tests(loop);
  for (int waited = 0; waited < timeout_ms; waited += 150) {
    evl_sleep_ms(150);
    uint64_t now = ceventloop__poller_iterations_for_tests(loop);
    if (now > 0 && now == prev) return true;
    prev = now;
  }
  return false;
}

// Only the poller closes a removed registration's bridge eventfd, and a DEL or
// ADD doesn't wake a blocked kernel wait, so the remove must wake an idle
// poller. The baseline is taken with the poller parked; otherwise its first
// pass sweeps the burst up and the test passes anyway.
TEST(ceventloop, idle_loop_reclaims_removed_registrations_without_handlers) {
  enum { CYCLES = 8 };
  ceventloop_construct_scoped(loop, 8, 4, 1);
  ccircq *cq = ccircq_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  ceventloop_handlers warm_handlers = {.on_readable = evl_on_readable,
                                       .on_writable = NULL,
                                       .on_error = NULL,
                                       .on_removed = NULL};
  char *err = NULL;
  ceventloop_reg warm = CEVENTLOOP_REG_NULL;
  if (queue_created)
    warm = ceventloop_add(loop, ccircq_selectable(cq, cselect_read),
                          warm_handlers, &ctx, &err);
  bool warmed = false, warm_removed = false;
  if (warm != CEVENTLOOP_REG_NULL) {
    char *payload = (char *)malloc(4);
    if (payload) {
      memcpy(payload, "abc", 4);
      cmessage msg = {.data = payload, .size = 4};
      if (ccircq_send(cq, &msg) == ccol_success)
        warmed = evl_wait_for(&ctx, &ctx.readable_count, 1, 5000);
      else
        free(payload);
    }
    warm_removed = (ceventloop_remove(loop, warm) == ccol_success);
  }

  bool parked =
      warmed && warm_removed && evl_wait_until_poller_parked(loop, 5000);

  size_t baseline = evl_count_open_fds();

  ceventloop_handlers handlers = {.on_readable = NULL,
                                  .on_writable = NULL,
                                  .on_error = NULL,
                                  .on_removed = NULL};
  bool all_cycles_ok = parked;
  for (int i = 0; i < CYCLES && all_cycles_ok; i++) {
    char *cycle_err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers, NULL, &cycle_err);
    if (reg == CEVENTLOOP_REG_NULL) {
      all_cycles_ok = false;
      break;
    }
    if (ceventloop_remove(loop, reg) != ccol_success) all_cycles_ok = false;
  }

  bool reclaimed = false;
  size_t observed = baseline;
  long long end = mono_now_us() + (long long)EVL_WAIT_MS * 1000;
  while (!reclaimed) {
    observed = evl_count_open_fds();
    if (observed <= baseline) {
      reclaimed = true;
      break;
    }
    if (mono_now_us() >= end) break;
    evl_sleep_ms(1);
  }

  size_t reg_count = ceventloop_reg_count(loop);
  if (queue_created) ccircq_destroy(cq);
  evl_sync_ctx_destroy(&ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_NE(warm, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(warmed);
  REQUIRE_TRUE(warm_removed);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(all_cycles_ok);
  REQUIRE_EQ(reg_count, (size_t)0);
  REQUIRE_TRUE(reclaimed);
  REQUIRE_LE(observed, baseline);
}

// The teardown walk frees a live queue registration and its entry, and must
// still fire on_removed.
TEST(ceventloop, on_removed_fires_on_destroy_for_a_live_queue_registration) {
  ccircq *cq = ccircq_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_slow_cb_ctx read_ctx, write_ctx;
  evl_slow_cb_ctx_init(&read_ctx);
  evl_slow_cb_ctx_init(&write_ctx);

  bool read_registered = false, write_registered = false;
  if (queue_created) {
    ceventloop_construct_scoped(loop, 8, 4, 1);
    ceventloop_handlers handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
    char *err = NULL;
    read_registered =
        (ceventloop_add(loop, ccircq_selectable(cq, cselect_read), handlers,
                        &read_ctx, &err) != CEVENTLOOP_REG_NULL);
    write_registered =
        (ceventloop_add(loop, ccircq_selectable(cq, cselect_write), handlers,
                        &write_ctx, &err) != CEVENTLOOP_REG_NULL);
  }

  pthread_mutex_lock(&read_ctx.mtx);
  bool read_removed_fired = read_ctx.on_removed_called;
  void *read_arg_seen = read_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&read_ctx.mtx);
  pthread_mutex_lock(&write_ctx.mtx);
  bool write_removed_fired = write_ctx.on_removed_called;
  void *write_arg_seen = write_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&write_ctx.mtx);

  if (queue_created) ccircq_destroy(cq);
  evl_slow_cb_ctx_destroy(&read_ctx);
  evl_slow_cb_ctx_destroy(&write_ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_TRUE(read_registered);
  REQUIRE_TRUE(write_registered);
  REQUIRE_TRUE(read_removed_fired);
  REQUIRE_EQ(read_arg_seen, (void *)&read_ctx);
  REQUIRE_TRUE(write_removed_fired);
  REQUIRE_EQ(write_arg_seen, (void *)&write_ctx);
}

// Releasing a slot pushes its index onto the free list, and that push can fail.
// The index is then counted as lost and a later acquire recovers it, so the
// slot array doesn't grow once per failure.
TEST(ceventloop, a_free_list_push_failure_does_not_strand_a_reg_slot) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  const size_t cycles = 8;
  bool all_ok = true;
  size_t before = 0, after = 0;
  bool reusable = false;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);
    ceventloop_handlers handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    char *err = NULL;
    ceventloop_reg warm = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    if (warm == CEVENTLOOP_REG_NULL) all_ok = false;
    if (all_ok && ceventloop_remove(loop, warm) != ccol_success) all_ok = false;

    before = ceventloop__reg_slot_count_for_tests(loop);

    for (size_t i = 0; i < cycles && all_ok; i++) {
      err = NULL;
      ceventloop_reg reg = ceventloop_add(
          loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
      if (reg == CEVENTLOOP_REG_NULL) {
        all_ok = false;
        break;
      }
      ceventloop__force_next_reg_free_index_push_failure_for_tests();
      if (ceventloop_remove(loop, reg) != ccol_success) all_ok = false;
    }

    err = NULL;
    ceventloop_reg revived = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), handlers, &ctx, &err);
    reusable = (revived != CEVENTLOOP_REG_NULL) &&
               (ceventloop_reg_generation(loop, revived) > 0);
    if (reusable && ceventloop_remove(loop, revived) != ccol_success)
      reusable = false;

    after = ceventloop__reg_slot_count_for_tests(loop);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(reusable);
  REQUIRE_LE(after, before + 1);
}

// Event loop slot table
//
// Destroying a loop pushes its index onto the free list, which allocates. A
// failed push must be reclaimed by the acquire path, and the loop from a
// reclaimed slot must work.
TEST(ceventloop_slot_table,
     a_lost_free_index_is_reclaimed_rather_than_growing) {
  {
    ceventloop warm = ceventloop_create(8, 1, 1, NULL);
    if (warm != CEVENTLOOP_NULL) ceventloop_destroy(warm);
  }
  size_t before = ceventloop__slot_table_capacity_for_tests();

  bool all_built = true;
  for (int i = 0; i < 4; i++) {
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop == CEVENTLOOP_NULL) {
      all_built = false;
      continue;
    }
    ceventloop__force_next_free_index_push_failure_for_tests();
    ceventloop_destroy(loop);
  }

  bool works = false;
  {
    ceventloop loop = ceventloop_create(8, 1, 1, NULL);
    if (loop != CEVENTLOOP_NULL) {
      works = (ceventloop_reg_count(loop) == (size_t)0);
      ceventloop_destroy(loop);
    }
  }

  size_t after = ceventloop__slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_built);
  REQUIRE_TRUE(works);
  REQUIRE_LE(after, before + 1);
}

// Queue teardown against a dispatch already in flight
//
// The flags of qdispatch_on_readable(), under mtx.
typedef struct qdispatch_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool callback_entered;
  bool callback_done_with_queue;
} qdispatch_ctx;

// Initializes c with both flags false. The pthread calls are checked.
static void qdispatch_ctx_init(qdispatch_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->callback_entered = false;
  c->callback_done_with_queue = false;
}

// Destroys the lock and condition of c once the loop is joined.
static void qdispatch_ctx_destroy(qdispatch_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

// Takes the queue's one message, sets callback_entered, holds the queue for
// 400 ms, touches it once more and sets callback_done_with_queue. The test
// destroys the queue during the hold, and that last try_recv hits freed memory
// if the destroy completes too early.
static void qdispatch_on_readable(ceventloop loop, ceventloop_reg reg,
                                  cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  qdispatch_ctx *c = (qdispatch_ctx *)arg;

  cmessage msg = {.data = NULL, .size = 0};
  if (ccircq_try_recv(sel->cq, &msg) == ccol_success) free(msg.data);

  pthread_mutex_lock(&c->mtx);
  c->callback_entered = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);

  struct timespec hold = {.tv_sec = 0, .tv_nsec = 400000000L};
  nanosleep(&hold, NULL);

  cmessage again = {.data = NULL, .size = 0};
  if (ccircq_try_recv(sel->cq, &again) == ccol_success) free(again.data);

  pthread_mutex_lock(&c->mtx);
  c->callback_done_with_queue = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// A remove doesn't wait for a callback the reactor has already collected, so
// the queue's destroy must wait for it. The test reads callback_done_with_queue
// right after the destroy; an early return finds it false.
TEST(ceventloop, queue_destroy_waits_for_an_already_collected_dispatch) {
  qdispatch_ctx ctx;
  qdispatch_ctx_init(&ctx);

  ccircq *cq = ccircq_create_mp(4, NULL, NULL);
  bool created = (cq != NULL);
  bool registered = false;
  bool sent = false;
  bool entered = false;
  bool done_when_destroy_returned = false;

  if (created) {
    ceventloop_construct_scoped(loop, 8, 1, 1);

    ceventloop_handlers handlers = {.on_readable = qdispatch_on_readable,
                                    .on_writable = NULL,
                                    .on_error = NULL};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, ccircq_selectable(cq, cselect_read), handlers, &ctx, &err);
    registered = (reg != CEVENTLOOP_REG_NULL);

    if (registered) {
      cmessage m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      sent = (ccircq_send(cq, &m) == ccol_success);
      if (!sent) free(m.data);

      entered = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.callback_entered, 5000);
      ceventloop_remove(loop, reg);
    }

    ccircq_destroy(cq);

    pthread_mutex_lock(&ctx.mtx);
    done_when_destroy_returned = ctx.callback_done_with_queue;
    pthread_mutex_unlock(&ctx.mtx);
  }

  qdispatch_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(done_when_destroy_returned);
}

// A cselect() waiter that gives up its wait must not swallow the wake
//
// One waiter thread: Its queue and timeout, and what its select returned.
typedef struct sel_forward_ctx {
  ccircq *cq;
  int timeout_ms;
  ccol_retval rv;
  size_t ready_index;
} sel_forward_ctx;

// Waits in cselect_timed() on c->cq for c->timeout_ms and stores the result and
// the ready index in c; the two waiters of the forward test run it.
static void *sel_forward_waiter_thread(void *arg) {
  sel_forward_ctx *c = (sel_forward_ctx *)arg;
  cselectable sels[1] = {ccircq_selectable(c->cq, cselect_read)};
  c->ready_index = (size_t)-1;
  c->rv =
      cselect_timed(&c->ready_index, 1, sels, (uint64_t)c->timeout_ms * 1000u);
  return NULL;
}

// Polls pred(arg) every millisecond for up to timeout_ms, so the forward test
// can wait for a state without a fixed sleep. Returns false if pred never
// became true.
static bool sel_forward_poll_until(bool (*pred)(void *), void *arg,
                                   int timeout_ms) {
  for (int waited_ms = 0; waited_ms < timeout_ms; waited_ms++) {
    if (pred(arg)) return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&ts, NULL);
  }
  return pred(arg);
}

// The queue and the waiter count that sel_forward_waiters_reached() waits for.
typedef struct sel_forward_count_arg {
  ccircq *cq;
  size_t target;
} sel_forward_count_arg;

// Tells whether a->cq has at least a->target linked read waiters, a predicate
// for sel_forward_poll_until().
static bool sel_forward_waiters_reached(void *arg) {
  sel_forward_count_arg *a = (sel_forward_count_arg *)arg;
  return ccircq__test_sel_read_waiter_count_for_tests(a->cq) >= a->target;
}

// Tells whether a timed-out waiter has entered the armed deregister delay, a
// predicate for sel_forward_poll_until().
static bool sel_forward_delay_entered(void *arg) {
  (void)arg;
  return cselect__test_timed_out_deregister_delay_count() > 0;
}

// A producer wakes one waiter per message, and that waiter may have just given
// up on its deadline. It must forward the wake, or a sibling stays parked with
// a message in the queue. The waiters start one at a time, so the later one is
// the head and gets the notify, and an armed delay holds its deregistration
// open while the producer sends.
TEST(cselect, a_waiter_that_abandons_its_wait_forwards_the_wake) {
  ccircq *cq = ccircq_create_mp(4, NULL, NULL);
  bool created = (cq != NULL);
  if (!created) {
    REQUIRE_TRUE(created);
    return;
  }

  sel_forward_ctx parked_waiter = {
      .cq = cq, .timeout_ms = 15000, .rv = ccol_success, .ready_index = 0};
  sel_forward_ctx abandoning_waiter = {
      .cq = cq, .timeout_ms = 500, .rv = ccol_success, .ready_index = 0};

  pthread_t parked_tid, abandoning_tid;
  bool parked_started = false;
  bool abandoning_started = false;
  bool parked_linked = false;
  bool both_linked = false;
  bool delay_entered = false;
  bool sent = false;

  sel_forward_count_arg one = {.cq = cq, .target = 1};
  sel_forward_count_arg two = {.cq = cq, .target = 2};

  parked_started = (pthread_create(&parked_tid, NULL, sel_forward_waiter_thread,
                                   &parked_waiter) == 0);
  if (parked_started) {
    parked_linked =
        sel_forward_poll_until(sel_forward_waiters_reached, &one, 10000);
  }

  if (parked_linked) {
    cselect__test_delay_next_timed_out_deregister_us(1000000u);
    abandoning_started =
        (pthread_create(&abandoning_tid, NULL, sel_forward_waiter_thread,
                        &abandoning_waiter) == 0);
  }

  if (abandoning_started) {
    both_linked =
        sel_forward_poll_until(sel_forward_waiters_reached, &two, 10000);
    delay_entered =
        sel_forward_poll_until(sel_forward_delay_entered, NULL, 15000);
  }

  if (delay_entered) {
    cmessage m = {.data = malloc(4), .size = 4};
    assert(m.data != NULL);
    memcpy(m.data, "abc", 4);
    sent = (ccircq_send(cq, &m) == ccol_success);
    if (!sent) free(m.data);
  }

  if (abandoning_started) pthread_join(abandoning_tid, NULL);
  if (parked_started) pthread_join(parked_tid, NULL);
  cselect__test_delay_next_timed_out_deregister_us(0u);

  cmessage drained = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &drained) == ccol_success) {
    free(drained.data);
    drained = (cmessage){.data = NULL, .size = 0};
  }
  ccircq_destroy(cq);

  REQUIRE_TRUE(parked_started);
  REQUIRE_TRUE(abandoning_started);
  REQUIRE_TRUE(parked_linked);
  REQUIRE_TRUE(both_linked);
  REQUIRE_TRUE(delay_entered);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(abandoning_waiter.rv, ccol_timed_out);
  REQUIRE_EQ(parked_waiter.rv, ccol_success);
  REQUIRE_EQ(parked_waiter.ready_index, (size_t)0);
}

// Registrations whose only handler is on_error
//
// An error-only read registration (on_readable NULL, on_error set) asks to hear
// when the fd dies. A peer that shuts down only its write side raises IN and
// RDHUP without HUP, and the loop must deliver that as an error, since nothing
// can read it.
TEST(ceventloop,
     fd_error_only_read_registration_fires_on_peer_shutdown_single_thread) {
  int sv[2];
  int pair_rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  if (pair_rv != 0) {
    REQUIRE_EQ(pair_rv, 0);
    return;
  }

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool registered = false;
  bool fired = false;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);
    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), handlers, &ctx, &err);
    registered = (reg != CEVENTLOOP_REG_NULL);
    if (registered) {
      shutdown(sv[1], SHUT_WR);
      fired = evl_wait_for(&ctx, &ctx.error_count, 1, 3000);
      ceventloop_remove(loop, reg);
    }
  }

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(fired);
}

// The same case against the poller's own copy of the dispatch
// classification.
TEST(ceventloop,
     fd_error_only_read_registration_fires_on_peer_shutdown_multi_thread) {
  int sv[2];
  int pair_rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  if (pair_rv != 0) {
    REQUIRE_EQ(pair_rv, 0);
    return;
  }

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool registered = false;
  bool fired = false;

  {
    ceventloop_construct_scoped(loop, 8, 4, 3);
    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), handlers, &ctx, &err);
    registered = (reg != CEVENTLOOP_REG_NULL);
    if (registered) {
      shutdown(sv[1], SHUT_WR);
      fired = evl_wait_for(&ctx, &ctx.error_count, 1, 3000);
      ceventloop_remove(loop, reg);
    }
  }

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(fired);
}

// A connected socket is writable at once, so arming OUT for a registration with
// no writer would report it on every kernel wait and spin the only reactor
// thread. The test counts completed kernel waits; on_error stays silent too.
TEST(ceventloop, fd_error_only_write_registration_does_not_spin_single_thread) {
  int sv[2];
  int pair_rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  if (pair_rv != 0) {
    REQUIRE_EQ(pair_rv, 0);
    return;
  }

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool registered = false;
  uint64_t iterations = 0;
  int errors_seen = 0;

  {
    ceventloop_construct_scoped(loop, 8, 1, 1);
    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), handlers, &ctx, &err);
    registered = (reg != CEVENTLOOP_REG_NULL);
    if (registered) {
      usleep(50000);
      uint64_t before = ceventloop__poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = ceventloop__poller_iterations_for_tests(loop) - before;
      ceventloop_remove(loop, reg);
    }
  }

  pthread_mutex_lock(&ctx.mtx);
  errors_seen = ctx.error_count;
  pthread_mutex_unlock(&ctx.mtx);
  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_LT(iterations, (uint64_t)20);
  REQUIRE_EQ(errors_seen, 0);
}

// The same with a dispatch pool, where the spin is a cycle of collect, submit,
// skip and re-arm.
TEST(ceventloop, fd_error_only_write_registration_does_not_spin_multi_thread) {
  int sv[2];
  int pair_rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  if (pair_rv != 0) {
    REQUIRE_EQ(pair_rv, 0);
    return;
  }

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool registered = false;
  uint64_t iterations = 0;
  int errors_seen = 0;

  {
    ceventloop_construct_scoped(loop, 8, 4, 3);
    ceventloop_handlers handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), handlers, &ctx, &err);
    registered = (reg != CEVENTLOOP_REG_NULL);
    if (registered) {
      usleep(50000);
      uint64_t before = ceventloop__poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = ceventloop__poller_iterations_for_tests(loop) - before;
      ceventloop_remove(loop, reg);
    }
  }

  pthread_mutex_lock(&ctx.mtx);
  errors_seen = ctx.error_count;
  pthread_mutex_unlock(&ctx.mtx);
  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_LT(iterations, (uint64_t)20);
  REQUIRE_EQ(errors_seen, 0);
}

// Timed waits: The deadline and the condition variable share one clock
//
// One timed call on its own thread. It stores its result and elapsed time under
// mtx and sets returned.
typedef struct timed_clock_ctx {
  ccircq *cq;
  cdynmq *dq;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool returned;
  ccol_retval rv;
  long long elapsed_us;
} timed_clock_ctx;

// Initializes c with no queue and no result. The pthread calls are checked.
static void timed_clock_ctx_init(timed_clock_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->cq = NULL;
  c->dq = NULL;
  c->returned = false;
  c->rv = ccol_success;
  c->elapsed_us = 0;
}

// Destroys the lock and condition of c once its thread is joined.
static void timed_clock_ctx_destroy(timed_clock_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

// Stores rv and the time since *started in c under c->mtx, sets c->returned and
// wakes the test, which waits for it with a bound.
static void timed_clock_ctx_finish(timed_clock_ctx *c, ccol_retval rv,
                                   const struct timespec *started) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  pthread_mutex_lock(&c->mtx);
  c->rv = rv;
  c->elapsed_us = ((long long)(now.tv_sec - started->tv_sec)) * 1000000LL +
                  ((long long)(now.tv_nsec - started->tv_nsec)) / 1000LL;
  c->returned = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

// The threads of the clock tests: Each makes one timed call with a 100 ms
// timeout on c->cq or c->dq and reports it through timed_clock_ctx_finish(), so
// the test can see a call that returns too soon or never.
static void *timed_clock_circq_recv_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  cmessage m = {.data = NULL, .size = 0};
  ccol_retval rv = ccircq_timed_recv(c->cq, &m, timeout);
  if (rv == ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

static void *timed_clock_circq_send_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  cmessage m = {.data = malloc(4), .size = 4};
  assert(m.data != NULL);
  memcpy(m.data, "abc", 4);
  ccol_retval rv = ccircq_timed_send(c->cq, &m, timeout);
  if (rv != ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

static void *timed_clock_dynmq_recv_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  cmessage m = {.data = NULL, .size = 0};
  ccol_retval rv = cdynmq_timed_recv(c->dq, &m, timeout);
  if (rv == ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

// The deadline and the condition variable must use the same clock. The two
// clocks are decades apart, so a mismatch either returns at once or blocks
// practically for ever. The call runs on its own thread with a bounded wait,
// and a send releases a thread that is still blocked so it can be joined.
TEST(circular_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccircq_create_mp(2, NULL, NULL);
  bool created = (ctx.cq != NULL);
  bool started = false;
  bool returned = false;
  ccol_retval rv = ccol_success;
  long long elapsed_us = 0;

  pthread_t tid;
  if (created) {
    started =
        (pthread_create(&tid, NULL, timed_clock_circq_recv_thread, &ctx) == 0);
  }
  if (started) {
    returned = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.returned, 5000);
    if (!returned) {
      cmessage m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      if (ccircq_send(ctx.cq, &m) != ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    cmessage drained = {.data = NULL, .size = 0};
    while (ccircq_try_recv(ctx.cq, &drained) == ccol_success) {
      free(drained.data);
      drained = (cmessage){.data = NULL, .size = 0};
    }
    ccircq_destroy(ctx.cq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

// The send-direction counterpart.
TEST(circular_queues, timed_send_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccircq_create_mp(1, NULL, NULL);
  bool created = (ctx.cq != NULL);
  bool filled = false;
  bool started = false;
  bool returned = false;
  ccol_retval rv = ccol_success;
  long long elapsed_us = 0;

  if (created) {
    cmessage fill = {.data = malloc(4), .size = 4};
    assert(fill.data != NULL);
    memcpy(fill.data, "abc", 4);
    filled = (ccircq_send(ctx.cq, &fill) == ccol_success);
    if (!filled) free(fill.data);
  }

  pthread_t tid;
  if (filled) {
    started =
        (pthread_create(&tid, NULL, timed_clock_circq_send_thread, &ctx) == 0);
  }
  if (started) {
    returned = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.returned, 5000);
    if (!returned) {
      cmessage m = {.data = NULL, .size = 0};
      if (ccircq_try_recv(ctx.cq, &m) == ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    cmessage drained = {.data = NULL, .size = 0};
    while (ccircq_try_recv(ctx.cq, &drained) == ccol_success) {
      free(drained.data);
      drained = (cmessage){.data = NULL, .size = 0};
    }
    ccircq_destroy(ctx.cq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(filled);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

TEST(dynamic_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.dq = cdynmq_create_mp(NULL, NULL);
  bool created = (ctx.dq != NULL);
  bool started = false;
  bool returned = false;
  ccol_retval rv = ccol_success;
  long long elapsed_us = 0;

  pthread_t tid;
  if (created) {
    started =
        (pthread_create(&tid, NULL, timed_clock_dynmq_recv_thread, &ctx) == 0);
  }
  if (started) {
    returned = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.returned, 5000);
    if (!returned) {
      cmessage m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      if (cdynmq_send(ctx.dq, &m) != ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    cmessage drained = {.data = NULL, .size = 0};
    while (cdynmq_try_recv(ctx.dq, &drained) == ccol_success) {
      free(drained.data);
      drained = (cmessage){.data = NULL, .size = 0};
    }
    cdynmq_destroy(ctx.dq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

// A callback that tears down another watched queue, on the dispatch pool
//
// The kinds of selectable the victim of the teardown can be.
enum { XQD_CIRCQ = 0, XQD_DYNQ = 1, XQD_CHAN = 2 };

// One worker's part of the scenario: A trigger queue whose callback tears down
// the victim selectable, and pointers to the counters and the gate the whole
// scenario shares.
typedef struct xqd_slot {
  ceventloop loop;
  int kind;
  ccircq *trigger;
  ccircq *victim_cq;
  cdynmq *victim_dq;
  cchan *victim_ch;
  cselectable victim_sel;
  ceventloop_reg victim_reg;
  bool victim_destroyed;
  _Atomic int *entered;
  _Atomic int *done;
  _Atomic bool *gate;
  _Atomic bool *gate_timed_out;
} xqd_slot;

// Sleeps 1 ms between the polls of the cross-queue scenario. Polling with it
// keeps a waiter under valgrind from burning the quanta the awaited thread
// needs.
static void xqd_nap(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  nanosleep(&ts, NULL);
}

// Frees every message left in the victim's queue (for a channel, the circular
// queue its selectable resolved to), so its destroy doesn't assert.
static void xqd_drain_victim(xqd_slot *s) {
  cmessage m = {.data = NULL, .size = 0};
  if (s->kind == XQD_DYNQ) {
    while (cdynmq_try_recv(s->victim_dq, &m) == ccol_success) {
      free(m.data);
      m = (cmessage){.data = NULL, .size = 0};
    }
    return;
  }
  while (ccircq_try_recv(s->victim_sel.cq, &m) == ccol_success) {
    free(m.data);
    m = (cmessage){.data = NULL, .size = 0};
  }
}

// Destroys the victim queue or channel of s, the last step of its teardown.
static void xqd_destroy_victim(xqd_slot *s) {
  if (s->kind == XQD_CIRCQ) {
    ccircq_destroy(s->victim_cq);
  } else if (s->kind == XQD_DYNQ) {
    cdynmq_destroy(s->victim_dq);
  } else {
    cchan_destroy(s->victim_ch);
  }
}

// Does nothing; the victim's dispatch only has to be queued behind the workers
// that tear it down.
static void xqd_victim_on_readable(ceventloop loop, ceventloop_reg reg,
                                   cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

// The callback that tears another queue down: Takes the trigger message, counts
// itself in s->entered and parks until s->gate opens (at most 20 s, which sets
// s->gate_timed_out). Then it runs the documented teardown of its victim
// (remove, drain, destroy) and counts itself in s->done.
static void xqd_trigger_on_readable(ceventloop loop, ceventloop_reg reg,
                                    cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  xqd_slot *s = (xqd_slot *)arg;
  cmessage m = {.data = NULL, .size = 0};
  if (ccircq_try_recv(sel->cq, &m) == ccol_success) free(m.data);

  atomic_fetch_add(s->entered, 1);
  for (int waited_ms = 0; !atomic_load(s->gate); waited_ms++) {
    if (waited_ms >= 20000) {
      atomic_store(s->gate_timed_out, true);
      break;
    }
    xqd_nap();
  }

  ceventloop_remove(s->loop, s->victim_reg);
  xqd_drain_victim(s);
  xqd_destroy_victim(s);
  s->victim_destroyed = true;
  atomic_fetch_add(s->done, 1);
}

// Sends one malloc'd message to cq, or to dq when cq is NULL, to make a trigger
// or a victim ready. Returns true on success.
static bool xqd_send_one(ccircq *cq, cdynmq *dq) {
  cmessage m = {.data = malloc(4), .size = 4};
  if (!m.data) return false;
  memcpy(m.data, "abc", 4);
  ccol_retval rv = cq ? ccircq_send(cq, &m) : cdynmq_send(dq, &m);
  if (rv != ccol_success) free(m.data);
  return rv == ccol_success;
}

// The most dispatch workers a scenario uses.
enum { XQD_MAX_WORKERS = 8 };

// One run: The loop, its slots and the counters and gate they share.
typedef struct xqd_scenario {
  ceventloop loop;
  size_t workers;
  size_t built;
  xqd_slot slots[XQD_MAX_WORKERS];
  _Atomic int entered;
  _Atomic int done;
  _Atomic bool gate;
  _Atomic bool gate_timed_out;
} xqd_scenario;

// Waits up to 20 s until *counter reaches target, the bounded wait of the
// cross-queue scenario. Returns false on a timeout.
static bool xqd_wait_count(_Atomic int *counter, int target) {
  for (int waited_ms = 0; atomic_load(counter) < target; waited_ms++) {
    if (waited_ms >= 20000) return false;
    xqd_nap();
  }
  return true;
}

// Tears a scenario down on every path: Opens the gate, waits for every running
// trigger callback to finish its teardown and destroys what is left. When the
// callbacks never finish the workers are deadlocked and destroying the loop
// would hang, so it leaves the loop behind on purpose and returns false.
static bool xqd_teardown(xqd_scenario *sc) {
  atomic_store(&sc->gate, true);
  if (!xqd_wait_count(&sc->done, atomic_load(&sc->entered))) return false;
  if (sc->loop != CEVENTLOOP_NULL) ceventloop_destroy(sc->loop);
  for (size_t i = 0; i < sc->built; i++) {
    xqd_slot *s = &sc->slots[i];
    cmessage m = {.data = NULL, .size = 0};
    while (ccircq_try_recv(s->trigger, &m) == ccol_success) {
      free(m.data);
      m = (cmessage){.data = NULL, .size = 0};
    }
    ccircq_destroy(s->trigger);
    if (!s->victim_destroyed) {
      xqd_drain_victim(s);
      xqd_destroy_victim(s);
    }
  }
  return true;
}

// Builds the trigger and victim queues of slot i of sc, of the given kind.
// Returns false, with nothing left behind, when an allocation fails.
static bool xqd_build_slot(xqd_scenario *sc, size_t i, int kind) {
  xqd_slot *s = &sc->slots[i];
  s->loop = sc->loop;
  s->kind = kind;
  s->entered = &sc->entered;
  s->done = &sc->done;
  s->gate = &sc->gate;
  s->gate_timed_out = &sc->gate_timed_out;
  s->victim_destroyed = false;
  s->trigger = ccircq_create_mp(4, NULL, NULL);
  if (!s->trigger) return false;
  if (kind == XQD_CIRCQ) {
    s->victim_cq = ccircq_create_mp(4, NULL, NULL);
    if (s->victim_cq)
      s->victim_sel = ccircq_selectable(s->victim_cq, cselect_read);
  } else if (kind == XQD_DYNQ) {
    s->victim_dq = cdynmq_create_mp(NULL, NULL);
    if (s->victim_dq)
      s->victim_sel = cdynmq_selectable(s->victim_dq, cselect_read);
  } else {
    s->victim_ch = cchan_create_mp(4, NULL, NULL);
    if (s->victim_ch)
      s->victim_sel = cchan_selectable(s->victim_ch, cselect_read);
  }
  if (!s->victim_cq && !s->victim_dq && !s->victim_ch) {
    ccircq_destroy(s->trigger);
    return false;
  }
  return true;
}

// Runs one cross-queue scenario with num_reactor_threads threads and a victim
// of the given kind: Keeps every dispatch worker busy in a trigger callback,
// queues one victim dispatch behind them, then opens the gate. Returns 0 on
// success, or the number of the step that failed; 18 is the deadlock under
// test.
static int xqd_run_scenario(size_t num_reactor_threads, int kind) {
  xqd_scenario *sc = calloc(1, sizeof(*sc));
  if (!sc) return 10;
  sc->workers = num_reactor_threads - 1;
  if (sc->workers == 0 || sc->workers > XQD_MAX_WORKERS) {
    free(sc);
    return 10;
  }
  sc->loop = ceventloop_create(8, 1, num_reactor_threads, NULL);
  if (sc->loop == CEVENTLOOP_NULL) {
    free(sc);
    return 11;
  }

  int rc = 0;
  ceventloop_handlers trig_h = {.on_readable = xqd_trigger_on_readable};
  ceventloop_handlers vict_h = {.on_readable = xqd_victim_on_readable};
  for (size_t i = 0; i < sc->workers && rc == 0; i++) {
    if (!xqd_build_slot(sc, i, kind)) {
      rc = 12;
      break;
    }
    sc->built++;
    xqd_slot *s = &sc->slots[i];
    s->victim_reg = ceventloop_add(sc->loop, s->victim_sel, vict_h, s, NULL);
    ceventloop_reg tr = ceventloop_add(
        sc->loop, ccircq_selectable(s->trigger, cselect_read), trig_h, s, NULL);
    if (s->victim_reg == CEVENTLOOP_REG_NULL || tr == CEVENTLOOP_REG_NULL)
      rc = 13;
  }

  for (size_t i = 0; i < sc->workers && rc == 0; i++)
    if (!xqd_send_one(sc->slots[i].trigger, NULL)) rc = 14;
  if (rc == 0 && !xqd_wait_count(&sc->entered, (int)sc->workers)) rc = 15;

  for (size_t i = 0; i < sc->workers && rc == 0; i++) {
    xqd_slot *s = &sc->slots[i];
    bool ok = (kind == XQD_DYNQ) ? xqd_send_one(NULL, s->victim_dq)
                                 : xqd_send_one(s->victim_sel.cq, NULL);
    if (!ok) rc = 16;
  }
  for (int waited_ms = 0;
       rc == 0 && ceventloop__dispatch_pool_pending_count_for_tests(sc->loop) <
                      sc->workers;
       waited_ms++) {
    if (waited_ms >= 20000) rc = 17;
    xqd_nap();
  }

  if (rc == 0) {
    atomic_store(&sc->gate, true);
    if (!xqd_wait_count(&sc->done, (int)sc->workers)) rc = 18;
  }
  if (rc == 0 && atomic_load(&sc->gate_timed_out)) rc = 19;

  if (!xqd_teardown(sc)) return rc != 0 ? rc : 20;
  free(sc);
  return rc;
}

// A callback may tear down a different watched queue. A victim dispatch that
// waits in the pool behind the worker running the destroy must hold nothing the
// destroy waits for. A reference claimed when the dispatch is collected instead
// of when it runs blocks every worker (code 18).
TEST(ceventloop, cross_queue_teardown_from_a_callback_on_the_pool) {
  static const size_t thread_counts[] = {2, 3, 4};
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  int results[3][3];
  for (size_t t = 0; t < 3; t++)
    for (size_t k = 0; k < 3; k++)
      results[t][k] = xqd_run_scenario(thread_counts[t], kinds[k]);
  for (size_t t = 0; t < 3; t++)
    for (size_t k = 0; k < 3; k++) REQUIRE_EQ(results[t][k], 0);
}

// Queue wakes: One eventfd write covers every send until the next dispatch
//
// The queue of one wake scenario (kind is an XQD_* value, sel the read
// selectable the listener watches) and the counters its callbacks and waiters
// update. The first callback parks until gate is set; abort_run stops a sender
// that waits on a full queue.
typedef struct nwk_ctx {
  int kind;
  ccircq *cq;
  cdynmq *dq;
  cchan *ch;
  cselectable sel;
  _Atomic long received;
  _Atomic long callbacks;
  _Atomic bool first_entered;
  _Atomic bool gate;
  _Atomic bool abort_run;
} nwk_ctx;

// Takes one message from the scenario's queue, so the listeners and waiters of
// these tests can drain it. A channel listener receives on the circular queue
// its selectable resolved to. Returns true when it got a message.
static bool nwk_try_recv(nwk_ctx *c) {
  cmessage m = {.data = NULL, .size = 0};
  if (c->kind == XQD_DYNQ) return cdynmq_try_recv(c->dq, &m) == ccol_success;
  return ccircq_try_recv(c->sel.cq, &m) == ccol_success;
}

// Sends one empty message to the queue the listener watches, the producer step
// of every wake scenario. A full circular queue is retried every millisecond
// until c->abort_run is set. Returns true on success.
static bool nwk_send(nwk_ctx *c) {
  for (;;) {
    cmessage m = {.data = NULL, .size = 0};
    ccol_retval rv = (c->kind == XQD_DYNQ) ? cdynmq_send(c->dq, &m)
                                           : ccircq_try_send(c->sel.cq, &m);
    if (rv == ccol_success) return true;
    if (rv != ccol_container_full || atomic_load(&c->abort_run)) return false;
    xqd_nap();
  }
}

// The listener callback of the wake tests: Counts the call in c->callbacks and
// drains the queue into c->received. The first call sets c->first_entered and
// parks (at most 20 s) until c->gate opens, so a burst can be sent while a
// dispatch is running.
static void nwk_on_readable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  nwk_ctx *c = (nwk_ctx *)arg;
  if (atomic_fetch_add(&c->callbacks, 1) == 0) {
    atomic_store(&c->first_entered, true);
    for (int waited_ms = 0; !atomic_load(&c->gate) && waited_ms < 20000;
         waited_ms++)
      xqd_nap();
  }
  while (nwk_try_recv(c)) atomic_fetch_add(&c->received, 1);
}

// Clears c and creates the queue of the given kind with its read selectable;
// every wake scenario starts with it. Returns false when the create fails.
static bool nwk_setup(nwk_ctx *c, int kind) {
  memset(c, 0, sizeof(*c));
  c->kind = kind;
  if (kind == XQD_CIRCQ) {
    c->cq = ccircq_create_mp(64, NULL, NULL);
    if (!c->cq) return false;
    c->sel = ccircq_selectable(c->cq, cselect_read);
  } else if (kind == XQD_DYNQ) {
    c->dq = cdynmq_create_mp(NULL, NULL);
    if (!c->dq) return false;
    c->sel = cdynmq_selectable(c->dq, cselect_read);
  } else {
    c->ch = cchan_create_mp(64, NULL, NULL);
    if (!c->ch) return false;
    c->sel = cchan_selectable(c->ch, cselect_read);
  }
  return true;
}

// Drains and destroys the scenario's queue, so the scenarios end without the
// assert on a queue that still holds messages.
static void nwk_teardown(nwk_ctx *c) {
  while (nwk_try_recv(c)) {
  }
  if (c->kind == XQD_CIRCQ) {
    ccircq_destroy(c->cq);
  } else if (c->kind == XQD_DYNQ) {
    cdynmq_destroy(c->dq);
  } else {
    cchan_destroy(c->ch);
  }
}

// Waits up to timeout_ms until c->received reaches target, the bounded wait the
// listener scenarios end with. Returns false on a timeout.
static bool nwk_wait_received(nwk_ctx *c, long target, int timeout_ms) {
  for (int waited_ms = 0; atomic_load(&c->received) < target; waited_ms++) {
    if (waited_ms >= timeout_ms) return false;
    xqd_nap();
  }
  return true;
}

// Sends a burst while the queue's only listener runs its parked first callback
// and returns how many eventfd wakes the burst cost, for the test below. The
// dispatch consumed the first wake before the callback ran, so only the first
// send of the burst should write the eventfd. *delivered_out tells whether
// every message reached the listener.
static long nwk_burst_wakes(size_t num_reactor_threads, int kind, long burst,
                            bool *delivered_out) {
  *delivered_out = false;
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return -1;
  ceventloop loop = ceventloop_create(8, 1, num_reactor_threads, NULL);
  if (loop == CEVENTLOOP_NULL) {
    nwk_teardown(&c);
    return -1;
  }
  ceventloop_handlers h = {.on_readable = nwk_on_readable};
  ceventloop_reg reg = ceventloop_add(loop, c.sel, h, &c, NULL);
  long wakes = -1;
  if (reg != CEVENTLOOP_REG_NULL) {
    bool entered = nwk_send(&c);
    for (int waited_ms = 0; entered && !atomic_load(&c.first_entered);
         waited_ms++) {
      if (waited_ms >= 20000) entered = false;
      xqd_nap();
    }
    if (entered) {
      uint64_t before = cselect__test_eventfd_wake_count();
      bool sent_all = true;
      for (long i = 0; i < burst && sent_all; i++) {
        sent_all = nwk_send(&c);
      }
      uint64_t after = cselect__test_eventfd_wake_count();
      if (sent_all) wakes = (long)(after - before);
    }
    atomic_store(&c.gate, true);
    *delivered_out = nwk_wait_received(&c, burst + 1, 20000);
    atomic_store(&c.abort_run, true);
    ceventloop_remove(loop, reg);
  }
  atomic_store(&c.gate, true);
  ceventloop_destroy(loop);
  nwk_teardown(&c);
  return wakes;
}

// A send to a listener that already has a wake pending costs no eventfd write.
// A send that always wrote it would make the burst of 48 cost 48 wakes.
TEST(ceventloop, a_pending_queue_wake_absorbs_later_sends) {
  static const size_t thread_counts[] = {1, 3};
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  long wakes[2][3];
  bool delivered[2][3];
  for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 3; k++)
      wakes[t][k] =
          nwk_burst_wakes(thread_counts[t], kinds[k], 48, &delivered[t][k]);
  for (size_t t = 0; t < 2; t++) {
    for (size_t k = 0; k < 3; k++) {
      REQUIRE_EQ(wakes[t][k], 1L);
      REQUIRE_TRUE(delivered[t][k]);
    }
  }
}

// The scenario a producer thread sends to, how many messages, and whether every
// send went through.
typedef struct nwk_producer_arg {
  nwk_ctx *c;
  long count;
  bool ok;
} nwk_producer_arg;

// Sends a->count messages, yielding every 64 sends so the consumer drains the
// queue to empty and many sends land while a dispatch clears the wake. Used by
// the lost-wake stress tests; a->ok reports whether every send went through.
static void *nwk_producer_thread(void *arg) {
  nwk_producer_arg *a = (nwk_producer_arg *)arg;
  a->ok = true;
  for (long i = 0; i < a->count && a->ok; i++) {
    a->ok = nwk_send(a->c);
    if ((i & 63) == 0) sched_yield();
  }
  return NULL;
}

// Checks that a loop listener that drains the queue on every callback gets
// every message while a producer sends as the callbacks run. A lost wake leaves
// messages behind with no dispatch coming, and the bounded wait for the full
// count fails. Returns true on success.
static bool nwk_stress_listener(size_t num_reactor_threads, int kind,
                                long count) {
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return false;
  atomic_store(&c.gate, true);
  bool ok = false;
  ceventloop loop = ceventloop_create(8, 1, num_reactor_threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_readable = nwk_on_readable};
    ceventloop_reg reg = ceventloop_add(loop, c.sel, h, &c, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      nwk_producer_arg pa = {.c = &c, .count = count, .ok = false};
      pthread_t tid;
      if (pthread_create(&tid, NULL, nwk_producer_thread, &pa) == 0) {
        ok = nwk_wait_received(&c, count, 60000);
        atomic_store(&c.abort_run, true);
        pthread_join(tid, NULL);
        ok = ok && pa.ok;
      }
      ceventloop_remove(loop, reg);
    }
    ceventloop_destroy(loop);
  }
  nwk_teardown(&c);
  return ok;
}

TEST(ceventloop, no_queue_wake_is_lost_while_a_dispatch_clears_it) {
  static const size_t thread_counts[] = {1, 3};
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 3; k++)
      ok[t][k] = nwk_stress_listener(thread_counts[t], kinds[k], 20000);
  for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[t][k]);
}

// One cselect() consumer of the stress test: Its target count, the idle fd that
// selects epoll mode (-1 for the condvar mode) and whether it timed out with
// messages still owed.
typedef struct nwk_select_arg {
  nwk_ctx *c;
  long target;
  int idle_fd;
  bool timed_out;
} nwk_select_arg;

// Waits in cselect_timed() and drains the queue until the scenario has received
// a->target messages, so the select stress test can look for lost wakes. Only a
// timeout with messages still owed sets a->timed_out; a waiter whose sibling
// took the last message may time out.
static void *nwk_select_consumer_thread(void *arg) {
  nwk_select_arg *a = (nwk_select_arg *)arg;
  cselectable sels[2];
  size_t n = 0;
  sels[n++] = a->c->sel;
  if (a->idle_fd >= 0) sels[n++] = cselectable_fd(a->idle_fd, cselect_read);
  while (atomic_load(&a->c->received) < a->target &&
         !atomic_load(&a->c->abort_run)) {
    size_t idx = 0;
    ccol_retval rv = cselect_timed(&idx, n, sels, 10000000);
    if (rv == ccol_timed_out) {
      a->timed_out = atomic_load(&a->c->received) < a->target;
      return NULL;
    }
    if (rv != ccol_success) return NULL;
    while (nwk_try_recv(a->c)) atomic_fetch_add(&a->c->received, 1);
  }
  return NULL;
}

// The lost-wake check for cselect() waiters, in condvar and in epoll mode. Two
// waiters compete for every message, so a woken waiter often finds the queue
// empty and waits again inside the same call. Returns true when every message
// arrived and no waiter timed out with work owed.
static bool nwk_stress_select(int kind, bool epoll_mode, long count) {
  enum { CONSUMERS = 2 };
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return false;
  int pfd[2] = {-1, -1};
  if (epoll_mode && pipe(pfd) != 0) {
    nwk_teardown(&c);
    return false;
  }
  nwk_select_arg sa[CONSUMERS];
  pthread_t ctids[CONSUMERS];
  bool c_started[CONSUMERS];
  for (int i = 0; i < CONSUMERS; i++) {
    sa[i] = (nwk_select_arg){
        .c = &c, .target = count, .idle_fd = pfd[0], .timed_out = false};
    c_started[i] = (pthread_create(&ctids[i], NULL, nwk_select_consumer_thread,
                                   &sa[i]) == 0);
  }
  nwk_producer_arg pa = {.c = &c, .count = count, .ok = false};
  pthread_t ptid;
  bool p_started = c_started[0] && c_started[1] &&
                   (pthread_create(&ptid, NULL, nwk_producer_thread, &pa) == 0);
  bool ok = p_started && nwk_wait_received(&c, count, 60000);
  atomic_store(&c.abort_run, true);
  if (p_started) pthread_join(ptid, NULL);
  for (int i = 0; i < CONSUMERS; i++) {
    cmessage wake = {.data = NULL, .size = 0};
    if (kind == XQD_DYNQ)
      (void)cdynmq_send(c.dq, &wake);
    else
      (void)ccircq_try_send(c.sel.cq, &wake);
  }
  for (int i = 0; i < CONSUMERS; i++) {
    if (c_started[i]) pthread_join(ctids[i], NULL);
    ok = ok && !sa[i].timed_out;
  }
  ok = ok && pa.ok;
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  nwk_teardown(&c);
  return ok;
}

TEST(cselect, no_queue_wake_is_lost_while_the_waiter_clears_it) {
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++)
      ok[m][k] = nwk_stress_select(kinds[k], m == 1, 20000);
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[m][k]);
}

// The single waiter of the empty-queue test, its mode and what it saw.
typedef struct nwk_single_arg {
  nwk_ctx *c;
  bool epoll_mode;
  int idle_fd;
  _Atomic int rounds_done;
  _Atomic bool stop;
  bool timed_out_with_work;
  bool failed;
} nwk_single_arg;

// Calls cselect_timed() round after round and takes every message it finds,
// counting rounds in a->rounds_done, until a->stop. It is the waiter of
// nwk_single_waiter_rounds(). A message that sat in the queue for a whole
// timeout is a slept-through wake and sets a->timed_out_with_work.
static void *nwk_single_waiter_thread(void *arg) {
  nwk_single_arg *a = (nwk_single_arg *)arg;
  cselectable sels[2];
  size_t n = 0;
  sels[n++] = a->c->sel;
  if (a->epoll_mode) sels[n++] = cselectable_fd(a->idle_fd, cselect_read);
  while (!atomic_load(&a->stop)) {
    size_t idx = 0;
    ccol_retval rv = cselect_timed(&idx, n, sels, 5000000);
    if (rv == ccol_timed_out) {
      if (nwk_try_recv(a->c)) {
        a->timed_out_with_work = true;
        return NULL;
      }
      continue;
    }
    if (rv != ccol_success) {
      a->failed = true;
      return NULL;
    }
    while (nwk_try_recv(a->c)) atomic_fetch_add(&a->c->received, 1);
    atomic_fetch_add(&a->rounds_done, 1);
  }
  return NULL;
}

// Tells whether the single waiter is linked into the queue's read-waiter list,
// so the producer knows when to send. A dynamic queue has no count hook, so it
// says yes and a pause covers it.
static bool nwk_waiter_linked(nwk_ctx *c) {
  if (c->kind == XQD_DYNQ) return true;
  return ccircq__test_sel_read_waiter_count_for_tests(c->sel.cq) >= 1;
}

// Runs the empty-queue rounds: The producer wakes the waiter and takes the
// message back first, so the waiter finds the queue empty and waits again in
// the same call, and the next message must wake it. In epoll mode only the wait
// loop clears the recorded wake. Returns true when no round failed.
static bool nwk_single_waiter_rounds(int kind, bool epoll_mode, int rounds) {
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return false;
  int pfd[2] = {-1, -1};
  if (epoll_mode && pipe(pfd) != 0) {
    nwk_teardown(&c);
    return false;
  }
  nwk_single_arg a = {.c = &c, .epoll_mode = epoll_mode, .idle_fd = pfd[0]};
  pthread_t tid;
  bool started =
      (pthread_create(&tid, NULL, nwk_single_waiter_thread, &a) == 0);
  bool ok = started;
  for (int r = 0; r < rounds && ok; r++) {
    for (int waited_ms = 0; !nwk_waiter_linked(&c); waited_ms++) {
      if (waited_ms >= 10000) {
        ok = false;
        break;
      }
      xqd_nap();
    }
    if (!ok) break;
    nwk_send(&c);
    (void)nwk_try_recv(&c);
    xqd_nap();
    int before = atomic_load(&a.rounds_done);
    nwk_send(&c);
    for (int waited_ms = 0; atomic_load(&a.rounds_done) == before;
         waited_ms++) {
      if (waited_ms >= 10000 || a.timed_out_with_work || a.failed) {
        ok = false;
        break;
      }
      xqd_nap();
    }
  }
  atomic_store(&a.stop, true);
  if (started) {
    nwk_send(&c);
    pthread_join(tid, NULL);
  }
  ok = ok && !a.timed_out_with_work && !a.failed;
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  nwk_teardown(&c);
  return ok;
}

TEST(cselect, a_woken_waiter_that_finds_the_queue_empty_waits_again) {
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++)
      ok[m][k] = nwk_single_waiter_rounds(kinds[k], m == 1, 200);
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[m][k]);
}

// Timeout arithmetic: Saturation and negative tv_nsec
//
// Gives the largest time_t, worked out from the width of the type and not from
// any constant of the library, so the saturation tests check the library
// against an independent value.
static time_t tsat_time_max(void) {
  return (time_t)(((uintmax_t)1 << (sizeof(time_t) * CHAR_BIT - 1)) - 1);
}

// UINT64_MAX microseconds added to a start near the largest time_t goes past it
// on every width. A sum that wrapped into a negative tv_sec would count as
// already elapsed in every timed wait.
TEST(ccol_timespec_add_us, saturates_instead_of_wrapping) {
  struct timespec t = {.tv_sec = 1000, .tv_nsec = 500000000L};
  ccol_timespec_add_us(&t, UINT64_MAX);
  REQUIRE_TRUE(t.tv_sec > 1000);
  REQUIRE_GE(t.tv_nsec, 0L);
  REQUIRE_LT(t.tv_nsec, 1000000000L);
  if (sizeof(time_t) == 4) {
    REQUIRE_TRUE(t.tv_sec == tsat_time_max());
    REQUIRE_EQ(t.tv_nsec, 999999999L);
  }

  struct timespec u = {.tv_sec = tsat_time_max() - 1, .tv_nsec = 0};
  ccol_timespec_add_us(&u, 5000000);
  REQUIRE_TRUE(u.tv_sec == tsat_time_max());
  REQUIRE_EQ(u.tv_nsec, 999999999L);

  struct timespec v = {.tv_sec = tsat_time_max() - 5, .tv_nsec = 0};
  ccol_timespec_add_us(&v, 5000000);
  REQUIRE_TRUE(v.tv_sec == tsat_time_max());
  REQUIRE_EQ(v.tv_nsec, 0L);
}

// The queue of tsat_recv_thread() and what its receive returned.
typedef struct tsat_recv_arg {
  ccircq *cq;
  _Atomic bool returned;
  ccol_retval rv;
} tsat_recv_arg;

// Makes one ccircq_timed_recv() with UINT64_MAX as timeout and sets
// a->returned, so the test can check that it waits for the message instead of
// timing out at once.
static void *tsat_recv_thread(void *arg) {
  tsat_recv_arg *a = (tsat_recv_arg *)arg;
  cmessage m = {.data = NULL, .size = 0};
  a->rv = ccircq_timed_recv(a->cq, &m, UINT64_MAX);
  atomic_store(&a->returned, true);
  return NULL;
}

// With a deadline that wraps the receive returns ccol_timed_out before the
// send.
TEST(circular_queues, a_timeout_past_the_end_of_time_waits_for_a_message) {
  ccircq *cq = ccircq_create_mp(4, NULL, NULL);
  REQUIRE_TRUE(cq != NULL);
  tsat_recv_arg a = {.cq = cq, .returned = false, .rv = ccol_success};
  pthread_t tid;
  bool started = (pthread_create(&tid, NULL, tsat_recv_thread, &a) == 0);
  bool early = false;
  if (started) {
    for (int waited_ms = 0; waited_ms < 200 && !early; waited_ms++) {
      early = atomic_load(&a.returned);
      xqd_nap();
    }
    cmessage m = {.data = NULL, .size = 0};
    bool sent = (ccircq_send(cq, &m) == ccol_success);
    (void)sent;
    pthread_join(tid, NULL);
  }
  cmessage drained = {.data = NULL, .size = 0};
  while (ccircq_try_recv(cq, &drained) == ccol_success) {
  }
  ccircq_destroy(cq);
  REQUIRE_TRUE(started);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(a.rv, ccol_success);
}

// The timed calls that the huge-timeout test makes, each with the largest
// timeout or one just below it. UINT64_MAX is the "no limit" of cselect_timed,
// so UINT64_MAX - 1 reaches a select's saturating deadline.
typedef enum {
  HUGE_CIRCQ_SEND,
  HUGE_DYNMQ_RECV,
  HUGE_CHAN_RECV,
  HUGE_CHAN_SEND,
  HUGE_SELECT_QUEUE,
  HUGE_SELECT_QUEUE_NO_LIMIT,
  HUGE_SELECT_FD,
  HUGE_KIND_COUNT
} huge_kind;

// One huge-timeout call: Which call, its objects and its result.
typedef struct huge_arg {
  huge_kind kind;
  ccircq *cq;
  cdynmq *dq;
  cchan *ch;
  int fd;
  _Atomic bool returned;
  ccol_retval rv;
} huge_arg;

// Makes the timed call that a->kind names and sets a->returned. The test runs
// each kind on its own thread and provides the event only after it has seen the
// call waiting, so a deadline that wrapped into the past shows as an early
// ccol_timed_out.
static void *huge_wait_thread(void *arg) {
  huge_arg *a = (huge_arg *)arg;
  cmessage m = {.data = NULL, .size = 0};
  size_t idx = 99;
  switch (a->kind) {
    case HUGE_CIRCQ_SEND:
      a->rv = ccircq_timed_send(a->cq, &m, UINT64_MAX);
      break;
    case HUGE_DYNMQ_RECV:
      a->rv = cdynmq_timed_recv(a->dq, &m, UINT64_MAX);
      break;
    case HUGE_CHAN_RECV:
      a->rv = cchan_timed_recv(a->ch, &m, UINT64_MAX);
      break;
    case HUGE_CHAN_SEND:
      a->rv = cchan_timed_send(a->ch, &m, UINT64_MAX - 1);
      break;
    case HUGE_SELECT_QUEUE:
      a->rv = cselect_timed_va(&idx, UINT64_MAX - 1,
                               ccircq_selectable(a->cq, cselect_read));
      break;
    case HUGE_SELECT_QUEUE_NO_LIMIT:
      a->rv = cselect_timed_va(&idx, UINT64_MAX,
                               ccircq_selectable(a->cq, cselect_read));
      break;
    case HUGE_SELECT_FD:
      a->rv = cselect_timed_va(&idx, UINT64_MAX - 1,
                               cselectable_fd(a->fd, cselect_read));
      break;
    default:
      break;
  }
  atomic_store(&a->returned, true);
  return NULL;
}

// Fills the worker-to-owner direction of the channel from a worker thread, so
// the owner's send in the huge-timeout test has to wait.
static void *huge_fill_chan_thread(void *arg) {
  cmessage m = {.data = NULL, .size = 0};
  cchan_try_send((cchan *)arg, &m);
  return NULL;
}

// Empties the owner-to-worker direction of the channel as a worker would.
static void *huge_drain_chan_thread(void *arg) {
  cmessage m = {.data = NULL, .size = 0};
  while (cchan_try_recv((cchan *)arg, &m) == ccol_success) {
  }
  return NULL;
}

// Runs huge_drain_chan_thread() on a new thread and joins it, because only a
// worker thread reaches the owner-to-worker queue.
static void huge_drain_chan_as_worker(cchan *ch) {
  pthread_t t;
  if (pthread_create(&t, NULL, huge_drain_chan_thread, ch) == 0)
    pthread_join(t, NULL);
}

TEST(timed_calls, a_huge_timeout_waits_for_the_event) {
  bool started[HUGE_KIND_COUNT] = {false};
  bool early[HUGE_KIND_COUNT] = {false};
  ccol_retval rv[HUGE_KIND_COUNT];
  for (int k = 0; k < HUGE_KIND_COUNT; k++) {
    huge_arg a = {.kind = (huge_kind)k,
                  .fd = -1,
                  .returned = false,
                  .rv = ccol_unexpected_failure};
    int pfd[2] = {-1, -1};
    cmessage filler = {.data = NULL, .size = 0};
    if (k == HUGE_CIRCQ_SEND) {
      a.cq = ccircq_create_mp(1, NULL, NULL);
      ccircq_send(a.cq, &filler);
    } else if (k == HUGE_SELECT_QUEUE || k == HUGE_SELECT_QUEUE_NO_LIMIT) {
      a.cq = ccircq_create_mp(1, NULL, NULL);
    } else if (k == HUGE_DYNMQ_RECV) {
      a.dq = cdynmq_create_mp(NULL, NULL);
    } else if (k == HUGE_CHAN_RECV || k == HUGE_CHAN_SEND) {
      a.ch = cchan_create_mp(1, NULL, NULL);
      if (k == HUGE_CHAN_SEND) {
        pthread_t ft;
        if (pthread_create(&ft, NULL, huge_fill_chan_thread, a.ch) == 0)
          pthread_join(ft, NULL);
      }
    } else if (k == HUGE_SELECT_FD) {
      if (pipe(pfd) == 0) a.fd = pfd[0];
    }
    pthread_t tid;
    started[k] = (pthread_create(&tid, NULL, huge_wait_thread, &a) == 0);
    if (started[k]) {
      for (int waited_ms = 0; waited_ms < 200 && !early[k]; waited_ms++) {
        early[k] = atomic_load(&a.returned);
        xqd_nap();
      }
      cmessage m = {.data = NULL, .size = 0};
      switch ((huge_kind)k) {
        case HUGE_CIRCQ_SEND:
        case HUGE_SELECT_QUEUE:
        case HUGE_SELECT_QUEUE_NO_LIMIT:
          if (k == HUGE_CIRCQ_SEND)
            ccircq_recv(a.cq, &m);
          else
            ccircq_send(a.cq, &m);
          break;
        case HUGE_DYNMQ_RECV:
          cdynmq_send(a.dq, &m);
          break;
        case HUGE_CHAN_RECV:
          cchan_send(a.ch, &m);
          break;
        case HUGE_CHAN_SEND:
          cchan_recv(a.ch, &m);
          break;
        case HUGE_SELECT_FD:
          if (pfd[1] >= 0) test_write_retry_eintr(pfd[1], "x", 1);
          break;
        default:
          break;
      }
      pthread_join(tid, NULL);
    }
    rv[k] = a.rv;
    cmessage drained = {.data = NULL, .size = 0};
    if (a.cq) {
      while (ccircq_try_recv(a.cq, &drained) == ccol_success) {
      }
      ccircq_destroy(a.cq);
    }
    if (a.dq) {
      while (cdynmq_try_recv(a.dq, &drained) == ccol_success) {
      }
      cdynmq_destroy(a.dq);
    }
    if (a.ch) {
      while (cchan_try_recv(a.ch, &drained) == ccol_success) {
      }
      huge_drain_chan_as_worker(a.ch);
      cchan_destroy(a.ch);
    }
    if (pfd[0] >= 0) close(pfd[0]);
    if (pfd[1] >= 0) close(pfd[1]);
  }
  for (int k = 0; k < HUGE_KIND_COUNT; k++) {
    REQUIRE_TRUE(started[k]);
    REQUIRE_FALSE(early[k]);
    REQUIRE_EQ(rv[k], ccol_success);
  }
}

// cselect_timed on an fd: A short or zero timeout still takes a look
//
// A wait loop that reports a timeout once the remaining time rounds down to
// 0 ms, without one kernel wait, makes both calls return ccol_timed_out.
TEST(cselect, timed_zero_and_one_ms_report_an_already_readable_fd) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  test_write_retry_eintr(pfd[1], "x", 1);
  ccircq *cq = ccircq_create(4, NULL);

  size_t idx0 = 99, idx1 = 99, idx_mixed = 99;
  ccol_retval r0 =
      cselect_timed_va(&idx0, 0, cselectable_fd(pfd[0], cselect_read));
  ccol_retval r1 =
      cselect_timed_va(&idx1, 1000, cselectable_fd(pfd[0], cselect_read));
  ccol_retval r_mixed =
      cselect_timed_va(&idx_mixed, 0, ccircq_selectable(cq, cselect_read),
                       cselectable_fd(pfd[0], cselect_read));

  ccircq_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(r0, ccol_success);
  REQUIRE_EQ(idx0, (size_t)0);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(idx1, (size_t)0);
  REQUIRE_EQ(r_mixed, ccol_success);
  REQUIRE_EQ(idx_mixed, (size_t)1);
}

// The remaining time is rounded up to a whole millisecond. Truncating it makes
// nearly every round return early.
TEST(cselect, timed_fd_wait_never_ends_before_its_deadline) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int early = 0;
  ccol_retval last = ccol_success;
  for (int round = 0; round < 20; round++) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t idx = 99;
    last = cselect_timed_va(&idx, 2000, cselectable_fd(pfd[0], cselect_read));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long elapsed_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                           (long long)(t1.tv_nsec - t0.tv_nsec);
    if (elapsed_ns < 2000000LL) early++;
    if (last != ccol_timed_out) break;
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_EQ(last, ccol_timed_out);
  REQUIRE_EQ(early, 0);
}

// ceventloop_add: An allocation failure after the entry is built
//
// The state of the refusing allocator below: The loop it watches, whether it is
// armed, how many allocations it refused, how far the poller moved during the
// refusals, and the pipe end it writes to make the added fd readable.
static ceventloop g_afi_loop = CEVENTLOOP_NULL;
static _Atomic bool g_afi_armed = false;
static _Atomic int g_afi_refusals = 0;
static _Atomic uint64_t g_afi_poller_delta = 0;
static int g_afi_signal_fd = -1;

// The allocator of the fd-index test. Only calloc misbehaves: While armed it
// refuses one allocation of 500 bytes or more (the size of a stripe's index
// growth), makes the fd being added readable, and adds to g_afi_poller_delta
// how far the poller moved in the next 50 ms. An entry already published to
// the kernel would wake the poller there.
static void *afi_malloc(size_t n) { return malloc(n); }
static void *afi_realloc(void *p, size_t n) { return realloc(p, n); }
static void afi_free(void *p) { free(p); }
static void *afi_calloc(size_t a, size_t b) {
  if (atomic_load(&g_afi_armed) && a * b >= 500) {
    atomic_store(&g_afi_armed, false);
    uint64_t before = ceventloop__poller_iterations_for_tests(g_afi_loop);
    test_write_retry_eintr(g_afi_signal_fd, "x", 1);
    evl_sleep_ms(50);
    uint64_t after = ceventloop__poller_iterations_for_tests(g_afi_loop);
    atomic_fetch_add(&g_afi_poller_delta, after - before);
    atomic_fetch_add(&g_afi_refusals, 1);
    return NULL;
  }
  return calloc(a, b);
}

// Counts the bytes afi_on_readable() read.
static _Atomic int g_afi_readable = 0;
// Reads one byte and counts it in g_afi_readable, so the test can wait until
// the byte the allocator wrote has been dispatched.
static void afi_on_readable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)arg;
  char c;
  if (read(sel->fd, &c, 1) == 1) atomic_fetch_add(&g_afi_readable, 1);
}

// An add must insert the entry into the fd index before it publishes it to the
// kernel. Otherwise the refused allocation wakes the poller, and on the failing
// add the poller can read the entry after the add freed it.
TEST(ceventloop, an_fd_index_allocation_failure_publishes_nothing) {
  enum { AFI_MAX_FDS = 64 };
  int rfd[AFI_MAX_FDS], wfd[AFI_MAX_FDS];
  ceventloop_reg regs[AFI_MAX_FDS];
  int n_fds = 0;
  int failed_at = -1;
  int expected_readable = 0;
  bool dispatch_timed_out = false;
  char *fail_err = NULL;
  ceventloop_reg retry_reg = CEVENTLOOP_REG_NULL;
  bool dispatched_after_retry = false;

  atomic_store(&g_afi_refusals, 0);
  atomic_store(&g_afi_readable, 0);
  atomic_store(&g_afi_poller_delta, 0);
  ccol_memmgmt_procs mp = {afi_malloc, afi_free, afi_calloc, afi_realloc};
  g_afi_loop = ceventloop_create_mp(8, 1, 1, &mp, NULL);
  bool created = (g_afi_loop != CEVENTLOOP_NULL);
  ceventloop_handlers h = {.on_readable = afi_on_readable};

  while (created && n_fds < AFI_MAX_FDS && failed_at < 0) {
    int p[2];
    if (pipe(p) != 0) break;
    rfd[n_fds] = p[0];
    wfd[n_fds] = p[1];
    g_afi_signal_fd = p[1];
    int refusals_before = atomic_load(&g_afi_refusals);
    atomic_store(&g_afi_armed, true);
    char *err = NULL;
    regs[n_fds] = ceventloop_add(g_afi_loop, cselectable_fd(p[0], cselect_read),
                                 h, NULL, &err);
    atomic_store(&g_afi_armed, false);
    if (regs[n_fds] == CEVENTLOOP_REG_NULL) {
      failed_at = n_fds;
      fail_err = err;
    } else if (atomic_load(&g_afi_refusals) != refusals_before) {
      expected_readable++;
      int waited = 0;
      while (atomic_load(&g_afi_readable) < expected_readable &&
             waited < EVL_WAIT_MS) {
        evl_sleep_ms(5);
        waited += 5;
      }
      if (atomic_load(&g_afi_readable) < expected_readable)
        dispatch_timed_out = true;
    }
    n_fds++;
  }

  if (failed_at >= 0) {
    retry_reg =
        ceventloop_add(g_afi_loop, cselectable_fd(rfd[failed_at], cselect_read),
                       h, NULL, NULL);
    expected_readable++;
    for (int waited = 0; waited < 2000 && !dispatched_after_retry;
         waited += 5) {
      dispatched_after_retry =
          atomic_load(&g_afi_readable) >= expected_readable;
      if (!dispatched_after_retry) evl_sleep_ms(5);
    }
  }

  bool err_names_allocation =
      fail_err != NULL && strstr(fail_err, "allocate") != NULL;
  if (created) {
    for (int i = 0; i < n_fds; i++)
      if (regs[i] != CEVENTLOOP_REG_NULL)
        ceventloop_remove(g_afi_loop, regs[i]);
    if (retry_reg != CEVENTLOOP_REG_NULL)
      ceventloop_remove(g_afi_loop, retry_reg);
    ceventloop_destroy(g_afi_loop);
  }
  for (int i = 0; i < n_fds; i++) {
    close(rfd[i]);
    close(wfd[i]);
  }

  REQUIRE_TRUE(created);
  REQUIRE_GE(atomic_load(&g_afi_refusals), 1);
  REQUIRE_GE(failed_at, 0);
  REQUIRE_FALSE(dispatch_timed_out);
  REQUIRE_EQ(atomic_load(&g_afi_poller_delta), (uint64_t)0);
  REQUIRE_TRUE(err_names_allocation);
  REQUIRE_NE(retry_reg, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(dispatched_after_retry);
}

// ceventloop_add: A direction that is already in use is told apart
TEST(ceventloop, add_reports_a_direction_already_in_use_by_its_err_str) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  ceventloop_handlers h = {.on_error = evl_on_error};
  char *err1 = NULL, *err2 = NULL;
  ceventloop_reg r1 = CEVENTLOOP_REG_NULL, r2 = CEVENTLOOP_REG_NULL;
  if (loop != CEVENTLOOP_NULL) {
    r1 = ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), h, NULL,
                        &err1);
    r2 = ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), h, NULL,
                        &err2);
    if (r1 != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r1);
    if (r2 != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r2);
    ceventloop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  bool in_use = err2 != NULL && strstr(err2, "already in use") != NULL;
  REQUIRE_NE(r1, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(err1 == NULL);
  REQUIRE_EQ(r2, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(in_use);
}

// ceventloop: An event that no handler can take doesn't spin
//
// A registration whose on_error removes it: The error count and the handle it
// removes.
typedef struct {
  _Atomic int error_count;
  ceventloop loop;
  _Atomic ceventloop_reg self_reg;
} unh_ctx;

// Counts an error and removes its own registration once, so the sibling case
// sees the hang-up reach on_error and the loop stops reporting it.
static void unh_on_error_remove_self(ceventloop loop, ceventloop_reg reg,
                                     cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  unh_ctx *c = (unh_ctx *)arg;
  atomic_fetch_add(&c->error_count, 1);
  ceventloop_reg r = atomic_exchange(&c->self_reg, CEVENTLOOP_REG_NULL);
  if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
}

// Counts the kernel waits the poller of loop completes in a 150 ms window,
// after a 50 ms lead-in that lets a runaway loop start. The no-handler tests
// compare it with a small bound.
static uint64_t unh_poller_iterations_in_window(ceventloop loop) {
  evl_sleep_ms(50);
  uint64_t before = ceventloop__poller_iterations_for_tests(loop);
  evl_sleep_ms(150);
  return ceventloop__poller_iterations_for_tests(loop) - before;
}

// Runs a read registration with no handler at all on a socket whose peer
// closes, so the hang-up reaches a registration that can't report it. Gives the
// poller iterations of the window, and those after a resume, which arms the
// registration again until the hang-up mutes it again. *ok is false when the
// setup failed.
static void unh_run_no_handler_case(size_t threads, uint64_t *iterations,
                                    uint64_t *after_resume, bool *ok) {
  *ok = false;
  *iterations = 0;
  *after_resume = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers none = {0};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), none, NULL, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      close(sv[1]);
      sv[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      bool resumed = ceventloop_resume(loop, reg) == ccol_success;
      *after_resume = unh_poller_iterations_in_window(loop);
      *ok = resumed && ceventloop_remove(loop, reg) == ccol_success;
    }
    ceventloop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

// The counters and the handle of unh_on_readable_remove_at_eof().
typedef struct {
  _Atomic int readable_count;
  _Atomic int eof_count;
  _Atomic ceventloop_reg self_reg;
} unh_eof_ctx;

// Reads the fd once and removes its own registration at end of file, which is
// what the hang-up contract asks of a handler that reads.
static void unh_on_readable_remove_at_eof(ceventloop loop, ceventloop_reg reg,
                                          cselectable *sel, void *arg) {
  (void)reg;
  unh_eof_ctx *c = (unh_eof_ctx *)arg;
  atomic_fetch_add(&c->readable_count, 1);
  char buf[64];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  if (n == 0) {
    atomic_fetch_add(&c->eof_count, 1);
    ceventloop_reg r = atomic_exchange(&c->self_reg, CEVENTLOOP_REG_NULL);
    if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
  }
}

// Runs a read registration with on_readable and no on_error on a pipe whose
// writer closes (HUP without IN). The hang-up goes to on_readable, whose read
// returns 0, and the handler removes the registration. Gives the poller
// iterations, the readable and EOF counts, and *ok.
static void unh_run_reader_without_on_error_case(size_t threads,
                                                 uint64_t *iterations,
                                                 int *readable, int *eofs,
                                                 bool *ok) {
  *ok = false;
  *iterations = 0;
  *readable = 0;
  *eofs = 0;
  int pfd[2];
  if (pipe(pfd) != 0) return;
  unh_eof_ctx ctx;
  atomic_init(&ctx.readable_count, 0);
  atomic_init(&ctx.eof_count, 0);
  atomic_init(&ctx.self_reg, CEVENTLOOP_REG_NULL);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_readable = unh_on_readable_remove_at_eof};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), h, &ctx, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      atomic_store(&ctx.self_reg, reg);
      close(pfd[1]);
      pfd[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = true;
    }
    ceventloop_destroy(loop);
  }
  *readable = atomic_load(&ctx.readable_count);
  *eofs = atomic_load(&ctx.eof_count);
  close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
}

// Runs a muted read registration beside a write registration of the same fd,
// whose on_error must still get the hang-up and remove it. Gives the poller
// iterations, the error count and *ok.
static void unh_run_sibling_case(size_t threads, uint64_t *iterations,
                                 int *errors, bool *ok) {
  *ok = false;
  *iterations = 0;
  *errors = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  unh_ctx ctx;
  atomic_init(&ctx.error_count, 0);
  atomic_init(&ctx.self_reg, CEVENTLOOP_REG_NULL);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ctx.loop = loop;
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers none = {0};
    ceventloop_handlers err_only = {.on_error = unh_on_error_remove_self};
    ceventloop_reg rreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), none, NULL, NULL);
    ceventloop_reg wreg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), err_only, &ctx, NULL);
    if (rreg != CEVENTLOOP_REG_NULL && wreg != CEVENTLOOP_REG_NULL) {
      atomic_store(&ctx.self_reg, wreg);
      close(sv[1]);
      sv[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = ceventloop_remove(loop, rreg) == ccol_success;
    }
    ceventloop_destroy(loop);
  }
  *errors = atomic_load(&ctx.error_count);
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

// The results of the three no-handler cases.
typedef struct {
  uint64_t it_none, it_none_resume, it_reader, it_sibling;
  bool ok_none, ok_reader, ok_sibling;
  int readable, eofs, sibling_errors;
} unh_results;

// Runs the three no-handler cases on a loop with `threads` reactor threads and
// stores their results, all before any assertion, so an early return leaves
// nothing running.
static void unh_run_all(size_t threads, unh_results *r) {
  unh_run_no_handler_case(threads, &r->it_none, &r->it_none_resume,
                          &r->ok_none);
  unh_run_reader_without_on_error_case(threads, &r->it_reader, &r->readable,
                                       &r->eofs, &r->ok_reader);
  unh_run_sibling_case(threads, &r->it_sibling, &r->sibling_errors,
                       &r->ok_sibling);
}

// A hang-up that no handler can take must not stay armed, or each window counts
// thousands of poller iterations. The reader case also fails when the hang-up
// doesn't reach on_readable.
TEST(ceventloop, an_event_with_no_handler_does_not_spin_single_thread) {
  unh_results r = {0};
  unh_run_all(1, &r);
  REQUIRE_TRUE(r.ok_none);
  REQUIRE_LT(r.it_none, (uint64_t)20);
  REQUIRE_LT(r.it_none_resume, (uint64_t)20);
  REQUIRE_TRUE(r.ok_reader);
  REQUIRE_LT(r.it_reader, (uint64_t)20);
  REQUIRE_EQ(r.readable, 1);
  REQUIRE_EQ(r.eofs, 1);
  REQUIRE_TRUE(r.ok_sibling);
  REQUIRE_LT(r.it_sibling, (uint64_t)20);
  REQUIRE_EQ(r.sibling_errors, 1);
}

// The dispatch-pool counterpart, where the spin is a cycle of collect and
// re-arm.
TEST(ceventloop, an_event_with_no_handler_does_not_spin_multi_thread) {
  unh_results r = {0};
  unh_run_all(3, &r);
  REQUIRE_TRUE(r.ok_none);
  REQUIRE_LT(r.it_none, (uint64_t)20);
  REQUIRE_LT(r.it_none_resume, (uint64_t)20);
  REQUIRE_TRUE(r.ok_reader);
  REQUIRE_LT(r.it_reader, (uint64_t)20);
  REQUIRE_EQ(r.readable, 1);
  REQUIRE_EQ(r.eofs, 1);
  REQUIRE_TRUE(r.ok_sibling);
  REQUIRE_LT(r.it_sibling, (uint64_t)20);
  REQUIRE_EQ(r.sibling_errors, 1);
}

// ceventloop: An error with no on_error reaches the direction handler
//
// What the UDP reader saw: Its calls, the ECONNREFUSED errors and the
// datagrams.
typedef struct {
  _Atomic int readable_count;
  _Atomic int refused_count;
  _Atomic int data_count;
} derr_udp_ctx;

// Drains a connected UDP socket, counting datagrams and ECONNREFUSED errors. A
// pending ICMP port unreachable comes back as one ECONNREFUSED, and reading it
// clears it.
static void derr_on_udp_readable(ceventloop loop, ceventloop_reg reg,
                                 cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  derr_udp_ctx *c = (derr_udp_ctx *)arg;
  atomic_fetch_add(&c->readable_count, 1);
  for (int i = 0; i < 64; i++) {
    char buf[64];
    ssize_t n = recv(sel->fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n >= 0) {
      atomic_fetch_add(&c->data_count, 1);
      continue;
    }
    if (errno == ECONNREFUSED) {
      atomic_fetch_add(&c->refused_count, 1);
      continue;
    }
    break;
  }
}

// Polls *counter until it reaches want, for at most max_ms, the bounded wait of
// the error tests. Returns false on a timeout.
static bool derr_wait_count(_Atomic int *counter, int want, int max_ms) {
  for (int waited = 0; waited < max_ms; waited += 5) {
    if (atomic_load(counter) >= want) return true;
    evl_sleep_ms(5);
  }
  return atomic_load(counter) >= want;
}

// Makes a non-blocking UDP client bound to a loopback port and fills
// *free_port with another loopback port that had a socket a moment ago and has
// none now, so a datagram there draws an ICMP error. The client is bound first,
// because a client the kernel binds at connect() can get the freed port and
// talk to itself. Returns the client fd, or -1.
static int derr_udp_client_and_free_port(struct sockaddr_in *free_port) {
  struct sockaddr_in lo = {.sin_family = AF_INET,
                           .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int cli = ccol_socket_nb(AF_INET, SOCK_DGRAM, 0);
  if (cli < 0) return -1;
  if (bind(cli, (struct sockaddr *)&lo, sizeof(lo)) != 0) {
    close(cli);
    return -1;
  }
  *free_port = lo;
  socklen_t alen = sizeof(*free_port);
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  bool have_port =
      probe >= 0 &&
      bind(probe, (struct sockaddr *)free_port, sizeof(*free_port)) == 0 &&
      getsockname(probe, (struct sockaddr *)free_port, &alen) == 0;
  if (probe >= 0) close(probe);
  if (!have_port) {
    close(cli);
    return -1;
  }
  return cli;
}

// Runs a connected UDP socket with on_readable and no on_error. A datagram to a
// port with no socket queues an ICMP error (ERR without IN); then a server
// binds that port and sends three datagrams back. Gives the ECONNREFUSED and
// datagram counts the reader saw, and *ok.
static void derr_run_udp_case(size_t threads, int *refused, int *data,
                              bool *ok) {
  *ok = false;
  *refused = 0;
  *data = 0;
  struct sockaddr_in srv_addr = {.sin_family = AF_INET,
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int cli = derr_udp_client_and_free_port(&srv_addr);
  if (cli < 0) return;
  if (test_connect(cli, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
    close(cli);
    return;
  }
  struct sockaddr_in cli_addr;
  socklen_t alen = sizeof(cli_addr);
  if (getsockname(cli, (struct sockaddr *)&cli_addr, &alen) != 0) {
    close(cli);
    return;
  }

  derr_udp_ctx ctx;
  atomic_init(&ctx.readable_count, 0);
  atomic_init(&ctx.refused_count, 0);
  atomic_init(&ctx.data_count, 0);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  int srv = -1;
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_readable = derr_on_udp_readable};
    ceventloop_reg reg =
        ceventloop_add(loop, cselectable_fd(cli, cselect_read), h, &ctx, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      bool sent = send(cli, "q", 1, 0) == 1;
      bool saw_error =
          sent && derr_wait_count(&ctx.refused_count, 1, EVL_WAIT_MS);
      srv = socket(AF_INET, SOCK_DGRAM, 0);
      bool bound = srv >= 0 && bind(srv, (struct sockaddr *)&srv_addr,
                                    sizeof(srv_addr)) == 0;
      bool replied = bound;
      for (int i = 0; replied && i < 3; i++)
        replied = sendto(srv, "hello", 5, 0, (struct sockaddr *)&cli_addr,
                         sizeof(cli_addr)) == 5;
      bool got_data =
          replied && derr_wait_count(&ctx.data_count, 3, EVL_WAIT_MS);
      *ok =
          saw_error && got_data && ceventloop_remove(loop, reg) == ccol_success;
    }
    ceventloop_destroy(loop);
  }
  *refused = atomic_load(&ctx.refused_count);
  *data = atomic_load(&ctx.data_count);
  if (srv >= 0) close(srv);
  close(cli);
}

// An error with no on_error that muted the registration would hide the
// ECONNREFUSED and the three datagrams after it.
TEST(ceventloop, udp_error_without_on_error_reaches_on_readable_1_thread) {
  int refused = 0, data = 0;
  bool ok = false;
  derr_run_udp_case(1, &refused, &data, &ok);
  REQUIRE_GE(refused, 1);
  REQUIRE_EQ(data, 3);
  REQUIRE_TRUE(ok);
}

TEST(ceventloop, udp_error_without_on_error_reaches_on_readable_4_threads) {
  int refused = 0, data = 0;
  bool ok = false;
  derr_run_udp_case(4, &refused, &data, &ok);
  REQUIRE_GE(refused, 1);
  REQUIRE_EQ(data, 3);
  REQUIRE_TRUE(ok);
}

// Counts an error in the _Atomic int at arg and takes the pending error with
// SO_ERROR, as an application would, so the error-only tests see each error
// once.
static void derr_on_error_count(ceventloop loop, ceventloop_reg reg,
                                cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  int e = 0;
  socklen_t el = sizeof(e);
  (void)getsockopt(sel->fd, SOL_SOCKET, SO_ERROR, &e, &el);
  atomic_fetch_add((_Atomic int *)arg, 1);
}

// Runs a connected UDP socket with on_error and no direction handler, and sends
// a datagram to a port with no socket. Returns how many errors reached
// on_error, or -1 when the setup failed.
static int derr_run_udp_error_only_case(size_t threads) {
  struct sockaddr_in srv_addr = {.sin_family = AF_INET,
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int cli = derr_udp_client_and_free_port(&srv_addr);
  if (cli < 0) return -1;
  if (test_connect(cli, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
    close(cli);
    return -1;
  }
  _Atomic int errors = 0;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_error = derr_on_error_count};
    ceventloop_reg reg = ceventloop_add(loop, cselectable_fd(cli, cselect_read),
                                        h, (void *)&errors, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      if (send(cli, "q", 1, 0) == 1)
        (void)derr_wait_count(&errors, 1, EVL_WAIT_MS);
      ceventloop_remove(loop, reg);
    }
    ceventloop_destroy(loop);
  }
  close(cli);
  return atomic_load(&errors);
}

// On kqueue this catches a report on the error-only read filter that refines to
// no bit, so on_error never runs.
TEST(ceventloop, udp_error_reaches_an_error_only_registration_1_thread) {
  REQUIRE_GE(derr_run_udp_error_only_case(1), 1);
}

TEST(ceventloop, udp_error_reaches_an_error_only_registration_4_threads) {
  REQUIRE_GE(derr_run_udp_error_only_case(4), 1);
}

// Counts the bytes derr_read_one_byte() read.
typedef struct {
  _Atomic int reads;
} derr_byte_reader_ctx;

// Reads one byte per call and counts it in the derr_byte_reader_ctx at arg, so
// the rest of the data has to report again.
static void derr_read_one_byte(ceventloop loop, ceventloop_reg reg,
                               cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  char b;
  if (recv(sel->fd, &b, 1, MSG_DONTWAIT) == 1)
    atomic_fetch_add(&((derr_byte_reader_ctx *)arg)->reads, 1);
}

// Runs a write direction with on_error alone, which arms the read filter in its
// error-only mode, then adds a reader for 10 bytes, which needs that filter in
// its level-triggered data mode. Returns how many bytes the reader got (10 on
// success), or -1. On kqueue a MOD that keeps the error-only trigger flags lets
// the reader run once.
static int derr_run_mode_change_case(size_t threads) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  derr_byte_reader_ctx ctx;
  atomic_init(&ctx.reads, 0);
  _Atomic int errors = 0;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers wh = {.on_error = derr_on_error_count};
    ceventloop_reg w = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, (void *)&errors, NULL);
    ceventloop_handlers rh = {.on_readable = derr_read_one_byte};
    ceventloop_reg r = ceventloop_add(loop, cselectable_fd(sv[0], cselect_read),
                                      rh, &ctx, NULL);
    if (w != CEVENTLOOP_REG_NULL && r != CEVENTLOOP_REG_NULL &&
        send(sv[1], "0123456789", 10, 0) == 10)
      (void)derr_wait_count(&ctx.reads, 10, EVL_WAIT_MS);
    if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
    if (w != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, w);
    ceventloop_destroy(loop);
  }
  close(sv[0]);
  close(sv[1]);
  return atomic_load(&ctx.reads);
}

TEST(ceventloop, a_reader_beside_an_error_only_writer_reads_all_1_thread) {
  REQUIRE_EQ(derr_run_mode_change_case(1), 10);
}

TEST(ceventloop, a_reader_beside_an_error_only_writer_reads_all_4_threads) {
  REQUIRE_EQ(derr_run_mode_change_case(4), 10);
}

// Stale reports, listeners, and an error-only filter that stays armed
//
// The reader that derr_pause_target() pauses, and the number of pauses it
// made.
static _Atomic ceventloop_reg derr_stale_target;
static _Atomic int derr_stale_paused;

// Drains the fd and holds the only reactor thread for 150 ms, so the next two
// sends of the stale-report test land in one poller batch.
static void derr_hold(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                      void *arg) {
  (void)loop;
  (void)reg;
  (void)arg;
  char b[16];
  (void)recv(sel->fd, b, sizeof(b), MSG_DONTWAIT);
  evl_sleep_ms(150);
}

// Drains the fd, pauses the reader of the other socket (whose data report is
// already in the same batch) and counts the pause.
static void derr_pause_target(ceventloop loop, ceventloop_reg reg,
                              cselectable *sel, void *arg) {
  (void)reg;
  (void)arg;
  char b[16];
  (void)recv(sel->fd, b, sizeof(b), MSG_DONTWAIT);
  ceventloop_pause(loop, atomic_load(&derr_stale_target));
  atomic_fetch_add(&derr_stale_paused, 1);
}

// Counts a dispatch in the _Atomic int at arg; the stale-report test's reader.
static void derr_count_reads(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  atomic_fetch_add((_Atomic int *)arg, 1);
}

// A data report collected while the reader wanted data and handled after it was
// paused isn't an error, so the error-only writer of the same socket sees
// nothing, as with epoll.
TEST(fd_stale_report, a_data_report_after_a_pause_is_no_error) {
  int hold[2] = {-1, -1}, pauser[2] = {-1, -1}, target[2] = {-1, -1};
  bool pairs = socketpair(AF_UNIX, SOCK_STREAM, 0, hold) == 0 &&
               socketpair(AF_UNIX, SOCK_STREAM, 0, pauser) == 0 &&
               socketpair(AF_UNIX, SOCK_STREAM, 0, target) == 0;
  atomic_store(&derr_stale_paused, 0);
  _Atomic int reads = 0, writer_errors = 0;
  ceventloop loop = pairs ? ceventloop_create(64, 1, 1, NULL) : CEVENTLOOP_NULL;
  bool sent = false;
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers hh = {.on_readable = derr_hold};
    ceventloop_add(loop, cselectable_fd(hold[0], cselect_read), hh, NULL, NULL);
    ceventloop_handlers hp = {.on_readable = derr_pause_target};
    ceventloop_add(loop, cselectable_fd(pauser[0], cselect_read), hp, NULL,
                   NULL);
    ceventloop_handlers hr = {.on_readable = derr_count_reads};
    atomic_store(&derr_stale_target,
                 ceventloop_add(loop, cselectable_fd(target[0], cselect_read),
                                hr, (void *)&reads, NULL));
    ceventloop_handlers hw = {.on_error = derr_on_error_count};
    ceventloop_add(loop, cselectable_fd(target[0], cselect_write), hw,
                   (void *)&writer_errors, NULL);
    evl_sleep_ms(30);
    sent = send(hold[1], "h", 1, 0) == 1;
    evl_sleep_ms(30);
    sent = sent && send(pauser[1], "p", 1, 0) == 1 &&
           send(target[1], "t", 1, 0) == 1;
    (void)derr_wait_count(&derr_stale_paused, 1, EVL_WAIT_MS);
    evl_sleep_ms(100);
    ceventloop_destroy(loop);
  }
  for (int i = 0; i < 2; i++) {
    if (hold[i] >= 0) close(hold[i]);
    if (pauser[i] >= 0) close(pauser[i]);
    if (target[i] >= 0) close(target[i]);
  }
  REQUIRE_TRUE(pairs);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(atomic_load(&derr_stale_paused), 1);
  REQUIRE_EQ(atomic_load(&writer_errors), 0);
}

// Pending connections on a listening socket aren't an error. An error-only
// registration of the listener sees nothing and the poller stays idle. On
// kqueue a listener ignores the low-water mark of an error-only read filter.
//
// The state of derr_late_listen_cb(): The socket, the error counter, the new
// registration and two flags.
typedef struct {
  ceventloop loop;
  int ls;
  _Atomic int *errs;
  _Atomic ceventloop_reg reg;
  atomic_bool listened;
  atomic_bool done;
} derr_late_listen_ctx;

// Registers the socket error-only and only then makes it listen, from inside a
// callback, then pauses itself. It runs once, for the late-listen case.
static void derr_late_listen_cb(ceventloop loop, ceventloop_reg self,
                                cselectable *sel, void *arg) {
  (void)sel;
  derr_late_listen_ctx *c = (derr_late_listen_ctx *)arg;
  if (atomic_load(&c->done)) return;
  ceventloop_handlers h = {.on_error = derr_on_error_count};
  atomic_store(&c->reg,
               ceventloop_add(loop, cselectable_fd(c->ls, cselect_read), h,
                              (void *)c->errs, NULL));
  atomic_store(&c->listened, listen(c->ls, 8) == 0);
  ceventloop_pause(loop, self);
  atomic_store(&c->done, true);
}

// Runs an error-only registration on a listening socket with pending
// connections (listen_late: Registered before it listens). Gives the error
// count and the poller iterations of a window.
static void derr_run_listener_case(size_t threads, bool listen_late,
                                   int *errors, uint64_t *iterations) {
  *errors = -1;
  *iterations = 0;
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) return;
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t al = sizeof(a);
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      (!listen_late && listen(ls, 8) != 0) ||
      getsockname(ls, (struct sockaddr *)&a, &al) != 0) {
    close(ls);
    return;
  }
  _Atomic int errs = 0;
  int cs[3] = {-1, -1, -1};
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_error = derr_on_error_count};
    ceventloop_reg reg = CEVENTLOOP_REG_NULL;
    if (listen_late) {
      derr_late_listen_ctx c = {.loop = loop, .ls = ls, .errs = &errs};
      int trig[2] = {-1, -1};
      bool ok = pipe(trig) == 0;
      ceventloop_reg treg = CEVENTLOOP_REG_NULL;
      if (ok)
        treg = ceventloop_add(
            loop, cselectable_fd(trig[0], cselect_read),
            (ceventloop_handlers){.on_readable = derr_late_listen_cb}, &c,
            NULL);
      ok = ok && treg != CEVENTLOOP_REG_NULL && write(trig[1], "x", 1) == 1;
      long long end = mono_now_us() + (long long)EVL_WAIT_MS * 1000;
      while (ok && !atomic_load(&c.done) && mono_now_us() < end)
        evl_sleep_ms(1);
      if (treg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, treg);
      if (trig[0] >= 0) close(trig[0]);
      if (trig[1] >= 0) close(trig[1]);
      reg = atomic_load(&c.reg);
      if (!atomic_load(&c.listened)) {
        if (reg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, reg);
        ceventloop_destroy(loop);
        close(ls);
        return;
      }
    } else {
      reg = ceventloop_add(loop, cselectable_fd(ls, cselect_read), h,
                           (void *)&errs, NULL);
    }
    for (int i = 0; i < 3; i++) {
      cs[i] = socket(AF_INET, SOCK_STREAM, 0);
      if (cs[i] >= 0)
        (void)test_connect(cs[i], (struct sockaddr *)&a, sizeof(a));
    }
    evl_sleep_ms(50);
    uint64_t before = ceventloop__poller_iterations_for_tests(loop);
    evl_sleep_ms(150);
    *iterations = ceventloop__poller_iterations_for_tests(loop) - before;
    if (reg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, reg);
    ceventloop_destroy(loop);
    *errors = atomic_load(&errs);
  }
  for (int i = 0; i < 3; i++)
    if (cs[i] >= 0) close(cs[i]);
  close(ls);
}

TEST(fd_listener, pending_connections_are_no_error_1_thread) {
  int errors;
  uint64_t iterations;
  derr_run_listener_case(1, false, &errors, &iterations);
  REQUIRE_EQ(errors, 0);
  REQUIRE_LT(iterations, (uint64_t)20);
}

TEST(fd_listener, pending_connections_are_no_error_4_threads) {
  int errors;
  uint64_t iterations;
  derr_run_listener_case(4, false, &errors, &iterations);
  REQUIRE_EQ(errors, 0);
  REQUIRE_LT(iterations, (uint64_t)20);
}

// The same for a socket registered before it listens, with no MOD after it.
TEST(fd_listener, a_socket_that_listens_late_reports_no_error_1_thread) {
  int errors;
  uint64_t iterations;
  derr_run_listener_case(1, true, &errors, &iterations);
  REQUIRE_EQ(errors, 0);
  REQUIRE_LT(iterations, (uint64_t)20);
}

// Runs a write direction with on_error alone on a loop with a dispatch pool.
// The peer's half-close isn't an event for it, but the full close after it must
// reach on_error. with_reader registers a reader first and pauses it, so the
// read filter changes from data mode to error-only mode. Returns the error
// count, or -1. On kqueue a one-shot error-only filter stays disabled after the
// half-close and loses the close.
static int derr_run_close_after_half_close(bool with_reader) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  _Atomic int errors = 0, reads = 0;
  ceventloop loop = ceventloop_create(8, 1, 4, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_reg r = CEVENTLOOP_REG_NULL;
    if (with_reader) {
      ceventloop_handlers rh = {.on_readable = derr_count_reads};
      r = ceventloop_add(loop, cselectable_fd(sv[0], cselect_read), rh,
                         (void *)&reads, NULL);
    }
    ceventloop_handlers wh = {.on_error = derr_on_error_count};
    ceventloop_reg w = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_write), wh, (void *)&errors, NULL);
    if (r != CEVENTLOOP_REG_NULL) ceventloop_pause(loop, r);
    evl_sleep_ms(30);
    if (shutdown(sv[1], SHUT_WR) == 0) {
      evl_sleep_ms(100);
      close(sv[1]);
      sv[1] = -1;
      (void)derr_wait_count(&errors, 1, EVL_WAIT_MS);
    }
    if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
    if (w != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, w);
    ceventloop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  return atomic_load(&errors);
}

TEST(fd_error_only, a_close_after_a_half_close_reaches_on_error) {
  REQUIRE_GE(derr_run_close_after_half_close(false), 1);
}

TEST(fd_error_only, a_close_after_a_paused_reader_reaches_on_error) {
  REQUIRE_GE(derr_run_close_after_half_close(true), 1);
}

// What the fd and queue callbacks of the mark probe counted.
typedef struct {
  ceventloop loop;
  cdynmq *dq;
  _Atomic int fd_calls;
  _Atomic int fd_marked;
  _Atomic int queue_calls;
  _Atomic int queue_marked;
} mark_probe_ctx;

// Reads one byte and counts the call, and whether the thread carries the loop's
// mark that the self-call guards read.
static void mark_probe_on_fd(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg) {
  (void)reg;
  mark_probe_ctx *c = (mark_probe_ctx *)arg;
  char byte;
  if (read(sel->fd, &byte, 1) != 1) return;
  if (ceventloop__test_runs_on_loop_thread(loop))
    atomic_fetch_add(&c->fd_marked, 1);
  atomic_fetch_add(&c->fd_calls, 1);
}

// Drains the queue and counts the call, and whether the thread carries both the
// loop mark and the queue-dispatch mark.
static void mark_probe_on_queue(ceventloop loop, ceventloop_reg reg,
                                cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  mark_probe_ctx *c = (mark_probe_ctx *)arg;
  cmessage out;
  while (cdynmq_try_recv(c->dq, &out) == ccol_success) {
  }
  if (ceventloop__test_runs_on_loop_thread(loop) &&
      ceventloop__test_dispatches_queue(c->dq))
    atomic_fetch_add(&c->queue_marked, 1);
  atomic_fetch_add(&c->queue_calls, 1);
}

// Runs one fd callback and one queue callback on a loop where storing each
// thread mark in its key fails, and gives the counts of mark_probe_ctx (-1 on a
// setup failure).
static void mark_probe_run(size_t threads, int *fd_calls, int *fd_marked,
                           int *queue_calls, int *queue_marked) {
  *fd_calls = *fd_marked = *queue_calls = *queue_marked = -1;
  int pfd[2];
  if (pipe(pfd) != 0) return;
  mark_probe_ctx ctx;
  ctx.dq = cdynmq_create(NULL);
  atomic_init(&ctx.fd_calls, 0);
  atomic_init(&ctx.fd_marked, 0);
  atomic_init(&ctx.queue_calls, 0);
  atomic_init(&ctx.queue_marked, 0);
  ceventloop__test_fail_next_marks(UINT32_MAX);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ctx.loop = loop;
  if (loop != CEVENTLOOP_NULL && ctx.dq) {
    ceventloop_handlers fh = {.on_readable = mark_probe_on_fd};
    ceventloop_handlers qh = {.on_readable = mark_probe_on_queue};
    ceventloop_reg fr = ceventloop_add(
        loop, cselectable_fd(pfd[0], cselect_read), fh, &ctx, NULL);
    ceventloop_reg qr = ceventloop_add(
        loop, cdynmq_selectable(ctx.dq, cselect_read), qh, &ctx, NULL);
    static int payload = 3;
    cmessage m = {.data = &payload, .size = sizeof(int)};
    char byte = 'x';
    if (fr != CEVENTLOOP_REG_NULL && qr != CEVENTLOOP_REG_NULL &&
        write(pfd[1], &byte, 1) == 1 &&
        cdynmq_send(ctx.dq, &m) == ccol_success) {
      (void)derr_wait_count(&ctx.fd_calls, 1, EVL_WAIT_MS);
      (void)derr_wait_count(&ctx.queue_calls, 1, EVL_WAIT_MS);
    }
    if (fr != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, fr);
    if (qr != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, qr);
  }
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  ceventloop__test_fail_next_marks(0);
  if (ctx.dq) {
    cmessage out;
    while (cdynmq_try_recv(ctx.dq, &out) == ccol_success) {
    }
    cdynmq_destroy(ctx.dq);
  }
  close(pfd[0]);
  close(pfd[1]);
  *fd_calls = atomic_load(&ctx.fd_calls);
  *fd_marked = atomic_load(&ctx.fd_marked);
  *queue_calls = atomic_load(&ctx.queue_calls);
  *queue_marked = atomic_load(&ctx.queue_marked);
}

// A thread whose key can't store its mark carries it anyway, so the
// self-destroy and self-shutdown guards still see a callback.
TEST(ceventloop, a_mark_that_the_key_cannot_store_still_holds) {
  int fc1, fm1, qc1, qm1, fc4, fm4, qc4, qm4;
  mark_probe_run(1, &fc1, &fm1, &qc1, &qm1);
  mark_probe_run(4, &fc4, &fm4, &qc4, &qm4);
  REQUIRE_GE(fc1, 1);
  REQUIRE_EQ(fm1, fc1);
  REQUIRE_GE(qc1, 1);
  REQUIRE_EQ(qm1, qc1);
  REQUIRE_GE(fc4, 1);
  REQUIRE_EQ(fm4, fc4);
  REQUIRE_GE(qc4, 1);
  REQUIRE_EQ(qm4, qc4);
}

// The error count and the handle of derr_pipe_on_error().
typedef struct {
  _Atomic int errors;
  _Atomic ceventloop_reg self_reg;
} derr_pipe_ctx;

// Counts an error and removes its own registration, because a hang-up stays
// true and would be reported again and again.
static void derr_pipe_on_error(ceventloop loop, ceventloop_reg reg,
                               cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  derr_pipe_ctx *c = (derr_pipe_ctx *)arg;
  atomic_fetch_add(&c->errors, 1);
  ceventloop_reg r = atomic_exchange(&c->self_reg, CEVENTLOOP_REG_NULL);
  if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
}

// Runs an error-only registration on one end of a pipe whose other end closes:
// the read end, whose writer closes after three unread writes, or the write
// end, whose reader closes. Gives the errors before the close (data isn't one),
// all errors, and the poller wake-ups while unread data sits in the pipe.
static void derr_run_pipe_case(bool read_end, size_t threads,
                               int *errors_before_close, int *errors,
                               uint64_t *iterations) {
  *errors_before_close = -1;
  *errors = -1;
  *iterations = UINT64_MAX;
  int pfd[2];
  if (pipe(pfd) != 0) return;
  derr_pipe_ctx ctx;
  atomic_init(&ctx.errors, 0);
  atomic_init(&ctx.self_reg, CEVENTLOOP_REG_NULL);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_error = derr_pipe_on_error};
    int watched = read_end ? pfd[0] : pfd[1];
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(watched, read_end ? cselect_read : cselect_write),
        h, &ctx, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      atomic_store(&ctx.self_reg, reg);
      bool wrote = true;
      for (int k = 0; k < 3 && wrote; k++) {
        wrote = write(pfd[1], "abc", 3) == 3;
        evl_sleep_ms(20);
      }
      *iterations = unh_poller_iterations_in_window(loop);
      *errors_before_close = wrote ? atomic_load(&ctx.errors) : -1;
      int other = read_end ? 1 : 0;
      close(pfd[other]);
      pfd[other] = -1;
      (void)derr_wait_count(&ctx.errors, 1, EVL_WAIT_MS);
      evl_sleep_ms(50);
      ceventloop_reg r = atomic_exchange(&ctx.self_reg, CEVENTLOOP_REG_NULL);
      if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
    }
    ceventloop_destroy(loop);
    *errors = atomic_load(&ctx.errors);
  }
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
}

// On kqueue a read end with no filter armed never reports the close.
TEST(fd_error_only, a_pipe_reader_hears_the_close_of_the_writer) {
  int before1, all1, before4, all4;
  uint64_t it1, it4;
  derr_run_pipe_case(true, 1, &before1, &all1, &it1);
  derr_run_pipe_case(true, 4, &before4, &all4, &it4);
  REQUIRE_EQ(before1, 0);
  REQUIRE_EQ(all1, 1);
  REQUIRE_LT(it1, (uint64_t)20);
  REQUIRE_EQ(before4, 0);
  REQUIRE_EQ(all4, 1);
  REQUIRE_LT(it4, (uint64_t)20);
}

// On kqueue a write end with no filter armed never reports the close.
TEST(fd_error_only, a_pipe_writer_hears_the_close_of_the_reader) {
  int before1, all1, before4, all4;
  uint64_t it1, it4;
  derr_run_pipe_case(false, 1, &before1, &all1, &it1);
  derr_run_pipe_case(false, 4, &before4, &all4, &it4);
  REQUIRE_EQ(before1, 0);
  REQUIRE_EQ(all1, 1);
  REQUIRE_LT(it1, (uint64_t)20);
  REQUIRE_EQ(before4, 0);
  REQUIRE_EQ(all4, 1);
  REQUIRE_LT(it4, (uint64_t)20);
}

// The count and the handle of derr_on_writable_remove_self().
typedef struct {
  _Atomic int writable_count;
  _Atomic ceventloop_reg self_reg;
} derr_writer_ctx;

// Counts the dispatch and removes its own registration. It never writes, so the
// broken pipe raises no SIGPIPE.
static void derr_on_writable_remove_self(ceventloop loop, ceventloop_reg reg,
                                         cselectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  derr_writer_ctx *c = (derr_writer_ctx *)arg;
  atomic_fetch_add(&c->writable_count, 1);
  ceventloop_reg r = atomic_exchange(&c->self_reg, CEVENTLOOP_REG_NULL);
  if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, r);
}

// Runs a write registration with on_writable and no on_error on a full pipe
// whose reader closes (ERR without OUT). Gives the writable count, the poller
// iterations and *ok.
static void derr_run_writer_case(size_t threads, int *writable,
                                 uint64_t *iterations, bool *ok) {
  *ok = false;
  *writable = 0;
  *iterations = 0;
  int pfd[2];
  if (pipe(pfd) != 0) return;
  if (fcntl(pfd[1], F_SETFL, fcntl(pfd[1], F_GETFL) | O_NONBLOCK) != 0) {
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  char chunk[4096];
  memset(chunk, 'x', sizeof(chunk));
  while (write(pfd[1], chunk, sizeof(chunk)) > 0) {
  }
  derr_writer_ctx ctx;
  atomic_init(&ctx.writable_count, 0);
  atomic_init(&ctx.self_reg, CEVENTLOOP_REG_NULL);
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_writable = derr_on_writable_remove_self};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(pfd[1], cselect_write), h, &ctx, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      atomic_store(&ctx.self_reg, reg);
      close(pfd[0]);
      pfd[0] = -1;
      bool delivered = derr_wait_count(&ctx.writable_count, 1, EVL_WAIT_MS);
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = delivered;
    }
    ceventloop_destroy(loop);
  }
  *writable = atomic_load(&ctx.writable_count);
  if (pfd[0] >= 0) close(pfd[0]);
  close(pfd[1]);
}

// An error with no on_error that muted the registration would keep on_writable
// from running.
TEST(ceventloop, error_without_on_error_reaches_on_writable) {
  int w1 = 0, w3 = 0;
  uint64_t it1 = 0, it3 = 0;
  bool ok1 = false, ok3 = false;
  derr_run_writer_case(1, &w1, &it1, &ok1);
  derr_run_writer_case(3, &w3, &it3, &ok3);
  REQUIRE_TRUE(ok1);
  REQUIRE_EQ(w1, 1);
  REQUIRE_LT(it1, (uint64_t)20);
  REQUIRE_TRUE(ok3);
  REQUIRE_EQ(w3, 1);
  REQUIRE_LT(it3, (uint64_t)20);
}

// ceventloop: The reclaim epoch crosses a 32-bit boundary
//
// Counts the on_removed calls in the _Atomic int at arg.
static void bgen_on_removed(void *arg) {
  atomic_fetch_add((_Atomic int *)arg, 1);
}

// Does nothing; the registration only needs a handler.
static void bgen_on_error(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                          void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

// The removed entry's epoch sits just below 2^32 and the next advance crosses
// it. A counter that wraps there leaves the entry pending for ever, and the
// poller wakes on its 50 ms reclaim retry for the rest of the loop's life.
TEST(ceventloop, reclaim_epoch_crossing_32_bits_does_not_keep_poller_awake) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  _Atomic int removed_calls = 0;
  uint64_t idle_iterations = UINT64_MAX;
  bool added = false, removed = false, notified = false;
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_error = bgen_on_error,
                             .on_removed = bgen_on_removed};
    ceventloop_reg reg =
        ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), h,
                       (void *)&removed_calls, NULL);
    added = reg != CEVENTLOOP_REG_NULL;
    if (added) {
      evl_sleep_ms(50);
      ceventloop__set_poller_batch_gen_for_tests(loop, (uint64_t)UINT32_MAX);
      removed = ceventloop_remove(loop, reg) == ccol_success;
      notified = derr_wait_count(&removed_calls, 1, EVL_WAIT_MS);
      evl_sleep_ms(100);
      uint64_t before = ceventloop__poller_iterations_for_tests(loop);
      evl_sleep_ms(500);
      idle_iterations = ceventloop__poller_iterations_for_tests(loop) - before;
    }
    ceventloop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(added);
  REQUIRE_TRUE(removed);
  REQUIRE_TRUE(notified);
  REQUIRE_LT(idle_iterations, (uint64_t)3);
}

// ceventloop_destroy drains with a live handle
//
// One connection with a registration per direction on the same fd. The callback
// closes it with the documented pattern (remove both, close the fd), and the
// test keeps owning this state, so a callback after the close is counted
// instead of reading freed memory.
typedef struct {
  ceventloop loop;
  int fd;
  ceventloop_reg rreg;
  ceventloop_reg wreg;
  _Atomic int closed;
  _Atomic int in_callback;
  _Atomic int destroy_called;
  _Atomic int callbacks_after_close;
  _Atomic int remove_r_rv;
  _Atomic int remove_w_rv;
} ddrain_conn;

// The callback of both registrations of the drain test. The first call waits
// until the main thread has called ceventloop_destroy(), then gives the handle
// up to 200 ms to stop resolving (a destroy that kills the handle before its
// drain shows here), removes both registrations, stores the results and closes
// the fd. A call after the close only counts itself.
static void ddrain_cb(ceventloop l, ceventloop_reg reg, cselectable *s,
                      void *arg) {
  (void)reg;
  (void)l;
  (void)s;
  ddrain_conn *c = arg;
  if (atomic_load(&c->closed)) {
    atomic_fetch_add(&c->callbacks_after_close, 1);
    return;
  }
  atomic_store(&c->in_callback, 1);
  for (int i = 0; i < 5000 && !atomic_load(&c->destroy_called); i++)
    evl_sleep_ms(1);
  for (int i = 0; i < 200 && ceventloop__resolve_for_tests(c->loop); i++)
    evl_sleep_ms(1);
  atomic_store(&c->remove_r_rv, (int)ceventloop_remove(c->loop, c->rreg));
  atomic_store(&c->remove_w_rv, (int)ceventloop_remove(c->loop, c->wreg));
  close(c->fd);
  atomic_store(&c->closed, 1);
}

// Registers both directions of a socket whose data is waiting and whose send
// buffer has room, then destroys the loop while the callback runs, so the close
// happens inside the destroy's drain. *entered tells whether the callback ran.
static void ddrain_run(size_t threads, ddrain_conn *c, bool *entered) {
  int sv[2];
  *entered = false;
  if (ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  memset(c, 0, sizeof(*c));
  atomic_store(&c->remove_r_rv, -1000);
  atomic_store(&c->remove_w_rv, -1000);
  c->fd = sv[0];
  c->loop = ceventloop_create(16, 1, threads, NULL);
  ceventloop_handlers hr = {.on_readable = ddrain_cb};
  ceventloop_handlers hw = {.on_writable = ddrain_cb};
  test_write_retry_eintr(sv[1], "hello", 5);
  c->rreg =
      ceventloop_add(c->loop, cselectable_fd(sv[0], cselect_read), hr, c, NULL);
  c->wreg = ceventloop_add(c->loop, cselectable_fd(sv[0], cselect_write), hw, c,
                           NULL);
  for (int i = 0; i < 5000 && !atomic_load(&c->in_callback); i++)
    evl_sleep_ms(1);
  *entered = atomic_load(&c->in_callback) != 0;
  atomic_store(&c->destroy_called, 1);
  ceventloop_destroy(c->loop);
  if (!atomic_load(&c->closed)) close(sv[0]);
  close(sv[1]);
}

// A callback that runs during the drain of a destroy calls into the loop as it
// would during a shutdown: Its removes succeed and no callback runs after its
// close. A destroy that stops resolving the handle before its drain fails both
// removes with ccol_invalid_args.
TEST(ceventloop, destroy_drain_callback_removes_its_registrations_1) {
  ddrain_conn c;
  bool entered;
  ddrain_run(1, &c, &entered);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ(atomic_load(&c.remove_r_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.remove_w_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.callbacks_after_close), 0);
}

TEST(ceventloop, destroy_drain_callback_removes_its_registrations_4) {
  ddrain_conn c;
  bool entered;
  ddrain_run(4, &c, &entered);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ(atomic_load(&c.remove_r_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.remove_w_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.callbacks_after_close), 0);
}

// Allocation failure: Create never returns a dead loop, and a failing dispatch
// never spins the poller
//
// The settings of the failing allocator: The allocation count, the Nth
// allocation to fail (1-based), a size whose every allocation fails, and a
// switch that fails them all.
static _Atomic long fa_count;
static _Atomic long fa_fail_at;
static _Atomic size_t fa_fail_size;
static _Atomic int fa_fail_all;

// Counts one allocation of n bytes and tells whether the settings above make it
// fail. Every fa_* allocator asks it.
static bool fa_should_fail(size_t n) {
  long c = atomic_fetch_add(&fa_count, 1) + 1;
  if (atomic_load(&fa_fail_all)) return true;
  long at = atomic_load(&fa_fail_at);
  if (at != 0 && c == at) return true;
  size_t sz = atomic_load(&fa_fail_size);
  return sz != 0 && n == sz;
}
// The allocator of fa_procs: Each call fails when fa_should_fail() says so.
static void *fa_malloc(size_t n) {
  return fa_should_fail(n) ? NULL : malloc(n);
}
static void *fa_calloc(size_t k, size_t n) {
  return fa_should_fail(k * n) ? NULL : calloc(k, n);
}
static void *fa_realloc(void *p, size_t n) {
  return fa_should_fail(n) ? NULL : realloc(p, n);
}
// Turns every failure off and clears the count, between two runs.
static void fa_reset(void) {
  atomic_store(&fa_fail_all, 0);
  atomic_store(&fa_fail_at, 0);
  atomic_store(&fa_fail_size, 0);
  atomic_store(&fa_count, 0);
}
// The allocator table that the allocation-failure tests hand to the loop.
static ccol_memmgmt_procs fa_procs = {.malloc = fa_malloc,
                                      .free = free,
                                      .calloc = fa_calloc,
                                      .realloc = fa_realloc};

// Counts an on_removed call in the _Atomic int at arg.
static void fa_count_removed(void *arg) {
  atomic_fetch_add((_Atomic int *)arg, 1);
}

// Does nothing; the registrations of the add sweep only have to exist.
static void fa_noop_readable(ceventloop l, ceventloop_reg reg, cselectable *s,
                             void *arg) {
  (void)l;
  (void)reg;
  (void)s;
  (void)arg;
}

// A destroy that can't allocate frees every fd registration anyway and calls
// its on_removed. A teardown that walks them with an allocating iterator walks
// nothing when the allocation fails.
TEST(ceventloop, a_destroy_without_memory_still_calls_on_removed) {
  fa_reset();
  char *err = NULL;
  ceventloop loop = ceventloop_create_mp(8, 4, 1, &fa_procs, &err);
  bool created = loop != CEVENTLOOP_NULL;
  int sv[2] = {-1, -1};
  bool have_pair = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
  _Atomic int removed = 0;
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  if (created && have_pair) {
    ceventloop_handlers h = {.on_readable = fa_noop_readable,
                             .on_removed = fa_count_removed};
    reg = ceventloop_add(loop, cselectable_fd(sv[0], cselect_read), h,
                         (void *)&removed, &err);
  }
  atomic_store(&fa_fail_all, 1);
  if (created) ceventloop_destroy(loop);
  fa_reset();
  if (have_pair) {
    close(sv[0]);
    close(sv[1]);
  }
  int removed_calls = atomic_load(&removed);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(have_pair);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_EQ(removed_calls, 1);
}

// Reads the 8 bytes of a wake pipe and counts the dispatch in the _Atomic int
// at arg, so fa_loop_dispatches() can tell that a loop works.
static void fa_on_readable(ceventloop l, ceventloop_reg reg, cselectable *s,
                           void *arg) {
  (void)reg;
  (void)l;
  uint64_t v;
  if (read(s->fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
    atomic_fetch_add((_Atomic int *)arg, 1);
}

// Makes a non-blocking, close-on-exec pipe and returns its read end, with the
// write end in *wfd, or -1. One 8-byte write keeps it readable until
// fa_on_readable() reads it, as a level-triggered fd does on every system.
static int fa_wake_pipe(int *wfd) {
  int p[2];
  if (pipe(p) != 0) return -1;
  for (int i = 0; i < 2; i++) {
    (void)fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
    (void)fcntl(p[i], F_SETFD, FD_CLOEXEC);
  }
  *wfd = p[1];
  return p[0];
}

// Tells whether l dispatches a readable fd within a bounded time while every
// allocation succeeds; the create sweep uses it to prove no handle names a dead
// loop.
static bool fa_loop_dispatches(ceventloop l) {
  int wfd = -1;
  int efd = fa_wake_pipe(&wfd);
  if (efd < 0) return false;
  _Atomic int got = 0;
  ceventloop_handlers h = {.on_readable = fa_on_readable};
  ceventloop_reg r = ceventloop_add(l, cselectable_fd(efd, cselect_read), h,
                                    (void *)&got, NULL);
  bool ok = false;
  if (r != CEVENTLOOP_REG_NULL) {
    uint64_t one = 1;
    test_write_retry_eintr(wfd, &one, sizeof(one));
    ok = derr_wait_count(&got, 1, 5000);
    ceventloop_remove(l, r);
  }
  ceventloop_shutdown(l);
  close(efd);
  if (wfd >= 0) close(wfd);
  return ok;
}

// Every handle the create returns must dispatch, whichever allocation fails.
// A poller that allocated its event buffer itself would exit on the failure and
// leave a dead loop behind a valid handle.
TEST(ceventloop, create_under_allocation_failure_never_returns_dead_loop) {
  int dead = 0, created = 0, refused = 0;
  for (size_t threads = 1; threads <= 3; threads += 2) {
    fa_reset();
    atomic_store(&fa_fail_size, (size_t)37 * sizeof(ccol_poll_event));
    char *err = NULL;
    ceventloop l = ceventloop_create_mp(37, 2, threads, &fa_procs, &err);
    evl_sleep_ms(50);
    fa_reset();
    if (l == CEVENTLOOP_NULL) {
      refused++;
      if (err == NULL) dead++;
      continue;
    }
    created++;
    if (!fa_loop_dispatches(l)) dead++;
    ceventloop_destroy(l);
  }

  fa_reset();
  ceventloop probe = ceventloop_create_mp(16, 2, 3, &fa_procs, NULL);
  long total = atomic_load(&fa_count);
  if (probe != CEVENTLOOP_NULL) ceventloop_destroy(probe);
  for (size_t threads = 1; threads <= 3; threads += 2) {
    for (long n = 1; n <= total + 4; n++) {
      fa_reset();
      atomic_store(&fa_fail_at, n);
      char *err = NULL;
      ceventloop l = ceventloop_create_mp(16, 2, threads, &fa_procs, &err);
      evl_sleep_ms(5);
      fa_reset();
      if (l == CEVENTLOOP_NULL) {
        if (err == NULL) dead++;
        continue;
      }
      if (!fa_loop_dispatches(l)) dead++;
      ceventloop_destroy(l);
    }
  }
  fa_reset();
  REQUIRE_GT(total, 0L);
  REQUIRE_EQ(created + refused, 2);
  REQUIRE_EQ(dead, 0);
}

// Adds one registration of sel to l while allocation number fail_at of the call
// fails, for the failure-class sweep. Returns 1 for a registration (removed
// again at once), 0 for a refusal whose message names a failed allocation and
// -1 for any other refusal.
static int fa_add_outcome(ceventloop l, cselectable sel, long fail_at) {
  fa_reset();
  atomic_store(&fa_fail_at, fail_at);
  char *err = NULL;
  ceventloop_handlers h = {.on_readable = fa_noop_readable};
  ceventloop_reg r = ceventloop_add(l, sel, h, NULL, &err);
  fa_reset();
  if (r != CEVENTLOOP_REG_NULL) {
    ceventloop_remove(l, r);
    return 1;
  }
  return (err && strstr(err, "failed to allocate")) ? 0 : -1;
}

// Each failed allocation of an add gives "failed to allocate", for an fd and
// for a queue, while a failed init of a queue registration's wait mutex
// (reached only through the hook) gives "a system call failed".
TEST(ceventloop, add_reports_each_failure_class_with_its_message) {
  fa_reset();
  ceventloop l = ceventloop_create_mp(8, 1, 1, &fa_procs, NULL);
  REQUIRE_NE(l, CEVENTLOOP_NULL);
  int sv[2] = {-1, -1};
  bool have_pair = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
  ccircq *cq = ccircq_create_mp(8, NULL, NULL);
  bool have_cq = cq != NULL;

  int wrong_messages = 0, refusals = 0, total_fd = 0, total_q = 0;
  if (have_pair && have_cq) {
    cselectable sels[2] = {cselectable_fd(sv[0], cselect_read),
                           ccircq_selectable(cq, cselect_read)};
    for (int k = 0; k < 2; k++) {
      fa_reset();
      ceventloop_handlers h = {.on_readable = fa_noop_readable};
      ceventloop_reg r = ceventloop_add(l, sels[k], h, NULL, NULL);
      long total = atomic_load(&fa_count);
      fa_reset();
      if (r != CEVENTLOOP_REG_NULL) ceventloop_remove(l, r);
      if (k == 0)
        total_fd = (int)total;
      else
        total_q = (int)total;
      for (long n = 1; n <= total + 2; n++) {
        int o = fa_add_outcome(l, sels[k], n);
        if (o < 0) wrong_messages++;
        if (o == 0) refusals++;
      }
    }
  }

  char *sync_err = NULL;
  ceventloop_reg sync_reg = CEVENTLOOP_REG_NULL;
  if (have_cq) {
    ceventloop__test_fail_next_reg_sync_init();
    ceventloop_handlers h = {.on_readable = fa_noop_readable};
    sync_reg = ceventloop_add(l, ccircq_selectable(cq, cselect_read), h, NULL,
                              &sync_err);
    if (sync_reg != CEVENTLOOP_REG_NULL) ceventloop_remove(l, sync_reg);
  }
  bool sync_message_ok =
      sync_err && strstr(sync_err, "a system call failed") != NULL;

  char *slot_err = NULL;
  ceventloop_reg slot_reg = CEVENTLOOP_REG_NULL;
  if (have_pair) {
    ceventloop__test_force_next_reg_slot_acquire_failure();
    ceventloop_handlers h = {.on_readable = fa_noop_readable};
    slot_reg = ceventloop_add(l, cselectable_fd(sv[0], cselect_read), h, NULL,
                              &slot_err);
    if (slot_reg != CEVENTLOOP_REG_NULL) ceventloop_remove(l, slot_reg);
  }
  bool slot_message_ok =
      slot_err && strstr(slot_err, "failed to allocate") != NULL;

  ceventloop_destroy(l);
  if (have_cq) ccircq_destroy(cq);
  if (have_pair) {
    close(sv[0]);
    close(sv[1]);
  }
  fa_reset();
  REQUIRE_TRUE(have_pair);
  REQUIRE_TRUE(have_cq);
  REQUIRE_GT(total_fd, 0);
  REQUIRE_GT(total_q, 0);
  REQUIRE_GT(refusals, 0);
  REQUIRE_EQ(wrong_messages, 0);
  REQUIRE_EQ(sync_reg, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(sync_message_ok);
  REQUIRE_EQ(slot_reg, CEVENTLOOP_REG_NULL);
  REQUIRE_TRUE(slot_message_ok);
}

// A dispatch that can't be submitted to the pool leaves the fd ready. The
// poller backs off instead of collecting it again at once, and dispatches
// normally once allocation works again.
TEST(ceventloop, failed_dispatch_submit_backs_off_instead_of_spinning) {
  fa_reset();
  ceventloop l = ceventloop_create_mp(8, 1, 3, &fa_procs, NULL);
  REQUIRE_NE(l, CEVENTLOOP_NULL);
  int wfd = -1;
  int efd = fa_wake_pipe(&wfd);
  _Atomic int got = 0;
  ceventloop_handlers h = {.on_readable = fa_on_readable};
  ceventloop_reg r = ceventloop_add(l, cselectable_fd(efd, cselect_read), h,
                                    (void *)&got, NULL);
  bool added = r != CEVENTLOOP_REG_NULL;
  uint64_t spins = 0;
  int got_while_failing = 0;
  bool recovered = false;
  if (added) {
    evl_sleep_ms(20);
    atomic_store(&fa_fail_all, 1);
    uint64_t one = 1;
    test_write_retry_eintr(wfd, &one, sizeof(one));
    evl_sleep_ms(100);
    uint64_t before = ceventloop__poller_iterations_for_tests(l);
    evl_sleep_ms(300);
    spins = ceventloop__poller_iterations_for_tests(l) - before;
    got_while_failing = atomic_load(&got);
    atomic_store(&fa_fail_all, 0);
    recovered = derr_wait_count(&got, 1, 5000);
    ceventloop_remove(l, r);
  }
  ceventloop_destroy(l);
  close(efd);
  if (wfd >= 0) close(wfd);
  fa_reset();
  REQUIRE_TRUE(added);
  REQUIRE_EQ(got_while_failing, 0);
  REQUIRE_LT(spins, (uint64_t)30);
  REQUIRE_TRUE(recovered);
}

// ceventloop: The self-call guards identify the poller by a thread-local mark,
// never by its thread ID
//
// The loop, the ID of its joined poller and what the matching thread saw.
typedef struct {
  ceventloop loop;
  bool created;
  pthread_t poller;
  _Atomic bool matched;
  _Atomic int shutdown_rv;
  _Atomic bool destroyed;
} tidr_ctx;

// Does nothing unless it carries the ID of the joined poller. Then it shuts the
// loop down and destroys it, which an application thread may do, and records
// the results.
static void *tidr_thread(void *arg) {
  tidr_ctx *c = (tidr_ctx *)arg;
  if (!pthread_equal(pthread_self(), c->poller)) return NULL;
  atomic_store(&c->matched, true);
  int rv = (int)ceventloop_shutdown(c->loop);
  atomic_store(&c->shutdown_rv, rv);
  if (rv == (int)ccol_success) {
    ceventloop l = c->loop;
    ceventloop_destroy(l);
    atomic_store(&c->destroyed, true);
  }
  return NULL;
}

// glibc declares these two only under _GNU_SOURCE, which this file doesn't
// define.
#if defined(__GLIBC__)
extern int pthread_getattr_default_np(pthread_attr_t *attr);
extern int pthread_setattr_default_np(const pthread_attr_t *attr);
#endif

static void tidr_run_body(size_t threads, tidr_ctx *c);

// Shuts the loop down, joining its poller, and starts batches of threads until
// one gets the joined poller's ID (glibc reuses a joined thread's descriptor).
// The default stack is set to 1 MiB meanwhile: glibc caches at most 40 MiB of
// stacks, and with 8 MiB stacks the poller's falls out of the cache.
static void tidr_run(size_t threads, tidr_ctx *c) {
  atomic_init(&c->matched, false);
  atomic_init(&c->shutdown_rv, -1000);
  atomic_init(&c->destroyed, false);
#if defined(__GLIBC__)
  pthread_attr_t saved_default;
  bool default_saved = pthread_getattr_default_np(&saved_default) == 0;
  bool default_changed = false;
  if (default_saved) {
    pthread_attr_t small;
    if (pthread_attr_init(&small) == 0) {
      default_changed = pthread_attr_setstacksize(&small, 1024 * 1024) == 0 &&
                        pthread_setattr_default_np(&small) == 0;
      pthread_attr_destroy(&small);
    }
  }
#endif
  tidr_run_body(threads, c);
#if defined(__GLIBC__)
  if (default_changed) pthread_setattr_default_np(&saved_default);
  if (default_saved) pthread_attr_destroy(&saved_default);
#endif
}

// The loop of tidr_run(): Up to 8 loops, 64 rounds of 16 threads each, every
// thread joined before the next batch. On macOS a pthread_t is the address of
// the thread's stack mapping, so once another mapping takes that address a new
// loop is tried.
static void tidr_run_body(size_t threads, tidr_ctx *c) {
  enum { TIDR_LOOPS = 8, TIDR_ROUNDS = 64, TIDR_BATCH = 16 };
  for (int attempt = 0; attempt < TIDR_LOOPS && !atomic_load(&c->matched);
       attempt++) {
    c->loop = ceventloop_create(8, 1, threads, NULL);
    c->created = c->loop != CEVENTLOOP_NULL;
    if (!c->created) return;
    c->poller = ceventloop__poller_thread_for_tests(c->loop);
    if (ceventloop_shutdown(c->loop) == ccol_success) {
      for (int round = 0; round < TIDR_ROUNDS && !atomic_load(&c->matched);
           round++) {
        pthread_t tids[TIDR_BATCH];
        int started = 0;
        for (; started < TIDR_BATCH; started++) {
          if (pthread_create(&tids[started], NULL, tidr_thread, c) != 0) break;
        }
        for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
        if (started == 0) break;
      }
    }
    if (!atomic_load(&c->destroyed)) {
      ceventloop l = c->loop;
      ceventloop_destroy(l);
    }
  }
}

// A guard that compared thread IDs would refuse the shutdown of the thread that
// inherited the poller's ID and treat its destroy as a self-destroy. The test
// fails when no thread got the ID.
TEST(ceventloop, a_thread_that_inherits_the_poller_id_may_destroy_1) {
  tidr_ctx c;
  tidr_run(1, &c);
  REQUIRE_TRUE(c.created);
  REQUIRE_TRUE(atomic_load(&c.matched));
  REQUIRE_EQ(atomic_load(&c.shutdown_rv), (int)ccol_success);
  REQUIRE_TRUE(atomic_load(&c.destroyed));
}

TEST(ceventloop, a_thread_that_inherits_the_poller_id_may_destroy_4) {
  tidr_ctx c;
  tidr_run(4, &c);
  REQUIRE_TRUE(c.created);
  REQUIRE_TRUE(atomic_load(&c.matched));
  REQUIRE_EQ(atomic_load(&c.shutdown_rv), (int)ccol_success);
  REQUIRE_TRUE(atomic_load(&c.destroyed));
}

// The loop and the result of orsd_on_removed().
typedef struct {
  ceventloop loop;
  _Atomic int rv;
  _Atomic bool done;
} orsd_ctx;

// Shuts its own loop down from on_removed and stores the result.
static void orsd_on_removed(void *arg) {
  orsd_ctx *c = (orsd_ctx *)arg;
  atomic_store(&c->rv, (int)ceventloop_shutdown(c->loop));
  atomic_store(&c->done, true);
}

// Registers and removes an fd on a loop with `threads` reactor threads and
// waits for orsd_on_removed(). on_removed runs on the poller whatever the
// thread count, so its shutdown is a self-join the guard must refuse.
static void orsd_run(size_t threads, orsd_ctx *c) {
  atomic_init(&c->rv, -1000);
  atomic_init(&c->done, false);
  int pfd[2];
  if (pipe(pfd) != 0) return;
  c->loop = ceventloop_create(8, 1, threads, NULL);
  if (c->loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_removed = orsd_on_removed};
    ceventloop_reg reg = ceventloop_add(
        c->loop, cselectable_fd(pfd[0], cselect_read), h, c, NULL);
    if (reg != CEVENTLOOP_REG_NULL &&
        ceventloop_remove(c->loop, reg) == ccol_success) {
      for (int i = 0; i < 5000 && !atomic_load(&c->done); i++) evl_sleep_ms(1);
    }
    ceventloop_destroy(c->loop);
  }
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ceventloop, shutdown_from_on_removed_on_the_poller_is_refused) {
  orsd_ctx c1, c4;
  orsd_run(1, &c1);
  orsd_run(4, &c4);
  REQUIRE_TRUE(atomic_load(&c1.done));
  REQUIRE_EQ(atomic_load(&c1.rv), (int)ccol_not_permitted);
  REQUIRE_TRUE(atomic_load(&c4.done));
  REQUIRE_EQ(atomic_load(&c4.rv), (int)ccol_not_permitted);
}

// ceventloop: A queue listener that moves one message per callback drains a
// backlog, and one that moves nothing doesn't spin
//
// What a backlog listener does on each callback.
enum { BKL_RECV_ONE = 0, BKL_NOTHING = 1, BKL_SEND_ONE = 2 };

// The static message data the writing listeners send.
static char bkl_token;

// One backlog listener: Its mode and its dispatch and moved counts.
typedef struct {
  int mode;
  _Atomic long dispatches;
  _Atomic long moved;
} bkl_ctx;

// Counts a dispatch and, in BKL_RECV_ONE mode, takes one message and counts it
// as moved.
static void bkl_on_readable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  bkl_ctx *c = (bkl_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (c->mode != BKL_RECV_ONE) return;
  cmessage m = {.data = NULL, .size = 0};
  ccol_retval rv = (sel->type == cselectable_type_ccircq)
                       ? ccircq_try_recv(sel->cq, &m)
                       : cdynmq_try_recv(sel->dq, &m);
  if (rv == ccol_success) atomic_fetch_add(&c->moved, 1);
}

// Counts a dispatch and, in BKL_SEND_ONE mode, sends one message and counts it
// as moved.
static void bkl_on_writable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  bkl_ctx *c = (bkl_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (c->mode != BKL_SEND_ONE) return;
  cmessage m = {.data = &bkl_token, .size = 1};
  if (ccircq_try_send(sel->cq, &m) == ccol_success)
    atomic_fetch_add(&c->moved, 1);
}

// What a backlog run measured.
typedef struct {
  long moved;
  long dispatches_settled;
  long dispatches_later;
  size_t left_in_queue;
  bool ok;
} bkl_result;

// Waits, at most about 10 s, until c has moved target messages.
static void bkl_wait_moved(bkl_ctx *c, long target) {
  for (int i = 0; i < 10000 && atomic_load(&c->moved) < target; i++)
    evl_sleep_ms(1);
}

// Waits, at most about 10 s, until c has had target dispatches. A listener that
// moves nothing gets its first dispatch from the add's wake, so the samples
// after this wait judge only the later dispatches.
static void bkl_wait_dispatches(bkl_ctx *c, long target) {
  for (int i = 0; i < 10000 && atomic_load(&c->dispatches) < target; i++)
    evl_sleep_ms(1);
}

// Puts count messages into a queue, registers nregs read listeners in the given
// mode and sends nothing more. It waits until the messages are taken
// (BKL_RECV_ONE) or each listener got one dispatch (BKL_NOTHING), then samples
// the dispatch count twice, 200 ms apart, so a spinning listener shows as a
// growing count. The results go to *r.
static void bkl_run_read(size_t threads, bool dynq, int mode, long count,
                         int nregs, bkl_result *r) {
  memset(r, 0, sizeof(*r));
  bkl_ctx c = {.mode = mode};
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.moved, 0);
  ccircq *cq = NULL;
  cdynmq *dq = NULL;
  if (dynq)
    dq = cdynmq_create(NULL);
  else
    cq = ccircq_create((size_t)count, NULL);
  if (!cq && !dq) return;
  bool filled = true;
  for (long i = 0; i < count && filled; i++) {
    cmessage m = {.data = &bkl_token, .size = 1};
    filled = (dynq ? cdynmq_send(dq, &m) : ccircq_send(cq, &m)) == ccol_success;
  }
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ceventloop_reg regs[4] = {CEVENTLOOP_REG_NULL, CEVENTLOOP_REG_NULL,
                            CEVENTLOOP_REG_NULL, CEVENTLOOP_REG_NULL};
  bool added = filled && loop != CEVENTLOOP_NULL;
  for (int i = 0; i < nregs && added; i++) {
    cselectable sel = dynq ? cdynmq_selectable(dq, cselect_read)
                           : ccircq_selectable(cq, cselect_read);
    ceventloop_handlers h = {.on_readable = bkl_on_readable};
    regs[i] = ceventloop_add(loop, sel, h, &c, NULL);
    added = regs[i] != CEVENTLOOP_REG_NULL;
  }
  if (added) {
    if (mode == BKL_RECV_ONE) bkl_wait_moved(&c, count);
    if (mode == BKL_NOTHING) bkl_wait_dispatches(&c, nregs);
    evl_sleep_ms(200);
    r->dispatches_settled = atomic_load(&c.dispatches);
    evl_sleep_ms(200);
    r->dispatches_later = atomic_load(&c.dispatches);
  }
  for (int i = 0; i < nregs; i++)
    if (regs[i] != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, regs[i]);
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  r->moved = atomic_load(&c.moved);
  r->left_in_queue = dynq ? cdynmq_msg_count(dq) : ccircq_msg_count(cq);
  cmessage m;
  if (dynq) {
    while (cdynmq_try_recv(dq, &m) == ccol_success) {
    }
    cdynmq_destroy(dq);
  } else {
    while (ccircq_try_recv(cq, &m) == ccol_success) {
    }
    ccircq_destroy(cq);
  }
  r->ok = added;
}

// Without a new wake after a dispatch that moved a message, one eventfd wake
// covers the whole backlog and the listeners stop after a message or two.
TEST(ceventloop, a_one_message_listener_drains_a_backlog) {
  static const size_t thread_counts[] = {1, 4};
  bkl_result r[2][2][2];
  for (int t = 0; t < 2; t++)
    for (int k = 0; k < 2; k++)
      for (int n = 0; n < 2; n++)
        bkl_run_read(thread_counts[t], k == 1, BKL_RECV_ONE, 1000, n + 1,
                     &r[t][k][n]);
  for (int t = 0; t < 2; t++) {
    for (int k = 0; k < 2; k++) {
      for (int n = 0; n < 2; n++) {
        REQUIRE_TRUE(r[t][k][n].ok);
        REQUIRE_EQ(r[t][k][n].moved, 1000L);
        REQUIRE_EQ(r[t][k][n].left_in_queue, (size_t)0);
        REQUIRE_EQ(r[t][k][n].dispatches_later, r[t][k][n].dispatches_settled);
      }
    }
  }
}

// A listener that ignores a ready queue isn't dispatched again for the same
// wake: One dispatch with one listener, at most two each with two.
TEST(ceventloop, a_listener_that_moves_nothing_does_not_spin) {
  static const size_t thread_counts[] = {1, 4};
  bkl_result one[2], two[2];
  for (int t = 0; t < 2; t++) {
    bkl_run_read(thread_counts[t], false, BKL_NOTHING, 5, 1, &one[t]);
    bkl_run_read(thread_counts[t], true, BKL_NOTHING, 5, 2, &two[t]);
  }
  for (int t = 0; t < 2; t++) {
    REQUIRE_TRUE(one[t].ok);
    REQUIRE_EQ(one[t].dispatches_settled, 1L);
    REQUIRE_EQ(one[t].dispatches_later, 1L);
    REQUIRE_TRUE(two[t].ok);
    REQUIRE_GE(two[t].dispatches_settled, 2L);
    REQUIRE_LE(two[t].dispatches_settled, 4L);
    REQUIRE_EQ(two[t].dispatches_later, two[t].dispatches_settled);
    REQUIRE_EQ(two[t].left_in_queue, (size_t)5);
  }
}

// Registers one write listener in the given mode on an empty circular queue of
// the given capacity. A sending listener fills it and stops, since a full queue
// isn't writable; a listener that sends nothing is dispatched once. The results
// go to *r.
static void bkl_run_write(size_t threads, int mode, long capacity,
                          bkl_result *r) {
  memset(r, 0, sizeof(*r));
  bkl_ctx c = {.mode = mode};
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.moved, 0);
  ccircq *cq = ccircq_create((size_t)capacity, NULL);
  if (!cq) return;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_writable = bkl_on_writable};
    reg =
        ceventloop_add(loop, ccircq_selectable(cq, cselect_write), h, &c, NULL);
  }
  if (reg != CEVENTLOOP_REG_NULL) {
    if (mode == BKL_SEND_ONE) bkl_wait_moved(&c, capacity);
    if (mode == BKL_NOTHING) bkl_wait_dispatches(&c, 1);
    evl_sleep_ms(200);
    r->dispatches_settled = atomic_load(&c.dispatches);
    evl_sleep_ms(200);
    r->dispatches_later = atomic_load(&c.dispatches);
    ceventloop_remove(loop, reg);
  }
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  r->moved = atomic_load(&c.moved);
  r->left_in_queue = ccircq_msg_count(cq);
  cmessage m;
  while (ccircq_try_recv(cq, &m) == ccol_success) {
  }
  ccircq_destroy(cq);
  r->ok = reg != CEVENTLOOP_REG_NULL;
}

// Without the new wake after a dispatch that sent a message, the listener sends
// one message and is never dispatched again.
TEST(ceventloop, a_one_message_writer_fills_the_queue_and_stops) {
  static const size_t thread_counts[] = {1, 4};
  bkl_result fill[2], idle[2];
  for (int t = 0; t < 2; t++) {
    bkl_run_write(thread_counts[t], BKL_SEND_ONE, 1000, &fill[t]);
    bkl_run_write(thread_counts[t], BKL_NOTHING, 1000, &idle[t]);
  }
  for (int t = 0; t < 2; t++) {
    REQUIRE_TRUE(fill[t].ok);
    REQUIRE_EQ(fill[t].moved, 1000L);
    REQUIRE_EQ(fill[t].left_in_queue, (size_t)1000);
    REQUIRE_EQ(fill[t].dispatches_later, fill[t].dispatches_settled);
    REQUIRE_TRUE(idle[t].ok);
    REQUIRE_EQ(idle[t].dispatches_settled, 1L);
    REQUIRE_EQ(idle[t].dispatches_later, 1L);
    REQUIRE_EQ(idle[t].left_in_queue, (size_t)0);
  }
}

// The dispatch and refusal counts of dsw_on_writable().
typedef struct {
  _Atomic long dispatches;
  _Atomic long refused;
} dsw_ctx;

// Counts a dispatch and tries a send, counting it when the queue refuses with
// ccol_not_permitted.
static void dsw_on_writable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  dsw_ctx *c = (dsw_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  cmessage m = {.data = &bkl_token, .size = 1};
  if (ccircq_try_send(sel->cq, &m) == ccol_not_permitted)
    atomic_fetch_add(&c->refused, 1);
}

// What a sends-off run measured.
typedef struct {
  long before_disable;
  long settled;
  long later;
  long refused;
  size_t left_in_queue;
  bool ok;
} dsw_result;

// Runs a write registration on a full circular queue whose sends are off. The
// queue counts as ready, so the registration gets exactly one call, whose send
// is refused. add_after_disable turns sends off before the add (the add reports
// the state); otherwise the disable does. The results go to *r.
static void dsw_run(size_t threads, bool add_after_disable, dsw_result *r) {
  memset(r, 0, sizeof(*r));
  dsw_ctx c;
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.refused, 0);
  ccircq *cq = ccircq_create(1, NULL);
  if (!cq) return;
  cmessage fill = {.data = &bkl_token, .size = 1};
  bool filled = ccircq_try_send(cq, &fill) == ccol_success;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  ceventloop_handlers h = {.on_writable = dsw_on_writable};
  cselectable sel = ccircq_selectable(cq, cselect_write);
  if (add_after_disable) ccircq_disable_sending(cq);
  if (loop != CEVENTLOOP_NULL) reg = ceventloop_add(loop, sel, h, &c, NULL);
  if (reg != CEVENTLOOP_REG_NULL) {
    if (!add_after_disable) {
      evl_sleep_ms(100);
      r->before_disable = atomic_load(&c.dispatches);
      ccircq_disable_sending(cq);
    }
    for (int i = 0; i < 10000 && atomic_load(&c.dispatches) < 1; i++)
      evl_sleep_ms(1);
    evl_sleep_ms(200);
    r->settled = atomic_load(&c.dispatches);
    evl_sleep_ms(200);
    r->later = atomic_load(&c.dispatches);
    ceventloop_remove(loop, reg);
  }
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  r->refused = atomic_load(&c.refused);
  r->left_in_queue = ccircq_msg_count(cq);
  cmessage m;
  while (ccircq_try_recv(cq, &m) == ccol_success) {
  }
  ccircq_destroy(cq);
  r->ok = filled && reg != CEVENTLOOP_REG_NULL;
}

// A queue with sends off must count as ready for writing, or a registration
// added after the disable gets no call at all.
TEST(ceventloop, a_write_registration_hears_once_that_sends_are_off) {
  static const size_t thread_counts[] = {1, 4};
  dsw_result before[2], after[2];
  for (int t = 0; t < 2; t++) {
    dsw_run(thread_counts[t], false, &before[t]);
    dsw_run(thread_counts[t], true, &after[t]);
  }
  for (int t = 0; t < 2; t++) {
    REQUIRE_TRUE(before[t].ok);
    REQUIRE_EQ(before[t].before_disable, 0L);
    REQUIRE_EQ(before[t].settled, 1L);
    REQUIRE_EQ(before[t].later, 1L);
    REQUIRE_EQ(before[t].refused, 1L);
    REQUIRE_EQ(before[t].left_in_queue, (size_t)1);
    REQUIRE_TRUE(after[t].ok);
    REQUIRE_EQ(after[t].settled, 1L);
    REQUIRE_EQ(after[t].later, 1L);
    REQUIRE_EQ(after[t].refused, 1L);
    REQUIRE_EQ(after[t].left_in_queue, (size_t)1);
  }
}

// ceventloop_add and ceventloop_modify: The documented outputs
TEST(ceventloop, add_sets_err_str_to_null_on_success) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  bool created = loop != CEVENTLOOP_NULL;
  char *err = (char *)"poison";
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  if (created) {
    ceventloop_handlers none = {0};
    reg = ceventloop_add(loop, cselectable_fd(pfd[0], cselect_read), none, NULL,
                         &err);
    ceventloop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(created);
  REQUIRE_NE(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_EQ((void *)err, NULL);
}

// Mutes a read registration with no handler through a hang-up, then modifies it
// to its own direction, which must arm it again so the hang-up is reported once
// more and mutes it again. Gives how far the poller advanced after the modify,
// the iterations of a later window and *ok.
static void msd_run(size_t threads, uint64_t *advanced, uint64_t *window,
                    bool *ok) {
  *ok = false;
  *advanced = 0;
  *window = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers none = {0};
    ceventloop_reg reg = ceventloop_add(
        loop, cselectable_fd(sv[0], cselect_read), none, NULL, NULL);
    if (reg != CEVENTLOOP_REG_NULL) {
      close(sv[1]);
      sv[1] = -1;
      evl_sleep_ms(100);
      uint64_t before = ceventloop__poller_iterations_for_tests(loop);
      bool modified =
          ceventloop_modify(loop, reg, cselect_read) == ccol_success;
      for (int i = 0; i < 2000; i++) {
        if (ceventloop__poller_iterations_for_tests(loop) > before) break;
        evl_sleep_ms(1);
      }
      *advanced = ceventloop__poller_iterations_for_tests(loop) - before;
      *window = unh_poller_iterations_in_window(loop);
      *ok = modified && ceventloop_remove(loop, reg) == ccol_success;
    }
    ceventloop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

// A modify to the current direction that doesn't arm the muted registration
// again leaves the iteration count where it was.
TEST(ceventloop, a_modify_to_the_same_direction_unmutes) {
  uint64_t adv1, win1, adv4, win4;
  bool ok1, ok4;
  msd_run(1, &adv1, &win1, &ok1);
  msd_run(4, &adv4, &win4, &ok4);
  REQUIRE_TRUE(ok1);
  REQUIRE_GE(adv1, (uint64_t)1);
  REQUIRE_LT(win1, (uint64_t)20);
  REQUIRE_TRUE(ok4);
  REQUIRE_GE(adv4, (uint64_t)1);
  REQUIRE_LT(win4, (uint64_t)20);
}

// The queue mutex registry removes an entry in constant time
#if CCOL_FORK_SAFETY_REQUIRED
// A remove that scans the registry makes the probe count grow with the square
// of the queue count, and a swap that doesn't update the moved entry's position
// fails the index check.
TEST(queue_mutex_registry, a_remove_is_constant_time_and_keeps_every_index) {
  enum { QMR_N = 256 };
  void *queues[QMR_N];
  bool is_dyn[QMR_N];
  size_t base_count = 0;
  uint64_t base_probes = 0;
  bool base_ok =
      ccol__queue_mutex_registry_check_for_tests(&base_count, &base_probes);
  int created = 0;
  for (; created < QMR_N; created++) {
    is_dyn[created] = (created % 3) == 0;
    queues[created] = is_dyn[created] ? (void *)cdynmq_create(NULL)
                                      : (void *)ccircq_create(4, NULL);
    if (!queues[created]) break;
  }
  size_t full_count = 0;
  bool full_ok = ccol__queue_mutex_registry_check_for_tests(&full_count, NULL);
  unsigned seed = 12345u;
  for (int i = created - 1; i > 0; i--) {
    int j = (int)(rand_r(&seed) % (unsigned)(i + 1));
    void *tq = queues[i];
    queues[i] = queues[j];
    queues[j] = tq;
    bool td = is_dyn[i];
    is_dyn[i] = is_dyn[j];
    is_dyn[j] = td;
  }
  bool every_step_ok = true;
  for (int i = 0; i < created; i++) {
    if (is_dyn[i]) {
      cdynmq *dq = (cdynmq *)queues[i];
      cdynmq_destroy(dq);
    } else {
      ccircq *cq = (ccircq *)queues[i];
      ccircq_destroy(cq);
    }
    if (!ccol__queue_mutex_registry_check_for_tests(NULL, NULL))
      every_step_ok = false;
  }
  size_t end_count = 0;
  uint64_t end_probes = 0;
  bool end_ok =
      ccol__queue_mutex_registry_check_for_tests(&end_count, &end_probes);
  REQUIRE_EQ(created, (int)QMR_N);
  REQUIRE_TRUE(base_ok);
  REQUIRE_TRUE(full_ok);
  REQUIRE_EQ(full_count, base_count + QMR_N);
  REQUIRE_TRUE(every_step_ok);
  REQUIRE_TRUE(end_ok);
  REQUIRE_EQ(end_count, base_count);
  REQUIRE_EQ(end_probes - base_probes, (uint64_t)QMR_N);
}
#endif

// ceventloop: A queue listener that moves messages keeps its own dispatches
// going wherever an idle sibling sits in the list
//
// One listener: Whether it moves messages, a writer's budget of messages left,
// and its dispatch and moved counts.
typedef struct {
  bool active;
  _Atomic long budget;
  _Atomic long dispatches;
  _Atomic long moved;
} sib_ctx;

// Counts a dispatch and, for an active writer with budget left, sends one
// message.
static void sib_on_writable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  sib_ctx *c = (sib_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (!c->active || atomic_load(&c->budget) <= 0) return;
  cmessage m = {.data = &bkl_token, .size = 1};
  ccol_retval rv = (sel->type == cselectable_type_ccircq)
                       ? ccircq_try_send(sel->cq, &m)
                       : cdynmq_send(sel->dq, &m);
  if (rv == ccol_success) {
    atomic_fetch_sub(&c->budget, 1);
    atomic_fetch_add(&c->moved, 1);
  }
}

// Counts a dispatch and, for an active reader, takes one message.
static void sib_on_readable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  sib_ctx *c = (sib_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (!c->active) return;
  cmessage m = {.data = NULL, .size = 0};
  ccol_retval rv = (sel->type == cselectable_type_ccircq)
                       ? ccircq_try_recv(sel->cq, &m)
                       : cdynmq_try_recv(sel->dq, &m);
  if (rv == ccol_success) atomic_fetch_add(&c->moved, 1);
}

// What a sibling run measured.
typedef struct {
  long active_moved;
  long idle_settled;
  long idle_later;
  size_t left_in_queue;
  bool ok;
} sib_result;

// Registers an idle and an active listener on one queue of the given kind and
// direction, the idle one first when idle_first. The active writer sends count
// messages one per callback; for reading, the queue holds count messages before
// the listeners exist. The results go to *r.
static void sib_run(size_t threads, bool dynq, cselect_dir dir, bool idle_first,
                    long count, sib_result *r) {
  memset(r, 0, sizeof(*r));
  sib_ctx idle_c = {.active = false};
  sib_ctx act_c = {.active = true};
  atomic_init(&idle_c.budget, 0);
  atomic_init(&idle_c.dispatches, 0);
  atomic_init(&idle_c.moved, 0);
  atomic_init(&act_c.budget, dir == cselect_write ? count : 0);
  atomic_init(&act_c.dispatches, 0);
  atomic_init(&act_c.moved, 0);
  ccircq *cq = NULL;
  cdynmq *dq = NULL;
  if (dynq)
    dq = cdynmq_create(NULL);
  else
    cq = ccircq_create((size_t)count, NULL);
  if (!cq && !dq) return;
  bool filled = true;
  for (long i = 0; dir == cselect_read && i < count && filled; i++) {
    cmessage m = {.data = &bkl_token, .size = 1};
    filled = (dynq ? cdynmq_send(dq, &m) : ccircq_send(cq, &m)) == ccol_success;
  }
  ceventloop loop = ceventloop_create(8, 1, threads, NULL);
  ceventloop_reg regs[2] = {CEVENTLOOP_REG_NULL, CEVENTLOOP_REG_NULL};
  bool added = filled && loop != CEVENTLOOP_NULL;
  for (int i = 0; i < 2 && added; i++) {
    bool idle_now = (i == 0) == idle_first;
    cselectable sel =
        dynq ? cdynmq_selectable(dq, dir) : ccircq_selectable(cq, dir);
    ceventloop_handlers h = {0};
    if (dir == cselect_read)
      h.on_readable = sib_on_readable;
    else
      h.on_writable = sib_on_writable;
    regs[i] = ceventloop_add(loop, sel, h, idle_now ? &idle_c : &act_c, NULL);
    added = regs[i] != CEVENTLOOP_REG_NULL;
  }
  if (added) {
    for (int i = 0; i < 10000 && atomic_load(&act_c.moved) < count; i++)
      evl_sleep_ms(1);
    evl_sleep_ms(200);
    r->idle_settled = atomic_load(&idle_c.dispatches);
    evl_sleep_ms(200);
    r->idle_later = atomic_load(&idle_c.dispatches);
  }
  for (int i = 0; i < 2; i++)
    if (regs[i] != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, regs[i]);
  if (loop != CEVENTLOOP_NULL) ceventloop_destroy(loop);
  r->active_moved = atomic_load(&act_c.moved);
  r->left_in_queue = dynq ? cdynmq_msg_count(dq) : ccircq_msg_count(cq);
  cmessage m;
  if (dynq) {
    while (cdynmq_try_recv(dq, &m) == ccol_success) {
    }
    cdynmq_destroy(dq);
  } else {
    while (ccircq_try_recv(cq, &m) == ccol_success) {
    }
    ccircq_destroy(cq);
  }
  r->ok = added;
}

// If only the tail of the list could wake a listener again, an active listener
// registered after an idle one would hand its progress to the idle tail, which
// ends the chain, and it would move one message. The idle sibling gets a
// bounded number of dispatches, none after the traffic ends.
TEST(ceventloop, an_idle_older_sibling_does_not_strand_an_active_listener) {
  static const size_t thread_counts[] = {1, 4};
  sib_result r[2][2][2][2];
  for (int t = 0; t < 2; t++)
    for (int k = 0; k < 2; k++)
      for (int d = 0; d < 2; d++)
        for (int o = 0; o < 2; o++)
          sib_run(thread_counts[t], k == 1,
                  d == 0 ? cselect_read : cselect_write, o == 0, 100,
                  &r[t][k][d][o]);
  for (int t = 0; t < 2; t++) {
    for (int k = 0; k < 2; k++) {
      for (int d = 0; d < 2; d++) {
        for (int o = 0; o < 2; o++) {
          sib_result *x = &r[t][k][d][o];
          REQUIRE_TRUE(x->ok);
          REQUIRE_EQ(x->active_moved, 100L);
          REQUIRE_EQ(x->left_in_queue, d == 0 ? (size_t)0 : (size_t)100);
          REQUIRE_LE(x->idle_settled, 210L);
          REQUIRE_EQ(x->idle_later, x->idle_settled);
        }
      }
    }
  }
}

// Three listeners that all decline a ready queue pass a wake between them a
// bounded number of times and stop.
TEST(ceventloop, three_listeners_that_move_nothing_do_not_spin) {
  static const size_t thread_counts[] = {1, 4};
  bkl_result r[2];
  for (int t = 0; t < 2; t++)
    bkl_run_read(thread_counts[t], true, BKL_NOTHING, 5, 3, &r[t]);
  for (int t = 0; t < 2; t++) {
    REQUIRE_TRUE(r[t].ok);
    REQUIRE_GE(r[t].dispatches_settled, 3L);
    REQUIRE_LE(r[t].dispatches_settled, 12L);
    REQUIRE_EQ(r[t].dispatches_later, r[t].dispatches_settled);
    REQUIRE_EQ(r[t].left_in_queue, (size_t)5);
  }
}

// ceventloop: A listener that leaves a queue hands on the wake it holds
//
// The pipe that holds L1's poller, the queue, the two counts and the result of
// the select waiter.
typedef struct {
  int pfd[2];
  _Atomic bool blocker_entered;
  _Atomic bool blocker_release;
  _Atomic long leaving_calls;
  _Atomic long staying_received;
  ccircq *cq;
  ceventloop l1;
  size_t sel_idx;
  ccol_retval sel_rv;
} rlw_ctx;

// Reads the pipe byte and holds L1's poller (at most 20 s) until released, so
// the leaving registration can't dispatch before it leaves.
static void rlw_on_block(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                         void *arg) {
  (void)loop;
  (void)reg;
  rlw_ctx *c = (rlw_ctx *)arg;
  char ch;
  ssize_t got = read(sel->fd, &ch, 1);
  (void)got;
  atomic_store(&c->blocker_entered, true);
  for (int i = 0; i < 20000 && !atomic_load(&c->blocker_release); i++)
    evl_sleep_ms(1);
}

// Counts the calls of the leaving registration, which must not run after a
// remove.
static void rlw_on_leaving(ceventloop loop, ceventloop_reg reg,
                           cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  atomic_fetch_add(&((rlw_ctx *)arg)->leaving_calls, 1);
}

// Takes one message and counts it, so the test sees that the staying
// registration got the wake.
static void rlw_on_staying(ceventloop loop, ceventloop_reg reg,
                           cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  cmessage m;
  if (ccircq_try_recv(sel->cq, &m) == ccol_success)
    atomic_fetch_add(&((rlw_ctx *)arg)->staying_received, 1);
}

// Waits up to 5 s in cselect_timed() on the queue; the staying listener of the
// select case.
static void *rlw_select_thread(void *arg) {
  rlw_ctx *c = (rlw_ctx *)arg;
  cselectable s = ccircq_selectable(c->cq, cselect_read);
  c->sel_rv = cselect_timed(&c->sel_idx, 1, &s, 5000000);
  return NULL;
}

// Destroys L1, the way the leaving registration leaves in the destroy case.
static void *rlw_destroy_thread(void *arg) {
  rlw_ctx *c = (rlw_ctx *)arg;
  ceventloop_destroy(c->l1);
  return NULL;
}

// The staying listener is a registration of a second loop, or a cselect_timed()
// caller (use_select). The leaving one is a registration of L1 linked after it,
// so it is the head and a send wakes it, and it leaves by a remove or by L1's
// destroy (by_destroy). Returns true when the staying listener got the message;
// *setup_ok is false when the setup failed.
static bool rlw_run(bool use_select, bool by_destroy, bool *setup_ok) {
  *setup_ok = false;
  rlw_ctx c;
  memset(&c, 0, sizeof(c));
  atomic_init(&c.blocker_entered, false);
  atomic_init(&c.blocker_release, false);
  atomic_init(&c.leaving_calls, 0);
  atomic_init(&c.staying_received, 0);
  c.sel_rv = ccol_unexpected_failure;
  if (pipe(c.pfd) != 0) return false;
  c.cq = ccircq_create(8, NULL);
  c.l1 = ceventloop_create(8, 1, 1, NULL);
  ceventloop l2 = CEVENTLOOP_NULL;
  ceventloop_reg r2 = CEVENTLOOP_REG_NULL;
  bool ok = c.cq && c.l1 != CEVENTLOOP_NULL;
  pthread_t st;
  bool st_started = false;
  if (ok && !use_select) {
    l2 = ceventloop_create(8, 1, 1, NULL);
    ok = l2 != CEVENTLOOP_NULL;
    if (ok) {
      ceventloop_handlers h = {.on_readable = rlw_on_staying};
      r2 = ceventloop_add(l2, ccircq_selectable(c.cq, cselect_read), h, &c,
                          NULL);
      ok = r2 != CEVENTLOOP_REG_NULL;
    }
  } else if (ok) {
    st_started = pthread_create(&st, NULL, rlw_select_thread, &c) == 0;
    ok = st_started;
    for (int i = 0;
         ok && i < 5000 && !ccircq__test_has_sel_read_waiter_for_tests(c.cq);
         i++)
      evl_sleep_ms(1);
    ok = ok && ccircq__test_has_sel_read_waiter_for_tests(c.cq);
  }
  ceventloop_reg rb = CEVENTLOOP_REG_NULL, r1 = CEVENTLOOP_REG_NULL;
  if (ok) {
    ceventloop_handlers hb = {.on_readable = rlw_on_block};
    rb = ceventloop_add(c.l1, cselectable_fd(c.pfd[0], cselect_read), hb, &c,
                        NULL);
    ceventloop_handlers h1 = {.on_readable = rlw_on_leaving};
    r1 = ceventloop_add(c.l1, ccircq_selectable(c.cq, cselect_read), h1, &c,
                        NULL);
    ok = rb != CEVENTLOOP_REG_NULL && r1 != CEVENTLOOP_REG_NULL;
  }
  if (ok) {
    ssize_t wr = write(c.pfd[1], "x", 1);
    ok = wr == 1;
    for (int i = 0; ok && i < 5000 && !atomic_load(&c.blocker_entered); i++)
      evl_sleep_ms(1);
    ok = ok && atomic_load(&c.blocker_entered);
  }
  bool sent = false;
  if (ok) {
    cmessage m = {.data = &bkl_token, .size = 1};
    sent = ccircq_send(c.cq, &m) == ccol_success;
  }
  pthread_t dt;
  bool dt_started = false;
  if (ok && sent) {
    if (by_destroy) {
      dt_started = pthread_create(&dt, NULL, rlw_destroy_thread, &c) == 0;
      evl_sleep_ms(100);
    } else {
      ceventloop_remove(c.l1, r1);
    }
  }
  atomic_store(&c.blocker_release, true);
  if (dt_started) pthread_join(dt, NULL);
  bool delivered = false;
  if (ok && sent) {
    if (use_select) {
      if (st_started) pthread_join(st, NULL);
      st_started = false;
      delivered = c.sel_rv == ccol_success;
    } else {
      for (int i = 0; i < 3000 && atomic_load(&c.staying_received) == 0; i++)
        evl_sleep_ms(1);
      delivered = atomic_load(&c.staying_received) == 1;
    }
  }
  if (st_started) pthread_join(st, NULL);
  if (r2 != CEVENTLOOP_REG_NULL) ceventloop_remove(l2, r2);
  if (l2 != CEVENTLOOP_NULL) ceventloop_destroy(l2);
  if (c.l1 != CEVENTLOOP_NULL && !(by_destroy && dt_started)) {
    if (rb != CEVENTLOOP_REG_NULL) ceventloop_remove(c.l1, rb);
    ceventloop_destroy(c.l1);
  }
  if (c.cq) {
    cmessage x;
    while (ccircq_try_recv(c.cq, &x) == ccol_success) {
    }
    ccircq_destroy(c.cq);
  }
  close(c.pfd[0]);
  close(c.pfd[1]);
  *setup_ok = ok && sent && (!by_destroy || dt_started) &&
              (by_destroy || atomic_load(&c.leaving_calls) == 0);
  return delivered;
}

// A registration that holds the only wake of a send and leaves without handing
// it on leaves the other listener with the message sitting in the queue.
TEST(ceventloop, a_departing_listener_hands_its_wake_on) {
  bool setup[2][2], got[2][2];
  for (int s = 0; s < 2; s++)
    for (int d = 0; d < 2; d++)
      got[s][d] = rlw_run(s == 1, d == 1, &setup[s][d]);
  for (int s = 0; s < 2; s++) {
    for (int d = 0; d < 2; d++) {
      REQUIRE_TRUE(setup[s][d]);
      REQUIRE_TRUE(got[s][d]);
    }
  }
}

// A queue destroy counts the messages only once no callback can send more
//
// The flags of ifs_on_writable().
typedef struct {
  _Atomic bool entered;
  _Atomic bool gate;
} ifs_ctx;

// The first call sets entered, waits (at most 20 s) for the gate and then sends
// one message, so it is still running when its registration is removed.
static void ifs_on_writable(ceventloop loop, ceventloop_reg reg,
                            cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  ifs_ctx *c = (ifs_ctx *)arg;
  if (atomic_exchange(&c->entered, true)) return;
  for (int i = 0; i < 20000 && !atomic_load(&c->gate); i++) evl_sleep_ms(1);
  cmessage m = {.data = &bkl_token, .size = 1};
  if (sel->type == cselectable_type_ccircq)
    (void)ccircq_try_send(sel->cq, &m);
  else
    (void)cdynmq_send(sel->dq, &m);
}

// Opens the gate after 200 ms, which lets the held callback send while the
// queue's destroy waits for it.
static void *ifs_open_gate(void *arg) {
  evl_sleep_ms(200);
  atomic_store(&((ifs_ctx *)arg)->gate, true);
  return NULL;
}

// Runs in a forked child: Removes the registration while its write callback
// runs and destroys the queue, which must wait for the callback and then count
// its message. It exits 0 when the destroy didn't assert.
static __attribute__((noreturn)) void ifs_child(bool dynq) {
  test_child_no_core();
  int dn = open("/dev/null", O_WRONLY);
  if (dn >= 0) {
    dup2(dn, STDOUT_FILENO);
    dup2(dn, STDERR_FILENO);
    close(dn);
  }
  alarm(10);
  ifs_ctx c;
  atomic_init(&c.entered, false);
  atomic_init(&c.gate, false);
  ccircq *cq = dynq ? NULL : ccircq_create(4, NULL);
  cdynmq *dq = dynq ? cdynmq_create(NULL) : NULL;
  if (!cq && !dq) test_child_exit(2);
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  if (loop == CEVENTLOOP_NULL) test_child_exit(2);
  ceventloop_handlers h = {.on_writable = ifs_on_writable};
  cselectable sel = dynq ? cdynmq_selectable(dq, cselect_write)
                         : ccircq_selectable(cq, cselect_write);
  ceventloop_reg reg = ceventloop_add(loop, sel, h, &c, NULL);
  if (reg == CEVENTLOOP_REG_NULL) test_child_exit(2);
  for (int i = 0; i < 5000 && !atomic_load(&c.entered); i++) evl_sleep_ms(1);
  if (!atomic_load(&c.entered)) test_child_exit(2);
  if (ceventloop_remove(loop, reg) != ccol_success) test_child_exit(2);
  pthread_t gt;
  if (pthread_create(&gt, NULL, ifs_open_gate, &c) != 0) test_child_exit(2);
  if (dynq)
    cdynmq_destroy(dq);
  else
    ccircq_destroy(cq);
  test_child_exit(0);
}

// A destroy that counts the messages before it waits for the callback sees an
// empty queue, and the child exits 0.
TEST(circular_queues, destroy_counts_a_message_sent_by_a_running_callback) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pids[2];
  for (int k = 0; k < 2; k++) {
    pids[k] = fork();
    if (pids[k] == 0) ifs_child(k == 1);
  }
  int status[2] = {0, 0};
  bool reaped[2] = {false, false};
  for (int k = 0; k < 2; k++)
    if (pids[k] > 0)
      reaped[k] = _wait_for_forked_child_bounded(pids[k], &status[k], 20000);
  for (int k = 0; k < 2; k++) {
    REQUIRE_NE(pids[k], -1);
    REQUIRE_TRUE(reaped[k]);
    REQUIRE_TRUE(WIFSIGNALED(status[k]));
    REQUIRE_EQ(WTERMSIG(status[k]), SIGABRT);
  }
}

// The documented refusals of a send, and of an fd that epoll can't watch
TEST(dynamic_queues, send_refuses_a_message_whose_data_and_size_disagree) {
  cdynmq *dq = cdynmq_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  cmessage no_data = {.data = NULL, .size = 1};
  ccol_retval rv_no_data = cdynmq_send(dq, &no_data);
  cmessage no_size = {.data = &bkl_token, .size = 0};
  ccol_retval rv_no_size = cdynmq_send(dq, &no_size);
  cmessage sentinel = {.data = NULL, .size = 0};
  ccol_retval rv_sentinel = cdynmq_send(dq, &sentinel);
  size_t count = cdynmq_msg_count(dq);
  cmessage m;
  while (cdynmq_try_recv(dq, &m) == ccol_success) {
  }
  cdynmq_destroy(dq);
  REQUIRE_EQ(rv_no_data, ccol_invalid_args);
  REQUIRE_EQ(rv_no_size, ccol_invalid_args);
  REQUIRE_EQ((void *)no_size.data, (void *)&bkl_token);
  REQUIRE_EQ(rv_sentinel, ccol_success);
  REQUIRE_EQ(count, (size_t)1);
}

// A live pointer with size 0 and a NULL pointer with a size above 0 are
// refused, leaving the message as it was, while {NULL, 0} goes through. The
// channel sends run on the owner thread, so they land in the owner-to-workers
// queue.
TEST(circular_queues,
     every_send_refuses_a_message_whose_data_and_size_disagree) {
  ccircq *cq = ccircq_create(8, NULL);
  cchan *ch = cchan_create(8, NULL);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_NE((void *)ch, NULL);
  cmessage bad[2] = {{.data = &bkl_token, .size = 0},
                     {.data = NULL, .size = 1}};
  ccol_retval rv[2][6];
  bool unchanged = true;
  for (int k = 0; k < 2; k++) {
    cmessage m = bad[k];
    rv[k][0] = ccircq_send(cq, &m);
    rv[k][1] = ccircq_try_send(cq, &m);
    rv[k][2] = ccircq_timed_send(cq, &m, 1000);
    rv[k][3] = cchan_send(ch, &m);
    rv[k][4] = cchan_try_send(ch, &m);
    rv[k][5] = cchan_timed_send(ch, &m, 1000);
    if (m.data != bad[k].data || m.size != bad[k].size) unchanged = false;
  }
  size_t cq_count = ccircq_msg_count(cq);
  cmessage sentinel = {.data = NULL, .size = 0};
  ccol_retval rv_sentinel = ccircq_try_send(cq, &sentinel);
  cmessage out;
  while (ccircq_try_recv(cq, &out) == ccol_success) {
  }
  ccircq_destroy(cq);
  cchan_destroy(ch);

  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 6; j++) REQUIRE_EQ(rv[k][j], ccol_invalid_args);
  REQUIRE_TRUE(unchanged);
  REQUIRE_EQ(cq_count, (size_t)0);
  REQUIRE_EQ(rv_sentinel, ccol_success);
}

// epoll(7) refuses a regular file, so cselect reports ccol_unexpected_failure
// and ceventloop_add reports a failed system call.
TEST(cselect, a_regular_file_fd_is_refused) {
  char path[] = "cselect_regular_file_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_GE(fd, 0);
  unlink(path);
  size_t idx = 12345;
  cselectable sel = cselectable_fd(fd, cselect_read);
  ccol_retval rv_select = cselect_timed(&idx, 1, &sel, 0);
  ccol_retval rv_select_block = cselect(&idx, 1, &sel);
  ceventloop loop = ceventloop_create(8, 1, 1, NULL);
  ceventloop_reg reg = CEVENTLOOP_REG_NULL;
  char *err = NULL;
  if (loop != CEVENTLOOP_NULL) {
    ceventloop_handlers h = {.on_readable = rlw_on_leaving};
    reg = ceventloop_add(loop, sel, h, NULL, &err);
    if (reg != CEVENTLOOP_REG_NULL) ceventloop_remove(loop, reg);
    ceventloop_destroy(loop);
  }
  close(fd);
  REQUIRE_EQ(rv_select, ccol_unexpected_failure);
  REQUIRE_EQ(rv_select_block, ccol_unexpected_failure);
  REQUIRE_EQ(idx, (size_t)12345);
  REQUIRE_EQ(reg, CEVENTLOOP_REG_NULL);
  REQUIRE_NE((void *)err, NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "a system call failed"), NULL);
}
