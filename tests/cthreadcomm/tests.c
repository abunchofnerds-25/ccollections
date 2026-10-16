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
#include <sys/socket.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_fds.h>
#include <test_sanitizer.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

/* Every write(2) call in this file writes a small signal of a few bytes into
 * a pipe that the same test already owns (a result fd or a notify fd). Each
 * write has one fixed size and is best-effort, and none of them is a bulk
 * transfer a partial write could damage, so the only failure that needs a
 * retry loop is EINTR, as in _eventfd_notify() of src/cthreadcomm.c. This
 * function needs to really consume the return value, a bare (void) cast
 * isn't enough: the fortified write(2) wrapper of _FORTIFY_SOURCE marks
 * itself warn_unused_result, and a (void) cast doesn't reliably silence it. */
static void test_write_retry_eintr(int fd, const void *buf, size_t n) {
  ssize_t rv;
  do {
    rv = write(fd, buf, n);
  } while (rv < 0 && errno == EINTR);
}

/* The conversion of a microsecond timeout into an absolute deadline that
 * every timed call of this module uses. We write the expected values out by
 * hand instead of computing them with the arithmetic under test. */
TEST(ccol_timespec_add_us, carries_whole_seconds_and_nanoseconds) {
  {
    struct timespec t = {.tv_sec = 1, .tv_nsec = 600000000};
    ccol_timespec_add_us(&t, 2400000); /* 2.4 s */
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
    ccol_timespec_add_us(&t, 1); /* exactly one carry */
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

/* A deadline that has passed gives a poll timeout of 0, which still takes
 * one look, and a deadline a fraction of a millisecond ahead gives 1, never
 * 0. A deadline at the end of time gives INT_MAX without overflowing. */
TEST(ccol_deadline_remaining_ms_ceil, rounds_up_and_saturates) {
  struct timespec past;
  clock_gettime(CLOCK_MONOTONIC, &past);
  past.tv_sec -= 1;
  REQUIRE_EQ(ccol_deadline_remaining_ms_ceil(&past), 0);

  struct timespec soon;
  REQUIRE_TRUE(ccol_deadline_after_us(500, &soon));
  int ms = ccol_deadline_remaining_ms_ceil(&soon);
  /* 1 while the deadline is ahead, 0 once a descheduled thread passed it */
  REQUIRE_LE(ms, 1);

  struct timespec later;
  REQUIRE_TRUE(ccol_deadline_after_us(10000000, &later)); /* 10 s */
  ms = ccol_deadline_remaining_ms_ceil(&later);
  REQUIRE_GT(ms, 9000);
  REQUIRE_LE(ms, 10000);

  struct timespec never;
  REQUIRE_TRUE(ccol_deadline_after_us(UINT64_MAX, &never));
  REQUIRE_EQ(ccol_deadline_remaining_ms_ceil(&never), INT_MAX);
}

// CIRCULAR_QUEUE TESTS

TEST(circular_queues, create_fails) {
  char *err_str = NULL;

  ccol_circular_queue *cq = ccol_circular_queue_create_mp(0, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccol_circular_queue_create_mp(-1, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccol_circular_queue_create_mp(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

/* verify_circular_queue_create_inputs guards against
 * max_size > SIZE_MAX / sizeof(c_message_t). Without that guard a max_size
 * in about [ccol_max_elem_count / 16, ccol_max_elem_count], the top part of
 * the documented valid range "1 to ccol_max_elem_count", is dangerous: for
 * such a max_size, max_size * sizeof(c_message_t) wraps around size_t, so
 * the queue thinks it has room for max_size messages while the real
 * allocation of msg_array is tiny (for max_size == 2^60 it's exactly 0
 * bytes). The first send then corrupts the heap, which AddressSanitizer
 * reports directly. Both values the test below refuses are well inside
 * ccol_max_elem_count, so only the overflow guard can refuse them and not
 * the max_size > ccol_max_elem_count check. The test passes NULL for
 * mmgmt_procs, which selects the default allocator, and the guard must
 * refuse both values before calling malloc(3) at all. */
TEST(circular_queues,
     create_rejects_max_size_that_would_overflow_the_backing_array_size) {
  char *err_str = NULL;

  size_t smallest_overflowing = SIZE_MAX / sizeof(c_message_t) + 1;
  REQUIRE_LT(smallest_overflowing, ccol_max_elem_count);
  ccol_circular_queue *cq =
      ccol_circular_queue_create_mp(smallest_overflowing, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  /* The exact value that wraps max_size * sizeof(c_message_t) to exactly 0,
   * and without the guard the first send then corrupts the heap. It's well
   * inside ccol_max_elem_count, but it can only be built where size_t is
   * wider than 32 bits. With a 32-bit size_t (i386, for example)
   * `(size_t)1 << 60` shifts by more than the width of the type, which is
   * undefined behavior and a compile error under
   * -Werror=shift-count-overflow, reported even for a branch that never
   * runs, so this half of the test has a compile-time #if guard rather than
   * a run-time skip. smallest_overflowing above covers the same overflow
   * guard on every platform and every size_t width, so a 32-bit build loses
   * no coverage, this second case just adds one more data point that needs a
   * wider size_t. */
#if SIZE_MAX > 0xFFFFFFFFu
  size_t known_bad = (size_t)1 << 60;
  REQUIRE_LT(known_bad, ccol_max_elem_count);
  err_str = NULL;
  cq = ccol_circular_queue_create_mp(known_bad, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
#endif
}

/* The create call should accept a large max_size when
 * max_size * sizeof(c_message_t) is a big allocation but nowhere near an
 * overflow, so the overflow guard isn't too strict.
 * create_succeeds_with_large_elem_size in tests/cvector/tests.c is the
 * precedent for the same class of guard. */
TEST(circular_queues, create_succeeds_with_large_non_overflowing_max_size) {
  char *err_str = NULL;
  size_t large_max_size = 1024 * 1024; /* 16 MiB of c_message_t slots */
  ccol_circular_queue *cq =
      ccol_circular_queue_create_mp(large_max_size, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, create_and_destroy_no_mem_procs) {
  char *err_str = "";

  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, create_and_destroy_with_mem_procs) {
  char *err_str = "";

  ccol_circular_queue *cq = ccol_circular_queue_create_mp(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, basic_send_and_receive_no_mem_procs) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_with_mem_procs) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, msg_count) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(ccol_circq_msg_count(cq), i);
    ccol_circq_send_zc(cq, &m1);
    REQUIRE_EQ(ccol_circq_msg_count(cq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(ccol_circq_msg_count(cq), i);
    ccol_circq_recv_zc(cq, &m1);
    REQUIRE_EQ(ccol_circq_msg_count(cq), i - 1);
  }

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_NULL_msg) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = (void *)0xabcdef01, .size = 0x35};
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);
  REQUIRE_EQ(m2.size, 0);

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, try_send_and_try_receive) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccol_circular_queue_destroy(cq);
}

#define getWallTime(A) clock_gettime(CLOCK_REALTIME, &A);
#define diffTimeUSec(A, B) \
  (B.tv_sec - A.tv_sec) * 1000000 + (B.tv_nsec - A.tv_nsec) / 1000

TEST(circular_queues, timed_send_and_timed_receive) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  uint64_t timeout = 100000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m1, timeout), ccol_success);
  getWallTime(after);
  /* The call doesn't wait. The bound is loose for a loaded or emulated
   * machine, and a call that waits for the timeout fails the result check. */
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m1, timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m2, timeout), ccol_success);
  getWallTime(after);
  /* The call doesn't wait. The bound is loose for a loaded or emulated
   * machine, and a call that waits for the timeout fails the result check. */
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m1, timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, enable_disable_sending) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  ccol_circq_disable_sending(cq);

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m1, 1000000), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  /* A timeout of 0 is the try variant and reports the same refusal. */
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m1, 0), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  ccol_circq_enable_sending(cq);

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, ring_wrap_around) {
  // The capacity is 2, so five sends and five receives make the head and
  // tail indices wrap around the ring boundary more than once.
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(2, NULL, NULL);

  for (int i = 0; i < 5; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = 'A' + i;
    c_message_t out = {.data = buf, .size = 1};
    REQUIRE_EQ(ccol_circq_send_zc(cq, &out), ccol_success);
    REQUIRE_EQ(out.data, NULL);

    c_message_t in = {.data = NULL, .size = 0};
    REQUIRE_EQ(ccol_circq_recv_zc(cq, &in), ccol_success);
    REQUIRE_EQ(*(char *)in.data, 'A' + i);
    free(in.data);
  }

  ccol_circular_queue_destroy(cq);
}

/* A timeout of 0 doesn't wait, so each timed call answers exactly what its
 * try variant answers, ccol_container_full or ccol_container_empty, never
 * ccol_timed_out. A timed call that builds a deadline of "now" for a 0 and
 * waits on it returns ccol_timed_out, so it fails here. */
TEST(circular_queues, timed_calls_with_a_zero_timeout_are_the_try_variants) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  struct timespec before, after;
  getWallTime(before);
  c_message_t recv_m = {.data = NULL, .size = 0};
  ccol_retval_t empty_rv = ccol_circq_timed_recv_zc(cq, &recv_m, 0);

  c_message_t m = {.data = malloc(sizeof(char)), .size = 1};
  ccol_retval_t first_rv = ccol_circq_timed_send_zc(cq, &m, 0);
  c_message_t m2 = {.data = malloc(sizeof(char)), .size = 1};
  ccol_retval_t full_rv = ccol_circq_timed_send_zc(cq, &m2, 0);
  void *kept = m2.data;
  getWallTime(after);

  c_message_t got = {.data = NULL, .size = 0};
  ccol_retval_t got_rv = ccol_circq_timed_recv_zc(cq, &got, 0);
  free(got.data);
  free(m2.data);
  ccol_retval_t null_rv = ccol_circq_timed_recv_zc(NULL, &got, 0);
  ccol_circular_queue_destroy(cq);

  REQUIRE_EQ(empty_rv, ccol_container_empty);
  REQUIRE_EQ(first_rv, ccol_success);
  REQUIRE_EQ(full_rv, ccol_container_full);
  REQUIRE_TRUE(kept != NULL); /* the caller keeps ownership on a failure */
  REQUIRE_EQ(got_rv, ccol_success);
  REQUIRE_EQ(null_rv, ccol_invalid_args);
  /* No call waited. The bound is loose for a loaded or emulated machine. */
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
}

TEST(circular_queues, timed_send_reports_unexpected_failure_on_condvar_error) {
  /* Regression test. When _ccol_cond_var_timedwait returns something other
   * than ETIMEDOUT, the wait loop of ccol_circq_timed_send_zc must not
   * always report ccol_unexpected_failure, it has to check first whether
   * space became free. The racing test below covers that second check and
   * this one covers the ordinary case where the queue is still full. A
   * forced error with no race still gives ccol_unexpected_failure,
   * because the loop must neither hide the error nor retry for ever. The test
   * sets a large timeout and a tight bound on the elapsed time, which proves
   * the call returns quickly on the forced error. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(1), .size = 1};
  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &filler), ccol_success);

  _ccol_circq_test_force_next_send_condvar_wait_error();

  c_message_t m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t send_rv = ccol_circq_timed_send_zc(cq, &m, timeout);
  int send_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(send_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL and errno carries it to the caller. */
  REQUIRE_EQ(send_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_NE(m.data, NULL);  // the caller keeps ownership on a failure
  free(m.data);

  c_message_t drained = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &drained), ccol_success);
  free(drained.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_send_condvar_error_racing_freed_slot_still_sends) {
  /* The same class of failure as _sel_wait_condvar of ccol_select (see
   * ccol_select.timed_wait_ready_racing_condvar_error_still_succeeds): the
   * FAILURE branch of ccol_circq_timed_send_zc must not always report
   * ccol_unexpected_failure, because a consumer on another thread can free a
   * slot at the same instant. _ccol_cond_var_timedwait always locks
   * cq->mutex again before returning, on success and on failure, so this
   * order of events can happen in production. The dedicated test hook makes
   * it deterministic, since a real consumer thread can't race into this
   * exact window on demand while the hook replaces _ccol_cond_var_timedwait
   * without unlocking cq->mutex. Without that handling the test fails with
   * ccol_unexpected_failure and the message is never sent. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(sizeof(int)), .size = sizeof(int)};
  *(int *)filler.data = 42;
  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &filler), ccol_success);

  _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready();

  c_message_t m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the queue takes ownership on success

  c_message_t raced_out = _ccol_circq_test_take_race_freed_msg();
  REQUIRE_NE(raced_out.data, NULL);
  REQUIRE_EQ(*(int *)raced_out.data, 42);
  free(raced_out.data);

  REQUIRE_EQ(ccol_circq_msg_count(cq), (size_t)1);
  c_message_t recv_m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &recv_m), ccol_success);
  free(recv_m.data);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  /* The receive-side mirror of
   * timed_send_reports_unexpected_failure_on_condvar_error: a forced error
   * on an empty queue with no race gives ccol_unexpected_failure, and
   * quickly. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);

  _ccol_circq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t recv_rv = ccol_circq_timed_recv_zc(cq, &m, timeout);
  int recv_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL and errno carries it to the caller. */
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* Regression test, the receive-side version of
   * timed_send_condvar_error_racing_freed_slot_still_sends. A producer on
   * another thread can complete a send at the same instant the test forces
   * an unrelated _ccol_cond_var_timedwait error, and the receive must return
   * that message instead of throwing it away and reporting
   * ccol_unexpected_failure. Without that handling the test fails with
   * ccol_unexpected_failure and the sentinel message the hook put into the
   * queue is stuck there for ever: msg_count stays at 1 and nothing can
   * receive the message, because the caller has already given up. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  _ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // overwritten on success
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel from the hook
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(ccol_circq_msg_count(cq), (size_t)0);

  ccol_circular_queue_destroy(cq);
}

/* ------------------------------------------------------------------ */
/* The operation that makes progress possible has to release a blocked
 * waiter, the waiter must not wait for its own timeout.
 *
 * Both queues signal a condition variable only when the matching waiter
 * count says somebody is parked on it, so a wait site that doesn't register
 * itself leaves a real waiter invisible and the queue skips the signal. The
 * symptom is a delay rather than a wrong answer: the waiter sleeps until its
 * own timeout, checks again and returns the message that was already there.
 * These tests tell the two cases apart in two ways. The wait's timeout is
 * several times longer than the bound the test then allows for the wake,
 * and the test polls the waiter count and runs the wake-up operation only
 * once the waiter has really parked, so a missing registration fails the
 * poll as well as the timing bound.
 *
 * Remove either the increment or the decrement around any of the three
 * timed wait sites and the matching test fails.
 */

#define CCOL_WAKE_WAIT_TIMEOUT_SEC 10    /* the blocked call's own timeout */
#define CCOL_WAKE_PARK_BOUND_US 5000000L /* to observe the waiter parked */
#define CCOL_WAKE_WAKE_BOUND_US 3000000L /* to observe it released again */

typedef struct {
  ccol_circular_queue *cq;
  ccol_dynamic_queue *dq;
  c_message_t msg;
  ccol_retval_t retval;
  atomic_bool finished;
} ccol_wake_args;

static void *_wake_circq_timed_recv(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = ccol_circq_timed_recv_zc(a->cq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

static void *_wake_circq_timed_send(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = ccol_circq_timed_send_zc(a->cq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

static void *_wake_dynmq_timed_recv(void *arg) {
  ccol_wake_args *a = (ccol_wake_args *)arg;
  uint64_t t = (uint64_t)CCOL_WAKE_WAIT_TIMEOUT_SEC * 1000000u;
  a->retval = ccol_dynmq_timed_recv_zc(a->dq, &a->msg, t);
  atomic_store(&a->finished, true);
  return NULL;
}

/* Bounded poll helpers. Every wait here has a deadline, so a regression
 * fails the run instead of hanging it. */
static void _wake_poll_pause(void) {
  struct timespec s = {.tv_sec = 0, .tv_nsec = 200000}; /* 0.2 ms */
  nanosleep(&s, NULL);
}

static bool _wake_await_circq_readers(ccol_circular_queue *cq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (_ccol_circq_waiting_readers_for_tests(cq) >= 1) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_circq_writers(ccol_circular_queue *cq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (_ccol_circq_waiting_writers_for_tests(cq) >= 1) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_dynmq_readers(ccol_dynamic_queue *dq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (_ccol_dynmq_waiting_readers_for_tests(dq) >= 1) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_finished(ccol_wake_args *a) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (atomic_load(&a->finished)) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_WAKE_BOUND_US) return false;
    _wake_poll_pause();
  }
}

TEST(circular_queues, blocked_timed_recv_is_released_by_a_send) {
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
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
  ccol_retval_t sent = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_circq_readers(cq);
    c_message_t m = {.data = payload, .size = 4};
    sent = ccol_circq_send_zc(cq, &m);
    if (sent != ccol_success) free(payload);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    matched = (a.msg.data == payload);
    free(a.msg.data);
  } else {
    free(payload);
  }

  /* Drains the queue before the destroy below. A receiver that comes back
     empty, the regression this test looks for, leaves the message in the
     queue, and destroying a queue that still holds messages is a caller error
     the library aborts on, which would stop the whole binary instead of
     failing the assertions below. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  /* Every waiter has returned, so the queue gives the counter back. */
  size_t residual = _ccol_circq_waiting_readers_for_tests(cq);
  ccol_retval_t got = a.retval;
  ccol_circular_queue_destroy(cq);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(sent, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(matched);
  REQUIRE_EQ(residual, (size_t)0);
}

TEST(circular_queues, blocked_timed_send_is_released_by_a_receive) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);

  void *first = malloc(4);
  c_message_t filler = {.data = first, .size = 4};
  ccol_retval_t filled = ccol_circq_send_zc(cq, &filler);
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
  c_message_t out = {.data = NULL, .size = 0};
  ccol_retval_t recvd = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_circq_writers(cq);
    recvd = ccol_circq_recv_zc(cq, &out);
    if (recvd == ccol_success) free(out.data);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    /* The message in the queue is the released sender's. */
    c_message_t tail = {.data = NULL, .size = 0};
    drained = (ccol_circq_try_recv_zc(cq, &tail) == ccol_success);
    matched = (tail.data == payload);
    free(tail.data);
  } else {
    free(payload);
  }

  /* Drains the queue before the destroy below, for the same reason as
     blocked_timed_recv_is_released_by_a_send. On the path where no thread
     starts the filler is still in the queue, and on the path this test
     looks for a sender that times out leaves its own message behind.
     Destroying a queue that still holds messages is a caller error the
     library aborts on, which would stop the whole binary instead of failing
     the assertions below. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = _ccol_circq_waiting_writers_for_tests(cq);
  ccol_retval_t got = a.retval;
  ccol_circular_queue_destroy(cq);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
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
  ccol_retval_t sent = ccol_unexpected_failure;

  if (started) {
    parked = _wake_await_dynmq_readers(dq);
    c_message_t m = {.data = payload, .size = 4};
    sent = ccol_dynmq_send_zc(dq, &m);
    if (sent != ccol_success) free(payload);
    released = _wake_await_finished(&a);
    pthread_join(tid, NULL);
    matched = (a.msg.data == payload);
    free(a.msg.data);
  } else {
    free(payload);
  }

  /* blocked_timed_recv_is_released_by_a_send explains why we drain first. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_dynmq_try_recv_zc(dq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = _ccol_dynmq_waiting_readers_for_tests(dq);
  ccol_retval_t got = a.retval;
  ccol_dynamic_queue_destroy(dq);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(sent, ccol_success);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(matched);
  REQUIRE_EQ(residual, (size_t)0);
}

TEST(circular_queues, recv_drains_successfully_after_disable) {
  // A receive still gets the messages already in the queue after the test
  // turns sending off. Once the queue is empty a timed recv should time out
  // and not return ccol_not_permitted, because the off state only affects
  // senders.
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(2, NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);

  ccol_circq_disable_sending(cq);

  // The messages already there must drain successfully.
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m), ccol_success);
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m), ccol_success);

  // The queue is empty and sending is off, so a timed recv times out
  // and not return ccol_not_permitted.
  uint64_t timeout = 50000;  // 50 ms
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m, timeout), ccol_timed_out);

  ccol_circular_queue_destroy(cq);
}

typedef struct {
  ccol_circular_queue *cq;
  ccol_retval_t result;
} cq_disable_recv_args;

void *cq_blocking_recv_thread(void *raw) {
  cq_disable_recv_args *a = (cq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = ccol_circq_recv_zc(a->cq, &m);
  return NULL;
}

TEST(circular_queues, disable_sending_does_not_unblock_recv) {
  // ccol_circq_disable_sending must NOT wake a thread blocked in
  // ccol_circq_recv_zc. That thread stays blocked and returns only after the
  // test turns sending back on and a message arrives.
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  cq_disable_recv_args args = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  ccol_circq_disable_sending(cq);
  usleep(20000);  // receiver must still be blocked at this point

  // Turn sending back on and send a message, which releases the receiver.
  ccol_circq_enable_sending(cq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_times_out_when_disabled) {
  // ccol_circq_timed_recv_zc is expected to return ccol_timed_out, not
  // ccol_not_permitted, when the queue is empty and sending is off, since the
  // off state must not make the call return before the deadline.
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  ccol_circq_disable_sending(cq);

  uint64_t timeout = 100000;  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m, timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  ccol_circular_queue_destroy(cq);
}

void *cq_helper_thread(void *args) {
  ccol_circular_queue *cq = (ccol_circular_queue *)args;
  // Makes the sender block on the second message.
  usleep(50000);

  c_message_t m = {.data = NULL, .size = 0};
  assert(ccol_circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  assert(ccol_circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'B');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(circular_queues, send_and_receive_thread) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_helper_thread, cq), 0);

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  m = (c_message_t){.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'B';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  ccol_circular_queue_destroy(cq);
}

/* Reads /proc/<pid>/<name>, a short pseudo-file, into buf, NUL-terminated,
 * and returns true on success. It's only a best-effort diagnostic, so it
 * returns false on any failure, a process the kernel has already reaped or a
 * permission problem for example, and the caller prints that false result as
 * part of the dump rather than treating it as fatal. It only runs after a
 * bounded wait has timed out, so it must never add a new way to hang or
 * crash the test binary. */
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

/* Diagnostics only. Prints what the host kernel's /proc still says about pid
 * and about each thread of pid after a bounded wait for pid has timed out;
 * the doc comment of _wait_for_forked_child_bounded explains why every
 * parent-side waitpid() in this file has a bound. It reads real files with
 * plain, buffered stdio and read() calls and deliberately uses nothing that
 * is safe inside a signal handler, because it only runs on a path that has
 * already failed, long after the timing window that mattered has closed, so
 * extra disturbance costs nothing.
 *
 * The State: line of /proc/<pid>/status is the most useful field here. Z
 * (zombie) means the child really exited and the kernel is only waiting for
 * a reap, which points at a bug in how waitpid() sees this under emulation
 * and not at anything the child did. D, R or S with a real
 * /proc/<pid>/syscall entry means the child is alive and stuck at a place you
 * can name, and one of the child's threads can be stuck the same way. The
 * function reads status, wchan, syscall and stat one at a time, so one file
 * that's missing or unreadable doesn't hide the others; wchan, for example,
 * needs a kernel config option the running kernel may not have. */
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

/* A bounded replacement for a blocking waitpid(pid, status_out, 0). It polls
 * with WNOHANG for up to timeout_ms of wall-clock time, returns true and
 * sets *status_out as soon as the kernel reaps pid, and returns false if
 * timeout_ms passes first. A qemu-user hang can make a forked child's alarm()
 * bound useless: the child completes, and may even abort visibly with
 * SIGABRT and a core dump, but the parent's waitpid() never returns. That's
 * why the parent side gets a bound here and why _dump_stuck_child_diagnostics,
 * this function's sibling, then prints the diagnosis from the parent rather
 * than from inside the child. A child-side wait without a bound still has its
 * own alarm(), but a parent-side one has nothing to fall back on and can hang
 * this whole test binary, and the CI job running it, for ever, however well
 * bounded every forked child in this file is. On a timeout this function
 * prints the diagnostics and then makes a best-effort attempt to SIGKILL and
 * reap pid, since otherwise a child that's only slow stays alive as an orphan
 * after this test ends. That reap attempt is bounded too, and its outcome
 * doesn't change the false return, because the timeout already establishes
 * the failure this test reports. */
static bool _wait_for_forked_child_bounded(pid_t pid, int *status_out,
                                           int timeout_ms) {
  enum { POLL_INTERVAL_MS = 50 };
  int elapsed_ms = 0;
  while (elapsed_ms < timeout_ms) {
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid) return true;
    if (r == -1) return false; /* for example ECHILD: nothing to wait for */
    struct timespec ts = {.tv_sec = POLL_INTERVAL_MS / 1000,
                          .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    elapsed_ms += POLL_INTERVAL_MS;
  }

  _dump_stuck_child_diagnostics(pid);

  kill(pid, SIGKILL);
  for (int i = 0; i < 20; i++) { /* up to 1s, best-effort only */
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid || r == -1) break;
    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);
  }
  return false;
}

/* Every caller of the helper below sits inside the
 * "#if CCOL_FORK_SAFETY_REQUIRED" block further down in this file, so a build
 * with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles that block out and leaves
 * the helper with no caller. This file builds with -Werror, so
 * -Wunused-function would turn that into a hard build failure for a
 * configuration the library documents as supported. The guard keeps the
 * helper and its callers on the same switch, and it isn't a suppression: a
 * helper that loses its LAST caller in the default build is still an error.
 */
#if CCOL_FORK_SAFETY_REQUIRED
/* A bounded alternative to a plain blocking read() on a forked child's
 * result pipe. Every fork_safety test below that uses a result pipe relies on
 * the forked child's alarm() to close the pipe's write end, which happens in
 * one of two ways: the child writes the result byte and exits normally, or
 * the default SIGALRM disposition stops it on a hang. That alarm is the only
 * bound a bare read() there would have. A qemu-user binary-translation lock
 * that stays locked across the guest fork() can wedge the forked child's
 * itimer and signal delivery along with everything else the child depends
 * on, which switches the alarm bound off, and then the write end stays open
 * for ever with nothing written and nothing closed. Bounding the read itself
 * with poll() closes that gap without relying on the child's alarm(), for the
 * same reason as _wait_for_forked_child_bounded above: this file never trusts
 * a parent-side wait without a bound. Within timeout_ms it returns what
 * read() would, 1 for a byte it really read and 0 for a real EOF, and on a
 * timeout it also returns 0, as if it saw EOF, so every REQUIRE_EQ((int)n, 1)
 * call site below fails that one test cleanly instead of hanging the whole
 * binary. */
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
    if (remaining_ms <= 0) return 0; /* a timeout, treated like EOF */

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r = poll(&pfd, 1, (int)remaining_ms);
    if (r == 0) return 0; /* a timeout, treated like EOF */
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    return read(fd, out_byte, 1);
  }
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* Destroying a queue that still has a linked ccol_select() or
 * ccol_event_loop waiter is an error, because that waiter's node then holds
 * a dangling pointer into the queue's mutex, which the destroy frees. The
 * msg_count > 0 check can't catch this since it only sees the messages left
 * in the queue, so both __ccol_circular_queue_destroy and
 * __ccol_dynamic_queue_destroy assert when sel_read_waiters_head or
 * sel_write_waiters_head isn't NULL. It runs in a forked child because the
 * _ccol_assert() and the abort() after it stop the whole process. */
TEST(circular_queues, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's lifetime, so an unexpected hang here fails this one
     * test loudly and fast: the default SIGALRM disposition stops the
     * process, and the parent's bounded wait below sees WIFSIGNALED with
     * SIGALRM instead of SIGABRT. This bound is separate from the parent's
     * bound below. */
    alarm(10);
    ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
    if (!cq) _exit(2);
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
    ccol_event_handlers_t handlers = {0};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, NULL,
        &err);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
    /* The misuse under test: nothing removes the registration above and
     * nothing destroys the loop before this call. */
    ccol_circular_queue_destroy(cq);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* 20s is much longer than the child's alarm(10), which leaves the parent
   * time to see the child and reap it, even under emulation. The doc comment
   * of _wait_for_forked_child_bounded explains why the parent needs a bound
   * of its own and can't rely on the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/*
 * A shut-down loop takes no wake from a queue it watches, but its
 * registration names the queue until a remove or the loop's destroy, so
 * destroying the queue before that is the same misuse as above and has to
 * stop the process, for both queue kinds. Without the check the destroy
 * frees the queue and the later destroy of the loop locks a mutex inside
 * freed memory.
 *
 * A destroy that looks only at the waiter lists, which the shutdown emptied,
 * lets the child exit 0 instead of with SIGABRT.
 */
TEST(ccol_event_loop,
     queue_destroy_after_shutdown_with_a_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int signals[2] = {0, 0};
  bool reaped_all = true;
  for (int kind = 0; kind < 2; kind++) {
    pid_t pid = fork();
    if (pid == 0) {
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      alarm(10);
      ccol_circular_queue *cq = NULL;
      ccol_dynamic_queue *dq = NULL;
      if (kind == 0)
        cq = ccol_circular_queue_create(4, NULL);
      else
        dq = ccol_dynamic_queue_create(NULL);
      if (!cq && !dq) _exit(2);
      ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
      if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
      ccol_event_handlers_t handlers = {0};
      ccol_selectable sel =
          cq ? ccol_selectable_from_circq(cq, ccol_select_read)
             : ccol_selectable_from_dynq(dq, ccol_select_read);
      ccol_event_reg reg = ccol_event_loop_add(loop, sel, handlers, NULL, NULL);
      if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
      if (ccol_event_loop_shutdown(loop) != ccol_success) _exit(2);
      if (cq)
        ccol_circular_queue_destroy(cq);
      else
        ccol_dynamic_queue_destroy(dq);
      _exit(0); /* not reachable when the assert fires, as it must */
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

typedef struct cq_select_waiter_args {
  ccol_circular_queue *cq;
} cq_select_waiter_args;

static void *cq_select_waiter_thread(void *arg) {
  cq_select_waiter_args *a = (cq_select_waiter_args *)arg;
  size_t idx;
  /* This timeout is long enough that the destroy call on the main thread,
   * which races this call, always ends the process first, so this call
   * never gets to return. */
  ccol_select_timed_va(&idx, 5000000,
                       ccol_selectable_from_circq(a->cq, ccol_select_read));
  return NULL;
}

TEST(circular_queues, destroy_while_ccol_select_is_watching_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    /* Bounds this child's lifetime so an unexpected hang here fails this one
     * test loudly and fast, where without the bound it would stop the whole
     * test binary, and the CI job running it, for ever. The same alarm in the
     * sibling test above gives the whole reason, and the doc comment of
     * _wait_for_forked_child_bounded explains why the parent below has its
     * own independent bound. */
    alarm(10);
    ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
    if (!cq) _exit(2);

    cq_select_waiter_args wargs = {.cq = cq};
    pthread_t waiter;
    /* An if-guard instead of REQUIRE_EQ, because this runs inside the forked
     * child above (pid == 0). The failure path of REQUIRE_EQ is an early
     * `return` out of this Tau test function, which would leave THIS test
     * function while the process is still the forked child, skip the exit
     * calls below and fall into the harness's test-running loop a second
     * time, although this process must only run this one child-side branch.
     * The same guard and comment on
     * fork_does_not_deadlock_with_queue_registered_before_event_loop, lower
     * down in this file, explain the reasoning. The poll loop below already
     * bounds a failed create here, since linked never becomes true when no
     * thread links it, but exiting at once with a distinct code puts the
     * real cause in the exit status. Otherwise it shows up 2 seconds later as
     * a REQUIRE_TRUE(linked) failure you can't tell from any other. */
    if (pthread_create(&waiter, NULL, cq_select_waiter_thread, &wargs) != 0)
      _exit(4);

    /* Polls until the waiter thread has REALLY linked itself into cq's
     * read-waiter list, which is real synchronization on the condition this
     * test needs instead of a fixed sleep that guesses at it. A return from
     * pthread_create() promises neither that the scheduler has run the new
     * thread at all nor that the thread got to its first _ccol_mutex_lock
     * and link step inside Phase 1 of ccol_select_timed. A fixed-sleep
     * hand-off here loses this race under qemu-user emulation, where thread
     * start latency is much higher and much more variable. The destroy()
     * then runs first, correctly sees no linked waiters, has nothing to
     * catch and frees cq, and the late waiter thread dereferences that freed
     * memory as soon as the scheduler runs it. That's a real use-after-free
     * whose undefined behaviour shows up as a child that hangs for ever,
     * which a waitpid below without a bound could never see. The bound is
     * 200 iterations of 10ms, so 2s, well above any realistic scheduling
     * delay and still a hard bound. If the condition never becomes true the
     * REQUIRE_TRUE below fails this test cleanly instead of going on into the
     * same race. */
    bool linked = false;
    for (int i = 0; i < 200 && !linked; i++) {
      linked = _ccol_circq_test_has_sel_read_waiter_for_tests(cq);
      if (!linked) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
      }
    }
    /* not reachable in practice; see the REQUIRE_TRUE below. */
    if (!linked) _exit(3);

    /* The misuse under test: the waiter thread above is confirmed to be
     * linked into cq's waiter list when this destroys cq. */
    ccol_circular_queue_destroy(cq);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* The doc comment of _wait_for_forked_child_bounded and the sibling test
   * above explain why the bound is 20s and why it's there at all. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// DYNAMIC_QUEUE TESTS

TEST(dynamic_queues, create_fails) {
  char *err_str = "";

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)dq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_no_mprocs) {
  char *err_str = "";

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_with_mp) {
  char *err_str = "";

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, basic_send_and_receive_no_mprocs) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_with_mp) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, msg_count) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(ccol_dynmq_msg_count(dq), i);
    ccol_dynmq_send_zc(dq, &m1);
    REQUIRE_EQ(ccol_dynmq_msg_count(dq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(ccol_dynmq_msg_count(dq), i);
    ccol_dynmq_recv_zc(dq, &m1);
    REQUIRE_EQ(ccol_dynmq_msg_count(dq), i - 1);
  }

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_NULL_msg) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 0;
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, fifo_ordering) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  const char labels[] = {'A', 'B', 'C'};
  for (int i = 0; i < 3; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = labels[i];
    c_message_t m = {.data = buf, .size = 1};
    REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
    REQUIRE_EQ(m.data, NULL);
  }
  REQUIRE_EQ(ccol_dynmq_msg_count(dq), 3);

  for (int i = 0; i < 3; ++i) {
    c_message_t m = {.data = NULL, .size = 0};
    REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m), ccol_success);
    REQUIRE_EQ(*(char *)m.data, labels[i]);
    free(m.data);
  }

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_try_receive) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_timed_receive) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  uint64_t timeout = 100000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m2, timeout), ccol_success);
  getWallTime(after);
  /* The call doesn't wait. The bound is loose for a loaded or emulated
   * machine, and a call that waits for the timeout fails the result check. */
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m1, timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  /* Mirrors circular_queues.timed_recv_reports_unexpected_failure_on_
   * condvar_error: a forced error on an empty queue with no race still has
   * to give ccol_unexpected_failure quickly. */
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  _ccol_dynmq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t recv_rv = ccol_dynmq_timed_recv_zc(dq, &m, timeout);
  int recv_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL and errno carries it to the caller. */
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* The same class of failure as _sel_wait_condvar of ccol_select: the
   * FAILURE branch of ccol_dynmq_timed_recv_zc must not always report
   * ccol_unexpected_failure, because a producer on another thread can put a
   * message into the queue at the same instant. _ccol_cond_var_timedwait
   * always locks dq->mutex again before returning, on success and on
   * failure, so this order of events can happen in production. The dedicated
   * test hook makes it deterministic, since a real producer thread can't race
   * into this exact window on demand. Without that handling the test fails
   * with ccol_unexpected_failure and the sentinel message the hook put into
   * the queue is stuck there for ever. */
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  _ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // overwritten on success
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel from the hook
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(ccol_dynmq_msg_count(dq), (size_t)0);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, enable_disable_sending) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  ccol_dynmq_disable_sending(dq);

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  ccol_dynmq_enable_sending(dq);

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  ccol_dynamic_queue_destroy(dq);
}

/* A timeout of 0 is ccol_dynmq_try_recv_zc, so an empty queue answers
 * ccol_container_empty immediately and not ccol_timed_out. */
TEST(dynamic_queues, timed_recv_with_a_zero_timeout_is_the_try_variant) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  struct timespec before, after;
  getWallTime(before);
  c_message_t m = {.data = NULL, .size = 0};
  ccol_retval_t empty_rv = ccol_dynmq_timed_recv_zc(dq, &m, 0);
  getWallTime(after);
  c_message_t sent = {.data = NULL, .size = 0};
  ccol_retval_t send_rv = ccol_dynmq_send_zc(dq, &sent);
  ccol_retval_t got_rv = ccol_dynmq_timed_recv_zc(dq, &m, 0);
  ccol_retval_t null_rv = ccol_dynmq_timed_recv_zc(NULL, &m, 0);
  ccol_dynamic_queue_destroy(dq);

  REQUIRE_EQ(empty_rv, ccol_container_empty);
  REQUIRE_EQ(send_rv, ccol_success);
  REQUIRE_EQ(got_rv, ccol_success);
  REQUIRE_EQ(null_rv, ccol_invalid_args);
  REQUIRE_LT(diffTimeUSec(before, after), 1000000);
}

TEST(dynamic_queues, recv_drains_successfully_after_disable) {
  // A receive still gets the messages already in the queue after the test
  // turns sending off. Once the queue is empty a timed recv has to time out
  // and not return ccol_not_permitted, because the off state only affects
  // senders.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);

  ccol_dynmq_disable_sending(dq);

  // The messages already there must drain successfully.
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m), ccol_success);
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m), ccol_success);

  // The queue is empty and sending is off, so expect a timed recv to time out
  // and not return ccol_not_permitted.
  uint64_t timeout = 50000;  // 50 ms
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m, timeout), ccol_timed_out);

  ccol_dynamic_queue_destroy(dq);
}

typedef struct {
  ccol_dynamic_queue *dq;
  ccol_retval_t result;
} dq_disable_recv_args;

void *dq_blocking_recv_thread(void *raw) {
  dq_disable_recv_args *a = (dq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = ccol_dynmq_recv_zc(a->dq, &m);
  return NULL;
}

TEST(dynamic_queues, disable_sending_does_not_unblock_recv) {
  // ccol_dynmq_disable_sending must NOT wake a thread blocked in
  // ccol_dynmq_recv_zc. That thread stays blocked and returns only after the
  // test turns sending back on and a message arrives.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  dq_disable_recv_args args = {.dq = dq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  ccol_dynmq_disable_sending(dq);
  usleep(20000);  // receiver must still be blocked at this point

  // Turn sending back on and send a message, which releases the receiver.
  ccol_dynmq_enable_sending(dq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_times_out_when_disabled) {
  // ccol_dynmq_timed_recv_zc returns ccol_timed_out, not
  // ccol_not_permitted, when the queue is empty and sending is off, since the
  // off state must not make the call return before the deadline.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  ccol_dynmq_disable_sending(dq);

  uint64_t timeout = 100000;  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m, timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  ccol_dynamic_queue_destroy(dq);
}

void *dq_helper_thread(void *args) {
  ccol_dynamic_queue *dq = (ccol_dynamic_queue *)args;

  c_message_t m = {.data = NULL, .size = 0};
  assert(ccol_dynmq_recv_zc(dq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(dynamic_queues, send_and_receive_thread) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_mp(NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_helper_thread, dq), 0);

  usleep(50000);  // This makes the receiver wait

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  ccol_dynamic_queue_destroy(dq);
}

/* The ccol_dynamic_queue version of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above;
 * see that test's comment. */
TEST(dynamic_queues, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
    if (!dq) _exit(2);
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
    ccol_event_handlers_t handlers = {0};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_dynq(dq, ccol_select_read), handlers, NULL,
        &err);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
    /* The misuse under test: nothing removes the registration above and
     * nothing destroys the loop before this call. */
    ccol_dynamic_queue_destroy(dq);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// CHANNEL TESTS

TEST(channels, create_fails) {
  char *err_str = NULL;

  ccol_channel *ch = ccol_channel_create_mp(0, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = ccol_channel_create_mp(-1, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = ccol_channel_create_mp(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = NULL, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(channels, create_and_destroy_no_mprocs) {
  char *err_str = "";

  ccol_channel *ch = ccol_channel_create_mp(1, NULL, &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

TEST(channels, create_and_destroy_with_mp) {
  char *err_str = "";

  ccol_channel *ch = ccol_channel_create_mp(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

void *thr_for_channels_basic_send_and_receive(void *args) {
  // Helper threads use direct assertions.
  ccol_channel *ch = (ccol_channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  assert(ccol_chan_recv_zc(ch, &msg) == ccol_success);

  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  assert(ccol_chan_send_zc(ch, &msg) == ccol_success);

  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, basic_send_and_receive_no_mprocs) {
  ccol_channel *ch = ccol_channel_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(ccol_chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(ch);
}

TEST(channels, basic_send_and_receive_with_mp) {
  ccol_channel *ch = ccol_channel_create_mp(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(ccol_chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(ch);
}

void *thr_for_channels_msg_count(void *args) {
  // Helper threads use direct assertions.
  ccol_channel *ch = (ccol_channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  for (size_t i = 3; i > 0; --i) {
    assert(ccol_chan_msg_count(ch, ccol_owner_to_workers) == i);
    ccol_chan_recv_zc(ch, &msg);
    assert(ccol_chan_msg_count(ch, ccol_owner_to_workers) == i - 1);
  }

  for (size_t i = 0; i < 3; ++i) {
    assert(ccol_chan_msg_count(ch, ccol_workers_to_owner) == i);
    ccol_chan_send_zc(ch, &msg);
    assert(ccol_chan_msg_count(ch, ccol_workers_to_owner) == i + 1);
  }

  return NULL;
}

TEST(channels, msg_count) {
  ccol_channel *ch = ccol_channel_create_mp(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_owner_to_workers), i);
    ccol_chan_send_zc(ch, &m1);
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_owner_to_workers), i + 1);
  }

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_channels_msg_count, ch), 0);

  /* The worker takes the three messages already queued and sends three into
   * a channel of capacity 3, so it never blocks. That bounds the join, and
   * after it the counts below don't depend on how fast the worker ran. */
  pthread_join(tid, NULL);

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_workers_to_owner), i);
    ccol_chan_recv_zc(ch, &m1);
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_workers_to_owner), i - 1);
  }

  ccol_channel_destroy(ch);
}

/* The two channel tests below take turns through phase, never through a
 * fixed sleep, because a sleep on a busy runner can end long after its time
 * and then the two threads lose their order. The worker records the line of
 * its first failed check in failed_line and the main thread asserts it after
 * the join, so a failure never ends the whole binary. */
typedef struct {
  ccol_channel *ch;
  atomic_int phase;
  atomic_int failed_line;
} chan_turns_t;

#define CHAN_CHECK(t, cond)                                                \
  do {                                                                     \
    if (!(cond)) {                                                         \
      int _zero = 0;                                                       \
      atomic_compare_exchange_strong(&(t)->failed_line, &_zero, __LINE__); \
    }                                                                      \
  } while (0)

/* Waits until phase reaches want, for at most 10 s, and returns false on a
 * timeout. */
static bool chan_wait_phase(chan_turns_t *t, int want) {
  for (int i = 0; i < 10000; i++) {
    if (atomic_load(&t->phase) >= want) return true;
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

void *thr_for_channels_try_send_and_try_receive(void *args) {
  chan_turns_t *t = (chan_turns_t *)args;
  if (!chan_wait_phase(t, 1)) {
    CHAN_CHECK(t, false);
    return NULL;
  }

  c_message_t msg = {.data = NULL, .size = 0};
  CHAN_CHECK(t, ccol_chan_try_recv_zc(t->ch, &msg) == ccol_success);
  CHAN_CHECK(t, msg.data != NULL && *((char *)msg.data) == 'A');
  if (msg.data) *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  CHAN_CHECK(t, ccol_chan_try_recv_zc(t->ch, &m2) == ccol_container_empty);
  CHAN_CHECK(t, m2.data == NULL);

  if (msg.data) {
    CHAN_CHECK(t, ccol_chan_try_send_zc(t->ch, &msg) == ccol_success);
    CHAN_CHECK(t, msg.data == NULL);
    free(msg.data);
  }

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  CHAN_CHECK(t, ccol_chan_try_send_zc(t->ch, &m2) == ccol_container_full);
  CHAN_CHECK(t, m2.data != NULL);
  free(m2.data);

  atomic_store(&t->phase, 2);
  return NULL;
}

TEST(channels, try_send_and_try_receive) {
  chan_turns_t t = {.ch = ccol_channel_create_mp(1, NULL, NULL)};
  REQUIRE_NE((void *)t.ch, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_try_send_and_try_receive, &t),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  ccol_retval_t first_send = ccol_chan_try_send_zc(t.ch, &m1);
  bool first_taken = m1.data == NULL;
  free(m1.data);

  m1 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  ccol_retval_t full_send = ccol_chan_try_send_zc(t.ch, &m1);
  bool full_kept = m1.data != NULL;
  free(m1.data);
  m1.data = NULL;

  atomic_store(&t.phase, 1);
  bool worker_done = chan_wait_phase(&t, 2);

  c_message_t m2 = {.data = NULL, .size = 0};
  ccol_retval_t got = ccol_chan_try_recv_zc(t.ch, &m2);
  char got_char = m2.data ? *(char *)m2.data : 0;
  ccol_retval_t empty = ccol_chan_try_recv_zc(t.ch, &m1);
  bool empty_left_null = m1.data == NULL;

  free(m2.data);
  free(m1.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(t.ch);

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

/* A call that should time out gets CHAN_SHORT_US and has to take at least
 * that long. A call that should succeed at once gets CHAN_LONG_US and must
 * return well before it, so an implementation that waits out its timeout
 * before taking a message that's already there fails, while a slow runner
 * doesn't fail a correct one. */
#define CHAN_SHORT_US 10000
#define CHAN_LONG_US 1000000
#define CHAN_AT_ONCE_US 500000

void *thr_for_channels_timed_send_and_timed_receive(void *args) {
  chan_turns_t *t = (chan_turns_t *)args;
  if (!chan_wait_phase(t, 1)) {
    CHAN_CHECK(t, false);
    return NULL;
  }

  struct timespec before;
  struct timespec after;

  c_message_t msg = {.data = NULL, .size = 0};
  getWallTime(before);
  CHAN_CHECK(
      t, ccol_chan_timed_recv_zc(t->ch, &msg, CHAN_LONG_US) == ccol_success);
  getWallTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) < CHAN_AT_ONCE_US);
  CHAN_CHECK(t, msg.data != NULL && *((char *)msg.data) == 'A');
  if (msg.data) *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  CHAN_CHECK(
      t, ccol_chan_timed_recv_zc(t->ch, &m2, CHAN_SHORT_US) == ccol_timed_out);
  getWallTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) >= CHAN_SHORT_US);
  CHAN_CHECK(t, m2.data == NULL);

  if (msg.data) {
    getWallTime(before);
    CHAN_CHECK(
        t, ccol_chan_timed_send_zc(t->ch, &msg, CHAN_LONG_US) == ccol_success);
    getWallTime(after);
    CHAN_CHECK(t, diffTimeUSec(before, after) < CHAN_AT_ONCE_US);
    CHAN_CHECK(t, msg.data == NULL);
    free(msg.data);
  }

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  getWallTime(before);
  CHAN_CHECK(
      t, ccol_chan_timed_send_zc(t->ch, &m2, CHAN_SHORT_US) == ccol_timed_out);
  getWallTime(after);
  CHAN_CHECK(t, diffTimeUSec(before, after) >= CHAN_SHORT_US);
  CHAN_CHECK(t, m2.data != NULL);
  free(m2.data);

  atomic_store(&t->phase, 2);
  return NULL;
}

TEST(channels, timed_send_and_timed_receive) {
  chan_turns_t t = {.ch = ccol_channel_create_mp(1, NULL, NULL)};
  REQUIRE_NE((void *)t.ch, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL,
                            thr_for_channels_timed_send_and_timed_receive, &t),
             0);

  struct timespec before;
  struct timespec after;

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  getWallTime(before);
  ccol_retval_t first_send = ccol_chan_timed_send_zc(t.ch, &m1, CHAN_LONG_US);
  getWallTime(after);
  long first_us = diffTimeUSec(before, after);
  bool first_taken = m1.data == NULL;
  free(m1.data);

  m1 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  getWallTime(before);
  ccol_retval_t full_send = ccol_chan_timed_send_zc(t.ch, &m1, CHAN_SHORT_US);
  getWallTime(after);
  long full_us = diffTimeUSec(before, after);
  bool full_kept = m1.data != NULL;
  free(m1.data);
  m1.data = NULL;

  atomic_store(&t.phase, 1);
  bool worker_done = chan_wait_phase(&t, 2);

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  ccol_retval_t got = ccol_chan_timed_recv_zc(t.ch, &m2, CHAN_LONG_US);
  getWallTime(after);
  long got_us = diffTimeUSec(before, after);
  char got_char = m2.data ? *(char *)m2.data : 0;

  getWallTime(before);
  ccol_retval_t empty = ccol_chan_timed_recv_zc(t.ch, &m1, CHAN_SHORT_US);
  getWallTime(after);
  long empty_us = diffTimeUSec(before, after);
  bool empty_left_null = m1.data == NULL;

  free(m2.data);
  free(m1.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(t.ch);

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

void *thr_for_enable_disable_sending(void *args) {
  ccol_channel *ch = (ccol_channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};
  assert(ccol_chan_recv_zc(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');
  *((char *)msg.data) = 'B';

  ccol_chan_disable_sending(ch, ccol_workers_to_owner);
  assert(ccol_chan_send_zc(ch, &msg) == ccol_not_permitted);
  assert(msg.data != NULL);

  ccol_chan_enable_sending(ch, ccol_workers_to_owner);
  assert(ccol_chan_send_zc(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, enable_disable_sending) {
  ccol_channel *ch = ccol_channel_create_mp(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_enable_disable_sending, ch), 0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  m1.size = 1;
  ((char *)(m1.data))[0] = 'A';

  ccol_chan_disable_sending(ch, ccol_owner_to_workers);

  REQUIRE_EQ(ccol_chan_send_zc(ch, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  ccol_chan_enable_sending(ch, ccol_owner_to_workers);

  REQUIRE_EQ(ccol_chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(ccol_chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(ch);
}

/* The ccol_channel version of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above.
 * __ccol_channel_destroy destroys both circular queues under the channel, so
 * it gets the same assert from whichever direction still has a live
 * ccol_event_loop registration watching it. */
TEST(channels, destroy_with_live_event_loop_registration_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_channel *ch = ccol_channel_create(4, NULL);
    if (!ch) _exit(2);
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
    ccol_event_handlers_t handlers = {0};
    char *err = NULL;
    /* This thread owns the ccol_channel, so ccol_select_read here resolves
     * to workers_to_owner_cq; the doc comment of ccol_selectable_from_chan
     * has the details. */
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_chan(ch, ccol_select_read), handlers, NULL,
        &err);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
    /* The misuse under test: nothing removes the registration above and
     * nothing destroys the loop before this call. */
    ccol_channel_destroy(ch);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// CCOL_SELECT TESTS

// helpers
typedef struct {
  ccol_circular_queue *cq;
  int delay_us;
  int value;
} sel_circq_args;

typedef struct {
  ccol_dynamic_queue *dq;
  int delay_us;
  int value;
} sel_dynq_args;

typedef struct {
  ccol_circular_queue *cq0;
  ccol_circular_queue *cq1;
  int delay_us;
  int value;
} sel_disable_reenable_args;

typedef struct {
  ccol_channel *ch;
  int delay_us;
  int value;
} sel_chan_args;

static void *thr_send_to_circq(void *arg) {
  sel_circq_args *a = (sel_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(ccol_circq_send_zc(a->cq, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_to_dynq(void *arg) {
  sel_dynq_args *a = (sel_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(ccol_dynmq_send_zc(a->dq, &msg) == ccol_success);
  return NULL;
}

/* Turns sending off on both queues and waits for another delay, then turns
 * sending back on for cq0 and sends one message to it. It checks that
 * ccol_select stays blocked through the off phase and wakes only on the
 * message that comes after it. */
static void *thr_disable_then_reenable_and_send(void *arg) {
  sel_disable_reenable_args *a = (sel_disable_reenable_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccol_circq_disable_sending(a->cq0);
  ccol_circq_disable_sending(a->cq1);
  usleep((useconds_t)a->delay_us);
  ccol_circq_enable_sending(a->cq0);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(ccol_circq_send_zc(a->cq0, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_via_channel(void *arg) {
  sel_chan_args *a = (sel_chan_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(ccol_chan_send_zc(a->ch, &msg) == ccol_success);
  return NULL;
}

// tests
TEST(ccol_select, returns_invalid_args_on_bad_inputs) {
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx = 0;
  ccol_selectable sel = ccol_selectable_from_circq(cq, ccol_select_read);

  REQUIRE_EQ(ccol_select(NULL, 1, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 0, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 1, NULL), ccol_invalid_args);

  ccol_selectable null_sel = ccol_selectable_from_circq(NULL, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &null_sel), ccol_invalid_args);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select,
     timed_n_too_large_rejected_before_any_allocation_or_oob_read) {
  /* Regression test for the size_t overflow guard in _sel_validate_args.
   * ccol_select_timed() keeps one waiter node per selectable in an array it
   * allocates with a single plain malloc(n * sizeof(internal waiter node))
   * call, which has no calloc-style overflow check of its own. The waiter
   * node struct has several pointer-sized fields, so n == SIZE_MAX is always
   * larger than SIZE_MAX / sizeof(that struct) and the call has to refuse it
   * with ccol_invalid_args. The refusal also needs to come BEFORE the code
   * forms the overflowing multiplication and before it reads selectables[i]
   * for any i > 0. The test passes an array of exactly one element on
   * purpose while n claims far more, so a regression that checks n only
   * after it starts scanning the array reads out of bounds here instead of
   * just getting an allocation size wrong. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  ccol_selectable sel = ccol_selectable_from_circq(cq, ccol_select_read);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed(&idx, SIZE_MAX, &sel, 0), ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select,
     timed_n_exceeding_int_max_rejected_before_any_allocation_or_oob_read) {
  /* Regression test for the n > INT_MAX guard in _sel_validate_args.
   * _sel_phase1_scan_register narrows the index of a matched selectable, a
   * size_t in the range 0..n-1, into a plain `int`, its `found` local, which
   * also carries the sentinels -1 for "nothing found yet" and -2 for "system
   * error", and ccol_select_timed then widens it back with `(size_t)found`.
   * With an n large enough for a match beyond INT_MAX that narrowing gives a
   * negative or wrapped value, which then corrupts *ready_index. This n is
   * far below the SIZE_MAX / sizeof(internal waiter node) guard the test
   * above covers: INT_MAX is about 2^31 and that other guard's ceiling is
   * several orders of magnitude higher on any 64-bit platform, so this test
   * covers the INT_MAX guard alone. Here too the refusal must come BEFORE the
   * code reads selectables[i] for any i > 0. The test passes an array of
   * exactly one element on purpose while n claims far more, so a regression
   * that checks n only after it starts scanning the array reads out of
   * bounds here instead of just getting an allocation size wrong. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  ccol_selectable sel = ccol_selectable_from_circq(cq, ccol_select_read);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed(&idx, (size_t)INT_MAX + 1, &sel, 0),
             ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, receives_from_first_queue_when_message_already_present) {
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_circular_queue *q1 = ccol_circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 7;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(q0, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(q0, ccol_select_read),
                     ccol_selectable_from_circq(q1, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 7);

  free(recv_msg.data);
  ccol_circular_queue_destroy(q0);
  ccol_circular_queue_destroy(q1);
}

TEST(ccol_select, receives_from_second_queue_when_message_already_present) {
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_circular_queue *q1 = ccol_circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 42;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(q1, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(q0, ccol_select_read),
                     ccol_selectable_from_circq(q1, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);

  free(recv_msg.data);
  ccol_circular_queue_destroy(q0);
  ccol_circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_circq) {
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_circular_queue *q1 = ccol_circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = q1, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(q0, ccol_select_read),
                     ccol_selectable_from_circq(q1, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(q0);
  ccol_circular_queue_destroy(q1);
}

TEST(ccol_select, timed_returns_timed_out_when_all_queues_disabled) {
  // Sending being off must not affect waiters in the read direction, so
  // ccol_select_timed should wait out the full timeout and not return
  // ccol_not_permitted.
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_circular_queue *q1 = ccol_circular_queue_create(4, NULL);

  ccol_circq_disable_sending(q0);
  ccol_circq_disable_sending(q1);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 50000 /* us */,
                           ccol_selectable_from_circq(q0, ccol_select_read),
                           ccol_selectable_from_circq(q1, ccol_select_read)),
      ccol_timed_out);

  ccol_circular_queue_destroy(q0);
  ccol_circular_queue_destroy(q1);
}

TEST(ccol_select,
     stays_blocked_through_disable_then_wakes_after_reenable_and_send) {
  // ccol_select must stay blocked while sending is off on all queues, and
  // once the test turns sending back on and a message arrives it should
  // return success.
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_circular_queue *q1 = ccol_circular_queue_create(4, NULL);

  sel_disable_reenable_args args = {
      .cq0 = q0, .cq1 = q1, .delay_us = 15000, .value = 42};
  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_disable_then_reenable_and_send, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(q0, ccol_select_read),
                     ccol_selectable_from_circq(q1, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(q0);
  ccol_circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_dynq) {
  ccol_circular_queue *q0 = ccol_circular_queue_create(4, NULL);
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 55};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(q0, ccol_select_read),
                     ccol_selectable_from_dynq(dq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 55);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(q0);
  ccol_dynamic_queue_destroy(dq);
}

TEST(ccol_select, ccol_channel_direction_resolved_correctly_for_owner_thread) {
  ccol_channel *ch = ccol_channel_create_mp(4, NULL, NULL);

  /* The worker thread sends with ccol_chan_send_zc, which goes to
   * workers_to_owner_cq, and the owner thread calls
   * ccol_selectable_from_chan() here, which resolves to workers_to_owner_cq
   * as well, so ccol_select watches the right queue.
   */
  sel_chan_args args = {.ch = ch, .delay_us = 15000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_via_channel, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_chan(ch, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_chan_try_recv_zc(ch, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  ccol_channel_destroy(ch);
}

// write-wait helpers
typedef struct {
  ccol_circular_queue *cq;
  int delay_us;
} sel_recv_circq_args;

static void *thr_recv_from_circq(void *arg) {
  sel_recv_circq_args *a = (sel_recv_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  c_message_t msg = {.data = NULL, .size = 0};
  assert(ccol_circq_recv_zc(a->cq, &msg) == ccol_success);
  free(msg.data);
  return NULL;
}

typedef struct {
  ccol_circular_queue *cq;
  int delay_us;
} sel_disable_circq_args;

static void *thr_disable_circq_sending(void *arg) {
  sel_disable_circq_args *a = (sel_disable_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccol_circq_disable_sending(a->cq);
  return NULL;
}

/* Fills a circular queue of capacity 1 with one message the queue doesn't
 * own (a static byte), so a test can drain it without a free. */
static char sel_full_token;

static ccol_retval_t sel_fill_one(ccol_circular_queue *cq) {
  c_message_t m = {.data = &sel_full_token, .size = 1};
  return ccol_circq_try_send_zc(cq, &m);
}

// write-wait tests
TEST(ccol_select, write_circq_writable_immediately) {
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(cq, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_blocks_until_reader_frees_space) {
  ccol_circular_queue *cq = ccol_circular_queue_create(2, NULL);

  /* Fill the queue to capacity */
  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 10;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 20;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);

  sel_recv_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_recv_from_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(cq, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);

  /* Drain the message that's left so the assert in destroy passes */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  ccol_circular_queue_destroy(cq);
}

/* A queue whose sends are turned off is ready for the write direction, the
 * way a pipe whose reader is gone is ready for POLLOUT. So a write select
 * waiting on a full queue returns as soon as another thread turns sends off,
 * and the try_send after it reports ccol_not_permitted. If a queue with
 * sends off didn't count as ready, the select would keep waiting after the
 * wake and end with ccol_timed_out. */
TEST(ccol_select, write_full_circq_wakes_when_sending_is_disabled) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval_t filled = sel_fill_one(cq);

  sel_disable_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  int created = pthread_create(&tid, NULL, thr_disable_circq_sending, &args);

  size_t idx = 99;
  ccol_retval_t sel_rv = ccol_timed_out;
  ccol_retval_t send_rv = ccol_success;
  if (created == 0) {
    sel_rv =
        ccol_select_timed_va(&idx, 10000000 /* us */,
                             ccol_selectable_from_circq(cq, ccol_select_write));
    c_message_t m = {.data = &sel_full_token, .size = 1};
    send_rv = ccol_circq_try_send_zc(cq, &m);
    pthread_join(tid, NULL);
  }
  c_message_t drain;
  while (ccol_circq_try_recv_zc(cq, &drain) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

/* Turning sends back on ends that state, and a full circular queue with
 * sends on isn't ready for the write direction. */
TEST(ccol_select, write_full_circq_is_not_ready_after_sending_is_enabled) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval_t filled = sel_fill_one(cq);
  ccol_circq_disable_sending(cq);
  ccol_circq_enable_sending(cq);

  size_t idx = 99;
  ccol_retval_t sel_rv = ccol_select_timed_va(
      &idx, 0, ccol_selectable_from_circq(cq, ccol_select_write));
  c_message_t drain;
  while (ccol_circq_try_recv_zc(cq, &drain) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(sel_rv, ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
}

TEST(ccol_select, write_dynq_writable_immediately) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_dynq(dq, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  ccol_dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_mixed_full_circq_and_readable_circq) {
  /* q_write is full and q_read is empty and gets a message, so ccol_select
   * picks q_read, in the read direction, first. */
  ccol_circular_queue *q_write = ccol_circular_queue_create(1, NULL);
  ccol_circular_queue *q_read = ccol_circular_queue_create(4, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 42;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(q_write, &fill_msg), ccol_success);

  sel_circq_args args = {.cq = q_read, .delay_us = 15000, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(
                 &idx, ccol_selectable_from_circq(q_write, ccol_select_write),
                 ccol_selectable_from_circq(q_read, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(q_read, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 88);

  free(recv_msg.data);
  pthread_join(tid, NULL);

  /* Drain q_write's fill message */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(q_write, &drain), ccol_success);
  free(drain.data);

  ccol_circular_queue_destroy(q_write);
  ccol_circular_queue_destroy(q_read);
}

TEST(ccol_select, write_channel_owner_resolves_send_direction) {
  ccol_channel *ch = ccol_channel_create_mp(4, NULL, NULL);

  /* The owner calls ccol_selectable_from_chan with ccol_select_write, which
   * resolves to owner_to_workers_cq. That queue is empty and writable, so
   * ccol_select returns immediately. */
  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_chan(ch, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  ccol_channel_destroy(ch);
}

// fd selectable helpers
typedef struct {
  int write_fd;
  int delay_us;
  int value;
} sel_fd_write_args;

static void *thr_write_to_fd(void *arg) {
  sel_fd_write_args *a = (sel_fd_write_args *)arg;
  usleep((useconds_t)a->delay_us);
  int v = a->value;
  assert(write(a->write_fd, &v, sizeof(v)) == sizeof(v));
  return NULL;
}

// fd selectable tests
TEST(ccol_select, fd_readable_immediately) {
  /* Writes data to the pipe before calling ccol_select, so the call returns
   * at once and reports the right index, and the caller then reads the
   * fd itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 42;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 42);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_wait_capped_below_a_long_timeout_waits_again) {
  /* A timeout of 30 days is longer than the INT_MAX milliseconds one kernel
   * wait can take. The hook ends the first three such capped waits at once
   * with no event, as the real wait does when the capped time runs out, and
   * the call has to wait again and report the fd, which is already readable,
   * instead of a timeout 30 days early. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char byte = 'x';
  ssize_t wrote = write(pfd[1], &byte, 1);

  ccol_selectable sels[1] = {ccol_selectable_from_fd(pfd[0], ccol_select_read)};
  size_t idx = 99;
  _ccol_select_test_short_next_capped_waits(3);
  ccol_retval_t rv =
      ccol_select_timed(&idx, 1, sels, 30ULL * 86400ULL * 1000000ULL);
  _ccol_select_test_short_next_capped_waits(0);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(wrote, (ssize_t)1);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
}

TEST(ccol_select, fd_blocks_until_data_arrives) {
  /* The thread writes to the pipe after a delay, so ccol_select blocks
   * and then wakes when the data arrives, and the caller then reads the fd
   * itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  sel_fd_write_args args = {.write_fd = pfd[1], .delay_us = 15000, .value = 77};
  /* The clock starts before the thread exists and the thread sleeps 15 ms
   * before writing, so a call that waits for the data always measures at
   * least 15 ms however the threads are scheduled. A call that returns
   * without data measures less unless this thread is descheduled, and that
   * can't make a correct build fail. */
  struct timespec before, after;
  getWallTime(before);
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_write_to_fd, &args), 0);

  size_t idx = 99;
  ccol_retval_t sel_rv =
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read));
  getWallTime(after);
  /* The writer only sleeps and writes once, so this join can't hang. */
  pthread_join(tid, NULL);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, 0);
  REQUIRE_GE(diffTimeUSec(before, after), 10000); /* the call really blocked */

  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 77);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_writable_immediately) {
  /* A new pipe write end is always writable, so the call returns
   * immediately. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[1], ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_and_queue_fd_wins) {
  /* The pipe already has data and the circular queue is empty, so the fd
   * wins at index 0. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 55;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read),
                     ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 55);

  close(pfd[0]);
  close(pfd[1]);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_queue_queue_wins) {
  /* The pipe has no data and a thread sends to the queue after a delay, so
   * the queue should win. That shows the eventfd bridge wakes epoll_wait for
   * a queue event in a selectable array that mixes fds and queues. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 15000, .value = 33};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read),
                     ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 33);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_dynq_dynq_wins) {
  /* The same as the test above but with a ccol_dynamic_queue, to check that
   * the eventfd bridge works for dynq selectables in epoll mode. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read),
                     ccol_selectable_from_dynq(dq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  ccol_dynamic_queue_destroy(dq);
}

TEST(ccol_select, fd_invalid_fd_returns_invalid_args) {
  size_t idx = 99;
  ccol_selectable bad = ccol_selectable_from_fd(-1, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &bad), ccol_invalid_args);
}

TEST(ccol_select, timed_circq_returns_timed_out) {
  /* The queue is empty and sending is on, so ccol_select_timed must return
   * ccol_timed_out after the deadline instead of blocking for ever. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 50000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_timed_out);
  /* the call must not touch idx on a timeout */
  REQUIRE_EQ(idx, (size_t)99);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, timed_fd_returns_timed_out) {
  /* The read end of a pipe with no data in it, so the call must time
   * out. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 50000 /* us */,
                           ccol_selectable_from_fd(pfd[0], ccol_select_read)),
      ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, timed_poll_zero_ms_circq_empty) {
  /* A timeout of 0 is a poll that doesn't block, and the queue is empty, so
   * the call gives timed_out right away. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(
                 &idx, 0, ccol_selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);
  ccol_circular_queue_destroy(cq);
}

typedef struct helper_thread_args {
  ccol_circular_queue *cq;
  c_message_t msg;
} helper_thread_args;

void *helper_thread_main(void *arg) {
  helper_thread_args *a = arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
  nanosleep(&ts, NULL);
  ccol_circq_send_zc(a->cq, &a->msg);
  return NULL;
}

TEST(ccol_select, timed_succeeds_before_deadline) {
  /* The producer sends after 20 ms, well before the 5 s deadline, so select
   * returns success. The deadline is wide so a loaded or emulated
   * machine can't delay the send past it. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  pthread_t tid;
  c_message_t send_msg = {.data = malloc(4), .size = 4};
  *(int *)send_msg.data = 1234;

  /* A helper thread that sleeps for 20 ms and then sends. */
  helper_thread_args args = {cq, send_msg};

  REQUIRE_EQ(pthread_create(&tid, NULL, helper_thread_main, &args), 0);

  size_t idx = 99;
  ccol_retval_t sel_rv = ccol_select_timed_va(
      &idx, 5000000, ccol_selectable_from_circq(cq, ccol_select_read));
  /* The helper only sleeps and sends once, so this join can't hang. */
  pthread_join(tid, NULL);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 1234);
  free(recv_msg.data);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, timed_out_deregisters_waiter_node) {
  /* After ccol_select_timed times out it has to remove the waiter node from
   * the queue's waiter list before freeing the node, otherwise a later send
   * dereferences freed memory, which valgrind and AddressSanitizer report. A
   * clean run here confirms the removal happens on the timeout path. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 30000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_timed_out);

  /* Sends to the queue AFTER the select that timed out has returned. If the
   * select left the waiter node registered, ccol_circq_send_zc ->
   * notify_one_sel_waiter dereferences the freed node right here. */
  c_message_t msg = {.data = malloc(4), .size = 4};
  *(int *)msg.data = 42;
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);

  /* Drain so the queue is empty before destroy. */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, timed_wait_reports_unexpected_failure_on_condvar_error) {
  /* Regression test. ccol_select_timed has a wait path for the case with no
   * fd selectables, and there the deadline branch of _sel_wait_condvar must
   * not spin for ever retrying the same call when _ccol_cond_var_timedwait
   * returns an error that's neither 0 nor ETIMEDOUT. It reports
   * ccol_unexpected_failure instead, as ccol_circq_timed_send_zc,
   * ccol_circq_timed_recv_zc and ccol_dynmq_timed_recv_zc do for the same
   * class of failure. The test uses a test-only hook to force exactly one
   * such error, since no correct deadline this library computes can produce
   * a real EINVAL from _ccol_cond_var_timedwait. It sets a large 5s timeout
   * and a tight bound on the elapsed time, which proves the call returns
   * quickly on the forced error instead of waiting out the whole deadline or
   * hanging for ever. A hang here keeps this test from ever returning. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  _ccol_select_test_force_next_condvar_wait_error();

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 5000000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_unexpected_failure);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_EQ(idx, (size_t)99);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, timed_wait_ready_racing_condvar_error_still_succeeds) {
  /* For a return from _ccol_cond_var_timedwait that's neither 0 nor
   * ETIMEDOUT, the deadline branch of _sel_wait_condvar must not always report
   * its FAILURE outcome. It checks *ready again before declaring a real
   * failure, the same way the sibling ETIMEDOUT branch above it does.
   * _ccol_cond_var_timedwait always locks its mutex again before it returns,
   * on success and on failure, so a producer's notify can complete and set
   * *ready = true just before an unrelated, spurious wait error arrives too.
   * Without that second check in the FAILURE branch the code drops a wake-up
   * that has already arrived, and ccol_select_timed reports
   * ccol_unexpected_failure instead of resuming successfully.
   *
   * The test uses the dedicated hook for exactly that order of events: it
   * forces the error and *ready = true at the same instant. A real producer
   * thread can't race into this exact window on its own, because the hook
   * replaces _ccol_cond_var_timedwait without unlocking sel_mtx and nothing
   * else can lock sel_mtx meanwhile. A second, real background send races
   * the re-scan the call makes after the hook fires, and that send is what
   * lets the call succeed on either path: once the call resumes instead of
   * giving up, it either sees this message at once or falls through to an
   * ordinary real wait for it, which other tests cover well. Without that
   * handling the test always fails with ccol_unexpected_failure and never
   * gets ccol_success. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 50000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  _ccol_select_test_force_next_condvar_wait_error_racing_ready();

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 2000000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, (size_t)0);

  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(cq);
}

/* A write select on a full circular queue whose sends are already off is
 * ready at once, with a timeout of 0 too, and the send that follows reports
 * ccol_not_permitted. A queue with sends off that doesn't count as ready
 * makes the poll report ccol_timed_out. */
TEST(ccol_select, write_full_circq_is_ready_at_once_when_sending_disabled) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  REQUIRE_NE((void *)cq, NULL);
  ccol_retval_t filled = sel_fill_one(cq);
  ccol_circq_disable_sending(cq);

  size_t idx = 99;
  ccol_retval_t sel_rv = ccol_select_timed_va(
      &idx, 0, ccol_selectable_from_circq(cq, ccol_select_write));
  c_message_t m = {.data = &sel_full_token, .size = 1};
  ccol_retval_t send_rv = ccol_circq_try_send_zc(cq, &m);
  c_message_t drain;
  while (ccol_circq_try_recv_zc(cq, &drain) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);

  REQUIRE_EQ(filled, ccol_success);
  REQUIRE_EQ(sel_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

/* The same for a dynamic queue: a write select on a queue whose sends are
 * already off returns at once, untimed as well as with a timeout of 0, and
 * the send that follows reports ccol_not_permitted. If a queue with sends
 * off doesn't count as ready, the poll reports ccol_timed_out (and the
 * untimed select is never reached, so it can't hang the binary). */
TEST(ccol_select, write_dynq_is_ready_at_once_when_sending_disabled) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  ccol_dynmq_disable_sending(dq);

  size_t idx = 99;
  ccol_retval_t poll_rv = ccol_select_timed_va(
      &idx, 0, ccol_selectable_from_dynq(dq, ccol_select_write));
  size_t untimed_idx = 99;
  ccol_retval_t untimed_rv = ccol_timed_out;
  if (poll_rv == ccol_success)
    untimed_rv = ccol_select_va(
        &untimed_idx, ccol_selectable_from_dynq(dq, ccol_select_write));
  int *data = malloc(sizeof(int));
  c_message_t m = {.data = data, .size = sizeof(int)};
  ccol_retval_t send_rv = ccol_dynmq_send_zc(dq, &m);
  if (send_rv != ccol_success) free(data);
  c_message_t drain;
  while (ccol_dynmq_try_recv_zc(dq, &drain) == ccol_success) free(drain.data);
  ccol_dynamic_queue_destroy(dq);

  REQUIRE_NE((void *)data, NULL);
  REQUIRE_EQ(poll_rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  REQUIRE_EQ(untimed_rv, ccol_success);
  REQUIRE_EQ(untimed_idx, (size_t)0);
  REQUIRE_EQ(send_rv, ccol_not_permitted);
}

// concurrent write-waiters helpers
typedef struct {
  ccol_circular_queue *cq;
  ccol_retval_t result;
} sel_write_wait_result;

static void *thr_circq_write_wait(void *arg) {
  sel_write_wait_result *a = (sel_write_wait_result *)arg;
  size_t idx = 0;
  /* The 5 s bound is wide so a loaded or emulated machine can't end the
   * wait before the main thread frees the slot. */
  a->result = ccol_select_timed_va(
      &idx, 5000000 /* us */,
      ccol_selectable_from_circq(a->cq, ccol_select_write));
  return NULL;
}

TEST(ccol_select, write_circq_two_concurrent_waiters_both_wake_on_slot_free) {
  /* Two threads wait at the same time for write readiness on a full queue
   * of capacity 1. One dequeue has to cascade through the waiter list and
   * wake BOTH threads, not only the head waiter, and without that cascade
   * the second waiter times out. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 0;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &fill_msg), ccol_success);

  sel_write_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_write_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  bool started_t1 = (pthread_create(&t1, NULL, thr_circq_write_wait, &a1) == 0);
  bool started_t2 = (pthread_create(&t2, NULL, thr_circq_write_wait, &a2) == 0);

  /* Gives both threads time to register as write waiters before the slot is
   * freed. The result is the same without it, since a thread that registers
   * late finds a free slot and succeeds at once. */
  usleep(30000);

  c_message_t drain = {.data = NULL, .size = 0};
  ccol_retval_t drain_rv = ccol_circq_recv_zc(cq, &drain);
  free(drain.data);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
  REQUIRE_EQ(drain_rv, ccol_success);
  REQUIRE_TRUE(started_t1);
  REQUIRE_TRUE(started_t2);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  ccol_circular_queue_destroy(cq);
}

// concurrent read-waiters helper
typedef struct {
  ccol_circular_queue *cq;
  ccol_retval_t result;
} sel_read_wait_result;

static void *thr_circq_read_wait(void *arg) {
  sel_read_wait_result *a = (sel_read_wait_result *)arg;
  size_t idx = 0;
  /* See thr_circq_write_wait for the 5 s bound. */
  a->result =
      ccol_select_timed_va(&idx, 5000000 /* us */,
                           ccol_selectable_from_circq(a->cq, ccol_select_read));
  if (a->result == ccol_success) {
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = ccol_circq_try_recv_zc(a->cq, &msg);
    a->result = rv;
    if (rv == ccol_success) free(msg.data);
  }
  return NULL;
}

TEST(ccol_select,
     read_circq_two_concurrent_waiters_both_wake_on_message_available) {
  /* The cascade in the read direction. ccol_select() never consumes a
   * message itself, it only peeks, the same shape as the peek and
   * forward-notify of the write-direction branch, so a thread that finds
   * itself ready has to forward the notify to the next waiter. Otherwise two
   * threads waiting at the same time to read from one empty queue misbehave:
   * only the first one wakes although two messages are available. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  sel_read_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_read_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  bool started_t1 = (pthread_create(&t1, NULL, thr_circq_read_wait, &a1) == 0);
  bool started_t2 = (pthread_create(&t2, NULL, thr_circq_read_wait, &a2) == 0);

  /* Wait, at most about 10 s, until every started thread is linked as a read
   * waiter, so the sends below reach two waiters and test the cascade. The
   * bound only stops a hang. */
  size_t want_waiters = (size_t)started_t1 + (size_t)started_t2;
  for (int i = 0; i < 10000 && _ccol_circq_test_sel_read_waiter_count_for_tests(
                                   cq) < want_waiters;
       i++)
    usleep(1000);
  size_t seen_waiters = _ccol_circq_test_sel_read_waiter_count_for_tests(cq);

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 1;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 2;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  ccol_retval_t send0_rv = ccol_circq_send_zc(cq, &m0);
  ccol_retval_t send1_rv = ccol_circq_send_zc(cq, &m1);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
  REQUIRE_TRUE(started_t1);
  REQUIRE_TRUE(started_t2);
  REQUIRE_EQ(seen_waiters, (size_t)2);
  REQUIRE_EQ(send0_rv, ccol_success);
  REQUIRE_EQ(send1_rv, ccol_success);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  ccol_circular_queue_destroy(cq);
}

/* _sel_setup_epoll must not call epoll_ctl(EPOLL_CTL_ADD) once per fd
 * selectable without removing duplicates. Two selectables can share one real
 * fd in any mix of directions, and one can be an exact duplicate, so the
 * second EPOLL_CTL_ADD fails with EEXIST and the whole ccol_select or
 * ccol_select_timed call fails with ccol_unexpected_failure. That even
 * happens for the common pattern of one connected socket the caller watches
 * for readability and writability at the same time. So _sel_setup_epoll
 * groups the fd selectables by fd, with a sort rather than a linear scan so
 * the cost stays O(n log n) instead of dropping to O(n^2) for the far more
 * common case of many different fds. It registers one combined epoll_ctl
 * call per unique fd, and when a combined event fires it maps that event
 * back to the member selectable whose own direction the event satisfies. */
TEST(ccol_select, fd_same_fd_two_directions_writable_wins) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(sv[0], ccol_select_read),
      ccol_selectable_from_fd(sv[0], ccol_select_write),
  };
  /* A socket the test just connected is writable immediately and has nothing
   * to read yet, so this call should resolve to the write-direction
   * selectable, beyond merely succeeding. */
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)1);

  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_select, fd_same_fd_two_directions_both_ready_resolves_to_a_member) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  char byte = 'x';
  REQUIRE_EQ(write(sv[1], &byte, 1), (ssize_t)1);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(sv[0], ccol_select_read),
      ccol_selectable_from_fd(sv[0], ccol_select_write),
  };
  /* Both directions are ready now: one byte is waiting to be read and the
   * send buffer has room. Either member may win, but the call itself must
   * succeed and pick one of the two real members instead of giving the
   * EEXIST failure the fd grouping prevents. */
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  char buf[8];
  REQUIRE_EQ(read(sv[0], buf, sizeof(buf)), (ssize_t)1);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_select, fd_duplicate_selectable_same_direction_still_succeeds) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(pfd[1], ccol_select_write),
      ccol_selectable_from_fd(pfd[1], ccol_select_write),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_duplicate_selectable_neither_ready_times_out) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  /* Nothing writes to this pipe, so two read watches on the read end are
   * expected to behave like one ordinary registration and time out, neither
   * succeeding by accident nor failing with an unrelated error. */
  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(pfd[0], ccol_select_read),
      ccol_selectable_from_fd(pfd[0], ccol_select_read),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 100000);
  REQUIRE_EQ(rv, ccol_timed_out);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_grouped_registration_coexists_with_ready_queue) {
  /* This call has an fd that isn't ready, registered twice so both share one
   * epoll_ctl group, and a queue selectable that is really ready, which
   * confirms that the fd grouping doesn't disturb how ccol_select handles
   * queue selectables. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  REQUIRE_NE((void *)cq, NULL);
  int *data = malloc(sizeof(int));
  REQUIRE_NE((void *)data, NULL);
  *data = 42;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);

  size_t idx = 999;
  ccol_selectable sels[3] = {
      ccol_selectable_from_fd(pfd[0], ccol_select_read),
      ccol_selectable_from_fd(pfd[0], ccol_select_read),
      ccol_selectable_from_circq(cq, ccol_select_read),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 3, sels, 500000);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)2);

  c_message_t out;
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &out), ccol_success);
  free(out.data);
  ccol_circular_queue_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);
}

/* Each of these three queues holds what makes it ready for the whole test,
 * and ccol_select() never consumes anything, so every call finds all three
 * ready, and the scan's rotation should give each about a third of the
 * calls anyway. A scan that always starts at index 0 would hand all 3000
 * calls to index 0. */
TEST(ccol_select, three_always_ready_queues_share_the_calls) {
  ccol_circular_queue *cq_r = ccol_circular_queue_create(4, NULL);
  ccol_dynamic_queue *dq_r = ccol_dynamic_queue_create(NULL);
  ccol_circular_queue *cq_w = ccol_circular_queue_create(4, NULL);
  REQUIRE_NE((void *)cq_r, NULL);
  REQUIRE_NE((void *)dq_r, NULL);
  REQUIRE_NE((void *)cq_w, NULL);
  int *a = malloc(sizeof(int));
  int *b = malloc(sizeof(int));
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_NE((void *)b, NULL);
  *a = 1;
  *b = 2;
  c_message_t ma = {.data = a, .size = sizeof(int)};
  c_message_t mb = {.data = b, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq_r, &ma), ccol_success);
  REQUIRE_EQ(ccol_dynmq_send_zc(dq_r, &mb), ccol_success);

  ccol_selectable sels[3] = {
      ccol_selectable_from_circq(cq_r, ccol_select_read),
      ccol_selectable_from_dynq(dq_r, ccol_select_read),
      ccol_selectable_from_circq(cq_w, ccol_select_write),
  };
  size_t wins[3] = {0, 0, 0};
  size_t failures = 0;
  for (int i = 0; i < 3000; i++) {
    size_t idx = 99;
    if (ccol_select(&idx, 3, sels) != ccol_success || idx >= 3)
      failures++;
    else
      wins[idx]++;
  }

  c_message_t out;
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq_r, &out), ccol_success);
  free(out.data);
  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq_r, &out), ccol_success);
  free(out.data);
  ccol_circular_queue_destroy(cq_r);
  ccol_dynamic_queue_destroy(dq_r);
  ccol_circular_queue_destroy(cq_w);

  REQUIRE_EQ(failures, (size_t)0);
  for (int k = 0; k < 3; k++) {
    REQUIRE_GE(wins[k], (size_t)900);
    REQUIRE_LE(wins[k], (size_t)1100);
  }
}

/* A worker waiting on {stop fd, busy queue} has to see the stop although the
 * queue never runs empty. A readable fd at index 0 next to a queue that
 * stays ready wins within a few calls, and over many calls it wins about
 * half of them. A scan that looks at the fds only when no queue is ready
 * never reports index 0 here. */
TEST(ccol_select, a_readable_fd_beside_an_always_ready_queue_wins) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char stop = 's';
  REQUIRE_EQ(write(pfd[1], &stop, 1), (ssize_t)1);

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  int *v = malloc(sizeof(int));
  REQUIRE_NE((void *)v, NULL);
  *v = 7;
  c_message_t m = {.data = v, .size = sizeof(int)};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);

  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(pfd[0], ccol_select_read),
      ccol_selectable_from_dynq(dq, ccol_select_read),
  };
  /* The fd's first win comes within 8 calls. */
  int first_fd_win = -1;
  size_t failures = 0;
  for (int i = 0; i < 8 && first_fd_win < 0; i++) {
    size_t idx = 99;
    if (ccol_select_timed(&idx, 2, sels, 1000000) != ccol_success)
      failures++;
    else if (idx == 0)
      first_fd_win = i;
  }
  size_t fd_wins = 0;
  for (int i = 0; i < 2000; i++) {
    size_t idx = 99;
    if (ccol_select(&idx, 2, sels) != ccol_success || idx > 1)
      failures++;
    else if (idx == 0)
      fd_wins++;
  }

  c_message_t out;
  REQUIRE_EQ(ccol_dynmq_try_recv_zc(dq, &out), ccol_success);
  free(out.data);
  ccol_dynamic_queue_destroy(dq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(failures, (size_t)0);
  REQUIRE_GE(first_fd_win, 0);
  REQUIRE_GE(fd_wins, (size_t)800);
  REQUIRE_LE(fd_wins, (size_t)1200);
}

/* Two pipes that both stay readable and no queue in the set, so the call's
 * own wait reports both fds, and each should win about half of the calls.
 * Choosing in array order among the fds one wait reports would hand every
 * call to index 0. */
TEST(ccol_select, two_always_readable_fds_share_the_calls) {
  int p0[2];
  int p1[2];
  REQUIRE_EQ(pipe(p0), 0);
  REQUIRE_EQ(pipe(p1), 0);
  char byte = 'x';
  REQUIRE_EQ(write(p0[1], &byte, 1), (ssize_t)1);
  REQUIRE_EQ(write(p1[1], &byte, 1), (ssize_t)1);

  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(p0[0], ccol_select_read),
      ccol_selectable_from_fd(p1[0], ccol_select_read),
  };
  size_t wins[2] = {0, 0};
  size_t failures = 0;
  for (int i = 0; i < 1000; i++) {
    size_t idx = 99;
    if (ccol_select(&idx, 2, sels) != ccol_success || idx > 1)
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

/* A call that a ready queue answers at once builds no epoll set for the idle
 * fd next to it, and neither does a call that its ready fd answers or that
 * a timeout of 0 ends; only a call that has to wait builds one. A call that
 * builds its epoll set before looking at its selectables counts one set per
 * call, and the count shows it. */
TEST(ccol_select, a_call_that_returns_at_once_creates_no_poll_set) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  int *v = malloc(sizeof(int));
  bool setup = dq != NULL && cq != NULL && v != NULL;
  c_message_t m = {.data = v, .size = sizeof(int)};
  if (setup) setup = ccol_dynmq_send_zc(dq, &m) == ccol_success;
  if (!setup) free(v);

  uint64_t ready_queue_sets = UINT64_MAX, ready_fd_sets = UINT64_MAX;
  uint64_t zero_timeout_sets = UINT64_MAX, waiting_sets = UINT64_MAX;
  size_t failures = 0;
  ccol_retval_t rv_zero = ccol_success, rv_wait = ccol_success;
  if (setup) {
    ccol_selectable mixed[2] = {
        ccol_selectable_from_fd(pfd[0], ccol_select_read),
        ccol_selectable_from_dynq(dq, ccol_select_read),
    };
    uint64_t before = _ccol_select_test_poll_sets_created();
    for (int i = 0; i < 100; i++) {
      size_t idx = 99;
      if (ccol_select(&idx, 2, mixed) != ccol_success || idx != 1) failures++;
    }
    ready_queue_sets = _ccol_select_test_poll_sets_created() - before;

    char byte = 'x';
    if (write(pfd[1], &byte, 1) != 1) failures++;
    ccol_selectable fd_only = ccol_selectable_from_fd(pfd[0], ccol_select_read);
    before = _ccol_select_test_poll_sets_created();
    for (int i = 0; i < 100; i++) {
      size_t idx = 99;
      if (ccol_select(&idx, 1, &fd_only) != ccol_success || idx != 0)
        failures++;
    }
    ready_fd_sets = _ccol_select_test_poll_sets_created() - before;
    if (read(pfd[0], &byte, 1) != 1) failures++;

    ccol_selectable idle[2] = {
        ccol_selectable_from_fd(pfd[0], ccol_select_read),
        ccol_selectable_from_circq(cq, ccol_select_read),
    };
    size_t idx = 99;
    before = _ccol_select_test_poll_sets_created();
    rv_zero = ccol_select_timed(&idx, 2, idle, 0);
    zero_timeout_sets = _ccol_select_test_poll_sets_created() - before;
    before = _ccol_select_test_poll_sets_created();
    rv_wait = ccol_select_timed(&idx, 2, idle, 20000);
    waiting_sets = _ccol_select_test_poll_sets_created() - before;

    c_message_t out;
    if (ccol_dynmq_try_recv_zc(dq, &out) == ccol_success) free(out.data);
  }
  if (dq) ccol_dynamic_queue_destroy(dq);
  if (cq) ccol_circular_queue_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_TRUE(setup);
  REQUIRE_EQ(failures, (size_t)0);
  REQUIRE_EQ(ready_queue_sets, (uint64_t)0);
  REQUIRE_EQ(ready_fd_sets, (uint64_t)0);
  REQUIRE_EQ(rv_zero, ccol_timed_out);
  REQUIRE_EQ(zero_timeout_sets, (uint64_t)0);
  REQUIRE_EQ(rv_wait, ccol_timed_out);
  REQUIRE_EQ(waiting_sets, (uint64_t)1);
}

/* A queue selectable that a writer fills while the call waits next to an
 * idle fd wakes the call through the eventfd the call made when it got
 * ready to wait. */
typedef struct {
  ccol_circular_queue *cq;
  int value;
} sel_late_send_arg;

static void *sel_late_send_thread(void *arg) {
  sel_late_send_arg *a = (sel_late_send_arg *)arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 30000000L};
  nanosleep(&ts, NULL);
  c_message_t m = {.data = &a->value, .size = sizeof(int)};
  (void)ccol_circq_send_zc(a->cq, &m);
  return NULL;
}

TEST(ccol_select, a_queue_beside_an_idle_fd_wakes_a_waiting_call) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  ccol_retval_t rv = ccol_unexpected_failure;
  size_t idx = 99;
  bool started = false;
  sel_late_send_arg arg = {.cq = cq, .value = 5};
  pthread_t t;
  if (cq) {
    started = pthread_create(&t, NULL, sel_late_send_thread, &arg) == 0;
    if (started) {
      ccol_selectable sels[2] = {
          ccol_selectable_from_fd(pfd[0], ccol_select_read),
          ccol_selectable_from_circq(cq, ccol_select_read),
      };
      rv = ccol_select_timed(&idx, 2, sels, 5000000);
      pthread_join(t, NULL);
      c_message_t out;
      (void)ccol_circq_try_recv_zc(cq, &out);
    }
    ccol_circular_queue_destroy(cq);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)1);
}

/* Every allocation ccol_select_timed() makes (the waiter nodes, the fds'
 * state and the wait's event buffer) reports ccol_not_enough_memory when it
 * fails and leaves no waiter linked. Treat the fds' state or the event
 * buffer as a failed system call and the call reports
 * ccol_unexpected_failure instead. */
TEST(ccol_select, a_failed_allocation_reports_not_enough_memory) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  ccol_retval_t rv[4] = {ccol_success, ccol_success, ccol_success,
                         ccol_success};
  ccol_retval_t rv_queues_only = ccol_success;
  size_t waiters_left = SIZE_MAX;
  bool created = cq != NULL;
  if (cq) {
    ccol_selectable sels[2] = {
        ccol_selectable_from_fd(pfd[0], ccol_select_read),
        ccol_selectable_from_circq(cq, ccol_select_read),
    };
    for (uint32_t k = 0; k < 4; k++) {
      size_t idx = 99;
      _ccol_select_test_fail_alloc(k + 1);
      rv[k] = ccol_select_timed(&idx, 2, sels, 10000);
      _ccol_select_test_fail_alloc(0);
    }
    waiters_left = _ccol_circq_test_sel_read_waiter_count_for_tests(cq);
    size_t idx = 99;
    _ccol_select_test_fail_alloc(1);
    rv_queues_only = ccol_select_timed(&idx, 1, &sels[1], 10000);
    _ccol_select_test_fail_alloc(0);
    ccol_circular_queue_destroy(cq);
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

/* An fd's stripe comes from the high bits of its hash, so fds that share
 * their low bits still spread over every stripe. Reducing modulo the stripe
 * count would put even fds on 2 of 4 stripes and fds 4 apart on 1. */
TEST(ccol_event_loop, fds_that_share_low_bits_spread_over_every_stripe) {
  size_t even[4] = {0, 0, 0, 0};
  size_t step4[4] = {0, 0, 0, 0};
  bool in_range = true;
  for (int fd = 0; fd < 1000; fd += 2) {
    size_t s = _ccol_event_loop_test_stripe_index(4, fd);
    if (s >= 4)
      in_range = false;
    else
      even[s]++;
  }
  for (int fd = 0; fd < 1000; fd += 4) {
    size_t s = _ccol_event_loop_test_stripe_index(4, fd);
    if (s >= 4)
      in_range = false;
    else
      step4[s]++;
  }
  bool odd_range = true;
  for (int fd = 0; fd < 100; fd++) {
    if (_ccol_event_loop_test_stripe_index(7, fd) >= 7) odd_range = false;
    if (_ccol_event_loop_test_stripe_index(1, fd) != 0) odd_range = false;
  }
  REQUIRE_TRUE(in_range);
  REQUIRE_TRUE(odd_range);
  /* 500 even fds and 250 fds 4 apart, and each stripe holds at least 15
   * percent of them. */
  for (int k = 0; k < 4; k++) {
    REQUIRE_GE(even[k], (size_t)75);
    REQUIRE_GE(step4[k], (size_t)37);
  }
}

// ccol_event_loop test helpers
typedef struct evl_sync_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int readable_count;
  int writable_count;
  int error_count;
  c_message_t last_msg;
  bool last_msg_valid;
  ccol_select_dir last_dir_seen;
  ccol_event_reg self_reg; /* for self-removal tests */
  ccol_event_loop self_loop;
} evl_sync_ctx;

static void evl_sync_ctx_init(evl_sync_ctx *c) {
  /* We check these calls because an unchecked failure here leaves c->mtx
   * and c->cond uninitialized, and every later pthread_mutex_lock and
   * pthread_cond_wait call on them in this file is then undefined behavior,
   * including a silent, endless hang if the uninitialized bytes happen to
   * look like a locked mutex. This helper only runs from ordinary test bodies
   * in this file, never from a forked child, so an assert is safe here; a
   * pthread_create inside a forked child elsewhere in this file is a
   * different case. */
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->readable_count = 0;
  c->writable_count = 0;
  c->error_count = 0;
  c->last_msg = (c_message_t){.data = NULL, .size = 0};
  c->last_msg_valid = false;
  c->self_reg = CCOL_EVENT_REG_INVALID;
  c->self_loop = CCOL_EVENT_LOOP_INVALID;
}

static void evl_sync_ctx_destroy(evl_sync_ctx *c) {
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

/* ccol_event_loop never does the receive itself, for any type of
 * selectable, which matches the contract of _ccol_event_loop_run_callback.
 * So a queue-backed registration needs to call ccol_circq_try_recv_zc or
 * ccol_dynmq_try_recv_zc here itself, exactly as a real caller does. A
 * result other than success leaves last_msg_valid false and this function
 * doesn't assert, because it means a consumer on another thread already took
 * the message, the same time-of-check to time-of-use gap that a win in the
 * write direction always has. */
static void evl_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  bool got_msg = false;
  if (sel->type == ccol_selectable_circq) {
    got_msg = (ccol_circq_try_recv_zc(sel->cq, &msg) == ccol_success);
  } else if (sel->type == ccol_selectable_dynq) {
    got_msg = (ccol_dynmq_try_recv_zc(sel->dq, &msg) == ccol_success);
  }
  pthread_mutex_lock(&c->mtx);
  c->readable_count++;
  c->last_dir_seen = sel->dir;
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  if (got_msg) {
    c->last_msg = msg;
    c->last_msg_valid = true;
  } else {
    c->last_msg = (c_message_t){.data = NULL, .size = 0};
    c->last_msg_valid = false;
  }
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->writable_count++;
  c->last_dir_seen = sel->dir;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_error(ccol_event_loop loop, ccol_event_reg reg,
                         ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->error_count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

/* An on_readable handler that removes its own registration from inside the
 * callback and then behaves exactly like evl_on_readable. */
static void evl_on_readable_self_remove(ccol_event_loop loop,
                                        ccol_event_reg reg,
                                        ccol_selectable *sel, void *arg) {
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  /* assert() instead of REQUIRE_EQ, because this callback runs on the
   * reactor thread or the dispatch thread of ccol_event_loop at the same
   * time as the main test thread's REQUIRE_EQ calls. Tau's assertion
   * machinery uses plain globals with no lock and no thread-local storage
   * (see tau.h), so calling it from here is a real data race the moment this
   * assertion fails, and failing is exactly what this assertion is there to
   * catch. It also wouldn't abort the TEST() around it the way a top-level
   * REQUIRE_EQ does, only this function's stack frame would return early.
   * Every other background-thread helper in this file sticks to assert()
   * for the same reason. */
  assert(reg == c->self_reg);
  assert(ccol_event_loop_remove(c->self_loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, arg);
}

/* Waits until *counter_field >= target or until timeout_ms passes, and
 * returns true if *counter_field got to the target before the deadline. */
static bool evl_wait_for(evl_sync_ctx *c, int *counter_field, int target,
                         int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&c->mtx);
  while (*counter_field < target) {
    int r = pthread_cond_timedwait(&c->cond, &c->mtx, &deadline);
    if (r == ETIMEDOUT) break;
  }
  bool ok = (*counter_field >= target);
  pthread_mutex_unlock(&c->mtx);
  return ok;
}

/* Several of the multi-thread tests below let their on_readable callback
 * call read() itself instead of leaving the read to the single-threaded
 * test driver, which is what the fd-selectable path of evl_on_readable
 * documents for the same reason. That's how a real production callback
 * behaves under really concurrent dispatch, and it makes a *blocking* read
 * unsafe. The ccol_event_loop documentation warns that a callback's receive
 * call "may find nothing, and must handle that gracefully", and a plain
 * blocking read() isn't graceful on an fd with nothing left to read and no
 * writer left to produce more. That state happens, for example, once every
 * feeder thread and driver thread in a test has finished: read() then hangs
 * the reactor thread that called it for ever, which in turn hangs the join
 * of ccol_event_loop_shutdown on that thread. It's a real deadlock, and
 * thread-apply-all-bt in gdb on the stuck test process shows exactly that
 * pair of stacks. With O_NONBLOCK "nothing available" returns -1 with EAGAIN
 * at once instead. */
static void evl_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ccol_event_loop tests
TEST(ccol_event_loop, create_destroy) {
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);
  ccol_event_loop_destroy(loop);
  REQUIRE_EQ(loop, CCOL_EVENT_LOOP_INVALID);
}

TEST(ccol_event_loop, create_destroy_scoped) {
  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);
    REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);
  }
  /* The scope exit destroyed loop. There's nothing more to assert than "no
   * crash, and a clean run under valgrind", which the memtest target
   * checks. */
}

TEST(ccol_event_loop, fd_on_readable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool last_msg_valid;

  {
    /* A nested block, the same pattern as in the multi-thread tests; the
     * comment on fd_modify_flips_direction_and_updates_sel explains it in
     * full. By design evl_on_readable never drains an fd selectable's
     * data, since the caller reads sel->fd itself, as this test does below.
     * So once pfd[0] becomes readable it stays ready, the reactor being
     * level-triggered, and the reactor thread keeps dispatching it until the
     * registration goes away. The test must not destroy ctx, and this
     * function must not return, until that has surely stopped, and only the
     * join at the end of this block makes sure of it. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);

    int val = 42;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
    /* This read happens under ctx.mtx, so it isn't unprotected; the same
     * last_dir_seen handling in fd_modify_flips_direction_and_updates_sel
     * explains why. */
    pthread_mutex_lock(&ctx.mtx);
    last_msg_valid = ctx.last_msg_valid;
    pthread_mutex_unlock(&ctx.mtx);

    char rbuf[16];
    ssize_t n = read(pfd[0], rbuf, sizeof(rbuf));
    REQUIRE_EQ(n, (ssize_t)sizeof(val));
    REQUIRE_EQ(*(int *)rbuf, 42);

    ccol_event_loop_remove(loop, reg);

    /* Leaving the block shuts loop down and joins its one reactor thread, so
     * ctx is quiet from here on. */
  }

  /* An fd selectable never gets a msg, the caller reads sel->fd itself. */
  REQUIRE_FALSE(last_msg_valid);

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_on_writable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. A new pipe write end stays writable for ever,
     * since nothing fills its buffer, so the reactor thread keeps
     * dispatching it until the registration goes away. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[1], ccol_select_write), handlers,
        &ctx, &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    /* A new pipe write end is always writable from the start. */
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    ccol_event_loop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_both_directions_combine_and_recombine) {
  /* A socketpair gives an fd that works in both directions, which a pipe
   * doesn't, and the test needs that to register read and write interest on
   * the SAME fd. This covers adding with EPOLL_CTL_ADD and then combining
   * with MOD, and also the MOD back down on a partial removal. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. Both directions here stay ready for ever with a
     * level-triggered reactor, because evl_on_readable never drains the read
     * side of sv[0] and the write side never fills up. So both ctx structs
     * must outlive every reactor thread, outliving their own
     * ccol_event_loop_remove call isn't enough. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx,
        &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)2);

    /* sv[0] is writable right away because its send buffer is empty. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    /* Removes the write direction, which is the MOD back down path, and the
     * read direction keeps working after it. */
    REQUIRE_EQ(ccol_event_loop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

    ccol_event_loop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_event_loop, fd_simultaneous_readable_and_writable) {
  /* The test registers both directions on the same fd and then the peer
   * writes, so sv[0] becomes readable while it's writable, and both
   * callbacks should fire for the SAME epoll_wait batch. That's why
   * ev.data.ptr carries the shared event_entry instead of a bare
   * ccol_event_reg*, which would deliver only one of the two. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx,
        &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

    /* Drains the initial "already writable" dispatch before the test writes
     * data, so the later wait sees the combined batch without any doubt. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    int val = 99;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));
    /* sv[0] stays writable the whole time since nothing fills its send
     * buffer, so the writable callback fires again too. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 2, 2000));

    ccol_event_loop_remove(loop, rreg);
    ccol_event_loop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_event_loop, fd_on_error_fires_for_both_directions) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. An EPOLLHUP or EPOLLERR condition from a peer
     * that hung up stays until the fd goes away with a level-triggered
     * reactor, just like a readable or writable fd nobody drains, so the
     * reactor thread keeps dispatching on_error until then. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t rh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx,
        &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

    close(sv[1]); /* peer hangs up */

    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.error_count, 1, 2000));
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.error_count, 1, 2000));

    ccol_event_loop_remove(loop, rreg);
    ccol_event_loop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
}

/* This guards against an asymmetric dispatch priority; the has_writer
 * comment in _ccol_event_loop_handle_event in src/cthreadcomm.c describes
 * the mechanism. EPOLLOUT and EPOLLERR or EPOLLHUP aren't mutually exclusive,
 * and neither are EPOLLIN and EPOLLERR or EPOLLHUP, as the setup of
 * fd_on_error_fires_for_both_directions just above shows, and this test
 * mirrors it. A socket whose peer just hung up reports both states at once:
 * writable, because the local send buffer has room (so a write(2) on it
 * returns at once with an error rather than blocking), and an error. A
 * standalone epoll probe against this exact socketpair shape reports both
 * directly. A write-only registration sets on_writable and leaves on_error
 * == NULL, which is a documented, supported pattern, since the doc comment
 * of ccol_event_handlers_t says "a write-only producer that never expects
 * on_error may pass NULL there". Such a registration has to see on_writable
 * fire for such an event instead of being starved of it for ever with
 * nothing to report the error that comes with it either.
 * num_reactor_threads == 1 here covers the copy of this handling in
 * _ccol_event_loop_handle_event, and the _multi_thread sibling test below
 * covers the copy in _ccol_event_loop_poller_collect. */
TEST(ccol_event_loop,
     fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. The EPOLLHUP condition from a peer that hung up
     * stays until the fd goes away with a level-triggered reactor, so the
     * reactor thread keeps dispatching on_writable until then. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

    close(sv[1]); /* peer hangs up: sv[0] becomes both EPOLLOUT and EPOLLHUP */

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    ccol_event_loop_remove(loop, wreg);
  }

  /* on_error is NULL on this registration, so nothing would consume the
   * error condition that comes with the event, and the loop must never
   * dispatch it. */
  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

/* The same as the test just above but with num_reactor_threads == 3, which
 * routes the dispatch through _ccol_event_loop_poller_collect and
 * _ccol_event_loop_dispatch_job_fn instead of _ccol_event_loop_handle_event.
 * That path has its own separate copy of the same has_writer handling. The
 * two copies are almost identical on purpose and aren't shared, so the
 * num_reactor_threads == 1 path stays byte for byte the same, and this
 * module's standing design note covers that duplication. The second copy
 * needs regression coverage of its own because being correct doesn't carry
 * over from the first. */
TEST(
    ccol_event_loop,
    fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up_multi_thread) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 3);

    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

    close(sv[1]);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    ccol_event_loop_remove(loop, wreg);
  }

  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

/* A registration of an fd whose earlier registration was removed. The two
 * tests below register an fd, remove that registration, register the same
 * fd number again and wait until the loop has reclaimed the removed
 * registration's entry. Only then do they make the fd readable, and they
 * require the new registration to fire. A loop that lets a removed
 * registration's entry change its fd's epoll interest later (an
 * EPOLL_CTL_DEL or EPOLL_CTL_MOD keyed by the fd number, from the reclaim or
 * from the re-arm after a dispatch that was in flight at the removal) takes
 * the new registration out of the interest set, so it never fires. An
 * EPOLL_CTL_DEL of the fd in the entry reclaim, for example, makes every
 * case fail. */

/* The gate that holds evl_readd_on_readable's callback while it's closed,
 * and the flag the callback sets once it's inside. Only the in-flight case
 * closes the gate. */
static atomic_bool evl_readd_gate_closed;
static atomic_bool evl_readd_in_callback;

/* Drains the fd and counts one readable dispatch. The drain doesn't block,
 * since a dispatch may find nothing left to read, and the wait on the gate
 * is bounded, so a callback can never hold the reactor for ever. */
static void evl_readd_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg) {
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

/* Waits, bounded by timeout_ms, until loop has reclaimed more than base
 * event entries. */
static bool evl_readd_wait_reclaimed(ccol_event_loop loop, uint64_t base,
                                     int timeout_ms) {
  for (int i = 0; i < timeout_ms; i++) {
    if (_ccol_event_loop_entries_reclaimed_for_tests(loop) > base) return true;
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  return _ccol_event_loop_entries_reclaimed_for_tests(loop) > base;
}

static int evl_readd_count(evl_sync_ctx *c) {
  pthread_mutex_lock(&c->mtx);
  int n = c->readable_count;
  pthread_mutex_unlock(&c->mtx);
  return n;
}

typedef enum {
  EVL_READD_PLAIN,       /* remove, add the same fd again */
  EVL_READD_PAUSED,      /* pause, remove, add the same fd again */
  EVL_READD_NUMBER_REUSE /* remove, close, a new socket on the same number */
} evl_readd_mode;

/* Runs one case on a fresh loop with num_threads reactor threads. Returns a
 * bit set of failures (0 on success) and releases everything on every
 * path. */
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
  ccol_event_loop loop = ccol_event_loop_create(8, 1, num_threads, &err);
  if (loop == CCOL_EVENT_LOOP_INVALID) {
    fail |= 1 << 1;
  } else {
    ccol_event_handlers_t h = {.on_readable = evl_readd_on_readable};
    int fd = sv[0];
    ccol_event_reg r1 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(fd, ccol_select_read), h, &old_ctx, &err);
    if (r1 == CCOL_EVENT_REG_INVALID) fail |= 1 << 2;
    if (mode == EVL_READD_PAUSED &&
        ccol_event_loop_pause(loop, r1) != ccol_success)
      fail |= 1 << 3;
    uint64_t base = _ccol_event_loop_entries_reclaimed_for_tests(loop);
    if (ccol_event_loop_remove(loop, r1) != ccol_success) fail |= 1 << 4;

    if (mode == EVL_READD_NUMBER_REUSE) {
      /* The documented order, removal before close. The new socket takes the
       * old number, directly when it's the lowest free one and through dup2
       * otherwise, so the number gets reused whatever else the process
       * opened in the meantime. */
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

    ccol_event_reg r2 = CCOL_EVENT_REG_INVALID;
    if (sv[0] >= 0) {
      r2 = ccol_event_loop_add(loop,
                               ccol_selectable_from_fd(sv[0], ccol_select_read),
                               h, &new_ctx, &err);
    }
    if (r2 == CCOL_EVENT_REG_INVALID) fail |= 1 << 6;
    /* r1's entry is freed only here, after r2 owns the fd. */
    if (!evl_readd_wait_reclaimed(loop, base, 5000)) fail |= 1 << 7;

    if (r2 != CCOL_EVENT_REG_INVALID && sv[1] >= 0) {
      test_write_retry_eintr(sv[1], "x", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, 1, 5000))
        fail |= 1 << 8;
      /* A second event proves the interest stays armed after the first
       * dispatch (the EPOLLONESHOT re-arm of a dispatch pool). */
      test_write_retry_eintr(sv[1], "y", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, 2, 5000))
        fail |= 1 << 9;
    }
    if (r2 != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r2);
    ccol_event_loop_destroy(loop);
  }
  if (evl_readd_count(&old_ctx) != 0) fail |= 1 << 10;

  evl_sync_ctx_destroy(&old_ctx);
  evl_sync_ctx_destroy(&new_ctx);
  if (sv[0] >= 0) close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  return fail;
}

TEST(ccol_event_loop, fd_readded_after_remove_fires_after_old_entry_reclaim) {
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

/* Removing a registration whose callback is in flight, and registering the
 * same fd again while that callback is running. Then the callback returns,
 * and with a dispatch pool its job runs the post-dispatch re-arm for the
 * removed registration's entry. Neither that step nor the later reclaim of
 * that entry may touch the interest the new registration installed. */
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
  ccol_event_loop loop = ccol_event_loop_create(8, 1, num_threads, &err);
  if (loop == CCOL_EVENT_LOOP_INVALID) {
    fail |= 1 << 1;
  } else {
    ccol_event_handlers_t h = {.on_readable = evl_readd_on_readable};
    ccol_event_reg r1 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), h, &old_ctx,
        &err);
    if (r1 == CCOL_EVENT_REG_INVALID) fail |= 1 << 2;
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

    uint64_t base = _ccol_event_loop_entries_reclaimed_for_tests(loop);
    if (ccol_event_loop_remove(loop, r1) != ccol_success) fail |= 1 << 4;
    ccol_event_reg r2 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), h, &new_ctx,
        &err);
    if (r2 == CCOL_EVENT_REG_INVALID) fail |= 1 << 5;

    /* r1's callback drains the byte "a" at this point and returns. */
    atomic_store(&evl_readd_gate_closed, false);
    if (!evl_wait_for(&old_ctx, &old_ctx.readable_count, 1, 5000))
      fail |= 1 << 6;
    if (!evl_readd_wait_reclaimed(loop, base, 5000)) fail |= 1 << 7;

    if (r2 != CCOL_EVENT_REG_INVALID) {
      int before = evl_readd_count(&new_ctx);
      test_write_retry_eintr(sv[1], "b", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, before + 1, 5000))
        fail |= 1 << 8;
      ccol_event_loop_remove(loop, r2);
    }
    ccol_event_loop_destroy(loop);
  }
  atomic_store(&evl_readd_gate_closed, false);

  evl_sync_ctx_destroy(&old_ctx);
  evl_sync_ctx_destroy(&new_ctx);
  close(sv[0]);
  close(sv[1]);
  return fail;
}

TEST(ccol_event_loop, fd_readded_while_removed_dispatch_in_flight_fires) {
  int fail_single = evl_readd_inflight_run(1);
  int fail_pool = evl_readd_inflight_run(3);
  REQUIRE_EQ(fail_single, 0);
  REQUIRE_EQ(fail_pool, 0);
}

TEST(ccol_event_loop, fd_duplicate_direction_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg1 = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg1, CCOL_EVENT_REG_INVALID);

  err = NULL;
  ccol_event_reg reg2 = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_EQ(reg2, CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);

  ccol_event_loop_remove(loop, reg1);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_modify_rejects_occupied_direction) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg rreg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(sv[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
  ccol_event_reg wreg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(sv[0], ccol_select_write), handlers, NULL,
      &err);
  REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

  /* Flipping rreg to the write direction collides with wreg. */
  REQUIRE_EQ(ccol_event_loop_modify(loop, rreg, ccol_select_write),
             ccol_not_permitted);

  ccol_event_loop_remove(loop, rreg);
  ccol_event_loop_remove(loop, wreg);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_event_loop, fd_modify_flips_direction_and_updates_sel) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_select_dir last_dir_seen;

  {
    /* A nested block, the same pattern as in the other multi-thread tests.
     * evl_on_readable never drains sv[0] for this fd selectable and nothing
     * in this test reads it, so once sv[0] becomes readable it stays ready
     * with a level-triggered reactor, and the reactor thread keeps
     * dispatching it until the registration goes away. The test must not
     * destroy ctx, and this function must not return and free ctx's stack
     * slot, until that has surely stopped, and only the join at the end of
     * this block makes sure of it. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                      .on_writable = evl_on_writable,
                                      .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    REQUIRE_EQ(ccol_event_loop_modify(loop, reg, ccol_select_read),
               ccol_success);

    int val = 5;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    /* Reads under ctx.mtx directly instead of going through evl_wait_for,
     * whose contract only covers the counter. evl_on_readable keeps writing
     * last_dir_seen, because it never drains sv[0] here, for as long as the
     * registration is live and the reactor thread keeps dispatching, so a
     * plain unprotected read right after evl_wait_for returns races those
     * writes. ThreadSanitizer reports that race here even at
     * num_reactor_threads == 1, since it has nothing to do with dispatch on
     * many threads: one reactor thread re-dispatching a level-triggered fd
     * nobody drains is already enough. */
    pthread_mutex_lock(&ctx.mtx);
    last_dir_seen = ctx.last_dir_seen;
    pthread_mutex_unlock(&ctx.mtx);

    ccol_event_loop_remove(loop, reg);

    /* Leaving the block shuts loop down and joins its one reactor thread, so
     * ctx is quiet from here on. */
  }

  REQUIRE_EQ((int)last_dir_seen, (int)ccol_select_read);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_event_loop, fd_modify_after_remove_returns_invalid_args) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_modify_rejects_queue_selectable) {
  /* ccol_event_loop_modify only works for an fd, and ccol_event_loop_pause
   * and ccol_event_loop_resume have the same restriction (see
   * pause_and_resume_reject_queue_selectable). The direction of a queue or a
   * ccol_channel registration is part of its identity, so to change it the
   * caller removes the registration and adds it again rather than flipping
   * the direction in place. So the call has to fail with ccol_invalid_args,
   * neither quietly doing nothing nor corrupting the registration. The type
   * check always runs first, before the "already in the requested
   * direction" check inside ccol_event_loop_modify, so a call for the same
   * direction fails the same way instead of counting as a success that does
   * nothing. */
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);
  /* Asking for the direction the registration already has must fail the
   * same way instead of counting as a call that does nothing. */
  REQUIRE_EQ(ccol_event_loop_modify(loop, reg, ccol_select_read),
             ccol_invalid_args);

  ccol_event_loop_remove(loop, reg);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_circq_readable_delivers_message) {
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int *payload = malloc(sizeof(int));
  *payload = 123;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(ctx.last_msg.size, sizeof(int));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 123);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_circq_writable_fires_without_consuming) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);

  /* Fill the queue so the write direction isn't ready from the start. */
  int *filler = malloc(sizeof(int));
  *filler = 1;
  c_message_t fmsg = {.data = filler, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &fmsg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. By design evl_on_writable never consumes or
     * produces anything on cq, to prove the reactor never does it for the
     * callback. The test itself then does a ccol_circq_try_send_zc and
     * ccol_circq_recv_zc round trip after the first wait has succeeded, and
     * that round trip makes cq ready for a write again. So a second
     * on_writable dispatch can still be in flight on the reactor thread at
     * the exact moment this function would go on to remove the registration
     * and destroy ctx and cq. ccol_event_loop_remove only stops a FUTURE
     * dispatch from starting and doesn't wait for one already in flight (see
     * the comment of _ccol_event_loop_run_callback). The test must not
     * destroy ctx or cq, and this function must not return, until that has
     * surely stopped, and only the join at the end of this block, which comes
     * from the scope-exit destructor of ccol_event_loop_construct_scoped,
     * makes sure of it. The race is real and not theoretical: without this
     * nested block ThreadSanitizer reports, in about 1 of every 4 to 9 runs,
     * that the main thread destroys ctx.mtx and ctx.cond while
     * evl_on_writable is still using them on the reactor thread. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_write), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    /* Frees the slot, so on_writable fires and must NOT consume
     * anything: the queue has room and holds no message, and the callback
     * does the real send itself. */
    c_message_t recvd;
    REQUIRE_EQ(ccol_circq_recv_zc(cq, &recvd), ccol_success);
    free(recvd.data);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    int *payload = malloc(sizeof(int));
    *payload = 55;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(ccol_circq_try_send_zc(cq, &msg), ccol_success);
    REQUIRE_EQ(ccol_circq_msg_count(cq), (size_t)1);

    c_message_t out;
    REQUIRE_EQ(ccol_circq_recv_zc(cq, &out), ccol_success);
    REQUIRE_EQ(*(int *)out.data, 55);
    free(out.data);

    ccol_event_loop_remove(loop, reg);

    /* Leaving the block shuts loop down and joins its one reactor thread, so
     * ctx and cq are quiet from here on. */
  }

  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_dynq_readable_delivers_message) {
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg =
      ccol_event_loop_add(loop, ccol_selectable_from_dynq(dq, ccol_select_read),
                          handlers, &ctx, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int *payload = malloc(sizeof(int));
  *payload = 321;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 321);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_dynamic_queue_destroy(dq);
}

typedef struct evl_chan_sender_args {
  ccol_channel *ch;
  int value;
} evl_chan_sender_args;

static void *evl_chan_sender_thread(void *arg) {
  evl_chan_sender_args *a = (evl_chan_sender_args *)arg;
  int *payload = malloc(sizeof(int));
  *payload = a->value;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  /* A new thread calls this, so _ccol_get_thread_id() differs from the
   * ccol_channel's owner (the test thread that called ccol_channel_create)
   * and the send goes to workers_to_owner_cq, which is exactly what the
   * owner-side ccol_select_read registration below watches. */
  ccol_chan_send_zc(a->ch, &msg);
  return NULL;
}

TEST(ccol_event_loop, queue_channel_selectable) {
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_channel *ch = ccol_channel_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  /* This thread owns the ccol_channel and the owner reads from
   * ccol_workers_to_owner, so a worker on another thread needs to send for
   * the owner-side read registration to fire. */
  ccol_event_reg reg =
      ccol_event_loop_add(loop, ccol_selectable_from_chan(ch, ccol_select_read),
                          handlers, &ctx, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  evl_chan_sender_args sargs = {.ch = ch, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, evl_chan_sender_thread, &sargs), 0);
  pthread_join(tid, NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 88);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_channel_destroy(ch);
}

TEST(ccol_event_loop, queue_persistent_across_multiple_cycles) {
  /* The core property: a registration is persistent rather than one-shot,
   * so one registration keeps firing across many separate send and recv
   * cycles and the test never adds it again. */
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  for (int i = 0; i < 5; i++) {
    int *payload = malloc(sizeof(int));
    *payload = i;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, i + 1, 2000));
    REQUIRE_EQ(*(int *)ctx.last_msg.data, i);
  }

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_already_pending_message_at_registration_time) {
  /* The loop has to deliver a message the test sent BEFORE calling
   * ccol_event_loop_add. The bridge eventfd has no earlier notify to use, so
   * ccol_event_loop_add has to trigger itself when the queue is already in
   * the target state at registration time. */
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 999;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 999);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

typedef struct evl_two_readers_ctx {
  _Atomic size_t calls;
} evl_two_readers_ctx;

/* This callback is slower on purpose than a feeder that keeps refilling the
 * queue, the same shape as evl_bounded_hot_queue_on_readable further below.
 * So a real backlog of 2 or more messages is waiting behind this
 * registration by the time it dispatches, which gives
 * _ccol_event_loop_queue_cascade_notify_next something real to forward to
 * whichever registration is linked behind this one. */
static void evl_two_readers_slow_on_readable(ccol_event_loop loop,
                                             ccol_event_reg reg,
                                             ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)ccol_circq_try_recv_zc(sel->cq, &msg);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

static void evl_two_readers_fast_on_readable(ccol_event_loop loop,
                                             ccol_event_reg reg,
                                             ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)ccol_circq_try_recv_zc(sel->cq, &msg);
  atomic_fetch_add(&c->calls, 1);
}

typedef struct evl_two_readers_feeder_args {
  ccol_circular_queue *cq;
  int iterations;
} evl_two_readers_feeder_args;

static void *evl_two_readers_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    /* This send blocks. The queue's finite capacity is what stops this
     * feeder from racing far ahead of both registrations, while it still
     * builds a real backlog against the slow registration. */
    (void)ccol_circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

TEST(ccol_event_loop, queue_second_registration_on_same_queue_is_not_starved) {
  /* This guards against a silent starvation. notify_one_sel_waiter() wakes
   * only the CURRENT head of a queue's waiter list. A ccol_select() waiter
   * cascades that wake onward correctly, because it always unlinks itself
   * before checking readiness again, but a ccol_event_loop registration's
   * waiter_node stays linked into the list for good, until
   * ccol_event_loop_remove. Without
   * _ccol_event_loop_queue_cascade_notify_next, a second live registration
   * on the same queue and direction that sits BEHIND another one that never
   * unlinks gets ZERO notifications for ever, however much traffic the queue
   * sees.
   *
   * The test registers r1 FIRST so that r1 ends up linked behind r2, since
   * the registration the test adds last always becomes the new list head
   * (see the "prepend to head" linking in ccol_event_loop_add). Remove the
   * cascade forward and ctx1.calls stays at 0 in this exact scenario every
   * time. That isn't a flaky result either, because nothing else can reach
   * a registration sitting behind a head that never unlinks, so the bounded
   * wait below runs to its full timeout with ctx1.calls at 0 instead of
   * passing now and then. */
  ccol_circular_queue *cq = ccol_circular_queue_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg r1 = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers1,
        &ctx1, &err);
    REQUIRE_NE(r1, CCOL_EVENT_REG_INVALID);

    /* The test adds r2 second, so r2 becomes the new head of the list. Its
     * callback is slow on purpose, which lets a backlog build up that r2
     * then cascades forward toward r1. */
    ccol_event_handlers_t handlers2 = {
        .on_readable = evl_two_readers_slow_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_reg r2 = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers2,
        &ctx2, &err);
    REQUIRE_NE(r2, CCOL_EVENT_REG_INVALID);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 300};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_two_readers_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    /* A bounded wait. r2, the slow head, gets most of the count, and what
     * we test is that r1, the tail, gets ANY turn at all. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    for (;;) {
      if (atomic_load(&ctx1.calls) > 0 && atomic_load(&ctx2.calls) > 0) break;
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      if (now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        break;
      }
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    REQUIRE_GT(atomic_load(&ctx1.calls), (size_t)0);
    REQUIRE_GT(atomic_load(&ctx2.calls), (size_t)0);

    ccol_event_loop_remove(loop, r1);
    ccol_event_loop_remove(loop, r2);

    /* Leaving the block shuts loop down and joins its one reactor thread,
     * and only after that is it safe to touch or destroy cq. Before it, a
     * dispatch that's already in flight (an ordinary on_readable callback or
     * a cascade forward) can still use cq, the same hazard that
     * remove_from_different_thread_concurrent_with_dispatch above documents
     * and guards against. */
  }

  /* Drains what's left. A few messages may stay, which is expected and
   * correct, since it matches the documented coalescing behaviour, a
   * separate topic (see the comment of
   * multi_thread_hot_queue_dispatch_pool_pending_stays_bounded), and
   * ccol_circular_queue_destroy asserts on any message that's left. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  ccol_circular_queue_destroy(cq);
}

/* Feeds one message at a time and sleeps between the sends, so the queue
 * always has time to drain completely before the next message arrives.
 * That's the OPPOSITE traffic shape, on purpose, from
 * evl_two_readers_feeder_thread above, which floods the queue as fast as it
 * can to build a backlog. When no backlog ever exists the cascade has
 * nothing to forward, because the cascade
 * (_ccol_event_loop_queue_cascade_notify_next) depends on readiness, so this
 * feeder tests the round-robin rotor in notify_one_sel_waiter and not the
 * cascade. */
static void *evl_two_readers_no_backlog_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    (void)ccol_circq_send_zc(a->cq, &msg);
    struct timespec ts = {0, 200000}; /* 0.2ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

TEST(ccol_event_loop, queue_multiple_registrations_share_traffic_fairly) {
  /* This guards against a silent starvation no ordinary test finds:
   * notify_one_sel_waiter() must not always wake the CURRENT HEAD of a
   * queue's waiter list. Unlike the transient node of a ccol_select() caller,
   * the waiter_node of a ccol_event_loop registration stays linked for good
   * and never links again, so the registration the caller adds LAST becomes
   * the head and stays the head, and would be the ONLY one that ever gets a
   * direct notify for as long as it stays registered. The cascade forwards a
   * wake only when the queue is STILL ready right after the notified
   * registration's callback returns, and a callback that keeps up with the
   * traffic routinely leaves nothing to forward. That includes a plain
   * callback with a single ccol_circq_try_recv_zc() per call and no loop,
   * which is a documented, ordinary pattern and exactly what both
   * registrations below do. Every OTHER live registration would then get
   * ZERO callbacks for as long as traffic flows, which flatly contradicts the
   * documented guarantee of ccol_event_loop_add, "no live listener is ever
   * passed over indefinitely". The failure is total rather than occasional:
   * with head-only notification two registrations that get 2000 messages one
   * at a time end up with calls1=0 and calls2=2000 on every run. So each
   * queue and direction's waiter list carries its own round-robin rotor (see
   * the doc comment of notify_one_sel_waiter), and every live registration
   * gets a turn once per full rotation instead of the same one winning for
   * ever. */
  ccol_circular_queue *cq = ccol_circular_queue_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    /* Both registrations use the same "fast" callback on purpose, one recv
     * and no loop, because that's the exact pairing where head-only
     * notification starves one side completely. A "slow" head, like the one
     * the sibling test above uses, builds a backlog by hand that the cascade
     * can ride to reach r1, while two equally fast callbacks build no such
     * backlog at all. */
    ccol_event_handlers_t handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg r1 = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers1,
        &ctx1, &err);
    REQUIRE_NE(r1, CCOL_EVENT_REG_INVALID);

    ccol_event_handlers_t handlers2 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_reg r2 = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers2,
        &ctx2, &err);
    REQUIRE_NE(r2, CCOL_EVENT_REG_INVALID);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_two_readers_no_backlog_feeder_thread,
                       &feeder_args),
        0);
    pthread_join(feeder, NULL);

    /* Every message has been sent. Wait, at most about 10 s, until the
     * callbacks have taken the share the test requires, so a slow reactor
     * thread doesn't fail the count. The bound only stops a hang, and a loop
     * that never reaches 350 fails below anyway. */
    for (int i = 0;
         i < 10000 &&
         atomic_load(&ctx1.calls) + atomic_load(&ctx2.calls) < (size_t)350;
         i++) {
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    /* Both registrations must get a really fair share, not just "more than
     * zero". Head-only notification gives an exact 0 to N split, so a loose
     * ">0" bound alone would already mean something, but we assert that each
     * side gets at least a quarter of a perfectly even half-and-half split.
     * That leaves real margin against scheduling jitter and still fails hard
     * on all-or-nothing behavior. */
    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    ccol_event_loop_remove(loop, r1);
    ccol_event_loop_remove(loop, r2);
  }

  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  ccol_circular_queue_destroy(cq);
}

/* The ccol_dynamic_queue version of the ccol_circular_queue test above. The
 * rotor works the same way for ccol_dynamic_queue's sel_read_rotor and
 * sel_write_rotor fields, through the same notify_one_sel_waiter() and
 * _sel_unlink_waiter() code paths, so this test covers that half directly
 * instead of relying on the ccol_circular_queue coverage alone. */
typedef struct evl_dynq_two_readers_ctx {
  _Atomic size_t calls;
} evl_dynq_two_readers_ctx;

static void evl_dynq_two_readers_on_readable(ccol_event_loop loop,
                                             ccol_event_reg reg,
                                             ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_dynq_two_readers_ctx *c = (evl_dynq_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  if (ccol_dynmq_try_recv_zc(sel->dq, &msg) == ccol_success) {
    atomic_fetch_add(&c->calls, 1);
  }
}

typedef struct evl_dynq_feeder_args {
  ccol_dynamic_queue *dq;
  int iterations;
} evl_dynq_feeder_args;

static void *evl_dynq_no_backlog_feeder_thread(void *arg) {
  evl_dynq_feeder_args *a = (evl_dynq_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    (void)ccol_dynmq_send_zc(a->dq, &msg);
    struct timespec ts = {0, 200000}; /* 0.2ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

TEST(ccol_event_loop, dynq_multiple_registrations_share_traffic_fairly) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  evl_dynq_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers1 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg r1 = ccol_event_loop_add(
        loop, ccol_selectable_from_dynq(dq, ccol_select_read), handlers1, &ctx1,
        &err);
    REQUIRE_NE(r1, CCOL_EVENT_REG_INVALID);

    ccol_event_handlers_t handlers2 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_reg r2 = ccol_event_loop_add(
        loop, ccol_selectable_from_dynq(dq, ccol_select_read), handlers2, &ctx2,
        &err);
    REQUIRE_NE(r2, CCOL_EVENT_REG_INVALID);

    evl_dynq_feeder_args feeder_args = {.dq = dq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_dynq_no_backlog_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    /* A bounded wait for the share the test requires, as in the
     * ccol_circular_queue version above. */
    for (int i = 0;
         i < 10000 &&
         atomic_load(&ctx1.calls) + atomic_load(&ctx2.calls) < (size_t)350;
         i++) {
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    ccol_event_loop_remove(loop, r1);
    ccol_event_loop_remove(loop, r2);
  }

  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_dynmq_try_recv_zc(dq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  ccol_dynamic_queue_destroy(dq);
}

TEST(ccol_event_loop, remove_from_within_callback) {
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ctx.self_loop = loop;
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable_self_remove,
                                    .on_writable = NULL,
                                    .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  ctx.self_reg = reg;

  int *payload = malloc(sizeof(int));
  *payload = 1;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

  /* The loop must NOT deliver a second message, since the registration
   * removed itself. */
  int *payload2 = malloc(sizeof(int));
  *payload2 = 2;
  c_message_t msg2 = {.data = payload2, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg2), ccol_success);
  usleep(50000);
  REQUIRE_EQ(ctx.readable_count, 1);

  c_message_t drained;
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &drained), ccol_success);
  free(drained.data);

  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

typedef struct evl_remover_args {
  ccol_event_loop loop;
  ccol_event_reg reg;
  int delay_us;
} evl_remover_args;

static void *evl_remover_thread(void *arg) {
  evl_remover_args *a = (evl_remover_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccol_event_loop_remove(a->loop, a->reg);
  return NULL;
}

TEST(ccol_event_loop, remove_from_different_thread_concurrent_with_dispatch) {
  /* Repeats a cycle of add, notify and a concurrent remove from another
   * thread, a targeted stress test for the refcount design, which must never
   * crash and never leak (the memtest target checks that separately under
   * valgrind).
   *
   * Each iteration's ctx is allocated on the heap and logged instead of
   * destroyed inline. ccol_event_loop_remove is documented to return while a
   * dispatch for the removed reg may still be running, since it defers only
   * the library's own memory reclamation and not however long the callback
   * takes. So joining the remover thread doesn't prove that evl_on_readable
   * has stopped touching ctx, and destroying ctx.mtx, or reusing its stack
   * slot on the next iteration, would race that in-flight callback.
   * ThreadSanitizer reports that even at num_reactor_threads == 1, because
   * the hazard comes from the documented contract of ccol_event_loop_remove
   * and has nothing to do with dispatch on many threads. The test frees
   * every logged ctx only after joining the whole loop, which also covers
   * the one reactor thread that may still be inside a callback, and the
   * nested block below does that join. cq has the same hazard. The scoped
   * loop's automatic destructor runs at the closing brace of this function
   * and joins the reactor thread, and destroying cq before that join would
   * let a stale dispatch, already collected for the reg the last iteration
   * removed, call ccol_circq_try_recv_zc(sel->cq, ...) on a queue that's
   * already freed. So cq is destroyed only after that same join, exactly
   * like every logged ctx. */
  evl_sync_ctx *ctx_log[50];
  int ctx_log_count = 0;
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    for (int iter = 0; iter < 50; iter++) {
      evl_sync_ctx *ctx = malloc(sizeof(*ctx));
      evl_sync_ctx_init(ctx);
      ctx_log[ctx_log_count++] = ctx;
      ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                        .on_writable = NULL,
                                        .on_error = NULL};
      char *err = NULL;
      ccol_event_reg reg = ccol_event_loop_add(
          loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, ctx,
          &err);
      REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

      evl_remover_args rargs = {.loop = loop, .reg = reg, .delay_us = 0};
      pthread_t rtid;
      REQUIRE_EQ(pthread_create(&rtid, NULL, evl_remover_thread, &rargs), 0);

      int *payload = malloc(sizeof(int));
      *payload = iter;
      c_message_t msg = {.data = payload, .size = sizeof(int)};
      ccol_circq_send_zc(cq, &msg); /* delivery is optional here, and correct */

      pthread_join(rtid, NULL);

      /* Drains what's left so the queue doesn't grow without bound across
       * the iterations, since nothing consumes the message when the removal
       * wins the race against the dispatch. */
      c_message_t leftover;
      while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
        free(leftover.data);
      }
    }

    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

    /* Leaving the block shuts loop down and joins its one reactor thread,
     * so cq and every logged ctx are quiet from here on. */
  }

  ccol_circular_queue_destroy(cq);
  for (int i = 0; i < ctx_log_count; i++) {
    evl_sync_ctx_destroy(ctx_log[i]);
    free(ctx_log[i]);
  }
}

/* A "slow callback" fixture the test controls, for the ordering guarantee
 * of on_removed. Unlike evl_sync_ctx above it needs an explicit gate under
 * the test's control, to hold a dispatch open on purpose and so mimic a
 * callback that's really in flight, which no field of evl_sync_ctx gives. */
typedef struct evl_slow_cb_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool callback_started;
  bool release_callback;
  bool callback_finished;
  bool on_removed_called;
  /* A copy of callback_finished that evl_on_removed_slow takes from inside
   * itself under the same mtx, which lets the test prove the order:
   * on_removed always sees callback_finished already true. Without it the
   * test would only see both flags true in the end, with no proof of which
   * came first. */
  bool on_removed_saw_finished;
  void *on_removed_arg_seen;
} evl_slow_cb_ctx;

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

static void evl_slow_cb_ctx_destroy(evl_slow_cb_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

/* A bounded wait for *flag to become true, signalled through cond. It's
 * never unbounded, because an unbounded wait on the main thread can hang the
 * whole test binary under load instead of failing just one test. It works
 * for any bool field of evl_slow_cb_ctx, unlike evl_wait_for above, which
 * only works on evl_sync_ctx's int counters. */
static bool evl_wait_bool(pthread_mutex_t *mtx, pthread_cond_t *cond,
                          bool *flag, int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(mtx);
  while (!*flag) {
    int r = pthread_cond_timedwait(cond, mtx, &deadline);
    if (r == ETIMEDOUT) break;
  }
  bool ok = *flag;
  pthread_mutex_unlock(mtx);
  return ok;
}

/* An on_readable handler that blocks on the reactor thread until the test's
 * main thread releases it. That lets the test force the exact order of
 * events it checks, deterministically rather than racing incidental
 * timing: ccol_event_loop_remove() returning while this dispatch is really
 * in flight. The deadline below bounds the block even if nobody releases it,
 * so a bug that skips the release can never hang the test binary, it can
 * only fail the assertion that depends on a release in time. */
static void evl_on_readable_slow(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_slow_cb_ctx *c = (evl_slow_cb_ctx *)arg;
  char buf[64];
  if (sel->type == ccol_selectable_fd) {
    ssize_t n = read(sel->fd, buf, sizeof(buf));
    (void)n; /* this test only needs the drain, and not the content */
  }

  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 2;

  pthread_mutex_lock(&c->mtx);
  c->callback_started = true;
  pthread_cond_broadcast(&c->cond);
  while (!c->release_callback) {
    int r = pthread_cond_timedwait(&c->cond, &c->mtx, &deadline);
    if (r == ETIMEDOUT) break;
  }
  c->callback_finished = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_removed_slow(void *arg) {
  evl_slow_cb_ctx *c = (evl_slow_cb_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->on_removed_called = true;
  c->on_removed_saw_finished = c->callback_finished;
  c->on_removed_arg_seen = arg;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

TEST(ccol_event_loop, on_removed_fires_only_after_in_flight_callback_finishes) {
  /* Reproduces the documented hazard of
   * remove_from_different_thread_concurrent_with_dispatch above, on purpose
   * and deterministically, and proves that on_removed closes it. Without
   * on_removed a caller has no way to know that an in-flight dispatch is
   * still touching arg at the moment ccol_event_loop_remove() returns, as
   * the comment of that other test explains. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable_slow,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  char b = 1;
  REQUIRE_EQ(write(pfd[1], &b, 1), (ssize_t)1);
  REQUIRE_TRUE(evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.callback_started, 2000));

  /* At this point the one reactor thread is blocked inside
   * evl_on_readable_slow, holding this registration's entry->dispatch_lock,
   * and this thread removes the registration at the same time, while that
   * dispatch is really in flight. */
  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);

  /* ccol_event_loop_remove() must return without waiting for the callback;
   * the doc comment of that function says why. This gives on_removed a wide
   * window, to prove it does NOT fire while the callback is blocked. */
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
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, 2000));
  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_TRUE(ctx.on_removed_saw_finished);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, on_removed_fires_for_an_ordinary_removal) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  /* on_readable stays NULL. This test never writes to pfd[1], so nothing
   * dispatches it and only on_removed is under test here. */
  ccol_event_handlers_t handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_TRUE(
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, 2000));
  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, on_removed_null_is_harmless) {
  /* Most registrations in this codebase leave on_removed at NULL, and this
   * test pins that as a fully supported case that does nothing. It must not
   * crash, and a caller with no use for the notification shouldn't need a
   * special case for it. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, on_removed_fires_on_destroy_for_a_still_registered_reg) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);

  {
    ccol_event_loop_construct_scoped(loop, 8, 4, 1);
    /* on_readable stays NULL. This test never writes to pfd[1], so nothing
     * dispatches it before the block below destroys the loop with the
     * registration in place; only on_removed is under test here. */
    ccol_event_handlers_t handlers = {.on_readable = NULL,
                                      .on_writable = NULL,
                                      .on_error = NULL,
                                      .on_removed = evl_on_removed_slow};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
    /* Nothing removes this registration. The scoped loop's destructor, at
     * the closing brace of this block, tears it down while it's registered,
     * which covers the direct walk in _ccol_event_loop_teardown_raw rather
     * than the ordinary removal and reclaim path the tests above cover. */
  }

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_TRUE(ctx.on_removed_called);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

/* Arguments and result for the background thread below, which makes one
 * ccol_event_loop_reg_generation call. The test slows that call down on
 * purpose with _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms, and
 * the delay lands exactly where the call releases its resolve pin on reg. */
typedef struct evl_delayed_reg_generation_ctx {
  ccol_event_loop loop;
  ccol_event_reg reg;
  uint64_t gen;
} evl_delayed_reg_generation_ctx;

static void *evl_delayed_reg_generation_thread(void *arg) {
  evl_delayed_reg_generation_ctx *c = (evl_delayed_reg_generation_ctx *)arg;
  c->gen = ccol_event_loop_reg_generation(c->loop, c->reg);
  return NULL;
}

TEST(ccol_event_loop,
     on_removed_fires_via_bounded_retry_when_resolve_unpin_races_remove) {
  /* Reproduces one exact order of events, deterministically: a resolve pin
   * is still held at the exact moment ccol_event_loop_remove marks reg
   * removed and defers its reclaim. The pin comes from
   * ccol_event_loop_reg_generation here, standing in for
   * ccol_event_loop_modify, ccol_event_loop_pause and
   * ccol_event_loop_resume, all of which are documented as callable at the
   * same time as ccol_event_loop_remove against the same reg. The
   * maybe_needs_wake copy inside _ccol_event_reg_resolve_unpin is taken
   * before remove() runs, so it's stale by the time the delayed unpin below
   * drops pending_resolve_count to 0, and in this exact order of events the
   * ping of that unpin is a documented, deliberate no-op (see its comment).
   * This loop is otherwise idle, so the only thing left to reclaim reg and
   * fire on_removed is the bounded epoll_wait retry of
   * EVENT_LOOP_RECLAIM_RETRY_MS (see _ccol_event_loop_reclaim_pending_frees),
   * and the bounded wait below proves that this retry is what delivers it. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  /* on_readable stays NULL. This test never writes to pfd[1], so nothing
   * dispatches it at all and only on_removed is under test here. */
  ccol_event_handlers_t handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  /* Arms a one-shot delay for the next resolve-unpin in the whole process
   * (see the doc comment of that function), which the background thread
   * started below consumes. */
  _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(300);

  evl_delayed_reg_generation_ctx gen_ctx = {.loop = loop, .reg = reg, .gen = 0};
  pthread_t th;
  int create_rv =
      pthread_create(&th, NULL, evl_delayed_reg_generation_thread, &gen_ctx);
  if (create_rv != 0) _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(0);
  REQUIRE_EQ(create_rv, 0);

  /* The remove below must run while the background thread holds its pin
   * on reg and sleeps in the armed delay, because that's what makes
   * ccol_event_loop_remove see pending_resolve_count > 0 for reg and defer
   * the reclaim instead of freeing reg at once. The thread takes the delay
   * only after it has resolved reg and read its removed flag, so the wait
   * below ends inside that window however late the thread starts. The bound
   * of about 10 s only stops a hang, and the checks of gen_ctx.gen and
   * on_removed below fail anyway if the thread never got there. */
  for (int i = 0;
       i < 10000 &&
       !_ccol_event_loop_test_reg_resolve_unpin_delay_taken_for_tests();
       i++)
    usleep(1000);

  ccol_retval_t remove_rv = ccol_event_loop_remove(loop, reg);

  int join_rv = pthread_join(th, NULL);
  /* This call runs every time, so an unconsumed delay can't leak into a
   * later, unrelated test. It does nothing when the background thread's
   * resolve-unpin above already consumed the delay, which is the expected
   * case. It sits before every REQUIRE_* below so it runs whichever
   * assertion fails first, because Tau's REQUIRE_* macros return from the
   * test function at once on a failure and skip every later line, so
   * cleanup can't come after an assertion that can fail. */
  _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(0);

  bool on_removed_fired =
      evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.on_removed_called, 2000);
  pthread_mutex_lock(&ctx.mtx);
  void *on_removed_arg_seen = ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(join_rv, 0);
  /* 0 is the documented "resolve failed" sentinel of
   * ccol_event_loop_reg_generation, so a result other than 0 proves that the
   * background thread's resolve really succeeded and really held a pin on
   * reg. Without this check the race could go the other way and exercise
   * nothing. */
  REQUIRE_NE(gen_ctx.gen, (uint64_t)0);
  REQUIRE_EQ(remove_rv, ccol_success);
  REQUIRE_TRUE(on_removed_fired);
  REQUIRE_EQ(on_removed_arg_seen, (void *)&ctx);
}

TEST(ccol_event_loop, shutdown_with_pending_registrations) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);
  /* Idempotent, so calling again must not hang or join twice. */
  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);

  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

/* A read handler that takes one message from its selectable's circular
 * queue and counts it. */
static _Atomic long evl_shutdown_peer_got;
static void evl_shutdown_peer_on_read(ccol_event_loop l, ccol_event_reg r,
                                      ccol_selectable *s, void *a) {
  (void)l;
  (void)r;
  (void)a;
  c_message_t m;
  if (ccol_circq_try_recv_zc(s->cq, &m) == ccol_success) {
    free(m.data);
    atomic_fetch_add(&evl_shutdown_peer_got, 1);
  }
}

/*
 * Two loops watch one queue, and one of them is shut down but not destroyed,
 * with its registration left in place. Every message sent after that reaches
 * the loop that's still running without delay, because the shut-down loop takes
 * no wake its peer needs. Both a loop with one reactor thread and a loop with
 * a dispatch pool take a turn as the shut-down one.
 *
 * If the shut-down loop's registration stayed in the queue's waiter list,
 * the wakes the queue gives to it would never be acted on and about every
 * second message would sit in the queue until the next send.
 */
TEST(ccol_event_loop, a_shut_down_loop_takes_no_wake_from_its_peers) {
  int stuck[2] = {0, 0};
  int setup_failed = 0;
  for (int pass = 0; pass < 2; pass++) {
    atomic_store(&evl_shutdown_peer_got, 0);
    ccol_circular_queue *q = ccol_circular_queue_create(16, NULL);
    ccol_event_loop a = ccol_event_loop_create(8, pass == 0 ? 1 : 3, 1, NULL);
    ccol_event_loop b = ccol_event_loop_create(8, 1, 1, NULL);
    ccol_event_handlers_t h = {.on_readable = evl_shutdown_peer_on_read};
    ccol_event_reg ra =
        a ? ccol_event_loop_add(a,
                                ccol_selectable_from_circq(q, ccol_select_read),
                                h, NULL, NULL)
          : CCOL_EVENT_REG_INVALID;
    ccol_event_reg rb =
        b ? ccol_event_loop_add(b,
                                ccol_selectable_from_circq(q, ccol_select_read),
                                h, NULL, NULL)
          : CCOL_EVENT_REG_INVALID;
    if (!q || !a || !b || ra == CCOL_EVENT_REG_INVALID ||
        rb == CCOL_EVENT_REG_INVALID ||
        ccol_event_loop_shutdown(a) != ccol_success) {
      setup_failed++;
    } else {
      for (long i = 0; i < 20; i++) {
        c_message_t m = {.data = malloc(1), .size = 1};
        if (!m.data || ccol_circq_send_zc(q, &m) != ccol_success) {
          free(m.data);
          setup_failed++;
          break;
        }
        /* Only a bound against a hang, a delivery takes microseconds. */
        int waited_ms = 0;
        while (atomic_load(&evl_shutdown_peer_got) < i + 1 &&
               waited_ms < 2000) {
          struct timespec ts = {0, 1000000L};
          nanosleep(&ts, NULL);
          waited_ms++;
        }
        if (waited_ms >= 2000) stuck[pass]++;
      }
    }
    if (ra != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(a, ra);
    if (rb != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(b, rb);
    if (a) ccol_event_loop_destroy(a);
    if (b) ccol_event_loop_destroy(b);
    if (q) {
      c_message_t m;
      while (ccol_circq_try_recv_zc(q, &m) == ccol_success) free(m.data);
      ccol_circular_queue_destroy(q);
    }
  }
  REQUIRE_EQ(setup_failed, 0);
  REQUIRE_EQ(stuck[0], 0);
  REQUIRE_EQ(stuck[1], 0);
}

/*
 * A loop that's shut down refuses a new registration of any kind, with a
 * message that says why, because nothing of it dispatches any more.
 *
 * Without the check both adds succeed and the queue registration takes
 * wakes that nothing acts on.
 */
TEST(ccol_event_loop, add_to_a_shut_down_loop_is_refused) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_circular_queue *q = ccol_circular_queue_create(4, NULL);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  ccol_retval_t shut = ccol_event_loop_shutdown(loop);
  ccol_event_handlers_t h = {.on_readable = evl_on_readable};
  char *err_q = NULL, *err_fd = NULL;
  ccol_event_reg rq = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(q, ccol_select_read), h, NULL, &err_q);
  ccol_event_reg rf = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, NULL,
      &err_fd);
  bool named_q = err_q && strstr(err_q, "shut down") != NULL;
  bool named_fd = err_fd && strstr(err_fd, "shut down") != NULL;
  if (rq != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, rq);
  if (rf != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, rf);
  ccol_event_loop_destroy(loop);
  ccol_circular_queue_destroy(q);
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_EQ(shut, ccol_success);
  REQUIRE_EQ(rq, CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ(rf, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(named_q);
  REQUIRE_TRUE(named_fd);
}

TEST(ccol_event_loop,
     destroy_while_queue_registration_pending_queue_outlives_loop) {
  /* The library has to unlink a queue-backed ccol_event_reg's waiter_node
   * from the queue's waiter list before it frees that reg. Without that the
   * queue holds a dangling pointer and the next send or receive on it is a
   * use-after-free, which is even more dangerous here because this test
   * destroys the queue on purpose only after destroying the
   * ccol_event_loop. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  /* Destroys the loop while the registration is pending, so the library has
   * to unlink from cq's waiter list before it frees, not after. */
  ccol_event_loop_destroy(loop);

  /* The queue must be perfectly usable afterward. */
  int *payload = malloc(sizeof(int));
  *payload = 42;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &msg), ccol_success);
  c_message_t out;
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.data, 42);
  free(out.data);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, reg_count_tracks_add_remove) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);
  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

  close(pfd[0]);
  close(pfd[1]);
}

/* A regression test for an ordering hazard in ccol_event_loop_add that only
 * reasoning about the order shows, not a crash reproduction.
 * ccol_event_loop_add must NOT wire reg into the fd registry or the queue
 * registry before it mints reg's public handle, because wiring it makes reg
 * fully live and dispatchable through the raw ccol_event_reg_s* the
 * registry stores. _ccol_event_reg_slot_acquire mints the handle, and its
 * only failure mode is a failed allocation that grows loop->reg_slots. On
 * that failure ccol_event_loop_add reports CCOL_EVENT_REG_INVALID to the
 * caller, who then believes the library never created a registration and
 * has no reason to think a callback could already be running against the
 * arg pointer it passed (with num_reactor_threads > 1 that callback can also
 * be sitting queued on a ctpool worker). So the caller may free arg
 * immediately, a real use-after-free that needs nothing more than an ordinary
 * allocation failure while the target fd is already ready. That's why the
 * library acquires the slot BEFORE wiring reg into the registry, so a failed
 * slot acquire always finds reg fully unwired and no dispatch is possible.
 *
 * The primary assertion below is last_forced_..._reg_count == 0, which
 * proves the ORDER directly and deterministically without depending on a
 * real race between threads.
 * _ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count() reports
 * the value of ccol_event_loop_reg_count() at one exact moment, inside
 * _ccol_event_reg_slot_acquire when the forced failure fires and before any
 * rollback can run. The opposite order, "wire first, acquire the slot last",
 * would already have incremented reg_count for this registration at that
 * point, so the snapshot would read 1 and not 0. Checking
 * ccol_event_loop_reg_count() after the return can't tell the two orders
 * apart, because rolling back a failure after the wire step brings the
 * count back to 0 before ccol_event_loop_add returns.
 *
 * That's why a simple live-dispatch race, arming the hook next to a really
 * ready fd and checking whether a callback fires, isn't a reliable
 * regression guard. Build ccol_event_loop_add in the "wire, then
 * acquire" shape with this same hook, and the window between releasing the
 * stripe lock and the slot-acquire call right after it is too narrow in
 * practice: even a poller thread on another core, already running and
 * already blocked in epoll_wait, doesn't reliably win it, across 5
 * consecutive full runs with num_reactor_threads = 3 and pfd[0] already
 * holding data before the call. The reg_count snapshot below doesn't depend
 * on timing at all, since it reads the order straight off the work
 * ccol_event_loop_add has already done at the moment the hook fires instead
 * of racing two threads. The live-dispatch checks further down are a
 * secondary, best-effort corroboration that also exercises the module's real
 * end-to-end behavior, but the test's correctness rests on the reg_count
 * snapshot. */
TEST(ccol_event_loop,
     add_slot_acquire_failure_leaves_nothing_wired_or_dispatched) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int one = 1;
  REQUIRE_EQ(write(pfd[1], &one, sizeof(one)), (ssize_t)sizeof(one));

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* num_reactor_threads = 3 exercises the async dispatch-pool path, the
     * more dangerous one, because a ctpool worker can run the callback long
     * after the caller of ccol_event_loop_add has moved on. So this value
     * covers more than the inline num_reactor_threads == 1 path. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 3);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    _ccol_event_loop_test_force_next_reg_slot_acquire_failure();

    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_EQ(reg, CCOL_EVENT_REG_INVALID);
    REQUIRE_NE((void *)err, NULL);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

    /* The deterministic proof; see this test's leading comment. */
    REQUIRE_EQ(
        _ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(),
        (size_t)0);

    /* A best-effort corroboration. pfd[0] is already readable and stays
     * readable, so if the failed registration left anything wired, the poller
     * has every chance to dispatch it during this bounded wait, and with
     * num_reactor_threads > 1 so does a dispatch_pool worker. The test
     * doesn't rely on this check to catch a regression (the leading comment
     * says why), because a real bug here shows up as a crash or a
     * use-after-free under valgrind or AddressSanitizer long before this
     * assertion's timing margin becomes the limit. */
    REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));
    pthread_mutex_lock(&ctx.mtx);
    REQUIRE_EQ(ctx.readable_count, 0);
    pthread_mutex_unlock(&ctx.mtx);

    /* The hook is one-shot, so an ordinary call right after it succeeds,
     * which confirms the hook only affected the single call above. */
    err = NULL;
    ccol_event_reg reg2 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg2, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    ccol_event_loop_remove(loop, reg2);

    /* Leaving the block shuts loop down and joins all of its threads, so
     * ctx is quiet from here on. */
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, high_add_remove_churn_stress) {
  ccol_event_loop_construct_scoped(loop, 32, 1, 1);
  const int n = 200;
  int pfds[200][2];
  ccol_event_reg regs[200];

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  /* Every pipe that was opened is closed before an assertion can return, so
   * a process with fewer than 400 descriptors fails only this test and the
   * tests after it keep their descriptors. */
  int opened = 0;
  bool added = true;
  for (; opened < n; opened++) {
    if (pipe(pfds[opened]) != 0) break;
    char *err = NULL;
    regs[opened] = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfds[opened][0], ccol_select_read),
        handlers, NULL, &err);
    if (regs[opened] == CCOL_EVENT_REG_INVALID) {
      added = false;
      close(pfds[opened][0]);
      close(pfds[opened][1]);
      break;
    }
  }
  if (opened < n || !added) {
    for (int i = 0; i < opened; i++) {
      ccol_event_loop_remove(loop, regs[i]);
      close(pfds[i][0]);
      close(pfds[i][1]);
    }
  }
  REQUIRE_EQ(opened, n);
  REQUIRE_TRUE(added);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)n);

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(ccol_event_loop_remove(loop, regs[i]), ccol_success);
    close(pfds[i][0]);
    close(pfds[i][1]);
  }
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);
}

TEST(ccol_event_loop, multiple_independent_instances) {
  /* Two separate ccol_event_loop instances share no global state, their
   * lock-stripe arrays included. loop_a uses 1 stripe, which is the same as
   * a single lock, and loop_b uses 8, the counts differing on purpose. This
   * confirms num_lock_stripes is a per-instance setting and that one
   * instance doesn't disturb the other. */
  int pfd_a[2], pfd_b[2];
  REQUIRE_EQ(pipe(pfd_a), 0);
  REQUIRE_EQ(pipe(pfd_b), 0);

  evl_sync_ctx ctx_a, ctx_b;
  evl_sync_ctx_init(&ctx_a);
  evl_sync_ctx_init(&ctx_b);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. Both fds here stay ready, since the reactor is
     * level-triggered and evl_on_readable never drains an fd selectable, so
     * the test has to join both loops' reactor threads before it destroys
     * either ctx. */
    ccol_event_loop_construct_scoped(loop_a, 8, 1, 1);
    ccol_event_loop_construct_scoped(loop_b, 8, 8, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg_a = ccol_event_loop_add(
        loop_a, ccol_selectable_from_fd(pfd_a[0], ccol_select_read), handlers,
        &ctx_a, &err);
    REQUIRE_NE(reg_a, CCOL_EVENT_REG_INVALID);
    ccol_event_reg reg_b = ccol_event_loop_add(
        loop_b, ccol_selectable_from_fd(pfd_b[0], ccol_select_read), handlers,
        &ctx_b, &err);
    REQUIRE_NE(reg_b, CCOL_EVENT_REG_INVALID);

    int val_a = 1, val_b = 2;
    REQUIRE_EQ((ssize_t)sizeof(val_a), write(pfd_a[1], &val_a, sizeof(val_a)));
    REQUIRE_TRUE(evl_wait_for(&ctx_a, &ctx_a.readable_count, 1, 2000));
    /* loop_b's registration must not fire for loop_a's fd, and that's the
     * property under test. The read takes ctx_b's lock because its callback
     * writes the count under that lock once pfd_b is written below, and only
     * the lock orders this read before that write as far as the memory model
     * goes, since the kernel event in between is no synchronisation the
     * language knows about. */
    pthread_mutex_lock(&ctx_b.mtx);
    int b_count_before = ctx_b.readable_count;
    pthread_mutex_unlock(&ctx_b.mtx);
    REQUIRE_EQ(b_count_before, 0);

    REQUIRE_EQ((ssize_t)sizeof(val_b), write(pfd_b[1], &val_b, sizeof(val_b)));
    REQUIRE_TRUE(evl_wait_for(&ctx_b, &ctx_b.readable_count, 1, 2000));

    ccol_event_loop_remove(loop_a, reg_a);
    ccol_event_loop_remove(loop_b, reg_b);

    /* Leaving the block shuts both loops down and joins their reactor
     * threads, so ctx_a and ctx_b are quiet from here on. */
  }

  evl_sync_ctx_destroy(&ctx_a);
  evl_sync_ctx_destroy(&ctx_b);
  close(pfd_a[0]);
  close(pfd_a[1]);
  close(pfd_b[0]);
  close(pfd_b[1]);
}

// lock-striping tests
TEST(ccol_event_loop, num_lock_stripes_zero_returns_null) {
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 0, 1, &err);
  REQUIRE_EQ(loop, CCOL_EVENT_LOOP_INVALID);
}

/* _ccol_event_loop_thread_fn narrows max_events_per_wait to a plain `int`
 * for epoll_wait(2)'s maxevents parameter, and also sizes the poller
 * thread's events buffer from it as max_events_per_wait *
 * sizeof(ccol_poll_event). The value needs an upper bound, because without
 * one two things can go wrong: a value whose low 32 bits read as a
 * non-positive signed int makes epoll_wait fail with EINVAL on the very
 * first call, and a value that's merely huge makes allocating the events
 * buffer fail instead. The poller thread's error handling treats both as an
 * ordinary, silent, permanent thread exit that nothing tells apart from any
 * other unexpected epoll_wait or allocation failure. The constructor still
 * returns a ccol_event_loop handle that looks valid and live and
 * ccol_event_loop_add keeps succeeding, but no callback ever fires again and
 * the caller sees nothing. So creation refuses any max_events_per_wait above
 * INT_MAX, and also any value that overflows size_t when multiplied by
 * sizeof(ccol_poll_event). That second guard is the one that binds on an
 * ILP32 platform, where INT_MAX alone doesn't stop the multiplication from
 * overflowing a 32-bit size_t. */
TEST(ccol_event_loop, create_rejects_max_events_per_wait_exceeding_int_max) {
  char *err = NULL;
  ccol_event_loop loop =
      ccol_event_loop_create((size_t)INT_MAX + 1, 1, 1, &err);
  REQUIRE_EQ(loop, CCOL_EVENT_LOOP_INVALID);
  REQUIRE_NE((void *)err, NULL);
}

TEST(
    ccol_event_loop,
    create_rejects_max_events_per_wait_that_would_overflow_events_buffer_size) {
  char *err = NULL;
  size_t smallest_overflowing = SIZE_MAX / sizeof(ccol_poll_event) + 1;
  ccol_event_loop loop =
      ccol_event_loop_create(smallest_overflowing, 1, 1, &err);
  REQUIRE_EQ(loop, CCOL_EVENT_LOOP_INVALID);
  REQUIRE_NE((void *)err, NULL);
}

/* A max_events_per_wait well inside both guards should be accepted and the
 * resulting loop should really dispatch, which confirms the validation
 * above isn't too strict. */
TEST(ccol_event_loop,
     create_succeeds_with_a_large_but_valid_max_events_per_wait) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  ccol_event_loop loop = ccol_event_loop_create(4096, 1, 1, NULL);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int val = 1;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_both_directions_combine_and_recombine_multi_stripe) {
  /* The same scenario as fd_both_directions_combine_and_recombine, but with
   * num_lock_stripes > 1. The read and the write direction on the SAME fd
   * hash to the SAME stripe, since _stripe_index_for_fd is a pure function of
   * the fd, so they stay serialized inside that stripe. Adding with
   * EPOLL_CTL_ADD and then combining with MOD, and the MOD back down on a
   * partial removal, must behave exactly as in the single-stripe case. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* A nested block, with the same pattern and comment as
     * fd_on_readable_fires. */
    ccol_event_loop_construct_scoped(loop, 8, 8, 1);

    ccol_event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    ccol_event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx,
        &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx,
        &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)2);

    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    REQUIRE_EQ(ccol_event_loop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

    ccol_event_loop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_event_loop,
     fd_modify_after_remove_returns_invalid_args_multi_stripe) {
  /* The same scenario as fd_modify_after_remove_returns_invalid_args, but
   * with num_lock_stripes > 1. ccol_event_loop_modify must key its stripe
   * lookup off reg->stripe_idx, a read that needs no lock and is always safe
   * for any reg* the caller legitimately holds, and it must return
   * ccol_invalid_args cleanly when reg is already removed, without crashing
   * or misbehaving.
   *
   * On purpose this test does NOT try to force reg's memory to be really
   * freed before it calls modify, for example by letting several reactor
   * drain cycles pass first. reg stays valid, stale memory only across the
   * *short* window before the next drain claims it (see the doc comment of
   * _ccol_event_loop_defer_reg_free). Extra drain cycles here would close
   * that window and really free reg, and the call would then not be on
   * a stale reg* that the library handles cleanly but a real use-after-free
   * outside the contract, which valgrind rightly reports against the test's
   * premise and not against ccol_event_loop_modify or
   * ccol_event_loop_remove. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 8, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

/* ========================================================================== */
/*                    ccol_event_loop_pause / ccol_event_loop_resume */
/* ========================================================================== */

TEST(ccol_event_loop, pause_stops_delivery_then_resume_restores_it) {
  /* This test pauses BEFORE anything makes the fd ready, on purpose. The
   * other order waits for one delivery and then races a pause call against
   * it. A plain pipe read end stays ready once it's ready until something
   * drains it, and with num_reactor_threads > 1 EPOLLONESHOT is in play, so
   * the re-arm the dispatch job runs after the callback happens on a
   * completely different thread from the one calling
   * ccol_event_loop_pause. Nothing synchronizes "the test saw the callback
   * run once" with "the pause takes effect before the next re-arm", so an
   * assertion in that order is a real race that fails now and then. Pausing
   * first avoids the race completely, since no delivery can happen before
   * the pause takes effect when none has happened yet. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);

  /* Makes the fd really ready while it's paused, and no on_readable callback
   * may fire however long the test waits. The bounded wait below stands in
   * for "never", which is how this file asserts a negative. */
  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, pause_write_direction) {
  /* The same "pause before anything can fire" shape as
   * pause_stops_delivery_then_resume_restores_it above, for the same reason,
   * and it matters even more here: a pipe's write end never stops being
   * writable by itself, so waiting for one delivery and then pausing before
   * a second one races a condition that re-fires without bound rather than
   * a one-shot event.
   *
   * A pipe's write end is writable from the moment it exists, including the
   * short window between ccol_event_loop_add returning and the very next
   * line of this test, which calls ccol_event_loop_pause. Unlike the read
   * side above, which really isn't ready until the test's later write()
   * call, a dispatch that races ahead of the pause here is a real
   * possibility, beyond a theoretical one. So the assertions below are
   * relative to a baseline: the count must not move past whatever it was at
   * the moment pause() returned, and the test passes deterministically on
   * either path without assuming the count is exactly 0 at that point. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  /* A pipe's write end is writable as soon as it has room, and it has room
   * right away, so this test needs no priming step, unlike the
   * ccol_circq_writable test above, which fills the queue first so that write
   * readiness means something there. */
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[1], ccol_select_write), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  int baseline = ctx.writable_count;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, 300));

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, pause_is_idempotent) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);

  ccol_event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, resume_never_paused_is_idempotent_and_harmless) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  /* Nothing paused this registration, so the call succeeds, does
   * nothing and leaves delivery alone. */
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, pause_and_resume_reject_queue_selectable) {
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  /* The same fd-only restriction ccol_event_loop_modify has. The bridge
   * eventfd of a queue registration or a ccol_channel registration has no use
   * case worth it, since such a case would stop caring for a while and keep
   * the registration anyway. */
  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_invalid_args);
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_invalid_args);

  ccol_event_loop_remove(loop, reg);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, pause_and_resume_null_args_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(CCOL_EVENT_LOOP_INVALID, reg),
             ccol_invalid_args);
  REQUIRE_EQ(ccol_event_loop_pause(loop, CCOL_EVENT_REG_INVALID),
             ccol_invalid_args);
  REQUIRE_EQ(ccol_event_loop_resume(CCOL_EVENT_LOOP_INVALID, reg),
             ccol_invalid_args);
  REQUIRE_EQ(ccol_event_loop_resume(loop, CCOL_EVENT_REG_INVALID),
             ccol_invalid_args);

  ccol_event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, resume_after_remove_returns_invalid_args) {
  /* Mirrors the exact timing of
   * fd_modify_after_remove_returns_invalid_args_multi_stripe: a remove
   * followed directly by the operation under test, with nothing forcing a
   * drain. reg stays valid, stale memory only across the short window before
   * the next reclamation drain claims it, so ccol_event_loop_resume needs to
   * key its stripe lookup off reg's stripe_idx, report ccol_invalid_args
   * cleanly and never crash. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop,
     sequential_double_remove_returns_invalid_args_not_success) {
  /* This pins the documented contract exactly: a SECOND
   * ccol_event_loop_remove() call on the same reg, made strictly after an
   * earlier one has returned, isn't a success that does nothing. The earlier
   * call already released reg's slot before it returned
   * (_ccol_event_reg_slot_release does that, and the successful-removal path
   * always calls it before the function returns), so reg_h doesn't resolve
   * at all by the time this second, purely sequential call runs, and the
   * call reports ccol_invalid_args. ccol_event_loop_modify(),
   * ccol_event_loop_pause(), ccol_event_loop_resume() and
   * ccol_event_loop_reg_generation() give the same result for a reg that's
   * already removed. Only a call that really RACES the first one, resolving
   * reg before that first call's slot release, can see ccol_success from a
   * second removal; reg_handle_survives_concurrent_remove_vs_accessor_race
   * below covers that separate, narrower window. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  ccol_event_handlers_t handlers = {0};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, pause_resume_preserve_reg_count_and_generation) {
  /* To any observer outside the library a pause and a resume must never
   * look like a remove followed by an add, since the registration itself
   * never goes away, so both identities a caller can see stay exactly the
   * same across a pause and resume cycle. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  size_t count_before = ccol_event_loop_reg_count(loop);
  uint64_t gen_before = ccol_event_loop_reg_generation(loop, reg);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), count_before);
  REQUIRE_EQ(ccol_event_loop_reg_generation(loop, reg), gen_before);

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), count_before);
  REQUIRE_EQ(ccol_event_loop_reg_generation(loop, reg), gen_before);

  ccol_event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

/* The kernel always reports EPOLLERR and EPOLLHUP whatever the registered
 * interest mask is, even for a mask of 0, so the only way to fully silence
 * a registration that ccol_event_loop_pause paused is to take its fd out of
 * the epoll set altogether. That matters when the fd enters an error or
 * hangup condition, or is already in one, because level-triggered
 * epoll_wait would otherwise report that condition on every single call for
 * ever. The re-check of reg->paused correctly skips the callback itself, but
 * nothing stops the reactor from seeing and collecting the condition again,
 * which is a silent CPU-spin busy loop with no bound. That flatly
 * contradicts the documented contract of ccol_event_loop_pause, "no
 * callback fires, exactly as if it had been removed". A registration that's
 * really removed produces zero further wakeups because of EPOLL_CTL_DEL,
 * while a paused one whose mask is only narrowed can't, because the interest
 * mask can't turn ERR and HUP off. So _ccol_event_loop_rearm_entry_locked
 * takes the fd out of the epoll interest set with EPOLL_CTL_DEL whenever
 * every live direction on it is paused, which the event_entry.epoll_added
 * field tracks, and once some direction wants real interest again an fd
 * removed this way has to come back with EPOLL_CTL_ADD, not EPOLL_CTL_MOD.
 *
 * We measure with _ccol_event_loop_poller_iterations_for_tests, a direct
 * count of completed epoll_wait calls, rather than wall-clock or CPU time.
 * With an unguarded pause the counter races into the thousands inside the
 * sleep window below. With this handling in place the poller stays really
 * blocked in epoll_wait for the whole window, since nothing else is
 * registered with this loop, so the count hardly moves. Remove the DEL and
 * ADD logic that epoll_added drives and the delta grows orders of magnitude
 * past the bound below. */
/* A socket's write direction hears nothing about the end of the receive
 * direction, since epoll reports no event for OUT|ERR|HUP when only the peer
 * shuts down its sending side. These two tests hold every backend to that.
 * On kqueue both catch a write-only interest that arms the socket's read
 * filter: the select below then ends the process with an assertion, and
 * the poller of the loop below runs without a pause. */
TEST(fd_half_close, a_write_select_on_a_half_closed_socket_reports_writable) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  REQUIRE_EQ(shutdown(sv[1], SHUT_WR), 0);
  usleep(20000);
  ccol_selectable sel = ccol_selectable_from_fd(sv[0], ccol_select_write);
  size_t idx = 99;
  ccol_retval_t rv = ccol_select_timed(&idx, 1, &sel, 2000000);
  close(sv[0]);
  close(sv[1]);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
}

static void fd_half_close_fill(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  static char chunk[16384];
  while (send(sel->fd, chunk, sizeof(chunk), MSG_DONTWAIT) > 0) {
  }
  atomic_fetch_add((_Atomic int *)arg, 1);
}

TEST(fd_half_close, a_writer_on_a_full_half_closed_socket_does_not_spin) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  static char chunk[16384];
  while (send(sv[0], chunk, sizeof(chunk), MSG_DONTWAIT) > 0) {
  }
  REQUIRE_EQ(shutdown(sv[1], SHUT_WR), 0);
  _Atomic int fills = 0;
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);
  ccol_event_handlers_t handlers = {.on_writable = fd_half_close_fill};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(sv[0], ccol_select_write), handlers,
      (void *)&fills, &err);
  usleep(50000);
  uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = _ccol_event_loop_poller_iterations_for_tests(loop);
  if (reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(sv[0]);
  close(sv[1]);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_LT(after - before, (uint64_t)20);
}

TEST(ccol_event_loop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_single_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                    .on_writable = NULL,
                                    .on_error = evl_on_error};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);

  /* Puts the fd into a persistent, level-triggered hangup condition while
   * it's paused, since closing the write end makes the read end report
   * EPOLLHUP. */
  close(pfd[1]);

  /* Gives an unguarded busy loop a real chance to run away before the first
   * sample, and then the test measures the delta across another wide
   * window. */
  usleep(50000);
  uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = _ccol_event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* The multi-threaded reactor counterpart of the test above. With a
 * dispatch_pool present the fully-paused mask holds EPOLLONESHOT alone and
 * no real interest bits, so the spin takes the shape of a continuous
 * epoll_wait -> ctpool submit -> worker dequeue -> skip -> re-arm cycle
 * instead of a tight single-thread loop, and it's just as unbounded. The same
 * poller_iterations_for_tests counter catches it the same way, because it
 * counts completed epoll_wait calls on the one and only poller thread
 * whatever num_reactor_threads is. */
TEST(ccol_event_loop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_multi_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 3);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                    .on_writable = NULL,
                                    .on_error = evl_on_error};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);

  close(pfd[1]);

  usleep(50000);
  uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = _ccol_event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* The companion of the two spin-regression tests above, covering the other
 * half of the same mechanism. EPOLL_CTL_DEL takes a fully-paused fd out of
 * the epoll interest set once every direction on it is paused, so a resume
 * must re-add that fd with EPOLL_CTL_ADD and not EPOLL_CTL_MOD, because a
 * MOD on an fd that isn't registered at the moment fails with ENOENT (see
 * the field comment of event_entry.epoll_added). The test confirms that the
 * library sees and dispatches the hangup condition, which is still there,
 * once the resume happens, which proves the re-add path really works.
 * Without it the fd silently stays unregistered. */
TEST(ccol_event_loop, resume_after_hangup_while_paused_delivers_dispatch) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                    .on_writable = NULL,
                                    .on_error = evl_on_error};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  REQUIRE_EQ(ccol_event_loop_pause(loop, reg), ccol_success);
  close(pfd[1]);
  /* Lets the reactor settle with the fd fully de-registered before the
   * resume below. */
  usleep(100000);

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);

  /* An empty pipe whose write end is closed reports EPOLLHUP alone; a
   * standalone epoll_ctl and epoll_wait probe against this kernel reports
   * exactly that, with no EPOLLIN or EPOLLRDHUP bit here. The socket case
   * where the peer writes and then closes is different, and the has_reader
   * comment of _ccol_event_loop_handle_event documents it. So this one is
   * dispatched as an error event rather than a readable event. */
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.error_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

typedef struct {
  evl_sync_ctx *ctx;
  ccol_event_loop loop;
  /* This field is _Atomic for one reason only. reg is known only once
   * ccol_event_loop_add below returns, so the code publishes it to this
   * struct AFTER the registration is live, when in principle another thread
   * can dispatch it. In this test's exact sequence the callback can never see
   * reg unset, because the fd really has nothing to read until the test's
   * later write() call, which comes strictly after reg is published. But a
   * plain struct field read on one thread is a real data race under the C
   * memory model anyway, since that read has no happens-before edge to the
   * write on the other thread, and the timing guarantee from the data
   * doesn't change that. ThreadSanitizer reports this race, which reading
   * the code alone doesn't show. An atomic store and load add the missing
   * synchronization and change no real behavior. */
  _Atomic(ccol_event_reg) reg;
} evl_pause_from_callback_args;

/* This callback pauses its own registration from inside itself, exactly
 * like the _conn_start_diverted -> ccol_event_loop_pause call site of
 * chttpserver.c, which runs synchronously from inside an on_readable
 * dispatch that's in flight. */
static void evl_on_readable_pause_self(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg) {
  evl_pause_from_callback_args *a = (evl_pause_from_callback_args *)arg;
  /* assert() instead of REQUIRE_EQ; the comment of
   * evl_on_readable_self_remove above explains why calling a Tau assertion
   * macro from such a callback is unsafe. That callback runs on the reactor
   * thread or the dispatch thread of ccol_event_loop, at the same time as
   * the main test thread's REQUIRE_* calls. */
  assert(reg == atomic_load(&a->reg));
  assert(ccol_event_loop_pause(a->loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, a->ctx);
}

TEST(ccol_event_loop,
     pause_from_within_callback_then_resume_from_another_thread) {
  /* The exact pattern chttpserver.c depends on: on_readable pauses its own
   * registration synchronously from inside the dispatch callback, as
   * _conn_start_diverted does, and a completely separate thread resumes it
   * later. That thread plays the part of chttpserver's worker_pool, which is
   * separate from the internal reactor threads and dispatch threads of
   * ccol_event_loop. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  evl_pause_from_callback_args cb_args = {.ctx = &ctx, .loop = loop};
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable_pause_self,
                                    .on_writable = NULL,
                                    .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers,
      &cb_args, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  atomic_store(&cb_args.reg, reg);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  /* The callback above paused this registration. The test drains the fd and
   * writes to it again, which proves nothing more is delivered while the
   * registration is paused, exactly as the single-thread pause test does. */
  int drain;
  REQUIRE_EQ(read(pfd[0], &drain, sizeof(drain)), (ssize_t)sizeof(drain));
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 2, 300));

  /* This resume runs on the TEST's own thread, not on an internal thread of
   * ccol_event_loop (the poller or a dispatch_pool worker). */
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 2, 2000));

  ccol_event_loop_remove(loop, reg);
  /* Joins the poller thread and any dispatch workers, so no callback can be
     running with &ctx when the code below destroys the sync context;
     ccol_event_loop_remove alone doesn't wait for a dispatch that was
     already in flight when it was called. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

/* A registration's callback that runs before ccol_event_loop_add has
 * returned gets the handle of its own registration anyway and can remove
 * that registration with it. The add hook of this file makes the first
 * dispatch run while ccol_event_loop_add is still inside the call, so the
 * handle the adding thread stores afterwards isn't there yet. */
typedef struct {
  _Atomic ccol_event_reg returned_reg; /* stored after add returns */
  _Atomic ccol_event_reg reg_seen;     /* reg parameter of the dispatch */
  _Atomic ccol_event_reg stored_seen;  /* returned_reg read in the dispatch */
  _Atomic int dispatch_count;
  _Atomic int remove_rv;
  _Atomic bool first_dispatch_done;
  _Atomic int removed_count;
} evl_early_reg_ctx;

static evl_early_reg_ctx *_Atomic g_evl_early_reg_ctx = NULL;

static void evl_early_reg_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                      ccol_selectable *sel, void *arg) {
  (void)sel;
  evl_early_reg_ctx *c = (evl_early_reg_ctx *)arg;
  if (atomic_fetch_add(&c->dispatch_count, 1) != 0) return;
  atomic_store(&c->stored_seen, atomic_load(&c->returned_reg));
  atomic_store(&c->reg_seen, reg);
  atomic_store(&c->remove_rv, (int)ccol_event_loop_remove(loop, reg));
  atomic_store(&c->first_dispatch_done, true);
}

static void evl_early_reg_on_removed(void *arg) {
  evl_early_reg_ctx *c = (evl_early_reg_ctx *)arg;
  atomic_fetch_add(&c->removed_count, 1);
}

/* Runs inside ccol_event_loop_add and waits, with a ten second bound, until
 * the new registration's first dispatch has finished. */
static void evl_early_reg_add_hook(void) {
  evl_early_reg_ctx *c = atomic_load(&g_evl_early_reg_ctx);
  if (!c) return;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int i = 0; i < 10000 && !atomic_load(&c->first_dispatch_done); i++)
    nanosleep(&ts, NULL);
}

TEST(ccol_event_loop, callback_before_add_returns_removes_itself_by_reg) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  /* The fd is readable before the registration exists. */
  char byte = 'x';
  ssize_t wrote = write(pfd[1], &byte, 1);

  evl_early_reg_ctx ctx;
  atomic_init(&ctx.returned_reg, CCOL_EVENT_REG_INVALID);
  atomic_init(&ctx.reg_seen, CCOL_EVENT_REG_INVALID);
  atomic_init(&ctx.stored_seen, CCOL_EVENT_REG_INVALID);
  atomic_init(&ctx.dispatch_count, 0);
  atomic_init(&ctx.remove_rv, -1);
  atomic_init(&ctx.first_dispatch_done, false);
  atomic_init(&ctx.removed_count, 0);

  ccol_event_loop loop = ccol_event_loop_create(8, 4, 4, NULL);
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  if (loop != CCOL_EVENT_LOOP_INVALID && wrote == 1) {
    atomic_store(&g_evl_early_reg_ctx, &ctx);
    _ccol_event_loop_test_set_add_before_return_hook(evl_early_reg_add_hook);
    reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read),
        (ccol_event_handlers_t){.on_readable = evl_early_reg_on_readable,
                                .on_removed = evl_early_reg_on_removed},
        &ctx, NULL);
    _ccol_event_loop_test_set_add_before_return_hook(NULL);
    atomic_store(&g_evl_early_reg_ctx, NULL);
    atomic_store(&ctx.returned_reg, reg);
    /* on_removed runs asynchronously after the removal, so we wait for it,
     * with a ten second bound. */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0; i < 10000 && atomic_load(&ctx.removed_count) == 0; i++)
      nanosleep(&ts, NULL);
  }
  int removed_before_destroy = atomic_load(&ctx.removed_count);
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(wrote, (ssize_t)1);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(atomic_load(&ctx.first_dispatch_done));
  /* The dispatch ran before ccol_event_loop_add returned, so the handle the
   * adding thread stores afterwards was still unset at that moment. */
  REQUIRE_EQ(atomic_load(&ctx.stored_seen), CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ(atomic_load(&ctx.reg_seen), reg);
  REQUIRE_EQ(atomic_load(&ctx.remove_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&ctx.dispatch_count), 1);
  REQUIRE_EQ(removed_before_destroy, 1);
  REQUIRE_EQ(atomic_load(&ctx.removed_count), 1);
}

typedef struct {
  ccol_event_loop loop;
  int thread_id;
  int iterations;
  /* The test opens this once before this worker's loop and closes it once
   * after it, rather than once per iteration. Closing it per iteration would
   * race two calls: while the loop and the other 7 workers running at the
   * same time are all alive, a legitimate stale dispatch of the kind a
   * thundering herd produces can call read() on this fd while this worker
   * calls close(). The comment of
   * multi_thread_fd_reuse_generation_stays_consistent documents the same
   * close()-against-read() hazard, which ThreadSanitizer reports. */
  int pfd[2];
  /* The test logs each iteration's ctx here instead of destroying it in
   * place, for the same reason as the identical field of
   * evl_reuse_driver_args. ccol_event_loop_remove can return while a
   * callback for the removed reg is still in flight, so destroying ctx.mtx
   * right after remove() returns can race that callback, and so can reusing
   * ctx's stack slot on the next iteration. It's a real race even with a
   * single reactor thread, nothing about it needs dispatch on many threads,
   * and ThreadSanitizer reports it here. The TEST function frees these only
   * after it has joined the whole loop and every worker thread. */
  evl_sync_ctx **ctx_log;
  int ctx_log_count;
  /* Only the queue worker uses this field. It holds the ccol_circular_queue
   * that pairs with ctx_log[j] at the same index j, which the test defers
   * and frees next to that ctx for the same reason (see the comment of
   * ctx_log above, and how evl_stripe_stress_queue_worker uses this field).
   * The fd worker doesn't use it and leaves it NULL. */
  ccol_circular_queue **cq_log;
} evl_stripe_stress_args;

static void *evl_stripe_stress_fd_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        a->loop, ccol_selectable_from_fd(a->pfd[0], ccol_select_read), handlers,
        ctx, &err);
    if (reg) {
      a->ctx_log[a->ctx_log_count++] = ctx;
      int val = a->thread_id;
      test_write_retry_eintr(a->pfd[1], &val, sizeof(val));
      evl_wait_for(ctx, &ctx->readable_count, 1, 2000);
      ccol_event_loop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
    }
  }
  return NULL;
}

TEST(ccol_event_loop, multi_threaded_multi_fd_stress_with_stripes) {
  /* This test uses many threads and many DISTINCT fds, unlike
   * high_add_remove_churn_stress, which is single-threaded with a stripe
   * count of 1. Here num_lock_stripes is well above 1, so the test exercises
   * really concurrent ccol_event_loop_add, ccol_event_loop_remove and
   * dispatch calls across different stripes running in parallel, not just
   * churn on a single lock. */
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* This fd is non-blocking (see the comment of evl_set_nonblocking),
     * because a legitimate duplicate or stale dispatch can read it after this
     * worker has moved on to a later iteration, and such a read must not
     * block for ever. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  size_t residual_regs = 0;
  {
    /* A nested block, the same pattern as in the other multi-thread tests.
     * The loop's destructor joins every reactor thread at the closing brace
     * of this block, and that join is what makes it safe to free every
     * logged ctx below. */
    ccol_event_loop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      /* This loop counts the failures instead of asserting inside itself. A
       * REQUIRE_* here would return from the test while the threads earlier
       * iterations created keep running and keep using args[] and `loop`,
       * whose scoped destructor runs on that very return. The test joins
       * only the threads that really started and checks the count once all
       * of them are back. */
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_fd_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    /* We capture these values here without asserting on them yet, because
       a failing assertion returns from the test at once while everything
       below this block still has pipes to close and per-thread logs to free.
       The registration count needs to be read while the loop is alive, which
       is why that read stays inside the block. */
    all_started = (started == n_threads);
    residual_regs = ccol_event_loop_reg_count(loop);
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

static void *evl_stripe_stress_queue_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        a->loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers,
        ctx, &err);
    if (reg) {
      /* The worker logs these instead of destroying them in place, because
       * ccol_event_loop_remove can return while a callback for the removed
       * reg is still in flight (see the field comments of ctx_log and cq_log
       * in evl_stripe_stress_args). Destroying ctx's mutex and condition
       * variable right after remove() returns can race that callback, and so
       * can freeing cq at that point. evl_stripe_stress_fd_worker uses the
       * same deferred-cleanup pattern. The TEST function frees these only
       * after it has joined the whole loop and every worker thread. */
      a->ctx_log[a->ctx_log_count] = ctx;
      a->cq_log[a->ctx_log_count] = cq;
      a->ctx_log_count++;
      int *payload = malloc(sizeof(int));
      *payload = a->thread_id;
      c_message_t msg = {.data = payload, .size = sizeof(int)};
      ccol_circq_send_zc(cq, &msg);
      evl_wait_for(ctx, &ctx->readable_count, 1, 2000);
      ccol_event_loop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
      ccol_circular_queue_destroy(cq);
    }
  }
  return NULL;
}

TEST(ccol_event_loop, multi_threaded_multi_queue_stress_with_stripes) {
  /* The queue-selectable version of the fd stress test above. It uses many
   * distinct queues, and so many distinct bridge_efds, which the loop
   * spreads over the stripes round robin through loop->next_queue_stripe,
   * while threads register and remove them at the same time. */
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].cq_log = malloc(sizeof(ccol_circular_queue *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  size_t residual_regs = 0;
  {
    /* A nested block, the same pattern as in the other multi-thread tests.
     * The loop's destructor joins every reactor thread at the closing brace
     * of this block, and that join is what makes it safe to free every
     * logged ctx and cq below. */
    ccol_event_loop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      /* This loop counts the failures instead of asserting inside itself. A
       * REQUIRE_* here would return from the test while the threads earlier
       * iterations created keep running and keep using args[] and `loop`,
       * whose scoped destructor runs on that very return. The test joins
       * only the threads that really started and checks the count once all
       * of them are back. */
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_queue_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    /* We capture these values here without asserting on them yet, because
       a failing assertion returns from the test at once while everything
       below this block still has pipes to close and per-thread logs to free.
       The registration count has to be read while the loop is alive, which
       is why that read stays inside the block. */
    all_started = (started == n_threads);
    residual_regs = ccol_event_loop_reg_count(loop);
  }

  for (int i = 0; i < n_threads; i++) {
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      evl_sync_ctx_destroy(args[i].ctx_log[j]);
      free(args[i].ctx_log[j]);
      ccol_circular_queue_destroy(args[i].cq_log[j]);
    }
    free(args[i].ctx_log);
    free(args[i].cq_log);
  }

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(residual_regs, (size_t)0);
}

TEST(ccol_event_loop, destroy_frees_registrations_across_multiple_stripes) {
  /* Registers enough distinct fds and queues, with num_lock_stripes > 1, to
   * spread the live registrations over several stripes, and then destroys
   * the loop WITHOUT removing anything first. That exercises the per-stripe
   * walk of __ccol_event_loop_destroy, over the fd_index chmap and the
   * unlink of queue_regs_head, which has to run for every stripe, stripe 0
   * included. */
  const int n = 20;
  int pfds[20][2];
  ccol_circular_queue *queues[20];
  ccol_event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  {
    ccol_event_loop_construct_scoped(loop, 32, 8, 1);
    char *err = NULL;

    for (int i = 0; i < n; i++) {
      REQUIRE_EQ(pipe(pfds[i]), 0);
      ccol_event_reg reg = ccol_event_loop_add(
          loop, ccol_selectable_from_fd(pfds[i][0], ccol_select_read), handlers,
          NULL, &err);
      REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

      queues[i] = ccol_circular_queue_create(4, NULL);
      ccol_event_reg qreg = ccol_event_loop_add(
          loop, ccol_selectable_from_circq(queues[i], ccol_select_read),
          handlers, NULL, &err);
      REQUIRE_NE(qreg, CCOL_EVENT_REG_INVALID);
    }
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)(2 * n));

    /* Leaving the scope destroys loop while every registration is live, so
     * __ccol_event_loop_destroy walks all of them across every stripe
     * and frees them. */
  }

  for (int i = 0; i < n; i++) {
    close(pfds[i][0]);
    close(pfds[i][1]);
    ccol_circular_queue_destroy(queues[i]);
  }
}

/* Multi-threaded reactor (num_reactor_threads > 1) tests below */

TEST(ccol_event_loop, multi_thread_basic_smoke) {
  /* A plain single-fd readable dispatch, but with several reactor threads
   * sharing the epoll instance. It confirms that ordinary dispatch works
   * correctly once more than one thread calls epoll_wait on the same epfd,
   * which is more than "the test doesn't crash". */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* A nested block, for the same reason as the other multi-thread tests
     * below. evl_sync_ctx_destroy assumes no callback can touch ctx any more
     * by the time it runs, and nothing else in this test makes sure of that.
     * The number of reactor threads doesn't matter here, because even with
     * one the poller is a thread of its own and ccol_event_loop_remove
     * explicitly doesn't wait for a dispatch that's already in flight when
     * it's called. The join is what makes this safe, so ctx must outlive
     * the loop, and both leaving this block and an explicit
     * ccol_event_loop_shutdown before the teardown give that join. */
    ccol_event_loop_construct_scoped(loop, 8, 4, 6);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    ccol_event_loop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct evl_no_double_dispatch_ctx {
  pthread_mutex_t mtx;
  int active;
  bool violation;
  _Atomic int total_calls;
} evl_no_double_dispatch_ctx;

/* This callback holds the region that entry->dispatch_lock protects open
 * for a moment on purpose, sleeping after it checks and updates `active`.
 * That widens the window for a second reactor thread to get this same,
 * still-ready fd from its own concurrent epoll_wait call and enter this
 * callback at the same time. epoll's default level-triggered,
 * non-EPOLLEXCLUSIVE semantics really allow that, but only a broken or
 * missing dispatch_lock lets it happen. */
static void evl_no_double_dispatch_on_readable(ccol_event_loop loop,
                                               ccol_event_reg reg,
                                               ccol_selectable *sel,
                                               void *arg) {
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
  struct timespec ts = {0, 200000}; /* 0.2ms */
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

typedef struct evl_feeder_args {
  int write_fd;
  int iterations;
} evl_feeder_args;

static void *evl_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n; /* A full pipe buffer that blocks for a short time is fine
              * here, and even wanted: it keeps the read end ready all
              * the time, so the concurrent epoll_wait calls of several
              * reactor threads have the best chance to all observe it
              * ready at once. */
  }
  return NULL;
}

TEST(ccol_event_loop, multi_thread_no_double_dispatch_same_fd) {
  /* Many reactor threads and ONE hot fd that a writer thread keeps feeding,
   * which is exactly the thundering-herd scenario entry->dispatch_lock
   * exists to close (see that lock's field comment in cthreadcomm.c).
   * Without the lock this test reliably catches active > 1 under stress, and
   * with it active never goes above 1 however many reactor threads race for
   * the same entry. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  /* This fd is non-blocking because the callback below calls read()
   * directly, the way real code would under really concurrent dispatch.
   * Once the feeder thread stops, a legitimate thundering-herd duplicate
   * dispatch can read a pipe that's already drained and quiet for good, and
   * a blocking read there would block for ever and deadlock the reactor
   * thread that ran it. See the comment of evl_set_nonblocking. */
  evl_set_nonblocking(pfd[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* A nested block, with the same pattern and comment as
     * multi_thread_cross_direction_serialization. The test must not destroy
     * ctx or close pfd until it has really joined every reactor thread,
     * which happens at the closing brace of this block; a grace period that
     * only guesses isn't enough. */
    ccol_event_loop_construct_scoped(loop, 8, 4, 12);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_no_double_dispatch_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 4000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_feeder_thread, &feeder_args),
               0);
    pthread_join(feeder, NULL);

    /* A short grace period for the reactor threads to finish draining what's
     * left in the pipe after the feeder stops. It's only there so total_calls
     * gets close to feeder_args.iterations before the assertion below; the
     * join of the nested block is what makes the test safe. */
    for (int spins = 0;
         spins < 400 && atomic_load(&ctx.total_calls) < feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(loop, reg);

    /* Leaving the block shuts loop down and joins every reactor thread, so
     * ctx and pfd are quiet from here on. */
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(pfd[0]);
  close(pfd[1]);
}

/* The same active-counter protocol as evl_no_double_dispatch_ctx, but
 * shared by a read-direction callback and a write-direction callback on the
 * SAME fd, which checks the stricter cross-direction guarantee of
 * entry->dispatch_lock and not only self-exclusion. */
static void evl_cross_dir_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                      ccol_selectable *sel, void *arg) {
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

static void evl_cross_dir_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                                      ccol_selectable *sel, void *arg) {
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

TEST(ccol_event_loop, multi_thread_cross_direction_serialization) {
  /* A stream socketpair gives one fd whose two directions are live
   * independently. The write direction of sv[0] stays ready for ever, since
   * nothing here writes from sv[0] to sv[1] and so nothing fills its send
   * buffer, and a peer thread keeps feeding sv[1], which keeps the read
   * direction of sv[0] ready as well. So both directions really are
   * dispatchable at the same time, across many reactor threads, for the
   * whole test. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  /* This fd is non-blocking for the same reason as pfd[0] of
   * multi_thread_no_double_dispatch_same_fd; see the comment of
   * evl_set_nonblocking. */
  evl_set_nonblocking(sv[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* A nested block. The destructor of ccol_event_loop_construct_scoped
     * fires at the closing brace of this block and shuts down AND JOINS
     * every reactor thread before control leaves the block. That join is a
     * real guarantee rather than a guess: no callback that references ctx
     * can run after it, while ThreadSanitizer catches a real race through a
     * grace period built on a fixed nanosleep. The documented contract of
     * ccol_event_loop_remove only guarantees that an in-flight callback for
     * the reg being removed finishes, not that no OTHER dispatch for the
     * same, still live entry is running, because another reactor thread can
     * collect such a dispatch on its own, which is a legitimate and expected
     * thundering-herd duplicate. multi_thread_no_double_dispatch_same_fd
     * exists to prove that the library serializes those duplicates with a
     * lock instead of removing them. Destroying ctx.mtx before that join is
     * exactly the hazard this nested block avoids, and returning from this
     * function before the join is the same hazard, because it frees ctx's
     * stack slot. */
    ccol_event_loop_construct_scoped(loop, 8, 4, 12);

    ccol_event_handlers_t read_handlers = {
        .on_readable = evl_cross_dir_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_handlers_t write_handlers = {
        .on_readable = NULL,
        .on_writable = evl_cross_dir_on_writable,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), read_handlers,
        &ctx, &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), write_handlers,
        &ctx, &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);

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

    ccol_event_loop_remove(loop, rreg);
    ccol_event_loop_remove(loop, wreg);

    /* Leaving the block shuts loop down and joins every reactor thread, so
     * ctx is quiet from here on. */
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(sv[0]);
  close(sv[1]);
}

typedef struct evl_reuse_ctx {
  ccol_event_reg reg;
  uint64_t expected_generation;
  _Atomic int mismatch_count;
  _Atomic int call_count;
} evl_reuse_ctx;

static void evl_reuse_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg) {
  (void)reg;
  evl_reuse_ctx *c = (evl_reuse_ctx *)arg;
  char buf[16];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  /* A stale batch entry can come from a PREVIOUS registration the test has
   * already removed, on a recycled fd number. If such an entry ever
   * misdispatches into this callback with the WRONG pairing of ctx and reg,
   * this check sees a generation mismatch. */
  if (ccol_event_loop_reg_generation(loop, c->reg) != c->expected_generation) {
    atomic_fetch_add(&c->mismatch_count, 1);
  }
  atomic_fetch_add(&c->call_count, 1);
}

typedef struct evl_reuse_driver_args {
  ccol_event_loop loop;
  int iterations;
  _Atomic int mismatch_total;
  /* The driver logs each iteration's ctx here instead of freeing it in
   * place. The documented contract of ccol_event_loop_remove says the call is
   * "safe to call concurrently with an in-flight dispatch" and that
   * "teardown is deferred until any in-progress callback returns", but it
   * does NOT promise that no more callback calls for this reg can be in
   * flight by the time remove() returns, since a second reactor thread can
   * collect the same, still registered entry from its own epoll_wait batch,
   * which is the documented thundering-herd behavior other tests in this
   * file exercise too. Freeing ctx in place, or reusing its memory on the
   * next iteration, would race a stale callback that's still running, whose
   * reads and writes would meet this iteration's writes and frees. Two other
   * designs don't work here either: a ctx nobody ever frees makes valgrind
   * report definitely-lost, and a fixed-length grace-period sleep makes
   * ThreadSanitizer report a real use-after-free. Only a real join gives the
   * guarantee, so the test function frees every logged ctx only after it
   * has shut down and joined the whole ccol_event_loop, meaning every
   * reactor thread. The nested-block comment of that test explains why a
   * join is a real guarantee and a sleep isn't. */
  evl_reuse_ctx **ctx_log;
  int ctx_log_count;
  /* This fd stays open for this driver's ENTIRE run instead of being closed
   * and reopened every iteration. Re-adding the SAME still-open fd after a
   * removal mints a fresh event_entry, and so a fresh generation, every
   * time anyway, because ccol_event_loop_remove deletes the fd's registry
   * entry before it returns, so a later ccol_event_loop_add on the same fd
   * number always takes the new-entry path. That's enough to exercise this
   * module's core generation guarantee under heavy concurrent churn of add,
   * remove, dispatch and reclaim calls across drivers sharing one
   * ccol_event_loop. It also avoids calling close() on an fd that a
   * legitimate, rare thundering-herd duplicate dispatch can still be
   * reading, since a close per iteration produces exactly that
   * close()-against-read() race, which ThreadSanitizer reports. */
  int pfd[2];
} evl_reuse_driver_args;

static void *evl_reuse_driver_thread(void *arg) {
  evl_reuse_driver_args *a = (evl_reuse_driver_args *)arg;
  ccol_event_handlers_t handlers = {.on_readable = evl_reuse_on_readable,
                                    .on_writable = NULL,
                                    .on_error = NULL};

  for (int i = 0; i < a->iterations; i++) {
    evl_reuse_ctx *ctx = malloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        a->loop, ccol_selectable_from_fd(a->pfd[0], ccol_select_read), handlers,
        ctx, &err);
    if (!reg) {
      free(ctx);
      continue;
    }
    ctx->reg = reg;
    ctx->expected_generation = ccol_event_loop_reg_generation(a->loop, reg);
    a->ctx_log[a->ctx_log_count++] = ctx;

    int val = i;
    ssize_t wn = write(a->pfd[1], &val, sizeof(val));
    (void)wn;

    for (int spins = 0; spins < 500 && atomic_load(&ctx->call_count) == 0;
         spins++) {
      struct timespec ts = {0, 500000};
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(a->loop, reg);

    atomic_fetch_add(&a->mismatch_total, atomic_load(&ctx->mismatch_count));
  }
  return NULL;
}

TEST(ccol_event_loop, multi_thread_fd_reuse_generation_stays_consistent) {
  /* Several driver threads run at the same time, each registering, writing
   * and removing against its own fd in a tight loop while they all share one
   * ccol_event_loop. That's exactly the high-churn workload of add, remove,
   * dispatch and reclaim calls under which the library must mint a fresh
   * generation every single time and every dispatch must see that
   * generation correctly, the core guarantee ccol_event_loop_reg_generation
   * exists to make safe by construction (see its own doc comment). The bug
   * CLASS is stale identity confusion across one fd's registration
   * lifecycle, which a hand-off that reuses an fd across a redirect also
   * faces. The test exercises it as repeated re-registration of the same fd
   * under concurrent load rather than a real close and reopen at the OS
   * level, which avoids a race between a legitimate thundering-herd
   * duplicate dispatch and a close() call (see the comment of
   * evl_reuse_driver_args). It asserts that every dispatch saw the
   * generation of its OWN registration and that no dispatch ever saw a stale
   * or mismatched one. */
  const int n_drivers = 4;
  const int iterations = 150;
  pthread_t drivers[4];
  evl_reuse_driver_args args[4];

  for (int i = 0; i < n_drivers; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* This fd is non-blocking for the same reason as pfd[0] of
     * multi_thread_no_double_dispatch_same_fd (see the comment of
     * evl_set_nonblocking). Here it also covers one more window, from the
     * moment a driver's last iteration removes its registration until the
     * whole loop is torn down. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].iterations = iterations;
    atomic_init(&args[i].mismatch_total, 0);
    args[i].ctx_log = malloc(sizeof(evl_reuse_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  {
    /* A nested block, with the same pattern and comment as
     * multi_thread_cross_direction_serialization. The loop's destructor shuts
     * down and joins every reactor thread at the closing brace of this block,
     * and that join, not a guess at the timing, is what makes it safe to free
     * every logged ctx below. */
    ccol_event_loop_construct_scoped(loop, 8, 4, 8);
    int started = 0;
    for (int i = 0; i < n_drivers; i++) {
      args[i].loop = loop;
      /* This loop counts the failures instead of asserting inside itself. A
       * REQUIRE_* here would return from the test while the threads earlier
       * iterations created keep running and keep using args[] and `loop`,
       * whose scoped destructor runs on that very return. The test joins
       * only the threads that really started and checks the count once all
       * of them are back. */
      if (pthread_create(&drivers[i], NULL, evl_reuse_driver_thread,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(drivers[i], NULL);
    }

    /* The fd stress test above explains why we capture this value here
       instead of asserting on it. */
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

TEST(ccol_event_loop, multi_thread_shutdown_joins_poller_promptly) {
  /* Many reactor threads and nothing registered, so only ONE thread,
   * poller_thread, ever blocks in epoll_wait here, while the rest are idle
   * ctpool workers with nothing queued (see
   * multi_thread_shutdown_drains_idle_dispatch_pool_promptly below for that
   * half). The single write() ccol_event_loop_shutdown makes to shutdown_efd
   * must wake poller_thread and join it promptly. Nothing ever drains
   * shutdown_efd, on purpose (the comment of ccol_event_loop_shutdown says
   * which hang draining it causes), so poller_thread's epoll_wait keeps
   * seeing it ready however long that thread takes to return and notice
   * shutting_down. The test bounds how long the shutdown may take instead of
   * only asserting that it returns in the end, so a regression that leaves
   * poller_thread stuck shows up as a slow or hung test and not a silent
   * pass. */
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  ccol_event_loop_destroy(loop);
}

TEST(ccol_event_loop,
     multi_thread_shutdown_drains_idle_dispatch_pool_promptly) {
  /* The other half of the shutdown. With num_reactor_threads > 1,
   * ccol_event_loop_shutdown also calls ctpool_shutdown_drain on
   * dispatch_pool, a completely different mechanism from the poller-wake
   * path above: a condition-variable broadcast that wakes idle ctpool
   * workers rather than an epoll_wait wakeup. The test bounds how long that
   * call takes with a large, fully idle worker pool, the same way the test
   * above bounds the poller half, so a regression that leaves some idle
   * worker unwoken shows up here as a slow or hung test. */
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  ccol_event_loop_destroy(loop);
}

typedef struct evl_shutdown_drain_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool started;
  bool finished;
} evl_shutdown_drain_ctx;

/* This callback signals "started" the moment it begins to run, then sleeps,
 * and only then signals "finished". That gives the test driver a reliable
 * way to call ccol_event_loop_shutdown while this job is provably in flight,
 * and without it the shutdown races the poller, which might not even notice
 * the write that triggers the callback. */
static void evl_shutdown_drain_on_readable(ccol_event_loop loop,
                                           ccol_event_reg reg,
                                           ccol_selectable *sel, void *arg) {
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

  struct timespec ts = {0, 100000000}; /* 100ms */
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->finished = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

TEST(ccol_event_loop, multi_thread_shutdown_drains_in_flight_dispatch_job) {
  /* Unlike the two tests above, which use an idle pool, this one puts a real
   * job in flight whose callback sleeps on purpose and synchronizes with the
   * test driver, so the driver knows the callback really started before it
   * calls ccol_event_loop_shutdown. That directly exercises the "finish
   * in-flight work" contract of ctpool_shutdown_drain, which the module picks
   * over ctpool_shutdown_immediate to keep ccol_event_loop_shutdown's
   * documented guarantee that no dispatch can be in flight once the call
   * returns (see the comment of that function). The test asserts that the
   * callback really completed, not just that the shutdown returned, which
   * immediate-cancel semantics wouldn't guarantee. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_shutdown_drain_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);
  assert(pthread_cond_init(&ctx.cond, NULL) == 0);

  ccol_event_loop loop = ccol_event_loop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  ccol_event_handlers_t handlers = {
      .on_readable = evl_shutdown_drain_on_readable,
      .on_writable = NULL,
      .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 2;
  pthread_mutex_lock(&ctx.mtx);
  while (!ctx.started) {
    if (pthread_cond_timedwait(&ctx.cond, &ctx.mtx, &deadline) == ETIMEDOUT)
      break;
  }
  bool started = ctx.started;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_TRUE(started);

  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  bool finished = ctx.finished;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_TRUE(finished);

  ccol_event_loop_destroy(loop);
  pthread_mutex_destroy(&ctx.mtx);
  pthread_cond_destroy(&ctx.cond);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, reg_generation_semantics) {
  /* A NULL reg reads as generation 0, a reserved value the library never
   * mints for a real registration. */
  REQUIRE_EQ(ccol_event_loop_reg_generation(CCOL_EVENT_LOOP_INVALID,
                                            CCOL_EVENT_REG_INVALID),
             (uint64_t)0);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int pfd2[2];
  REQUIRE_EQ(pipe(pfd2), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* A nested block, for the same reason as the other multi-thread tests in
     * this file. The write direction of wreg dispatches almost at once,
     * because a fresh pipe write end is always writable, and this loop has 4
     * reactor threads. So evl_sync_ctx_destroy must not run until that
     * dispatch, and any other one in flight, is sure to be finished, and this
     * function must not return before then either, because returning frees
     * ctx's stack slot. Only this block's join gives that guarantee. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 4);
    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;

    /* Both directions on the same fd share one generation. */
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(rreg, CCOL_EVENT_REG_INVALID);
    uint64_t rgen = ccol_event_loop_reg_generation(loop, rreg);
    REQUIRE_GT(rgen, (uint64_t)0);

    ccol_event_handlers_t write_handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_write),
        write_handlers, &ctx, &err);
    REQUIRE_NE(wreg, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_generation(loop, wreg), rgen);

    /* Flipping the direction with ccol_event_loop_modify keeps the same
     * generation, since it's the same fd and the same connection under it,
     * just a different direction. */
    ccol_event_loop_remove(loop, wreg);
    REQUIRE_EQ(ccol_event_loop_modify(loop, rreg, ccol_select_write),
               ccol_success);
    REQUIRE_EQ(ccol_event_loop_reg_generation(loop, rreg), rgen);
    REQUIRE_EQ(ccol_event_loop_modify(loop, rreg, ccol_select_read),
               ccol_success);

    /* A different fd gets a different generation. */
    ccol_event_reg reg2 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd2[0], ccol_select_read), handlers,
        &ctx, &err);
    REQUIRE_NE(reg2, CCOL_EVENT_REG_INVALID);
    REQUIRE_NE(ccol_event_loop_reg_generation(loop, reg2), rgen);

    ccol_event_loop_remove(loop, rreg);
    ccol_event_loop_remove(loop, reg2);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
  close(pfd2[0]);
  close(pfd2[1]);
}

/* This callback does nothing. The test registers it only to keep the poller
 * going round its loop (epoll_wait returning, poller_batch_gen advancing,
 * epoll_wait entered again) as fast as possible for the whole run of
 * reg_handle_survives_concurrent_remove_vs_accessor_race below. A pipe's
 * write end is writable from the moment it exists and stays that way for
 * ever, so a few of them registered for write interest keep the reactor busy
 * without a second thread feeding it. */
static void evl_reg_race_hot_writable(ccol_event_loop loop, ccol_event_reg reg,
                                      ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

typedef struct evl_reg_race_args {
  ccol_event_loop loop;
  ccol_event_reg reg;
} evl_reg_race_args;

static void *evl_reg_race_remover_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  ccol_event_loop_remove(a->loop, a->reg);
  return NULL;
}

static void *evl_reg_race_accessor_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  /* This thread really races the ccol_event_loop_remove call on the other
   * thread for the exact same reg. Any return value from any of these five
   * calls is acceptable (ccol_success, ccol_not_permitted, ccol_invalid_args
   * or generation 0), since the value depends only on which thread the
   * scheduler lets win. What the test checks is something else: none of
   * these calls may ever touch memory the reclaimer has already freed under
   * them. A clean run shows that, and a clean `make memtest` run shows it
   * best. See the field comment of reg_slots in struct _ccol_event_loop_s. */
  ccol_event_loop_modify(a->loop, a->reg, ccol_select_write);
  ccol_event_loop_pause(a->loop, a->reg);
  ccol_event_loop_resume(a->loop, a->reg);
  (void)ccol_event_loop_reg_generation(a->loop, a->reg);
  ccol_event_loop_remove(a->loop, a->reg);
  return NULL;
}

TEST(ccol_event_loop, reg_handle_survives_concurrent_remove_vs_accessor_race) {
  /* ccol_event_loop_modify, ccol_event_loop_pause, ccol_event_loop_resume,
   * ccol_event_loop_remove and ccol_event_loop_reg_generation must not
   * dereference a reg pointer from the caller directly, starting with
   * reg->stripe_idx, which only tells the call which stripe lock would
   * protect the rest of it. Such a dereference has no protection against a
   * concurrent poller reclaim of that exact reg, which can complete on the
   * very next turn of the poller loop after a DIFFERENT thread deferred it
   * with ccol_event_loop_remove(). For two threads racing on the same reg
   * that's a real use-after-free, not a theoretical concern. So
   * ccol_event_reg is an opaque, generation-checked VALUE handle that the
   * library resolves through its own loop's registration table before it
   * dereferences anything, the same way ccol_event_loop's own handle works.
   * See the field comment of reg_slots in struct _ccol_event_loop_s for the
   * full design.
   *
   * A few always-ready write-direction "hot" registrations keep the poller
   * cycling for the whole test, so an unprotected window gets exercised as
   * often as possible. The test then races many short-lived fd
   * registrations back to back: one thread removes each of them while a
   * second thread calls every other reg-accessor entry point on that exact
   * reg at the same time. That's the most direct way to catch a regression
   * here. Run it under `make memtest`, where an unprotected dereference
   * reliably reports a real use-after-free that valgrind detects instead of
   * a failure that only happens now and then. */
  ccol_event_loop_construct_scoped(loop, 64, 4, 4);

  enum { N_HOT = 4 };
  int hot_pfd[N_HOT][2];
  ccol_event_reg hot_reg[N_HOT];
  ccol_event_handlers_t hot_handlers = {
      .on_readable = NULL,
      .on_writable = evl_reg_race_hot_writable,
      .on_error = NULL};
  for (int i = 0; i < N_HOT; i++) {
    REQUIRE_EQ(pipe(hot_pfd[i]), 0);
    char *err = NULL;
    hot_reg[i] = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(hot_pfd[i][1], ccol_select_write),
        hot_handlers, NULL, &err);
    REQUIRE_NE(hot_reg[i], CCOL_EVENT_REG_INVALID);
  }

  const int iterations = 300;
  ccol_event_handlers_t reg_handlers = {0};
  for (int i = 0; i < iterations; i++) {
    int pfd[2];
    REQUIRE_EQ(pipe(pfd), 0);
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), reg_handlers,
        NULL, &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

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
    ccol_event_loop_remove(loop, hot_reg[i]);
    close(hot_pfd[i][0]);
    close(hot_pfd[i][1]);
  }
}

typedef struct evl_bounded_ctx {
  _Atomic size_t calls;
} evl_bounded_ctx;

/* This callback is slower on purpose than a feeder that keeps refilling,
 * which gives the poller the best chance to come back to epoll_wait while a
 * dispatch job for this exact fd is queued or running. That's exactly the
 * scenario that mints an unbounded stream of redundant dispatch jobs, and
 * only the re-arm-after-dispatch handling of EPOLLONESHOT stops it; see the
 * comment of _ccol_event_loop_poller_collect in cthreadcomm.c. */
static void evl_bounded_hot_fd_on_readable(ccol_event_loop loop,
                                           ccol_event_reg reg,
                                           ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  char buf[64];
  ssize_t n;
  do {
    n = read(sel->fd, buf, sizeof(buf));
  } while (n > 0);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

static void *evl_bounded_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n;
  }
  return NULL;
}

TEST(ccol_event_loop, multi_thread_hot_fd_dispatch_pool_pending_stays_bounded) {
  /* A direct regression test for the EPOLLONESHOT re-arm contract.
   * Collection runs on poller_thread and dispatch on a ctpool worker, so the
   * two are decoupled, and an fd that is still ready but hasn't been re-armed
   * yet must never be collected again. Without that rule the pending-job
   * count of dispatch_pool grows without bound for as long as a slow callback
   * lags behind a fast feeder. The test samples
   * _ccol_event_loop_dispatch_pool_pending_count_for_tests while a feeder
   * thread writes the whole time, and asserts that the count never goes
   * above a small bound, well below num_reactor_threads - 1 workers plus one
   * more queued job. The livelock this test guards against goes far past
   * that bound rather than just a little over it. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    ccol_event_loop_construct_scoped(loop, 8, 4, 8);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_bounded_hot_fd_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    /* This count is modest on purpose. At about 0.5ms per dispatch it drains
     * completely inside the grace period below, the same pattern as
     * multi_thread_no_double_dispatch_same_fd. What stresses the mechanism
     * is the window where the test samples while the feeder runs, not the
     * total byte count. */
    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_bounded_feeder_thread, &feeder_args),
        0);

    for (int i = 0; i < 200; i++) {
      size_t pending =
          _ccol_event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* A short, best-effort settle time before any teardown, only as test
     * hygiene. Nothing here asserts on unread pipe bytes, unlike the queue
     * version of this test below where ccol_circular_queue_destroy does
     * assert. The max_pending assertion holds without this loop, which
     * stays short because a full drain isn't needed and isn't worth the
     * test's wall-clock time. */
    for (int spins = 0; spins < 100 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(loop, reg);
  }

  REQUIRE_LT(max_pending, (size_t)10);
}

typedef struct evl_bounded_queue_feeder_args {
  ccol_circular_queue *cq;
  int iterations;
} evl_bounded_queue_feeder_args;

static void *evl_bounded_queue_feeder_thread(void *arg) {
  evl_bounded_queue_feeder_args *a = (evl_bounded_queue_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    /* This send blocks, it isn't ccol_circq_try_send_zc. A full queue should
     * throttle this feeder exactly as a full pipe buffer throttles the plain
     * write() of evl_bounded_feeder_thread above, so both tests get the same
     * guarantee that every iteration is delivered in the end. A try-send
     * would silently drop the sends past the capacity, and ctx.calls would
     * then never reliably reach iterations below. */
    (void)ccol_circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

/* The same slow-callback shape as evl_bounded_hot_fd_on_readable, but for
 * the path of the bridge eventfd and the queue selectable. The design note
 * in _ccol_event_loop_dispatch_job_fn marks this case as one with NO natural
 * throttle of its own, because a bridge eventfd has no finite kernel buffer
 * while a pipe does, so this one is the more valuable of the two regression
 * tests rather than a redundant mirror of the fd one. */
static void evl_bounded_hot_queue_on_readable(ccol_event_loop loop,
                                              ccol_event_reg reg,
                                              ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)ccol_circq_try_recv_zc(sel->cq, &msg);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

TEST(ccol_event_loop,
     multi_thread_hot_queue_dispatch_pool_pending_stays_bounded) {
  ccol_circular_queue *cq = ccol_circular_queue_create(64, NULL);
  REQUIRE_NE((void *)cq, NULL);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    ccol_event_loop_construct_scoped(loop, 8, 4, 8);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_bounded_hot_queue_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    evl_bounded_queue_feeder_args feeder_args = {.cq = cq, .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_bounded_queue_feeder_thread,
                              &feeder_args),
               0);

    for (int i = 0; i < 200; i++) {
      size_t pending =
          _ccol_event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* A best-effort grace period that nothing asserts on. Comparing directly
     * against num_reactor_threads == 1 shows that a queue reader with only
     * ONE registration can legitimately finish with a few messages left
     * unconsumed once the feeder stops, because of the notify-on-send
     * coalescing of the bridge eventfd, and that has nothing to do with the
     * EPOLLONESHOT coverage of this test. The direct drain below, after the
     * registration is removed, is what makes sure ccol_circular_queue_destroy
     * never asserts, not this loop. */
    for (int spins = 0; spins < 500 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(loop, reg);
  }

  /* Drains directly whatever the consumption above, driven by
   * ccol_event_loop, didn't reach (see the comment of the grace-period
   * loop). It runs here because the test has already removed reg and the
   * loop's scope has already closed. ccol_circular_queue_destroy asserts on
   * any message that's left (see its own comment in cthreadcomm.c), so this
   * drain is needed for a clean teardown and isn't optional hygiene. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }

  ccol_circular_queue_destroy(cq);
  REQUIRE_LT(max_pending, (size_t)10);
}

/* An on_readable handler that never returns on its own. Its only use is to
 * hold a job in flight on a dispatch_pool worker (or, for
 * num_reactor_threads == 1, to occupy the only thread instead) long enough
 * for the test driver to reliably call ccol_event_loop_shutdown from inside
 * it. */
typedef struct evl_self_shutdown_ctx {
  ccol_event_loop loop;
  _Atomic int observed_rv; /* This holds a ccol_retval_t. It is _Atomic for
                            * one reason: the test driver polls it from the
                            * main thread while the callback, which runs on
                            * poller_thread or on a dispatch_pool worker,
                            * writes it, and nothing else synchronizes the
                            * two. A plain ccol_retval_t field here is a
                            * real data race that ThreadSanitizer
                            * reports. */
} evl_self_shutdown_ctx;

static void evl_self_shutdown_on_readable(ccol_event_loop loop,
                                          ccol_event_reg reg,
                                          ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  evl_self_shutdown_ctx *c = (evl_self_shutdown_ctx *)arg;
  atomic_store(&c->observed_rv, (int)ccol_event_loop_shutdown(loop));
}

TEST(ccol_event_loop,
     shutdown_from_within_callback_returns_not_permitted_single_thread) {
  /* With num_reactor_threads == 1 the only thread both polls and
   * dispatches, so without the guard a self-call here joins that thread to
   * itself and gets EDEADLK. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);
  ctx.loop = loop;

  ccol_event_handlers_t handlers = {
      .on_readable = evl_self_shutdown_on_readable,
      .on_writable = NULL,
      .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  /* A bounded poll instead of evl_wait_for, because this ctx has no mutex
   * and no condition variable of its own on purpose. The test waits on the
   * default sentinel value of observed_rv instead. */
  for (int i = 0;
       i < 500 && atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure;
       i++) {
    struct timespec ts = {0, 2000000}; /* 2ms */
    nanosleep(&ts, NULL);
  }
  REQUIRE_EQ(atomic_load(&ctx.observed_rv), (int)ccol_not_permitted);

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop,
     shutdown_from_within_callback_returns_not_permitted_multi_thread) {
  /* With num_reactor_threads > 1 the callback runs on a dispatch_pool
   * worker, not on poller_thread. Without the guard a self-call here calls
   * ctpool_shutdown_drain from inside one of that pool's workers, which then
   * joins itself with _ccol_thread_join. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  ccol_event_loop loop = ccol_event_loop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);
  ctx.loop = loop;

  ccol_event_handlers_t handlers = {
      .on_readable = evl_self_shutdown_on_readable,
      .on_writable = NULL,
      .on_error = NULL};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  for (int i = 0;
       i < 500 && atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure;
       i++) {
    struct timespec ts = {0, 2000000}; /* 2ms */
    nanosleep(&ts, NULL);
  }
  REQUIRE_EQ(atomic_load(&ctx.observed_rv), (int)ccol_not_permitted);

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

/* ========================================================================== */
/*         EVENT_LOOP HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)         */
/* ========================================================================== */

/* This group mirrors the chttpcli_handle_lifecycle test group in
 * tests/chttpclient/tests.c, adapted for the fully lock-free pin mechanism
 * of ccol_event_loop. The field comment of pending_resolve_count in struct
 * _ccol_event_loop_s in src/cthreadcomm.c explains why the unpin side here
 * is a bare atomic decrement instead of the lock-protected one of chttpcli
 * and chttpsvr, and the comment of __ccol_event_loop_destroy explains why
 * that makes destroy wait by polling instead of on a condition variable. */

/* A destroy can complete fully, but a second destroy later, on a copy of the
 * same original handle value that the caller kept separately, has to be a
 * fatal error. It runs in a forked child because _ccol_fatal_err aborts the
 * whole process, following the precedent tests/clogger/tests.c sets for a
 * fork test of misuse that stops the process. */
TEST(ccol_event_loop_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
    ccol_event_loop stale = loop;     /* This is a copy of the handle value that
            the caller holds on its own. It differs from the local variable that
            the macro below invalidates. */
    ccol_event_loop_destroy(loop);    /* This completes normally. The local
           `loop` is now CCOL_EVENT_LOOP_INVALID, but `stale` still holds the
           original value. */
    __ccol_event_loop_destroy(stale); /* This is the misuse under test. It is
        a second, purely sequential destroy of a handle that is already fully
        torn down. */
    _exit(0); /* not reachable when _ccol_fatal_err() aborts, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

static void evl_self_destroy_on_readable(ccol_event_loop loop,
                                         ccol_event_reg reg,
                                         ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  (void)arg;
  ccol_event_loop_destroy(loop); /* This is the misuse under test. */
}

/* Calling ccol_event_loop_destroy on a loop has to be a fatal error when the
 * call comes from inside a callback that dispatches on a thread of that same
 * loop: the poller thread for num_reactor_threads == 1, or a dispatch_pool
 * worker for a larger count. That matches the sequential and concurrent
 * double-destroy cases above, instead of being a teardown the library
 * silently skips. The internal ccol_event_loop_shutdown call of
 * __ccol_event_loop_destroy already detects and refuses a self-join here
 * (see shutdown_from_within_callback_returns_not_permitted_* above), but
 * that alone isn't enough because __ccol_event_loop_destroy can't pass
 * the refusal on through its macro-driven contract, which returns void.
 * Without a guard of its own it frees every live registration and the loop
 * struct itself under the callback that's running, which is a real
 * use-after-free, beyond a deadlock. It runs in a forked child because
 * _ccol_fatal_err aborts the whole process. */
TEST(ccol_event_loop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_single_thread) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) _exit(2);
    evl_set_nonblocking(pfd[0]);

    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_self_destroy_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
        NULL);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val)) _exit(2);

    /* Bounds this child's lifetime in case _ccol_fatal_err somehow doesn't
     * fire, which would hang the whole suite otherwise. The abort ends the
     * child long before this sleep does, so its length only needs to cover a
     * slow or loaded machine. It stays below the parent's 20 s bound, so a
     * child that doesn't abort exits and fails the WIFSIGNALED check below
     * instead of the reap. */
    struct timespec ts = {15, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* not reachable when _ccol_fatal_err() aborts, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(ccol_event_loop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_multi_thread) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) _exit(2);
    evl_set_nonblocking(pfd[0]);

    ccol_event_loop loop = ccol_event_loop_create(8, 4, 4, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_self_destroy_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL,
        NULL);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val)) _exit(2);

    /* See the single-thread version above for this bound. */
    struct timespec ts = {15, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* not reachable when _ccol_fatal_err() aborts, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  ccol_event_loop h;
} evl_concurrent_destroy_arg_t;

static void *evl_concurrent_destroy_thread(void *arg) {
  evl_concurrent_destroy_arg_t *a = (evl_concurrent_destroy_arg_t *)arg;
  __ccol_event_loop_destroy(a->h);
  return NULL;
}

/* Two threads may call destroy on two copies of the SAME still-valid handle,
 * each holding its own copy, as close to the same moment as possible, and
 * that is fatal too. It's the same class of concurrent double free
 * the generation-tagged slot table exists to close for chttpcli and
 * chttpsvr. */
TEST(ccol_event_loop_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
    evl_concurrent_destroy_arg_t a1 = {.h = loop};
    evl_concurrent_destroy_arg_t a2 = {.h = loop};
    pthread_t t1, t2;
    /* if-guards instead of REQUIRE_EQ, because this runs inside the forked
     * child above (pid == 0); the same guard and comment on
     * cq_select_waiter_thread earlier in this file explain why the
     * early-return failure path of REQUIRE_EQ isn't safe here. Joining a
     * pthread_t that pthread_create never initialized is undefined
     * behavior, it can hang or crash, or worse give a stray SIGABRT from
     * something unrelated, so the test would seem to pass for the wrong
     * reason. These guards exit with a distinct code instead, so a failed
     * create is reported as what it is and nobody mistakes it for the
     * _ccol_fatal_err() this test expects. */
    if (pthread_create(&t1, NULL, evl_concurrent_destroy_thread, &a1) != 0)
      _exit(3);
    if (pthread_create(&t2, NULL, evl_concurrent_destroy_thread, &a2) != 0)
      _exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* not reachable: whichever of the two destroy calls loses the
                  race must hit _ccol_fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  ccol_event_loop h;
  atomic_int pinned;
  atomic_int release;
  bool resolved;
} evl_pin_hold_arg_t;

static void *evl_pin_hold_thread(void *arg) {
  evl_pin_hold_arg_t *a = (evl_pin_hold_arg_t *)arg;
  a->resolved = _ccol_event_loop_resolve_pin_until_for_tests(a->h, &a->pinned,
                                                             &a->release);
  /* A handle that didn't resolve never sets pinned, so the test must not
   * wait for it. */
  if (!a->resolved) atomic_store(&a->pinned, -1);
  return NULL;
}

typedef struct {
  ccol_event_loop h;
  atomic_int returned;
} evl_destroy_arg_t;

static void *evl_destroy_thread(void *arg) {
  evl_destroy_arg_t *a = (evl_destroy_arg_t *)arg;
  ccol_event_loop_destroy(a->h);
  atomic_store(&a->returned, 1);
  return NULL;
}

/* This proves that destroy waits out a resolve that's in flight. A thread
 * resolves and pins the handle and holds the pin until this test releases
 * it, through _ccol_event_loop_resolve_pin_until_for_tests, since
 * ccol_event_loop has no naturally slow public entry point we could borrow.
 * A destroy on a second thread must block until the pin is released instead
 * of racing ahead and freeing the loop under the pointer that's still
 * resolved.
 *
 * The check doesn't compare two clocks. While release is clear the pin is
 * held, so a destroy that waits for pins can't have returned, however slow
 * the machine is. The wait before the check gives a destroy that doesn't
 * wait for pins enough time to return, and the test then fails. */
TEST(ccol_event_loop_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  evl_pin_hold_arg_t pin_arg = {.h = loop, .resolved = false};
  atomic_init(&pin_arg.pinned, 0);
  atomic_init(&pin_arg.release, 0);
  evl_destroy_arg_t destroy_arg = {.h = loop};
  atomic_init(&destroy_arg.returned, 0);

  pthread_t pin_thread, destroy_thread;
  bool pin_started =
      pthread_create(&pin_thread, NULL, evl_pin_hold_thread, &pin_arg) == 0;
  struct timespec poll = {.tv_sec = 0, .tv_nsec = 1000000};
  /* Only a bound against a hang, the pin normally lands at once. */
  for (int i = 0; pin_started && i < 10000 && atomic_load(&pin_arg.pinned) == 0;
       i++)
    nanosleep(&poll, NULL);
  bool pinned = atomic_load(&pin_arg.pinned) == 1;

  bool destroy_started = false;
  int returned_while_pinned = -1;
  if (pinned) {
    destroy_started = pthread_create(&destroy_thread, NULL, evl_destroy_thread,
                                     &destroy_arg) == 0;
    struct timespec margin = {.tv_sec = 0, .tv_nsec = 200000000}; /* 200 ms */
    nanosleep(&margin, NULL);
    returned_while_pinned = atomic_load(&destroy_arg.returned);
  }
  atomic_store(&pin_arg.release, 1);
  if (pin_started) pthread_join(pin_thread, NULL);
  if (destroy_started) {
    pthread_join(destroy_thread, NULL);
  } else {
    ccol_event_loop_destroy(loop);
  }

  REQUIRE_TRUE(pin_started);
  REQUIRE_TRUE(pin_arg.resolved);
  REQUIRE_TRUE(pinned);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_EQ(returned_while_pinned, 0);
  REQUIRE_EQ(atomic_load(&destroy_arg.returned), 1);
}

typedef struct {
  ccol_event_loop h;
} evl_reg_count_arg_t;

static void *evl_reg_count_thread(void *arg) {
  evl_reg_count_arg_t *a = (evl_reg_count_arg_t *)arg;
  /* We ignore the return value on purpose. A legitimate race with a
   * concurrent destroy can make this resolve fail and return (size_t)-1
   * instead of succeeding, and both outcomes are correct. This thread only
   * exists to make resolve, pin and unpin traffic at the same time as the
   * destroy thread below. */
  ccol_event_loop_reg_count(a->h);
  return NULL;
}

/* This test isn't redundant with resolve_then_use_race_destroy_waits above.
 * The long pin that test holds on purpose keeps pending_resolve_count above
 * 0 for the whole race window, so destroy's poll-wait always finds it
 * nonzero on its first check there. This one needs the opposite shape: it
 * races a fast, non-blocking entry point, ccol_event_loop_reg_count, which
 * resolves, does one atomic read, unpins and returns with no sleep at all,
 * against a concurrent destroy, and repeats that race under stress. For a
 * truly lock-free pin and unpin pair the failure window is only a few
 * instructions wide, so a single run without stress doesn't reproduce it
 * reliably. Each iteration uses a fresh loop, so every repetition gets its
 * own independent race and none of them reuses an already destroyed
 * handle. */
TEST(ccol_event_loop_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

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

/* The library must never confuse a legitimate reuse of a slot with a stale
 * handle to the slot's earlier occupant, and that's what the generation
 * counter is for. */
TEST(ccol_event_loop_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  ccol_event_loop a = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(a, CCOL_EVENT_LOOP_INVALID);
  ccol_event_loop stale_a = a;
  ccol_event_loop_destroy(a);

  ccol_event_loop b = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(b, CCOL_EVENT_LOOP_INVALID);

  /* B's operations must succeed normally whether or not the allocator
   * reused A's exact address for B. */
  REQUIRE_EQ(ccol_event_loop_reg_count(b), (size_t)0);

  /* A's stale handle must never resolve to B, even when B reuses the same
   * address below it; that's what the generation counter is for. */
  REQUIRE_EQ((void *)_ccol_event_loop_resolve_for_tests(stale_a), NULL);

  ccol_event_loop_destroy(b);
}

/* The slot table is bounded and doesn't grow for ever. A churn loop of
 * create and destroy calls keeps only a single slot in flight at a time, so
 * it should reuse that one freed slot on every iteration instead of growing
 * the table. We capture the capacity right after the first create and
 * destroy pair rather than asserting a fixed absolute value such as 1,
 * because earlier tests in the same process may already have grown the
 * table to some N above 1. What this test has to prove is that ITS OWN churn
 * adds no growth, without knowing the table's absolute size when it runs. */
TEST(ccol_event_loop_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  ccol_event_loop loop0 = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop0, CCOL_EVENT_LOOP_INVALID);
  ccol_event_loop_destroy(loop0);
  size_t capacity_after_first =
      _ccol_event_loop_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);
    ccol_event_loop_destroy(loop);
  }

  REQUIRE_EQ(_ccol_event_loop_slot_table_capacity_for_tests(),
             capacity_after_first);
}

/* ========================================================================== */
/*                       FORK SAFETY (pthread_atfork)                         */
/* ========================================================================== */

/* The whole rest of this file exercises the fork() safety machinery of
 * src/cthreadcomm.c, which is built on pthread_atfork(): the machinery of
 * ccol_event_loop itself and the merged mutex registry of
 * ccol_circular_queue and ccol_dynamic_queue. The compiler drops that
 * machinery when CCOL_FORK_SAFETY_REQUIRED is 0 (see the doc comment of that
 * macro in common.h), and without it the premises of these tests don't
 * hold: that a forked child never inherits a locked ccol_event_loop mutex or
 * queue mutex, and that it never runs into the AB-BA lock-ordering hazard
 * that two independently registered sets of atfork handlers are exposed to.
 * So these tests are dropped together with the machinery instead of staying
 * in to hang or fail. */
#if CCOL_FORK_SAFETY_REQUIRED

static void fork_safety_hot_fd_on_readable(ccol_event_loop loop,
                                           ccol_event_reg reg,
                                           ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)arg;
  char buf[64];
  while (read(sel->fd, buf, sizeof(buf)) > 0) {
  }
}

typedef struct {
  /* This field is _Atomic and not plain volatile, because volatile alone
   * gives no atomicity and no ordering under the C11 memory model, it only
   * stops the compiler from caching the read in a register. The ordering
   * between the main test thread's write (churn.stop = 1;) and this thread's
   * read matters, so a plain volatile field here would be a real data race
   * under the C11 memory model, even if a small one. Every other cross-thread
   * handshake flag in this file uses _Atomic for exactly this reason. */
  _Atomic int stop;
} fork_safety_churn_arg_t;

/* This thread keeps creating and destroying throwaway ccol_event_loop
 * instances, completely unrelated to the loop the main test thread keeps
 * busy below. Its only purpose is to have SOME thread inside the mutex of
 * ccol_event_loop_slot_table, which it reaches through
 * ccol_event_loop_create and ccol_event_loop_destroy, as often as possible,
 * so it races this test's repeated fork() calls. */
static void *fork_safety_churn_thread(void *arg) {
  fork_safety_churn_arg_t *a = (fork_safety_churn_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ccol_event_loop l = ccol_event_loop_create_mp(4, 1, 1, NULL, &err);
    if (l != CCOL_EVENT_LOOP_INVALID) {
      int pfd[2];
      if (pipe(pfd) == 0) {
        ccol_event_handlers_t h = {0};
        ccol_event_reg r = ccol_event_loop_add(
            l, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, NULL,
            NULL);
        (void)r;
        close(pfd[0]);
        close(pfd[1]);
      }
      ccol_event_loop_destroy(l);
    }
  }
  return NULL;
}

/* This test guards against a fork-safety hang; the doc comment of
 * _ccol_event_loop_atfork_prepare in src/cthreadcomm.c describes the
 * mechanism. fork() duplicates only the thread that calls it, so without
 * that machinery a child inherits several locks in a locked state (the
 * mutex of ccol_event_loop_slot_table, and the shutdown_lock, the
 * reg_slot_rwlock and the stripes[].lock of any live loop) and no thread is
 * left alive in that child that could ever unlock them.
 *
 * This one bounded test recreates both shapes of the hazard at once. A churn
 * thread keeps creating and destroying throwaway ccol_event_loop instances,
 * which touches the mutex of ccol_event_loop_slot_table, and races the
 * process's repeated fork() calls. The same process ALSO keeps one
 * single-stripe, multi-threaded ccol_event_loop busy with a hot fd, so its
 * poller thread and dispatch threads take and release that loop's one and
 * only stripe lock over and over. Each forked child first tries one more
 * ccol_event_loop_add on the exact loop it just inherited, guarded by
 * alarm(30), which turns a hang on an inherited locked mutex into a visible
 * failure instead of a test that never finishes. A single stripe gives
 * fork() the best chance to land in the middle of a critical section on
 * it. */
TEST(fork_safety, fork_does_not_inherit_a_locked_event_loop_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* The one really UNBOUNDED call here is fork() itself, on the MAIN test
   * thread. In every OTHER hang-prone test of this file alarm(30) bounds the
   * forked child's lifetime and the WNOHANG poll loop of
   * _wait_for_forked_child_bounded() bounds the parent's reap, but nothing
   * bounds the fork() syscall itself or the atfork prepare() and parent()
   * handlers it runs synchronously before it returns, and a lock-ordering
   * hazard can wedge right THERE. This outer alarm catches that one case.
   * It's armed for the whole test body and disarmed just before the normal
   * end, and 30 trials with a worst-case reap bound of 10s each fit easily
   * inside it. */
  alarm(360);
  fork_safety_churn_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  REQUIRE_EQ(pthread_create(&churn_tid, NULL, fork_safety_churn_thread, &churn),
             0);

  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create_mp(16, 1, 3, NULL, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  int hotfd[2];
  REQUIRE_EQ(pipe(hotfd), 0);
  evl_set_nonblocking(hotfd[0]);
  ccol_event_handlers_t h = {.on_readable = fork_safety_hot_fd_on_readable,
                             .on_writable = NULL,
                             .on_error = NULL};
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(hotfd[0], ccol_select_read), h, NULL, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  enum { TRIALS = 30 };
  int hangs = 0;
  for (int i = 0; i < TRIALS; i++) {
    char byte = 'x';
    REQUIRE_EQ(write(hotfd[1], &byte, 1), 1);

    pid_t pid = fork();
    REQUIRE_NE(pid, -1);
    if (pid == 0) {
      /* Bounds this child's lifetime in case the hazard this test guards
       * against fires anyway, which would hang the whole suite otherwise.
       * The parent below tells this apart from a clean exit with
       * WIFEXITED. */
      alarm(30);

      int fd2[2];
      if (pipe(fd2) != 0) _exit(2);
      evl_set_nonblocking(fd2[0]);
      ccol_event_handlers_t h2 = {0};
      ccol_event_reg r2 = ccol_event_loop_add(
          loop, ccol_selectable_from_fd(fd2[0], ccol_select_read), h2, NULL,
          NULL);
      int rc = r2 ? 0 : 1;
      if (r2 != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r2);
      _exit(rc);
    }

    int status = 0;
    /* 10s is comfortably longer than the child's alarm(30). The doc comment
     * of _wait_for_forked_child_bounded explains why the parent needs a bound
     * of its own here too, independent of the child's. */
    bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
    REQUIRE_TRUE(reaped);
    if (!reaped) return;
    if (!WIFEXITED(status)) hangs++;
  }

  REQUIRE_EQ(hangs, 0);

  atomic_store(&churn.stop, 1);
  pthread_join(churn_tid, NULL);

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(hotfd[0]);
  close(hotfd[1]);

  alarm(0);
}

typedef struct {
  ccol_circular_queue *cq;
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} queue_fork_lock_arg_t;

static void *queue_fork_lock_thread(void *arg) {
  queue_fork_lock_arg_t *a = (queue_fork_lock_arg_t *)arg;
  _ccol_circq_test_lock_mutex_for_tests(a->cq);
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule, completely independent
   * of what the forking thread does below. pthread_atfork's prepare()
   * handler MUST block on this exact mutex until this thread releases it
   * (see _queue_atfork_prepare), so the forking thread must never be the one
   * that tells this thread to let go. If it were, the two threads would wait
   * on each other in a real cycle, with fork() blocked in prepare() waiting
   * for this thread to unlock while this thread waits for a signal that the
   * forking thread can only send once fork() returns. That would be a
   * deadlock inside the test itself, unrelated to the behaviour under
   * test. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  /* The store comes before the unlock, so a prepare handler that waits for
   * the unlock can't let fork() return before the store is visible. */
  atomic_store(&a->released, true);
  _ccol_circq_test_unlock_mutex_for_tests(a->cq);
  return NULL;
}

/* A pthread_atfork() handler of its own walks the internal mutex of
 * ccol_circular_queue, ccol_dynamic_queue and ccol_channel, exactly as for
 * every lock that ccol_event_loop owns (see
 * fork_does_not_inherit_a_locked_event_loop_mutex above and the doc comment
 * of _queue_atfork_prepare in src/cthreadcomm.c). Without that handler a
 * child inherits a locked cq->mutex whenever a thread OTHER than the one
 * calling fork() holds it at the exact moment of the fork, and since no
 * thread is left alive in the child to unlock it, every later operation on
 * that queue in the child hangs.
 *
 * The ccol_event_loop regression test above depends on many repeated fork()
 * trials racing a real critical section, which is naturally short, while
 * this test reproduces the hazard deterministically.
 * _ccol_circq_test_lock_mutex_for_tests and
 * _ccol_circq_test_unlock_mutex_for_tests exist only under
 * RUNNING_UNIT_TESTS and let a dedicated holder thread keep cq->mutex locked
 * for a fixed window of HOLD_MS, much longer than any real critical section,
 * after which the holder releases on its own schedule. The forking thread
 * calls fork() only once it has confirmed that the lock is really held.
 * With the atfork prepare() handler in place fork() must then BLOCK until
 * the holder releases, because its own _ccol_mutex_lock(cq->mutex) can't
 * return before that. The holder sets `released` just before its unlock and
 * the parent reads it as soon as fork() returns. A true value proves that
 * the blocking behaviour really kicked in on this run, while without it the
 * run would only show that the race happened not to matter. The check
 * needs no clock, a correct handler makes it true however the
 * threads are scheduled. Without the handler fork() returns while the holder
 * is sleeping through its HOLD_MS and the value is false. Only a forking
 * thread that is descheduled for the whole hold can hide the defect, and
 * that can't make a correct build fail. */
TEST(fork_safety, fork_does_not_inherit_a_locked_circular_queue_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ccol_circular_queue *cq = ccol_circular_queue_create(4, &err);
  REQUIRE_NE((void *)cq, NULL);

  enum { HOLD_MS = 300 };
  queue_fork_lock_arg_t arg = {
      .cq = cq, .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, queue_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* Waits for the holder thread to confirm it took cq->mutex, before the
     * fork below. Natively this is a short, bounded spin because the holder
     * does nothing else before that store, but a bare atomic-load spin with
     * no yield isn't cheap under valgrind: memcheck time-slices every thread
     * through one single instrumented execution engine instead of giving
     * them real parallelism across cores (ctp_fork_feeder_thread in
     * tests/cthreadpool/tests.c behaves the same way). Each iteration is
     * cheap natively, but the iteration count alone would make the `make
     * memtest` run of this test take tens of seconds or even several minutes
     * where it should take a fraction of a second. sched_yield() caps this
     * thread's spin rate at what the scheduler's time-slice granularity
     * allows, so the holder thread gets scheduled promptly. Without the
     * yield this thread starves it by winning the turn of the single
     * instrumented engine over and over. */
    sched_yield();
  }

  /* The child reports its result over a pipe instead of through its exit
   * status, because under make memtest the WEXITSTATUS of a forked child, as
   * the parent's waitpid() sees it, isn't always the value the child passed
   * to _exit(). valgrind's --errors-for-leak-kinds=all and --error-exitcode
   * machinery can override it from whatever it finds "reachable" in the
   * child's inherited process image at exit, which has nothing to do with
   * this test's logic. A debug build shows that the child's rv is really
   * ccol_success on every run while WEXITSTATUS comes back non-zero now and
   * then under valgrind, and the fork_safety section of
   * tests/clogger/tests.c documents the same mechanism for its
   * child_can_log_after_fork test. For the same reason only WIFEXITED is
   * asserted below: unlike WEXITSTATUS it reliably tells a clean exit from a
   * real regression, which is a hang past alarm(30) or a real _ccol_assert()
   * or _ccol_fatal_err() abort that raises SIGABRT. */
  /* The holder is alive and holds cq->mutex until HOLD_MS ends, so every
   * step up to its join records its outcome and the REQUIRE_* checks run
   * only after the join. */
  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);
  pid_t pid = -1;
  bool released_at_fork = false;
  if (pipe_ok) pid = fork();
  if (pipe_ok && pid == 0) {
    /* `holder` doesn't exist here, since fork() duplicates only the thread
     * that calls it. This child process can only come into existence once
     * the parent's fork() call returns, which by the atfork contract needs
     * the _ccol_mutex_lock(cq->mutex) of _queue_atfork_prepare to have
     * succeeded, so the holder thread, gone in this process, must already
     * have released it, so cq->mutex is unlocked here and this call
     * returns at once. Without this handling no atfork handler ever touches
     * cq->mutex, fork() returns almost at once whatever the live holder
     * thread on the parent side is doing, and this child gets a snapshot of
     * a mutex that was really still locked, so this exact call hangs until
     * alarm(30) kills the child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = ccol_circq_try_send_zc(cq, &msg);
    char byte = (rv == ccol_success) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  if (pipe_ok) {
    /* This proves that the blocking behaviour of the atfork prepare handler
     * really kicked in: fork() can't return before the holder released
     * cq->mutex, and the holder sets `released` before that unlock. */
    released_at_fork = atomic_load(&arg.released);
    close(result_pipe[1]);
    if (pid == -1) close(result_pipe[0]);
  }

  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(released_at_fork);

  /* A bounded read instead of a bare blocking read(). The doc comment of
   * _read_result_byte_bounded explains why a plain read() here can't be
   * trusted to return, even for a child that was killed or that hangs. 10s
   * matches this test's parent-side _wait_for_forked_child_bounded call
   * below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s is comfortably longer than the child's alarm(30). The doc comment of
   * _wait_for_forked_child_bounded explains why the parent needs a bound of
   * its own here too, independent of the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  ccol_circular_queue_destroy(cq);
}

typedef struct {
  ccol_event_loop loop;
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} reg_slot_fork_lock_arg_t;

static void *reg_slot_fork_lock_thread(void *arg) {
  reg_slot_fork_lock_arg_t *a = (reg_slot_fork_lock_arg_t *)arg;
  /* The matching unlock call below has to get the opaque pointer this call
   * returns and not a->loop itself (see the doc comment of
   * _ccol_event_loop_test_wrlock_reg_slot_for_tests), because resolving
   * a->loop a second time from this thread at unlock time is a real deadlock
   * hazard against a concurrent fork(). */
  void *resolved = _ccol_event_loop_test_wrlock_reg_slot_for_tests(a->loop);
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule, completely independent
   * of what the forking thread does below. The same comment of
   * queue_fork_lock_thread explains why, and the reasoning applies word for
   * word, here to the Phase 1 reg_slot_rwlock write-lock of
   * _cthreadcomm_atfork_prepare instead of the cq->mutex lock of
   * _queue_atfork_prepare. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  /* See queue_fork_lock_thread for why the store comes before the unlock. */
  atomic_store(&a->released, true);
  _ccol_event_loop_test_wrunlock_reg_slot_for_tests(resolved);
  return NULL;
}

/* A regression test for the TID-tracked write-lock hazard of
 * reg_slot_rwlock, which the comment of _cthreadcomm_atfork_release_impl on
 * its in_child branch for reg_slot_rwlock describes. reg_slot_rwlock is a
 * _ccol_rw_lock_t, so concurrent _ccol_event_reg_resolve calls, the hottest
 * path of ccol_event_loop under a caller that pauses and resumes for each
 * request such as chttpserver, don't serialize behind one lock. Any thread
 * that calls ccol_event_loop_add or ccol_event_loop_remove can take the
 * write side, and that's not always the thread that later calls fork().
 * glibc's rwlock write-lock tracks ownership by TID, so a plain
 * _ccol_rw_lock_unlock from the child's surviving thread, which has a
 * different TID, silently fails to release a lock that a different thread,
 * gone by now, really locked, and every later resolve in that child hangs.
 * This test has exactly the structure of
 * fork_does_not_inherit_a_locked_circular_queue_mutex, with the
 * reg_slot_rwlock of ccol_event_loop in place of the cq->mutex of
 * ccol_circular_queue. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_reg_slot_rwlock) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create_mp(4, 1, 1, NULL, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  ccol_event_handlers_t h = {.on_readable = fork_safety_hot_fd_on_readable,
                             .on_writable = NULL,
                             .on_error = NULL};
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, NULL, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  enum { HOLD_MS = 300 };
  reg_slot_fork_lock_arg_t arg = {
      .loop = loop, .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, reg_slot_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* The same spin-wait comment of
     * fork_does_not_inherit_a_locked_circular_queue_mutex explains why
     * sched_yield() is needed under valgrind and a bare spin isn't enough. */
    sched_yield();
  }

  /* See fork_does_not_inherit_a_locked_circular_queue_mutex: every step up
   * to the holder's join records its outcome, and the REQUIRE_* checks run
   * only after that join. */
  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);
  pid_t pid = -1;
  bool released_at_fork = false;
  if (pipe_ok) pid = fork();
  if (pipe_ok && pid == 0) {
    /* `holder` doesn't exist here, since fork() duplicates only the thread
     * that calls it. This child can only come into existence once the
     * parent's fork() call returns, which needs the
     * _ccol_rw_lock_wrlock(loop->reg_slot_rwlock) of
     * _cthreadcomm_atfork_prepare to have succeeded, so the holder thread,
     * gone in this process, must already have released it. Without this
     * handling the child does a plain _ccol_rw_lock_unlock instead of a
     * reinit, this process inherits reg_slot_rwlock write-locked with no
     * thread that could ever release it, and the resolve inside
     * ccol_event_loop_reg_generation below hangs until alarm(30) kills this
     * child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    uint64_t gen = ccol_event_loop_reg_generation(loop, reg);
    char byte = (gen != 0) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  if (pipe_ok) {
    /* This proves that the blocking behaviour of the atfork prepare handler
     * really kicked in: fork() can't return before the holder released the
     * write lock, and the holder sets `released` before that unlock. The
     * comment of fork_does_not_inherit_a_locked_circular_queue_mutex
     * explains why this check fails without the handler. */
    released_at_fork = atomic_load(&arg.released);
    close(result_pipe[1]);
    if (pid == -1) close(result_pipe[0]);
  }

  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(released_at_fork);

  /* A bounded read instead of a bare blocking read(). The doc comment of
   * _read_result_byte_bounded explains why a plain read() here can't be
   * trusted to return, even for a child that was killed or that hangs. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s is comfortably longer than the child's alarm(30). The doc comment of
   * _wait_for_forked_child_bounded explains why the parent needs a bound of
   * its own here too, independent of the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  /* The parent's reg_slot_rwlock must be really usable after all of the
   * above. The parent releases it with a plain _ccol_rw_lock_unlock, while
   * the child does a reinit, and the fork() call of this same thread validly
   * released that lock, so a plain unlock on it is exactly what should
   * work. */
  uint64_t gen_after = ccol_event_loop_reg_generation(loop, reg);
  REQUIRE_NE(gen_after, (uint64_t)0);

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct {
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} ccol_event_loop_slot_table_fork_lock_arg_t;

static void *ccol_event_loop_slot_table_fork_lock_thread(void *arg) {
  ccol_event_loop_slot_table_fork_lock_arg_t *a =
      (ccol_event_loop_slot_table_fork_lock_arg_t *)arg;
  _ccol_event_loop_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule, completely independent
   * of what the forking thread does below; the same reasoning of
   * queue_fork_lock_thread elsewhere in this file explains why. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  /* See queue_fork_lock_thread for why the store comes before the unlock. */
  atomic_store(&a->released, true);
  _ccol_event_loop_test_wrunlock_slot_table_for_tests();
  return NULL;
}

/* A regression test for the TID-tracked write-lock hazard of
 * ccol_event_loop_slot_table.rwlock, which the comment of
 * _cthreadcomm_atfork_release_impl on its in_child branch for that lock
 * describes. This lock guards the process-wide LOOP table, unlike the
 * reg_slot_rwlock of a single loop that
 * fork_does_not_inherit_a_write_locked_reg_slot_rwlock above covers. Every
 * ccol_event_loop_create and every ccol_event_loop_destroy call takes it,
 * and its read side is the very first thing that ccol_event_loop_pause,
 * ccol_event_loop_resume, ccol_event_loop_modify, ccol_event_loop_remove
 * and ccol_event_loop_reg_generation do. Any thread that calls
 * ccol_event_loop_create or ccol_event_loop_destroy can take the write side,
 * and that's not always the thread that later calls fork(). glibc's rwlock
 * write-lock tracks ownership by TID, so a plain _ccol_rw_lock_unlock from
 * the child's surviving thread, which has a different TID, silently fails
 * to release a lock that a different thread, gone by now, really locked.
 * Every later _ccol_event_loop_resolve in that child then hangs, which
 * covers every ccol_event_loop_create, ccol_event_loop_destroy,
 * ccol_event_loop_add, ccol_event_loop_remove, ccol_event_loop_pause,
 * ccol_event_loop_resume and ccol_event_loop_modify call. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_event_loop_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  enum { HOLD_MS = 300 };
  ccol_event_loop_slot_table_fork_lock_arg_t arg = {
      .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL,
                            ccol_event_loop_slot_table_fork_lock_thread, &arg),
             0);

  while (!atomic_load(&arg.locked)) {
    /* The same spin-wait reasoning of
     * fork_does_not_inherit_a_locked_circular_queue_mutex explains why
     * sched_yield() is needed under valgrind and a bare spin isn't enough. */
    sched_yield();
  }

  /* Every step from here on can fail while holder is already alive and
   * holds ccol_event_loop_slot_table.rwlock, so NONE of them may carry a
   * direct assertion. A REQUIRE_* failure returns from this test function at
   * once, and holder's lifetime doesn't depend on pipe(), fork(), the
   * release check or the child's response succeeding, so a direct assertion
   * on pipe(), on fork() or on the release check would leak holder un-joined
   * on each of those failure paths, and the same goes for the LATER
   * assertions below the read and reap steps. So the test captures every
   * outcome into a local below, every time, and the matching REQUIRE_* runs
   * only after pthread_join(holder, NULL) and, when the test really forked a
   * child, only after _wait_for_forked_child_bounded as well, for the same
   * reason (see the comment of that call). This is the one test in this file
   * where skipping _dump_stuck_child_diagnostics on a real hang would matter
   * most. */
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
      /* `holder` doesn't exist here, since fork() duplicates only the
       * thread that calls it. This child can only come into existence once
       * the parent's fork() call returns, which needs the
       * _ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock) of
       * _cthreadcomm_atfork_prepare to have succeeded, so the holder thread,
       * gone in this process, must already have released it. Without this
       * handling the child does a plain _ccol_rw_lock_unlock instead of a
       * reinit, this process inherits ccol_event_loop_slot_table.rwlock
       * write-locked with no thread that could ever release it, and the
       * resolve inside ccol_event_loop_create below hangs until alarm(30)
       * kills this child. */
      close(result_pipe[0]);
      alarm(30);
      char *err = NULL;
      ccol_event_loop l = ccol_event_loop_create(4, 1, 1, &err);
      char b = (l != CCOL_EVENT_LOOP_INVALID) ? 1 : 0;
      if (l != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(l);
      test_write_retry_eintr(result_pipe[1], &b, 1);
      close(result_pipe[1]);
      _exit(0);
    }

    if (pid > 0) {
      /* fork() can't return before the holder released the write lock, and
       * the holder sets `released` before that unlock. */
      released_at_fork = atomic_load(&arg.released);
      close(result_pipe[1]);

      /* A bounded read instead of a bare blocking read(). The doc comment of
       * _read_result_byte_bounded explains why a plain read() here can't be
       * trusted to return, even for a child that was killed or that hangs,
       * and this is exactly the test where an unbounded read can hang a
       * whole CI job: under qemu-arm both processes can go silent right
       * after the child-side atfork release of this fork() completes, and
       * neither of them is ever heard from again. */
      n = _read_result_byte_bounded(result_pipe[0], &byte, 40000);
      close(result_pipe[0]);

      /* A bounded wait instead of a bare blocking waitpid(), like every
       * sibling fork_safety test. The doc comment of
       * _wait_for_forked_child_bounded explains why this file never waits
       * on the parent side without a bound, although the child's alarm(30)
       * above already bounds the ordinary case. */
      reaped = _wait_for_forked_child_bounded(pid, &status, 40000);
    } else {
      /* fork() itself failed, so there's no child to reap and only the two
       * pipe fds of this process need a close. */
      close(result_pipe[0]);
      close(result_pipe[1]);
    }
  }

  /* This join runs every time, after all the steps above and whichever of
   * them failed, because holder's lifetime never depended on pipe(), fork()
   * or the child's response succeeding. */
  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  /* This proves that the blocking behaviour of the atfork prepare handler
   * really kicked in. The comment of
   * fork_does_not_inherit_a_locked_circular_queue_mutex explains why this
   * check fails without the handler. */
  REQUIRE_TRUE(released_at_fork);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  /* The parent's ccol_event_loop_slot_table.rwlock must be really usable
   * after all of the above. The parent releases it with a plain
   * _ccol_rw_lock_unlock, while the child does a reinit, and the fork() call
   * of this same thread validly released that lock, so a plain unlock on it
   * is exactly what needs to work. */
  char *err2 = NULL;
  ccol_event_loop l2 = ccol_event_loop_create(4, 1, 1, &err2);
  REQUIRE_NE(l2, CCOL_EVENT_LOOP_INVALID);
  ccol_event_loop_destroy(l2);
}

static void fork_safety2_noop_readable(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

typedef struct {
  ccol_circular_queue *cq;
  _Atomic bool started;
} fork_safety2_sender_arg_t;

static void *fork_safety2_sender_thread(void *arg) {
  fork_safety2_sender_arg_t *a = (fork_safety2_sender_arg_t *)arg;
  atomic_store(&a->started, true);
  c_message_t msg = {.data = NULL, .size = 0};
  ccol_circq_send_zc(a->cq, &msg);
  return NULL;
}

/* This test guards against an AB-BA deadlock that a SEPARATE, independent
 * _ccol_at_fork() call for the queue-mutex atfork handling would make
 * possible. pthread_atfork runs prepare handlers in REVERSE registration
 * order, so which of two independent handler sets runs first at fork() time
 * is an accident that depends only on which subsystem the process happens to
 * use first, a ccol_circular_queue or a ccol_event_loop.
 * Take a queue created before the first ccol_event_loop. The queue's atfork
 * triple registers first, so the prepare handler of ccol_event_loop, which
 * registers second, runs FIRST at fork() time and locks the wait_mtx of a
 * queue-backed registration. A separate queue-registry prepare handler then
 * tries to lock the cq->mutex of that SAME queue, the reverse of the order
 * _notify_waiter always uses, and any ordinary ccol_circq_send_zc or
 * ccol_circq_recv_zc call running at the same time on an unrelated thread
 * reaches _notify_waiter. The forking thread then deadlocks inside fork()
 * itself: it blocks on cq->mutex from that second, independent atfork
 * prepare handler while a concurrent sender thread blocks on wait_mtx inside
 * _notify_waiter. That's a textbook cycle and gdb shows exactly those two
 * stacks. So the locking of both subsystems is merged into ONE
 * _ccol_at_fork() registration (_cthreadcomm_atfork_prepare,
 * _cthreadcomm_atfork_release and _cthreadcomm_atfork_child_release; see the
 * three-phase design comment of that function in src/cthreadcomm.c), and
 * nothing depends on which of two independent handler sets happens to run
 * first.
 *
 * The test forces exactly that dangerous construction order by creating a
 * ccol_circular_queue before the first ccol_event_loop in a fresh process.
 * It then uses _notify_waiter_test_set_delay_us, which exists only under
 * RUNNING_UNIT_TESTS, to widen a real window inside _notify_waiter between
 * "cq->mutex already held" and "about to lock wait_mtx". That window is
 * otherwise only a few instructions long, and widened it lasts long enough
 * for a concurrent fork() to land inside it reliably instead of by timing
 * luck.
 *
 * The whole scenario runs in its own forked child, bounded by an alarm,
 * which is the pattern the tests of this file that trigger
 * _ccol_fatal_err() use to isolate a risky operation and bound it from the
 * parent with an alarm and a waitpid. Unlike every OTHER fork_safety test in
 * this file, a regression here deadlocks the FORKING THREAD ITSELF, inside
 * the prepare() handler of fork(), before fork() ever returns to userspace,
 * rather than a spawned child that a plain alarm(3) inside it could bound.
 * That's why the whole scenario lives in its own child process with ITS OWN
 * alarm(180), so a regression never hangs the outer test suite.
 *
 * The test reports success over a pipe rather than through the child's exit
 * status, for the same reason the doc comment of
 * fork_does_not_inherit_a_locked_circular_queue_mutex above gives: the
 * WEXITSTATUS of a forked child, as waitpid() sees it, isn't reliably what
 * the child passed to _exit(), because the --errors-for-leak-kinds=all and
 * --error-exitcode machinery of make memtest overrides it. Only WIFEXITED
 * means anything, and we use even that only to reap the process. A child
 * that hangs exits through SIGALRM from its own alarm(180), which gives
 * WIFSIGNALED, and the pipe read is what tells a success from a regression.
 * The same alarm bounds the read, because the parent has already closed its
 * own copy of the write end. */
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
    /* This bound is wide on purpose, which is the project's usual timing
     * margin for valgrind (the async tests of chttpclient do the same). It
     * needs to absorb the real overhead valgrind adds to each fork() in a
     * fully loaded test run, which carries the accumulated heap and
     * shadow-memory state of the 168 tests before this one, and that
     * overhead alone can get close to several real seconds now and then
     * without any bug at all. A regression this test exists to catch hangs
     * for ever anyway, so a wider bound only costs a slower report of a
     * real regression. */
    alarm(180);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }

    char byte = 0; /* 0 means a failure or a regression, until the code below
                      proves otherwise. */

    /* This forces the dangerous construction order. It's the first use of a
     * ccol_circular_queue or a ccol_event_loop in this freshly forked
     * process, so creating a queue before the loop makes the lazy init of
     * the queue-mutex registry happen first, every time. */
    char *err = NULL;
    ccol_circular_queue *cq = ccol_circular_queue_create(4, &err);
    ccol_event_loop loop =
        cq ? ccol_event_loop_create(16, 1, 1, &err) : CCOL_EVENT_LOOP_INVALID;
    ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
    if (cq && loop) {
      ccol_event_handlers_t h = {.on_readable = fork_safety2_noop_readable};
      reg = ccol_event_loop_add(
          loop, ccol_selectable_from_circq(cq, ccol_select_read), h, NULL,
          &err);
    }

    if (cq && loop && reg) {
      _notify_waiter_test_set_delay_us(300000);

      fork_safety2_sender_arg_t sarg = {.cq = cq, .started = false};
      pthread_t sender;
      /* We check this create instead of firing and forgetting it, because
       * it runs inside the forked child above (outer_pid == 0), where a
       * failed create can't be reported with REQUIRE_EQ. Every sibling
       * fork_safety test with the same spin-wait setup can do that, since its
       * own pthread_create runs on the PARENT before any fork. The failure
       * path of REQUIRE_EQ is an early `return` out of this Tau test
       * function, which there lands safely back in the harness that called
       * it. Here the same early `return` leaves THIS test function while the
       * process is still the forked child, so it skips the _exit(0) below
       * and falls into the harness's test-running loop a second time,
       * although this process was only ever meant to run this one
       * child-side branch and exit. So a failed create is handled exactly
       * like a failed create of cq, loop or reg just above: the risky
       * section is skipped completely and we fall through with byte still
       * 0, and the pipe protocol then reports the run as a failure instead
       * of entering the wait loop below with no thread that could ever
       * satisfy it. A deliberately tightened ulimit on processes or threads
       * reproduces this on demand. Left unchecked, a failure here leaves
       * sarg.started false for ever and the sched_yield() loop below spins
       * for ever, a silent endless hang instead of a clean, fast failure. */
      if (pthread_create(&sender, NULL, fork_safety2_sender_thread, &sarg) ==
          0) {
        while (!atomic_load(&sarg.started)) {
          /* Waits for the sender to confirm it's about to call
           * ccol_circq_send_zc, before the fork below. A bare atomic-load
           * spin with no yield isn't cheap under valgrind, because memcheck
           * time-slices every thread through one single instrumented
           * execution engine instead of giving them real parallelism across
           * cores; fork_does_not_inherit_a_locked_circular_queue_mutex above
           * and ctp_fork_feeder_thread in tests/cthreadpool/tests.c say the
           * same. Each iteration is cheap natively, but the iteration count
           * alone would make the `make memtest` run of this test take well
           * over a minute, sometimes even past the test's alarm, instead of a
           * fraction of a second. sched_yield() caps this thread's spin rate
           * at what the scheduler's time-slice granularity allows, so the
           * sender thread gets scheduled promptly. Without the yield this
           * thread starves it by winning the turn of the single instrumented
           * engine over and over. */
          sched_yield();
        }
        /* Gives the sender a moment to take cq->mutex and enter the widened
         * _notify_waiter delay before the risky fork() call below. */
        usleep(50000);

        pid_t inner_pid = fork();
        if (inner_pid == 0) {
          _exit(0);
        } else if (inner_pid > 0) {
          /* What we test is that control reaches this point at all, since a
           * regression hangs until the alarm(180) above kills this process.
           * The wait is bounded instead of being a bare blocking waitpid().
           * _wait_for_forked_child_bounded holds no Tau macro, so it's safe
           * to call from this already-forked child too, and that keeps this
           * call consistent with every other forked-child wait in this file
           * instead of being the one bare exception. */
          int inner_status = 0;
          (void)_wait_for_forked_child_bounded(inner_pid, &inner_status, 10000);
          byte = 1;
        }

        _notify_waiter_test_set_delay_us(0);
        pthread_join(sender, NULL);
      }
    }

    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);

    if (reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, reg);
    if (cq) {
      c_message_t drain = {0};
      ccol_circq_try_recv_zc(cq, &drain);
      ccol_circular_queue_destroy(cq);
    }
    if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
    _exit(0);
  }

  close(result_pipe[1]);
  /* A bounded read instead of a bare blocking read(). The doc comment of
   * _read_result_byte_bounded explains why a plain read() here can't be
   * trusted to return, even for a child that was killed or that hangs, and
   * the same comment of
   * fork_does_not_inherit_a_write_locked_event_loop_slot_table above says the
   * same. 200s matches this test's outer_pid bound below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 200000);
  close(result_pipe[0]);

  int status = 0;
  /* A bounded wait instead of a bare blocking waitpid(). The same comment of
   * fork_does_not_inherit_a_write_locked_event_loop_slot_table above says
   * why, although the child's alarm(180) already bounds the ordinary
   * case. */
  bool reaped = _wait_for_forked_child_bounded(outer_pid, &status, 200000);
  REQUIRE_TRUE(reaped);

  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
}

/* This test covers ccol_event_loop_shutdown and ccol_event_loop_destroy on a
 * ccol_event_loop that a child INHERITS across fork(), while every other
 * fork test in this file either only calls ccol_event_loop_add and
 * ccol_event_loop_remove on the inherited loop or destroys a loop the child
 * created fresh. It's exactly the scenario the foreign_since_fork field of
 * struct _ccol_event_loop_s exists for. fork() duplicates only the thread
 * that calls it, so in a forked child the poller_thread field names a
 * pthread_t that this process never created and can never join, and
 * loop->shutdown_efd is a real kernel object that the child shares with the
 * parent instead of copying, with a really live poller thread on the parent
 * side. Without the foreign_since_fork fixup, ccol_event_loop_destroy() in a
 * forked child SIGSEGVs for an inherited loop with num_reactor_threads > 1
 * on almost every run, inside glibc's __pthread_clockjoin_ex, reached
 * through the worker-thread join loop of ctpool_shutdown_drain. The test
 * guards against a future regression that moves where the
 * foreign_since_fork check sits relative to the self-call guard, or changes
 * the matching fixup in cthreadpool.c that this mechanism depends on.
 *
 * It also checks the other half: destroying the loop in the child must not
 * disturb the parent's live loop at all. In particular it must never write
 * loop->shutdown_efd, a kernel object the two processes share across fork(),
 * because that write would wrongly wake the parent's poller thread. We check
 * this by confirming that the parent's identical, still registered fd
 * selectable keeps dispatching normally after the child has torn down its
 * own copy completely. */
TEST(fork_safety, ccol_event_loop_destroy_of_inherited_loop_in_child_is_safe) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create_mp(8, 2, 3, NULL, &err);
  REQUIRE_NE(loop, CCOL_EVENT_LOOP_INVALID);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t h = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, &ctx, &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

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
    /* Bounds this child's lifetime in case the hazard this test guards
     * against somehow fires as a hang instead of a crash, which would hang
     * the whole suite otherwise. The parent below tells this apart from a
     * clean exit with WIFEXITED, as the fork-safety tests of this file
     * do. */
    alarm(5);

    /* The misuse under test: it destroys the exact, fully inherited loop
     * handle, whose dispatch_pool workers and poller thread exist in this
     * process only as inert, copy-on-write memory. */
    ccol_event_loop_destroy(loop);

    char byte = 1;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }

  close(result_pipe[1]);
  /* A bounded read instead of a bare blocking read(). The doc comment of
   * _read_result_byte_bounded explains why a plain read() here can't be
   * trusted to return, even for a child that was killed or that hangs. 10s
   * matches the bound below, which is this test's alarm(5) plus a
   * margin. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);

  int status = 0;
  /* 10s is comfortably longer than the child's alarm(5). The doc comment of
   * _wait_for_forked_child_bounded explains why the parent needs a bound of
   * its own here too, independent of the child's. Unlike the simpler
   * fork-safety tests of this file there's no early return on a timeout
   * here, because the parent's loop, pfd and ctx below need a teardown
   * whether the child was reaped or not. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);

  /* The real assertion is that the child reaches a clean exit and really
   * wrote the byte. A SIGSEGV, the regression this test guards against,
   * makes both fail: the status says WIFSIGNALED, and the read() above
   * returns 0 because the pipe's only writer dies without writing. We check
   * only WIFEXITED on the status and not WEXITSTATUS, as elsewhere in this
   * file, because the WEXITSTATUS of a forked child, as the parent's
   * waitpid() sees it, isn't reliably what the child passed to _exit(): the
   * --errors-for-leak-kinds=all and --error-exitcode machinery of make
   * memtest overrides it. See the comment of
   * fork_does_not_inherit_a_locked_circular_queue_mutex. */
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  /* The parent's own loop must be fully alive and dispatch normally. The
   * destroy the child ran on its inherited copy must not have written the
   * shared shutdown_efd, which would have woken the parent's live poller,
   * and must not have disturbed the parent's kernel-level epoll instance in
   * any other way. */
  char val = 'x';
  REQUIRE_EQ(write(pfd[1], &val, 1), 1);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*        QUEUE-BACKED REGISTRATION LIFETIME AGAINST THE QUEUE ITSELF         */
/* ========================================================================== */

/* Counts the file descriptors this process has open; see test_fds.h. */
static size_t evl_count_open_fds(void) { return (size_t)test_count_open_fds(); }

/* Sleeps for ms milliseconds. It only paces bounded polling loops and is
 * never the mechanism a property under test depends on. */
static void evl_sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

TEST(ccol_event_loop, queue_cascade_finishes_before_remove_returns) {
  /* The cascade step that runs after a dispatch for a queue-backed
   * registration locks the mutex of the queue below it, which the
   * application owns and which has no refcount of its own. The module
   * documents one supported way to retire such a registration: call
   * ccol_event_loop_remove and then destroy the queue immediately (see
   * ccol_circular_queue_destroy). So the cascade must never be on its way
   * to that mutex any more once remove returns, or it locks freed memory.
   *
   * We assert the ordering directly instead of hoping a sanitizer sees the
   * freed access. The cascade publishes a begun count and a finished count,
   * and the property is that remove can't return between the two. The armed
   * delay below sits exactly in the window a removal would have to be held
   * off across, and since it only makes that window longer it can't make a
   * correct implementation fail, however long it is.
   *
   * Move the cascade's readiness check out from under the stripe lock and
   * remove returns with finished at the value it had before the dispatch,
   * which fails the test. */
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {.on_readable = evl_on_readable,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = NULL};
  char *err = NULL;
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  if (queue_created)
    reg = ccol_event_loop_add(loop,
                              ccol_selectable_from_circq(cq, ccol_select_read),
                              handlers, &ctx, &err);
  bool registered = (reg != CCOL_EVENT_REG_INVALID);

  uint64_t begun_before = 0, finished_before = 0;
  _ccol_event_loop_test_queue_cascade_counts(&begun_before, &finished_before);

  bool sent = false, dispatched = false, cascade_begun = false;
  if (registered) {
    _ccol_event_loop_test_delay_next_queue_cascade_us(300000);
    char *payload = (char *)malloc(4);
    if (payload) {
      memcpy(payload, "abc", 4);
      c_message_t msg = {.data = payload, .size = 4};
      sent = (ccol_circq_send_zc(cq, &msg) == ccol_success);
      if (!sent) free(payload);
    }
    dispatched = sent && evl_wait_for(&ctx, &ctx.readable_count, 1, 5000);

    /* A bounded wait. Once begun advances the cascade holds the stripe lock
     * the removal below needs, so this is a real synchronisation point and
     * not a guess about elapsed time. */
    for (int i = 0; i < 5000 && !cascade_begun; i++) {
      uint64_t begun = 0;
      _ccol_event_loop_test_queue_cascade_counts(&begun, NULL);
      if (begun > begun_before) {
        cascade_begun = true;
        break;
      }
      evl_sleep_ms(1);
    }
  }

  ccol_retval_t remove_rv = ccol_invalid_args;
  uint64_t finished_at_return = finished_before;
  if (registered) {
    remove_rv = ccol_event_loop_remove(loop, reg);
    _ccol_event_loop_test_queue_cascade_counts(NULL, &finished_at_return);
  }

  /* The documented retirement pattern, exercised for real: we wait for
   * nothing else between the removal and this destroy. */
  if (queue_created) ccol_circular_queue_destroy(cq);

  /* Unconditional and before any assertion, so an unconsumed delay can
   * never leak into a later test. */
  _ccol_event_loop_test_delay_next_queue_cascade_us(0);
  evl_sync_ctx_destroy(&ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(dispatched);
  REQUIRE_TRUE(cascade_begun);
  REQUIRE_EQ(remove_rv, ccol_success);
  REQUIRE_GT(finished_at_return, finished_before);
}

/* A bounded wait until loop's poller thread stops completing epoll_wait
 * calls, which is how "parked for ever with nothing left to reclaim" looks
 * from outside, since while anything is deferred the poller re-arms a
 * bounded retry and this count keeps moving. The count has to be non-zero
 * already, so a poller that simply hasn't started yet is never mistaken for
 * a parked one. */
static bool evl_wait_until_poller_parked(ccol_event_loop loop, int timeout_ms) {
  uint64_t prev = _ccol_event_loop_poller_iterations_for_tests(loop);
  for (int waited = 0; waited < timeout_ms; waited += 150) {
    evl_sleep_ms(150);
    uint64_t now = _ccol_event_loop_poller_iterations_for_tests(loop);
    if (now > 0 && now == prev) return true;
    prev = now;
  }
  return false;
}

TEST(ccol_event_loop,
     idle_loop_reclaims_removed_registrations_without_handlers) {
  /* The library closes the bridge eventfd of a removed registration only
   * when it finally reclaims that deferred registration, and only the poller
   * thread ever reclaims one. Neither epoll_ctl(EPOLL_CTL_DEL) nor
   * epoll_ctl(EPOLL_CTL_ADD) of a descriptor that isn't ready at that moment
   * wakes a blocked epoll_wait, so on a loop with no other traffic the
   * removal itself has to wake the poller. Without that, a repeated add and
   * remove on an idle loop leaks one descriptor per cycle, with no bound.
   *
   * on_removed is NULL on purpose for every registration the count below
   * covers, since almost every registration in this codebase looks like
   * that.
   *
   * The test first drives the poller into a really parked state and only
   * then takes the baseline. Without that step a burst of add and remove
   * calls can run before the poller ever blocks, the poller's very first
   * reclaim pass sweeps the burst up, and the check passes whatever the
   * removal does.
   *
   * If the removal woke the poller only for registrations that carry an
   * on_removed handler, the descriptor count would stay CYCLES above its
   * baseline and the test would fail. */
  enum { CYCLES = 8 };
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  /* One real dispatch first, only to prove that the poller thread runs and
   * has completed at least one epoll_wait. */
  ccol_event_handlers_t warm_handlers = {.on_readable = evl_on_readable,
                                         .on_writable = NULL,
                                         .on_error = NULL,
                                         .on_removed = NULL};
  char *err = NULL;
  ccol_event_reg warm = CCOL_EVENT_REG_INVALID;
  if (queue_created)
    warm = ccol_event_loop_add(loop,
                               ccol_selectable_from_circq(cq, ccol_select_read),
                               warm_handlers, &ctx, &err);
  bool warmed = false, warm_removed = false;
  if (warm != CCOL_EVENT_REG_INVALID) {
    char *payload = (char *)malloc(4);
    if (payload) {
      memcpy(payload, "abc", 4);
      c_message_t msg = {.data = payload, .size = 4};
      if (ccol_circq_send_zc(cq, &msg) == ccol_success)
        warmed = evl_wait_for(&ctx, &ctx.readable_count, 1, 5000);
      else
        free(payload);
    }
    warm_removed = (ccol_event_loop_remove(loop, warm) == ccol_success);
  }

  bool parked =
      warmed && warm_removed && evl_wait_until_poller_parked(loop, 5000);

  /* We take this baseline with the poller really blocked, so every
   * descriptor the cycles below open is one that only their own removals can
   * close again. */
  size_t baseline = evl_count_open_fds();

  ccol_event_handlers_t handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = NULL};
  bool all_cycles_ok = parked;
  for (int i = 0; i < CYCLES && all_cycles_ok; i++) {
    char *cycle_err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, NULL,
        &cycle_err);
    if (reg == CCOL_EVENT_REG_INVALID) {
      all_cycles_ok = false;
      break;
    }
    if (ccol_event_loop_remove(loop, reg) != ccol_success)
      all_cycles_ok = false;
  }

  /* A bounded loop. A correct reclaim happens within one poller wakeup, so
   * this loop normally exits on its first or second sample. */
  bool reclaimed = false;
  size_t observed = baseline;
  for (int i = 0; i < 3000 && !reclaimed; i++) {
    observed = evl_count_open_fds();
    if (observed <= baseline) {
      reclaimed = true;
      break;
    }
    evl_sleep_ms(1);
  }

  size_t reg_count = ccol_event_loop_reg_count(loop);
  if (queue_created) ccol_circular_queue_destroy(cq);
  evl_sync_ctx_destroy(&ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_NE(warm, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(warmed);
  REQUIRE_TRUE(warm_removed);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(all_cycles_ok);
  REQUIRE_EQ(reg_count, (size_t)0);
  REQUIRE_TRUE(reclaimed);
  REQUIRE_LE(observed, baseline);
}

TEST(ccol_event_loop,
     on_removed_fires_on_destroy_for_a_live_queue_registration) {
  /* The teardown walk for a queue-backed registration that is still
   * registered frees that registration and the event entry it owns. The
   * registration is what names the entry, and whether on_removed fires
   * depends on freeing that registration, so the code needs to read both
   * before it releases either one. This test pins that on_removed reaches a
   * queue-backed registration torn down this way. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_slow_cb_ctx read_ctx, write_ctx;
  evl_slow_cb_ctx_init(&read_ctx);
  evl_slow_cb_ctx_init(&write_ctx);

  bool read_registered = false, write_registered = false;
  if (queue_created) {
    ccol_event_loop_construct_scoped(loop, 8, 4, 1);
    ccol_event_handlers_t handlers = {.on_readable = NULL,
                                      .on_writable = NULL,
                                      .on_error = NULL,
                                      .on_removed = evl_on_removed_slow};
    char *err = NULL;
    read_registered =
        (ccol_event_loop_add(
             loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers,
             &read_ctx, &err) != CCOL_EVENT_REG_INVALID);
    write_registered =
        (ccol_event_loop_add(
             loop, ccol_selectable_from_circq(cq, ccol_select_write), handlers,
             &write_ctx, &err) != CCOL_EVENT_REG_INVALID);
    /* Nothing ever removes these two. The destructor of the scoped loop, at
     * the closing brace of this block, tears both of them down while they
     * are registered. */
  }

  pthread_mutex_lock(&read_ctx.mtx);
  bool read_removed_fired = read_ctx.on_removed_called;
  void *read_arg_seen = read_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&read_ctx.mtx);
  pthread_mutex_lock(&write_ctx.mtx);
  bool write_removed_fired = write_ctx.on_removed_called;
  void *write_arg_seen = write_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&write_ctx.mtx);

  /* This is safe here because the teardown walk unlinked both waiter nodes
   * from the queue. */
  if (queue_created) ccol_circular_queue_destroy(cq);
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

TEST(ccol_event_loop, a_free_list_push_failure_does_not_strand_a_reg_slot) {
  /* The library releases a registration slot in two steps: it clears that
   * slot's entry and then pushes the slot's index back onto the loop's free
   * list. That push can fail to allocate, and then the library counts the
   * index as lost and a later acquire recovers it, so the slot array doesn't
   * grow once per failure for as long as the loop lives.
   *
   * Without the recovery the slot array grows by one on every cycle, and
   * `after` ends up larger than `before` by the number of cycles instead of
   * staying the same. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  const size_t cycles = 8;
  bool all_ok = true;
  size_t before = 0, after = 0;
  bool reusable = false;

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);
    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    /* One add and remove pair first to warm the free list, so `before`
     * counts a table that has already reached its steady size. */
    char *err = NULL;
    ccol_event_reg warm = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    if (warm == CCOL_EVENT_REG_INVALID) all_ok = false;
    if (all_ok && ccol_event_loop_remove(loop, warm) != ccol_success)
      all_ok = false;

    before = _ccol_event_loop_reg_slot_count_for_tests(loop);

    for (size_t i = 0; i < cycles && all_ok; i++) {
      err = NULL;
      ccol_event_reg reg = ccol_event_loop_add(
          loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers,
          &ctx, &err);
      if (reg == CCOL_EVENT_REG_INVALID) {
        all_ok = false;
        break;
      }
      _ccol_event_loop_force_next_reg_free_index_push_failure_for_tests();
      if (ccol_event_loop_remove(loop, reg) != ccol_success) all_ok = false;
    }

    /* This registration comes from a recovered index and it should work,
     * so a recovered slot with stale bookkeeping fails here. */
    err = NULL;
    ccol_event_reg revived = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    reusable = (revived != CCOL_EVENT_REG_INVALID) &&
               (ccol_event_loop_reg_generation(loop, revived) > 0);
    if (reusable && ccol_event_loop_remove(loop, revived) != ccol_success)
      reusable = false;

    after = _ccol_event_loop_reg_slot_count_for_tests(loop);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(reusable);
  /* The recovery reuses the stranded indices, so the table doesn't grow per
   * cycle. One more slot is acceptable, for the registration that's in
   * flight. */
  REQUIRE_LE(after, before + 1);
}

/* ========================================================================== */
/*               EVENT LOOP SLOT TABLE LOST-FREE-INDEX RECOVERY               */
/* ========================================================================== */

/*
 * Destroying a loop pushes its slot index back onto the free list, and that
 * push allocates. If it fails the index is stranded unless the acquire path
 * reclaims it, and without that reclaim a process that creates and destroys
 * loops under memory pressure grows the table once per destroy.
 *
 * Take the reclaim branch out of _ccol_event_loop_handle_slot_acquire and
 * the final capacity goes past the tolerance below by the number of
 * iterations.
 */
TEST(ccol_event_loop_slot_table,
     a_lost_free_index_is_reclaimed_rather_than_growing) {
  {
    ccol_event_loop warm = ccol_event_loop_create(8, 1, 1, NULL);
    if (warm != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(warm);
  }
  size_t before = _ccol_event_loop_slot_table_capacity_for_tests();

  bool all_built = true;
  for (int i = 0; i < 4; i++) {
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop == CCOL_EVENT_LOOP_INVALID) {
      all_built = false;
      continue;
    }
    _ccol_event_loop_force_next_free_index_push_failure_for_tests();
    ccol_event_loop_destroy(loop);
  }

  /* This loop comes from a reclaimed slot and must be usable, since
     a reclaimed index with stale bookkeeping answers a resolve with the
     wrong object instead of failing at acquire time. */
  bool works = false;
  {
    ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
    if (loop != CCOL_EVENT_LOOP_INVALID) {
      works = (ccol_event_loop_reg_count(loop) == (size_t)0);
      ccol_event_loop_destroy(loop);
    }
  }

  size_t after = _ccol_event_loop_slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_built);
  REQUIRE_TRUE(works);
  REQUIRE_LE(after, before + 1);
}

/* ==========================================================================
 * Queue teardown against a dispatch that is already in flight
 * ========================================================================== */

typedef struct qdispatch_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool callback_entered;
  bool callback_done_with_queue;
} qdispatch_ctx;

static void qdispatch_ctx_init(qdispatch_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->callback_entered = false;
  c->callback_done_with_queue = false;
}

static void qdispatch_ctx_destroy(qdispatch_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

/* This callback drains the queue's one message at once, so the queue is
 * really empty by the time the test's destroy runs. Then it holds the queue
 * for a long, precisely controlled window and touches it one last time
 * after that window. That last ccol_circq_try_recv_zc is the read that lands
 * on freed memory if a destroy can complete while this callback is still in
 * flight. */
static void qdispatch_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  qdispatch_ctx *c = (qdispatch_ctx *)arg;

  c_message_t msg = {.data = NULL, .size = 0};
  if (ccol_circq_try_recv_zc(sel->cq, &msg) == ccol_success) free(msg.data);

  pthread_mutex_lock(&c->mtx);
  c->callback_entered = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);

  struct timespec hold = {.tv_sec = 0, .tv_nsec = 400000000L};
  nanosleep(&hold, NULL);

  c_message_t again = {.data = NULL, .size = 0};
  if (ccol_circq_try_recv_zc(sel->cq, &again) == ccol_success) free(again.data);

  pthread_mutex_lock(&c->mtx);
  c->callback_done_with_queue = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

/* This test pins the documented teardown sequence (drain the queue, call
 * ccol_event_loop_remove, then destroy) as really safe, beyond documented.
 * ccol_event_loop_remove unlinks the registration's waiter node
 * and returns without waiting for a callback the reactor already collected,
 * so the queue looks unwatched the moment that call returns while the
 * callback holds the queue through its own snapshot. The destroy has to wait
 * that callback out.
 *
 * The check is structural rather than a race against a clock. The callback
 * sets callback_done_with_queue only after its last touch of the queue, and
 * the test reads that flag on the line after the destroy. A destroy that
 * returns early reads false, because by construction the callback is then
 * inside a 400 ms hold it entered before anyone called the destroy. Without
 * the wait the same run is a use-after-free too, which the callback commits
 * a moment later and which AddressSanitizer and valgrind both report. */
TEST(ccol_event_loop, queue_destroy_waits_for_an_already_collected_dispatch) {
  qdispatch_ctx ctx;
  qdispatch_ctx_init(&ctx);

  ccol_circular_queue *cq = ccol_circular_queue_create_mp(4, NULL, NULL);
  bool created = (cq != NULL);
  bool registered = false;
  bool sent = false;
  bool entered = false;
  bool done_when_destroy_returned = false;

  if (created) {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {.on_readable = qdispatch_on_readable,
                                      .on_writable = NULL,
                                      .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_read), handlers, &ctx,
        &err);
    registered = (reg != CCOL_EVENT_REG_INVALID);

    if (registered) {
      c_message_t m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      sent = (ccol_circq_send_zc(cq, &m) == ccol_success);
      if (!sent) free(m.data);

      entered = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.callback_entered, 5000);
      ccol_event_loop_remove(loop, reg);
    }

    ccol_circular_queue_destroy(cq);

    pthread_mutex_lock(&ctx.mtx);
    done_when_destroy_returned = ctx.callback_done_with_queue;
    pthread_mutex_unlock(&ctx.mtx);

    /* Leaving the block shuts the scoped loop down and joins its reactor
     * thread, so nothing can be running against ctx below. */
  }

  qdispatch_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(done_when_destroy_returned);
}

/* ==========================================================================
 * A ccol_select() waiter that gives up its wait must not swallow the wake
 * ========================================================================== */

typedef struct sel_forward_ctx {
  ccol_circular_queue *cq;
  int timeout_ms;
  ccol_retval_t rv;
  size_t ready_index;
} sel_forward_ctx;

static void *sel_forward_waiter_thread(void *arg) {
  sel_forward_ctx *c = (sel_forward_ctx *)arg;
  ccol_selectable sels[1] = {
      ccol_selectable_from_circq(c->cq, ccol_select_read)};
  c->ready_index = (size_t)-1;
  c->rv = ccol_select_timed(&c->ready_index, 1, sels,
                            (uint64_t)c->timeout_ms * 1000u);
  return NULL;
}

/* Polls pred in a bounded loop and returns false if pred never becomes
 * true. */
static bool sel_forward_poll_until(bool (*pred)(void *), void *arg,
                                   int timeout_ms) {
  for (int waited_ms = 0; waited_ms < timeout_ms; waited_ms++) {
    if (pred(arg)) return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&ts, NULL);
  }
  return pred(arg);
}

typedef struct sel_forward_count_arg {
  ccol_circular_queue *cq;
  size_t target;
} sel_forward_count_arg;

static bool sel_forward_waiters_reached(void *arg) {
  sel_forward_count_arg *a = (sel_forward_count_arg *)arg;
  return _ccol_circq_test_sel_read_waiter_count_for_tests(a->cq) >= a->target;
}

static bool sel_forward_delay_entered(void *arg) {
  (void)arg;
  return _ccol_select_test_timed_out_deregister_delay_count() > 0;
}

/* A producer wakes exactly one ccol_select() waiter per message, and that
 * waiter may already have given up because its deadline elapsed, so the
 * notification reaches a thread that never acts on it. The waiter that
 * leaves has to forward it, otherwise a sibling waiter stays parked with a
 * message sitting in the queue.
 *
 * Two steps make this deterministic. First, the test starts the waiters one
 * at a time and checks that each one is linked before the next starts, so
 * the later one is the list head and therefore the target of the producer's
 * single notify (a fresh rotation starts at the head). Second, the head
 * waiter's deadline elapses first and the window between that and its
 * deregistration is held open through
 * _ccol_select_test_delay_next_timed_out_deregister_us, so the producer
 * makes the message squarely inside that window and not by luck.
 *
 * Without the forward nothing ever wakes the second waiter, and it returns
 * ccol_timed_out on its own much later deadline instead of ccol_success. */
TEST(ccol_select, a_waiter_that_abandons_its_wait_forwards_the_wake) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(4, NULL, NULL);
  bool created = (cq != NULL);
  if (!created) {
    REQUIRE_TRUE(created);
    return;
  }

  /* parked_waiter outlives the whole exchange and the forward must wake it.
   * abandoning_waiter is the one the notify aims at, and it's also the one
   * that gives up before it can act on that notify. */
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
    /* We arm this before the second waiter starts, because that waiter is
     * the only call that can reach its own deadline while it's armed. */
    _ccol_select_test_delay_next_timed_out_deregister_us(1000000u);
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
    c_message_t m = {.data = malloc(4), .size = 4};
    assert(m.data != NULL);
    memcpy(m.data, "abc", 4);
    sent = (ccol_circq_send_zc(cq, &m) == ccol_success);
    if (!sent) free(m.data);
  }

  /* Every thread that started is joined before any assertion runs and the
   * armed delay is cleared every time, so the delay can't leak into a later
   * test. */
  if (abandoning_started) pthread_join(abandoning_tid, NULL);
  if (parked_started) pthread_join(parked_tid, NULL);
  _ccol_select_test_delay_next_timed_out_deregister_us(0u);

  c_message_t drained = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &drained) == ccol_success) {
    free(drained.data);
    drained = (c_message_t){.data = NULL, .size = 0};
  }
  ccol_circular_queue_destroy(cq);

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

/* ==========================================================================
 * Registrations whose only handler is on_error
 * ========================================================================== */

/* An error-only read registration has on_readable NULL and on_error set,
 * which is how a caller says "tell me when this fd dies, I don't want to
 * read it". A peer that closes only its write side raises EPOLLIN and
 * EPOLLRDHUP without EPOLLHUP, since EPOLLHUP needs both directions down, so
 * a condition the loop dispatches as readable or not at all never reaches
 * such a registration.
 *
 * The library must deliver a read-side condition that nothing can consume
 * as an error; if it didn't, on_error would never fire here and the wait
 * below would time out. Every epoll_wait also reports the same event again
 * for as long as nothing dispatches it, which the write-direction test below
 * measures directly. */
TEST(ccol_event_loop,
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
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);
    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), handlers, &ctx,
        &err);
    registered = (reg != CCOL_EVENT_REG_INVALID);
    if (registered) {
      shutdown(sv[1], SHUT_WR);
      fired = evl_wait_for(&ctx, &ctx.error_count, 1, 3000);
      ccol_event_loop_remove(loop, reg);
    }
    /* Leaving the block joins the reactor thread, so ctx is quiet below. */
  }

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(fired);
}

/* _ccol_event_loop_poller_collect has its own copy of the dispatch
 * classification, and this is the same case run against that copy. */
TEST(ccol_event_loop,
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
    ccol_event_loop_construct_scoped(loop, 8, 4, 3);
    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), handlers, &ctx,
        &err);
    registered = (reg != CCOL_EVENT_REG_INVALID);
    if (registered) {
      shutdown(sv[1], SHUT_WR);
      fired = evl_wait_for(&ctx, &ctx.error_count, 1, 3000);
      ccol_event_loop_remove(loop, reg);
    }
  }

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(fired);
}

/* A healthy connected socket is writable from the moment the loop registers
 * it, so a write direction armed for EPOLLOUT on behalf of a registration
 * with no on_writable handler is reported ready on every single epoll_wait
 * and nothing can consume it. That's an unbounded spin on the reactor
 * thread, which at the default num_reactor_threads of 1 is the loop's only
 * thread, so the spin starves every other registration on it too, and no
 * action from a peer is needed to trigger it.
 *
 * We measure with _ccol_event_loop_poller_iterations_for_tests, a direct
 * count of completed epoll_wait calls, the same way the pause tests above
 * measure their own spin. Arm EPOLLOUT for a registration with no writer
 * and the delta below reaches the hundreds of thousands. on_error should
 * stay silent as well, because nothing is actually wrong with this
 * socket. */
TEST(ccol_event_loop,
     fd_error_only_write_registration_does_not_spin_single_thread) {
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
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);
    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), handlers, &ctx,
        &err);
    registered = (reg != CCOL_EVENT_REG_INVALID);
    if (registered) {
      /* Gives a runaway loop a real chance to start before the first sample,
       * and then the test measures the delta across another wide
       * window. */
      usleep(50000);
      uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = _ccol_event_loop_poller_iterations_for_tests(loop) - before;
      ccol_event_loop_remove(loop, reg);
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

/* The multi-threaded reactor counterpart. With EPOLLONESHOT the spin is a
 * continuous cycle of collect, submit, skip and re-arm rather than a tight
 * single-thread loop, and the same counter, the number of completed
 * epoll_wait calls on the one and only poller thread, catches it the same
 * way. */
TEST(ccol_event_loop,
     fd_error_only_write_registration_does_not_spin_multi_thread) {
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
    ccol_event_loop_construct_scoped(loop, 8, 4, 3);
    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), handlers, &ctx,
        &err);
    registered = (reg != CCOL_EVENT_REG_INVALID);
    if (registered) {
      usleep(50000);
      uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = _ccol_event_loop_poller_iterations_for_tests(loop) - before;
      ccol_event_loop_remove(loop, reg);
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

/* ==========================================================================
 * Timed waits: the deadline and the condition variable share one clock
 * ========================================================================== */

typedef struct timed_clock_ctx {
  ccol_circular_queue *cq;
  ccol_dynamic_queue *dq;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool returned;
  ccol_retval_t rv;
  long long elapsed_us;
} timed_clock_ctx;

static void timed_clock_ctx_init(timed_clock_ctx *c) {
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->cq = NULL;
  c->dq = NULL;
  c->returned = false;
  c->rv = ccol_success;
  c->elapsed_us = 0;
}

static void timed_clock_ctx_destroy(timed_clock_ctx *c) {
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

static void timed_clock_ctx_finish(timed_clock_ctx *c, ccol_retval_t rv,
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

static void *timed_clock_circq_recv_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  c_message_t m = {.data = NULL, .size = 0};
  ccol_retval_t rv = ccol_circq_timed_recv_zc(c->cq, &m, timeout);
  if (rv == ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

static void *timed_clock_circq_send_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  c_message_t m = {.data = malloc(4), .size = 4};
  assert(m.data != NULL);
  memcpy(m.data, "abc", 4);
  ccol_retval_t rv = ccol_circq_timed_send_zc(c->cq, &m, timeout);
  if (rv != ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

static void *timed_clock_dynmq_recv_thread(void *arg) {
  timed_clock_ctx *c = (timed_clock_ctx *)arg;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  uint64_t timeout = 100000;
  c_message_t m = {.data = NULL, .size = 0};
  ccol_retval_t rv = ccol_dynmq_timed_recv_zc(c->dq, &m, timeout);
  if (rv == ccol_success) free(m.data);
  timed_clock_ctx_finish(c, rv, &started);
  return NULL;
}

/* A timed wait's absolute deadline and the condition variable it goes to
 * must use the same clock. The two clocks a deadline can come from are
 * decades apart, so a mismatch is never a small error: in one direction the
 * call returns at once without waiting at all, and in the other it blocks
 * for what is practically for ever.
 *
 * The call runs on its own thread with the main thread's wait bounding it,
 * so the second failure shows up as an ordinary test failure instead of a
 * hung binary, and the queue operation that releases a thread still blocked
 * is what lets the test join it on either path. Both mismatches are caught:
 * initialise the queue's condition variables against a different clock
 * from the deadline and the call returns well inside the 100 ms it asked
 * for, compute the deadline against a different clock from the condition
 * variable and the call doesn't return inside the bound below. */
TEST(circular_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccol_circular_queue_create_mp(2, NULL, NULL);
  bool created = (ctx.cq != NULL);
  bool started = false;
  bool returned = false;
  ccol_retval_t rv = ccol_success;
  long long elapsed_us = 0;

  pthread_t tid;
  if (created) {
    started =
        (pthread_create(&tid, NULL, timed_clock_circq_recv_thread, &ctx) == 0);
  }
  if (started) {
    returned = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.returned, 5000);
    if (!returned) {
      /* Releases a thread that's blocked on a deadline it can never reach,
       * so the test can still join it below. */
      c_message_t m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      if (ccol_circq_send_zc(ctx.cq, &m) != ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    c_message_t drained = {.data = NULL, .size = 0};
    while (ccol_circq_try_recv_zc(ctx.cq, &drained) == ccol_success) {
      free(drained.data);
      drained = (c_message_t){.data = NULL, .size = 0};
    }
    ccol_circular_queue_destroy(ctx.cq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

/* The send-direction counterpart; see the receive test above. */
TEST(circular_queues, timed_send_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccol_circular_queue_create_mp(1, NULL, NULL);
  bool created = (ctx.cq != NULL);
  bool filled = false;
  bool started = false;
  bool returned = false;
  ccol_retval_t rv = ccol_success;
  long long elapsed_us = 0;

  if (created) {
    c_message_t fill = {.data = malloc(4), .size = 4};
    assert(fill.data != NULL);
    memcpy(fill.data, "abc", 4);
    filled = (ccol_circq_send_zc(ctx.cq, &fill) == ccol_success);
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
      /* A thread is blocked on a deadline it can never reach. This frees
       * the slot it waits for, so the test can still join it below. */
      c_message_t m = {.data = NULL, .size = 0};
      if (ccol_circq_try_recv_zc(ctx.cq, &m) == ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    c_message_t drained = {.data = NULL, .size = 0};
    while (ccol_circq_try_recv_zc(ctx.cq, &drained) == ccol_success) {
      free(drained.data);
      drained = (c_message_t){.data = NULL, .size = 0};
    }
    ccol_circular_queue_destroy(ctx.cq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(filled);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

/* The dynamic queue's receive path; see the circular queue receive test
 * above. */
TEST(dynamic_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.dq = ccol_dynamic_queue_create_mp(NULL, NULL);
  bool created = (ctx.dq != NULL);
  bool started = false;
  bool returned = false;
  ccol_retval_t rv = ccol_success;
  long long elapsed_us = 0;

  pthread_t tid;
  if (created) {
    started =
        (pthread_create(&tid, NULL, timed_clock_dynmq_recv_thread, &ctx) == 0);
  }
  if (started) {
    returned = evl_wait_bool(&ctx.mtx, &ctx.cond, &ctx.returned, 5000);
    if (!returned) {
      c_message_t m = {.data = malloc(4), .size = 4};
      assert(m.data != NULL);
      memcpy(m.data, "abc", 4);
      if (ccol_dynmq_send_zc(ctx.dq, &m) != ccol_success) free(m.data);
    }
    pthread_join(tid, NULL);
    pthread_mutex_lock(&ctx.mtx);
    rv = ctx.rv;
    elapsed_us = ctx.elapsed_us;
    pthread_mutex_unlock(&ctx.mtx);
  }

  if (created) {
    c_message_t drained = {.data = NULL, .size = 0};
    while (ccol_dynmq_try_recv_zc(ctx.dq, &drained) == ccol_success) {
      free(drained.data);
      drained = (c_message_t){.data = NULL, .size = 0};
    }
    ccol_dynamic_queue_destroy(ctx.dq);
  }
  timed_clock_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed_us, 90000LL);
}

/* ==========================================================================
 * A callback that tears down ANOTHER watched queue, on the dispatch pool
 * ========================================================================== */

/* The kinds of selectable that the victim of a cross-queue teardown can be. */
enum { XQD_CIRCQ = 0, XQD_DYNQ = 1, XQD_CHAN = 2 };

typedef struct xqd_slot {
  ccol_event_loop loop;
  int kind;
  ccol_circular_queue *trigger;
  ccol_circular_queue *victim_cq;
  ccol_dynamic_queue *victim_dq;
  ccol_channel *victim_ch;
  ccol_selectable victim_sel;
  ccol_event_reg victim_reg;
  bool victim_destroyed;
  _Atomic int *entered;
  _Atomic int *done;
  _Atomic bool *gate;
  _Atomic bool *gate_timed_out;
} xqd_slot;

/* Sleeps for one millisecond. Every bounded wait of this scenario polls with
 * it, so under valgrind a waiter never burns the quanta that the thread it
 * waits for needs. */
static void xqd_nap(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  nanosleep(&ts, NULL);
}

static void xqd_drain_victim(xqd_slot *s) {
  c_message_t m = {.data = NULL, .size = 0};
  if (s->kind == XQD_DYNQ) {
    while (ccol_dynmq_try_recv_zc(s->victim_dq, &m) == ccol_success) {
      free(m.data);
      m = (c_message_t){.data = NULL, .size = 0};
    }
    return;
  }
  /* A circular queue, or the circular queue that the channel selectable
   * resolved to. The channel's other direction stays empty the whole time. */
  while (ccol_circq_try_recv_zc(s->victim_sel.cq, &m) == ccol_success) {
    free(m.data);
    m = (c_message_t){.data = NULL, .size = 0};
  }
}

static void xqd_destroy_victim(xqd_slot *s) {
  if (s->kind == XQD_CIRCQ) {
    ccol_circular_queue_destroy(s->victim_cq);
  } else if (s->kind == XQD_DYNQ) {
    ccol_dynamic_queue_destroy(s->victim_dq);
  } else {
    ccol_channel_destroy(s->victim_ch);
  }
}

/* The victim's own callback never needs to do anything. Its dispatch only
 * has to be collected and sit behind the workers that tear it down. */
static void xqd_victim_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                   ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

/* The trigger callback. It parks until every dispatch worker is inside a
 * trigger callback AND a dispatch for every victim is queued behind them,
 * and then runs the documented teardown of its victim: remove, drain and
 * destroy. */
static void xqd_trigger_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  xqd_slot *s = (xqd_slot *)arg;
  c_message_t m = {.data = NULL, .size = 0};
  if (ccol_circq_try_recv_zc(sel->cq, &m) == ccol_success) free(m.data);

  atomic_fetch_add(s->entered, 1);
  for (int waited_ms = 0; !atomic_load(s->gate); waited_ms++) {
    if (waited_ms >= 20000) {
      atomic_store(s->gate_timed_out, true);
      break;
    }
    xqd_nap();
  }

  ccol_event_loop_remove(s->loop, s->victim_reg);
  xqd_drain_victim(s);
  xqd_destroy_victim(s);
  s->victim_destroyed = true;
  atomic_fetch_add(s->done, 1);
}

static bool xqd_send_one(ccol_circular_queue *cq, ccol_dynamic_queue *dq) {
  c_message_t m = {.data = malloc(4), .size = 4};
  if (!m.data) return false;
  memcpy(m.data, "abc", 4);
  ccol_retval_t rv =
      cq ? ccol_circq_send_zc(cq, &m) : ccol_dynmq_send_zc(dq, &m);
  if (rv != ccol_success) free(m.data);
  return rv == ccol_success;
}

enum { XQD_MAX_WORKERS = 8 };

typedef struct xqd_scenario {
  ccol_event_loop loop;
  size_t workers;
  size_t built; /* how many slots hold live queues */
  xqd_slot slots[XQD_MAX_WORKERS];
  _Atomic int entered;
  _Atomic int done;
  _Atomic bool gate;
  _Atomic bool gate_timed_out;
} xqd_scenario;

static bool xqd_wait_count(_Atomic int *counter, int target) {
  for (int waited_ms = 0; atomic_load(counter) < target; waited_ms++) {
    if (waited_ms >= 20000) return false;
    xqd_nap();
  }
  return true;
}

/* Tears the scenario down on every path. It opens the gate so every trigger
 * callback that is already running finishes the teardown of its own victim,
 * and then destroys whatever is left. If those callbacks never finish, the
 * dispatch workers are deadlocked and destroying the loop would join them
 * for ever, so in that case the function leaves the loop and its queues
 * behind on purpose and returns false, the test fails and the binary goes
 * on. */
static bool xqd_teardown(xqd_scenario *sc) {
  atomic_store(&sc->gate, true);
  if (!xqd_wait_count(&sc->done, atomic_load(&sc->entered))) return false;
  if (sc->loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(sc->loop);
  for (size_t i = 0; i < sc->built; i++) {
    xqd_slot *s = &sc->slots[i];
    c_message_t m = {.data = NULL, .size = 0};
    while (ccol_circq_try_recv_zc(s->trigger, &m) == ccol_success) {
      free(m.data);
      m = (c_message_t){.data = NULL, .size = 0};
    }
    ccol_circular_queue_destroy(s->trigger);
    if (!s->victim_destroyed) {
      xqd_drain_victim(s);
      xqd_destroy_victim(s);
    }
  }
  return true;
}

/* Builds the queues of one slot. Returns false, with nothing left behind, if
 * an allocation fails. */
static bool xqd_build_slot(xqd_scenario *sc, size_t i, int kind) {
  xqd_slot *s = &sc->slots[i];
  s->loop = sc->loop;
  s->kind = kind;
  s->entered = &sc->entered;
  s->done = &sc->done;
  s->gate = &sc->gate;
  s->gate_timed_out = &sc->gate_timed_out;
  s->victim_destroyed = false;
  s->trigger = ccol_circular_queue_create_mp(4, NULL, NULL);
  if (!s->trigger) return false;
  if (kind == XQD_CIRCQ) {
    s->victim_cq = ccol_circular_queue_create_mp(4, NULL, NULL);
    if (s->victim_cq)
      s->victim_sel =
          ccol_selectable_from_circq(s->victim_cq, ccol_select_read);
  } else if (kind == XQD_DYNQ) {
    s->victim_dq = ccol_dynamic_queue_create_mp(NULL, NULL);
    if (s->victim_dq)
      s->victim_sel = ccol_selectable_from_dynq(s->victim_dq, ccol_select_read);
  } else {
    s->victim_ch = ccol_channel_create_mp(4, NULL, NULL);
    if (s->victim_ch)
      s->victim_sel = ccol_selectable_from_chan(s->victim_ch, ccol_select_read);
  }
  if (!s->victim_cq && !s->victim_dq && !s->victim_ch) {
    ccol_circular_queue_destroy(s->trigger);
    return false;
  }
  return true;
}

/* Runs the whole scenario and returns 0 on success. Any other value names
 * the step that failed, and 18 is the deadlock the test guards against. */
static int xqd_run_scenario(size_t num_reactor_threads, int kind) {
  xqd_scenario *sc = calloc(1, sizeof(*sc));
  if (!sc) return 10;
  sc->workers = num_reactor_threads - 1;
  if (sc->workers == 0 || sc->workers > XQD_MAX_WORKERS) {
    free(sc);
    return 10;
  }
  sc->loop = ccol_event_loop_create(8, 1, num_reactor_threads, NULL);
  if (sc->loop == CCOL_EVENT_LOOP_INVALID) {
    free(sc);
    return 11;
  }

  int rc = 0;
  ccol_event_handlers_t trig_h = {.on_readable = xqd_trigger_on_readable};
  ccol_event_handlers_t vict_h = {.on_readable = xqd_victim_on_readable};
  for (size_t i = 0; i < sc->workers && rc == 0; i++) {
    if (!xqd_build_slot(sc, i, kind)) {
      rc = 12;
      break;
    }
    sc->built++;
    xqd_slot *s = &sc->slots[i];
    s->victim_reg =
        ccol_event_loop_add(sc->loop, s->victim_sel, vict_h, s, NULL);
    ccol_event_reg tr = ccol_event_loop_add(
        sc->loop, ccol_selectable_from_circq(s->trigger, ccol_select_read),
        trig_h, s, NULL);
    if (s->victim_reg == CCOL_EVENT_REG_INVALID || tr == CCOL_EVENT_REG_INVALID)
      rc = 13;
  }

  /* Keep every dispatch worker busy with one trigger callback. */
  for (size_t i = 0; i < sc->workers && rc == 0; i++)
    if (!xqd_send_one(sc->slots[i].trigger, NULL)) rc = 14;
  if (rc == 0 && !xqd_wait_count(&sc->entered, (int)sc->workers)) rc = 15;

  /* Queue one dispatch per victim behind those busy workers. The running
   * trigger jobs don't count as pending, so the count reaches workers exactly
   * when every victim job is waiting in the pool's queue. */
  for (size_t i = 0; i < sc->workers && rc == 0; i++) {
    xqd_slot *s = &sc->slots[i];
    bool ok = (kind == XQD_DYNQ) ? xqd_send_one(NULL, s->victim_dq)
                                 : xqd_send_one(s->victim_sel.cq, NULL);
    if (!ok) rc = 16;
  }
  for (int waited_ms = 0;
       rc == 0 && _ccol_event_loop_dispatch_pool_pending_count_for_tests(
                      sc->loop) < sc->workers;
       waited_ms++) {
    if (waited_ms >= 20000) rc = 17;
    xqd_nap();
  }

  if (rc == 0) {
    atomic_store(&sc->gate, true);
    /* A deadlocked worker never returns, so a regression fails at this
     * step. */
    if (!xqd_wait_count(&sc->done, (int)sc->workers)) rc = 18;
  }
  if (rc == 0 && atomic_load(&sc->gate_timed_out)) rc = 19;

  if (!xqd_teardown(sc)) return rc != 0 ? rc : 20; /* sc leaks on purpose */
  free(sc);
  return rc;
}

/* On a loop with a dispatch pool, a callback that follows the documented
 * teardown for a DIFFERENT watched queue must finish, because the header
 * only forbids destroying the queue whose own callback runs. A dispatch the
 * poller collected for the victim, waiting in the pool's queue behind the
 * very worker that runs the destroy, must hold nothing the destroy waits
 * for.
 *
 * Suppose a collected dispatch claimed the reference a queue destroy waits
 * for when it's collected, instead of when the job runs: every worker would
 * block in the destroy and the scenario would fail at the final wait with
 * code 18. With more than one worker the scenario keeps every worker busy
 * the same way, so no worker is left to run a queued victim job. A
 * deadlocked loop can't be destroyed, so on that failure the scenario leaves
 * it behind and the rest of the suite runs anyway. */
TEST(ccol_event_loop, cross_queue_teardown_from_a_callback_on_the_pool) {
  static const size_t thread_counts[] = {2, 3, 4};
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  int results[3][3];
  for (size_t t = 0; t < 3; t++)
    for (size_t k = 0; k < 3; k++)
      results[t][k] = xqd_run_scenario(thread_counts[t], kinds[k]);
  for (size_t t = 0; t < 3; t++)
    for (size_t k = 0; k < 3; k++) REQUIRE_EQ(results[t][k], 0);
}

/* ==========================================================================
 * Queue wakes: one eventfd write covers every send until the next dispatch
 * ========================================================================== */

typedef struct nwk_ctx {
  int kind; /* XQD_CIRCQ, XQD_DYNQ or XQD_CHAN */
  ccol_circular_queue *cq;
  ccol_dynamic_queue *dq;
  ccol_channel *ch;
  ccol_selectable sel; /* the read selectable that the listener watches */
  _Atomic long received;
  _Atomic long callbacks;
  _Atomic bool first_entered;
  _Atomic bool gate; /* the first callback parks until this is set */
  _Atomic bool abort_run;
} nwk_ctx;

static bool nwk_try_recv(nwk_ctx *c) {
  c_message_t m = {.data = NULL, .size = 0};
  if (c->kind == XQD_DYNQ)
    return ccol_dynmq_try_recv_zc(c->dq, &m) == ccol_success;
  /* A channel selectable resolves to one circular queue, and a channel
   * listener's callback receives on that queue rather than through the
   * channel. */
  return ccol_circq_try_recv_zc(c->sel.cq, &m) == ccol_success;
}

/* Sends one empty message to the queue the listener watches. A full
 * circular queue is retried until the run is aborted. */
static bool nwk_send(nwk_ctx *c) {
  for (;;) {
    c_message_t m = {.data = NULL, .size = 0};
    ccol_retval_t rv = (c->kind == XQD_DYNQ)
                           ? ccol_dynmq_send_zc(c->dq, &m)
                           : ccol_circq_try_send_zc(c->sel.cq, &m);
    if (rv == ccol_success) return true;
    if (rv != ccol_container_full || atomic_load(&c->abort_run)) return false;
    xqd_nap();
  }
}

static void nwk_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
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

static bool nwk_setup(nwk_ctx *c, int kind) {
  memset(c, 0, sizeof(*c));
  c->kind = kind;
  if (kind == XQD_CIRCQ) {
    c->cq = ccol_circular_queue_create_mp(64, NULL, NULL);
    if (!c->cq) return false;
    c->sel = ccol_selectable_from_circq(c->cq, ccol_select_read);
  } else if (kind == XQD_DYNQ) {
    c->dq = ccol_dynamic_queue_create_mp(NULL, NULL);
    if (!c->dq) return false;
    c->sel = ccol_selectable_from_dynq(c->dq, ccol_select_read);
  } else {
    c->ch = ccol_channel_create_mp(64, NULL, NULL);
    if (!c->ch) return false;
    c->sel = ccol_selectable_from_chan(c->ch, ccol_select_read);
  }
  return true;
}

static void nwk_teardown(nwk_ctx *c) {
  while (nwk_try_recv(c)) {
  }
  if (c->kind == XQD_CIRCQ) {
    ccol_circular_queue_destroy(c->cq);
  } else if (c->kind == XQD_DYNQ) {
    ccol_dynamic_queue_destroy(c->dq);
  } else {
    ccol_channel_destroy(c->ch);
  }
}

static bool nwk_wait_received(nwk_ctx *c, long target, int timeout_ms) {
  for (int waited_ms = 0; atomic_load(&c->received) < target; waited_ms++) {
    if (waited_ms >= timeout_ms) return false;
    xqd_nap();
  }
  return true;
}

/* Sends a burst while the queue's only listener runs its first callback and
 * returns how many eventfd wakes the burst cost. The dispatch consumed the
 * first wake before running the callback, so the burst's first send delivers
 * one new wake and every later send finds that wake still pending.
 * *delivered_out tells whether every message reached the listener. */
static long nwk_burst_wakes(size_t num_reactor_threads, int kind, long burst,
                            bool *delivered_out) {
  *delivered_out = false;
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return -1;
  ccol_event_loop loop =
      ccol_event_loop_create(8, 1, num_reactor_threads, NULL);
  if (loop == CCOL_EVENT_LOOP_INVALID) {
    nwk_teardown(&c);
    return -1;
  }
  ccol_event_handlers_t h = {.on_readable = nwk_on_readable};
  ccol_event_reg reg = ccol_event_loop_add(loop, c.sel, h, &c, NULL);
  long wakes = -1;
  if (reg != CCOL_EVENT_REG_INVALID) {
    bool entered = nwk_send(&c);
    for (int waited_ms = 0; entered && !atomic_load(&c.first_entered);
         waited_ms++) {
      if (waited_ms >= 20000) entered = false;
      xqd_nap();
    }
    if (entered) {
      uint64_t before = _ccol_select_test_eventfd_wake_count();
      bool sent_all = true;
      for (long i = 0; i < burst && sent_all; i++) {
        /* A 64-slot circular queue can't hold a longer burst while the
         * callback is parked, so we keep the burst below that. */
        sent_all = nwk_send(&c);
      }
      uint64_t after = _ccol_select_test_eventfd_wake_count();
      if (sent_all) wakes = (long)(after - before);
    }
    atomic_store(&c.gate, true);
    *delivered_out = nwk_wait_received(&c, burst + 1, 20000);
    atomic_store(&c.abort_run, true);
    ccol_event_loop_remove(loop, reg);
  }
  atomic_store(&c.gate, true);
  ccol_event_loop_destroy(loop);
  nwk_teardown(&c);
  return wakes;
}

/* A send to a listener that already has a wake pending costs nothing beyond
 * the queue operation itself. If every send wrote the listener's eventfd
 * whatever its state, the burst of 48 sends would cost 48 wakes instead of
 * 1. */
TEST(ccol_event_loop, a_pending_queue_wake_absorbs_later_sends) {
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

typedef struct nwk_producer_arg {
  nwk_ctx *c;
  long count;
  bool ok;
} nwk_producer_arg;

static void *nwk_producer_thread(void *arg) {
  nwk_producer_arg *a = (nwk_producer_arg *)arg;
  a->ok = true;
  for (long i = 0; i < a->count && a->ok; i++) {
    a->ok = nwk_send(a->c);
    /* A short pause every so often lets the listener drain the queue to
     * empty, so many sends land exactly while a dispatch clears the wake. */
    if ((i & 63) == 0) sched_yield();
  }
  return NULL;
}

/* Every message reaches a listener that drains the queue on each callback
 * while a producer sends right as the callbacks run. A lost wake leaves
 * messages in the queue with no dispatch coming, and the bounded wait for
 * the full count fails. */
static bool nwk_stress_listener(size_t num_reactor_threads, int kind,
                                long count) {
  nwk_ctx c;
  if (!nwk_setup(&c, kind)) return false;
  atomic_store(&c.gate, true); /* no callback parks in this scenario */
  bool ok = false;
  ccol_event_loop loop =
      ccol_event_loop_create(8, 1, num_reactor_threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_readable = nwk_on_readable};
    ccol_event_reg reg = ccol_event_loop_add(loop, c.sel, h, &c, NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      nwk_producer_arg pa = {.c = &c, .count = count, .ok = false};
      pthread_t tid;
      if (pthread_create(&tid, NULL, nwk_producer_thread, &pa) == 0) {
        ok = nwk_wait_received(&c, count, 60000);
        atomic_store(&c.abort_run, true);
        pthread_join(tid, NULL);
        ok = ok && pa.ok;
      }
      ccol_event_loop_remove(loop, reg);
    }
    ccol_event_loop_destroy(loop);
  }
  nwk_teardown(&c);
  return ok;
}

TEST(ccol_event_loop, no_queue_wake_is_lost_while_a_dispatch_clears_it) {
  static const size_t thread_counts[] = {1, 3};
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 3; k++)
      ok[t][k] = nwk_stress_listener(thread_counts[t], kinds[k], 20000);
  for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[t][k]);
}

typedef struct nwk_select_arg {
  nwk_ctx *c;
  long target;
  int idle_fd; /* -1 for the condvar mode of ccol_select */
  bool timed_out;
} nwk_select_arg;

static void *nwk_select_consumer_thread(void *arg) {
  nwk_select_arg *a = (nwk_select_arg *)arg;
  ccol_selectable sels[2];
  size_t n = 0;
  sels[n++] = a->c->sel;
  if (a->idle_fd >= 0)
    sels[n++] = ccol_selectable_from_fd(a->idle_fd, ccol_select_read);
  while (atomic_load(&a->c->received) < a->target &&
         !atomic_load(&a->c->abort_run)) {
    size_t idx = 0;
    ccol_retval_t rv = ccol_select_timed(&idx, n, sels, 10000000);
    if (rv == ccol_timed_out) {
      /* Only a timeout with messages still owed is a lost wake. A waiter
       * whose sibling took the last message is allowed to time out. */
      a->timed_out = atomic_load(&a->c->received) < a->target;
      return NULL;
    }
    if (rv != ccol_success) return NULL;
    while (nwk_try_recv(a->c)) atomic_fetch_add(&a->c->received, 1);
  }
  return NULL;
}

/* The same property for ccol_select() waiters, in condvar mode and in epoll
 * mode. Two waiters compete for every message, so a waiter that a notify
 * woke often finds the queue empty again and waits again inside the same
 * call. ccol_select.a_woken_waiter_that_finds_the_queue_empty_waits_again
 * pins that second wait on its own, where no sibling can cover for a waiter
 * that sleeps through its wake. */
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
  /* A waiter that's still inside ccol_select_timed returns on one more
   * message instead of on its own timeout. */
  for (int i = 0; i < CONSUMERS; i++) {
    c_message_t wake = {.data = NULL, .size = 0};
    if (kind == XQD_DYNQ)
      (void)ccol_dynmq_send_zc(c.dq, &wake);
    else
      (void)ccol_circq_try_send_zc(c.sel.cq, &wake);
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

TEST(ccol_select, no_queue_wake_is_lost_while_the_waiter_clears_it) {
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++)
      ok[m][k] = nwk_stress_select(kinds[k], m == 1, 20000);
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[m][k]);
}

typedef struct nwk_single_arg {
  nwk_ctx *c;
  bool epoll_mode;
  int idle_fd;
  _Atomic int rounds_done;
  _Atomic bool stop;
  bool timed_out_with_work;
  bool failed;
} nwk_single_arg;

/* One waiter that calls ccol_select_timed round after round and takes every
 * message it finds. */
static void *nwk_single_waiter_thread(void *arg) {
  nwk_single_arg *a = (nwk_single_arg *)arg;
  ccol_selectable sels[2];
  size_t n = 0;
  sels[n++] = a->c->sel;
  if (a->epoll_mode)
    sels[n++] = ccol_selectable_from_fd(a->idle_fd, ccol_select_read);
  while (!atomic_load(&a->stop)) {
    size_t idx = 0;
    ccol_retval_t rv = ccol_select_timed(&idx, n, sels, 5000000);
    if (rv == ccol_timed_out) {
      /* A message that sat in the queue for a whole timeout is a wake this
       * waiter slept through. */
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

static bool nwk_waiter_linked(nwk_ctx *c) {
  if (c->kind == XQD_DYNQ) return true; /* no count hook; the pause covers it */
  return _ccol_circq_test_sel_read_waiter_count_for_tests(c->sel.cq) >= 1;
}

/* One waiter, and a producer that takes back the message that woke it
 * before the waiter can look, so the waiter finds the queue empty and waits
 * again inside the same ccol_select_timed call. The next message must wake
 * it. In epoll mode only the wait loop itself clears the wake the first
 * notify recorded, so without that clear the waiter sleeps through the next
 * message and its call times out with that message sitting in the
 * queue. */
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
    /* Wake the waiter with a message and take the message straight back. */
    nwk_send(&c);
    (void)nwk_try_recv(&c);
    xqd_nap();
    /* The message the waiter must see. */
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
    /* One more message ends the current ccol_select_timed call immediately. */
    nwk_send(&c);
    pthread_join(tid, NULL);
  }
  ok = ok && !a.timed_out_with_work && !a.failed;
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  nwk_teardown(&c);
  return ok;
}

TEST(ccol_select, a_woken_waiter_that_finds_the_queue_empty_waits_again) {
  static const int kinds[] = {XQD_CIRCQ, XQD_DYNQ, XQD_CHAN};
  bool ok[2][3];
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++)
      ok[m][k] = nwk_single_waiter_rounds(kinds[k], m == 1, 200);
  for (size_t m = 0; m < 2; m++)
    for (size_t k = 0; k < 3; k++) REQUIRE_TRUE(ok[m][k]);
}

/* ==========================================================================
 * Timeout arithmetic: saturation and negative tv_nsec
 * ========================================================================== */

/* The largest time_t, worked out here without the library: from the width
 * of the type, not from any constant the library defines. */
static time_t tsat_time_max(void) {
  return (time_t)(((uintmax_t)1 << (sizeof(time_t) * CHAR_BIT - 1)) - 1);
}

/* A duration past the end of time saturates. UINT64_MAX microseconds is
 * about 584942 years, more than a 32-bit time_t can count from any start, so
 * added to a start near the largest time_t it goes past it on every width.
 * An unsaturated sum would wrap into a negative tv_sec, and every timed
 * wait takes that as already elapsed. */
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
  ccol_timespec_add_us(&u, 5000000); /* 5 s */
  REQUIRE_TRUE(u.tv_sec == tsat_time_max());
  REQUIRE_EQ(u.tv_nsec, 999999999L);

  struct timespec v = {.tv_sec = tsat_time_max() - 5, .tv_nsec = 0};
  ccol_timespec_add_us(&v, 5000000); /* lands exactly on the largest time_t */
  REQUIRE_TRUE(v.tv_sec == tsat_time_max());
  REQUIRE_EQ(v.tv_nsec, 0L);
}

typedef struct tsat_recv_arg {
  ccol_circular_queue *cq;
  _Atomic bool returned;
  ccol_retval_t rv;
} tsat_recv_arg;

static void *tsat_recv_thread(void *arg) {
  tsat_recv_arg *a = (tsat_recv_arg *)arg;
  c_message_t m = {.data = NULL, .size = 0};
  a->rv = ccol_circq_timed_recv_zc(a->cq, &m, UINT64_MAX);
  atomic_store(&a->returned, true);
  return NULL;
}

/* A timed receive whose timeout lies past the end of time waits for a
 * message instead of timing out at once. With a deadline that wraps, the
 * receive returns ccol_timed_out before the send. */
TEST(circular_queues, a_timeout_past_the_end_of_time_waits_for_a_message) {
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(4, NULL, NULL);
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
    c_message_t m = {.data = NULL, .size = 0};
    bool sent = (ccol_circq_send_zc(cq, &m) == ccol_success);
    (void)sent;
    pthread_join(tid, NULL);
  }
  c_message_t drained = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &drained) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);
  REQUIRE_TRUE(started);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(a.rv, ccol_success);
}

/* Every other timed call, with the largest timeout and with one just below
 * it, waits for its event instead of timing out at once. UINT64_MAX is the
 * "no time limit" of ccol_select_timed, so UINT64_MAX - 1 is the value that
 * reaches a select's saturating deadline. Each case runs on its own thread,
 * and the main thread provides the event only after it has seen the call
 * waiting for a while. A deadline that wraps into the past would make each
 * call return ccol_timed_out before the event. */
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

typedef struct huge_arg {
  huge_kind kind;
  ccol_circular_queue *cq;
  ccol_dynamic_queue *dq;
  ccol_channel *ch;
  int fd;
  _Atomic bool returned;
  ccol_retval_t rv;
} huge_arg;

static void *huge_wait_thread(void *arg) {
  huge_arg *a = (huge_arg *)arg;
  c_message_t m = {.data = NULL, .size = 0};
  size_t idx = 99;
  switch (a->kind) {
    case HUGE_CIRCQ_SEND:
      a->rv = ccol_circq_timed_send_zc(a->cq, &m, UINT64_MAX);
      break;
    case HUGE_DYNMQ_RECV:
      a->rv = ccol_dynmq_timed_recv_zc(a->dq, &m, UINT64_MAX);
      break;
    case HUGE_CHAN_RECV:
      a->rv = ccol_chan_timed_recv_zc(a->ch, &m, UINT64_MAX);
      break;
    case HUGE_CHAN_SEND:
      a->rv = ccol_chan_timed_send_zc(a->ch, &m, UINT64_MAX - 1);
      break;
    case HUGE_SELECT_QUEUE:
      a->rv = ccol_select_timed_va(
          &idx, UINT64_MAX - 1,
          ccol_selectable_from_circq(a->cq, ccol_select_read));
      break;
    case HUGE_SELECT_QUEUE_NO_LIMIT:
      a->rv = ccol_select_timed_va(
          &idx, UINT64_MAX,
          ccol_selectable_from_circq(a->cq, ccol_select_read));
      break;
    case HUGE_SELECT_FD:
      a->rv = ccol_select_timed_va(
          &idx, UINT64_MAX - 1,
          ccol_selectable_from_fd(a->fd, ccol_select_read));
      break;
    default:
      break;
  }
  atomic_store(&a->returned, true);
  return NULL;
}

/* A worker of the channel fills the worker-to-owner direction. */
static void *huge_fill_chan_thread(void *arg) {
  c_message_t m = {.data = NULL, .size = 0};
  ccol_chan_try_send_zc((ccol_channel *)arg, &m);
  return NULL;
}

/* A worker of the channel empties the owner-to-worker direction. */
static void *huge_drain_chan_thread(void *arg) {
  c_message_t m = {.data = NULL, .size = 0};
  while (ccol_chan_try_recv_zc((ccol_channel *)arg, &m) == ccol_success) {
  }
  return NULL;
}

static void huge_drain_chan_as_worker(ccol_channel *ch) {
  pthread_t t;
  if (pthread_create(&t, NULL, huge_drain_chan_thread, ch) == 0)
    pthread_join(t, NULL);
}

TEST(timed_calls, a_huge_timeout_waits_for_the_event) {
  bool started[HUGE_KIND_COUNT] = {false};
  bool early[HUGE_KIND_COUNT] = {false};
  ccol_retval_t rv[HUGE_KIND_COUNT];
  for (int k = 0; k < HUGE_KIND_COUNT; k++) {
    huge_arg a = {.kind = (huge_kind)k,
                  .fd = -1,
                  .returned = false,
                  .rv = ccol_unexpected_failure};
    int pfd[2] = {-1, -1};
    c_message_t filler = {.data = NULL, .size = 0};
    if (k == HUGE_CIRCQ_SEND) {
      a.cq = ccol_circular_queue_create_mp(1, NULL, NULL);
      ccol_circq_send_zc(a.cq, &filler); /* full */
    } else if (k == HUGE_SELECT_QUEUE || k == HUGE_SELECT_QUEUE_NO_LIMIT) {
      a.cq = ccol_circular_queue_create_mp(1, NULL, NULL);
    } else if (k == HUGE_DYNMQ_RECV) {
      a.dq = ccol_dynamic_queue_create_mp(NULL, NULL);
    } else if (k == HUGE_CHAN_RECV || k == HUGE_CHAN_SEND) {
      /* The main thread owns the channel and the waiting thread is a
       * worker, so it receives from and sends to the queues that the owner
       * reaches with the opposite call. */
      a.ch = ccol_channel_create_mp(1, NULL, NULL);
      if (k == HUGE_CHAN_SEND) {
        /* fill the worker-to-owner direction from a worker thread */
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
      c_message_t m = {.data = NULL, .size = 0};
      switch ((huge_kind)k) {
        case HUGE_CIRCQ_SEND:
        case HUGE_SELECT_QUEUE:
        case HUGE_SELECT_QUEUE_NO_LIMIT:
          if (k == HUGE_CIRCQ_SEND)
            ccol_circq_recv_zc(a.cq, &m);
          else
            ccol_circq_send_zc(a.cq, &m);
          break;
        case HUGE_DYNMQ_RECV:
          ccol_dynmq_send_zc(a.dq, &m);
          break;
        case HUGE_CHAN_RECV:
          ccol_chan_send_zc(a.ch, &m);
          break;
        case HUGE_CHAN_SEND:
          ccol_chan_recv_zc(a.ch, &m);
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
    c_message_t drained = {.data = NULL, .size = 0};
    if (a.cq) {
      while (ccol_circq_try_recv_zc(a.cq, &drained) == ccol_success) {
      }
      ccol_circular_queue_destroy(a.cq);
    }
    if (a.dq) {
      while (ccol_dynmq_try_recv_zc(a.dq, &drained) == ccol_success) {
      }
      ccol_dynamic_queue_destroy(a.dq);
    }
    if (a.ch) {
      while (ccol_chan_try_recv_zc(a.ch, &drained) == ccol_success) {
      }
      huge_drain_chan_as_worker(a.ch);
      ccol_channel_destroy(a.ch);
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

/* ------------------------------------------------------------------------
 * ccol_select_timed on an fd: a short or zero timeout still takes a look.
 * ------------------------------------------------------------------------ */

/* A timeout of 0 is a poll that doesn't block and a timeout of 1000 us is a
 * wait of at most one millisecond, and either one must report an fd that is
 * already readable. A wait loop that reports a timeout without making a
 * single epoll_wait call once the remaining time rounds down to 0 ms makes
 * both calls return ccol_timed_out. */
TEST(ccol_select, timed_zero_and_one_ms_report_an_already_readable_fd) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  test_write_retry_eintr(pfd[1], "x", 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx0 = 99, idx1 = 99, idx_mixed = 99;
  ccol_retval_t r0 = ccol_select_timed_va(
      &idx0, 0, ccol_selectable_from_fd(pfd[0], ccol_select_read));
  ccol_retval_t r1 = ccol_select_timed_va(
      &idx1, 1000, ccol_selectable_from_fd(pfd[0], ccol_select_read));
  /* An empty queue next to the ready fd, which is the second selectable. */
  ccol_retval_t r_mixed = ccol_select_timed_va(
      &idx_mixed, 0, ccol_selectable_from_circq(cq, ccol_select_read),
      ccol_selectable_from_fd(pfd[0], ccol_select_read));

  ccol_circular_queue_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);

  REQUIRE_EQ(r0, ccol_success);
  REQUIRE_EQ(idx0, (size_t)0);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(idx1, (size_t)0);
  REQUIRE_EQ(r_mixed, ccol_success);
  REQUIRE_EQ(idx_mixed, (size_t)1);
}

/* A timed wait on an fd never ends before its deadline, since the remaining
 * time is rounded UP to a whole millisecond. Truncate the remaining time and
 * the wait gives up once less than a millisecond is left, so nearly every
 * round below returns early. */
TEST(ccol_select, timed_fd_wait_never_ends_before_its_deadline) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int early = 0;
  ccol_retval_t last = ccol_success;
  for (int round = 0; round < 20; round++) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t idx = 99;
    last = ccol_select_timed_va(
        &idx, 2000, ccol_selectable_from_fd(pfd[0], ccol_select_read));
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

/* ------------------------------------------------------------------------
 * ccol_event_loop_add: an allocation failure after the entry is built.
 * ------------------------------------------------------------------------ */

/* An allocator that refuses every allocation of an fd add that is large
 * enough to be the growth of a stripe's fd index. The index takes an insert
 * for as long as its table has room, so the first refusals leave the add
 * successful and a later one makes it fail. During each refusal the
 * allocator makes the fd being added readable and then watches the poller
 * for a while. If the entry were already published to epoll at that moment
 * the poller would wake for it, and on the add that fails it could hold the
 * entry pointer while the add frees the entry, so the poller must stay
 * asleep during every refusal. */
static ccol_event_loop g_afi_loop = CCOL_EVENT_LOOP_INVALID;
static _Atomic bool g_afi_armed = false;
static _Atomic int g_afi_refusals = 0;
static _Atomic uint64_t g_afi_poller_delta = 0;
static int g_afi_signal_fd = -1;

static void *afi_malloc(size_t n) { return malloc(n); }
static void *afi_realloc(void *p, size_t n) { return realloc(p, n); }
static void afi_free(void *p) { free(p); }
static void *afi_calloc(size_t a, size_t b) {
  if (atomic_load(&g_afi_armed) && a * b >= 500) {
    atomic_store(&g_afi_armed, false);
    uint64_t before = _ccol_event_loop_poller_iterations_for_tests(g_afi_loop);
    test_write_retry_eintr(g_afi_signal_fd, "x", 1);
    evl_sleep_ms(50);
    uint64_t after = _ccol_event_loop_poller_iterations_for_tests(g_afi_loop);
    atomic_fetch_add(&g_afi_poller_delta, after - before);
    atomic_fetch_add(&g_afi_refusals, 1);
    return NULL;
  }
  return calloc(a, b);
}

static _Atomic int g_afi_readable = 0;
static void afi_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)arg;
  char c;
  if (read(sel->fd, &c, 1) == 1) atomic_fetch_add(&g_afi_readable, 1);
}

/* An add that published the entry to epoll before inserting it into the fd
 * index would wake the poller during the refused allocation, so
 * g_afi_poller_delta wouldn't be 0, and under valgrind the poller would read
 * the entry after the failed add freed it. */
TEST(ccol_event_loop, an_fd_index_allocation_failure_publishes_nothing) {
  enum { AFI_MAX_FDS = 64 };
  int rfd[AFI_MAX_FDS], wfd[AFI_MAX_FDS];
  ccol_event_reg regs[AFI_MAX_FDS];
  int n_fds = 0;
  int failed_at = -1;
  int expected_readable = 0;
  bool dispatch_timed_out = false;
  char *fail_err = NULL;
  ccol_event_reg retry_reg = CCOL_EVENT_REG_INVALID;
  bool dispatched_after_retry = false;

  atomic_store(&g_afi_refusals, 0);
  atomic_store(&g_afi_readable, 0);
  atomic_store(&g_afi_poller_delta, 0);
  ccol_memmgmt_procs_t mp = {afi_malloc, afi_free, afi_calloc, afi_realloc};
  g_afi_loop = ccol_event_loop_create_mp(8, 1, 1, &mp, NULL);
  bool created = (g_afi_loop != CCOL_EVENT_LOOP_INVALID);
  ccol_event_handlers_t h = {.on_readable = afi_on_readable};

  while (created && n_fds < AFI_MAX_FDS && failed_at < 0) {
    int p[2];
    if (pipe(p) != 0) break;
    rfd[n_fds] = p[0];
    wfd[n_fds] = p[1];
    g_afi_signal_fd = p[1];
    int refusals_before = atomic_load(&g_afi_refusals);
    atomic_store(&g_afi_armed, true);
    char *err = NULL;
    regs[n_fds] = ccol_event_loop_add(
        g_afi_loop, ccol_selectable_from_fd(p[0], ccol_select_read), h, NULL,
        &err);
    atomic_store(&g_afi_armed, false);
    if (regs[n_fds] == CCOL_EVENT_REG_INVALID) {
      failed_at = n_fds;
      fail_err = err;
    } else if (atomic_load(&g_afi_refusals) != refusals_before) {
      /* The add succeeded after a refusal and its fd holds the byte the
       * allocator wrote. We wait until that byte is dispatched so the poller
       * is asleep again before the next refusal watches it. */
      expected_readable++;
      int waited = 0;
      while (atomic_load(&g_afi_readable) < expected_readable &&
             waited < 2000) {
        evl_sleep_ms(5);
        waited += 5;
      }
      if (atomic_load(&g_afi_readable) < expected_readable)
        dispatch_timed_out = true;
    }
    n_fds++;
  }

  /* The loop keeps working: the same fd adds cleanly without the refusal,
   * and the byte the allocator wrote gets dispatched. */
  if (failed_at >= 0) {
    retry_reg = ccol_event_loop_add(
        g_afi_loop, ccol_selectable_from_fd(rfd[failed_at], ccol_select_read),
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
      if (regs[i] != CCOL_EVENT_REG_INVALID)
        ccol_event_loop_remove(g_afi_loop, regs[i]);
    if (retry_reg != CCOL_EVENT_REG_INVALID)
      ccol_event_loop_remove(g_afi_loop, retry_reg);
    ccol_event_loop_destroy(g_afi_loop);
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
  REQUIRE_NE(retry_reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(dispatched_after_retry);
}

/* ------------------------------------------------------------------------
 * ccol_event_loop_add: a direction that is already in use is told apart.
 * ------------------------------------------------------------------------ */

TEST(ccol_event_loop, add_reports_a_direction_already_in_use_by_its_err_str) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  ccol_event_handlers_t h = {.on_error = evl_on_error};
  char *err1 = NULL, *err2 = NULL;
  ccol_event_reg r1 = CCOL_EVENT_REG_INVALID, r2 = CCOL_EVENT_REG_INVALID;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    r1 = ccol_event_loop_add(loop,
                             ccol_selectable_from_fd(pfd[0], ccol_select_read),
                             h, NULL, &err1);
    r2 = ccol_event_loop_add(loop,
                             ccol_selectable_from_fd(pfd[0], ccol_select_read),
                             h, NULL, &err2);
    if (r1 != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r1);
    if (r2 != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r2);
    ccol_event_loop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  bool in_use = err2 != NULL && strstr(err2, "already in use") != NULL;
  REQUIRE_NE(r1, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(err1 == NULL);
  REQUIRE_EQ(r2, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(in_use);
}

/* ------------------------------------------------------------------------
 * ccol_event_loop: an event that no handler can take doesn't spin.
 * ------------------------------------------------------------------------ */

typedef struct {
  _Atomic int error_count;
  ccol_event_loop loop;
  _Atomic ccol_event_reg self_reg;
} unh_ctx;

static void unh_on_error_remove_self(ccol_event_loop loop, ccol_event_reg reg,
                                     ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  unh_ctx *c = (unh_ctx *)arg;
  atomic_fetch_add(&c->error_count, 1);
  ccol_event_reg r = atomic_exchange(&c->self_reg, CCOL_EVENT_REG_INVALID);
  if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
}

/* Counts the epoll_wait calls the poller completes in a 150 ms window, after
 * a 50 ms lead-in that gives a runaway loop time to start. */
static uint64_t unh_poller_iterations_in_window(ccol_event_loop loop) {
  evl_sleep_ms(50);
  uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
  evl_sleep_ms(150);
  return _ccol_event_loop_poller_iterations_for_tests(loop) - before;
}

/* A read registration with no handler at all, on a socket whose peer
 * closes, so the hang-up reaches a registration that can't report it to
 * anyone. */
static void unh_run_no_handler_case(size_t threads, uint64_t *iterations,
                                    uint64_t *after_resume, bool *ok) {
  *ok = false;
  *iterations = 0;
  *after_resume = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t none = {0};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), none, NULL,
        NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      close(sv[1]);
      sv[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      /* A resume arms the registration again, and the hang-up mutes it
       * again without any spin. */
      bool resumed = ccol_event_loop_resume(loop, reg) == ccol_success;
      *after_resume = unh_poller_iterations_in_window(loop);
      *ok = resumed && ccol_event_loop_remove(loop, reg) == ccol_success;
    }
    ccol_event_loop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

typedef struct {
  _Atomic int readable_count;
  _Atomic int eof_count;
  _Atomic ccol_event_reg self_reg;
} unh_eof_ctx;

/* Reads the fd once, and at end of file removes its own registration, which
 * is what the hang-up contract asks of a handler that reads it. */
static void unh_on_readable_remove_at_eof(ccol_event_loop loop,
                                          ccol_event_reg reg,
                                          ccol_selectable *sel, void *arg) {
  (void)reg;
  unh_eof_ctx *c = (unh_eof_ctx *)arg;
  atomic_fetch_add(&c->readable_count, 1);
  char buf[64];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  if (n == 0) {
    atomic_fetch_add(&c->eof_count, 1);
    ccol_event_reg r = atomic_exchange(&c->self_reg, CCOL_EVENT_REG_INVALID);
    if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
  }
}

/* A read registration with on_readable and no on_error, on a pipe whose
 * writer closes, which reports EPOLLHUP without EPOLLIN. Since there's no
 * on_error the hang-up goes to on_readable, whose read() returns 0, and the
 * handler removes the registration, so nothing spins. */
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
  atomic_init(&ctx.self_reg, CCOL_EVENT_REG_INVALID);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_readable = unh_on_readable_remove_at_eof};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, &ctx, NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      atomic_store(&ctx.self_reg, reg);
      close(pfd[1]);
      pfd[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = true;
    }
    /* The destroy joins every thread, so no callback runs with &ctx after
     * this point, and a registration the callback didn't remove goes away
     * with the loop. */
    ccol_event_loop_destroy(loop);
  }
  *readable = atomic_load(&ctx.readable_count);
  *eofs = atomic_load(&ctx.eof_count);
  close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
}

/* A muted read registration leaves the write registration of the same fd
 * armed, so the hang-up reaches its on_error, which removes it. */
static void unh_run_sibling_case(size_t threads, uint64_t *iterations,
                                 int *errors, bool *ok) {
  *ok = false;
  *iterations = 0;
  *errors = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  unh_ctx ctx;
  atomic_init(&ctx.error_count, 0);
  atomic_init(&ctx.self_reg, CCOL_EVENT_REG_INVALID);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ctx.loop = loop;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t none = {0};
    ccol_event_handlers_t err_only = {.on_error = unh_on_error_remove_self};
    ccol_event_reg rreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), none, NULL,
        NULL);
    ccol_event_reg wreg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), err_only, &ctx,
        NULL);
    if (rreg != CCOL_EVENT_REG_INVALID && wreg != CCOL_EVENT_REG_INVALID) {
      atomic_store(&ctx.self_reg, wreg);
      close(sv[1]);
      sv[1] = -1;
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = ccol_event_loop_remove(loop, rreg) == ccol_success;
    }
    /* A registration the callback didn't remove goes away with the
     * loop. */
    ccol_event_loop_destroy(loop);
  }
  *errors = atomic_load(&ctx.error_count);
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

/* Runs the three cases of this group on a loop with `threads` reactor
 * threads, before any assertion, so an early return leaves nothing
 * running. */
typedef struct {
  uint64_t it_none, it_none_resume, it_reader, it_sibling;
  bool ok_none, ok_reader, ok_sibling;
  int readable, eofs, sibling_errors;
} unh_results;

static void unh_run_all(size_t threads, unh_results *r) {
  unh_run_no_handler_case(threads, &r->it_none, &r->it_none_resume,
                          &r->ok_none);
  unh_run_reader_without_on_error_case(threads, &r->it_reader, &r->readable,
                                       &r->eofs, &r->ok_reader);
  unh_run_sibling_case(threads, &r->it_sibling, &r->sibling_errors,
                       &r->ok_sibling);
}

/* A hang-up that no handler can take must not stay armed: level-triggered
 * epoll would report it on every epoll_wait and each window below would
 * count thousands of poller iterations instead of a handful. The reader
 * case also fails if the hang-up isn't delivered to on_readable, because
 * the registration is then muted and readable stays 0. */
TEST(ccol_event_loop, an_event_with_no_handler_does_not_spin_single_thread) {
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

/* The dispatch-pool counterpart. With EPOLLONESHOT the spin turns into a
 * cycle of collect and re-arm, and the same counter catches it. */
TEST(ccol_event_loop, an_event_with_no_handler_does_not_spin_multi_thread) {
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

/* ------------------------------------------------------------------------
 * ccol_event_loop: an error with no on_error reaches the direction handler.
 * ------------------------------------------------------------------------ */

typedef struct {
  _Atomic int readable_count;
  _Atomic int refused_count;
  _Atomic int data_count;
} derr_udp_ctx;

/* Drains a connected UDP socket. A pending ICMP port unreachable comes back
 * as one ECONNREFUSED, and reading it clears it too. */
static void derr_on_udp_readable(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, void *arg) {
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

/* Polls an atomic counter until it reaches want, for at most max_ms. */
static bool derr_wait_count(_Atomic int *counter, int want, int max_ms) {
  for (int waited = 0; waited < max_ms; waited += 5) {
    if (atomic_load(counter) >= want) return true;
    evl_sleep_ms(5);
  }
  return atomic_load(counter) >= want;
}

/* A connected UDP socket with on_readable and no on_error. Sending a
 * datagram to a port with no socket makes the kernel queue an ICMP port
 * unreachable on the socket, which epoll reports as EPOLLERR without
 * EPOLLIN. A server then binds that port and sends three datagrams back. */
static void derr_run_udp_case(size_t threads, int *refused, int *data,
                              bool *ok) {
  *ok = false;
  *refused = 0;
  *data = 0;
  struct sockaddr_in srv_addr = {.sin_family = AF_INET,
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t alen = sizeof(srv_addr);
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  if (probe < 0) return;
  bool have_port =
      bind(probe, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0 &&
      getsockname(probe, (struct sockaddr *)&srv_addr, &alen) == 0;
  close(probe);
  if (!have_port) return;

  int cli = ccol_socket_nb(AF_INET, SOCK_DGRAM, 0);
  if (cli < 0) return;
  if (test_connect(cli, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
    close(cli);
    return;
  }
  struct sockaddr_in cli_addr;
  alen = sizeof(cli_addr);
  if (getsockname(cli, (struct sockaddr *)&cli_addr, &alen) != 0) {
    close(cli);
    return;
  }

  derr_udp_ctx ctx;
  atomic_init(&ctx.readable_count, 0);
  atomic_init(&ctx.refused_count, 0);
  atomic_init(&ctx.data_count, 0);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  int srv = -1;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_readable = derr_on_udp_readable};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(cli, ccol_select_read), h, &ctx, NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      bool sent = send(cli, "q", 1, 0) == 1;
      bool saw_error = sent && derr_wait_count(&ctx.refused_count, 1, 2000);
      srv = socket(AF_INET, SOCK_DGRAM, 0);
      bool bound = srv >= 0 && bind(srv, (struct sockaddr *)&srv_addr,
                                    sizeof(srv_addr)) == 0;
      bool replied = bound;
      for (int i = 0; replied && i < 3; i++)
        replied = sendto(srv, "hello", 5, 0, (struct sockaddr *)&cli_addr,
                         sizeof(cli_addr)) == 5;
      bool got_data = replied && derr_wait_count(&ctx.data_count, 3, 2000);
      *ok = saw_error && got_data &&
            ccol_event_loop_remove(loop, reg) == ccol_success;
    }
    /* The destroy joins every thread, so no callback runs with &ctx after
     * this point. */
    ccol_event_loop_destroy(loop);
  }
  *refused = atomic_load(&ctx.refused_count);
  *data = atomic_load(&ctx.data_count);
  if (srv >= 0) close(srv);
  close(cli);
}

/* If an error with no on_error muted the registration, neither the
 * ECONNREFUSED nor any of the three datagrams after it would ever reach
 * on_readable. */
TEST(ccol_event_loop, udp_error_without_on_error_reaches_on_readable_1_thread) {
  int refused = 0, data = 0;
  bool ok = false;
  derr_run_udp_case(1, &refused, &data, &ok);
  REQUIRE_GE(refused, 1);
  REQUIRE_EQ(data, 3);
  REQUIRE_TRUE(ok);
}

TEST(ccol_event_loop,
     udp_error_without_on_error_reaches_on_readable_4_threads) {
  int refused = 0, data = 0;
  bool ok = false;
  derr_run_udp_case(4, &refused, &data, &ok);
  REQUIRE_GE(refused, 1);
  REQUIRE_EQ(data, 3);
  REQUIRE_TRUE(ok);
}

/* Counts the errors of an error-only registration and takes the pending
 * error, the way an application that checks SO_ERROR would. */
static void derr_on_error_count(ccol_event_loop loop, ccol_event_reg reg,
                                ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  int e = 0;
  socklen_t el = sizeof(e);
  (void)getsockopt(sel->fd, SOL_SOCKET, SO_ERROR, &e, &el);
  atomic_fetch_add((_Atomic int *)arg, 1);
}

/* A connected UDP socket with on_error and no direction handler. The ICMP
 * port unreachable of a datagram to a port with no socket reaches on_error,
 * since epoll reports it with EPOLLERR. */
static int derr_run_udp_error_only_case(size_t threads) {
  struct sockaddr_in srv_addr = {.sin_family = AF_INET,
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t alen = sizeof(srv_addr);
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  if (probe < 0) return -1;
  bool have_port =
      bind(probe, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0 &&
      getsockname(probe, (struct sockaddr *)&srv_addr, &alen) == 0;
  close(probe);
  if (!have_port) return -1;
  int cli = ccol_socket_nb(AF_INET, SOCK_DGRAM, 0);
  if (cli < 0) return -1;
  if (test_connect(cli, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
    close(cli);
    return -1;
  }
  _Atomic int errors = 0;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_error = derr_on_error_count};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(cli, ccol_select_read), h,
        (void *)&errors, NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      if (send(cli, "q", 1, 0) == 1) (void)derr_wait_count(&errors, 1, 2000);
      ccol_event_loop_remove(loop, reg);
    }
    /* The destroy joins every thread, so no callback runs with &errors
     * after this point. */
    ccol_event_loop_destroy(loop);
  }
  close(cli);
  return atomic_load(&errors);
}

/* On kqueue this catches a report of the pending error on the error-only
 * read filter that refines to no bit: on_error then never runs. */
TEST(ccol_event_loop, udp_error_reaches_an_error_only_registration_1_thread) {
  REQUIRE_GE(derr_run_udp_error_only_case(1), 1);
}

TEST(ccol_event_loop, udp_error_reaches_an_error_only_registration_4_threads) {
  REQUIRE_GE(derr_run_udp_error_only_case(4), 1);
}

typedef struct {
  _Atomic int reads;
} derr_byte_reader_ctx;

/* Takes one byte per call, so the rest of the data has to report again. */
static void derr_read_one_byte(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  char b;
  if (recv(sel->fd, &b, 1, MSG_DONTWAIT) == 1)
    atomic_fetch_add(&((derr_byte_reader_ctx *)arg)->reads, 1);
}

/* A write direction with on_error alone arms the socket's read filter in
 * its error-only mode, and a reader that joins the same fd then needs that
 * filter in its data mode, level-triggered, so the bytes one call leaves
 * behind report again. On kqueue the 1-thread form fails when the MOD keeps
 * the trigger flags of the error-only filter: the reader runs once and the
 * other bytes stay unread. The 4-thread form checks the same result
 * where every re-arm evaluates the filter again, so it holds either way, and
 * fd_error_only.a_close_after_a_paused_reader_reaches_on_error covers the
 * change of mode in a loop with a dispatch pool. */
static int derr_run_mode_change_case(size_t threads) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  derr_byte_reader_ctx ctx;
  atomic_init(&ctx.reads, 0);
  _Atomic int errors = 0;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t wh = {.on_error = derr_on_error_count};
    ccol_event_reg w = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh,
        (void *)&errors, NULL);
    ccol_event_handlers_t rh = {.on_readable = derr_read_one_byte};
    ccol_event_reg r = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), rh, &ctx, NULL);
    if (w != CCOL_EVENT_REG_INVALID && r != CCOL_EVENT_REG_INVALID &&
        send(sv[1], "0123456789", 10, 0) == 10)
      (void)derr_wait_count(&ctx.reads, 10, 2000);
    if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
    if (w != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, w);
    ccol_event_loop_destroy(loop);
  }
  close(sv[0]);
  close(sv[1]);
  return atomic_load(&ctx.reads);
}

TEST(ccol_event_loop, a_reader_beside_an_error_only_writer_reads_all_1_thread) {
  REQUIRE_EQ(derr_run_mode_change_case(1), 10);
}

TEST(ccol_event_loop,
     a_reader_beside_an_error_only_writer_reads_all_4_threads) {
  REQUIRE_EQ(derr_run_mode_change_case(4), 10);
}

/* ---- Stale reports, listeners, and an error-only filter that stays armed */

static _Atomic ccol_event_reg derr_stale_target;
static _Atomic int derr_stale_paused;

/* Holds the only reactor thread so the next two sends land in one poller
 * batch. */
static void derr_hold(ccol_event_loop loop, ccol_event_reg reg,
                      ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)arg;
  char b[16];
  (void)recv(sel->fd, b, sizeof(b), MSG_DONTWAIT);
  evl_sleep_ms(150);
}

/* Pauses the reader of the other socket, whose data report is already in
 * the same batch. */
static void derr_pause_target(ccol_event_loop loop, ccol_event_reg reg,
                              ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)arg;
  char b[16];
  (void)recv(sel->fd, b, sizeof(b), MSG_DONTWAIT);
  ccol_event_loop_pause(loop, atomic_load(&derr_stale_target));
  atomic_fetch_add(&derr_stale_paused, 1);
}

static void derr_count_reads(ccol_event_loop loop, ccol_event_reg reg,
                             ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  atomic_fetch_add((_Atomic int *)arg, 1);
}

/* A data report the poller collected while a reader wanted data, and
 * handles after that reader was paused, isn't an error, so the writer of the
 * same socket, which has on_error alone, sees nothing, just as with epoll.
 * On kqueue, turning every read report under a mask without CCOL_POLL_IN
 * into an error makes the writer's on_error run once, and the test sees it. */
TEST(fd_stale_report, a_data_report_after_a_pause_is_no_error) {
  int hold[2] = {-1, -1}, pauser[2] = {-1, -1}, target[2] = {-1, -1};
  bool pairs = socketpair(AF_UNIX, SOCK_STREAM, 0, hold) == 0 &&
               socketpair(AF_UNIX, SOCK_STREAM, 0, pauser) == 0 &&
               socketpair(AF_UNIX, SOCK_STREAM, 0, target) == 0;
  atomic_store(&derr_stale_paused, 0);
  _Atomic int reads = 0, writer_errors = 0;
  ccol_event_loop loop =
      pairs ? ccol_event_loop_create(64, 1, 1, NULL) : CCOL_EVENT_LOOP_INVALID;
  bool sent = false;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t hh = {.on_readable = derr_hold};
    ccol_event_loop_add(loop,
                        ccol_selectable_from_fd(hold[0], ccol_select_read), hh,
                        NULL, NULL);
    ccol_event_handlers_t hp = {.on_readable = derr_pause_target};
    ccol_event_loop_add(loop,
                        ccol_selectable_from_fd(pauser[0], ccol_select_read),
                        hp, NULL, NULL);
    ccol_event_handlers_t hr = {.on_readable = derr_count_reads};
    atomic_store(&derr_stale_target,
                 ccol_event_loop_add(
                     loop, ccol_selectable_from_fd(target[0], ccol_select_read),
                     hr, (void *)&reads, NULL));
    ccol_event_handlers_t hw = {.on_error = derr_on_error_count};
    ccol_event_loop_add(loop,
                        ccol_selectable_from_fd(target[0], ccol_select_write),
                        hw, (void *)&writer_errors, NULL);
    evl_sleep_ms(30);
    sent = send(hold[1], "h", 1, 0) == 1;
    evl_sleep_ms(30);
    sent = sent && send(pauser[1], "p", 1, 0) == 1 &&
           send(target[1], "t", 1, 0) == 1;
    (void)derr_wait_count(&derr_stale_paused, 1, 2000);
    evl_sleep_ms(100);
    ccol_event_loop_destroy(loop);
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

/* Connections waiting on a listening socket aren't an error. An error-only
 * registration of the listener sees nothing and the poller stays idle, as
 * with epoll. On kqueue an error-only read filter armed on a listener, which
 * ignores its low-water mark, sends every pending connection to on_error,
 * and a loop with a dispatch pool reports it again on every re-arm. */
typedef struct {
  ccol_event_loop loop;
  int ls;
  _Atomic int *errs;
  _Atomic ccol_event_reg reg;
  atomic_bool listened;
  atomic_bool done;
} derr_late_listen_ctx;

static void derr_late_listen_cb(ccol_event_loop loop, ccol_event_reg self,
                                ccol_selectable *sel, void *arg) {
  (void)sel;
  derr_late_listen_ctx *c = (derr_late_listen_ctx *)arg;
  if (atomic_load(&c->done)) return;
  ccol_event_handlers_t h = {.on_error = derr_on_error_count};
  atomic_store(&c->reg,
               ccol_event_loop_add(
                   loop, ccol_selectable_from_fd(c->ls, ccol_select_read), h,
                   (void *)c->errs, NULL));
  atomic_store(&c->listened, listen(c->ls, 8) == 0);
  ccol_event_loop_pause(loop, self);
  atomic_store(&c->done, true);
}

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
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_error = derr_on_error_count};
    ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
    if (listen_late) {
      derr_late_listen_ctx c = {.loop = loop, .ls = ls, .errs = &errs};
      int trig[2] = {-1, -1};
      bool ok = pipe(trig) == 0;
      ccol_event_reg treg = CCOL_EVENT_REG_INVALID;
      if (ok)
        treg = ccol_event_loop_add(
            loop, ccol_selectable_from_fd(trig[0], ccol_select_read),
            (ccol_event_handlers_t){.on_readable = derr_late_listen_cb}, &c,
            NULL);
      ok = ok && treg != CCOL_EVENT_REG_INVALID && write(trig[1], "x", 1) == 1;
      for (int w = 0; ok && w < 2000 && !atomic_load(&c.done); w++)
        evl_sleep_ms(1);
      if (treg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, treg);
      if (trig[0] >= 0) close(trig[0]);
      if (trig[1] >= 0) close(trig[1]);
      reg = atomic_load(&c.reg);
      if (!atomic_load(&c.listened)) {
        if (reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, reg);
        ccol_event_loop_destroy(loop);
        close(ls);
        return;
      }
    } else {
      reg = ccol_event_loop_add(loop,
                                ccol_selectable_from_fd(ls, ccol_select_read),
                                h, (void *)&errs, NULL);
    }
    for (int i = 0; i < 3; i++) {
      cs[i] = socket(AF_INET, SOCK_STREAM, 0);
      if (cs[i] >= 0)
        (void)test_connect(cs[i], (struct sockaddr *)&a, sizeof(a));
    }
    evl_sleep_ms(50);
    uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
    evl_sleep_ms(150);
    *iterations = _ccol_event_loop_poller_iterations_for_tests(loop) - before;
    if (reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, reg);
    ccol_event_loop_destroy(loop);
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

/* The same for a socket that's registered before it listens, with no MOD
 * after it. On kqueue, if only the registration asks whether the socket
 * listens, every pending connection reaches on_error. */
TEST(fd_listener, a_socket_that_listens_late_reports_no_error_1_thread) {
  int errors;
  uint64_t iterations;
  derr_run_listener_case(1, true, &errors, &iterations);
  REQUIRE_EQ(errors, 0);
  REQUIRE_LT(iterations, (uint64_t)20);
}

/* A write direction with on_error alone, in a loop with a dispatch pool.
 * The peer's half-close is no event for it (and refines to nothing on
 * kqueue), but the full close that follows must reach on_error. With
 * with_reader true a reader is registered first and then paused, so the
 * read filter changes from its data mode to its error-only mode. On kqueue
 * both forms catch an error-only read filter that a report disables,
 * because it was created one-shot or because it kept the one-shot flag of
 * its data mode: it stays disabled after the half-close and the full close
 * is lost. */
static int derr_run_close_after_half_close(bool with_reader) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  _Atomic int errors = 0, reads = 0;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 4, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_reg r = CCOL_EVENT_REG_INVALID;
    if (with_reader) {
      ccol_event_handlers_t rh = {.on_readable = derr_count_reads};
      r = ccol_event_loop_add(loop,
                              ccol_selectable_from_fd(sv[0], ccol_select_read),
                              rh, (void *)&reads, NULL);
    }
    ccol_event_handlers_t wh = {.on_error = derr_on_error_count};
    ccol_event_reg w = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_write), wh,
        (void *)&errors, NULL);
    if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_pause(loop, r);
    evl_sleep_ms(30);
    if (shutdown(sv[1], SHUT_WR) == 0) {
      evl_sleep_ms(100);
      close(sv[1]);
      sv[1] = -1;
      (void)derr_wait_count(&errors, 1, 2000);
    }
    if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
    if (w != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, w);
    ccol_event_loop_destroy(loop);
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

typedef struct {
  ccol_event_loop loop;
  ccol_dynamic_queue *dq;
  _Atomic int fd_calls;
  _Atomic int fd_marked;
  _Atomic int queue_calls;
  _Atomic int queue_marked;
} mark_probe_ctx;

static void mark_probe_on_fd(ccol_event_loop loop, ccol_event_reg reg,
                             ccol_selectable *sel, void *arg) {
  (void)reg;
  mark_probe_ctx *c = (mark_probe_ctx *)arg;
  char byte;
  if (read(sel->fd, &byte, 1) != 1) return;
  if (_ccol_event_loop_test_runs_on_loop_thread(loop))
    atomic_fetch_add(&c->fd_marked, 1);
  atomic_fetch_add(&c->fd_calls, 1);
}

static void mark_probe_on_queue(ccol_event_loop loop, ccol_event_reg reg,
                                ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  mark_probe_ctx *c = (mark_probe_ctx *)arg;
  c_message_t out;
  while (ccol_dynmq_try_recv_zc(c->dq, &out) == ccol_success) {
  }
  if (_ccol_event_loop_test_runs_on_loop_thread(loop) &&
      _ccol_event_loop_test_dispatches_queue(c->dq))
    atomic_fetch_add(&c->queue_marked, 1);
  atomic_fetch_add(&c->queue_calls, 1);
}

/* Runs one fd callback and one queue callback on a loop where storing each
 * thread mark in its key fails, and counts the callbacks that see the marks
 * the self-call guards read anyway. */
static void mark_probe_run(size_t threads, int *fd_calls, int *fd_marked,
                           int *queue_calls, int *queue_marked) {
  *fd_calls = *fd_marked = *queue_calls = *queue_marked = -1;
  int pfd[2];
  if (pipe(pfd) != 0) return;
  mark_probe_ctx ctx;
  ctx.dq = ccol_dynamic_queue_create(NULL);
  atomic_init(&ctx.fd_calls, 0);
  atomic_init(&ctx.fd_marked, 0);
  atomic_init(&ctx.queue_calls, 0);
  atomic_init(&ctx.queue_marked, 0);
  _ccol_event_loop_test_fail_next_marks(UINT32_MAX);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ctx.loop = loop;
  if (loop != CCOL_EVENT_LOOP_INVALID && ctx.dq) {
    ccol_event_handlers_t fh = {.on_readable = mark_probe_on_fd};
    ccol_event_handlers_t qh = {.on_readable = mark_probe_on_queue};
    ccol_event_reg fr = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), fh, &ctx,
        NULL);
    ccol_event_reg qr = ccol_event_loop_add(
        loop, ccol_selectable_from_dynq(ctx.dq, ccol_select_read), qh, &ctx,
        NULL);
    static int payload = 3;
    c_message_t m = {.data = &payload, .size = sizeof(int)};
    char byte = 'x';
    if (fr != CCOL_EVENT_REG_INVALID && qr != CCOL_EVENT_REG_INVALID &&
        write(pfd[1], &byte, 1) == 1 &&
        ccol_dynmq_send_zc(ctx.dq, &m) == ccol_success) {
      (void)derr_wait_count(&ctx.fd_calls, 1, 2000);
      (void)derr_wait_count(&ctx.queue_calls, 1, 2000);
    }
    if (fr != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, fr);
    if (qr != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, qr);
  }
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  _ccol_event_loop_test_fail_next_marks(0);
  if (ctx.dq) {
    c_message_t out;
    while (ccol_dynmq_try_recv_zc(ctx.dq, &out) == ccol_success) {
    }
    ccol_dynamic_queue_destroy(ctx.dq);
  }
  close(pfd[0]);
  close(pfd[1]);
  *fd_calls = atomic_load(&ctx.fd_calls);
  *fd_marked = atomic_load(&ctx.fd_marked);
  *queue_calls = atomic_load(&ctx.queue_calls);
  *queue_marked = atomic_load(&ctx.queue_marked);
}

/* A thread whose key can't store its mark carries it anyway, so the
 * self-destroy and self-shutdown guards see a callback of the loop and of
 * the queue. A failed store that left the thread unmarked would mean no
 * callback sees its mark. */
TEST(ccol_event_loop, a_mark_that_the_key_cannot_store_still_holds) {
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

typedef struct {
  _Atomic int errors;
  _Atomic ccol_event_reg self_reg;
} derr_pipe_ctx;

/* Counts an error dispatch and removes its own registration, because a
 * hang-up stays true and would be reported over and over. */
static void derr_pipe_on_error(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  derr_pipe_ctx *c = (derr_pipe_ctx *)arg;
  atomic_fetch_add(&c->errors, 1);
  ccol_event_reg r = atomic_exchange(&c->self_reg, CCOL_EVENT_REG_INVALID);
  if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
}

/* An error-only registration on one end of a pipe whose other end closes.
 * read_end picks the end we watch: the read end with a read-direction
 * registration, whose writer closes after three writes that nobody reads, or
 * the write end with a write-direction registration, whose reader closes.
 * *errors_before_close counts the error dispatches before the close (data
 * isn't an error), *errors all the dispatches, and *iterations the poller
 * wake-ups while the unread data sits in the pipe. */
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
  atomic_init(&ctx.self_reg, CCOL_EVENT_REG_INVALID);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_error = derr_pipe_on_error};
    int watched = read_end ? pfd[0] : pfd[1];
    ccol_event_reg reg = ccol_event_loop_add(
        loop,
        ccol_selectable_from_fd(
            watched, read_end ? ccol_select_read : ccol_select_write),
        h, &ctx, NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
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
      (void)derr_wait_count(&ctx.errors, 1, 2000);
      /* A second dispatch would show up here. */
      evl_sleep_ms(50);
      ccol_event_reg r = atomic_exchange(&ctx.self_reg, CCOL_EVENT_REG_INVALID);
      if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
    }
    ccol_event_loop_destroy(loop);
    *errors = atomic_load(&ctx.errors);
  }
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
}

/* An error-only reader of a pipe hears the writer's close, and the data
 * before it isn't an error. On kqueue a read end with no filter armed for
 * it never reports the close, so this catches that. */
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

/* An error-only writer of a pipe hears the reader's close, and the room in
 * the pipe before it isn't an error. On kqueue a write end with no filter
 * armed for it never reports the close. */
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

typedef struct {
  _Atomic int writable_count;
  _Atomic ccol_event_reg self_reg;
} derr_writer_ctx;

/* Counts the dispatch and removes its own registration. It never writes, so
 * the broken pipe raises no SIGPIPE. */
static void derr_on_writable_remove_self(ccol_event_loop loop,
                                         ccol_event_reg reg,
                                         ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)sel;
  derr_writer_ctx *c = (derr_writer_ctx *)arg;
  atomic_fetch_add(&c->writable_count, 1);
  ccol_event_reg r = atomic_exchange(&c->self_reg, CCOL_EVENT_REG_INVALID);
  if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, r);
}

/* A write registration with on_writable and no on_error, on a full pipe
 * whose reader closes. There's no room in the pipe, so epoll reports
 * EPOLLERR without EPOLLOUT. */
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
  atomic_init(&ctx.self_reg, CCOL_EVENT_REG_INVALID);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_writable = derr_on_writable_remove_self};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[1], ccol_select_write), h, &ctx,
        NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      atomic_store(&ctx.self_reg, reg);
      close(pfd[0]);
      pfd[0] = -1;
      bool delivered = derr_wait_count(&ctx.writable_count, 1, 2000);
      *iterations = unh_poller_iterations_in_window(loop);
      *ok = delivered;
    }
    ccol_event_loop_destroy(loop);
  }
  *writable = atomic_load(&ctx.writable_count);
  if (pfd[0] >= 0) close(pfd[0]);
  close(pfd[1]);
}

/* If an error with no on_error muted the registration, on_writable would
 * never run. */
TEST(ccol_event_loop, error_without_on_error_reaches_on_writable) {
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

/* ------------------------------------------------------------------------
 * ccol_event_loop: the reclaim epoch crosses a 32-bit boundary.
 * ------------------------------------------------------------------------ */

static void bgen_on_removed(void *arg) {
  atomic_fetch_add((_Atomic int *)arg, 1);
}

static void bgen_on_error(ccol_event_loop loop, ccol_event_reg reg,
                          ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

/* The epoch that proves a removed fd entry safe to free sits just below
 * 2^32 when the entry is removed, and the next advance crosses 2^32. A
 * counter that wraps there leaves the entry pending for ever, and the poller
 * then wakes on its bounded reclaim retry (every 50 ms) for the rest of the
 * loop's life. On a target with a 32-bit size_t this shows up directly:
 * with a size_t epoch the window below counts about ten iterations instead
 * of none. */
TEST(ccol_event_loop,
     reclaim_epoch_crossing_32_bits_does_not_keep_poller_awake) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  _Atomic int removed_calls = 0;
  uint64_t idle_iterations = UINT64_MAX;
  bool added = false, removed = false, notified = false;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_error = bgen_on_error,
                               .on_removed = bgen_on_removed};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h,
        (void *)&removed_calls, NULL);
    added = reg != CCOL_EVENT_REG_INVALID;
    if (added) {
      /* Let the poller settle in an epoll_wait with no timeout, so the value
       * below is the one the removal stamps. */
      evl_sleep_ms(50);
      _ccol_event_loop_set_poller_batch_gen_for_tests(loop,
                                                      (uint64_t)UINT32_MAX);
      removed = ccol_event_loop_remove(loop, reg) == ccol_success;
      notified = derr_wait_count(&removed_calls, 1, 2000);
      evl_sleep_ms(100);
      uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
      evl_sleep_ms(500);
      idle_iterations =
          _ccol_event_loop_poller_iterations_for_tests(loop) - before;
    }
    ccol_event_loop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(added);
  REQUIRE_TRUE(removed);
  REQUIRE_TRUE(notified);
  REQUIRE_LT(idle_iterations, (uint64_t)3);
}

/* ------------------------------------------------------------------------
 * ccol_event_loop_destroy drains with a live handle
 * ------------------------------------------------------------------------ */

/* One connection with two registrations on the same fd, one per direction.
 * The application closes it from inside a callback with the documented
 * pattern: remove both registrations, close the fd and release the state.
 * The test keeps owning the state, so a callback that runs after the close
 * gets counted instead of reading freed memory. */
typedef struct {
  ccol_event_loop loop;
  int fd;
  ccol_event_reg rreg;
  ccol_event_reg wreg;
  _Atomic int closed;
  _Atomic int in_callback;
  _Atomic int destroy_called;
  _Atomic int callbacks_after_close;
  _Atomic int remove_r_rv;
  _Atomic int remove_w_rv;
} ddrain_conn;

static void ddrain_cb(ccol_event_loop l, ccol_event_reg reg, ccol_selectable *s,
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
  /* The main thread calls ccol_event_loop_destroy while this callback runs,
   * so the close below happens inside that destroy's drain. */
  for (int i = 0; i < 5000 && !atomic_load(&c->destroy_called); i++)
    evl_sleep_ms(1);
  /* A destroy that makes the handle stop resolving right at its start shows
   * up here within this window. One that keeps the handle live for its drain
   * never does, and the window just runs out. */
  for (int i = 0; i < 200 && _ccol_event_loop_resolve_for_tests(c->loop); i++)
    evl_sleep_ms(1);
  atomic_store(&c->remove_r_rv, (int)ccol_event_loop_remove(c->loop, c->rreg));
  atomic_store(&c->remove_w_rv, (int)ccol_event_loop_remove(c->loop, c->wreg));
  close(c->fd);
  atomic_store(&c->closed, 1);
}

static void ddrain_run(size_t threads, ddrain_conn *c, bool *entered) {
  int sv[2];
  *entered = false;
  if (ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  memset(c, 0, sizeof(*c));
  atomic_store(&c->remove_r_rv, -1000);
  atomic_store(&c->remove_w_rv, -1000);
  c->fd = sv[0];
  c->loop = ccol_event_loop_create(16, 1, threads, NULL);
  ccol_event_handlers_t hr = {.on_readable = ddrain_cb};
  ccol_event_handlers_t hw = {.on_writable = ddrain_cb};
  /* Data is waiting and the socket is writable, so both directions are
   * ready at the same time. */
  test_write_retry_eintr(sv[1], "hello", 5);
  c->rreg = ccol_event_loop_add(
      c->loop, ccol_selectable_from_fd(sv[0], ccol_select_read), hr, c, NULL);
  c->wreg = ccol_event_loop_add(
      c->loop, ccol_selectable_from_fd(sv[0], ccol_select_write), hw, c, NULL);
  for (int i = 0; i < 5000 && !atomic_load(&c->in_callback); i++)
    evl_sleep_ms(1);
  *entered = atomic_load(&c->in_callback) != 0;
  atomic_store(&c->destroy_called, 1);
  ccol_event_loop_destroy(c->loop);
  if (!atomic_load(&c->closed)) close(sv[0]);
  close(sv[1]);
}

/* A callback that runs during the drain of ccol_event_loop_destroy calls
 * back into the loop exactly as it would during ccol_event_loop_shutdown.
 * Its ccol_event_loop_remove calls succeed, and no callback of the removed
 * registrations runs after it closed the connection. A destroy that makes
 * the handle stop resolving before its drain fails both removals with
 * ccol_invalid_args. */
TEST(ccol_event_loop, destroy_drain_callback_removes_its_registrations_1) {
  ddrain_conn c;
  bool entered;
  ddrain_run(1, &c, &entered);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ(atomic_load(&c.remove_r_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.remove_w_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.callbacks_after_close), 0);
}

TEST(ccol_event_loop, destroy_drain_callback_removes_its_registrations_4) {
  ddrain_conn c;
  bool entered;
  ddrain_run(4, &c, &entered);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ(atomic_load(&c.remove_r_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.remove_w_rv), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&c.callbacks_after_close), 0);
}

/* ------------------------------------------------------------------------
 * Allocation failure: create never returns a dead loop, and a failing
 * dispatch never spins the poller
 * ------------------------------------------------------------------------ */

/* An allocator that fails on demand: fa_fail_at fails exactly the Nth
 * allocation (1-based), fa_fail_size fails every allocation of that size and
 * fa_fail_all fails all of them. */
static _Atomic long fa_count;
static _Atomic long fa_fail_at;
static _Atomic size_t fa_fail_size;
static _Atomic int fa_fail_all;

static bool fa_should_fail(size_t n) {
  long c = atomic_fetch_add(&fa_count, 1) + 1;
  if (atomic_load(&fa_fail_all)) return true;
  long at = atomic_load(&fa_fail_at);
  if (at != 0 && c == at) return true;
  size_t sz = atomic_load(&fa_fail_size);
  return sz != 0 && n == sz;
}
static void *fa_malloc(size_t n) {
  return fa_should_fail(n) ? NULL : malloc(n);
}
static void *fa_calloc(size_t k, size_t n) {
  return fa_should_fail(k * n) ? NULL : calloc(k, n);
}
static void *fa_realloc(void *p, size_t n) {
  return fa_should_fail(n) ? NULL : realloc(p, n);
}
static void fa_reset(void) {
  atomic_store(&fa_fail_all, 0);
  atomic_store(&fa_fail_at, 0);
  atomic_store(&fa_fail_size, 0);
  atomic_store(&fa_count, 0);
}
static ccol_memmgmt_procs_t fa_procs = {.malloc = fa_malloc,
                                        .free = free,
                                        .calloc = fa_calloc,
                                        .realloc = fa_realloc};

static void fa_count_removed(void *arg) {
  atomic_fetch_add((_Atomic int *)arg, 1);
}

static void fa_noop_readable(ccol_event_loop l, ccol_event_reg reg,
                             ccol_selectable *s, void *arg) {
  (void)l;
  (void)reg;
  (void)s;
  (void)arg;
}

/*
 * A destroy that can't allocate frees every fd registration anyway and calls
 * its on_removed, which is what ccol_event_loop_destroy() promises for a
 * registration that is live at the destroy.
 *
 * A teardown that walks the fd registrations with an iterator that
 * allocates walks nothing when that allocation fails, so on_removed never
 * runs and the registrations leak.
 */
TEST(ccol_event_loop, a_destroy_without_memory_still_calls_on_removed) {
  fa_reset();
  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create_mp(8, 4, 1, &fa_procs, &err);
  bool created = loop != CCOL_EVENT_LOOP_INVALID;
  int sv[2] = {-1, -1};
  bool have_pair = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
  _Atomic int removed = 0;
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  if (created && have_pair) {
    ccol_event_handlers_t h = {.on_readable = fa_noop_readable,
                               .on_removed = fa_count_removed};
    reg = ccol_event_loop_add(loop,
                              ccol_selectable_from_fd(sv[0], ccol_select_read),
                              h, (void *)&removed, &err);
  }
  atomic_store(&fa_fail_all, 1);
  if (created) ccol_event_loop_destroy(loop);
  fa_reset();
  if (have_pair) {
    close(sv[0]);
    close(sv[1]);
  }
  int removed_calls = atomic_load(&removed);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(have_pair);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ(removed_calls, 1);
}

static void fa_on_readable(ccol_event_loop l, ccol_event_reg reg,
                           ccol_selectable *s, void *arg) {
  (void)reg;
  (void)l;
  uint64_t v;
  if (read(s->fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
    atomic_fetch_add((_Atomic int *)arg, 1);
}

/* The read end of a pipe, non-blocking and close-on-exec, with its write end
 * in *wfd. One 8-byte write makes it readable and it stays readable until
 * fa_on_readable() reads those bytes, as a level-triggered fd does on every
 * system. */
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

/* Tells whether l dispatches a readable fd within a bounded time while every
 * allocation succeeds. */
static bool fa_loop_dispatches(ccol_event_loop l) {
  int wfd = -1;
  int efd = fa_wake_pipe(&wfd);
  if (efd < 0) return false;
  _Atomic int got = 0;
  ccol_event_handlers_t h = {.on_readable = fa_on_readable};
  ccol_event_reg r = ccol_event_loop_add(
      l, ccol_selectable_from_fd(efd, ccol_select_read), h, (void *)&got, NULL);
  bool ok = false;
  if (r != CCOL_EVENT_REG_INVALID) {
    uint64_t one = 1;
    test_write_retry_eintr(wfd, &one, sizeof(one));
    ok = derr_wait_count(&got, 1, 5000);
    ccol_event_loop_remove(l, r);
  }
  /* The shutdown joins every thread that could still be holding &got. */
  ccol_event_loop_shutdown(l);
  close(efd);
  if (wfd >= 0) close(wfd);
  return ok;
}

/* Every handle ccol_event_loop_create_mp returns names a loop that
 * dispatches, whichever allocation fails. The first part fails every
 * allocation of the epoll_wait buffer's size and keeps failing them for a
 * while after create returns. A poller thread that allocated that buffer
 * itself would exit on the failure, and the loop that create returned would
 * never dispatch. */
TEST(ccol_event_loop, create_under_allocation_failure_never_returns_dead_loop) {
  int dead = 0, created = 0, refused = 0;
  for (size_t threads = 1; threads <= 3; threads += 2) {
    fa_reset();
    atomic_store(&fa_fail_size, (size_t)37 * sizeof(ccol_poll_event));
    char *err = NULL;
    ccol_event_loop l =
        ccol_event_loop_create_mp(37, 2, threads, &fa_procs, &err);
    evl_sleep_ms(50);
    fa_reset();
    if (l == CCOL_EVENT_LOOP_INVALID) {
      refused++;
      if (err == NULL) dead++;
      continue;
    }
    created++;
    if (!fa_loop_dispatches(l)) dead++;
    ccol_event_loop_destroy(l);
  }

  /* The sweep. One clean create counts the allocations it makes. */
  fa_reset();
  ccol_event_loop probe = ccol_event_loop_create_mp(16, 2, 3, &fa_procs, NULL);
  long total = atomic_load(&fa_count);
  if (probe != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(probe);
  for (size_t threads = 1; threads <= 3; threads += 2) {
    for (long n = 1; n <= total + 4; n++) {
      fa_reset();
      atomic_store(&fa_fail_at, n);
      char *err = NULL;
      ccol_event_loop l =
          ccol_event_loop_create_mp(16, 2, threads, &fa_procs, &err);
      evl_sleep_ms(5);
      fa_reset();
      if (l == CCOL_EVENT_LOOP_INVALID) {
        if (err == NULL) dead++;
        continue;
      }
      if (!fa_loop_dispatches(l)) dead++;
      ccol_event_loop_destroy(l);
    }
  }
  fa_reset();
  REQUIRE_GT(total, 0L);
  REQUIRE_EQ(created + refused, 2);
  REQUIRE_EQ(dead, 0);
}

/* Adds one registration of sel to l while the allocation numbered fail_at
 * (1-based, counted from the start of the call) fails, and sorts the
 * outcome: 1 for a registration (removed again immediately), 0 for a refusal
 * whose message names a failed allocation and -1 for any other refusal. */
static int fa_add_outcome(ccol_event_loop l, ccol_selectable sel,
                          long fail_at) {
  fa_reset();
  atomic_store(&fa_fail_at, fail_at);
  char *err = NULL;
  ccol_event_handlers_t h = {.on_readable = fa_noop_readable};
  ccol_event_reg r = ccol_event_loop_add(l, sel, h, NULL, &err);
  fa_reset();
  if (r != CCOL_EVENT_REG_INVALID) {
    ccol_event_loop_remove(l, r);
    return 1;
  }
  return (err && strstr(err, "failed to allocate")) ? 0 : -1;
}

/* Failing each allocation ccol_event_loop_add makes in turn gives a message
 * that contains "failed to allocate", as ccol_event_loop_add(3) documents,
 * for an fd and for a queue selectable, while a failed initialisation of a
 * queue registration's wait mutex, which no allocator reaches, gives "a
 * system call failed" instead. The test fails if a message doesn't contain
 * the documented text, or if a failed mutex initialisation is reported as
 * an allocation failure. */
TEST(ccol_event_loop, add_reports_each_failure_class_with_its_message) {
  fa_reset();
  ccol_event_loop l = ccol_event_loop_create_mp(8, 1, 1, &fa_procs, NULL);
  REQUIRE_NE(l, CCOL_EVENT_LOOP_INVALID);
  int sv[2] = {-1, -1};
  bool have_pair = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
  ccol_circular_queue *cq = ccol_circular_queue_create_mp(8, NULL, NULL);
  bool have_cq = cq != NULL;

  int wrong_messages = 0, refusals = 0, total_fd = 0, total_q = 0;
  if (have_pair && have_cq) {
    ccol_selectable sels[2] = {
        ccol_selectable_from_fd(sv[0], ccol_select_read),
        ccol_selectable_from_circq(cq, ccol_select_read)};
    for (int k = 0; k < 2; k++) {
      /* One clean add counts the allocations the call makes. */
      fa_reset();
      ccol_event_handlers_t h = {.on_readable = fa_noop_readable};
      ccol_event_reg r = ccol_event_loop_add(l, sels[k], h, NULL, NULL);
      long total = atomic_load(&fa_count);
      fa_reset();
      if (r != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(l, r);
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
  ccol_event_reg sync_reg = CCOL_EVENT_REG_INVALID;
  if (have_cq) {
    _ccol_event_loop_test_fail_next_reg_sync_init();
    ccol_event_handlers_t h = {.on_readable = fa_noop_readable};
    sync_reg =
        ccol_event_loop_add(l, ccol_selectable_from_circq(cq, ccol_select_read),
                            h, NULL, &sync_err);
    if (sync_reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(l, sync_reg);
  }
  bool sync_message_ok =
      sync_err && strstr(sync_err, "a system call failed") != NULL;

  /* The handle slot table grows when the loop is created, so only the hook
   * can reach the failure of that growth. */
  char *slot_err = NULL;
  ccol_event_reg slot_reg = CCOL_EVENT_REG_INVALID;
  if (have_pair) {
    _ccol_event_loop_test_force_next_reg_slot_acquire_failure();
    ccol_event_handlers_t h = {.on_readable = fa_noop_readable};
    slot_reg =
        ccol_event_loop_add(l, ccol_selectable_from_fd(sv[0], ccol_select_read),
                            h, NULL, &slot_err);
    if (slot_reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(l, slot_reg);
  }
  bool slot_message_ok =
      slot_err && strstr(slot_err, "failed to allocate") != NULL;

  ccol_event_loop_destroy(l);
  if (have_cq) ccol_circular_queue_destroy(cq);
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
  REQUIRE_EQ(sync_reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(sync_message_ok);
  REQUIRE_EQ(slot_reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_TRUE(slot_message_ok);
}

/* A dispatch that can't be submitted to the dispatch pool, because every
 * allocation fails, leaves a level-triggered fd ready. The poller backs off
 * instead of spinning and dispatches normally once allocation works again.
 * We count completed epoll_wait calls here rather than time. Without the
 * backoff the poller re-arms and collects the same event again right away,
 * and the counter races into the thousands inside the window. */
TEST(ccol_event_loop, failed_dispatch_submit_backs_off_instead_of_spinning) {
  fa_reset();
  ccol_event_loop l = ccol_event_loop_create_mp(8, 1, 3, &fa_procs, NULL);
  REQUIRE_NE(l, CCOL_EVENT_LOOP_INVALID);
  int wfd = -1;
  int efd = fa_wake_pipe(&wfd);
  _Atomic int got = 0;
  ccol_event_handlers_t h = {.on_readable = fa_on_readable};
  ccol_event_reg r = ccol_event_loop_add(
      l, ccol_selectable_from_fd(efd, ccol_select_read), h, (void *)&got, NULL);
  bool added = r != CCOL_EVENT_REG_INVALID;
  uint64_t spins = 0;
  int got_while_failing = 0;
  bool recovered = false;
  if (added) {
    evl_sleep_ms(20);
    atomic_store(&fa_fail_all, 1);
    uint64_t one = 1;
    test_write_retry_eintr(wfd, &one, sizeof(one));
    evl_sleep_ms(100);
    uint64_t before = _ccol_event_loop_poller_iterations_for_tests(l);
    evl_sleep_ms(300);
    spins = _ccol_event_loop_poller_iterations_for_tests(l) - before;
    got_while_failing = atomic_load(&got);
    atomic_store(&fa_fail_all, 0);
    recovered = derr_wait_count(&got, 1, 5000);
    ccol_event_loop_remove(l, r);
  }
  ccol_event_loop_destroy(l);
  close(efd);
  if (wfd >= 0) close(wfd);
  fa_reset();
  REQUIRE_TRUE(added);
  REQUIRE_EQ(got_while_failing, 0);
  REQUIRE_LT(spins, (uint64_t)30);
  REQUIRE_TRUE(recovered);
}

/* ------------------------------------------------------------------------
 * ccol_event_loop: the self-call guards identify the poller thread by a
 * thread-local mark, never by its thread ID.
 * ------------------------------------------------------------------------ */

typedef struct {
  ccol_event_loop loop;
  bool created;
  pthread_t poller;
  _Atomic bool matched;
  _Atomic int shutdown_rv;
  _Atomic bool destroyed;
} tidr_ctx;

/* Runs only on a thread that carries the ID of the joined poller. An
 * application thread is allowed to shut the loop down and destroy it. */
static void *tidr_thread(void *arg) {
  tidr_ctx *c = (tidr_ctx *)arg;
  if (!pthread_equal(pthread_self(), c->poller)) return NULL;
  atomic_store(&c->matched, true);
  int rv = (int)ccol_event_loop_shutdown(c->loop);
  atomic_store(&c->shutdown_rv, rv);
  if (rv == (int)ccol_success) {
    ccol_event_loop l = c->loop;
    ccol_event_loop_destroy(l);
    atomic_store(&c->destroyed, true);
  }
  return NULL;
}

#if defined(__GLIBC__)
/* glibc declares these two only under _GNU_SOURCE, which this file doesn't
 * define, so the prototypes are copied from <pthread.h>. */
extern int pthread_getattr_default_np(pthread_attr_t *attr);
extern int pthread_setattr_default_np(const pthread_attr_t *attr);
#endif

/* Shuts the loop down, which joins its poller, and then starts batches of
 * threads until one of them carries the ID of that joined poller. glibc
 * hands the cached descriptor of a joined thread to a thread started later,
 * so this happens within the first batches. Every started thread is joined
 * before the next batch and before the function returns.
 *
 * glibc keeps at most 40 MiB of stacks of joined threads, which is four
 * stacks with the default 8 MiB stack. The shutdown joins the poller before
 * the dispatch workers, so with four workers the poller's stack falls out of
 * the cache and gets unmapped, and a later thread gets the same ID only if a
 * new mapping lands at the same address, which AddressSanitizer often
 * prevents. Giving the threads of this test a default stack of 1 MiB keeps
 * the poller's stack in the cache, so a later thread always gets its ID. */
static void tidr_run_body(size_t threads, tidr_ctx *c);

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

static void tidr_run_body(size_t threads, tidr_ctx *c) {
  c->loop = ccol_event_loop_create(8, 1, threads, NULL);
  c->created = c->loop != CCOL_EVENT_LOOP_INVALID;
  if (!c->created) return;
  c->poller = _ccol_event_loop_poller_thread_for_tests(c->loop);
  if (ccol_event_loop_shutdown(c->loop) == ccol_success) {
    enum { TIDR_BATCH = 16 };
    for (int round = 0; round < 64 && !atomic_load(&c->matched); round++) {
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
    ccol_event_loop l = c->loop;
    ccol_event_loop_destroy(l);
  }
}

/* Guards that compared _ccol_get_thread_id() with the ID of the joined
 * poller would give ccol_not_permitted from the shutdown to the thread that
 * inherits that ID, and a destroy from it would end the process as a
 * self-destroy. The test fails loudly when no thread got the ID, so the
 * case never goes unexamined. */
TEST(ccol_event_loop, a_thread_that_inherits_the_poller_id_may_destroy_1) {
  tidr_ctx c;
  tidr_run(1, &c);
  REQUIRE_TRUE(c.created);
  REQUIRE_TRUE(atomic_load(&c.matched));
  REQUIRE_EQ(atomic_load(&c.shutdown_rv), (int)ccol_success);
  REQUIRE_TRUE(atomic_load(&c.destroyed));
}

TEST(ccol_event_loop, a_thread_that_inherits_the_poller_id_may_destroy_4) {
  tidr_ctx c;
  tidr_run(4, &c);
  REQUIRE_TRUE(c.created);
  REQUIRE_TRUE(atomic_load(&c.matched));
  REQUIRE_EQ(atomic_load(&c.shutdown_rv), (int)ccol_success);
  REQUIRE_TRUE(atomic_load(&c.destroyed));
}

typedef struct {
  ccol_event_loop loop;
  _Atomic int rv;
  _Atomic bool done;
} orsd_ctx;

static void orsd_on_removed(void *arg) {
  orsd_ctx *c = (orsd_ctx *)arg;
  atomic_store(&c->rv, (int)ccol_event_loop_shutdown(c->loop));
  atomic_store(&c->done, true);
}

/* The on_removed of an ordinary removal runs on the poller thread whatever
 * the number of reactor threads, so shutting its own loop down from there is
 * a self-join and the guard has to refuse it. */
static void orsd_run(size_t threads, orsd_ctx *c) {
  atomic_init(&c->rv, -1000);
  atomic_init(&c->done, false);
  int pfd[2];
  if (pipe(pfd) != 0) return;
  c->loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (c->loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_removed = orsd_on_removed};
    ccol_event_reg reg = ccol_event_loop_add(
        c->loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), h, c, NULL);
    if (reg != CCOL_EVENT_REG_INVALID &&
        ccol_event_loop_remove(c->loop, reg) == ccol_success) {
      for (int i = 0; i < 5000 && !atomic_load(&c->done); i++) evl_sleep_ms(1);
    }
    ccol_event_loop_destroy(c->loop);
  }
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, shutdown_from_on_removed_on_the_poller_is_refused) {
  orsd_ctx c1, c4;
  orsd_run(1, &c1);
  orsd_run(4, &c4);
  REQUIRE_TRUE(atomic_load(&c1.done));
  REQUIRE_EQ(atomic_load(&c1.rv), (int)ccol_not_permitted);
  REQUIRE_TRUE(atomic_load(&c4.done));
  REQUIRE_EQ(atomic_load(&c4.rv), (int)ccol_not_permitted);
}

/* ------------------------------------------------------------------------
 * ccol_event_loop: a queue listener that moves one message per callback
 * drains a backlog, and one that moves nothing doesn't spin.
 * ------------------------------------------------------------------------ */

enum { BKL_RECV_ONE = 0, BKL_NOTHING = 1, BKL_SEND_ONE = 2 };

static char bkl_token;

typedef struct {
  int mode;
  _Atomic long dispatches;
  _Atomic long moved;
} bkl_ctx;

static void bkl_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  bkl_ctx *c = (bkl_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (c->mode != BKL_RECV_ONE) return;
  c_message_t m = {.data = NULL, .size = 0};
  ccol_retval_t rv = (sel->type == ccol_selectable_circq)
                         ? ccol_circq_try_recv_zc(sel->cq, &m)
                         : ccol_dynmq_try_recv_zc(sel->dq, &m);
  if (rv == ccol_success) atomic_fetch_add(&c->moved, 1);
}

static void bkl_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  bkl_ctx *c = (bkl_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (c->mode != BKL_SEND_ONE) return;
  c_message_t m = {.data = &bkl_token, .size = 1};
  if (ccol_circq_try_send_zc(sel->cq, &m) == ccol_success)
    atomic_fetch_add(&c->moved, 1);
}

typedef struct {
  long moved;
  long dispatches_settled;
  long dispatches_later;
  size_t left_in_queue;
  bool ok;
} bkl_result;

static void bkl_wait_moved(bkl_ctx *c, long target) {
  for (int i = 0; i < 10000 && atomic_load(&c->moved) < target; i++)
    evl_sleep_ms(1);
}

/* Waits, at most about 10 s, until the listeners have had `target`
 * dispatches between them. A listener that moves nothing gets its first
 * dispatch from the add's wake, so the 200 ms samples after this wait judge
 * only the dispatches that come after, however late the reactor ran. */
static void bkl_wait_dispatches(bkl_ctx *c, long target) {
  for (int i = 0; i < 10000 && atomic_load(&c->dispatches) < target; i++)
    evl_sleep_ms(1);
}

/* Puts `count` messages into a queue of the given kind, registers `nregs`
 * read listeners with the given mode on it and sends nothing more. With
 * BKL_RECV_ONE it waits until the listeners have taken every message, and
 * with BKL_NOTHING until each listener got one dispatch. Then it samples the
 * dispatch count twice, 200 ms apart, so a listener that keeps getting
 * dispatched shows up as a count that is still growing. */
static void bkl_run_read(size_t threads, bool dynq, int mode, long count,
                         int nregs, bkl_result *r) {
  memset(r, 0, sizeof(*r));
  bkl_ctx c = {.mode = mode};
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.moved, 0);
  ccol_circular_queue *cq = NULL;
  ccol_dynamic_queue *dq = NULL;
  if (dynq)
    dq = ccol_dynamic_queue_create(NULL);
  else
    cq = ccol_circular_queue_create((size_t)count, NULL);
  if (!cq && !dq) return;
  bool filled = true;
  for (long i = 0; i < count && filled; i++) {
    c_message_t m = {.data = &bkl_token, .size = 1};
    filled = (dynq ? ccol_dynmq_send_zc(dq, &m) : ccol_circq_send_zc(cq, &m)) ==
             ccol_success;
  }
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ccol_event_reg regs[4] = {CCOL_EVENT_REG_INVALID, CCOL_EVENT_REG_INVALID,
                            CCOL_EVENT_REG_INVALID, CCOL_EVENT_REG_INVALID};
  bool added = filled && loop != CCOL_EVENT_LOOP_INVALID;
  for (int i = 0; i < nregs && added; i++) {
    ccol_selectable sel =
        dynq ? ccol_selectable_from_dynq(dq, ccol_select_read)
             : ccol_selectable_from_circq(cq, ccol_select_read);
    ccol_event_handlers_t h = {.on_readable = bkl_on_readable};
    regs[i] = ccol_event_loop_add(loop, sel, h, &c, NULL);
    added = regs[i] != CCOL_EVENT_REG_INVALID;
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
    if (regs[i] != CCOL_EVENT_REG_INVALID)
      ccol_event_loop_remove(loop, regs[i]);
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  r->moved = atomic_load(&c.moved);
  r->left_in_queue = dynq ? ccol_dynmq_msg_count(dq) : ccol_circq_msg_count(cq);
  c_message_t m;
  if (dynq) {
    while (ccol_dynmq_try_recv_zc(dq, &m) == ccol_success) {
    }
    ccol_dynamic_queue_destroy(dq);
  } else {
    while (ccol_circq_try_recv_zc(cq, &m) == ccol_success) {
    }
    ccol_circular_queue_destroy(cq);
  }
  r->ok = added;
}

/* If no listener were woken again after a dispatch that moved a message,
 * one eventfd wake would cover the whole backlog, each callback would take
 * one message, and the listeners would stop after one or two messages with
 * the rest stuck in the queue. */
TEST(ccol_event_loop, a_one_message_listener_drains_a_backlog) {
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

/* A listener that ignores a ready queue isn't dispatched again for the same
 * wake. With one listener that's exactly one dispatch, and with two the wake
 * reaches each of them at most twice before the count stops growing. */
TEST(ccol_event_loop, a_listener_that_moves_nothing_does_not_spin) {
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

/* The write direction. A listener that sends one message per callback fills
 * a circular queue to its bound without any receive and then stops, because
 * a full queue isn't writable. A listener that sends nothing is dispatched
 * once. */
static void bkl_run_write(size_t threads, int mode, long capacity,
                          bkl_result *r) {
  memset(r, 0, sizeof(*r));
  bkl_ctx c = {.mode = mode};
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.moved, 0);
  ccol_circular_queue *cq = ccol_circular_queue_create((size_t)capacity, NULL);
  if (!cq) return;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_writable = bkl_on_writable};
    reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_write), h, &c, NULL);
  }
  if (reg != CCOL_EVENT_REG_INVALID) {
    if (mode == BKL_SEND_ONE) bkl_wait_moved(&c, capacity);
    if (mode == BKL_NOTHING) bkl_wait_dispatches(&c, 1);
    evl_sleep_ms(200);
    r->dispatches_settled = atomic_load(&c.dispatches);
    evl_sleep_ms(200);
    r->dispatches_later = atomic_load(&c.dispatches);
    ccol_event_loop_remove(loop, reg);
  }
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  r->moved = atomic_load(&c.moved);
  r->left_in_queue = ccol_circq_msg_count(cq);
  c_message_t m;
  while (ccol_circq_try_recv_zc(cq, &m) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);
  r->ok = reg != CCOL_EVENT_REG_INVALID;
}

/* Without the re-wake after a dispatch that sent a message, the listener
 * sends one message and never gets dispatched again. */
TEST(ccol_event_loop, a_one_message_writer_fills_the_queue_and_stops) {
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

/* A write registration on a full circular queue whose sends are off. The
 * queue counts as ready, the same way a pipe whose reader is gone counts as
 * ready for POLLOUT, so the registration gets one on_writable call, whose
 * send reports ccol_not_permitted, and no other call since that send moved
 * nothing. With add_after_disable the sends are off before the add and the
 * add itself reports the state. Otherwise the registration exists first,
 * gets no call while the queue is full, and the disable reports the state. */
typedef struct {
  _Atomic long dispatches;
  _Atomic long refused;
} dsw_ctx;

static void dsw_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  dsw_ctx *c = (dsw_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  c_message_t m = {.data = &bkl_token, .size = 1};
  if (ccol_circq_try_send_zc(sel->cq, &m) == ccol_not_permitted)
    atomic_fetch_add(&c->refused, 1);
}

typedef struct {
  long before_disable;
  long settled;
  long later;
  long refused;
  size_t left_in_queue;
  bool ok;
} dsw_result;

static void dsw_run(size_t threads, bool add_after_disable, dsw_result *r) {
  memset(r, 0, sizeof(*r));
  dsw_ctx c;
  atomic_init(&c.dispatches, 0);
  atomic_init(&c.refused, 0);
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  if (!cq) return;
  c_message_t fill = {.data = &bkl_token, .size = 1};
  bool filled = ccol_circq_try_send_zc(cq, &fill) == ccol_success;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  ccol_event_handlers_t h = {.on_writable = dsw_on_writable};
  ccol_selectable sel = ccol_selectable_from_circq(cq, ccol_select_write);
  if (add_after_disable) ccol_circq_disable_sending(cq);
  if (loop != CCOL_EVENT_LOOP_INVALID)
    reg = ccol_event_loop_add(loop, sel, h, &c, NULL);
  if (reg != CCOL_EVENT_REG_INVALID) {
    if (!add_after_disable) {
      evl_sleep_ms(100);
      r->before_disable = atomic_load(&c.dispatches);
      ccol_circq_disable_sending(cq);
    }
    for (int i = 0; i < 10000 && atomic_load(&c.dispatches) < 1; i++)
      evl_sleep_ms(1);
    evl_sleep_ms(200);
    r->settled = atomic_load(&c.dispatches);
    evl_sleep_ms(200);
    r->later = atomic_load(&c.dispatches);
    ccol_event_loop_remove(loop, reg);
  }
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  r->refused = atomic_load(&c.refused);
  r->left_in_queue = ccol_circq_msg_count(cq);
  c_message_t m;
  while (ccol_circq_try_recv_zc(cq, &m) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);
  r->ok = filled && reg != CCOL_EVENT_REG_INVALID;
}

/* A queue with sends off has to count as ready for the write direction;
 * otherwise a registration added after the disable gets no call at all. */
TEST(ccol_event_loop, a_write_registration_hears_once_that_sends_are_off) {
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

/* ------------------------------------------------------------------------
 * ccol_event_loop_add and ccol_event_loop_modify: the documented outputs.
 * ------------------------------------------------------------------------ */

TEST(ccol_event_loop, add_sets_err_str_to_null_on_success) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  bool created = loop != CCOL_EVENT_LOOP_INVALID;
  char *err = (char *)"poison";
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  if (created) {
    ccol_event_handlers_t none = {0};
    reg = ccol_event_loop_add(loop,
                              ccol_selectable_from_fd(pfd[0], ccol_select_read),
                              none, NULL, &err);
    ccol_event_loop_destroy(loop);
  }
  close(pfd[0]);
  close(pfd[1]);
  REQUIRE_TRUE(created);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_EQ((void *)err, NULL);
}

/* A read registration with no handler at all mutes itself on a hang-up. A
 * modify that keeps its direction arms it again, so the hang-up is reported
 * once more, which the poller counts as one more epoll_wait, and then mutes
 * it again without a spin. */
static void msd_run(size_t threads, uint64_t *advanced, uint64_t *window,
                    bool *ok) {
  *ok = false;
  *advanced = 0;
  *window = 0;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t none = {0};
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(sv[0], ccol_select_read), none, NULL,
        NULL);
    if (reg != CCOL_EVENT_REG_INVALID) {
      close(sv[1]);
      sv[1] = -1;
      /* Gives the hang-up time to arrive and mute the registration. */
      evl_sleep_ms(100);
      uint64_t before = _ccol_event_loop_poller_iterations_for_tests(loop);
      bool modified =
          ccol_event_loop_modify(loop, reg, ccol_select_read) == ccol_success;
      for (int i = 0; i < 2000; i++) {
        if (_ccol_event_loop_poller_iterations_for_tests(loop) > before) break;
        evl_sleep_ms(1);
      }
      *advanced = _ccol_event_loop_poller_iterations_for_tests(loop) - before;
      *window = unh_poller_iterations_in_window(loop);
      *ok = modified && ccol_event_loop_remove(loop, reg) == ccol_success;
    }
    ccol_event_loop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

/* A modify to the current direction that returns without arming the muted
 * registration again leaves nothing to wake the poller, and the iteration
 * count doesn't move. */
TEST(ccol_event_loop, a_modify_to_the_same_direction_unmutes) {
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

/* ------------------------------------------------------------------------
 * The queue mutex registry removes an entry in constant time.
 * ------------------------------------------------------------------------ */

#if CCOL_FORK_SAFETY_REQUIRED
/* A remove that scans the registry for the mutex looks at up to every live
 * entry, so the probe count grows with the square of the number of queues,
 * and a swap that doesn't update the moved entry's position fails the index
 * check. Either mistake fails here. */
TEST(queue_mutex_registry, a_remove_is_constant_time_and_keeps_every_index) {
  enum { QMR_N = 256 };
  void *queues[QMR_N];
  bool is_dyn[QMR_N];
  size_t base_count = 0;
  uint64_t base_probes = 0;
  bool base_ok =
      _ccol_queue_mutex_registry_check_for_tests(&base_count, &base_probes);
  int created = 0;
  for (; created < QMR_N; created++) {
    is_dyn[created] = (created % 3) == 0;
    queues[created] = is_dyn[created]
                          ? (void *)ccol_dynamic_queue_create(NULL)
                          : (void *)ccol_circular_queue_create(4, NULL);
    if (!queues[created]) break;
  }
  size_t full_count = 0;
  bool full_ok = _ccol_queue_mutex_registry_check_for_tests(&full_count, NULL);
  /* A fixed shuffle, so the removes hit the middle, both ends and the last
   * entry in every pattern. */
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
      ccol_dynamic_queue *dq = (ccol_dynamic_queue *)queues[i];
      ccol_dynamic_queue_destroy(dq);
    } else {
      ccol_circular_queue *cq = (ccol_circular_queue *)queues[i];
      ccol_circular_queue_destroy(cq);
    }
    if (!_ccol_queue_mutex_registry_check_for_tests(NULL, NULL))
      every_step_ok = false;
  }
  size_t end_count = 0;
  uint64_t end_probes = 0;
  bool end_ok =
      _ccol_queue_mutex_registry_check_for_tests(&end_count, &end_probes);
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

/* ------------------------------------------------------------------------
 * ccol_event_loop: a queue listener that moves messages keeps its own
 * dispatches going wherever an idle sibling sits in the list.
 * ------------------------------------------------------------------------ */

typedef struct {
  bool active;
  _Atomic long budget; /* writers: messages left to send */
  _Atomic long dispatches;
  _Atomic long moved;
} sib_ctx;

static void sib_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  sib_ctx *c = (sib_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (!c->active || atomic_load(&c->budget) <= 0) return;
  c_message_t m = {.data = &bkl_token, .size = 1};
  ccol_retval_t rv = (sel->type == ccol_selectable_circq)
                         ? ccol_circq_try_send_zc(sel->cq, &m)
                         : ccol_dynmq_send_zc(sel->dq, &m);
  if (rv == ccol_success) {
    atomic_fetch_sub(&c->budget, 1);
    atomic_fetch_add(&c->moved, 1);
  }
}

static void sib_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  sib_ctx *c = (sib_ctx *)arg;
  atomic_fetch_add(&c->dispatches, 1);
  if (!c->active) return;
  c_message_t m = {.data = NULL, .size = 0};
  ccol_retval_t rv = (sel->type == ccol_selectable_circq)
                         ? ccol_circq_try_recv_zc(sel->cq, &m)
                         : ccol_dynmq_try_recv_zc(sel->dq, &m);
  if (rv == ccol_success) atomic_fetch_add(&c->moved, 1);
}

typedef struct {
  long active_moved;
  long idle_settled;
  long idle_later;
  size_t left_in_queue;
  bool ok;
} sib_result;

/* Registers an idle listener first and an active one second on one queue of
 * the given kind and direction (idle_first), or the other way round. The
 * active writer sends `count` messages, one per callback. For the read
 * direction the queue already holds `count` messages before the listeners
 * exist and the active reader takes one per callback. Nothing else touches
 * the queue. */
static void sib_run(size_t threads, bool dynq, ccol_select_dir dir,
                    bool idle_first, long count, sib_result *r) {
  memset(r, 0, sizeof(*r));
  sib_ctx idle_c = {.active = false};
  sib_ctx act_c = {.active = true};
  atomic_init(&idle_c.budget, 0);
  atomic_init(&idle_c.dispatches, 0);
  atomic_init(&idle_c.moved, 0);
  atomic_init(&act_c.budget, dir == ccol_select_write ? count : 0);
  atomic_init(&act_c.dispatches, 0);
  atomic_init(&act_c.moved, 0);
  ccol_circular_queue *cq = NULL;
  ccol_dynamic_queue *dq = NULL;
  if (dynq)
    dq = ccol_dynamic_queue_create(NULL);
  else
    cq = ccol_circular_queue_create((size_t)count, NULL);
  if (!cq && !dq) return;
  bool filled = true;
  for (long i = 0; dir == ccol_select_read && i < count && filled; i++) {
    c_message_t m = {.data = &bkl_token, .size = 1};
    filled = (dynq ? ccol_dynmq_send_zc(dq, &m) : ccol_circq_send_zc(cq, &m)) ==
             ccol_success;
  }
  ccol_event_loop loop = ccol_event_loop_create(8, 1, threads, NULL);
  ccol_event_reg regs[2] = {CCOL_EVENT_REG_INVALID, CCOL_EVENT_REG_INVALID};
  bool added = filled && loop != CCOL_EVENT_LOOP_INVALID;
  for (int i = 0; i < 2 && added; i++) {
    bool idle_now = (i == 0) == idle_first;
    ccol_selectable sel = dynq ? ccol_selectable_from_dynq(dq, dir)
                               : ccol_selectable_from_circq(cq, dir);
    ccol_event_handlers_t h = {0};
    if (dir == ccol_select_read)
      h.on_readable = sib_on_readable;
    else
      h.on_writable = sib_on_writable;
    regs[i] =
        ccol_event_loop_add(loop, sel, h, idle_now ? &idle_c : &act_c, NULL);
    added = regs[i] != CCOL_EVENT_REG_INVALID;
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
    if (regs[i] != CCOL_EVENT_REG_INVALID)
      ccol_event_loop_remove(loop, regs[i]);
  if (loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(loop);
  r->active_moved = atomic_load(&act_c.moved);
  r->left_in_queue = dynq ? ccol_dynmq_msg_count(dq) : ccol_circq_msg_count(cq);
  c_message_t m;
  if (dynq) {
    while (ccol_dynmq_try_recv_zc(dq, &m) == ccol_success) {
    }
    ccol_dynamic_queue_destroy(dq);
  } else {
    while (ccol_circq_try_recv_zc(cq, &m) == ccol_success) {
    }
    ccol_circular_queue_destroy(cq);
  }
  r->ok = added;
}

/* Suppose only the last listener of the list may wake a listener again
 * after a dispatch that moved a message. Then an active listener registered
 * after an idle one is the head of the list, its progress only reaches the
 * idle tail and the idle tail ends the chain, so the active writer sends one
 * message and the active reader takes one, which this test catches. */
TEST(ccol_event_loop,
     an_idle_older_sibling_does_not_strand_an_active_listener) {
  static const size_t thread_counts[] = {1, 4};
  sib_result r[2][2][2][2]; /* threads, kind, direction, order */
  for (int t = 0; t < 2; t++)
    for (int k = 0; k < 2; k++)
      for (int d = 0; d < 2; d++)
        for (int o = 0; o < 2; o++)
          sib_run(thread_counts[t], k == 1,
                  d == 0 ? ccol_select_read : ccol_select_write, o == 0, 100,
                  &r[t][k][d][o]);
  for (int t = 0; t < 2; t++) {
    for (int k = 0; k < 2; k++) {
      for (int d = 0; d < 2; d++) {
        for (int o = 0; o < 2; o++) {
          sib_result *x = &r[t][k][d][o];
          REQUIRE_TRUE(x->ok);
          REQUIRE_EQ(x->active_moved, 100L);
          REQUIRE_EQ(x->left_in_queue, d == 0 ? (size_t)0 : (size_t)100);
          /* The idle sibling gets a bounded number of dispatches: at most
           * two per message that moved (a forward from the active listener,
           * and the count moving under its own dispatch when the two run on
           * different threads) plus the wakes of the registrations, and
           * none at all once the traffic is over. */
          REQUIRE_LE(x->idle_settled, 210L);
          REQUIRE_EQ(x->idle_later, x->idle_settled);
        }
      }
    }
  }
}

/* Three listeners that all decline a ready queue pass a wake between them a
 * bounded number of times and then stop. */
TEST(ccol_event_loop, three_listeners_that_move_nothing_do_not_spin) {
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

/* ------------------------------------------------------------------------
 * ccol_event_loop: a listener that leaves a queue hands on the wake it
 * holds, so the queue's other listeners hear about the message anyway.
 * ------------------------------------------------------------------------ */

typedef struct {
  int pfd[2];
  _Atomic bool blocker_entered;
  _Atomic bool blocker_release;
  _Atomic long leaving_calls;
  _Atomic long staying_received;
  ccol_circular_queue *cq;
  ccol_event_loop l1;
  size_t sel_idx;
  ccol_retval_t sel_rv;
} rlw_ctx;

static void rlw_on_block(ccol_event_loop loop, ccol_event_reg reg,
                         ccol_selectable *sel, void *arg) {
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

static void rlw_on_leaving(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  atomic_fetch_add(&((rlw_ctx *)arg)->leaving_calls, 1);
}

static void rlw_on_staying(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  c_message_t m;
  if (ccol_circq_try_recv_zc(sel->cq, &m) == ccol_success)
    atomic_fetch_add(&((rlw_ctx *)arg)->staying_received, 1);
}

static void *rlw_select_thread(void *arg) {
  rlw_ctx *c = (rlw_ctx *)arg;
  ccol_selectable s = ccol_selectable_from_circq(c->cq, ccol_select_read);
  c->sel_rv = ccol_select_timed(&c->sel_idx, 1, &s, 5000000);
  return NULL;
}

static void *rlw_destroy_thread(void *arg) {
  rlw_ctx *c = (rlw_ctx *)arg;
  ccol_event_loop_destroy(c->l1);
  return NULL;
}

/* The listener that stays is a registration of a second loop (use_select
 * false) or a ccol_select_timed() caller. The one that leaves is a
 * registration of loop L1 linked after it, so it's the head of the list and
 * a send wakes it. L1's poller is held in a pipe callback, so the leaving
 * registration can't dispatch before it leaves, either by
 * ccol_event_loop_remove (by_destroy false) or by the destroy of L1. Returns
 * true when the staying listener got the message. */
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
  c.cq = ccol_circular_queue_create(8, NULL);
  c.l1 = ccol_event_loop_create(8, 1, 1, NULL);
  ccol_event_loop l2 = CCOL_EVENT_LOOP_INVALID;
  ccol_event_reg r2 = CCOL_EVENT_REG_INVALID;
  bool ok = c.cq && c.l1 != CCOL_EVENT_LOOP_INVALID;
  pthread_t st;
  bool st_started = false;
  if (ok && !use_select) {
    l2 = ccol_event_loop_create(8, 1, 1, NULL);
    ok = l2 != CCOL_EVENT_LOOP_INVALID;
    if (ok) {
      ccol_event_handlers_t h = {.on_readable = rlw_on_staying};
      r2 = ccol_event_loop_add(
          l2, ccol_selectable_from_circq(c.cq, ccol_select_read), h, &c, NULL);
      ok = r2 != CCOL_EVENT_REG_INVALID;
    }
  } else if (ok) {
    st_started = pthread_create(&st, NULL, rlw_select_thread, &c) == 0;
    ok = st_started;
    for (int i = 0; ok && i < 5000 &&
                    !_ccol_circq_test_has_sel_read_waiter_for_tests(c.cq);
         i++)
      evl_sleep_ms(1);
    ok = ok && _ccol_circq_test_has_sel_read_waiter_for_tests(c.cq);
  }
  ccol_event_reg rb = CCOL_EVENT_REG_INVALID, r1 = CCOL_EVENT_REG_INVALID;
  if (ok) {
    ccol_event_handlers_t hb = {.on_readable = rlw_on_block};
    rb = ccol_event_loop_add(
        c.l1, ccol_selectable_from_fd(c.pfd[0], ccol_select_read), hb, &c,
        NULL);
    ccol_event_handlers_t h1 = {.on_readable = rlw_on_leaving};
    r1 = ccol_event_loop_add(
        c.l1, ccol_selectable_from_circq(c.cq, ccol_select_read), h1, &c, NULL);
    ok = rb != CCOL_EVENT_REG_INVALID && r1 != CCOL_EVENT_REG_INVALID;
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
    c_message_t m = {.data = &bkl_token, .size = 1};
    sent = ccol_circq_send_zc(c.cq, &m) == ccol_success;
  }
  pthread_t dt;
  bool dt_started = false;
  if (ok && sent) {
    if (by_destroy) {
      dt_started = pthread_create(&dt, NULL, rlw_destroy_thread, &c) == 0;
      /* Gives the destroy time to claim the loop and ask the poller to stop
       * before the poller leaves the blocking callback. */
      evl_sleep_ms(100);
    } else {
      ccol_event_loop_remove(c.l1, r1);
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
  if (r2 != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(l2, r2);
  if (l2 != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(l2);
  if (c.l1 != CCOL_EVENT_LOOP_INVALID && !(by_destroy && dt_started)) {
    if (rb != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(c.l1, rb);
    ccol_event_loop_destroy(c.l1);
  }
  if (c.cq) {
    c_message_t x;
    while (ccol_circq_try_recv_zc(c.cq, &x) == ccol_success) {
    }
    ccol_circular_queue_destroy(c.cq);
  }
  close(c.pfd[0]);
  close(c.pfd[1]);
  /* After a remove the leaving registration never runs. A destroy may still
   * dispatch it during its drain, and that hands the wake on too. */
  *setup_ok = ok && sent && (!by_destroy || dt_started) &&
              (by_destroy || atomic_load(&c.leaving_calls) == 0);
  return delivered;
}

/* A registration that holds the only wake of a send and leaves without
 * handing it on means nobody wakes the queue's other listener, so the second
 * loop's registration never gets the message and the ccol_select_timed()
 * caller times out with the message sitting in the queue. */
TEST(ccol_event_loop, a_departing_listener_hands_its_wake_on) {
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

/* ------------------------------------------------------------------------
 * A queue destroy counts the messages only once no callback can send any
 * more of them.
 * ------------------------------------------------------------------------ */

typedef struct {
  _Atomic bool entered;
  _Atomic bool gate;
} ifs_ctx;

static void ifs_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  ifs_ctx *c = (ifs_ctx *)arg;
  if (atomic_exchange(&c->entered, true)) return;
  for (int i = 0; i < 20000 && !atomic_load(&c->gate); i++) evl_sleep_ms(1);
  c_message_t m = {.data = &bkl_token, .size = 1};
  if (sel->type == ccol_selectable_circq)
    (void)ccol_circq_try_send_zc(sel->cq, &m);
  else
    (void)ccol_dynmq_send_zc(sel->dq, &m);
}

static void *ifs_open_gate(void *arg) {
  evl_sleep_ms(200);
  atomic_store(&((ifs_ctx *)arg)->gate, true);
  return NULL;
}

/* Runs in a forked child. A write callback that is already running when its
 * registration is removed sends one message while its queue's destroy waits
 * for it. Returns only if the destroy didn't assert. */
static void ifs_child(bool dynq) {
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
  ccol_circular_queue *cq = dynq ? NULL : ccol_circular_queue_create(4, NULL);
  ccol_dynamic_queue *dq = dynq ? ccol_dynamic_queue_create(NULL) : NULL;
  if (!cq && !dq) _exit(2);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  if (loop == CCOL_EVENT_LOOP_INVALID) _exit(2);
  ccol_event_handlers_t h = {.on_writable = ifs_on_writable};
  ccol_selectable sel = dynq
                            ? ccol_selectable_from_dynq(dq, ccol_select_write)
                            : ccol_selectable_from_circq(cq, ccol_select_write);
  ccol_event_reg reg = ccol_event_loop_add(loop, sel, h, &c, NULL);
  if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
  for (int i = 0; i < 5000 && !atomic_load(&c.entered); i++) evl_sleep_ms(1);
  if (!atomic_load(&c.entered)) _exit(2);
  if (ccol_event_loop_remove(loop, reg) != ccol_success) _exit(2);
  pthread_t gt;
  if (pthread_create(&gt, NULL, ifs_open_gate, &c) != 0) _exit(2);
  /* The queue is empty at this point, and the running callback sends one
   * message before this destroy is allowed to free the queue. */
  if (dynq)
    ccol_dynamic_queue_destroy(dq);
  else
    ccol_circular_queue_destroy(cq);
  _exit(0);
}

/* A destroy that counts the messages before it waits for the running
 * callback sees an empty queue, the callback's message gets freed along with
 * the queue, and the child exits 0, so the test fails. */
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

/* ------------------------------------------------------------------------
 * The documented refusals of a send, and of an fd that epoll can't watch.
 * ------------------------------------------------------------------------ */

TEST(dynamic_queues, send_refuses_a_message_whose_data_and_size_disagree) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  REQUIRE_NE((void *)dq, NULL);
  c_message_t no_data = {.data = NULL, .size = 1};
  ccol_retval_t rv_no_data = ccol_dynmq_send_zc(dq, &no_data);
  c_message_t no_size = {.data = &bkl_token, .size = 0};
  ccol_retval_t rv_no_size = ccol_dynmq_send_zc(dq, &no_size);
  c_message_t sentinel = {.data = NULL, .size = 0};
  ccol_retval_t rv_sentinel = ccol_dynmq_send_zc(dq, &sentinel);
  size_t count = ccol_dynmq_msg_count(dq);
  c_message_t m;
  while (ccol_dynmq_try_recv_zc(dq, &m) == ccol_success) {
  }
  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ(rv_no_data, ccol_invalid_args);
  REQUIRE_EQ(rv_no_size, ccol_invalid_args);
  REQUIRE_EQ((void *)no_size.data, (void *)&bkl_token);
  REQUIRE_EQ(rv_sentinel, ccol_success);
  REQUIRE_EQ(count, (size_t)1);
}

/* Every send of a circular queue and of a channel refuses a live pointer
 * with a size of 0 and a NULL pointer with a size above 0, leaving the
 * message as it was and queueing nothing, while the {NULL, 0} sentinel goes
 * through. The channel sends run on the owner thread, so they land in the
 * owner-to-workers queue. */
TEST(circular_queues,
     every_send_refuses_a_message_whose_data_and_size_disagree) {
  ccol_circular_queue *cq = ccol_circular_queue_create(8, NULL);
  ccol_channel *ch = ccol_channel_create(8, NULL);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_NE((void *)ch, NULL);
  c_message_t bad[2] = {{.data = &bkl_token, .size = 0},
                        {.data = NULL, .size = 1}};
  ccol_retval_t rv[2][6];
  bool unchanged = true;
  for (int k = 0; k < 2; k++) {
    c_message_t m = bad[k];
    rv[k][0] = ccol_circq_send_zc(cq, &m);
    rv[k][1] = ccol_circq_try_send_zc(cq, &m);
    rv[k][2] = ccol_circq_timed_send_zc(cq, &m, 1000);
    rv[k][3] = ccol_chan_send_zc(ch, &m);
    rv[k][4] = ccol_chan_try_send_zc(ch, &m);
    rv[k][5] = ccol_chan_timed_send_zc(ch, &m, 1000);
    if (m.data != bad[k].data || m.size != bad[k].size) unchanged = false;
  }
  size_t cq_count = ccol_circq_msg_count(cq);
  c_message_t sentinel = {.data = NULL, .size = 0};
  ccol_retval_t rv_sentinel = ccol_circq_try_send_zc(cq, &sentinel);
  c_message_t out;
  while (ccol_circq_try_recv_zc(cq, &out) == ccol_success) {
  }
  ccol_circular_queue_destroy(cq);
  ccol_channel_destroy(ch);

  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 6; j++) REQUIRE_EQ(rv[k][j], ccol_invalid_args);
  REQUIRE_TRUE(unchanged);
  REQUIRE_EQ(cq_count, (size_t)0);
  REQUIRE_EQ(rv_sentinel, ccol_success);
}

/* epoll(7) refuses a regular file, so ccol_select reports it as
 * ccol_unexpected_failure and ccol_event_loop_add refuses the fd as a failed
 * system call. */
TEST(ccol_select, a_regular_file_fd_is_refused) {
  char path[] = "ccol_select_regular_file_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_GE(fd, 0);
  unlink(path);
  size_t idx = 12345;
  ccol_selectable sel = ccol_selectable_from_fd(fd, ccol_select_read);
  ccol_retval_t rv_select = ccol_select_timed(&idx, 1, &sel, 0);
  ccol_retval_t rv_select_block = ccol_select(&idx, 1, &sel);
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, NULL);
  ccol_event_reg reg = CCOL_EVENT_REG_INVALID;
  char *err = NULL;
  if (loop != CCOL_EVENT_LOOP_INVALID) {
    ccol_event_handlers_t h = {.on_readable = rlw_on_leaving};
    reg = ccol_event_loop_add(loop, sel, h, NULL, &err);
    if (reg != CCOL_EVENT_REG_INVALID) ccol_event_loop_remove(loop, reg);
    ccol_event_loop_destroy(loop);
  }
  close(fd);
  REQUIRE_EQ(rv_select, ccol_unexpected_failure);
  REQUIRE_EQ(rv_select_block, ccol_unexpected_failure);
  REQUIRE_EQ(idx, (size_t)12345);
  REQUIRE_EQ(reg, CCOL_EVENT_REG_INVALID);
  REQUIRE_NE((void *)err, NULL);
  if (err) REQUIRE_NE((void *)strstr(err, "a system call failed"), NULL);
}
